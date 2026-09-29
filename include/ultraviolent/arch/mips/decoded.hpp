#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <array>
#include <cstdint>

namespace ultraviolent::mips {

// One value per operation the R10000 distinguishes (IR.adoc "DecodedInstruction"). Words
// that decode to the same behavior share a value: `reserved` for every reserved encoding,
// `cop0_reserved` for reserved CP0 encodings (which check CP0 usability first), `cop1` and
// `cop1x` for the FPU, which stays opaque (execute_fpu decodes it).
enum class DecodedOpcode : std::uint8_t {
    // SPECIAL
    sll,
    movci, // MOVF, MOVT
    srl,
    sra,
    sllv,
    srlv,
    srav,
    jr,
    jalr,
    movz,
    movn,
    syscall,
    break_,
    sync,
    mfhi,
    mthi,
    mflo,
    mtlo,
    dsllv,
    dsrlv,
    dsrav,
    mult,
    multu,
    div,
    divu,
    dmult,
    dmultu,
    ddiv,
    ddivu,
    add,
    addu,
    sub,
    subu,
    and_,
    or_,
    xor_,
    nor,
    slt,
    sltu,
    dadd,
    daddu,
    dsub,
    dsubu,
    tge,
    tgeu,
    tlt,
    tltu,
    teq,
    tne,
    dsll,
    dsrl,
    dsra,
    dsll32,
    dsrl32,
    dsra32,
    // REGIMM
    bltz,
    bgez,
    bltzl,
    bgezl,
    tgei,
    tgeiu,
    tlti,
    tltiu,
    teqi,
    tnei,
    bltzal,
    bgezal,
    bltzall,
    bgezall,
    // Jumps, branches, immediates
    j,
    jal,
    beq,
    bne,
    blez,
    bgtz,
    addi,
    addiu,
    slti,
    sltiu,
    andi,
    ori,
    xori,
    lui,
    beql,
    bnel,
    blezl,
    bgtzl,
    daddi,
    daddiu,
    // COP0
    mfc0,
    dmfc0,
    mtc0,
    dmtc0,
    tlbr,
    tlbwi,
    tlbwr,
    tlbp,
    eret,
    cop0_reserved,
    // Coprocessors 1 and 2
    cop1,  // COP1, LWC1, LDC1, SWC1, SDC1
    cop1x, // COP1X (MIPS IV)
    cop2,  // COP2, LWC2, LDC2, SWC2, SDC2: coprocessor unusable or reserved
    // Loads and stores
    ldl,
    ldr,
    lb,
    lh,
    lwl,
    lw,
    lbu,
    lhu,
    lwr,
    lwu,
    sb,
    sh,
    swl,
    sw,
    sdl,
    sdr,
    swr,
    cache,
    ll,
    pref,
    lld,
    ld,
    sc,
    scd,
    sd,
    reserved,
};

// An instruction word decoded once: the operation, and the word, whose fields the semantics
// extract as they need them (IR.adoc "DecodedInstruction"). A pure function of the word;
// everything that depends on processor state is checked when it executes.
struct DecodedInstruction {
    DecodedOpcode opcode{DecodedOpcode::reserved};
    Instruction instruction{};
};

namespace detail {

// Operations by index (decode): primary opcode 0-63 (0, 1, and 16 are decoded further),
// SPECIAL function 64-127, REGIMM rt 128-159, COP0 rs below 16 160-175, COP0 function (rs 16
// and up) 176-239. Encodings not listed are reserved.
inline constexpr std::array<DecodedOpcode, 240> operations = {
    DecodedOpcode::reserved,      // opcode 0
    DecodedOpcode::reserved,      // opcode 1
    DecodedOpcode::j,             // opcode 2
    DecodedOpcode::jal,           // opcode 3
    DecodedOpcode::beq,           // opcode 4
    DecodedOpcode::bne,           // opcode 5
    DecodedOpcode::blez,          // opcode 6
    DecodedOpcode::bgtz,          // opcode 7
    DecodedOpcode::addi,          // opcode 8
    DecodedOpcode::addiu,         // opcode 9
    DecodedOpcode::slti,          // opcode 10
    DecodedOpcode::sltiu,         // opcode 11
    DecodedOpcode::andi,          // opcode 12
    DecodedOpcode::ori,           // opcode 13
    DecodedOpcode::xori,          // opcode 14
    DecodedOpcode::lui,           // opcode 15
    DecodedOpcode::reserved,      // opcode 16
    DecodedOpcode::cop1,          // opcode 17
    DecodedOpcode::cop2,          // opcode 18
    DecodedOpcode::cop1x,         // opcode 19
    DecodedOpcode::beql,          // opcode 20
    DecodedOpcode::bnel,          // opcode 21
    DecodedOpcode::blezl,         // opcode 22
    DecodedOpcode::bgtzl,         // opcode 23
    DecodedOpcode::daddi,         // opcode 24
    DecodedOpcode::daddiu,        // opcode 25
    DecodedOpcode::ldl,           // opcode 26
    DecodedOpcode::ldr,           // opcode 27
    DecodedOpcode::reserved,      // opcode 28
    DecodedOpcode::reserved,      // opcode 29
    DecodedOpcode::reserved,      // opcode 30
    DecodedOpcode::reserved,      // opcode 31
    DecodedOpcode::lb,            // opcode 32
    DecodedOpcode::lh,            // opcode 33
    DecodedOpcode::lwl,           // opcode 34
    DecodedOpcode::lw,            // opcode 35
    DecodedOpcode::lbu,           // opcode 36
    DecodedOpcode::lhu,           // opcode 37
    DecodedOpcode::lwr,           // opcode 38
    DecodedOpcode::lwu,           // opcode 39
    DecodedOpcode::sb,            // opcode 40
    DecodedOpcode::sh,            // opcode 41
    DecodedOpcode::swl,           // opcode 42
    DecodedOpcode::sw,            // opcode 43
    DecodedOpcode::sdl,           // opcode 44
    DecodedOpcode::sdr,           // opcode 45
    DecodedOpcode::swr,           // opcode 46
    DecodedOpcode::cache,         // opcode 47
    DecodedOpcode::ll,            // opcode 48
    DecodedOpcode::cop1,          // opcode 49
    DecodedOpcode::cop2,          // opcode 50
    DecodedOpcode::pref,          // opcode 51
    DecodedOpcode::lld,           // opcode 52
    DecodedOpcode::cop1,          // opcode 53
    DecodedOpcode::cop2,          // opcode 54
    DecodedOpcode::ld,            // opcode 55
    DecodedOpcode::sc,            // opcode 56
    DecodedOpcode::cop1,          // opcode 57
    DecodedOpcode::cop2,          // opcode 58
    DecodedOpcode::reserved,      // opcode 59
    DecodedOpcode::scd,           // opcode 60
    DecodedOpcode::cop1,          // opcode 61
    DecodedOpcode::cop2,          // opcode 62
    DecodedOpcode::sd,            // opcode 63
    DecodedOpcode::sll,           // SPECIAL function 0
    DecodedOpcode::movci,         // SPECIAL function 1
    DecodedOpcode::srl,           // SPECIAL function 2
    DecodedOpcode::sra,           // SPECIAL function 3
    DecodedOpcode::sllv,          // SPECIAL function 4
    DecodedOpcode::reserved,      // SPECIAL function 5
    DecodedOpcode::srlv,          // SPECIAL function 6
    DecodedOpcode::srav,          // SPECIAL function 7
    DecodedOpcode::jr,            // SPECIAL function 8
    DecodedOpcode::jalr,          // SPECIAL function 9
    DecodedOpcode::movz,          // SPECIAL function 10
    DecodedOpcode::movn,          // SPECIAL function 11
    DecodedOpcode::syscall,       // SPECIAL function 12
    DecodedOpcode::break_,        // SPECIAL function 13
    DecodedOpcode::reserved,      // SPECIAL function 14
    DecodedOpcode::sync,          // SPECIAL function 15
    DecodedOpcode::mfhi,          // SPECIAL function 16
    DecodedOpcode::mthi,          // SPECIAL function 17
    DecodedOpcode::mflo,          // SPECIAL function 18
    DecodedOpcode::mtlo,          // SPECIAL function 19
    DecodedOpcode::dsllv,         // SPECIAL function 20
    DecodedOpcode::reserved,      // SPECIAL function 21
    DecodedOpcode::dsrlv,         // SPECIAL function 22
    DecodedOpcode::dsrav,         // SPECIAL function 23
    DecodedOpcode::mult,          // SPECIAL function 24
    DecodedOpcode::multu,         // SPECIAL function 25
    DecodedOpcode::div,           // SPECIAL function 26
    DecodedOpcode::divu,          // SPECIAL function 27
    DecodedOpcode::dmult,         // SPECIAL function 28
    DecodedOpcode::dmultu,        // SPECIAL function 29
    DecodedOpcode::ddiv,          // SPECIAL function 30
    DecodedOpcode::ddivu,         // SPECIAL function 31
    DecodedOpcode::add,           // SPECIAL function 32
    DecodedOpcode::addu,          // SPECIAL function 33
    DecodedOpcode::sub,           // SPECIAL function 34
    DecodedOpcode::subu,          // SPECIAL function 35
    DecodedOpcode::and_,          // SPECIAL function 36
    DecodedOpcode::or_,           // SPECIAL function 37
    DecodedOpcode::xor_,          // SPECIAL function 38
    DecodedOpcode::nor,           // SPECIAL function 39
    DecodedOpcode::reserved,      // SPECIAL function 40
    DecodedOpcode::reserved,      // SPECIAL function 41
    DecodedOpcode::slt,           // SPECIAL function 42
    DecodedOpcode::sltu,          // SPECIAL function 43
    DecodedOpcode::dadd,          // SPECIAL function 44
    DecodedOpcode::daddu,         // SPECIAL function 45
    DecodedOpcode::dsub,          // SPECIAL function 46
    DecodedOpcode::dsubu,         // SPECIAL function 47
    DecodedOpcode::tge,           // SPECIAL function 48
    DecodedOpcode::tgeu,          // SPECIAL function 49
    DecodedOpcode::tlt,           // SPECIAL function 50
    DecodedOpcode::tltu,          // SPECIAL function 51
    DecodedOpcode::teq,           // SPECIAL function 52
    DecodedOpcode::reserved,      // SPECIAL function 53
    DecodedOpcode::tne,           // SPECIAL function 54
    DecodedOpcode::reserved,      // SPECIAL function 55
    DecodedOpcode::dsll,          // SPECIAL function 56
    DecodedOpcode::reserved,      // SPECIAL function 57
    DecodedOpcode::dsrl,          // SPECIAL function 58
    DecodedOpcode::dsra,          // SPECIAL function 59
    DecodedOpcode::dsll32,        // SPECIAL function 60
    DecodedOpcode::reserved,      // SPECIAL function 61
    DecodedOpcode::dsrl32,        // SPECIAL function 62
    DecodedOpcode::dsra32,        // SPECIAL function 63
    DecodedOpcode::bltz,          // REGIMM rt 0
    DecodedOpcode::bgez,          // REGIMM rt 1
    DecodedOpcode::bltzl,         // REGIMM rt 2
    DecodedOpcode::bgezl,         // REGIMM rt 3
    DecodedOpcode::reserved,      // REGIMM rt 4
    DecodedOpcode::reserved,      // REGIMM rt 5
    DecodedOpcode::reserved,      // REGIMM rt 6
    DecodedOpcode::reserved,      // REGIMM rt 7
    DecodedOpcode::tgei,          // REGIMM rt 8
    DecodedOpcode::tgeiu,         // REGIMM rt 9
    DecodedOpcode::tlti,          // REGIMM rt 10
    DecodedOpcode::tltiu,         // REGIMM rt 11
    DecodedOpcode::teqi,          // REGIMM rt 12
    DecodedOpcode::reserved,      // REGIMM rt 13
    DecodedOpcode::tnei,          // REGIMM rt 14
    DecodedOpcode::reserved,      // REGIMM rt 15
    DecodedOpcode::bltzal,        // REGIMM rt 16
    DecodedOpcode::bgezal,        // REGIMM rt 17
    DecodedOpcode::bltzall,       // REGIMM rt 18
    DecodedOpcode::bgezall,       // REGIMM rt 19
    DecodedOpcode::reserved,      // REGIMM rt 20
    DecodedOpcode::reserved,      // REGIMM rt 21
    DecodedOpcode::reserved,      // REGIMM rt 22
    DecodedOpcode::reserved,      // REGIMM rt 23
    DecodedOpcode::reserved,      // REGIMM rt 24
    DecodedOpcode::reserved,      // REGIMM rt 25
    DecodedOpcode::reserved,      // REGIMM rt 26
    DecodedOpcode::reserved,      // REGIMM rt 27
    DecodedOpcode::reserved,      // REGIMM rt 28
    DecodedOpcode::reserved,      // REGIMM rt 29
    DecodedOpcode::reserved,      // REGIMM rt 30
    DecodedOpcode::reserved,      // REGIMM rt 31
    DecodedOpcode::mfc0,          // COP0 rs 0
    DecodedOpcode::dmfc0,         // COP0 rs 1
    DecodedOpcode::cop0_reserved, // COP0 rs 2
    DecodedOpcode::cop0_reserved, // COP0 rs 3
    DecodedOpcode::mtc0,          // COP0 rs 4
    DecodedOpcode::dmtc0,         // COP0 rs 5
    DecodedOpcode::cop0_reserved, // COP0 rs 6
    DecodedOpcode::cop0_reserved, // COP0 rs 7
    DecodedOpcode::cop0_reserved, // COP0 rs 8
    DecodedOpcode::cop0_reserved, // COP0 rs 9
    DecodedOpcode::cop0_reserved, // COP0 rs 10
    DecodedOpcode::cop0_reserved, // COP0 rs 11
    DecodedOpcode::cop0_reserved, // COP0 rs 12
    DecodedOpcode::cop0_reserved, // COP0 rs 13
    DecodedOpcode::cop0_reserved, // COP0 rs 14
    DecodedOpcode::cop0_reserved, // COP0 rs 15
    DecodedOpcode::cop0_reserved, // COP0 function 0
    DecodedOpcode::tlbr,          // COP0 function 1
    DecodedOpcode::tlbwi,         // COP0 function 2
    DecodedOpcode::cop0_reserved, // COP0 function 3
    DecodedOpcode::cop0_reserved, // COP0 function 4
    DecodedOpcode::cop0_reserved, // COP0 function 5
    DecodedOpcode::tlbwr,         // COP0 function 6
    DecodedOpcode::cop0_reserved, // COP0 function 7
    DecodedOpcode::tlbp,          // COP0 function 8
    DecodedOpcode::cop0_reserved, // COP0 function 9
    DecodedOpcode::cop0_reserved, // COP0 function 10
    DecodedOpcode::cop0_reserved, // COP0 function 11
    DecodedOpcode::cop0_reserved, // COP0 function 12
    DecodedOpcode::cop0_reserved, // COP0 function 13
    DecodedOpcode::cop0_reserved, // COP0 function 14
    DecodedOpcode::cop0_reserved, // COP0 function 15
    DecodedOpcode::cop0_reserved, // COP0 function 16
    DecodedOpcode::cop0_reserved, // COP0 function 17
    DecodedOpcode::cop0_reserved, // COP0 function 18
    DecodedOpcode::cop0_reserved, // COP0 function 19
    DecodedOpcode::cop0_reserved, // COP0 function 20
    DecodedOpcode::cop0_reserved, // COP0 function 21
    DecodedOpcode::cop0_reserved, // COP0 function 22
    DecodedOpcode::cop0_reserved, // COP0 function 23
    DecodedOpcode::eret,          // COP0 function 24
    DecodedOpcode::cop0_reserved, // COP0 function 25
    DecodedOpcode::cop0_reserved, // COP0 function 26
    DecodedOpcode::cop0_reserved, // COP0 function 27
    DecodedOpcode::cop0_reserved, // COP0 function 28
    DecodedOpcode::cop0_reserved, // COP0 function 29
    DecodedOpcode::cop0_reserved, // COP0 function 30
    DecodedOpcode::cop0_reserved, // COP0 function 31
    DecodedOpcode::cop0_reserved, // COP0 function 32
    DecodedOpcode::cop0_reserved, // COP0 function 33
    DecodedOpcode::cop0_reserved, // COP0 function 34
    DecodedOpcode::cop0_reserved, // COP0 function 35
    DecodedOpcode::cop0_reserved, // COP0 function 36
    DecodedOpcode::cop0_reserved, // COP0 function 37
    DecodedOpcode::cop0_reserved, // COP0 function 38
    DecodedOpcode::cop0_reserved, // COP0 function 39
    DecodedOpcode::cop0_reserved, // COP0 function 40
    DecodedOpcode::cop0_reserved, // COP0 function 41
    DecodedOpcode::cop0_reserved, // COP0 function 42
    DecodedOpcode::cop0_reserved, // COP0 function 43
    DecodedOpcode::cop0_reserved, // COP0 function 44
    DecodedOpcode::cop0_reserved, // COP0 function 45
    DecodedOpcode::cop0_reserved, // COP0 function 46
    DecodedOpcode::cop0_reserved, // COP0 function 47
    DecodedOpcode::cop0_reserved, // COP0 function 48
    DecodedOpcode::cop0_reserved, // COP0 function 49
    DecodedOpcode::cop0_reserved, // COP0 function 50
    DecodedOpcode::cop0_reserved, // COP0 function 51
    DecodedOpcode::cop0_reserved, // COP0 function 52
    DecodedOpcode::cop0_reserved, // COP0 function 53
    DecodedOpcode::cop0_reserved, // COP0 function 54
    DecodedOpcode::cop0_reserved, // COP0 function 55
    DecodedOpcode::cop0_reserved, // COP0 function 56
    DecodedOpcode::cop0_reserved, // COP0 function 57
    DecodedOpcode::cop0_reserved, // COP0 function 58
    DecodedOpcode::cop0_reserved, // COP0 function 59
    DecodedOpcode::cop0_reserved, // COP0 function 60
    DecodedOpcode::cop0_reserved, // COP0 function 61
    DecodedOpcode::cop0_reserved, // COP0 function 62
    DecodedOpcode::cop0_reserved, // COP0 function 63
};

} // namespace detail

inline DecodedInstruction decode(std::uint32_t word) {
    // One table index, selected without branches: measured, branching on the opcode class
    // here added a second data-dependent branch per instruction to execute's dispatch.
    const Instruction i{word};
    const unsigned op = i.opcode();
    unsigned index = op;
    index = op == 0 ? 64 + i.funct() : index;
    index = op == 1 ? 128 + i.rt() : index;
    const unsigned cop0 = i.rs() < 16 ? 160 + i.rs() : 176 + i.funct();
    index = op == 16 ? cop0 : index;
    return {.opcode = detail::operations[index], .instruction = i};
}

// Executes `instruction`, located at `pc`, against the CPU: the MIPS semantics, shared by
// every execution engine. Leaves PC to the caller (apply_flow).
ExecutionResult execute(Cpu& cpu, const DecodedInstruction& instruction, std::uint64_t pc);

// Advances PC and the delay-slot state after an instruction completed with `control`.
inline void apply_flow(IntegerState& s, const Control& control) {
    switch (control.flow) {
    case Flow::sequential:
        s.pc = s.next_pc;
        s.next_pc = s.pc + 4;
        s.delay_slot = false;
        break;
    case Flow::branch:
        // The delay slot executes whether or not the branch is taken.
        s.pc = s.next_pc;
        s.next_pc = control.taken ? control.target : s.pc + 4;
        s.delay_slot = true;
        break;
    case Flow::branch_likely:
        if (control.taken) {
            s.pc = s.next_pc;
            s.next_pc = control.target;
            s.delay_slot = true;
        } else {
            // A branch-likely that is not taken nullifies its delay slot.
            s.pc = s.next_pc + 4;
            s.next_pc = s.pc + 4;
            s.delay_slot = false;
        }
        break;
    case Flow::exception_return:
        // ERET has no delay slot (UM 14.30).
        s.pc = control.target;
        s.next_pc = s.pc + 4;
        s.delay_slot = false;
        break;
    }
}

// The rest of a processor cycle once the instruction at pc is known: executes it and
// accounts the cycle, the one sequence every engine uses after its own way of finding the
// instruction (IR.adoc "Executor"). Returns whether it retired; false when it took an
// exception, which is then taken.
[[gnu::always_inline]] inline bool complete_cycle(Cpu& cpu, const DecodedInstruction& instruction) {
    IntegerState& s = cpu.state();
    auto result = execute(cpu, instruction, s.pc);
    if (!result) {
        cpu.take_exception(result.error());
        cpu.end_cycle(false);
        return false;
    }
    apply_flow(s, *result);
    cpu.end_cycle(true);
    return true;
}

} // namespace ultraviolent::mips
