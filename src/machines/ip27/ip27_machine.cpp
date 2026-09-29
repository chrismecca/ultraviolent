#include <ultraviolent/machines/ip27/ip27_machine.hpp>

#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <vector>

namespace ultraviolent::ip27 {

namespace {

// slate's CD-ROM drive is ID 6 on the BaseIO's first SCSI bus (OBS-SLATE-0001).
constexpr unsigned cdrom_id = 6;
// slate's system disk is ID 1 on the same bus; sash installs to dksc(0,1,*).
constexpr unsigned system_disk_id = 1;

// IP27.adoc "Node address space".
constexpr std::uint64_t ualias_size = 0x1000'0000;
constexpr std::uint64_t lboot_base = 0x1000'0000;
// The reset vector's physical address: LBOOT offset 0x0fc00000 (IP27.adoc "Boot PROM").
constexpr std::uint64_t reset_flash_base = 0x1fc0'0000;
constexpr std::uint64_t rboot_base = 0x3000'0000;
// The flash appears at the base of LBOOT, at the reset vector, and at the base of RBOOT.
constexpr std::uint64_t flash_windows[] = {lboot_base, reset_flash_base, rboot_base};
constexpr std::uint64_t flash_command_span = std::uint64_t{flash_size} << 3;
constexpr unsigned hspec_attribute = 0;
constexpr unsigned io_attribute = 1;
constexpr unsigned uncac_attribute = 3;
// IALIAS: the local Hub's registers at the start of widget 1's 16 MB I/O window.
constexpr std::uint64_t hub_widget = 1;
constexpr std::uint64_t small_window_shift = 24;
constexpr unsigned big_window_count = 7;
constexpr std::uint64_t big_window_shift = 29;

std::uint64_t fnv1a(std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325;
    for (const std::byte b : bytes) {
        hash = (hash ^ std::to_integer<std::uint64_t>(b)) * 0x0000'0100'0000'01b3;
    }
    return hash;
}

void must_map(std::expected<void, MapError> result, const char* what) {
    invariant(result.has_value(), what);
}

} // namespace

std::uint32_t reset_config_word(const Ip27Config& config, std::span<const std::byte> flash) {
    // Config bits 31:29 (IC) and 28:26 (DC) are hardwired to 32 KB; bits 24:0 are the mode
    // bits latched at reset (UM 14.14, 8.5).
    constexpr std::uint32_t hardwired_caches = (3u << 29) | (3u << 26);
    if (const auto mode = prom_mode_bits(flash)) {
        return hardwired_caches | (*mode & 0x01ff'ffffu);
    }
    return config.cpu.config;
}

std::optional<std::array<std::uint64_t, memory_banks>>
memory_bank_sizes(std::uint64_t memory_bytes) {
    // MD_SIZE_8MB is the smallest bank size code (LINUX sn0/hubmd.h).
    constexpr std::uint64_t minimum_bank = std::uint64_t{8} << 20;
    std::array<std::uint64_t, memory_banks> banks{};
    std::uint64_t remaining = memory_bytes;
    for (std::uint64_t& bank : banks) {
        bank = std::min(remaining, bank_window);
        remaining -= bank;
    }
    const std::uint64_t last = memory_bytes % bank_window;
    if (memory_bytes == 0 || remaining != 0 ||
        (last != 0 && (!std::has_single_bit(last) || last < minimum_bank))) {
        return std::nullopt;
    }
    return banks;
}

std::expected<void, std::string> validate(const Ip27Config& config,
                                          std::span<const std::byte> prom) {
    if (prom.empty() || prom.size() > prom_code_size) {
        return std::unexpected("PROM image does not fit the flash code area");
    }
    if (!memory_bank_sizes(config.memory_bytes)) {
        return std::unexpected(std::format(
            "memory must fill 512 MiB banks, then at most one bank of a power of two from 8 "
            "MiB, up to 4096 MiB in all (got {} bytes)",
            config.memory_bytes));
    }
    if (config.node_slot < 1 || config.node_slot > 4) {
        return std::unexpected("node slot must be n1 to n4");
    }
    if (config.cpu_clock.hertz == 0 || config.cpu_clock.hertz > maximum_frequency_hertz) {
        return std::unexpected("processor clock out of range");
    }
    if ((reset_config_word(config, prom) & mips::config_big_endian) == 0) {
        return std::unexpected("IP27 memory is big-endian: the reset mode bits must select it");
    }
    return {};
}

namespace {

std::array<std::uint64_t, memory_banks> checked_bank_sizes(const Ip27Config& config) {
    const auto banks = memory_bank_sizes(config.memory_bytes);
    invariant(banks.has_value(), "Ip27Machine built from an invalid configuration");
    return banks.value_or(std::array<std::uint64_t, memory_banks>{});
}

mips::CpuConfig cpu_config(const Ip27Config& config, std::span<const std::byte> prom) {
    mips::CpuConfig cpu = config.cpu;
    cpu.config = reset_config_word(config, prom);
    return cpu;
}

} // namespace

std::array<std::byte, devices::I2cEeprom::size> initial_controller_nvram(const Ip27Config& config) {
    std::array<std::byte, devices::I2cEeprom::size> nvram{};
    std::ranges::fill(nvram, std::byte{0xff});
    // Bytes 0x700-0x70f as slate's controller holds them (real hardware, IP27.adoc "System
    // controller"): the magic '7' the PROM checks, the password "none", and the module number
    // at 0x708, which the PROM reads to vote for its module ID (observed).
    constexpr std::array<std::uint8_t, 16> block = {'7', 'n', 'o', 'n',  'e', 0, 0, 0,
                                                    0,   0,   0,   0x78, 0,   0, 0, 0};
    for (std::size_t i = 0; i < block.size(); ++i) {
        nvram[0x700 + i] = std::byte{block[i]};
    }
    nvram[0x708] = std::byte{config.module_number};
    return nvram;
}

std::array<std::uint8_t, devices::Nic::memory_size>
ethernet_nic_memory(const std::array<std::uint8_t, 6>& address) {
    // observed (IRIX 6.5.30 nic_eaddr, by disassembly): Read Memory from address 0 returns the
    // CRC-8 of the command and address (0x8d), then a record: length 10, ten bytes, and a
    // CRC-16 whose residue over the 13 bytes is 0xb001. Address byte j is record byte 10 - j.
    // Linux ioc3-eth.c agrees (nic[13..0] read in reverse, the address from nic[2..7]).
    // hypothesis: record bytes 1-4 are zero and the rest of memory is unprogrammed.
    std::array<std::uint8_t, devices::Nic::memory_size> memory{};
    memory.fill(0xff);
    constexpr std::size_t length = 10;
    memory[0] = length;
    std::fill_n(memory.begin() + 1, length, std::uint8_t{0});
    for (std::size_t j = 0; j < address.size(); ++j) {
        memory[length - j] = address[j];
    }
    const auto crc = static_cast<std::uint16_t>(~devices::dallas_crc16(memory.data(), length + 1));
    memory[length + 1] = static_cast<std::uint8_t>(crc);
    memory[length + 2] = static_cast<std::uint8_t>(crc >> 8);
    return memory;
}

std::array<std::uint8_t, devices::Nic::memory_size> board_nic_memory(const BoardIdentity& board) {
    // observed (IRIX 6.5.30 nic_mfg_next, nic_read_mfg, nic_read_one_page, by disassembly): a
    // DS2502 (family 0x09) holds the record in pages 0 ("A") and 1 ("B"), read unless
    // redirected; each page ends in a CRC-16 over its 32 bytes that must leave the residue
    // 0xb001. IRIX parses the record when A[0] is 1. Text fields keep printable characters
    // and skip the rest: A[1..10] serial, A[11..29] then B[0..5] part, B[6..9] revision,
    // B[16..29] name; hex fields B[10] group, B[11..14] capability, B[15] variety.
    // Text is padded with spaces (observed: slate's hinv shows spaces after a barcode; NUL
    // padding shows as dots). hypothesis: unused bytes are unprogrammed (0xff).
    std::array<std::uint8_t, devices::Nic::memory_size> memory{};
    memory.fill(0xff);
    const auto put = [&memory](std::size_t at, std::size_t length, std::string_view text) {
        for (std::size_t i = 0; i < length; ++i) {
            memory[at + i] = i < text.size() ? static_cast<std::uint8_t>(text[i]) : ' ';
        }
    };
    constexpr std::size_t page_b = devices::Nic::page_size;
    constexpr std::size_t part_in_a = 19;
    memory[0] = 1;
    put(1, 10, board.serial);
    put(11, part_in_a, board.part.substr(0, std::min(board.part.size(), part_in_a)));
    put(page_b, 6, board.part.size() > part_in_a ? board.part.substr(part_in_a) : "");
    put(page_b + 6, 4, board.revision);
    memory[page_b + 10] = board.group;
    for (std::size_t i = 0; i < 4; ++i) {
        memory[page_b + 11 + i] = static_cast<std::uint8_t>(board.capability >> (24 - 8 * i));
    }
    memory[page_b + 15] = board.variety;
    put(page_b + 16, 14, board.name);
    for (const std::size_t page : {std::size_t{0}, page_b}) {
        const auto crc = static_cast<std::uint16_t>(~devices::dallas_crc16(&memory[page], 30));
        memory[page + 30] = static_cast<std::uint8_t>(crc);
        memory[page + 31] = static_cast<std::uint8_t>(crc >> 8);
    }
    return memory;
}

unsigned hub_xbow_port(unsigned slot) {
    // observed: the PROM's SN0 table of Hub widget numbers by slot code: n1 9, n2 0xa, n3 0xa,
    // n4 9 (two crossbows per module).
    constexpr std::array<unsigned, 5> ports = {0, 0x9, 0xa, 0xa, 0x9};
    return ports[slot];
}

unsigned node_slot_id(unsigned slot) {
    // observed: the PROM maps SN0 slot IDs 7, 6, 5, 4 to node slots n1-n4 (its table for IDs
    // 0-7 is 0xf0, 0xf0, 0x52, 0x51, 4, 3, 2, 1, where 0xf0 is invalid).
    return (8 - slot) & 7;
}

Ip27Machine::Ip27Machine(const Ip27Config& config, std::span<const std::byte> prom)
    : config_{config}, memory_{config.memory_bytes}, flash_{flash_size},
      directory_{tracer_, checked_bank_sizes(config), config.directory},
      cpu_{bus_, tracer_, cpu_config(config, prom)} {
    hub_.connect_cpu(0, cpu_);
    invariant(validate(config, prom).has_value(),
              "Ip27Machine built from an invalid configuration");
    i2c_bus_.attach(controller_nvram_);
    hub_.connect_xtalk(xbow_);
    xbow_.attach(hub_xbow_port(config_.node_slot), hub_.widget());
    xbow_.attach(baseio_port, baseio_bridge_);
    baseio_bridge_.connect(xbow_);
    // Slate's BaseIO boards (OBS-SLATE-0004), with synthetic barcodes.
    mio_nic_.memory() = board_nic_memory(
        {.serial = "UVX001", .part = "030-0880-003", .revision = "C", .name = "MIO"});
    baseio_nic_.memory() = board_nic_memory(
        {.serial = "UVX002", .part = "030-0734-003", .revision = "A", .name = "BASEIO"});
    baseio_nic_bus_.attach(mio_nic_);
    baseio_nic_bus_.attach(baseio_nic_);
    baseio_bridge_.connect_nic(baseio_nic_bus_);
    baseio_bridge_.attach(0, scsi0_);
    baseio_bridge_.attach(1, scsi1_);
    scsi0_.connect_dma(baseio_bridge_.dma_port(0));
    scsi1_.connect_dma(baseio_bridge_.dma_port(1));
    scsi0_.connect_bus(scsi_bus0_);
    // Each slot's INTA is the Bridge interrupt pin of the same number.
    scsi0_.connect_interrupt(baseio_bridge_, 0);
    scsi1_.connect_interrupt(baseio_bridge_, 1);
    scsi_bus0_.attach(cdrom_id, cdrom_);
    scsi1_.connect_bus(scsi_bus1_);
    baseio_bridge_.attach(2, ioc3_);
    ioc3_.connect_dma(baseio_bridge_.dma_port(2));
    // The IOC3's SIO interrupts are its INTB. The BaseIO board wires it to Bridge pin 4, not
    // the generic slot ^ 4 (Linux 5.10 pci-xtalk-bridge.c bridge_setup_ip27_baseio:
    // int_mapping[2][1] = 4; observed: IRIX enables pin 4 with the IOC3's vector once the
    // board's NICs name it a BASEIO, and pin 6 while it knew only a generic widget).
    constexpr unsigned ioc3_intb_pin = 4;
    ioc3_.connect_interrupt(baseio_bridge_, ioc3_intb_pin);
    ioc3_.connect_timekeeper(timekeeper_);
    // The Ethernet interrupt is the IOC3's INTA, Bridge pin 2 (slot 2; Linux ioc3.c: the
    // Ethernet uses the device's own PCI interrupt). The PHY answers at MII address 31
    // (observed: the PROM's enet_phy_reg diagnostic addresses 31 and expects it in register
    // 25; IRIX probes addresses down to it).
    constexpr unsigned ioc3_inta_pin = 2;
    constexpr unsigned ethernet_phy_address = 31;
    ioc3_.ethernet().connect_interrupt(baseio_bridge_, ioc3_inta_pin);
    ioc3_.ethernet().connect_phy(ethernet_phy_address, ethernet_phy_);
    // IOC3 GPIO 5 is the PHY's active-low reset (ioc3.h GPPR_PHY_RESET_PIN; observed: the
    // BASEIO PROM pulses GPPR[5] 0 then 1 just before its PHY register test, and with a cable
    // attached it expects the link still down then).
    constexpr unsigned phy_reset_pin = 5;
    ioc3_.connect_gpio(phy_reset_pin, [this](bool high) { ethernet_phy_.set_reset(!high); });
    ethernet_nic_.memory() = ethernet_nic_memory(config_.ethernet_address);
    ioc3_nic_bus_.attach(ethernet_nic_);
    ioc3_.connect_nic(ioc3_nic_bus_);
    std::ranges::copy(initial_controller_nvram(config), controller_nvram_.bytes().begin());
    // Unprogrammed flash reads as 0xff.
    std::ranges::fill(flash_.bytes(), std::byte{0xff});
    std::ranges::copy(prom, flash_.bytes().begin());
    flash_hash_ = fnv1a(flash_.bytes());
    map_system_windows();
}

void Ip27Machine::map_system_windows() {
    using mips::system_address::uncached_window;
    const std::uint64_t memory = config_.memory_bytes;

    // CAC: cached requests reach memory.
    must_map(bus_.map_memory({PhysicalAddress{0}, memory}, memory_, 0, MemoryAccess::read_write),
             "CAC memory window");
    // UNCAC: uncached attribute 3 reaches memory.
    must_map(bus_.map_memory({PhysicalAddress{uncached_window(uncac_attribute)}, memory}, memory_,
                             0, MemoryAccess::read_write),
             "UNCAC memory window");
    // HSPEC UALIAS: uncached attribute 0, the first 256 MB of node space, reaches memory.
    must_map(bus_.map_memory(
                 {PhysicalAddress{uncached_window(hspec_attribute)}, std::min(memory, ualias_size)},
                 memory_, 0, MemoryAccess::read_write),
             "UALIAS window");
    // HSPEC LBOOT: the boot PROM, at the base of LBOOT (observed: the PROM reads its
    // ip27conf block there) and where the reset vector points. HSPEC RBOOT (LINUX addrs.h
    // NODE_RBOOT_BASE, "Hub's RBOOT space"): the node's boot PROM addressed through the
    // node's own HSPEC space (observed: the PROM reads the ip27conf processor clock at RBOOT
    // + 0x70). Reads come straight from the flash array while it is in read-array mode;
    // writes always reach the flash through its interface (IP27.adoc "Boot PROM").
    for (const std::uint64_t base : flash_windows) {
        must_map(
            bus_.map_memory({PhysicalAddress{uncached_window(hspec_attribute) + base}, flash_size},
                            flash_, 0, MemoryAccess::read_only, &synchronized_flash_),
            "boot PROM window");
    }
    // Command writes address the flash at offset << 3, so writing all of it takes 8 MB of
    // LBOOT (observed: sector 14's erase command goes to LBOOT + 0x700000). hypothesis: the
    // rest of that range only takes writes; RBOOT decodes the same way.
    for (const std::uint64_t base : {lboot_base, rboot_base}) {
        must_map(
            bus_.map_mmio({PhysicalAddress{uncached_window(hspec_attribute) + base + flash_size},
                           flash_command_span - flash_size},
                          synchronized_flash_commands_),
            "boot PROM command window");
    }
    // HSPEC back door: directory and protection memory at 3 GB (IP27.adoc "Memory and
    // directory").
    must_map(bus_.map_mmio({PhysicalAddress{uncached_window(hspec_attribute) +
                                            DirectoryMemory::window_offset},
                            DirectoryMemory::window_size},
                           directory_),
             "back-door directory window");
    // IO: the local Hub's registers, at IALIAS and at the remote-Hub alias for this node
    // (widget 1 window + 0x800000, LINUX REMOTE_HUB_ADDR; observed: the PROM uses it for its
    // own NASID 0).
    const std::uint64_t hub_window =
        uncached_window(io_attribute) + (hub_widget << small_window_shift);
    for (const std::uint64_t base : {hub_window, hub_window + Hub::window_size}) {
        must_map(bus_.map_mmio({PhysicalAddress{base}, Hub::window_size}, synchronized_hub_),
                 "Hub window");
    }
    // IO: every other widget's small window is PIO over the Hub's crosstalk link.
    for (unsigned widget = 0; widget < xtalk::widget_count; ++widget) {
        if (widget == hub_widget) {
            continue;
        }
        auto& window = xtalk_windows_[widget].emplace(*this, hub_.io_window(widget));
        must_map(bus_.map_mmio({PhysicalAddress{uncached_window(io_attribute) +
                                                (std::uint64_t{widget} << small_window_shift)},
                                xtalk::small_window_size},
                               window),
                 "crosstalk window");
    }
    // IO: the big windows (M mode: seven of 512 MB from IO + 512 MB, LINUX sn0/addrs.h
    // NODE_BWIN_BASE) reach crosstalk through the Hub's IIO_ITTE translation entries
    // (Hub::BigWindow). observed: IRIX programs the ITTEs, and its flash utility reaches the
    // BaseIO PROM through one.
    for (unsigned window = 0; window < big_window_count; ++window) {
        auto& target = big_windows_[window].emplace(*this, hub_.big_window(window + 1));
        must_map(bus_.map_mmio({PhysicalAddress{uncached_window(io_attribute) +
                                                (std::uint64_t{window + 1} << big_window_shift)},
                                std::uint64_t{1} << big_window_shift},
                               target),
                 "crosstalk big window");
    }
}

std::expected<std::uint64_t, AccessFault>
Ip27Machine::FlashInterface::mmio_read(std::uint64_t offset, AccessWidth width) {
    // Byte-addressed: an access returns consecutive flash bytes, the first most significant
    // (observed: the PROM's driver reads the manufacturer ID at +0 and the device ID at +1).
    std::uint64_t value = 0;
    for (std::uint64_t i = 0; i < byte_count(width); ++i) {
        value = (value << 8) | flash.read(static_cast<std::uint32_t>(offset + i));
    }
    tracer.log(TraceCategory::machine, "flash read {:#08x} width {} = {:#x}", offset,
               byte_count(width), value);
    return value;
}

std::expected<void, AccessFault> Ip27Machine::FlashInterface::mmio_write(std::uint64_t offset,
                                                                         AccessWidth /*width*/,
                                                                         std::uint64_t value) {
    // observed: the PROM's flash driver writes flash address A at LBOOT + (A << 3), with the
    // data in bits 7:0.
    tracer.log(TraceCategory::machine, "flash write {:#08x} -> address {:#07x} data {:#04x}",
               offset, offset >> 3, value & 0xff);
    flash.write(static_cast<std::uint32_t>(offset >> 3), static_cast<std::uint8_t>(value));
    return {};
}

std::expected<std::uint64_t, AccessFault>
Ip27Machine::FlashCommands::mmio_read(std::uint64_t offset, AccessWidth width) {
    interface.tracer.log(TraceCategory::machine, "flash command window read {:#x} width {}", offset,
                         byte_count(width));
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Ip27Machine::FlashCommands::mmio_write(std::uint64_t offset,
                                                                        AccessWidth width,
                                                                        std::uint64_t value) {
    return interface.mmio_write(offset + flash_size, width, value);
}

void Ip27Machine::flash_mode_changed() {
    using mips::system_address::uncached_window;
    for (const std::uint64_t base : flash_windows) {
        const PhysicalRange range{PhysicalAddress{uncached_window(hspec_attribute) + base},
                                  flash_size};
        must_map(
            flash_device_.array_mode()
                ? bus_.remap_memory(range, flash_, 0, MemoryAccess::read_only, &synchronized_flash_)
                : bus_.remap_mmio(range, synchronized_flash_),
            "boot PROM window");
    }
}

void Ip27Machine::load_baseio_flash(std::span<const std::byte> image) {
    invariant(image.size() <= baseio_flash_.bytes().size(), "the BaseIO flash holds 1 MB");
    std::ranges::fill(baseio_flash_.bytes(), std::byte{0xff});
    std::ranges::copy(image, baseio_flash_.bytes().begin());
    // observed: a 16-bit part in word mode (see the Bridge's external flash port); the PROM's
    // table accepts AMD 0x22da, the Am29LV800BT.
    baseio_flash_device_.set_word_mode(0x22da);
    baseio_bridge_.connect_flash(baseio_flash_device_);
}

void Ip27Machine::load_flash(std::span<const std::byte> contents) {
    invariant(contents.size() == flash_size, "flash contents are 1 MB");
    std::ranges::copy(contents, flash_.bytes().begin());
}

void Ip27Machine::connect_ethernet(EthernetLink* link) {
    if (link != nullptr) {
        ioc3_.ethernet().connect_link(*link);
    }
    ethernet_phy_.set_link_partner(link != nullptr);
}

std::span<const std::uint8_t> Ip27Machine::nvram_contents() {
    synchronize_time();
    return timekeeper_.contents();
}

void Ip27Machine::load_nvram(std::span<const std::uint8_t> contents, bool resume_clock) {
    timekeeper_.load_contents(contents, resume_clock);
}

StateImage Ip27Machine::save_state() {
    synchronize_time();
    StateImage image;
    image.put("machine.memory_bytes", config_.memory_bytes);
    image.put("machine.cpu_clock", config_.cpu_clock.hertz);
    image.put("machine.flash_hash", flash_hash_);
    image.put("machine.baseio_port", std::uint64_t{baseio_port});
    scheduler_.save_state(image);
    cpu_.save_state(image);
    hub_.save_state(image);
    i2c_.save_state(image);
    elsc_.save_state(image);
    flash_device_.save_state(image);
    baseio_flash_device_.save_state(image);
    xbow_.save_state(image);
    baseio_bridge_.save_state(image);
    scsi0_.save_state(image);
    cdrom_.save_state(image, "scsi0.target6");
    if (system_disk_) {
        system_disk_->save_state(image, "scsi0.target1");
    }
    scsi1_.save_state(image);
    ioc3_.save_state(image);
    timekeeper_.save_state(image);
    ethernet_phy_.save_state(image);
    controller_nvram_.save_state(image, "controller_nvram");
    directory_.save_state(image);
    image.put_sparse("memory", memory_.bytes());
    return image;
}

std::expected<void, std::string> Ip27Machine::load_state(const StateImage& image) {
    std::uint64_t memory_bytes = 0;
    std::uint64_t cpu_clock = 0;
    std::uint64_t flash_hash = 0;
    if (!image.get("machine.memory_bytes", memory_bytes) ||
        !image.get("machine.cpu_clock", cpu_clock) ||
        !image.get("machine.flash_hash", flash_hash)) {
        return std::unexpected("not an IP27 snapshot");
    }
    if (memory_bytes != config_.memory_bytes || cpu_clock != config_.cpu_clock.hertz) {
        return std::unexpected("snapshot was taken with a different machine configuration");
    }
    if (flash_hash != flash_hash_) {
        return std::unexpected("snapshot was taken with a different PROM image");
    }
    // The guest's configuration records the topology it was booted on. Snapshots from before
    // this field had the BaseIO on Crossbow port 15.
    std::uint64_t snapshot_baseio_port = 0xf;
    image.get("machine.baseio_port", snapshot_baseio_port);
    if (snapshot_baseio_port != baseio_port) {
        return std::unexpected("snapshot was taken with the BaseIO on another Crossbow port");
    }
    if (cpu_.cycles() != 0) {
        return std::unexpected("snapshots load only into a machine that has not run");
    }
    if (!image.get_sparse("memory", memory_.bytes())) {
        return std::unexpected("snapshot memory is malformed");
    }
    scheduler_.load_state(image);
    cpu_.load_state(image);
    hub_.load_state(image);
    i2c_.load_state(image);
    elsc_.load_state(image);
    flash_device_.load_state(image);
    baseio_flash_device_.load_state(image);
    xbow_.load_state(image);
    baseio_bridge_.load_state(image);
    scsi0_.load_state(image);
    cdrom_.load_state(image, "scsi0.target6");
    if (system_disk_) {
        system_disk_->load_state(image, "scsi0.target1");
    }
    scsi1_.load_state(image);
    ioc3_.load_state(image);
    timekeeper_.load_state(image);
    ethernet_phy_.load_state(image);
    controller_nvram_.load_state(image, "controller_nvram");
    directory_.load_state(image);
    update_next_event();
    return {};
}

void Ip27Machine::reset(ResetKind kind) {
    hub_.reset(kind);
    if (kind != ResetKind::warm) {
        xbow_.reset();
        baseio_bridge_.reset();
    }
    cpu_.reset(kind);
}

void Ip27Machine::attach_disk(BlockStore& store) {
    system_disk_.emplace(tracer_, "disk", store);
    scsi_bus0_.attach(system_disk_id, *system_disk_);
}

void Ip27Machine::step() {
    const mips::IntegerState& state = cpu_.state();
    for (const std::uint64_t probe : pc_probes_) {
        if (probe == state.pc) {
            tracer_.log(TraceCategory::cpu,
                        "probe {:#018x} a0 {:#x} a1 {:#x} a2 {:#x} a3 {:#x} ra {:#x} v0 {:#x}",
                        state.pc, state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7],
                        state.gpr[31], state.gpr[2]);
        }
    }
    if (tracer_.enabled(TraceCategory::cpu) && pc_probes_.empty()) {
        tracer_.log(TraceCategory::cpu, "pc {:#018x}", state.pc);
    }
    executing_ = true;
    interpreter_.step();
    executing_ = false;
    // Events fire at the first cycle boundary at or after their deadline. Between events,
    // time is brought up to date only when a device is accessed.
    if (cpu_.cycles() >= next_event_cycle_) {
        synchronize_time();
    }
}

void Ip27Machine::synchronize_time() {
    invariant(!executing_ || synchronized_accesses_ != 0,
              "events run only between instructions or inside a synchronized access");
    scheduler_.advance_to(time_at_cycles(cpu_.cycles(), config_.cpu_clock));
    update_next_event();
}

void Ip27Machine::update_next_event() {
    const auto deadline = scheduler_.next_deadline();
    if (!deadline) {
        next_event_cycle_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        // The smallest cycle count whose time is at or after the deadline. Remembered per
        // deadline: this runs after every device access.
        if (*deadline != converted_deadline_) {
            std::uint64_t cycle = cycles_at(*deadline, config_.cpu_clock);
            if (time_at_cycles(cycle, config_.cpu_clock) < *deadline) {
                ++cycle;
            }
            converted_deadline_ = *deadline;
            converted_deadline_cycle_ = cycle;
        }
        next_event_cycle_ = converted_deadline_cycle_;
    }
    stop_cycle_ = std::min(next_event_cycle_, run_end_cycle_);
}

std::expected<std::uint64_t, AccessFault>
Ip27Machine::SynchronizedTarget::mmio_read(std::uint64_t offset, AccessWidth width) {
    ++machine_.synchronized_accesses_;
    machine_.synchronize_time();
    auto result = target_.mmio_read(offset, width);
    machine_.update_next_event();
    --machine_.synchronized_accesses_;
    return result;
}

std::expected<void, AccessFault> Ip27Machine::SynchronizedTarget::mmio_write(std::uint64_t offset,
                                                                             AccessWidth width,
                                                                             std::uint64_t value) {
    ++machine_.synchronized_accesses_;
    machine_.synchronize_time();
    auto result = target_.mmio_write(offset, width, value);
    machine_.update_next_event();
    --machine_.synchronized_accesses_;
    return result;
}

void Ip27Machine::run(std::uint64_t cycles) {
    run_end_cycle_ = cpu_.cycles() + cycles;
    stop_cycle_ = std::min(next_event_cycle_, run_end_cycle_);
    // Probes and instruction tracing observe every cycle; otherwise the interpreter runs
    // uninterrupted to the next event, the same cycle-for-cycle sequence as step().
    const bool observed = !pc_probes_.empty() || tracer_.enabled(TraceCategory::cpu);
    while (cpu_.cycles() < run_end_cycle_) {
        if (observed) {
            step();
            continue;
        }
        executing_ = true;
        interpreter_.run_until(stop_cycle_);
        executing_ = false;
        if (cpu_.cycles() >= next_event_cycle_) {
            synchronize_time();
        }
    }
    run_end_cycle_ = 0;
    synchronize_time();
}

} // namespace ultraviolent::ip27
