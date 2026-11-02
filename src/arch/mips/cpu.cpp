#include <ultraviolent/arch/mips/cpu.hpp>

#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/invariant.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

// Section references are to the MIPS R10000 Microprocessor User's Manual, Version 2.0 (UM).
namespace ultraviolent::mips {

namespace {

constexpr std::uint64_t sign_extend_32(std::uint64_t value) {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))));
}

constexpr bool is_sign_extended_32(std::uint64_t address) {
    return sign_extend_32(address) == address;
}

constexpr std::uint64_t bit(unsigned n) {
    return std::uint64_t{1} << n;
}

constexpr std::uint64_t user_segment_limit = bit(44);            // xuseg: 16 TB (UM 16.2)
constexpr std::uint64_t supervisor_base = 0x4000'0000'0000'0000; // xsseg
constexpr std::uint64_t kernel_segment_last = 0xc000'0fff'ffff'ffff;
constexpr std::uint64_t compatibility_base = 0xffff'ffff'8000'0000;
constexpr std::uint64_t csseg_first = 0xffff'ffff'c000'0000;
constexpr std::uint64_t csseg_last = 0xffff'ffff'dfff'ffff;
constexpr std::uint64_t physical_address_mask = system_address::physical_mask;

// System address of a reference to physical address `physical` (ADR-019).
constexpr std::uint64_t system_address_of(std::uint64_t physical, bool uncached,
                                          std::uint64_t attribute) {
    return uncached ? system_address::uncached_window(static_cast<unsigned>(attribute)) | physical
                    : physical;
}

// Registers whose MFC0/MTC0 behavior follows the 64-bit rules of UM Table 14-26.
constexpr bool is_64bit_register(unsigned reg) {
    switch (reg) {
    case cp0::entry_lo0:
    case cp0::entry_lo1:
    case cp0::context:
    case cp0::bad_vaddr:
    case cp0::entry_hi:
    case cp0::epc:
    case cp0::xcontext:
    case cp0::diagnostic:
    case cp0::error_epc:
        return true;
    default:
        return false;
    }
}

constexpr bool is_tlb_exception(ExceptionCode code) {
    return code == ExceptionCode::tlb_modified || code == ExceptionCode::tlb_load ||
           code == ExceptionCode::tlb_store;
}

} // namespace

Cpu::Cpu(AddressSpace& bus, Tracer& tracer, const ProcessorModel& model, unsigned requester)
    : bus_{bus}, requester_{requester}, tracer_{tracer}, model_{model} {
    invariant(requester < AddressSpace::max_requesters, "no such bus requester");
    invariant(((model.identity().config & config_big_endian) != 0) ==
                  (bus.byte_order() == ByteOrder::big),
              "Config.BE must match the system bus byte order");
    // The starting context (0) holds the first tag, so pages filled before the first change
    // keep theirs.
    context_tag_ = context_tag(context_);
    reset(ResetKind::power_on);
}

Cpu::NativeLayout Cpu::native_layout() const {
    static_assert(sizeof(HostPage) == 64, "tier 1 scales a host page slot by a shift");
    static_assert(sizeof(interrupt_check_) == 1 && std::atomic<bool>::is_always_lock_free,
                  "tier 1 tests the interrupt check as a byte");
    static_assert(std::has_single_bit(host_page_count), "tier 1 masks the host page slot");
    const auto at = [this](const void* field) {
        return static_cast<std::int32_t>(static_cast<const std::byte*>(field) -
                                         reinterpret_cast<const std::byte*>(this));
    };
    return {.gpr = at(state_.gpr.data()),
            .hi = at(&state_.hi),
            .lo = at(&state_.lo),
            .pc = at(&state_.pc),
            .next_pc = at(&state_.next_pc),
            .delay_slot = at(&state_.delay_slot),
            .cycles = at(&cycles_),
            .unretired_cycles = at(&unretired_cycles_),
            .timer_cycle = at(&timer_cycle_),
            .interrupt_check = at(&interrupt_check_),
            .left_host_path = at(&left_host_path_),
            .translation_context = at(&context_),
            .context_tag = at(&context_tag_),
            .status = at(&cp0_.status),
            .fpr = at(fpu_.fpr.data()),
            .fcsr = at(&fpu_.fcsr),
            .code_changes = at(&code_changes_),
            .data_pages = at(data_pages_.data()),
            .host_page_page = offsetof(HostPage, page),
            .host_page_store_page = offsetof(HostPage, store_page),
            .host_page_addend = offsetof(HostPage, addend),
            .host_page_count = host_page_count,
            .host_page_size = sizeof(HostPage),
            .byte_order = bus_.byte_order()};
}

std::uint64_t Cpu::context_tag(std::uint64_t context) {
    constexpr std::size_t slots = std::tuple_size_v<decltype(context_ids_)>;
    std::size_t slot = static_cast<std::size_t>((context * 0x9e37'79b9'7f4a'7c15) >>
                                                (64 - std::countr_zero(slots)));
    while (context_ids_[slot].id != 0) {
        if (context_ids_[slot].context == context) {
            return std::uint64_t{context_ids_[slot].id - 1} << 3;
        }
        slot = (slot + 1) % slots;
    }
    if (context_ids_used_ == max_context_ids) {
        // Every number is taken: start again, without the host pages tagged with the old
        // numbers.
        context_ids_.fill({});
        context_ids_used_ = 0;
        code_pages_.fill({});
        data_pages_.fill({});
        return context_tag(context);
    }
    context_ids_[slot] = {.context = context, .id = static_cast<std::uint32_t>(++context_ids_used_)};
    return std::uint64_t{context_ids_[slot].id - 1} << 3;
}

void Cpu::reset(ResetKind kind) {
    if (kind == ResetKind::warm) {
        // Soft Reset exception (UM 17 "Soft Reset Exception"): registers are preserved except
        // ErrorEPC, Status ERL/SR/BEV/TS/NMI, the PC, and pending external interrupts.
        cp0_.error_epc = state_.pc;
        cp0_.status =
            (cp0_.status | status::erl | status::sr | status::bev) & ~(status::ts | status::nmi);
        external_interrupts_ = 0;
        enter_reset_vector();
        update_derived_state();
        tracer_.log(TraceCategory::exception, "soft reset");
        return;
    }
    // Cold Reset exception (UM 17 "Cold Reset Exception"). Registers the manual leaves
    // undefined start at zero so that every run is deterministic.
    const std::uint64_t pc = state_.pc;
    state_ = {};
    fpu_ = {};
    cp0_ = {};
    tlb_.clear();
    cp0_.error_epc = pc;
    cp0_.status = status::erl | status::bev;
    cp0_.config = model_.identity().config;
    set_random(static_cast<std::uint32_t>(Tlb::entry_count - 1));
    cp0_.count_offset = static_cast<std::uint32_t>(0 - (cycles_ >> model_.count_shift()));
    external_interrupts_ = 0;
    timer_interrupt_ = false;
    watch_pending_ = false;
    enter_reset_vector();
    schedule_timer();
    update_derived_state();
}

void Cpu::nonmaskable_interrupt() {
    // UM 17 "NMI Exception": taken at an instruction boundary; Cause is unchanged.
    cp0_.error_epc = state_.pc;
    cp0_.status =
        (cp0_.status | status::erl | status::sr | status::bev | status::nmi) & ~status::ts;
    enter_reset_vector();
    update_derived_state();
    tracer_.log(TraceCategory::exception, "nmi");
}

void Cpu::enter_reset_vector() {
    invalidate_host_pages();
    state_.pc = reset_vector;
    state_.next_pc = reset_vector + 4;
    state_.delay_slot = false;
}

void Cpu::set_interrupt_level(std::uint32_t input, bool asserted) {
    invariant(input < external_interrupt_count, "R10000 has five external interrupts");
    const std::uint32_t mask = 1u << (input + 2);
    external_interrupts_ = asserted ? external_interrupts_ | mask : external_interrupts_ & ~mask;
    update_derived_state();
}

std::uint32_t Cpu::random() const {
    // Random decrements as instructions graduate, from 63 down to Wired, then continues from
    // 63 (UM 14.2); a value at or below Wired is followed by 63. cp0_.random was the value
    // when random_base_ instructions had retired.
    constexpr std::uint64_t top = Tlb::entry_count - 1;
    const std::uint64_t steps = retired() - random_base_;
    const std::uint64_t start = cp0_.random;
    const std::uint64_t wired = cp0_.wired;
    const std::uint64_t to_top = start > wired ? start - wired + 1 : 1;
    if (steps < to_top) {
        return static_cast<std::uint32_t>(start - steps);
    }
    return static_cast<std::uint32_t>(top - (steps - to_top) % (top - wired + 1));
}

void Cpu::schedule_timer() {
    // The next cycle after this one, at a Count step, at which
    // (cycle >> count_shift) + count_offset mod 2^32, Count, equals Compare.
    const std::uint64_t match = std::uint64_t{cp0_.compare - cp0_.count_offset}
                                << model_.count_shift();
    timer_cycle_ = (cycles_ & ~(timer_period() - 1)) + match;
    if (timer_cycle_ <= cycles_) {
        timer_cycle_ += timer_period();
    }
}

std::expected<Translation, Exception> Cpu::translate_address(std::uint64_t address,
                                                            AccessKind kind) {
    const auto address_error = [&] {
        return std::unexpected(Exception{.code = kind == AccessKind::store
                                                     ? ExceptionCode::address_error_store
                                                     : ExceptionCode::address_error_load,
                                         .bad_address = address});
    };
    const bool ux = (cp0_.status & status::ux) != 0;
    const bool sx = (cp0_.status & status::sx) != 0;
    const bool kx = (cp0_.status & status::kx) != 0;
    const bool erl = (cp0_.status & status::erl) != 0;
    const std::uint64_t region = address >> 62;

    switch (mode()) {
    case OperatingMode::user:
        if (ux) {
            return address < user_segment_limit ? translate_mapped(address, kind) : address_error();
        }
        // 32-bit User mode (UM 16.2): instruction fetches fault if any of the upper 33 bits
        // is set; data references clear the upper 32 bits before the check (UM Table 16-2
        // note).
        if (kind == AccessKind::fetch) {
            return (address >> 31) == 0 ? translate_mapped(address, kind) : address_error();
        }
        address &= 0xffff'ffff;
        return (address >> 31) == 0 ? translate_mapped(address, kind) : address_error();

    case OperatingMode::supervisor:
        if (!sx) {
            if (!is_sign_extended_32(address)) {
                return address_error();
            }
            if (address < bit(31) || (address >= csseg_first && address <= csseg_last)) {
                return translate_mapped(address, kind);
            }
            return address_error();
        }
        if (region == 0) {
            return address < (ux ? user_segment_limit : bit(31)) ? translate_mapped(address, kind)
                                                                 : address_error();
        }
        if (region == 1) {
            return address - supervisor_base < user_segment_limit ? translate_mapped(address, kind)
                                                                  : address_error();
        }
        if (address >= csseg_first && address <= csseg_last) {
            return translate_mapped(address, kind);
        }
        return address_error();

    case OperatingMode::kernel:
        break;
    }

    if (!kx) {
        return is_sign_extended_32(address) ? translate_compatibility(address, kind)
                                            : address_error();
    }
    switch (region) {
    case 0:
        // xkuseg (UM 16.2): the full 16 TB only with UX set and ERL clear.
        if (ux && !erl) {
            return address < user_segment_limit ? translate_mapped(address, kind) : address_error();
        }
        if (address >= bit(31)) {
            return address_error();
        }
        if (erl) {
            return Translation{PhysicalAddress{system_address_of(address, true, 0)}, true};
        }
        return translate_mapped(address, kind);
    case 1:
        return sx && address - supervisor_base < user_segment_limit
                   ? translate_mapped(address, kind)
                   : address_error();
    case 2: {
        // xkphys (UM 16.2, Figure 16-4). Bits 61:59 select the cache algorithm. Uncached
        // algorithms carry the uncached attribute in bits 58:57 and require bits 56:40 clear;
        // the others require bits 58:40 clear.
        const auto coherency = static_cast<unsigned>((address >> 59) & 7);
        const bool uncached = is_uncached(coherency);
        const std::uint64_t reserved_bits = uncached ? (bit(57) - bit(40)) : (bit(59) - bit(40));
        if ((address & reserved_bits) != 0) {
            return address_error();
        }
        return Translation{PhysicalAddress{system_address_of(address & physical_address_mask,
                                                             uncached, (address >> 57) & 3)},
                           uncached};
    }
    default:
        if (address >= compatibility_base) {
            return translate_compatibility(address, kind);
        }
        return address <= kernel_segment_last ? translate_mapped(address, kind) : address_error();
    }
}

std::expected<Translation, Exception> Cpu::translate_compatibility(std::uint64_t address,
                                                                   AccessKind kind) {
    // The 32-bit kernel segments and their 64-bit compatibility images (UM 16.2).
    const std::uint64_t low = address & 0xffff'ffff;
    if (low < 0x8000'0000) {
        // kuseg; with ERL set it becomes unmapped and uncached.
        if ((cp0_.status & status::erl) != 0) {
            return Translation{PhysicalAddress{system_address_of(low, true, 0)}, true};
        }
        return translate_mapped(low, kind);
    }
    if (low < 0xa000'0000) {
        // kseg0: unmapped, cache algorithm from Config.K0.
        const bool uncached = is_uncached(cp0_.config & 7);
        return Translation{PhysicalAddress{system_address_of(low - 0x8000'0000, uncached, 0)},
                           uncached};
    }
    if (low < 0xc000'0000) {
        // kseg1: unmapped, uncached.
        return Translation{PhysicalAddress{system_address_of(low - 0xa000'0000, true, 0)}, true};
    }
    // ksseg and kseg3: mapped.
    return translate_mapped(address, kind);
}

std::expected<Translation, Exception> Cpu::translate_mapped(std::uint64_t address,
                                                            AccessKind kind) {
    const ExceptionCode miss_code =
        kind == AccessKind::store ? ExceptionCode::tlb_store : ExceptionCode::tlb_load;
    const auto asid = static_cast<std::uint8_t>(cp0_.entry_hi & entry_hi::asid_mask);
    const auto lookup = tlb_.lookup(address, asid);
    if (!lookup) {
        // The refill vector depends on the region of the address and its extended addressing
        // bit, not on the current mode. sseg/ksseg follow KX (UM 17.3, Table 17-2).
        const std::uint64_t region = address >> 62;
        bool extended = false;
        if (region == 0) {
            extended = (cp0_.status & status::ux) != 0;
        } else if (region == 1) {
            extended = (cp0_.status & status::sx) != 0;
        } else {
            extended = (cp0_.status & status::kx) != 0;
        }
        return std::unexpected(Exception{.code = miss_code,
                                         .bad_address = address,
                                         .tlb_refill = true,
                                         .extended_refill = extended});
    }
    const TlbEntry& entry = tlb_.entry(lookup->index);
    const std::uint64_t lo = lookup->odd ? entry.entry_lo1 : entry.entry_lo0;
    if ((lo & entry_lo::v) == 0) {
        return std::unexpected(Exception{.code = miss_code, .bad_address = address});
    }
    if (kind == AccessKind::store && (lo & entry_lo::d) == 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::tlb_modified, .bad_address = address});
    }
    const std::uint64_t size = Tlb::page_size(entry);
    const std::uint64_t frame = ((lo & entry_lo::pfn_mask) >> entry_lo::pfn_shift) << 12;
    const std::uint64_t physical = (frame & ~(size - 1)) | (address & (size - 1));
    const bool uncached = is_uncached(static_cast<unsigned>((lo >> entry_lo::c_shift) & 7));
    return Translation{
        PhysicalAddress{system_address_of(physical, uncached, lo >> entry_lo::uc_shift)}, uncached,
        static_cast<std::int8_t>(lookup->index)};
}

std::expected<void, Exception> Cpu::check_watch(const Translation& translation, AccessKind kind) {
    // UM 17 "Watch Exception": matches physical doubleword addresses of loads and stores.
    const std::uint32_t enable = kind == AccessKind::store ? 1u : 2u;
    if (kind == AccessKind::fetch || (cp0_.watch_lo & enable) == 0) {
        return {};
    }
    const std::uint64_t watched =
        (std::uint64_t{cp0_.watch_hi & 0xff} << 32) | (cp0_.watch_lo & 0xffff'fff8u);
    if ((translation.address.value & physical_address_mask & ~std::uint64_t{7}) != watched) {
        return {};
    }
    if ((cp0_.status & (status::exl | status::erl)) != 0) {
        // Deferred until EXL and ERL are both clear; the reference still happens.
        watch_pending_ = true;
        update_derived_state();
        return {};
    }
    return std::unexpected(Exception{.code = ExceptionCode::watch});
}

PhysicalAddress Cpu::bus_address(const Translation& translation, AccessWidth width) const {
    // A reference in the opposite byte order to the bus selects the mirrored byte lanes of
    // the doubleword; the value is unchanged. See MIPS.adoc "Byte order".
    PhysicalAddress address = translation.address;
    if (big_endian() != (bus_.byte_order() == ByteOrder::big)) {
        address.value ^= (8 - byte_count(width)) & 7;
    }
    return address;
}

std::expected<std::uint64_t, Exception> Cpu::read(const Translation& translation, AccessWidth width,
                                                  AccessKind kind) {
    if (auto watch = check_watch(translation, kind); !watch) {
        return std::unexpected(watch.error());
    }
    const PhysicalAddress address = bus_address(translation, width);
    if (kind != AccessKind::fetch && !translation.uncached && caches_.holds_data_lines()) {
        if (const auto way = cached_line(translation, address)) {
            return caches_.read_data(primary_index(translation, address), *way,
                                     static_cast<unsigned>(byte_count(width)));
        }
    }
    left_host_path_ = true;
    last_bus_access_ = classify_bus_access(address, width, translation.uncached);
    auto value = bus_.read(address, width, requester_);
    check_host_pages();
    if (!value) {
        // A read that receives an error response takes a bus error (UM 17 "Bus Error").
        tracer_.log(TraceCategory::memory, "read fault {:#x} width {} fault {}", address.value,
                    byte_count(width), std::to_underlying(value.error()));
        return std::unexpected(Exception{.code = kind == AccessKind::fetch
                                                     ? ExceptionCode::bus_error_instruction
                                                     : ExceptionCode::bus_error_data});
    }
    return *value;
}

std::expected<void, Exception> Cpu::write(const Translation& translation, AccessWidth width,
                                          std::uint64_t value) {
    if (auto watch = check_watch(translation, AccessKind::store); !watch) {
        return watch;
    }
    const PhysicalAddress address = bus_address(translation, width);
    if (!translation.uncached && caches_.holds_data_lines()) {
        if (const auto way = cached_line(translation, address)) {
            caches_.write_data(primary_index(translation, address), *way,
                               static_cast<unsigned>(byte_count(width)), value);
            return {};
        }
    }
    left_host_path_ = true;
    last_bus_access_ = classify_bus_access(address, width, translation.uncached);
    const std::uint64_t code_writes = bus_.code_writes();
    auto result = bus_.write(address, width, value, requester_);
    // Host threads use no reservations (store_conditional).
    if (coherence_ != nullptr && !parallel_ && result) {
        coherence_->store(coherence_index_, address, byte_count(width));
    }
    if (bus_.code_writes() != code_writes) {
        last_bus_access_ = BusAccess::code_store;
        ++code_frame_stores_;
        synchronize_code();
    }
    check_host_pages();
    if (!result) {
        // Only reads can take bus errors on the R10000 (UM 17 "Bus Error Exception" lists
        // read requests only). A refused write is lost; report it for diagnosis.
        tracer_.log(TraceCategory::memory, "write fault {:#x} width {} value {:#x} fault {}",
                    address.value, byte_count(width), value, std::to_underlying(result.error()));
    }
    return {};
}

std::optional<unsigned> Cpu::cached_line(const Translation& translation,
                                         PhysicalAddress address) {
    // The arrays hold big-endian words; the IP27 is big-endian throughout.
    if (!big_endian() || bus_.byte_order() != ByteOrder::big) {
        return std::nullopt;
    }
    std::optional<CacheArrays::Writeback> replaced;
    const auto way = caches_.data_reference(primary_index(translation, address),
                                            address.value & cache_physical_bits, replaced);
    if (replaced) {
        write_back(*replaced);
    }
    return way;
}

void Cpu::write_back(const CacheArrays::Writeback& line) {
    // hypothesis: with no secondary line modeled for it, the data goes to memory as the
    // secondary cache's writeback would (UM 10.3, 10.4).
    for (std::size_t i = 0; i < line.words.size(); i += 2) {
        const PhysicalAddress at{line.address + 4 * i};
        const std::uint64_t value = (std::uint64_t{line.words[i]} << 32) | line.words[i + 1];
        const auto result = bus_.write(at, AccessWidth::bits64, value, requester_);
        if (coherence_ != nullptr && !parallel_ && result) {
            coherence_->store(coherence_index_, at, 8);
        }
    }
    synchronize_code();
}

std::expected<std::uint64_t, Exception> Cpu::load_slow(std::uint64_t address, AccessWidth width) {
    if (address % byte_count(width) != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_load, .bad_address = address});
    }
    if (const DevicePage* page = device_page(address, false)) {
        return device_read(*page, address, width);
    }
    auto translation = translate(address, AccessKind::load);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    filling_ = true;
    auto value = read(*translation, width, AccessKind::load);
    filling_ = false;
    if (value) {
        remember_page(address, *translation, AccessKind::load);
        remember_device_page(address, *translation, AccessKind::load);
    }
    return value;
}

// read() for an address in a device page: the same device access, without translation and bus
// decode (their results are the page's).
std::expected<std::uint64_t, Exception> Cpu::device_read(const DevicePage& page,
                                                         std::uint64_t address,
                                                         AccessWidth width) {
    left_host_path_ = true;
    last_bus_access_ = BusAccess::device;
    const std::uint64_t offset = address & page_offset_mask;
    auto value = page.target->mmio_read(page.offset + offset, width);
    invariant(!value || width == AccessWidth::bits64 || *value >> (8 * byte_count(width)) == 0,
              "MMIO read value wider than the access");
    check_host_pages();
    if (!value) {
        tracer_.log(TraceCategory::memory, "read fault {:#x} width {} fault {}",
                    page.physical + offset, byte_count(width), std::to_underlying(value.error()));
        return std::unexpected(Exception{.code = ExceptionCode::bus_error_data});
    }
    return *value;
}

// write() for an address in a device page, likewise.
void Cpu::device_write(const DevicePage& page, std::uint64_t address, AccessWidth width,
                       std::uint64_t value) {
    left_host_path_ = true;
    last_bus_access_ = BusAccess::device;
    const std::uint64_t offset = address & page_offset_mask;
    const std::uint64_t code_writes = bus_.code_writes();
    auto result = page.target->mmio_write(page.offset + offset, width, value);
    if (bus_.code_writes() != code_writes) {
        last_bus_access_ = BusAccess::code_store;
        ++code_frame_stores_;
        synchronize_code();
    }
    check_host_pages();
    if (!result) {
        tracer_.log(TraceCategory::memory, "write fault {:#x} width {} value {:#x} fault {}",
                    page.physical + offset, byte_count(width), value,
                    std::to_underlying(result.error()));
    }
}

// After a successful access through the full path: remembers its page when the whole page
// translates to one device mapping under the conditions that make device_read and
// device_write exact (no watchpoint armed, references in the bus byte order).
void Cpu::remember_device_page(std::uint64_t address, const Translation& translation,
                               AccessKind kind) {
    if (last_bus_access_ != BusAccess::device || (cp0_.watch_lo & 3) != 0 ||
        big_endian() != (bus_.byte_order() == ByteOrder::big)) {
        return;
    }
    const std::uint64_t page = address & ~page_offset_mask;
    const PhysicalAddress frame{translation.address.value & ~page_offset_mask};
    const auto range = bus_.mmio_range(frame, page_offset_mask + 1, requester_);
    if (!range) {
        return;
    }
    DevicePage& entry = device_pages_[(address >> 12) % device_page_count];
    const bool stores = kind == AccessKind::store || (entry.page == page && entry.stores &&
                                                       entry.context == translation_context());
    entry = {.page = page,
             .context = translation_context(),
             .target = range->target,
             .offset = range->offset,
             .physical = frame.value,
             .tlb_index = translation.tlb_index,
             .stores = stores};
}

std::expected<void, Exception> Cpu::store_slow(std::uint64_t address, AccessWidth width,
                                               std::uint64_t value) {
    if (address % byte_count(width) != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_store, .bad_address = address});
    }
    if (const DevicePage* page = device_page(address, true)) {
        device_write(*page, address, width, value);
        return {};
    }
    auto translation = translate(address, AccessKind::store);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    filling_ = true;
    auto result = write(*translation, width, value);
    filling_ = false;
    if (result) {
        remember_page(address, *translation, AccessKind::store);
        remember_device_page(address, *translation, AccessKind::store);
    }
    return result;
}

std::optional<Cpu::CodePage> Cpu::code_page(std::uint64_t address) {
    if (address % 4 != 0) {
        return std::nullopt;
    }
    const std::uint64_t page = address & ~page_offset_mask;
    const auto hit = [&]() -> std::optional<CodePage> {
        const HostPage& entry = code_pages_[host_page_slot(address)];
        if (entry.page == host_page_tag(address)) {
            return CodePage{entry.bytes, bus_.byte_order(), entry.block, entry.frame,
                            entry.tlb_index};
        }
        return std::nullopt;
    };
    if (auto found = hit()) {
        return found;
    }
    // Fill as a fetch would, without reading: remember_page caches plain memory only.
    const auto translation = translate(address, AccessKind::fetch);
    if (!translation) {
        return std::nullopt;
    }
    remember_page(address, *translation, AccessKind::fetch);
    return hit();
}

void Cpu::claim_code_frame(const CodePage& page) {
    if (page.tlb_index >= 0) {
        tlb_code_entries_ |= std::uint64_t{1} << page.tlb_index;
    }
    // A marked frame is never cached as a writable host page (remember_page), and every CPU
    // dropped its writable pages when the frame was marked. So only a first mark has more to
    // do (SMP.adoc "S4.0": a mark per build cost a drop every 2,000 cycles).
    if (!page.block->mark_code(page.frame)) {
        return;
    }
    // Other CPUs may hold the frame as a writable host page; their stores must reach the bus
    // from now on.
    if (coherence_ != nullptr) {
        const std::uint64_t generation = coherence_->drop_writable_pages();
        if (parallel_) {
            // They run now, so wait until each has dropped its pages (SMP.adoc "S4"). Other
            // CPUs may have advanced the generation since this one last looked: drop all.
            drop_writable_pages();
            await_acknowledgments(generation);
            return;
        }
        seen_coherence_ = generation;
    }
    for (HostPage& entry : data_pages_) {
        if (entry.writable_bytes == page.bytes) {
            entry = {};
        }
    }
}

void Cpu::await_acknowledgments(std::uint64_t generation) {
    for (unsigned other = 0; other < Coherence::max_cpus; ++other) {
        if (other == coherence_index_) {
            continue;
        }
        for (unsigned spins = 0; !coherence_->acknowledged(other, generation); ++spins) {
            // Attention can be lost when the other CPU recomputes its check at that moment;
            // asking again bounds the wait.
            if (spins % 64 == 0) {
                coherence_->cpu(other)->raise_attention();
            }
            // The other CPU may be waiting for this one in the same way.
            if (coherence_->generation() != seen_coherence_) {
                drop_writable_pages();
            }
            std::this_thread::yield();
        }
    }
}

void Cpu::set_parallel(bool parallel) {
    parallel_ = parallel;
    ll_address_ = no_ll_address;
    if (coherence_ != nullptr) {
        coherence_->release(coherence_index_);
    }
}

void Cpu::post_interrupt_levels(std::uint32_t levels) {
    posted_levels_.store(levels, std::memory_order_relaxed);
    levels_posted_.store(true, std::memory_order_release);
    raise_attention();
}

void Cpu::post_reset(ResetKind kind, std::uint64_t release) {
    posted_release_.store(release, std::memory_order_relaxed);
    posted_reset_.store(static_cast<int>(kind), std::memory_order_release);
    raise_attention();
}

void Cpu::raise_attention() {
    attention_.store(true, std::memory_order_seq_cst);
    interrupt_check_.store(true, std::memory_order_relaxed);
}

bool Cpu::attend() {
    attention_.store(false, std::memory_order_seq_cst);
    bool reset_taken = false;
    if (const int kind = posted_reset_.exchange(-1, std::memory_order_acq_rel); kind >= 0) {
        hold_until(reset_rendezvous_ != nullptr ? reset_rendezvous_->arrive(*this, cycles_)
                                                : posted_release_.load(std::memory_order_relaxed));
        reset(static_cast<ResetKind>(kind));
        reset_taken = true;
    }
    if (levels_posted_.exchange(false, std::memory_order_acq_rel)) {
        constexpr std::uint32_t inputs = (1u << external_interrupt_count) - 1;
        external_interrupts_ = (posted_levels_.load(std::memory_order_relaxed) & inputs) << 2;
    }
    if (coherence_ != nullptr && coherence_->generation() != seen_coherence_) {
        drop_writable_pages();
    }
    update_derived_state();
    return reset_taken;
}

void Cpu::park() {
    coherence_->park(coherence_index_);
}

void Cpu::unpark() {
    coherence_->unpark(coherence_index_);
    if (coherence_->generation_after_unpark() != seen_coherence_) {
        drop_writable_pages();
    }
}

void Cpu::note_load_linked(std::uint64_t address, AccessWidth width, std::uint64_t value) {
    if (coherence_ == nullptr) {
        return;
    }
    // The load has just translated `address`, so this cannot fault.
    if (const auto translation = translate(address, AccessKind::load)) {
        if (parallel_) {
            ll_address_ = bus_address(*translation, width);
            ll_value_ = value;
            return;
        }
        coherence_->reserve(coherence_index_, translation->address);
        // Our own reservation leaves our pages alone.
        seen_coherence_ = coherence_->generation();
    }
}

std::expected<bool, Exception> Cpu::store_conditional(const Translation& translation,
                                                      AccessWidth width, std::uint64_t value) {
    if (!state_.ll_bit) {
        return false;
    }
    const auto plain_store = [&]() -> std::expected<bool, Exception> {
        if (auto result = write(translation, width, value); !result) {
            return std::unexpected(result.error());
        }
        return true;
    };
    if (!parallel_) {
        if (coherence_ != nullptr && !coherence_->held(coherence_index_)) {
            return false;
        }
        return plain_store();
    }
    // Host threads: SC succeeds when memory still holds what LL loaded, as one host
    // compare-and-swap; at another address than LL's it fails (the reservation is LL's). Where
    // the swap cannot apply (uncached or device space, lines only this CPU's cache holds), an
    // ordinary store, as when no other CPU wrote the line.
    const PhysicalAddress address = bus_address(translation, width);
    const std::uint64_t size = byte_count(width);
    if (address != ll_address_) {
        return false;
    }
    if (translation.uncached || caches_.holds_data_lines()) {
        return plain_store();
    }
    if (auto watch = check_watch(translation, AccessKind::store); !watch) {
        return std::unexpected(watch.error());
    }
    const std::uint64_t code_writes = bus_.code_writes();
    const std::span<std::byte> bytes = bus_.writable_memory_bytes(address, size);
    if (bytes.size() != size) {
        return plain_store();
    }
    std::array<std::byte, 8> old_bytes{};
    std::array<std::byte, 8> new_bytes{};
    const std::uint64_t mask = width == AccessWidth::bits64 ? ~std::uint64_t{0} : 0xffff'ffff;
    store_unsigned(std::span{old_bytes}.first(size), ll_value_ & mask, bus_.byte_order());
    store_unsigned(std::span{new_bytes}.first(size), value & mask, bus_.byte_order());
    bool stored = false;
    // The access is aligned to its size (SC checked), and so are the host bytes behind it.
    if (width == AccessWidth::bits64) {
        std::uint64_t expected{};
        std::uint64_t desired{};
        std::memcpy(&expected, old_bytes.data(), 8);
        std::memcpy(&desired, new_bytes.data(), 8);
        stored = std::atomic_ref{*reinterpret_cast<std::uint64_t*>(bytes.data())}
                     .compare_exchange_strong(expected, desired);
    } else {
        std::uint32_t expected{};
        std::uint32_t desired{};
        std::memcpy(&expected, old_bytes.data(), 4);
        std::memcpy(&desired, new_bytes.data(), 4);
        stored = std::atomic_ref{*reinterpret_cast<std::uint32_t*>(bytes.data())}
                     .compare_exchange_strong(expected, desired);
    }
    left_host_path_ = true;
    last_bus_access_ = BusAccess::other;
    if (bus_.code_writes() != code_writes) {
        last_bus_access_ = BusAccess::code_store;
        ++code_frame_stores_;
        synchronize_code();
    }
    check_host_pages();
    return stored;
}

void Cpu::drop_writable_pages() {
    seen_coherence_ = coherence_->generation();
    for (HostPage& entry : data_pages_) {
        if (entry.writable_bytes != nullptr) {
            entry = {};
        }
    }
    if (parallel_) {
        coherence_->acknowledge(coherence_index_, seen_coherence_);
    }
}

Cpu::BusAccess Cpu::classify_bus_access(PhysicalAddress address, AccessWidth width,
                                        bool uncached) const {
    if (bus_.memory_bytes(address, byte_count(width)).empty()) {
        return BusAccess::device;
    }
    if (uncached) {
        return BusAccess::uncached_memory;
    }
    return filling_ ? BusAccess::memory_fill : BusAccess::other;
}

void Cpu::forget_tlb_pages(std::uint64_t entries) {
    // Only entries that translated decoded code can invalidate a block.
    if ((entries & tlb_code_entries_) != 0) {
        tlb_code_entries_ &= ~entries;
        ++code_changes_;
    }
    for (std::size_t index = 0; index < Tlb::entry_count; ++index) {
        if ((entries >> index & 1) != 0) {
            ++tlb_generations_[index];
        }
    }
    for (DevicePage& page : device_pages_) {
        if (page.tlb_index >= 0 && (entries >> page.tlb_index & 1) != 0) {
            page = {};
        }
    }
    for (auto* pages : {&code_pages_, &data_pages_}) {
        for (HostPage& page : *pages) {
            if (page.tlb_index >= 0 && (entries >> page.tlb_index & 1) != 0) {
                page = {};
            }
        }
    }
}

void Cpu::remember_page(std::uint64_t address, const Translation& translation, AccessKind kind) {
    constexpr std::uint64_t page_size = page_offset_mask + 1;
    if ((kind != AccessKind::fetch && (cp0_.watch_lo & 3) != 0) ||
        big_endian() != (bus_.byte_order() == ByteOrder::big)) {
        return;
    }
    check_host_pages();
    const std::uint64_t page = address & ~page_offset_mask;
    const PhysicalAddress frame{translation.address.value & ~page_offset_mask};
    const auto memory = bus_.memory_frame(frame);
    if (!memory) {
        return;
    }
    // While the primary data cache holds lines, cached data references go through read and
    // write, which consult it.
    if (kind != AccessKind::fetch && !translation.uncached && caches_.holds_data_lines()) {
        return;
    }
    if (kind == AccessKind::store) {
        // A frame holding decoded code is never writable through the host page cache: its
        // stores must reach the bus to advance its generation.
        if (memory->block->holds_code(memory->offset)) {
            return;
        }
        // Nor is a frame where another CPU holds a reservation: the stores must break it.
        if (coherence_ != nullptr && coherence_->watched(coherence_index_, frame)) {
            return;
        }
        const auto bytes = bus_.writable_memory_bytes(frame, page_size);
        if (bytes.size() == page_size) {
            const std::uint64_t tag = page | context_tag_;
            data_pages_[host_page_slot(address)] = {
                tag,           bytes.data(),   bytes.data(),
                reinterpret_cast<std::uintptr_t>(bytes.data()) - page,
                translation.tlb_index, memory->block, memory->offset,
                tag};
        }
        return;
    }
    const auto bytes = bus_.memory_bytes(frame, page_size);
    if (bytes.size() == page_size) {
        (kind == AccessKind::fetch ? code_pages_ : data_pages_)[host_page_slot(address)] = {
            page | context_tag_,
            bytes.data(),
            nullptr,
            reinterpret_cast<std::uintptr_t>(bytes.data()) - page,
            translation.tlb_index,
            memory->block,
            memory->offset};
    }
}

std::expected<std::uint32_t, Exception> Cpu::fetch_slow(std::uint64_t address) {
    if (address % 4 != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_load, .bad_address = address});
    }
    auto translation = translate(address, AccessKind::fetch);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    auto word = read(*translation, AccessWidth::bits32, AccessKind::fetch);
    if (!word) {
        return std::unexpected(word.error());
    }
    remember_page(address, *translation, AccessKind::fetch);
    return static_cast<std::uint32_t>(*word);
}

std::uint64_t Cpu::read_cp0(unsigned reg) const {
    switch (reg) {
    case cp0::index:
        return cp0_.index;
    case cp0::random:
        return random();
    case cp0::entry_lo0:
        return cp0_.entry_lo0;
    case cp0::entry_lo1:
        return cp0_.entry_lo1;
    case cp0::context:
        return cp0_.context;
    case cp0::page_mask:
        return cp0_.page_mask;
    case cp0::wired:
        return cp0_.wired;
    case cp0::bad_vaddr:
        return cp0_.bad_vaddr;
    case cp0::count:
        return count();
    case cp0::entry_hi:
        return cp0_.entry_hi;
    case cp0::compare:
        return cp0_.compare;
    case cp0::status:
        return cp0_.status;
    case cp0::cause:
        return cause_value();
    case cp0::epc:
        return cp0_.epc;
    case cp0::prid:
        return model_.identity().processor_id;
    case cp0::config:
        return cp0_.config;
    case cp0::ll_addr:
        return cp0_.ll_addr;
    case cp0::watch_lo:
        return cp0_.watch_lo;
    case cp0::watch_hi:
        return cp0_.watch_hi;
    case cp0::xcontext:
        return cp0_.xcontext;
    case cp0::frame_mask:
        return cp0_.frame_mask;
    case cp0::diagnostic:
        return cp0_.diagnostic;
    case cp0::ecc:
        return cp0_.ecc;
    case cp0::tag_lo:
        return cp0_.tag_lo;
    case cp0::tag_hi:
        return cp0_.tag_hi;
    case cp0::error_epc:
        return cp0_.error_epc;
    default:
        // Reserved registers, the performance counters, and CacheErr are not modeled.
        // The value of a nonexistent register is undefined (UM 14.26); read zero.
        tracer_.log(TraceCategory::cpu, "read of unmodeled CP0 register {}", reg);
        return 0;
    }
}

void Cpu::write_cp0(unsigned reg, std::uint64_t value) {
    const auto low = static_cast<std::uint32_t>(value);
    switch (reg) {
    case cp0::index:
        // The probe-failure bit is set only by TLBP.
        cp0_.index = (cp0_.index & 0x8000'0000u) | (low & 0x3f);
        return;
    case cp0::entry_lo0:
        cp0_.entry_lo0 = value & entry_lo::writable;
        return;
    case cp0::entry_lo1:
        cp0_.entry_lo1 = value & entry_lo::writable;
        return;
    case cp0::context:
        // PTEBase, bits 63:23; BadVPN2 is written by hardware.
        cp0_.context = (value & ~(bit(23) - 1)) | (cp0_.context & (bit(23) - 1));
        return;
    case cp0::page_mask:
        cp0_.page_mask = low & static_cast<std::uint32_t>(page_mask_writable);
        return;
    case cp0::wired:
        cp0_.wired = low & 0x3f;
        set_random(static_cast<std::uint32_t>(Tlb::entry_count - 1));
        return;
    case cp0::count:
        cp0_.count_offset = low - static_cast<std::uint32_t>(cycles_ >> model_.count_shift());
        schedule_timer();
        tracer_.log(TraceCategory::irq, "count = {:#x}", low);
        return;
    case cp0::entry_hi:
        cp0_.entry_hi = value & entry_hi::writable;
        return;
    case cp0::compare:
        cp0_.compare = low;
        timer_interrupt_ = false;
        schedule_timer();
        tracer_.log(TraceCategory::irq, "compare = {:#x} (count {:#x})", low, count());
        return;
    case cp0::status:
        cp0_.status = low & status::writable;
        return;
    case cp0::cause:
        cp0_.cause =
            (cp0_.cause & ~cause::software_interrupts) | (low & cause::software_interrupts);
        return;
    case cp0::epc:
        cp0_.epc = value;
        return;
    case cp0::config:
        invalidate_host_pages();
        // hypothesis: only K0 is writable; the remaining fields are latched mode bits.
        cp0_.config = (cp0_.config & ~7u) | (low & 7u);
        return;
    case cp0::ll_addr:
        cp0_.ll_addr = low;
        return;
    case cp0::watch_lo:
        cp0_.watch_lo = low & 0xffff'fffbu;
        data_pages_.fill({}); // armed watchpoints need the full path
        device_pages_.fill({});
        watch_pending_ = false;
        return;
    case cp0::watch_hi:
        cp0_.watch_hi = low & 0xff;
        return;
    case cp0::xcontext:
        // PTEBase, bits 63:37 (UM 14.17); R and BadVPN2 are written by hardware.
        cp0_.xcontext = (value & ~(bit(37) - 1)) | (cp0_.xcontext & (bit(37) - 1));
        return;
    case cp0::frame_mask:
        cp0_.frame_mask = low & 0xffff;
        return;
    case cp0::diagnostic:
        cp0_.diagnostic = value;
        return;
    case cp0::ecc:
        // A 10-bit register (UM 14.21).
        cp0_.ecc = low & 0x3ff;
        return;
    case cp0::tag_lo:
        cp0_.tag_lo = low;
        return;
    case cp0::tag_hi:
        cp0_.tag_hi = low;
        return;
    case cp0::error_epc:
        cp0_.error_epc = value;
        return;
    case cp0::random:
    case cp0::bad_vaddr:
    case cp0::prid:
        return;
    default:
        tracer_.log(TraceCategory::cpu, "write of unmodeled CP0 register {}", reg);
        return;
    }
}

std::uint64_t Cpu::mfc0(unsigned reg) const {
    return sign_extend_32(read_cp0(reg));
}

std::uint64_t Cpu::dmfc0(unsigned reg) const {
    const std::uint64_t value = read_cp0(reg);
    // A 32-bit register reads zero-extended (UM Table 14-26).
    return is_64bit_register(reg) ? value : value & 0xffff'ffff;
}

void Cpu::mtc0(unsigned reg, std::uint64_t value) {
    // UM Table 14-26: MTC0 to a 64-bit register copies all 64 bits of rt (rd <- rt63..0), as
    // DMTC0 does; only MFC0 truncates. observed: IRIX loads EntryLo with MTC0, and the uncached
    // attribute of I/O mappings is in bits 63:62.
    write_cp0(reg, is_64bit_register(reg) ? value : value & 0xffff'ffff);
    update_derived_state();
}

void Cpu::dmtc0(unsigned reg, std::uint64_t value) {
    write_cp0(reg, is_64bit_register(reg) ? value : value & 0xffff'ffff);
    update_derived_state();
}

TlbEntry Cpu::tlb_entry_from_registers() const {
    // UM 14.36: the TLB receives PageMask, EntryHi without the masked VPN2 bits, and both
    // EntryLo registers; G is the AND of the two G bits; FrameMask clears PFN bits 33:18
    // (UM 14.18); PFN bits below the page size are forced to zero.
    const std::uint64_t mask_bits = (cp0_.page_mask >> 13) & 0xfff;
    const std::uint64_t frame_mask = std::uint64_t{cp0_.frame_mask} << 18;
    const std::uint64_t low_pfn = mask_bits << entry_lo::pfn_shift;
    const auto lo = [&](std::uint64_t value) {
        return value & ~frame_mask & ~low_pfn & ~entry_lo::g;
    };
    return TlbEntry{
        .page_mask = cp0_.page_mask,
        .entry_hi = cp0_.entry_hi & ~(mask_bits << entry_hi::vpn2_shift),
        .entry_lo0 = lo(cp0_.entry_lo0),
        .entry_lo1 = lo(cp0_.entry_lo1),
        .global = (cp0_.entry_lo0 & cp0_.entry_lo1 & entry_lo::g) != 0,
        .enabled = true,
    };
}

void Cpu::write_tlb(std::size_t index) {
    const std::uint64_t invalidated = tlb_.write(index, tlb_entry_from_registers());
    forget_tlb_pages(invalidated | std::uint64_t{1} << index);
    const bool conflict = invalidated != 0;
    // TS reports whether this write invalidated conflicting entries (UM 14.10).
    cp0_.status = conflict ? cp0_.status | status::ts : cp0_.status & ~status::ts;
    update_derived_state();
    tracer_.log(TraceCategory::tlb, "write {} hi {:#x} lo0 {:#x} lo1 {:#x} mask {:#x}{}", index,
                cp0_.entry_hi, cp0_.entry_lo0, cp0_.entry_lo1, cp0_.page_mask,
                conflict ? " (conflict)" : "");
}

void Cpu::tlb_probe() {
    if (const auto match = tlb_.probe(cp0_.entry_hi)) {
        cp0_.index = static_cast<std::uint32_t>(*match);
    } else {
        cp0_.index = 0x8000'0000u;
    }
}

void Cpu::tlb_read() {
    const TlbEntry& entry = tlb_.entry(cp0_.index & 0x3f);
    const std::uint64_t g = entry.global ? entry_lo::g : 0;
    cp0_.page_mask = static_cast<std::uint32_t>(entry.page_mask);
    cp0_.entry_hi = entry.entry_hi;
    update_derived_state();
    cp0_.entry_lo0 = entry.entry_lo0 | g;
    cp0_.entry_lo1 = entry.entry_lo1 | g;
}

void Cpu::tlb_write_indexed() {
    write_tlb(cp0_.index & 0x3f);
}

void Cpu::tlb_write_random() {
    write_tlb(random());
}

std::uint64_t Cpu::exception_return() {
    std::uint64_t target = 0;
    if ((cp0_.status & status::erl) != 0) {
        target = cp0_.error_epc;
        cp0_.status &= ~status::erl;
    } else {
        target = cp0_.epc;
        cp0_.status &= ~status::exl;
    }
    update_derived_state();
    state_.ll_bit = false;
    if (coherence_ != nullptr && !parallel_) {
        coherence_->release(coherence_index_);
    }
    return target;
}

void Cpu::take_exception(const Exception& exception) {
    const std::uint64_t address = exception.bad_address;
    if (is_tlb_exception(exception.code)) {
        // UM 17 "TLB Exceptions": BadVAddr, Context, XContext, and EntryHi describe the
        // failed address; the ASID is unchanged.
        const std::uint64_t region = address >> 62;
        const std::uint64_t vpn2 = (address >> 13) & (bit(31) - 1);
        cp0_.bad_vaddr = address;
        cp0_.context = (cp0_.context & ~(bit(23) - 1)) | ((vpn2 & (bit(19) - 1)) << 4);
        cp0_.xcontext = (cp0_.xcontext & ~(bit(37) - 1)) | (region << 35) | (vpn2 << 4);
        cp0_.entry_hi = (region << 62) | (vpn2 << 13) | (cp0_.entry_hi & entry_hi::asid_mask);
    } else if (exception.code == ExceptionCode::address_error_load ||
               exception.code == ExceptionCode::address_error_store) {
        cp0_.bad_vaddr = address;
    }

    std::uint64_t offset = 0x180;
    if ((cp0_.status & status::exl) == 0) {
        // With EXL already set, EPC and BD are not written and TLB refills use the general
        // vector (UM 14.12, 17.2).
        cp0_.epc = state_.delay_slot ? state_.pc - 4 : state_.pc;
        cp0_.cause = state_.delay_slot ? cp0_.cause | cause::bd : cp0_.cause & ~cause::bd;
        if (exception.tlb_refill) {
            offset = exception.extended_refill ? 0x080 : 0x000;
        }
    }
    cp0_.cause = (cp0_.cause & ~(cause::exc_code_mask | cause::ce_mask)) |
                 (std::uint32_t{std::to_underlying(exception.code)} << cause::exc_code_shift) |
                 (std::uint32_t{exception.coprocessor} << cause::ce_shift);
    cp0_.status |= status::exl;
    update_derived_state();

    const std::uint64_t base =
        (cp0_.status & status::bev) != 0 ? 0xffff'ffff'bfc0'0200 : 0xffff'ffff'8000'0000;
    tracer_.log(TraceCategory::exception, "code {} pc {:#x} bad {:#x} vector {:#x}",
                std::to_underlying(exception.code), state_.pc, address, base + offset);
    state_.pc = base + offset;
    state_.next_pc = state_.pc + 4;
    state_.delay_slot = false;
}

bool Cpu::take_pending_exception() {
    // Another thread asked for attention (host-parallel CPUs): a reset takes the place of the
    // next instruction; otherwise go on with what is now pending.
    if (attention_.load(std::memory_order_relaxed)) [[unlikely]] {
        if (attend()) {
            return true;
        }
    }
    if ((cp0_.status & (status::exl | status::erl)) == 0) {
        if (watch_pending_) {
            watch_pending_ = false;
            take_exception(Exception{.code = ExceptionCode::watch}); // updates the derived state
            return true;
        }
        const std::uint32_t pending = (cause_value() >> cause::ip_shift) & 0xff;
        const std::uint32_t enabled = (cp0_.status >> status::im_shift) & 0xff;
        if ((cp0_.status & status::ie) != 0 && (pending & enabled) != 0) {
            take_exception(Exception{.code = ExceptionCode::interrupt});
            return true;
        }
    }
    // Nothing to take, yet the check was set: on host threads another thread's attention can
    // set it after attend() recomputed it, and left set, every engine would stop at every
    // block without advancing (measured: a parallel `reboot2` spun for hours). Recompute it.
    update_derived_state();
    return false;
}

std::expected<void, Exception> Cpu::cache(unsigned operation, std::uint64_t address) {
    // Index operations use the virtual address (primary caches) or the translated physical
    // address (secondary); Hit operations compare the physical address (UM 10.1).
    auto translation = translate(address, AccessKind::load);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    constexpr std::uint64_t physical_bits = (std::uint64_t{1} << 40) - 1;
    CacheRegisters registers{cp0_.tag_lo, cp0_.tag_hi, cp0_.ecc};
    const bool held = caches_.holds_data_lines();
    const auto ch =
        caches_.execute(operation, address, translation->address.value & physical_bits, registers);
    for (const CacheArrays::Writeback& line : caches_.take_writebacks()) {
        write_back(line);
    }
    if (caches_.holds_data_lines() && !held) {
        // Cached data references must now consult the primary data cache, not host pages.
        for (HostPage& entry : data_pages_) {
            entry = {};
        }
    }
    cp0_.tag_lo = registers.tag_lo;
    cp0_.tag_hi = registers.tag_hi;
    cp0_.ecc = registers.ecc;
    if (ch) {
        cp0_.status = *ch ? cp0_.status | status::ch : cp0_.status & ~status::ch;
        update_derived_state();
    }
    return {};
}

Cpu::State Cpu::capture() const {
    State state{.integer = state_,
                .fpu = fpu_,
                .cp0 = cp0_,
                .tlb = tlb_.entries(),
                .cycles = cycles_,
                .retired = retired(),
                .external_interrupts = external_interrupts_,
                .timer_interrupt = timer_interrupt_,
                .watch_pending = watch_pending_};
    state.cp0.random = random();
    return state;
}

void Cpu::save_state(StateImage& image, std::string_view prefix) const {
    const auto key = [prefix](std::string_view name) { return std::string{prefix}.append(name); };
    image.put(key("cpu.integer"), state_);
    image.put(key("cpu.fpu"), fpu_);
    Cp0Registers cp0 = cp0_;
    cp0.random = random();
    image.put(key("cpu.cp0"), cp0);
    image.put(key("cpu.tlb"), tlb_.entries());
    image.put(key("cpu.cycles"), cycles_);
    image.put(key("cpu.external_interrupts"), external_interrupts_);
    image.put(key("cpu.timer_interrupt"), timer_interrupt_);
    image.put(key("cpu.watch_pending"), watch_pending_);
    caches_.save_state(image, prefix);
}

void Cpu::load_state(const StateImage& image, std::string_view prefix) {
    const auto key = [prefix](std::string_view name) { return std::string{prefix}.append(name); };
    image.get(key("cpu.integer"), state_);
    image.get(key("cpu.fpu"), fpu_);
    image.get(key("cpu.cp0"), cp0_);
    std::array<TlbEntry, Tlb::entry_count> entries{};
    if (image.get(key("cpu.tlb"), entries)) {
        tlb_.restore(entries);
    }
    image.get(key("cpu.cycles"), cycles_);
    image.get(key("cpu.external_interrupts"), external_interrupts_);
    image.get(key("cpu.timer_interrupt"), timer_interrupt_);
    image.get(key("cpu.watch_pending"), watch_pending_);
    caches_.load_state(image, prefix);
    invalidate_host_pages();
    set_random(cp0_.random);
    schedule_timer();
    update_derived_state();
}

} // namespace ultraviolent::mips
