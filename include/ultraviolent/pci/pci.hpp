#pragma once

#include <ultraviolent/core/state_image.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// PCI as the Bridge's devices see it (PCI Local Bus Specification 2.1; IP27.adoc "PCI").
// Configuration registers are 32-bit little-endian dwords written under byte enables; memory
// and I/O space accesses carry a PCI address, a size of 1, 2, or 4 bytes, and a value in the
// PCI byte lanes that address selects.
namespace ultraviolent::pci {

enum class Space : std::uint8_t { memory, io };

class Device {
  public:
    virtual ~Device() = default;

    // Type-0 configuration dword at `reg` (a multiple of 4), function 0.
    virtual std::uint32_t config_read(std::uint8_t reg) = 0;
    // `byte_enable` bit n enables byte n of the dword.
    virtual void config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) = 0;

    // A memory or I/O access the device decodes through its BARs. Returns nothing when the
    // device does not claim the address (a master abort).
    virtual std::optional<std::uint32_t> read(Space space, std::uint64_t address,
                                              unsigned size) = 0;
    virtual bool write(Space space, std::uint64_t address, unsigned size, std::uint32_t value) = 0;

    // PCI RST#: the device returns to its power-on state.
    virtual void pci_reset() {}

  protected:
    Device() = default;
    Device(const Device&) = default;
    Device& operator=(const Device&) = default;
};

// What a bus-master device reaches memory through: its bus bridge's decode of PCI memory space
// for DMA. Bytes are in PCI byte-address order. A transfer nobody decodes fails, which the
// device reports in its own way.
class DmaPort {
  public:
    virtual bool dma_read(std::uint64_t address, std::span<std::byte> bytes) = 0;
    virtual bool dma_write(std::uint64_t address, std::span<const std::byte> bytes) = 0;

  protected:
    DmaPort() = default;
    DmaPort(const DmaPort&) = default;
    DmaPort& operator=(const DmaPort&) = default;
    ~DmaPort() = default;
};

// A type-0 configuration header: identity, command/status, and up to six BARs sized by their
// address masks. Registers not listed read 0 and ignore writes.
class ConfigHeader {
  public:
    struct Bar {
        Space space{Space::memory};
        std::uint32_t size{}; // a power of two; 0 for an unimplemented BAR
    };

    // `status` is the status register's constant part (for example the DEVSEL timing).
    ConfigHeader(std::uint16_t vendor, std::uint16_t device, std::uint16_t status,
                 std::uint32_t class_revision, std::array<Bar, 6> bars, std::uint8_t interrupt_pin);

    [[nodiscard]] std::uint32_t read(std::uint8_t reg) const;
    // PCI RST#: command, BARs, and the other writable fields return to zero.
    void reset();
    void write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable);

    // The BAR decoding `address` in `space`, if the matching decode is enabled in the command
    // register, and the offset within it.
    struct Hit {
        unsigned bar;
        std::uint64_t offset;
    };
    [[nodiscard]] std::optional<Hit> decode(Space space, std::uint64_t address) const;

    void save_state(StateImage& image, const std::string& key) const;
    void load_state(const StateImage& image, const std::string& key);

  private:
    std::uint32_t id_;
    std::uint16_t status_;
    std::uint32_t class_revision_;
    std::array<Bar, 6> bars_;
    std::uint8_t interrupt_pin_;
    std::uint16_t command_{};
    std::array<std::uint32_t, 6> bar_values_{};
    std::uint8_t interrupt_line_{};
    std::uint8_t latency_{};
    std::uint8_t cache_line_{};
};

} // namespace ultraviolent::pci
