#include <ultraviolent/machines/ip27/ip27_machine.hpp>

#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <thread>
#include <utility>
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
constexpr std::uint64_t flash_command_span = std::uint64_t{flash_size} << 3;
constexpr unsigned hspec_attribute = 0;
constexpr unsigned io_attribute = 1;
constexpr unsigned uncac_attribute = 3;
// IALIAS: the local Hub's registers at the start of widget 1's 16 MB I/O window.
constexpr std::uint64_t hub_widget = 1;
constexpr std::uint64_t small_window_shift = 24;
constexpr unsigned big_window_count = 7;
constexpr std::uint64_t big_window_shift = 29;
// The node ID's place in a physical address, bits 39:32 (IP27.adoc "Node address space").
constexpr unsigned node_id_shift = 32;

std::uint64_t fnv1a(std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325;
    for (const std::byte b : bytes) {
        hash = (hash ^ std::to_integer<std::uint64_t>(b)) * 0x0000'0100'0000'01b3;
    }
    return hash;
}

// Scheduler event names for node `index`'s components: the first node keeps the one-node
// names, so its snapshots are unchanged.
std::string event_prefix(unsigned index) {
    return index == 0 ? std::string{} : std::format("node{}.", index);
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
    if (config.module == ModuleType::deskside && config.node_slot > 2) {
        return std::unexpected("a deskside module has node slots n1 and n2");
    }
    if (config.nodes < 1 || config.nodes > 2) {
        return std::unexpected("one or two node boards");
    }
    if (config.nodes == 2 && (config.module != ModuleType::deskside || config.node_slot != 1)) {
        // A rack module's n1 and n2 are on different Crossbows, which is not modeled.
        return std::unexpected("two nodes are modeled in a deskside module, in n1 and n2");
    }
    if (config.cpus < 1 || config.cpus > 2) {
        return std::unexpected("a node holds one or two CPUs");
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

mips::ProcessorModel processor_model(const Ip27Config& config, std::span<const std::byte> prom) {
    const mips::CpuConfig identity = cpu_config(config, prom);
    return config.processor == ProcessorType::r12000 ? mips::r12000(identity)
                                                    : mips::r10000(identity);
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

unsigned baseio_port(ModuleType module) {
    // observed: with the BaseIO on port 15 a rack module's PROM names it io3, so a rack module
    // cables it to port 8, its io1 (IP27.adoc "Crossbow"); a deskside's io1 is port 15.
    return module == ModuleType::deskside ? 0xf : 0x8;
}

unsigned hub_xbow_port(ModuleType module, unsigned slot) {
    // observed: the PROM's Crossbow tables by slot code: deskside n1 0xa, n2 9; rack (two
    // crossbows per module) n1 9, n2 0xa, n3 0xa, n4 9.
    if (module == ModuleType::deskside) {
        return slot == 1 ? 0xa : 0x9;
    }
    constexpr std::array<unsigned, 5> ports = {0, 0x9, 0xa, 0xa, 0x9};
    return ports[slot];
}

unsigned node_slot_id(ModuleType module, unsigned slot) {
    // observed: the PROM maps SN0 slot IDs 0-7 through 0xf0, 0xf0, 0x52, 0x51, 4, 3, 2, 1
    // (0xf0 invalid): IDs 3 and 2 are deskside n1 and n2, IDs 7-4 rack n1-n4.
    if (module == ModuleType::deskside) {
        return slot == 1 ? 3 : 2;
    }
    return (8 - slot) & 7;
}

Ip27Machine::Node::Node(Ip27Machine& owner, unsigned node_index, unsigned slot,
                        std::span<const std::byte> prom)
    : machine{owner}, index{node_index}, memory{owner.config_.memory_bytes}, flash{flash_size},
      flash_device{owner.scheduler_, owner.tracer_, flash.bytes(), [this] { flash_mode_changed(); },
                   node_index == 0 ? std::string{} : std::format("node{}.flash", node_index)},
      flash_interface{flash_device, owner.tracer_}, flash_commands{flash_interface},
      i2c{owner.scheduler_, owner.tracer_, owner.i2c_bus_, event_prefix(node_index)},
      directory{owner.tracer_, checked_bank_sizes(owner.config_), owner.config_.directory},
      board_nic{0x09, 0x5 + node_index},
      hub{owner.scheduler_,
          owner.tracer_,
          owner.config_.hub_revision,
          node_slot_id(owner.config_.module, slot),
          owner.config_.cpus,
          i2c,
          directory,
          bus,
          [this] { reset_cpus(ResetKind::cold); },
          event_prefix(node_index)},
      reset_rendezvous{owner},
      cpu_a{owner, bus, owner.tracer_, processor_model(owner.config_, prom), 0},
      synchronized_hub{owner, hub.port(0), &hub}, synchronized_hub_b{owner, hub.port(1), &hub},
      synchronized_flash{owner, flash_interface},
      synchronized_flash_commands{owner, flash_commands} {
    hub.connect_cpu(0, cpu_a.pins);
    cpu_a.cpu.set_reset_rendezvous(&reset_rendezvous);
    reset_rendezvous.add(cpu_a.cpu);
    // hypothesis: an IP27 node board's record is named "IP27", like slate's IP31 boards'
    // "IP31" (OBS-SLATE-0004); its part number is not known, so it is left blank. The
    // barcode is synthetic.
    board_nic.memory() = board_nic_memory(
        {.serial = std::format("UVX{:03}", 5 + node_index), .part = "", .revision = "",
         .name = "IP27"});
    mlan.attach(board_nic);
    hub.connect_mlan(mlan);
    if (owner.config_.cpus == 2) {
        // Slice B: the same processor on the Hub's second port, sharing memory with A
        // (SMP.adoc).
        cpu_b.emplace(owner, bus, owner.tracer_, processor_model(owner.config_, prom), 1);
        hub.connect_cpu(1, cpu_b->pins);
        cpu_b->cpu.set_reset_rendezvous(&reset_rendezvous);
        reset_rendezvous.add(cpu_b->cpu);
    }
    for (unsigned widget = 0; widget < xtalk::widget_count; ++widget) {
        if (widget != hub_widget) {
            xtalk_windows[widget].emplace(owner, hub.io_window(widget));
        }
    }
    for (unsigned window = 0; window < big_window_count; ++window) {
        big_windows[window].emplace(owner, hub.big_window(window + 1));
    }
    hub.connect_node_id([&owner] { owner.map_views(); });
    if (owner.config_.nodes > 1) {
        hub.set_trace_name(std::format("n{} ", slot));
    }
    // Unprogrammed flash reads as 0xff.
    std::ranges::fill(flash.bytes(), std::byte{0xff});
    std::ranges::copy(prom, flash.bytes().begin());
}

Ip27Machine::Ip27Machine(const Ip27Config& config, std::span<const std::byte> prom)
    : config_{config} {
    invariant(validate(config, prom).has_value(),
              "Ip27Machine built from an invalid configuration");
    Node& node = *nodes_.emplace_back(std::make_unique<Node>(*this, 0, config.node_slot, prom));
    processors_[0] = &node.cpu_a;
    if (node.cpu_b) {
        processors_[1] = &*node.cpu_b;
    }
    if (config.nodes == 2) {
        Node& second = *nodes_.emplace_back(std::make_unique<Node>(*this, 1, 2, prom));
        processors_[2] = &second.cpu_a;
        if (second.cpu_b) {
            processors_[3] = &*second.cpu_b;
        }
    }
    for (Processor* p : processors_) {
        if (p != nullptr) {
            installed_[cpu_count_++] = p;
        }
    }
    if (cpu_count() > 1) {
        for (unsigned i = 0; i < processors_.size(); ++i) {
            if (processors_[i] != nullptr) {
                processors_[i]->cpu.connect_coherence(coherence_, i);
                coherence_.attach(i, processors_[i]->cpu);
            }
        }
    }
    // The default engine: tier 1 where this host has a backend, tier 0 elsewhere (JIT.adoc).
    if (!set_execution_engine(mips::ExecutionEngine::tier1)) {
        set_execution_engine(mips::ExecutionEngine::tier0);
    }
    i2c_bus_.attach(controller_nvram_);
    if (nodes_.size() == 2) {
        // slate's two Hubs are cabled to each other without a router (SMP.adoc "S5").
        nodes_[0]->hub.connect_link(nodes_[1]->hub);
        nodes_[1]->hub.connect_link(nodes_[0]->hub);
    }
    for (auto& n : nodes_) {
        n->hub.connect_xtalk(xbow_);
        xbow_.attach(hub_xbow_port(config_.module, n->index == 0 ? config_.node_slot : 2),
                     n->hub.widget());
    }
    xbow_.attach(baseio_port(config_.module), baseio_bridge_);
    baseio_bridge_.connect(xbow_);
    // Slate's BaseIO boards (OBS-SLATE-0004), with synthetic barcodes.
    mio_nic_.memory() = board_nic_memory(
        {.serial = "UVX001", .part = "030-0880-003", .revision = "C", .name = "MIO"});
    baseio_nic_.memory() = board_nic_memory(
        {.serial = "UVX002", .part = "030-0734-003", .revision = "A", .name = "BASEIO"});
    baseio_nic_bus_.attach(mio_nic_);
    baseio_nic_bus_.attach(baseio_nic_);
    baseio_bridge_.connect_nic(baseio_nic_bus_);
    if (config_.module == ModuleType::deskside) {
        // slate's module NICs (OBS-SLATE-0004) on n1's MicroLAN, with synthetic serials:
        // MODULEID (no part number) and the deskside midplane, 4P1G5_MPLN 030-1066-002 rev K.
        module_nic_.memory() = board_nic_memory(
            {.serial = "K0000001", .part = "", .revision = "", .name = "MODULEID"});
        midplane_nic_.memory() = board_nic_memory(
            {.serial = "UVX004", .part = "030-1066-002", .revision = "K", .name = "4P1G5_MPLN"});
        nodes_.front()->mlan.attach(module_nic_);
        nodes_.front()->mlan.attach(midplane_nic_);
    }
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
    flash_hash_ = fnv1a(node.flash.bytes());
    for (auto& n : nodes_) {
        if (n.get() != &node) {
            n->bus.share_code_writes(node.bus);
        }
    }
    map_views();
}

void Ip27Machine::map_views() {
    // Each view changes as a whole (AddressSpace::begin_update).
    for (auto& node : nodes_) {
        node->bus.begin_update();
        node->map_system_windows();
        node->bus.end_update();
    }
}

void Ip27Machine::Node::map_system_windows() {
    // The node's view of the system (SMP.adoc "S5: the second node"): its own resources at
    // its node ID (physical address bits 39:32), the position-independent aliases at node 0's
    // addresses (LINUX addrs.h: UALIAS, LBOOT, and IALIAS always reach the reader's own node),
    // and each other node at that node's ID once the IDs differ. With node ID 0 this is the
    // one-node map, in which the aliases are the node's own addresses.
    bus.clear();
    map_resources(*this, std::uint64_t{hub.node_id()} << node_id_shift);
    map_aliases();
    for (const auto& other : machine.nodes_) {
        if (other.get() != this && other->hub.node_id() != hub.node_id()) {
            map_resources(*other, std::uint64_t{other->hub.node_id()} << node_id_shift);
        }
    }
}

void Ip27Machine::Node::map_flash(std::uint64_t address) {
    const PhysicalRange range{PhysicalAddress{address}, flash_size};
    must_map(flash_device.array_mode()
                 ? bus.map_memory(range, flash, 0, MemoryAccess::read_only, &synchronized_flash)
                 : bus.map_mmio(range, synchronized_flash),
             "boot PROM window");
}

void Ip27Machine::Node::map_aliases() {
    using mips::system_address::uncached_window;
    const std::uint64_t memory_size = machine.config_.memory_bytes;
    // HSPEC UALIAS: uncached attribute 0, the first 256 MB of node space, reaches memory.
    must_map(bus.map_memory({PhysicalAddress{uncached_window(hspec_attribute)},
                             std::min(memory_size, ualias_size)},
                            memory, 0, MemoryAccess::read_write),
             "UALIAS window");
    // HSPEC LBOOT: the boot PROM, at the base of LBOOT (observed: the PROM reads its
    // ip27conf block there) and where the reset vector points. Reads come straight from the
    // flash array while it is in read-array mode; writes always reach the flash through its
    // interface (IP27.adoc "Boot PROM").
    for (const std::uint64_t base : {lboot_base, reset_flash_base}) {
        map_flash(uncached_window(hspec_attribute) + base);
    }
    // Command writes address the flash at offset << 3, so writing all of it takes 8 MB of
    // LBOOT (observed: sector 14's erase command goes to LBOOT + 0x700000). hypothesis: the
    // rest of that range only takes writes.
    must_map(bus.map_mmio({PhysicalAddress{uncached_window(hspec_attribute) + lboot_base +
                                           flash_size},
                           flash_command_span - flash_size},
                          synchronized_flash_commands),
             "boot PROM command window");
    // IO IALIAS: the local Hub's registers. Each CPU reaches the Hub through its own port
    // (ADR-023).
    must_map(bus.map_ports({PhysicalAddress{uncached_window(io_attribute) +
                                            (hub_widget << small_window_shift)},
                            Hub::window_size},
                           {&synchronized_hub, &synchronized_hub_b}),
             "Hub window");
}

void Ip27Machine::Node::map_resources(Node& node, std::uint64_t base) {
    using mips::system_address::uncached_window;
    const std::uint64_t memory_size = machine.config_.memory_bytes;
    const bool own = &node == this;
    // CAC: cached requests reach memory.
    must_map(bus.map_memory({PhysicalAddress{base}, memory_size}, node.memory, 0,
                            MemoryAccess::read_write),
             "CAC memory window");
    // UNCAC: uncached attribute 3 reaches memory.
    must_map(bus.map_memory({PhysicalAddress{uncached_window(uncac_attribute) + base}, memory_size},
                            node.memory, 0, MemoryAccess::read_write),
             "UNCAC memory window");
    // HSPEC RBOOT (LINUX addrs.h NODE_RBOOT_BASE, "Hub's RBOOT space"): the node's boot PROM
    // addressed through the node's own HSPEC space (observed: the PROM reads the ip27conf
    // processor clock at RBOOT + 0x70). Commands decode as in LBOOT (hypothesis).
    const std::uint64_t rboot = uncached_window(hspec_attribute) + base + rboot_base;
    if (own) {
        map_flash(rboot);
    } else {
        // Another node's PROM, read through its interface (its array mode is its own).
        must_map(bus.map_mmio({PhysicalAddress{rboot}, flash_size}, node.synchronized_flash),
                 "remote boot PROM window");
    }
    must_map(bus.map_mmio({PhysicalAddress{rboot + flash_size}, flash_command_span - flash_size},
                          node.synchronized_flash_commands),
             "boot PROM command window");
    // HSPEC back door: directory and protection memory at 3 GB (IP27.adoc "Memory and
    // directory").
    must_map(bus.map_mmio({PhysicalAddress{uncached_window(hspec_attribute) + base +
                                           DirectoryMemory::window_offset},
                           DirectoryMemory::window_size},
                          node.directory),
             "back-door directory window");
    // IO: the node's Hub registers at the remote-Hub alias (widget 1 window + 0x800000, LINUX
    // REMOTE_HUB_ADDR; observed: the PROM uses it for its own NASID 0). A remote request comes
    // over the link rather than from a processor: hypothesis, the Hub answers it as from
    // slice A.
    const std::uint64_t hub_window =
        uncached_window(io_attribute) + base + (hub_widget << small_window_shift);
    must_map(bus.map_ports({PhysicalAddress{hub_window + Hub::window_size}, Hub::window_size},
                           own ? AddressSpace::Ports{&synchronized_hub, &synchronized_hub_b}
                               : AddressSpace::Ports{&node.synchronized_hub,
                                                     &node.synchronized_hub}),
             "Hub window");
    // IO: every other widget's small window is PIO over the node's crosstalk link.
    for (unsigned widget = 0; widget < xtalk::widget_count; ++widget) {
        if (widget == hub_widget) {
            continue;
        }
        must_map(bus.map_mmio({PhysicalAddress{uncached_window(io_attribute) + base +
                                               (std::uint64_t{widget} << small_window_shift)},
                               xtalk::small_window_size},
                              *node.xtalk_windows[widget]),
                 "crosstalk window");
    }
    // IO: the big windows (M mode: seven of 512 MB from IO + 512 MB, LINUX sn0/addrs.h
    // NODE_BWIN_BASE) reach crosstalk through the Hub's IIO_ITTE translation entries
    // (Hub::BigWindow). observed: IRIX programs the ITTEs, and its flash utility reaches the
    // BaseIO PROM through one.
    for (unsigned window = 0; window < big_window_count; ++window) {
        must_map(bus.map_mmio({PhysicalAddress{uncached_window(io_attribute) + base +
                                               (std::uint64_t{window + 1} << big_window_shift)},
                               std::uint64_t{1} << big_window_shift},
                              *node.big_windows[window]),
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

void Ip27Machine::Node::flash_mode_changed() {
    // Every view that reads this flash fast follows its mode; rebuilding them is simplest.
    machine.map_views();
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

void Ip27Machine::load_flash(std::span<const std::byte> contents, unsigned node) {
    invariant(contents.size() == flash_size, "flash contents are 1 MB");
    std::ranges::copy(contents, nodes_.at(node)->flash.bytes().begin());
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
    image.put("machine.baseio_port", std::uint64_t{baseio_port(config_.module)});
    image.put("machine.module", std::uint64_t{std::to_underlying(config_.module)});
    image.put("machine.node_slot", std::uint64_t{config_.node_slot});
    image.put("machine.processor_id", std::uint64_t{config_.cpu.processor_id});
    Node& node = *nodes_.front();
    if (node.cpu_b) {
        // One-CPU snapshots have no such key and keep their format.
        image.put("machine.cpus", std::uint64_t{2});
        node.cpu_b->cpu.save_state(image, "slice_b.");
        coherence_.save_state(image);
    }
    scheduler_.save_state(image);
    node.cpu_a.cpu.save_state(image);
    node.hub.save_state(image);
    node.i2c.save_state(image);
    elsc_.save_state(image);
    node.flash_device.save_state(image);
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
    node.directory.save_state(image);
    image.put_sparse("memory", node.memory.bytes());
    if (nodes_.size() > 1) {
        // The second node's components keep their field names under "node1.".
        image.put("machine.nodes", std::uint64_t{nodes_.size()});
        StateImage second;
        nodes_[1]->save_state(second);
        image.put_image("node1.", second);
    }
    return image;
}

void Ip27Machine::Node::save_state(StateImage& image) const {
    cpu_a.cpu.save_state(image);
    if (cpu_b) {
        cpu_b->cpu.save_state(image, "slice_b.");
    }
    hub.save_state(image);
    i2c.save_state(image);
    flash_device.save_state(image);
    directory.save_state(image);
    image.put_sparse("memory", memory.bytes());
}

bool Ip27Machine::Node::load_state(const StateImage& image) {
    if (!image.get_sparse("memory", memory.bytes())) {
        return false;
    }
    cpu_a.cpu.load_state(image);
    if (cpu_b) {
        cpu_b->cpu.load_state(image, "slice_b.");
    }
    hub.load_state(image);
    i2c.load_state(image);
    flash_device.load_state(image);
    directory.load_state(image);
    return true;
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
    if (snapshot_baseio_port != baseio_port(config_.module)) {
        return std::unexpected("snapshot was taken with the BaseIO on another Crossbow port");
    }
    // Snapshots from before these fields were taken in rack slot n1 (the slot ID and Hub port
    // the guest saw).
    std::uint64_t module = std::to_underlying(ModuleType::rack);
    std::uint64_t node_slot = 1;
    image.get("machine.module", module);
    image.get("machine.node_slot", node_slot);
    if (module != std::to_underlying(config_.module) || node_slot != config_.node_slot) {
        return std::unexpected("snapshot was taken in another module type or node slot");
    }
    // Snapshots from before this field were taken with the default R10000.
    std::uint64_t processor_id = Ip27Config{}.cpu.processor_id;
    image.get("machine.processor_id", processor_id);
    if (processor_id != config_.cpu.processor_id) {
        return std::unexpected("snapshot was taken with another processor");
    }
    std::uint64_t cpus = 1;
    image.get("machine.cpus", cpus);
    if (cpus != config_.cpus) {
        return std::unexpected("snapshot was taken with a different number of CPUs");
    }
    std::uint64_t nodes = 1;
    image.get("machine.nodes", nodes);
    if (nodes != nodes_.size()) {
        return std::unexpected("snapshot was taken with a different number of nodes");
    }
    if (std::ranges::any_of(processors_,
                            [](const Processor* p) { return p != nullptr && p->cpu.cycles() != 0; })) {
        return std::unexpected("snapshots load only into a machine that has not run");
    }
    Node& node = *nodes_.front();
    if (!image.get_sparse("memory", node.memory.bytes())) {
        return std::unexpected("snapshot memory is malformed");
    }
    scheduler_.load_state(image);
    node.cpu_a.cpu.load_state(image);
    if (node.cpu_b) {
        node.cpu_b->cpu.load_state(image, "slice_b.");
        coherence_.load_state(image);
    }
    node.hub.load_state(image);
    node.i2c.load_state(image);
    elsc_.load_state(image);
    node.flash_device.load_state(image);
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
    node.directory.load_state(image);
    if (nodes_.size() > 1 && !nodes_[1]->load_state(image.sub_image("node1."))) {
        return std::unexpected("snapshot memory is malformed");
    }
    // The Hubs' node IDs and the flash modes decide each node's view.
    map_views();
    update_next_event();
    return {};
}

void Ip27Machine::reset(ResetKind kind) {
    for (auto& node : nodes_) {
        node->hub.reset(kind);
    }
    if (kind != ResetKind::warm) {
        xbow_.reset();
        baseio_bridge_.reset();
    }
    for (auto& node : nodes_) {
        node->reset_cpus(kind);
    }
}

void Ip27Machine::Node::reset_cpus(ResetKind kind) {
    // On host threads each CPU takes the reset on its own thread at its next boundary, the
    // caller's own too, and they meet there (NodeReset) to leave it at one cycle count, as
    // below (SMP.adoc "S4").
    if (machine.parallel_running_) {
        reset_rendezvous.post(kind);
        return;
    }
    // hypothesis: a reset reaches both CPUs of the node together and releases them together
    // (IP27.adoc "Two CPUs on a Hub"). The CPUs' own times differ by up to a slice here, so
    // the one behind is held until the other's cycle, as if in reset, and both leave reset at
    // the same cycle; otherwise their phase after a reset would depend on the slice length.
    std::uint64_t release = cpu_a.cpu.cycles();
    if (cpu_b) {
        release = std::max(release, cpu_b->cpu.cycles());
    }
    cpu_a.cpu.hold_until(release);
    cpu_a.cpu.reset(kind);
    if (cpu_b) {
        cpu_b->cpu.hold_until(release);
        cpu_b->cpu.reset(kind);
    }
    machine.coherence_.clear();
}

void Ip27Machine::attach_disk(BlockStore& store) {
    system_disk_.emplace(tracer_, "disk", store);
    scsi_bus0_.attach(system_disk_id, *system_disk_);
}

void Ip27Machine::step() {
    step(*processors_[0]);
}

void Ip27Machine::step(Processor& processor) {
    const mips::IntegerState& state = processor.cpu.state();
    // With several CPUs, each line names the CPU's slice, and with two nodes its node slot.
    constexpr std::array<std::string_view, 4> slices = {"A ", "B ", "n2 A ", "n2 B "};
    constexpr std::array<std::string_view, 4> node_slices = {"n1 A ", "n1 B ", "n2 A ",
                                                             "n2 B "};
    const auto index = static_cast<std::size_t>(std::ranges::find(processors_, &processor) -
                                                processors_.begin());
    const std::string_view slice = cpu_count() == 1   ? ""
                                   : nodes_.size() > 1 ? node_slices[index]
                                                       : slices[index];
    for (const std::uint64_t probe : pc_probes_) {
        if (probe == state.pc) {
            tracer_.log(TraceCategory::cpu,
                        "{}probe {:#018x} a0 {:#x} a1 {:#x} a2 {:#x} a3 {:#x} ra {:#x} v0 {:#x}",
                        slice, state.pc, state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7],
                        state.gpr[31], state.gpr[2]);
        }
    }
    if (tracer_.enabled(TraceCategory::cpu) && pc_probes_.empty()) {
        tracer_.log(TraceCategory::cpu, "{}pc {:#018x}", slice, state.pc);
    }
    executing_ = true;
    processor.interpreter.step();
    executing_ = false;
    // Events fire at the first cycle boundary at or after their deadline. Between events,
    // time is brought up to date only when a device is accessed.
    if (lowest_cycles() >= next_event_cycle_) {
        synchronize_time();
    }
}

std::uint64_t Ip27Machine::lowest_of_several() const {
    std::uint64_t lowest = std::numeric_limits<std::uint64_t>::max();
    for (unsigned i = 0; i < cpu_count_; ++i) {
        lowest = std::min(lowest, installed_[i]->cpu.cycles());
    }
    return lowest;
}

// On every device access (SynchronizedTarget), so inlined there: measured 2026-10-14, the call
// cost the device-bound `prom` workload about 3%.
[[gnu::always_inline]] inline void Ip27Machine::synchronize_time() {
    invariant(!executing_ || synchronized_accesses_ != 0,
              "events run only between instructions or inside a synchronized access");
    // Device time may be ahead of every CPU after a run on host threads (SMP.adoc "S4").
    scheduler_.advance_to(std::max(cpu_time_.time_at(lowest_cycles()), scheduler_.now()));
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
            if (cpu_time_.time_at(cycle) < *deadline) {
                ++cycle;
            }
            converted_deadline_ = *deadline;
            converted_deadline_cycle_ = cycle;
        }
        next_event_cycle_ = converted_deadline_cycle_;
    }
    // With several CPUs, virtual time is another CPU's while the running one is ahead of it,
    // so an event later than that CPU cannot fall due in this slice.
    const bool several = cpu_count() > 1;
    std::uint64_t event_stop = next_event_cycle_;
    if (several && next_event_cycle_ > lowest_other_cycles()) {
        event_stop = std::numeric_limits<std::uint64_t>::max();
    }
    // With several CPUs a run ends between slices (run_many), unless a stop was requested.
    const std::uint64_t run_end =
        several && !stop_requested_ ? std::numeric_limits<std::uint64_t>::max() : run_end_cycle_;
    stop_cycle_ = std::min({event_stop, run_end, slice_end_cycle_});
}

void Ip27Machine::CpuPins::set_interrupt_level(std::uint32_t input, bool asserted) {
    levels_ = asserted ? levels_ | 1u << input : levels_ & ~(1u << input);
    if (!machine_.parallel_running_ || current_ == &processor_) {
        processor_.cpu.set_interrupt_level(input, asserted);
    }
    // Posted also from the CPU's own thread, so an older posting cannot undo this change.
    if (machine_.parallel_running_) {
        processor_.cpu.post_interrupt_levels(levels_);
    }
}

thread_local Ip27Machine::Processor* Ip27Machine::current_ = nullptr;

template <typename Access> auto Ip27Machine::locked_access(Access access) {
    Processor& self = *current_;
    // Waiting for the lock, the CPU is parked: it stores nothing until it has looked at the
    // coherence generation again (mips::Coherence).
    self.cpu.park();
    device_lock_.lock();
    self.cpu.unpark();
    advance_device_time(self.cpu.cycles());
    const VirtualTime own_time = cpu_time_.time_at(self.cpu.cycles());
    for (auto& node : nodes_) {
        node->hub.set_reader_time(own_time);
    }
    auto result = access();
    for (auto& node : nodes_) {
        node->hub.set_reader_time(std::nullopt);
    }
    publish_next_event();
    device_lock_.unlock();
    return result;
}

std::expected<std::uint64_t, AccessFault>
Ip27Machine::SynchronizedTarget::mmio_read(std::uint64_t offset, AccessWidth width) {
    if (machine_.parallel_running_) [[unlikely]] {
        // Without the lock: the firmware's delay loops, most device reads, and IRIX's polling
        // of PI_INT_PEND0 (SMP.adoc "S4.0").
        if (hub_ != nullptr && width == AccessWidth::bits64) {
            switch (offset) {
            case Hub::rt_count_offset:
                return hub_->rt_count_at(machine_.cpu_time_.time_at(current_->cpu.cycles()));
            case Hub::int_pend0_offset:
                return hub_->interrupt_pending(0);
            case Hub::int_pend1_offset:
                return hub_->interrupt_pending(1);
            default:
                break;
            }
        }
        return machine_.locked_access([&] { return target_.mmio_read(offset, width); });
    }
    ++machine_.synchronized_accesses_;
    machine_.synchronize_time();
    // PI_RT_COUNT, most device reads (the firmware's delay loops): a function of device time,
    // read straight from the Hub. It changes no event, so the next one stays where it was.
    if (hub_ != nullptr && offset == Hub::rt_count_offset && width == AccessWidth::bits64) {
        --machine_.synchronized_accesses_;
        return hub_->rt_count();
    }
    auto result = target_.mmio_read(offset, width);
    machine_.update_next_event();
    --machine_.synchronized_accesses_;
    return result;
}

std::expected<void, AccessFault> Ip27Machine::SynchronizedTarget::mmio_write(std::uint64_t offset,
                                                                             AccessWidth width,
                                                                             std::uint64_t value) {
    if (machine_.parallel_running_) [[unlikely]] {
        return machine_.locked_access([&] { return target_.mmio_write(offset, width, value); });
    }
    ++machine_.synchronized_accesses_;
    machine_.synchronize_time();
    auto result = target_.mmio_write(offset, width, value);
    machine_.update_next_event();
    --machine_.synchronized_accesses_;
    return result;
}

void Ip27Machine::run(std::uint64_t cycles) {
    if (cpu_count() > 1) {
        const bool observed = !pc_probes_.empty() || tracer_.enabled(TraceCategory::cpu);
        if (parallel_ && !observed) {
            run_parallel(cycles);
        } else {
            run_many(cycles);
        }
        return;
    }
    Processor& cpu_a = *processors_[0];
    run_end_cycle_ = cpu_a.cpu.cycles() + cycles;
    stop_cycle_ = std::min(next_event_cycle_, run_end_cycle_);
    // Probes and instruction tracing observe every cycle; otherwise the interpreter runs
    // uninterrupted to the next event, the same cycle-for-cycle sequence as step().
    const bool observed = !pc_probes_.empty() || tracer_.enabled(TraceCategory::cpu);
    while (cpu_a.cpu.cycles() < run_end_cycle_) {
        if (observed) {
            step(cpu_a);
            continue;
        }
        run_processor(false);
        if (cpu_a.cpu.cycles() >= next_event_cycle_) {
            synchronize_time();
        }
    }
    run_end_cycle_ = 0;
    synchronize_time();
}

void Ip27Machine::run_processor(bool observed) {
    Processor& p = *installed_[running_];
    if (observed) {
        while (p.cpu.cycles() < stop_cycle_) {
            step(p);
        }
        return;
    }
    executing_ = true;
    if (engine_ != mips::ExecutionEngine::reference) {
        p.blocks.run_until(stop_cycle_);
    } else {
        p.interpreter.run_until(stop_cycle_);
    }
    executing_ = false;
}

void Ip27Machine::run_many(std::uint64_t cycles) {
    // Lowest time first (ADR-023): the CPU furthest behind runs until it is cpu_slice ahead of
    // the next lowest; the lowest index goes first on a tie. A slice ends only there: the
    // engine also returns when an event falls due (and after every instruction when observed),
    // and the slice then goes on. So the order depends only on the cycle counts, never on where
    // an engine returns or a run ends: a run ends between slices once every CPU has completed
    // `cycles` more, so splitting a run, observing it, or resuming a snapshot changes nothing.
    run_end_cycle_ = lowest_cycles() + cycles;
    stop_requested_ = false;
    const bool observed = !pc_probes_.empty() || tracer_.enabled(TraceCategory::cpu);
    bool in_slice = false;
    while (in_slice || lowest_cycles() < run_end_cycle_) {
        if (!in_slice) {
            running_ = 0;
            for (unsigned i = 1; i < cpu_count_; ++i) {
                if (installed_[i]->cpu.cycles() < installed_[running_]->cpu.cycles()) {
                    running_ = i;
                }
            }
            slice_end_cycle_ = lowest_other_cycles() + config_.cpu_slice;
        }
        update_next_event();
        run_processor(observed);
        if (lowest_cycles() >= next_event_cycle_) {
            synchronize_time();
        }
        // A stop ends the slice; the CPU behind then catches up to the stop (update_next_event).
        in_slice = !stop_requested_ && installed_[running_]->cpu.cycles() < slice_end_cycle_;
    }
    running_ = 0;
    slice_end_cycle_ = std::numeric_limits<std::uint64_t>::max();
    run_end_cycle_ = 0;
    stop_requested_ = false;
    synchronize_time();
}

void Ip27Machine::run_parallel(std::uint64_t cycles) {
    // SMP.adoc "S4". Everything the threads share is made safe for them here, before they
    // start: the views publish versions, and the CPUs switch to compare-and-swap LL/SC.
    const std::uint64_t end = lowest_cycles() + cycles;
    for (auto& node : nodes_) {
        node->bus.set_shared(true);
    }
    for (unsigned i = 0; i < cpu_count_; ++i) {
        Processor& p = *installed_[i];
        p.cpu.set_parallel(true);
        p.pins.set_levels(p.cpu.external_interrupt_levels());
        p.published.store(p.cpu.cycles(), std::memory_order_relaxed);
        // Until its thread runs, a CPU stores nothing.
        p.cpu.park();
    }
    parallel_stop_.store(false, std::memory_order_relaxed);
    parallel_finished_.store(0, std::memory_order_relaxed);
    parallel_running_ = true;
    publish_next_event();
    {
        std::vector<std::jthread> threads;
        for (unsigned i = 1; i < cpu_count_; ++i) {
            threads.emplace_back([this, i, end] { run_thread(*installed_[i], i, end); });
        }
        run_thread(*installed_[0], 0, end);
    }
    parallel_running_ = false;
    // What other threads posted at the end is applied now, on this one.
    for (unsigned i = 0; i < cpu_count_; ++i) {
        Processor& p = *installed_[i];
        p.cpu.attend();
        p.cpu.unpark();
        p.cpu.set_parallel(false);
    }
    for (auto& node : nodes_) {
        node->reset_rendezvous.clear();
    }
    stop_requested_ = false;
    update_next_event();
}

void Ip27Machine::run_thread(Processor& processor, unsigned index, std::uint64_t end) {
    current_ = &processor;
    mips::Cpu& cpu = processor.cpu;
    cpu.unpark();
    std::uint64_t limit = 0;
    while (true) {
        if (cpu.attention_raised()) {
            cpu.attend();
        }
        const std::uint64_t cycles = cpu.cycles();
        processor.published.store(cycles, std::memory_order_release);
        if (cycles >= end || parallel_stop_.load(std::memory_order_relaxed)) {
            break;
        }
        const std::uint64_t next_event = parallel_next_event_.load(std::memory_order_acquire);
        if (cycles >= next_event) {
            cpu.park();
            device_lock_.lock();
            cpu.unpark();
            advance_device_time(cycles);
            publish_next_event();
            device_lock_.unlock();
            continue;
        }
        // The window: at most parallel_window_ cycles ahead of the slowest other CPU.
        const std::uint64_t window_end = lowest_published_other(index) + parallel_window_;
        if (cycles >= window_end) {
            cpu.park();
            // Attention (a reset to meet, a level to apply) goes to the loop's top.
            while (cycles >= lowest_published_other(index) + parallel_window_ &&
                   !parallel_stop_.load(std::memory_order_relaxed) && !cpu.attention_raised()) {
                std::this_thread::yield();
            }
            cpu.unpark();
            continue;
        }
        limit = std::min({end, next_event, window_end});
        if (engine_ != mips::ExecutionEngine::reference) {
            processor.blocks.run_until(limit);
        } else {
            processor.interpreter.run_until(limit);
        }
    }
    processor.published.store(cpu.cycles(), std::memory_order_release);
    cpu.park();
    parallel_finished_.fetch_add(1, std::memory_order_relaxed);
    current_ = nullptr;
}

void Ip27Machine::NodeReset::add(mips::Cpu& cpu) {
    invariant(participants_ < cpus_.size(), "two CPUs per node");
    cpus_[participants_++] = &cpu;
}

void Ip27Machine::NodeReset::post(ResetKind kind) {
    if (open_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    for (unsigned i = 0; i < participants_; ++i) {
        cpus_[i]->post_reset(kind, 0);
    }
}

void Ip27Machine::NodeReset::clear() {
    open_.store(false, std::memory_order_relaxed);
    arrived_.store(0, std::memory_order_relaxed);
    latest_.store(0, std::memory_order_relaxed);
}

std::uint64_t Ip27Machine::NodeReset::arrive(mips::Cpu& cpu, std::uint64_t cycles) {
    // After the threads (run_parallel applies what is left posted): at once.
    if (!machine_.parallel_running_ || participants_ == 1) {
        open_.store(false, std::memory_order_release);
        return cycles;
    }
    const std::uint64_t round = round_.load(std::memory_order_acquire);
    std::uint64_t latest = latest_.load(std::memory_order_relaxed);
    while (latest < cycles && !latest_.compare_exchange_weak(latest, cycles)) {
    }
    if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == participants_) {
        released_.store(latest_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        latest_.store(0, std::memory_order_relaxed);
        arrived_.store(0, std::memory_order_relaxed);
        // A reset asked for from here on is a new round.
        open_.store(false, std::memory_order_release);
        round_.store(round + 1, std::memory_order_release);
        return released_.load(std::memory_order_relaxed);
    }
    // Waiting, the CPU is parked. A run that ends meanwhile releases it where it is.
    cpu.park();
    while (round_.load(std::memory_order_acquire) == round) {
        if (machine_.parallel_stop_.load(std::memory_order_relaxed) ||
            machine_.parallel_finished_.load(std::memory_order_relaxed) != 0) {
            cpu.unpark();
            return cycles;
        }
        std::this_thread::yield();
    }
    cpu.unpark();
    return released_.load(std::memory_order_relaxed);
}

std::uint64_t Ip27Machine::lowest_published_other(unsigned index) const {
    std::uint64_t lowest = std::numeric_limits<std::uint64_t>::max();
    for (unsigned i = 0; i < cpu_count_; ++i) {
        if (i != index) {
            lowest = std::min(lowest, installed_[i]->published.load(std::memory_order_acquire));
        }
    }
    return lowest;
}

void Ip27Machine::advance_device_time(std::uint64_t cycles) {
    // Device time is the latest time any CPU has presented: it never moves back, and a CPU
    // behind sees it up to a window ahead of its own (SMP.adoc "S4").
    scheduler_.advance_to(std::max(cpu_time_.time_at(cycles), scheduler_.now()));
}

void Ip27Machine::publish_next_event() {
    std::uint64_t cycle = std::numeric_limits<std::uint64_t>::max();
    if (const auto deadline = scheduler_.next_deadline()) {
        cycle = cycles_at(*deadline, config_.cpu_clock);
        if (cpu_time_.time_at(cycle) < *deadline) {
            ++cycle;
        }
    }
    parallel_next_event_.store(cycle, std::memory_order_release);
}

} // namespace ultraviolent::ip27
