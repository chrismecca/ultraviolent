#pragma once

#include <ultraviolent/arch/mips/cache_arrays.hpp>
#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/tlb.hpp>
#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace ultraviolent::mips {

// Reset-time identity of the processor, supplied by the machine.
struct CpuConfig {
    // PRId: implementation number 0x09 in bits 15:8, revision in bits 7:0 (UM 14.13).
    std::uint32_t processor_id{0x0900};
    // Config register as latched from the reset mode bits (UM 14.14). Bit 15 (BE) must match
    // the byte order of the system bus.
    std::uint32_t config{config_big_endian};
    // FPU Implementation and Revision register, CP1 control register 0 (UM 15.4).
    std::uint32_t fpu_id{0x0900};
};

enum class OperatingMode : std::uint8_t {
    kernel,
    supervisor,
    user,
};

enum class AccessKind : std::uint8_t {
    fetch,
    load,
    store,
};

// A synchronous or asynchronous exception, before it is taken.
struct Exception {
    ExceptionCode code{};
    // Faulting virtual address for address-error and TLB exceptions.
    std::uint64_t bad_address{};
    // TLB refill (no matching entry): uses the refill vectors when EXL is clear.
    bool tlb_refill{};
    // Refill through the 64-bit (XTLB) vector (UM 17.3).
    bool extended_refill{};
    // Cause.CE for coprocessor-unusable exceptions.
    std::uint8_t coprocessor{};
};

// Encoding of a system-interface address in a PhysicalAddress (ADR-019). The machine decodes
// all three parts: an Origin Hub sends uncached attribute 0 to its special spaces and cached
// references to memory, even at the same physical address.
namespace system_address {
// Physical address, bits 39:0.
inline constexpr std::uint64_t physical_mask = (std::uint64_t{1} << 40) - 1;
// Set for uncached (double/single/partial-word) requests.
inline constexpr std::uint64_t uncached = std::uint64_t{1} << 59;
// Uncached attribute, bits 58:57 (UM 6.23).
inline constexpr unsigned attribute_shift = 57;
// Base of the system addresses of uncached references with `attribute`.
constexpr std::uint64_t uncached_window(unsigned attribute) {
    return uncached | (std::uint64_t{attribute & 3} << attribute_shift);
}
} // namespace system_address

// The unsigned integer type of an access width.
template <AccessWidth W> struct UnsignedOfWidth;
template <> struct UnsignedOfWidth<AccessWidth::bits8> {
    using type = std::uint8_t;
};
template <> struct UnsignedOfWidth<AccessWidth::bits16> {
    using type = std::uint16_t;
};
template <> struct UnsignedOfWidth<AccessWidth::bits32> {
    using type = std::uint32_t;
};
template <> struct UnsignedOfWidth<AccessWidth::bits64> {
    using type = std::uint64_t;
};
template <AccessWidth W> using UnsignedOf = typename UnsignedOfWidth<W>::type;

// Result of address translation.
struct Translation {
    // System address as driven on the system interface; see system_address.
    PhysicalAddress address;
    bool uncached{};
    // The TLB entry that mapped the address, or -1 for an unmapped segment.
    std::int8_t tlb_index{-1};
};

// Integer architectural state that execution engines read and write directly.
struct IntegerState {
    std::array<std::uint64_t, 32> gpr{};
    std::uint64_t hi{};
    std::uint64_t lo{};
    // Address of the next instruction to execute.
    std::uint64_t pc{};
    // Address of the instruction after it: pc + 4, or a branch target when pc is a delay slot.
    std::uint64_t next_pc{};
    // The instruction at pc is in a branch delay slot.
    bool delay_slot{};
    bool ll_bit{};
    friend bool operator==(const IntegerState&, const IntegerState&) = default;
};

// Floating-point architectural state (UM 15.3, 15.4). Register numbering follows Status.FR:
// with FR=1 there are 32 64-bit registers; with FR=0 the 32 logical 32-bit registers are the
// halves of the even-numbered physical registers.
struct FpuState {
    std::array<std::uint64_t, 32> fpr{};
    // FCSR, CP1 control register 31.
    std::uint32_t fcsr{};
    friend bool operator==(const FpuState&, const FpuState&) = default;
};

// R10000 architectural CPU (ARCHITECTURE "The CPU is not the interpreter").
//
// Owns every guest-visible register and implements the semantics that all execution engines
// share: operating modes, address translation, memory access, CP0, the TLB, exceptions, and
// interrupts. It does not decode or execute instructions.
//
// Time: one step of an execution engine (one instruction, or one exception taken instead of
// an instruction) is one processor cycle. Count advances every other cycle (UM 14.8).
class Cpu final : public InterruptSink {
  public:
    static constexpr std::uint64_t reset_vector = 0xffff'ffff'bfc0'0000;
    // External interrupt inputs 0-4 set Cause.IP[2]-IP[6].
    static constexpr std::uint32_t external_interrupt_count = 5;

    Cpu(AddressSpace& bus, Tracer& tracer, const CpuConfig& config);
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    // power_on and cold take the Cold Reset exception; warm takes the Soft Reset exception.
    void reset(ResetKind kind);
    void nonmaskable_interrupt();

    void set_interrupt_level(std::uint32_t input, bool asserted) override;

    [[nodiscard]] IntegerState& state() {
        return state_;
    }
    [[nodiscard]] const IntegerState& state() const {
        return state_;
    }
    [[nodiscard]] FpuState& fpu() {
        return fpu_;
    }
    [[nodiscard]] const FpuState& fpu() const {
        return fpu_;
    }
    [[nodiscard]] std::uint32_t fpu_id() const {
        return config_.fpu_id;
    }
    // Status.FR: 32 64-bit floating-point registers.
    [[nodiscard]] bool fpu_64bit_registers() const {
        return (cp0_.status & status::fr) != 0;
    }
    void set_gpr(unsigned index, std::uint64_t value) {
        if (index != 0) {
            state_.gpr[index] = value;
        }
    }

    // Processor cycles since construction.
    [[nodiscard]] std::uint64_t cycles() const {
        return cycles_;
    }

    // Modes and permissions (UM 16.1).
    [[nodiscard]] OperatingMode mode() const {
        if ((cp0_.status & (status::exl | status::erl)) != 0) {
            return OperatingMode::kernel;
        }
        switch ((cp0_.status & status::ksu_mask) >> status::ksu_shift) {
        case 0:
            return OperatingMode::kernel;
        case 1:
            return OperatingMode::supervisor;
        default:
            // KSU = 3 is undefined; the R10000 implements it as User mode (UM 14.10).
            return OperatingMode::user;
        }
    }
    [[nodiscard]] bool allows_64bit_operations() const {
        // UM 16.1 Table 16-1; 64-bit operations are always valid in Kernel mode (UM 17
        // "Reserved Instruction Exception").
        switch (mode()) {
        case OperatingMode::kernel:
            return true;
        case OperatingMode::supervisor:
            return (cp0_.status & status::sx) != 0;
        case OperatingMode::user:
            return (cp0_.status & status::ux) != 0;
        }
        return false;
    }
    [[nodiscard]] bool allows_mips4() const {
        // XX matters only in User mode (UM 14.10).
        return mode() != OperatingMode::user || (cp0_.status & status::xx) != 0;
    }
    [[nodiscard]] bool coprocessor_usable(unsigned unit) const {
        invariant(unit < 3, "the R10000 has coprocessors 0-2");
        if (unit == 0 && mode() == OperatingMode::kernel) {
            return true;
        }
        return (cp0_.status & (status::cu0 << unit)) != 0;
    }
    // Effective byte order of data and instruction references in the current mode.
    [[nodiscard]] bool big_endian() const {
        const bool memory_big_endian = (cp0_.config & config_big_endian) != 0;
        // RE reverses the byte order in User mode only (UM 14.10).
        const bool reversed = mode() == OperatingMode::user && (cp0_.status & status::re) != 0;
        return memory_big_endian != reversed;
    }

    // Memory references. Alignment is the caller's concern for translate/read/write; load,
    // store, and fetch check natural alignment first.
    std::expected<Translation, Exception> translate(std::uint64_t address, AccessKind kind);
    std::expected<std::uint64_t, Exception> read(const Translation& translation, AccessWidth width,
                                                 AccessKind kind);
    std::expected<void, Exception> write(const Translation& translation, AccessWidth width,
                                         std::uint64_t value);
    std::expected<std::uint64_t, Exception> load(std::uint64_t address, AccessWidth width) {
        const std::size_t size = byte_count(width);
        if (const HostPage* page = data_page(address); page != nullptr && address % size == 0)
            [[likely]] {
            return load_unsigned(std::span{page->bytes + (address & page_offset_mask), size},
                                 bus_.byte_order());
        }
        return load_slow(address, width);
    }
    std::expected<void, Exception> store(std::uint64_t address, AccessWidth width,
                                         std::uint64_t value) {
        const std::size_t size = byte_count(width);
        if (const HostPage* page = data_page(address);
            page != nullptr && page->writable_bytes != nullptr && address % size == 0) [[likely]] {
            store_unsigned(std::span{page->writable_bytes + (address & page_offset_mask), size},
                           value, bus_.byte_order());
            return {};
        }
        return store_slow(address, width, value);
    }
    // load and store for a width known at compile time: the same semantics, with the fast
    // path reduced to one host access (execution engines use these for the common opcodes).
    template <AccessWidth W>
    std::expected<std::uint64_t, Exception> load_as(std::uint64_t address) {
        using Word = UnsignedOf<W>;
        if (const HostPage* page = data_page(address);
            page != nullptr && address % sizeof(Word) == 0) [[likely]] {
            return detail::load_word<Word>(page->bytes + (address & page_offset_mask),
                                           bus_.byte_order());
        }
        return load_slow(address, W);
    }
    template <AccessWidth W>
    std::expected<void, Exception> store_as(std::uint64_t address, std::uint64_t value) {
        using Word = UnsignedOf<W>;
        if (const HostPage* page = data_page(address);
            page != nullptr && page->writable_bytes != nullptr && address % sizeof(Word) == 0)
            [[likely]] {
            detail::store_word(page->writable_bytes + (address & page_offset_mask),
                               static_cast<Word>(value), bus_.byte_order());
            return {};
        }
        return store_slow(address, W, value);
    }
    std::expected<std::uint32_t, Exception> fetch(std::uint64_t address) {
        const HostPage& page = code_pages_[host_page_slot(address)];
        if (page.page == (address & ~page_offset_mask) && address % 4 == 0 &&
            page.context == translation_context()) [[likely]] {
#ifndef NDEBUG
            invariant(host_pages_current(), "stale host page");
#endif
            return detail::load_word<std::uint32_t>(page.bytes + (address & page_offset_mask),
                                                    bus_.byte_order());
        }
        return fetch_slow(address);
    }

    // Where instruction fetches at `address` would take the host fast path now: the host
    // bytes of its 4 KiB virtual page, in the bus byte order, for building decoded blocks
    // (IR.adoc "Blocks"). Empty when a fetch there would fault or go through the bus.
    // Translates and consults the memory map only: no bus access, no architectural effect.
    struct CodePage {
        const std::byte* bytes;
        ByteOrder order;
        // The memory block and offset of the 4 KiB frame (code-frame tracking).
        MemoryBlock* block;
        std::uint64_t frame;
        // The TLB entry that translated it, or -1 for unmapped segments.
        std::int8_t tlb_index;
    };
    [[nodiscard]] std::optional<CodePage> code_page(std::uint64_t address);
    // Marks the page's frame as holding decoded code and drops any cached writable host
    // page for it, so every later store to the frame goes through the bus, where it advances
    // the frame's generation (IR.adoc "Keeping blocks valid").
    void claim_code_frame(const CodePage& page);

    // Validity inputs of decoded blocks (IR.adoc "Keeping blocks valid"): the translation
    // context (mode bits and ASID); a generation per TLB entry, advanced when the entry is
    // written or invalidated; and an epoch advanced whenever translation or the bus mappings
    // change in a way the others do not capture (Config, reset, snapshot loads, remapping).
    [[nodiscard]] std::uint64_t code_context() const {
        return translation_context();
    }
    [[nodiscard]] std::uint64_t tlb_generation(std::size_t index) const {
        return tlb_generations_[index];
    }
    [[nodiscard]] std::uint64_t code_epoch() const {
        return code_epoch_;
    }

    // What the last access that reached the bus was (IR.adoc "Slow bus paths are barriers").
    enum class BusAccess : std::uint8_t {
        // Cached-attribute access to memory by a load or store that missed the host page
        // cache: transparent, a fill.
        memory_fill,
        // An access to a device (MMIO) or to unmapped space.
        device,
        // An uncached access to memory.
        uncached_memory,
        // A store that wrote a frame holding decoded code.
        code_store,
        // Anything else that reaches the bus: partial-word and conditional stores, LWL/LWR,
        // accesses while a watchpoint is armed.
        other,
    };
    static constexpr std::size_t bus_access_count = 5;
    [[nodiscard]] BusAccess last_bus_access() const {
        return last_bus_access_;
    }
    // Stores that went through the bus because their frame held decoded code.
    [[nodiscard]] std::uint64_t code_frame_stores() const {
        return code_frame_stores_;
    }

    // Set by every access that reaches the bus (read, write): the access left the host fast
    // path and may have synchronized devices. Execution engines clear it before an
    // instruction and treat it as a barrier after (IR.adoc "Slow bus paths are barriers").
    [[nodiscard]] bool left_host_path() const {
        return left_host_path_;
    }
    void clear_left_host_path() {
        left_host_path_ = false;
    }

    // Called by execution engines before running instructions: the machine may have changed
    // the bus mappings since the last run.
    void synchronize_host_pages() {
        check_host_pages();
    }

    // CP0 moves with the R10000 width rules (UM 14.26, Table 14-26).
    [[nodiscard]] std::uint64_t mfc0(unsigned reg) const;
    [[nodiscard]] std::uint64_t dmfc0(unsigned reg) const;
    void mtc0(unsigned reg, std::uint64_t value);
    void dmtc0(unsigned reg, std::uint64_t value);

    // CACHE (UM chapter 10) on `address`, after the caller has checked that CP0 is usable.
    // Translation exceptions are those of a load.
    std::expected<void, Exception> cache(unsigned operation, std::uint64_t address);

    // TLB instructions (UM 14.34-14.37).
    void tlb_probe();
    void tlb_read();
    void tlb_write_indexed();
    void tlb_write_random();
    [[nodiscard]] const Tlb& tlb() const {
        return tlb_;
    }

    // ERET (UM 14.30): leaves the error or exception level and returns the resume address.
    std::uint64_t exception_return();

    // Takes `exception` for the instruction at state().pc.
    void take_exception(const Exception& exception);

    // At an instruction boundary: takes a pending interrupt or delayed watch exception.
    // Returns true when one was taken.
    bool service_pending_exceptions() {
#ifndef NDEBUG
        invariant(exception_pending() == interrupt_check_, "stale interrupt check");
#endif
        if (!interrupt_check_) [[likely]] {
            return false;
        }
        return take_pending_exception();
    }

    // Accounts one processor cycle; `retired` when an instruction completed in it.
    void end_cycle(bool retired) {
        ++cycles_;
        if (!retired) {
            ++unretired_cycles_;
        }
        // Count advances on every other cycle; IP[7] is set when it becomes equal to Compare,
        // which next happens at timer_cycle_.
        if (cycles_ == timer_cycle_) [[unlikely]] {
            timer_interrupt_ = true;
            timer_cycle_ += timer_period;
            update_derived_state();
        }
    }

    // Snapshot support (StateImage).
    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    struct Cp0Registers {
        std::uint32_t index{};
        std::uint32_t random{};
        std::uint64_t entry_lo0{};
        std::uint64_t entry_lo1{};
        std::uint64_t context{};
        std::uint32_t page_mask{};
        std::uint32_t wired{};
        std::uint64_t bad_vaddr{};
        std::uint32_t count_offset{};
        std::uint64_t entry_hi{};
        std::uint32_t compare{};
        std::uint32_t status{};
        // BD, CE, ExcCode, and the software interrupt bits. IP[7:2] are computed.
        std::uint32_t cause{};
        std::uint64_t epc{};
        std::uint32_t config{};
        std::uint32_t ll_addr{};
        std::uint32_t watch_lo{};
        std::uint32_t watch_hi{};
        std::uint64_t xcontext{};
        std::uint32_t frame_mask{};
        std::uint64_t diagnostic{};
        std::uint32_t ecc{};
        std::uint32_t tag_lo{};
        std::uint32_t tag_hi{};
        std::uint64_t error_epc{};
        friend bool operator==(const Cp0Registers&, const Cp0Registers&) = default;
    };

  public:
    // The architectural state, compared field by field to check one execution engine against
    // another (IR.adoc "Correctness method"). The cache arrays, large, are compared in place
    // through caches(). Host-side caches and derived values are not part of it.
    struct State {
        IntegerState integer;
        FpuState fpu;
        // Random as of now; Count through count_offset and cycles.
        Cp0Registers cp0;
        std::array<TlbEntry, Tlb::entry_count> tlb;
        std::uint64_t cycles;
        std::uint64_t retired;
        std::uint32_t external_interrupts;
        bool timer_interrupt;
        bool watch_pending;
        friend bool operator==(const State&, const State&) = default;
    };
    [[nodiscard]] State capture() const;
    [[nodiscard]] const CacheArrays& caches() const {
        return caches_;
    }

  private:
    [[nodiscard]] std::uint32_t count() const {
        return static_cast<std::uint32_t>(cycles_ >> 1) + cp0_.count_offset;
    }
    // Cycles in which an instruction graduated.
    [[nodiscard]] std::uint64_t retired() const {
        return cycles_ - unretired_cycles_;
    }
    // Random (UM 14.2), derived from the instructions retired since cp0_.random was set.
    [[nodiscard]] std::uint32_t random() const;
    // Sets cp0_.random as of now.
    void set_random(std::uint32_t value) {
        cp0_.random = value;
        random_base_ = retired();
    }
    // Count equals Compare every 2^32 Count increments, 2^33 cycles.
    static constexpr std::uint64_t timer_period = std::uint64_t{1} << 33;
    // Finds timer_cycle_ after Count, Compare, or the cycle count changed.
    void schedule_timer();
    // Whether an interrupt or delayed watch exception would be taken at the next boundary.
    [[nodiscard]] bool exception_pending() const {
        if ((cp0_.status & (status::exl | status::erl)) != 0) {
            return false;
        }
        const std::uint32_t pending = (cause_value() >> cause::ip_shift) & 0xff;
        const std::uint32_t enabled = (cp0_.status >> status::im_shift) & 0xff;
        return watch_pending_ || ((cp0_.status & status::ie) != 0 && (pending & enabled) != 0);
    }
    // Recomputes what the per-instruction paths cache from Status, Cause, EntryHi, and the
    // interrupt inputs. Every change to those calls it.
    void update_derived_state() {
        interrupt_check_ = exception_pending();
        context_ = compute_translation_context();
    }
    [[nodiscard]] std::uint32_t cause_value() const {
        std::uint32_t pending = external_interrupts_;
        if (timer_interrupt_) {
            pending |= 1u << 7;
        }
        return cp0_.cause | (pending << cause::ip_shift);
    }
    bool take_pending_exception();

    // Slow paths of load, store, and fetch: full translation and bus decode.
    std::expected<std::uint64_t, Exception> load_slow(std::uint64_t address, AccessWidth width);
    std::expected<void, Exception> store_slow(std::uint64_t address, AccessWidth width,
                                              std::uint64_t value);
    std::expected<std::uint32_t, Exception> fetch_slow(std::uint64_t address);

    // Data fast path: host bytes of recently used 4 KiB data pages, by virtual page.
    struct HostPage;
    [[nodiscard]] const HostPage* data_page(std::uint64_t address) const {
        // No data page is filled while a watchpoint is armed (remember_page), and arming one
        // drops them: watched accesses need the full path.
        const HostPage& page = data_pages_[host_page_slot(address)];
        if (page.page != (address & ~page_offset_mask) || page.context != translation_context()) {
            return nullptr;
        }
#ifndef NDEBUG
        invariant(host_pages_current() && (cp0_.watch_lo & 3) == 0, "stale host page");
#endif
        return &page;
    }
    // Everything besides the TLB that selects a translation: the mode and addressing bits of
    // Status (EXL, ERL, KSU, UX, SX, KX, RE) and the ASID.
    [[nodiscard]] std::uint64_t translation_context() const {
#ifndef NDEBUG
        invariant(context_ == compute_translation_context(), "stale translation context");
#endif
        return context_;
    }
    [[nodiscard]] std::uint64_t compute_translation_context() const {
        constexpr std::uint32_t mode_bits = status::exl | status::erl | status::ksu_mask |
                                            status::ux | status::sx | status::kx | status::re;
        return (cp0_.status & mode_bits) | (cp0_.entry_hi & entry_hi::asid_mask) << 32;
    }
    // Drops the host pages if translation or the bus mappings changed since they were filled.
    // The bus changes only between runs or during a bus access, so this runs at the start of
    // a run (synchronize_host_pages) and after every bus access (read, write).
    void check_host_pages() {
        if (!host_pages_current()) {
            ++code_epoch_;
            code_pages_.fill({});
            data_pages_.fill({});
            host_pages_translation_generation_ = translation_generation_;
            host_pages_bus_generation_ = bus_.generation();
        }
    }
    // Translation changed in a way the host pages cannot track.
    void invalidate_host_pages() {
        ++translation_generation_;
        check_host_pages();
    }
    // Drops cached pages that TLB entries in `entries` (bit n: entry n) translated.
    void forget_tlb_pages(std::uint64_t entries);
    // What an access to `address` that reaches the bus is (BusAccess), before it is made.
    [[nodiscard]] BusAccess classify_bus_access(PhysicalAddress address, AccessWidth width,
                                                bool uncached) const;
    // Records the page of a successful access to `address` (kind fetch, load, or store).
    void remember_page(std::uint64_t address, const Translation& translation, AccessKind kind);
    [[nodiscard]] std::uint64_t read_cp0(unsigned reg) const;
    void write_cp0(unsigned reg, std::uint64_t value);
    [[nodiscard]] TlbEntry tlb_entry_from_registers() const;
    void write_tlb(std::size_t index);

    std::expected<Translation, Exception> translate_mapped(std::uint64_t address, AccessKind kind);
    std::expected<Translation, Exception> translate_compatibility(std::uint64_t address,
                                                                  AccessKind kind);
    std::expected<void, Exception> check_watch(const Translation& translation, AccessKind kind);
    [[nodiscard]] PhysicalAddress bus_address(const Translation& translation,
                                              AccessWidth width) const;
    void enter_reset_vector();

    AddressSpace& bus_;
    Tracer& tracer_;
    CpuConfig config_;
    IntegerState state_;
    FpuState fpu_;
    Cp0Registers cp0_;
    Tlb tlb_;
    CacheArrays caches_{config_.config};
    std::uint64_t cycles_{};
    // Latched external interrupt requests, Cause.IP[6:2].
    std::uint32_t external_interrupts_{};
    bool timer_interrupt_{};
    bool watch_pending_{};
    bool left_host_path_{};
    BusAccess last_bus_access_{BusAccess::other};
    // Set while load_slow and store_slow make the access that fills the host page cache.
    bool filling_{};
    std::uint64_t code_frame_stores_{};
    std::array<std::uint64_t, Tlb::entry_count> tlb_generations_{};
    std::uint64_t code_epoch_{};

    // Per-instruction work kept out of the instruction loop; transparent (the same values
    // the direct computation gives, which debug builds check). Measured: the interrupt
    // check, Count/Compare, Random, and the translation context were about 20% of IRIX run
    // time (perf, 2026-09-28).
    std::uint64_t unretired_cycles_{};
    std::uint64_t random_base_{};
    std::uint64_t timer_cycle_{};
    bool interrupt_check_{};
    std::uint64_t context_{};

    // Host page caches: the host bytes of recently used 4 KiB virtual pages that translate
    // to plain memory in the bus byte order, one set for instruction fetch and one for loads
    // and stores, direct-mapped by virtual page. Data loads hit any entry; stores only
    // entries a store filled from writable memory, since a store's translation can fault
    // (TLB Modified) where a load's does not; no data entry is used while a watchpoint is
    // armed. Transparent: every hit returns or changes exactly what the full path would. An
    // entry is used only in the translation context it was filled in (translation_context());
    // a TLB write drops the entries its target and the entries it invalidated had mapped;
    // everything is dropped when translation_generation_ or the bus generation changes, as
    // soon as it changes, so a hit needs no generation check.
    // Measured: translation and bus decode of fetches, loads, and stores were about 45% of
    // IP27 PROM run time (perf, 2026-09-26 and 2026-09-27); under IRIX, flushing on every
    // exception entry and return left most accesses on the full path (2026-09-27).
    static constexpr std::uint64_t page_offset_mask = 0xfff;
    struct HostPage {
        std::uint64_t page{~std::uint64_t{0}};
        const std::byte* bytes{};
        std::byte* writable_bytes{};
        std::uint64_t context{};
        std::int8_t tlb_index{-1};
        // The memory block and offset of the frame (code-frame tracking).
        MemoryBlock* block{};
        std::uint64_t frame{};
    };
    static constexpr std::size_t host_page_count = 64;
    [[nodiscard]] bool host_pages_current() const {
        return host_pages_translation_generation_ == translation_generation_ &&
               host_pages_bus_generation_ == bus_.generation();
    }
    [[nodiscard]] static std::size_t host_page_slot(std::uint64_t address) {
        return (address >> 12) % host_page_count;
    }
    std::array<HostPage, host_page_count> code_pages_{};
    std::array<HostPage, host_page_count> data_pages_{};
    std::uint64_t host_pages_translation_generation_{};
    std::uint64_t host_pages_bus_generation_{};
    // Changes whenever address translation may have changed in a way translation_context()
    // and TLB-entry tracking do not capture: Config, reset, snapshot loads.
    std::uint64_t translation_generation_{1};
};

} // namespace ultraviolent::mips
