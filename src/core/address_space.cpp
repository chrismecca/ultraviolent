#include <ultraviolent/core/address_space.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <iterator>

namespace ultraviolent {

namespace {

bool fits_width(std::uint64_t value, AccessWidth width) {
    return width == AccessWidth::bits64 || (value >> (8 * byte_count(width))) == 0;
}

} // namespace

std::expected<void, MapError> AddressSpace::map_memory(PhysicalRange range, MemoryBlock& block,
                                                       std::uint64_t block_offset,
                                                       MemoryAccess access,
                                                       MmioTarget* write_target) {
    if (block_offset > block.size() || range.size > block.size() - block_offset) {
        return std::unexpected(MapError::outside_block);
    }
    return insert({range, MemoryMapping{block.bytes().subspan(block_offset, range.size), access,
                                        write_target, &block, block_offset}});
}

std::expected<void, MapError> AddressSpace::remap_memory(PhysicalRange range, MemoryBlock& block,
                                                         std::uint64_t block_offset,
                                                         MemoryAccess access,
                                                         MmioTarget* write_target) {
    if (block_offset > block.size() || range.size > block.size() - block_offset) {
        return std::unexpected(MapError::outside_block);
    }
    return replace({range, MemoryMapping{block.bytes().subspan(block_offset, range.size), access,
                                         write_target, &block, block_offset}});
}

std::expected<void, MapError> AddressSpace::remap_mmio(PhysicalRange range, MmioTarget& target) {
    Ports ports;
    ports.fill(&target);
    return replace({range, ports});
}

std::expected<void, MapError> AddressSpace::replace(Mapping mapping) {
    for (Mapping& existing : mappings_) {
        if (existing.range.base == mapping.range.base &&
            existing.range.size == mapping.range.size) {
            existing = mapping;
            publish();
            return {};
        }
    }
    return std::unexpected(MapError::invalid_range);
}

std::expected<void, MapError> AddressSpace::map_mmio(PhysicalRange range, MmioTarget& target) {
    Ports ports;
    ports.fill(&target);
    return insert({range, ports});
}

std::expected<void, MapError> AddressSpace::map_ports(PhysicalRange range, const Ports& ports) {
    invariant(std::ranges::none_of(ports, [](const MmioTarget* t) { return t == nullptr; }),
              "every requester needs a port");
    return insert({range, ports});
}

std::expected<std::uint64_t, AccessFault> AddressSpace::read(PhysicalAddress address,
                                                             AccessWidth width,
                                                             unsigned requester) {
    const std::uint64_t size = byte_count(width);
    if (address.value % size != 0) {
        return std::unexpected(AccessFault::misaligned);
    }
    const Mapping* mapping = find(address);
    if (mapping == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    const std::uint64_t offset = address - mapping->range.base;
    if (const auto* memory = std::get_if<MemoryMapping>(&mapping->target)) {
        return load_unsigned(memory->bytes.subspan(offset, size), byte_order_);
    }
    auto value = std::get<Ports>(mapping->target)[requester]->mmio_read(offset, width);
    invariant(!value || fits_width(*value, width), "MMIO read value wider than the access");
    return value;
}

std::expected<void, AccessFault> AddressSpace::write(PhysicalAddress address, AccessWidth width,
                                                     std::uint64_t value, unsigned requester) {
    invariant(fits_width(value, width), "write value wider than the access");
    const std::uint64_t size = byte_count(width);
    if (address.value % size != 0) {
        return std::unexpected(AccessFault::misaligned);
    }
    const Mapping* mapping = find(address);
    if (mapping == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    const std::uint64_t offset = address - mapping->range.base;
    if (const auto* memory = std::get_if<MemoryMapping>(&mapping->target)) {
        if (memory->access == MemoryAccess::read_only) {
            if (memory->write_target != nullptr) {
                return memory->write_target->mmio_write(offset, width, value);
            }
            return std::unexpected(AccessFault::read_only);
        }
        if (memory->block->note_write(memory->block_offset + offset, size)) {
            code_writes_->fetch_add(1, std::memory_order_relaxed);
        }
        store_unsigned(memory->bytes.subspan(offset, size), value, byte_order_);
        return {};
    }
    return std::get<Ports>(mapping->target)[requester]->mmio_write(offset, width, value);
}

std::span<const std::byte> AddressSpace::memory_bytes(PhysicalAddress address,
                                                      std::uint64_t size) const {
    const Mapping* mapping = find(address);
    if (mapping == nullptr || size == 0 || !mapping->range.contains(address + (size - 1))) {
        return {};
    }
    const auto* memory = std::get_if<MemoryMapping>(&mapping->target);
    if (memory == nullptr) {
        return {};
    }
    return memory->bytes.subspan(address - mapping->range.base, size);
}

std::span<std::byte> AddressSpace::writable_memory_bytes(PhysicalAddress address,
                                                         std::uint64_t size) {
    const Mapping* mapping = find(address);
    if (mapping == nullptr || size == 0 || !mapping->range.contains(address + (size - 1))) {
        return {};
    }
    const auto* memory = std::get_if<MemoryMapping>(&mapping->target);
    if (memory == nullptr || memory->access != MemoryAccess::read_write) {
        return {};
    }
    const std::uint64_t offset = address - mapping->range.base;
    if (memory->block->note_write(memory->block_offset + offset, size)) {
        code_writes_->fetch_add(1, std::memory_order_relaxed);
    }
    return memory->bytes.subspan(offset, size);
}

std::optional<AddressSpace::MemoryFrame> AddressSpace::memory_frame(PhysicalAddress address) const {
    const Mapping* mapping = find(address);
    if (mapping == nullptr) {
        return std::nullopt;
    }
    const auto* memory = std::get_if<MemoryMapping>(&mapping->target);
    if (memory == nullptr) {
        return std::nullopt;
    }
    return MemoryFrame{memory->block, memory->block_offset + (address - mapping->range.base)};
}

std::optional<AddressSpace::MmioRange>
AddressSpace::mmio_range(PhysicalAddress address, std::uint64_t size, unsigned requester) const {
    const Mapping* mapping = find(address);
    if (mapping == nullptr || size == 0 || !mapping->range.contains(address + (size - 1))) {
        return std::nullopt;
    }
    const auto* ports = std::get_if<Ports>(&mapping->target);
    if (ports == nullptr) {
        return std::nullopt;
    }
    return MmioRange{(*ports)[requester], address - mapping->range.base};
}

void AddressSpace::clear() {
    mappings_.clear();
    publish();
}

void AddressSpace::end_update() {
    invariant(updating_ != 0, "end_update without begin_update");
    if (--updating_ == 0) {
        publish();
    }
}

void AddressSpace::set_shared(bool shared) {
    shared_ = shared;
    publish();
}

void AddressSpace::publish() {
    if (updating_ != 0) {
        return;
    }
    if (shared_) {
        versions_.push_back(std::make_unique<const Mappings>(mappings_));
        current_.store(versions_.back().get(), std::memory_order_release);
    } else {
        current_.store(&mappings_, std::memory_order_release);
    }
    last_hit_.store(0, std::memory_order_relaxed);
    generation_.store(generation_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

const AddressSpace::Mapping* AddressSpace::find(PhysicalAddress address) const {
    const Mappings& mappings = *current_.load(std::memory_order_acquire);
    const std::size_t hint = last_hit_.load(std::memory_order_relaxed);
    if (hint < mappings.size() && mappings[hint].range.contains(address)) {
        return &mappings[hint];
    }
    // First mapping whose base is above the address; the candidate is the one before it.
    auto next = std::ranges::upper_bound(mappings, address, {},
                                         [](const Mapping& m) { return m.range.base; });
    if (next == mappings.begin()) {
        return nullptr;
    }
    const Mapping& candidate = *std::prev(next);
    if (!candidate.range.contains(address)) {
        return nullptr;
    }
    last_hit_.store(static_cast<std::size_t>(std::prev(next) - mappings.begin()),
                    std::memory_order_relaxed);
    return &candidate;
}

std::expected<void, MapError> AddressSpace::insert(Mapping mapping) {
    const PhysicalRange& range = mapping.range;
    if (!range.is_valid()) {
        return std::unexpected(MapError::invalid_range);
    }
    if (range.base.value % mapping_alignment != 0 || range.size % mapping_alignment != 0) {
        return std::unexpected(MapError::misaligned);
    }
    auto next = std::ranges::upper_bound(mappings_, range.base, {},
                                         [](const Mapping& m) { return m.range.base; });
    if (next != mappings_.end() && next->range.overlaps(range)) {
        return std::unexpected(MapError::overlap);
    }
    if (next != mappings_.begin() && std::prev(next)->range.overlaps(range)) {
        return std::unexpected(MapError::overlap);
    }
    mappings_.insert(next, mapping);
    publish();
    return {};
}

} // namespace ultraviolent
