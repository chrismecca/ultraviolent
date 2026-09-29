#include <ultraviolent/arch/mips/cpu.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <bit>
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

Cpu::Cpu(AddressSpace& bus, Tracer& tracer, const CpuConfig& config)
    : bus_{bus}, tracer_{tracer}, config_{config} {
    invariant(((config.config & config_big_endian) != 0) == (bus.byte_order() == ByteOrder::big),
              "Config.BE must match the system bus byte order");
    reset(ResetKind::power_on);
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
    cp0_.config = config_.config;
    set_random(static_cast<std::uint32_t>(Tlb::entry_count - 1));
    cp0_.count_offset = static_cast<std::uint32_t>(0 - (cycles_ >> 1));
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
    // The next even cycle after this one at which (cycle / 2 + count_offset) mod 2^32, Count,
    // equals Compare.
    const std::uint64_t match = 2 * std::uint64_t{cp0_.compare - cp0_.count_offset};
    timer_cycle_ = (cycles_ & ~(timer_period - 1)) + match;
    if (timer_cycle_ <= cycles_) {
        timer_cycle_ += timer_period;
    }
}

std::expected<Translation, Exception> Cpu::translate(std::uint64_t address, AccessKind kind) {
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
    left_host_path_ = true;
    auto value = bus_.read(address, width);
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
    left_host_path_ = true;
    auto result = bus_.write(address, width, value);
    check_host_pages();
    if (!result) {
        // Only reads can take bus errors on the R10000 (UM 17 "Bus Error Exception" lists
        // read requests only). A refused write is lost; report it for diagnosis.
        tracer_.log(TraceCategory::memory, "write fault {:#x} width {} value {:#x} fault {}",
                    address.value, byte_count(width), value, std::to_underlying(result.error()));
    }
    return {};
}

std::expected<std::uint64_t, Exception> Cpu::load_slow(std::uint64_t address, AccessWidth width) {
    if (address % byte_count(width) != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_load, .bad_address = address});
    }
    auto translation = translate(address, AccessKind::load);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    auto value = read(*translation, width, AccessKind::load);
    if (value) {
        remember_page(address, *translation, AccessKind::load);
    }
    return value;
}

std::expected<void, Exception> Cpu::store_slow(std::uint64_t address, AccessWidth width,
                                               std::uint64_t value) {
    if (address % byte_count(width) != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_store, .bad_address = address});
    }
    auto translation = translate(address, AccessKind::store);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    auto result = write(*translation, width, value);
    if (result) {
        remember_page(address, *translation, AccessKind::store);
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
        if (entry.page == page && entry.context == translation_context()) {
            return CodePage{entry.bytes, bus_.byte_order()};
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

void Cpu::forget_tlb_pages(std::uint64_t entries) {
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
    if (kind == AccessKind::store) {
        const auto bytes = bus_.writable_memory_bytes(frame, page_size);
        if (bytes.size() == page_size) {
            data_pages_[host_page_slot(address)] = {page, bytes.data(), bytes.data(),
                                                    translation_context(), translation.tlb_index};
        }
        return;
    }
    const auto bytes = bus_.memory_bytes(frame, page_size);
    if (bytes.size() == page_size) {
        (kind == AccessKind::fetch ? code_pages_ : data_pages_)[host_page_slot(address)] = {
            page, bytes.data(), nullptr, translation_context(), translation.tlb_index};
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
        return config_.processor_id;
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
        cp0_.count_offset = low - static_cast<std::uint32_t>(cycles_ >> 1);
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
    if ((cp0_.status & (status::exl | status::erl)) != 0) {
        return false;
    }
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
    const auto ch =
        caches_.execute(operation, address, translation->address.value & physical_bits, registers);
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

void Cpu::save_state(StateImage& image) const {
    image.put("cpu.integer", state_);
    image.put("cpu.fpu", fpu_);
    Cp0Registers cp0 = cp0_;
    cp0.random = random();
    image.put("cpu.cp0", cp0);
    image.put("cpu.tlb", tlb_.entries());
    image.put("cpu.cycles", cycles_);
    image.put("cpu.external_interrupts", external_interrupts_);
    image.put("cpu.timer_interrupt", timer_interrupt_);
    image.put("cpu.watch_pending", watch_pending_);
    caches_.save_state(image);
}

void Cpu::load_state(const StateImage& image) {
    image.get("cpu.integer", state_);
    image.get("cpu.fpu", fpu_);
    image.get("cpu.cp0", cp0_);
    std::array<TlbEntry, Tlb::entry_count> entries{};
    if (image.get("cpu.tlb", entries)) {
        tlb_.restore(entries);
    }
    image.get("cpu.cycles", cycles_);
    image.get("cpu.external_interrupts", external_interrupts_);
    image.get("cpu.timer_interrupt", timer_interrupt_);
    image.get("cpu.watch_pending", watch_pending_);
    caches_.load_state(image);
    invalidate_host_pages();
    set_random(cp0_.random);
    schedule_timer();
    update_derived_state();
}

} // namespace ultraviolent::mips
