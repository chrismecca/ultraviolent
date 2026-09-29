#pragma once

#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/pci/pci.hpp>
#include <ultraviolent/scsi/scsi.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::devices {

// The QLogic ISP1020-family PCI SCSI controller (IP27.adoc "ISP1020"). Register map, mailbox
// commands, and handshake: the public Linux driver drivers/scsi/qlogicisp.c (2.6.12).
//
// The chip is a RISC processor running firmware the host downloads into its RAM. It is
// modeled at its host interface, not by executing that firmware: the 16-bit registers, the
// mailbox handshake (HCCR, PCI_INTF_STS, the semaphore), RISC RAM, and each mailbox command's
// effect. hypothesis: the downloaded firmware behaves as the Linux driver expects of QLogic's
// ISP1020 firmware: parameter commands, the request and response queues of 64-byte
// little-endian IOCBs (command, continuation, marker; status back), and automatic REQUEST
// SENSE after CHECK CONDITION.
class Isp1020 final : public pci::Device {
  public:
    // RISC RAM, 16-bit words addressed by a 16-bit mailbox value.
    static constexpr std::size_t ram_words = 0x1'0000;
    // hypothesis: a mailbox command completes this long after the host sets the host
    // interrupt.
    static constexpr VirtualDuration command_time{10'000};

    // `name` distinguishes this controller's event and snapshot fields.
    Isp1020(Scheduler& scheduler, Tracer& tracer, std::string name);

    // The Bridge slot port its DMA goes through.
    void connect_dma(pci::DmaPort& port) {
        dma_ = &port;
    }
    // Its PCI INTA wire.
    void connect_interrupt(InterruptSink& sink, std::uint32_t input) {
        interrupt_.connect(sink, input);
    }
    // The SCSI bus it is the initiator on.
    void connect_bus(scsi::Bus& bus) {
        bus_ = &bus;
    }

    std::uint32_t config_read(std::uint8_t reg) override;
    void config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) override;
    std::optional<std::uint32_t> read(pci::Space space, std::uint64_t address,
                                      unsigned size) override;
    bool write(pci::Space space, std::uint64_t address, unsigned size,
               std::uint32_t value) override;

    void pci_reset() override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    std::uint16_t read_register(std::uint32_t offset);
    void write_register(std::uint32_t offset, std::uint16_t value);
    void store_register(std::uint32_t offset, std::uint16_t value);
    // HCCR_RESET and PCI_INTF_CTL ISP_RESET: the RISC restarts in its ROM monitor.
    void reset_risc();
    // Drives INTA: a RISC interrupt with both PCI_INTF_CTL enables set.
    void update_interrupt();
    void host_command(std::uint16_t command);
    void start_command();
    void complete_command();
    // Carries out the command in the incoming mailboxes and fills the outgoing ones.
    void execute();
    // The downloaded firmware's mailbox commands; false when it has no such command.
    bool execute_firmware_command();
    // Services the request queue up to the host's in-pointer (mailbox 4).
    void service_requests();
    void execute_iocb(std::span<const std::byte> entry, std::uint16_t& out);
    void post_response(std::span<const std::byte> entry);
    bool dma_read_words(std::uint64_t pci_address, std::uint16_t ram_address, std::uint32_t count);
    bool dma_write_words(std::uint64_t pci_address, std::uint16_t ram_address, std::uint32_t count);

    Scheduler& scheduler_;
    Tracer& tracer_;
    std::string name_;
    pci::ConfigHeader config_;
    pci::DmaPort* dma_{};
    EventId command_done_;
    EventId queue_work_;
    scsi::Bus* bus_{};
    InterruptLine interrupt_;

    std::uint16_t cfg1_{};
    std::uint16_t interface_control_{};
    bool risc_interrupt_{};
    bool host_interrupt_{};
    bool paused_{};
    std::uint16_t semaphore_{};
    // Mailboxes: the host writes the incoming set and reads the outgoing one.
    std::array<std::uint16_t, 8> mailbox_in_{};
    std::array<std::uint16_t, 8> mailbox_out_{};
    bool firmware_running_{};
    std::uint16_t firmware_start_{};
    // The queues in host memory (INIT_REQ_QUEUE, INIT_RES_QUEUE): entries, PCI address, and
    // the pointer the chip owns (request out, response in).
    struct Queue {
        std::uint16_t length{};
        std::uint64_t address{};
        std::uint16_t pointer{};
    };
    Queue requests_;
    Queue responses_;
    // Firmware parameters, by SET command (low nibble) and first mailbox, as the host set them.
    std::map<std::uint32_t, std::array<std::uint16_t, 8>> parameters_;
    std::vector<std::uint16_t> ram_;
    // Registers not modeled yet: traced storage.
    std::map<std::uint32_t, std::uint16_t> storage_;
};

} // namespace ultraviolent::devices
