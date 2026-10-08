#include "semantics.hpp"

namespace ultraviolent::mips {

Control execute(Cpu& cpu, const DecodedInstruction& instruction, std::uint64_t pc) {
    return execute_inline(cpu, instruction, pc);
}

} // namespace ultraviolent::mips
