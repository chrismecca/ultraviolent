#include <ultraviolent/core/memory_block.hpp>

#include <new>

#include <sys/mman.h>

namespace ultraviolent {

// Anonymous private pages are zero until written and cost nothing until touched (Linux
// mmap(2)); a zero-size block still gets a page so that its storage has an address.
std::unique_ptr<std::byte[], MemoryBlock::Free> MemoryBlock::allocate(std::size_t size) {
    const std::size_t length = size == 0 ? 1 : size;
    void* bytes =
        ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bytes == MAP_FAILED) {
        throw std::bad_alloc{};
    }
    return std::unique_ptr<std::byte[], Free>{static_cast<std::byte*>(bytes), Free{length}};
}

void MemoryBlock::Free::operator()(std::byte* bytes) const {
    ::munmap(bytes, size);
}

} // namespace ultraviolent
