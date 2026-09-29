#include "support/test.hpp"

#include <ultraviolent/devices/uart16550.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace ultraviolent;
using devices::Uart16550;

class Terminal final : public devices::SerialSink, public devices::SerialSource {
  public:
    void serial_transmit(std::uint8_t byte) override {
        output.push_back(static_cast<char>(byte));
    }
    std::optional<std::uint8_t> serial_receive() override {
        if (input.empty()) {
            return std::nullopt;
        }
        const auto byte = static_cast<std::uint8_t>(input.front());
        input.erase(0, 1);
        return byte;
    }
    std::string output;
    std::string input;
};

const test::Registration registers{
    "uart16550.divisor_and_transmit", [](test::Context& t) {
        Uart16550 uart;
        Terminal terminal;
        uart.connect(static_cast<devices::SerialSink&>(terminal));
        // The PROM's set-up: DLAB, divisor, 8N1.
        uart.write(3, 0x80);
        uart.write(0, 0x5f);
        uart.write(1, 0x00);
        uart.write(3, 0x03);
        t.check_equal(uart.read(3), std::uint8_t{0x03});
        uart.write(3, 0x80);
        t.check_equal(uart.read(0), std::uint8_t{0x5f}); // divisor latch, not the receiver
        uart.write(3, 0x03);
        uart.write(0, 'A');
        t.check_equal(terminal.output, std::string{"A"});
        t.check_equal(uart.read(5) & 0x60, 0x60); // THRE, TEMT
    }};

const test::Registration loopback{"uart16550.loopback", [](test::Context& t) {
                                      Uart16550 uart;
                                      Terminal terminal;
                                      uart.connect(static_cast<devices::SerialSink&>(terminal));
                                      uart.write(4, 0x10); // MCR LOOP
                                      uart.write(0, 'x');
                                      t.check(terminal.output.empty(),
                                              "loopback does not transmit");
                                      t.check_equal(uart.read(5) & 1, 1); // data ready
                                      t.check_equal(uart.read(0), std::uint8_t{'x'});
                                      t.check_equal(uart.read(5) & 1, 0);
                                  }};

const test::Registration input{"uart16550.input_on_demand", [](test::Context& t) {
                                   Uart16550 uart;
                                   Terminal terminal;
                                   terminal.input = "ok";
                                   uart.connect(static_cast<devices::SerialSource&>(terminal));
                                   // Reading LSR pulls one byte when the receiver is empty.
                                   t.check_equal(uart.read(5) & 1, 1);
                                   t.check_equal(terminal.input, std::string{"k"});
                                   t.check_equal(uart.read(0), std::uint8_t{'o'});
                                   t.check_equal(uart.read(0), std::uint8_t{'k'});
                                   t.check_equal(uart.read(5) & 1, 0);
                               }};

} // namespace
