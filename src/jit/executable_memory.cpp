#include <ultraviolent/jit/executable_memory.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <sys/mman.h>
#include <unistd.h>

namespace ultraviolent::jit {

ExecutableMemory::ExecutableMemory(std::size_t size) {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    size_ = (size + page - 1) / page * page;
    file_ = memfd_create("ultraviolent-code", MFD_CLOEXEC);
    invariant(file_ >= 0, "memfd_create for generated code");
    invariant(ftruncate(file_, static_cast<off_t>(size_)) == 0, "sizing generated-code memory");
    void* w = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, file_, 0);
    void* x = mmap(nullptr, size_, PROT_READ | PROT_EXEC, MAP_SHARED, file_, 0);
    invariant(w != MAP_FAILED && x != MAP_FAILED, "mapping generated-code memory");
    writable_ = static_cast<std::uint8_t*>(w);
    executable_ = static_cast<std::uint8_t*>(x);
}

ExecutableMemory::~ExecutableMemory() {
    munmap(writable_, size_);
    munmap(executable_, size_);
    close(file_);
}

ExecutableMemory::Allocation ExecutableMemory::allocate(std::size_t size, std::size_t alignment) {
    const std::size_t start = (used_ + alignment - 1) / alignment * alignment;
    if (start + size > size_) {
        return {};
    }
    used_ = start + size;
    return {std::span{writable_ + start, size}, reinterpret_cast<std::uint64_t>(executable_ + start)};
}

} // namespace ultraviolent::jit
