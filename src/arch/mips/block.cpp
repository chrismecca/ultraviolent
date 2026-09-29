#include <ultraviolent/arch/mips/block.hpp>

#include <ultraviolent/core/invariant.hpp>

namespace ultraviolent::mips {

std::optional<NoBlock> build_block(Cpu& cpu, std::uint64_t pc, Block& block,
                                   std::size_t max_length) {
    invariant(max_length >= 2, "a block holds at least a branch and its delay slot");
    block.operations.clear();
    const auto page = cpu.code_page(pc);
    if (!page) {
        return NoBlock::not_host_backed;
    }
    block.start_pc = pc;
    block.code = *page;

    constexpr std::uint64_t page_size = 0x1000;
    const auto at = [&](std::uint64_t offset) {
        return decode(
            ultraviolent::detail::load_word<std::uint32_t>(page->bytes + offset, page->order));
    };
    auto& operations = block.operations;
    for (std::uint64_t offset = pc & (page_size - 1);; offset += 4) {
        if (operations.size() == max_length) {
            block.end = BlockEnd::length_limit;
            break;
        }
        if (offset == page_size) {
            block.end = BlockEnd::page_boundary;
            break;
        }
        const DecodedInstruction operation = at(offset);
        const OperationClass kind = operation_class(operation.opcode);
        if (kind == OperationClass::unsupported) {
            block.end = BlockEnd::unsupported;
            break;
        }
        if (kind == OperationClass::control) {
            if (offset + 4 == page_size) {
                block.end = BlockEnd::delay_slot;
                break;
            }
            const DecodedInstruction slot = at(offset + 4);
            const OperationClass slot_kind = operation_class(slot.opcode);
            if (slot_kind == OperationClass::control || slot_kind == OperationClass::unsupported) {
                block.end = BlockEnd::delay_slot;
                break;
            }
            if (operations.size() + 2 > max_length) {
                block.end = BlockEnd::length_limit;
                break;
            }
            operations.push_back(operation);
            operations.push_back(slot);
            block.end = BlockEnd::control_flow;
            break;
        }
        operations.push_back(operation);
        if (kind == OperationClass::state_change) {
            block.end = BlockEnd::state_change;
            break;
        }
    }
    if (operations.empty()) {
        return block.end == BlockEnd::unsupported ? NoBlock::unsupported : NoBlock::delay_slot;
    }
    return std::nullopt;
}

} // namespace ultraviolent::mips
