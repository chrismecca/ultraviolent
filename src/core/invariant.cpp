#include <ultraviolent/core/invariant.hpp>

#include <cstdio>
#include <cstdlib>

namespace ultraviolent {

void invariant_failed(std::string_view message, std::source_location location) {
    std::fprintf(stderr, "ultraviolent: invariant violated: %.*s\n  at %s:%u in %s\n",
                 static_cast<int>(message.size()), message.data(), location.file_name(),
                 static_cast<unsigned>(location.line()), location.function_name());
    std::abort();
}

} // namespace ultraviolent
