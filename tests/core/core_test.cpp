#include <ultraviolent/core/virtual_time.hpp>

#include <cstdlib>

int main() {
    using ultraviolent::VirtualTime;

    constexpr VirtualTime earlier{1};
    constexpr VirtualTime later{2};

    if (!(earlier < later)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
