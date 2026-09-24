#include <ultraviolent/backends/stream_trace_sink.hpp>

namespace ultraviolent::backends {

void StreamTraceSink::write(const TraceRecord& record) {
    const std::string_view category = trace_category_name(record.category);
    std::fprintf(stream_, "%llu %.*s %.*s\n",
                 static_cast<unsigned long long>(record.time.nanoseconds),
                 static_cast<int>(category.size()), category.data(),
                 static_cast<int>(record.message.size()), record.message.data());
}

} // namespace ultraviolent::backends
