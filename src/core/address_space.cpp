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
                                                       MemoryAccess access) {
    if (block_offset > block.size() || range.size > block.size() - block_offset) {
        return std::unexpected(MapError::outside_block);
    }
    return insert({range, MemoryMapping{block.bytes().subspan(block_offset, range.size), access}});
}

std::expected<void, MapError> AddressSpace::map_mmio(PhysicalRange range, MmioTarget& target) {
    return insert({range, &target});
}

std::expected<std::uint64_t, AccessFault> AddressSpace::read(PhysicalAddress address,
                                                             AccessWidth width) {
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
    auto value = std::get<MmioTarget*>(mapping->target)->mmio_read(offset, width);
    invariant(!value || fits_width(*value, width), "MMIO read value wider than the access");
    return value;
}

std::expected<void, AccessFault> AddressSpace::write(PhysicalAddress address, AccessWidth width,
                                                     std::uint64_t value) {
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
            return std::unexpected(AccessFault::read_only);
        }
        store_unsigned(memory->bytes.subspan(offset, size), value, byte_order_);
        return {};
    }
    return std::get<MmioTarget*>(mapping->target)->mmio_write(offset, width, value);
}

const AddressSpace::Mapping* AddressSpace::find(PhysicalAddress address) const {
    if (last_hit_ < mappings_.size() && mappings_[last_hit_].range.contains(address)) {
        return &mappings_[last_hit_];
    }
    // First mapping whose base is above the address; the candidate is the one before it.
    auto next = std::ranges::upper_bound(mappings_, address, {},
                                         [](const Mapping& m) { return m.range.base; });
    if (next == mappings_.begin()) {
        return nullptr;
    }
    const Mapping& candidate = *std::prev(next);
    if (!candidate.range.contains(address)) {
        return nullptr;
    }
    last_hit_ = static_cast<std::size_t>(std::prev(next) - mappings_.begin());
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
    last_hit_ = 0;
    return {};
}

} // namespace ultraviolent
