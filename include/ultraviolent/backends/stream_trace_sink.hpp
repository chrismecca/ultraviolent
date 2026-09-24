#pragma once

#include <ultraviolent/core/trace.hpp>

#include <cstdio>

namespace ultraviolent::backends {

// Writes trace records as text lines, "<virtual ns> <category> <message>", to a stdio stream
// the caller owns.
class StreamTraceSink final : public TraceSink {
  public:
    explicit StreamTraceSink(std::FILE* stream) : stream_{stream} {}

    void write(const TraceRecord& record) override;

  private:
    std::FILE* stream_;
};

} // namespace ultraviolent::backends
