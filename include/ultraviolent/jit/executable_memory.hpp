#pragma once

// Memory for generated code with W^X (JIT.adoc "Native code: memory, lifetime, calls"): one
// region mapped twice from an anonymous memory file, writable at one address and executable
// at another, so no page is ever writable and executable at once and nothing changes
// protection per block. Allocation is a bump pointer; the only reclamation is discarding
// everything after the permanent prefix (the helper table and entry code) between engine
// invocations. Linux only.

#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraviolent::jit {

class ExecutableMemory {
  public:
    // Code bytes, written through `writable` and executed at `executable`.
    struct Allocation {
        std::span<std::uint8_t> writable;
        std::uint64_t executable{};
    };

    // Reserves `size` bytes (rounded up to pages). Fails with an invariant when the host
    // refuses the mappings: a host configuration error, reported before any guest runs.
    explicit ExecutableMemory(std::size_t size);
    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;
    ~ExecutableMemory();

    // `size` bytes aligned to `alignment`, or an empty allocation when the region is full.
    [[nodiscard]] Allocation allocate(std::size_t size, std::size_t alignment = 16);

    // Everything allocated so far stays for the region's life; later allocations are what
    // discard() drops.
    void make_permanent() {
        permanent_ = used_;
    }
    // Drops every allocation after the permanent prefix. Callers ensure none of that code is
    // executing (only between engine invocations).
    void discard() {
        used_ = permanent_;
    }

    [[nodiscard]] std::size_t used() const {
        return used_;
    }
    [[nodiscard]] std::size_t capacity() const {
        return size_;
    }
    // The executable address of the region's first byte.
    [[nodiscard]] std::uint64_t executable_start() const {
        return reinterpret_cast<std::uint64_t>(executable_);
    }

  private:
    std::size_t size_{};
    std::size_t used_{};
    std::size_t permanent_{};
    int file_{-1};
    std::uint8_t* writable_{};
    std::uint8_t* executable_{};
};

} // namespace ultraviolent::jit
