#include <ultraviolent/backends/stream_serial_sink.hpp>

namespace ultraviolent::backends {

void StreamSerialSink::serial_transmit(std::uint8_t byte) {
    std::fputc(byte, stream_);
    if (byte == '\n') {
        std::fflush(stream_);
    }
}

} // namespace ultraviolent::backends
