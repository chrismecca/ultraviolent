// Writes check.s (GNU as, Intel syntax) and check.expected (our encoding of each line, in
// hex) for tools/check_x86_64_encodings.sh, which assembles check.s and compares. Every form
// the tier-1 encoder emits, across registers that need REX bits, SIB bytes (rsp, r12), and
// forced displacements (rbp, r13).

#include <ultraviolent/jit/x86_64/assembler.hpp>

#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ultraviolent::jit::x86_64;

namespace {

const char* const names64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                               "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
const char* const names32[] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                               "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
const char* const names8[] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                              "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
const char* const conditions[] = {"o", "no", "b", "ae", "e",  "ne", "be", "a",
                                  "s", "ns", "p", "np", "l", "ge", "le", "g"};

std::string mem(Mem m, const char* size = "QWORD") {
    if (m.displacement == 0) {
        return std::format("{} PTR [{}]", size, names64[static_cast<unsigned>(m.base)]);
    }
    return std::format("{} PTR [{}{:+}]", size, names64[static_cast<unsigned>(m.base)],
                       m.displacement);
}

struct Case {
    std::string text;
    std::function<void(Assembler&)> emit;
};

} // namespace

int main() {
    std::vector<Case> cases;
    const auto add = [&](std::string text, std::function<void(Assembler&)> emit) {
        cases.push_back({std::move(text), std::move(emit)});
    };
    const std::int32_t displacements[] = {0, 8, -8, 0x1000, -0x2000};
    for (unsigned r = 0; r < 16; ++r) {
        const auto reg = static_cast<Reg>(r);
        const auto other = static_cast<Reg>((r * 7 + 3) % 16);
        const char* n = names64[r];
        const char* o = names64[(r * 7 + 3) % 16];
        add(std::format("mov {}, {}", n, o), [=](Assembler& a) { a.mov(reg, other); });
        add(std::format("add {}, {}", n, o), [=](Assembler& a) { a.add(reg, other); });
        add(std::format("sub {}, {}", n, o), [=](Assembler& a) { a.sub(reg, other); });
        add(std::format("and {}, {}", n, o), [=](Assembler& a) { a.and_(reg, other); });
        add(std::format("or {}, {}", n, o), [=](Assembler& a) { a.or_(reg, other); });
        add(std::format("xor {}, {}", n, o), [=](Assembler& a) { a.xor_(reg, other); });
        add(std::format("cmp {}, {}", n, o), [=](Assembler& a) { a.cmp(reg, other); });
        add(std::format("test {}, {}", n, o), [=](Assembler& a) { a.test(reg, other); });
        add(std::format("add {}, {}", names32[r], names32[(r * 7 + 3) % 16]),
            [=](Assembler& a) { a.add32(reg, other); });
        add(std::format("sub {}, {}", names32[r], names32[(r * 7 + 3) % 16]),
            [=](Assembler& a) { a.sub32(reg, other); });
        add(std::format("movsxd {}, {}", n, names32[(r * 7 + 3) % 16]),
            [=](Assembler& a) { a.movsxd(reg, other); });
        add(std::format("movzx {}, {}", names32[r], names8[(r * 7 + 3) % 16]),
            [=](Assembler& a) { a.movzx_byte(reg, other); });
        for (const std::int32_t imm : {1, -1, 127, 128, -129, 0x12345678}) {
            add(std::format("add {}, {}", n, imm), [=](Assembler& a) { a.add(reg, imm); });
            add(std::format("and {}, {}", n, imm), [=](Assembler& a) { a.and_(reg, imm); });
            add(std::format("or {}, {}", n, imm), [=](Assembler& a) { a.or_(reg, imm); });
            add(std::format("xor {}, {}", n, imm), [=](Assembler& a) { a.xor_(reg, imm); });
            add(std::format("cmp {}, {}", n, imm), [=](Assembler& a) { a.cmp(reg, imm); });
            add(std::format("add {}, {}", names32[r], imm), [=](Assembler& a) { a.add32(reg, imm); });
        }
        add(std::format("not {}", n), [=](Assembler& a) { a.not_(reg); });
        for (const std::uint8_t c : std::initializer_list<std::uint8_t>{1, 5, 31, 63}) {
            add(std::format("shl {}, {}", n, c), [=](Assembler& a) { a.shl(reg, c); });
            add(std::format("shr {}, {}", n, c), [=](Assembler& a) { a.shr(reg, c); });
            add(std::format("sar {}, {}", n, c), [=](Assembler& a) { a.sar(reg, c); });
            if (c < 32) {
                add(std::format("shl {}, {}", names32[r], c), [=](Assembler& a) { a.shl32(reg, c); });
                add(std::format("shr {}, {}", names32[r], c), [=](Assembler& a) { a.shr32(reg, c); });
                add(std::format("sar {}, {}", names32[r], c), [=](Assembler& a) { a.sar32(reg, c); });
            }
        }
        add(std::format("shl {}, cl", n), [=](Assembler& a) { a.shl_cl(reg); });
        add(std::format("shr {}, cl", n), [=](Assembler& a) { a.shr_cl(reg); });
        add(std::format("sar {}, cl", n), [=](Assembler& a) { a.sar_cl(reg); });
        add(std::format("shl {}, cl", names32[r]), [=](Assembler& a) { a.shl32_cl(reg); });
        add(std::format("shr {}, cl", names32[r]), [=](Assembler& a) { a.shr32_cl(reg); });
        add(std::format("sar {}, cl", names32[r]), [=](Assembler& a) { a.sar32_cl(reg); });
        for (unsigned c = 0; c < 16; ++c) {
            add(std::format("set{} {}", conditions[c], names8[r]),
                [=](Assembler& a) { a.setcc(static_cast<Cond>(c), reg); });
            add(std::format("cmov{} {}, {}", conditions[c], n, o),
                [=](Assembler& a) { a.cmov(static_cast<Cond>(c), reg, other); });
        }
        add(std::format("push {}", n), [=](Assembler& a) { a.push(reg); });
        add(std::format("pop {}", n), [=](Assembler& a) { a.pop(reg); });
        add(std::format("call {}", n), [=](Assembler& a) { a.call(reg); });
        add(std::format("jmp {}", n), [=](Assembler& a) { a.jmp(reg); });
        add(std::format("mov {}, 0x12345678", names32[r]),
            [=](Assembler& a) { a.mov(reg, std::uint64_t{0x1234'5678}); });
        add(std::format("mov {}, -2147483648", n),
            [=](Assembler& a) { a.mov(reg, std::uint64_t{0xffff'ffff'8000'0000}); });
        add(std::format("movabs {}, 0x123456789abcdef0", n),
            [=](Assembler& a) { a.mov(reg, std::uint64_t{0x1234'5678'9abc'def0}); });
        for (unsigned b = 0; b < 16; ++b) {
            for (const std::int32_t d : displacements) {
                const Mem m{static_cast<Reg>(b), d};
                add(std::format("mov {}, {}", n, mem(m)), [=](Assembler& a) { a.mov(reg, m); });
                add(std::format("mov {}, {}", mem(m), n), [=](Assembler& a) { a.mov(m, reg); });
                add(std::format("lea {}, {}", n, mem(m, "QWORD").substr(10)),
                    [=](Assembler& a) { a.lea(reg, m); });
                add(std::format("cmp {}, {}", n, mem(m)), [=](Assembler& a) { a.cmp(reg, m); });
                add(std::format("movzx {}, {}", names32[r], mem(m, "BYTE")),
                    [=](Assembler& a) { a.movzx_byte(reg, m); });
                add(std::format("mov {}, {}", mem(m, "BYTE"), names8[r]),
                    [=](Assembler& a) { a.mov_byte(m, reg); });
                if (r == 0) {
                    add(std::format("mov {}, 0x5a", mem(m, "BYTE")),
                        [=](Assembler& a) { a.mov_byte(m, 0x5a); });
                    add(std::format("cmp {}, 0x1", mem(m, "BYTE")),
                        [=](Assembler& a) { a.cmp_byte(m, 1); });
                    add(std::format("add {}, 1", mem(m)), [=](Assembler& a) { a.add(m, 1); });
                    add(std::format("add {}, 0x1000", mem(m)),
                        [=](Assembler& a) { a.add(m, 0x1000); });
                }
            }
        }
    }
    add("ret", [](Assembler& a) { a.ret(); });
    add("call QWORD PTR [rip+0x100]", [](Assembler& a) { a.call_indirect(a.here() + 6 + 0x100); });
    add("call QWORD PTR [rip-0x100]", [](Assembler& a) { a.call_indirect(a.here() + 6 - 0x100); });

    std::FILE* source = std::fopen("check.s", "w");
    std::FILE* expected = std::fopen("check.expected", "w");
    if (source == nullptr || expected == nullptr) {
        return 1;
    }
    std::fputs(".intel_syntax noprefix\n.text\n", source);
    for (const Case& c : cases) {
        Assembler a;
        c.emit(a);
        std::string hex;
        for (const std::uint8_t b : a.finish()) {
            hex += std::format("{:02x}", b);
        }
        std::fprintf(source, "%s\n", c.text.c_str());
        std::fprintf(expected, "%s %s\n", hex.c_str(), c.text.c_str());
    }
    std::fclose(source);
    std::fclose(expected);
    std::printf("%zu cases\n", cases.size());
    return 0;
}
