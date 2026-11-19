// Host self-tests for the Phase 5 recompiler core (R0, `docs/RECOMP_PLAN.md`): the x86-64
// decoder subset, the reference interpreter's flag rules, function decoding, the PE reader
// and the R0 (d) test vectors. Synthetic code only: every byte sequence below is written for
// this test (no retail code; that runs only in the local `tools/r0_pc_reference.cpp`).
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/r2_rig.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"
#include "wse/recomp/unwind.h"
#include "wse/recomp/x86_decode.h"
#include "wse/recomp/x86_interp.h"
#include "wse/recomp/x86_synth.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

using namespace wse::recomp;

constexpr std::uint64_t kBase = 0x140001000ULL;

// int sum_bytes(const uint8_t* p /*rcx*/, int n /*edx*/)
const std::vector<std::uint8_t> kSumBytes = {
    0x31, 0xc0,              // 00 xor eax, eax
    0x85, 0xd2,              // 02 test edx, edx
    0x7e, 0x0e,              // 04 jle 0x14
    0x44, 0x0f, 0xb6, 0x01,  // 06 movzx r8d, byte [rcx]
    0x44, 0x01, 0xc0,        // 0a add eax, r8d
    0x48, 0xff, 0xc1,        // 0d inc rcx
    0xff, 0xca,              // 10 dec edx
    0x75, 0xf2,              // 12 jne 0x06
    0xc3,                    // 14 ret
};

// int smax(int a /*ecx*/, int b /*edx*/)
const std::vector<std::uint8_t> kSmax = {
    0x89, 0xc8,  // 00 mov eax, ecx
    0x39, 0xd1,  // 02 cmp ecx, edx
    0x7d, 0x02,  // 04 jge 0x08
    0x89, 0xd0,  // 06 mov eax, edx
    0xc3,        // 08 ret
};

// Saves rbx in the home area, pushes/pops it, and returns [rsp+0x10] (the saved copy).
const std::vector<std::uint8_t> kHomeArea = {
    0x48, 0x89, 0x5c, 0x24, 0x08,  // 00 mov [rsp+8], rbx
    0x53,                          // 05 push rbx
    0x48, 0x8b, 0x44, 0x24, 0x10,  // 06 mov rax, [rsp+0x10]
    0x5b,                          // 0b pop rbx
    0xc3,                          // 0c ret
};

std::uint64_t run_fn(const std::vector<std::uint8_t>& code, std::uint64_t rcx, std::uint64_t rdx,
                     std::uint64_t rbx = 0, bool* ok = nullptr) {
    const DecodedFunction fn = decode_function(code.data(), kBase, kBase + code.size());
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    s.rip = kBase;
    s.gpr[static_cast<int>(Reg::rcx)] = rcx;
    s.gpr[static_cast<int>(Reg::rdx)] = rdx;
    s.gpr[static_cast<int>(Reg::rbx)] = rbx;
    Interpreter interp;
    std::string err;
    const bool ran = fn.error.empty() &&
                     interp.run(s, GuestStack::kReturnSentinel,
                                [&](std::uint64_t va) { return fn.at(va); }, 100000, &err);
    if (ok != nullptr) {
        *ok = ran;
    }
    return s.gpr[0];
}

void test_decoder_operands() {
    Insn i{};
    const std::uint8_t mov_home[] = {0x48, 0x89, 0x5c, 0x24, 0x08};
    expect(decode_insn(mov_home, sizeof(mov_home), kBase, i) && i.op == Op::Mov &&
               i.length == 5 && i.dst.kind == Operand::Kind::Mem && i.dst.base == Reg::rsp &&
               i.dst.index == Reg::none && i.dst.disp == 8 && i.dst.size == 8 &&
               i.src.kind == Operand::Kind::Reg && i.src.reg == Reg::rbx,
           "decode mov [rsp+8], rbx (SIB, disp8)");
    const std::uint8_t movsxd[] = {0x49, 0x63, 0xd8};  // movsxd rbx, r8d
    expect(decode_insn(movsxd, sizeof(movsxd), kBase, i) && i.op == Op::Movsx &&
               i.dst.reg == Reg::rbx && i.dst.size == 8 && i.src.reg == Reg::r8 && i.src.size == 4,
           "decode movsxd rbx, r8d");
    const std::uint8_t cmp8[] = {0x41, 0x38, 0x0c, 0x02};  // cmp [r10+rax], cl
    expect(decode_insn(cmp8, sizeof(cmp8), kBase, i) && i.op == Op::Cmp && i.dst.size == 1 &&
               i.dst.base == Reg::r10 && i.dst.index == Reg::rax && i.dst.scale == 1 &&
               i.src.reg == Reg::rcx,
           "decode cmp byte [r10+rax], cl");
    const std::uint8_t or_imm[] = {0x83, 0xc8, 0xff};  // or eax, -1
    expect(decode_insn(or_imm, sizeof(or_imm), kBase, i) && i.op == Op::Or &&
               i.src.kind == Operand::Kind::Imm && i.src.imm == -1 && i.dst.size == 4,
           "decode or eax, imm8 sign-extended");
    const std::uint8_t jl[] = {0x7c, 0xeb};
    expect(decode_insn(jl, sizeof(jl), kBase + 0x13, i) && i.op == Op::Jcc &&
               i.cond == Cond::L && i.target == kBase + 0x13 + 2 - 0x15,
           "decode jl rel8 backwards");
    const std::uint8_t jne32[] = {0x0f, 0x85, 0x10, 0x00, 0x00, 0x00};
    expect(decode_insn(jne32, sizeof(jne32), kBase, i) && i.op == Op::Jcc &&
               i.cond == Cond::NE && i.target == kBase + 6 + 0x10 && i.length == 6,
           "decode jne rel32");
    const std::uint8_t nop[] = {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect(decode_insn(nop, sizeof(nop), kBase, i) && i.op == Op::Nop && i.length == 8,
           "decode 8-byte nop");
    const std::uint8_t rip_lea[] = {0x48, 0x8d, 0x05, 0x10, 0x00, 0x00, 0x00};
    expect(decode_insn(rip_lea, sizeof(rip_lea), kBase, i) && i.op == Op::Lea &&
               i.src.base == Reg::rip &&
               Interpreter::address(CpuState{}, i, i.src) == kBase + 7 + 0x10,
           "decode lea rax, [rip+disp32] and resolve it");
    const std::uint8_t movabs[] = {0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8};
    expect(decode_insn(movabs, sizeof(movabs), kBase, i) && i.op == Op::Mov &&
               static_cast<std::uint64_t>(i.src.imm) == 0x0807060504030201ULL && i.length == 10,
           "decode movabs rax, imm64");

    std::string err;
    const std::uint8_t call[] = {0xe8, 0, 0, 0, 0};
    expect(!decode_insn(call, sizeof(call), kBase, i, &err) &&
               err.find("unsupported opcode") != std::string::npos &&
               err.find("e8") != std::string::npos,
           "decode rejects call with the opcode bytes in the message");
    const std::uint8_t lock[] = {0xf0, 0x0f, 0xb1, 0x0a};
    expect(!decode_insn(lock, sizeof(lock), kBase, i, &err), "decode rejects lock prefix");
    const std::uint8_t ah[] = {0x88, 0xe0};  // mov al, ah
    expect(!decode_insn(ah, sizeof(ah), kBase, i, &err), "decode rejects AH byte register");
    const std::uint8_t truncated[] = {0x48, 0x8b};
    expect(!decode_insn(truncated, sizeof(truncated), kBase, i, &err),
           "decode rejects a truncated instruction");
}

void test_interpreter_flags() {
    CpuState s;
    Insn i{};
    // add eax, ecx with signed overflow: 0x7fffffff + 1.
    const std::uint8_t add[] = {0x01, 0xc8};
    (void)decode_insn(add, sizeof(add), kBase, i);
    s.gpr[0] = 0xFFFFFFFF7FFFFFFFULL;
    s.gpr[1] = 1;
    Interpreter::execute(s, i);
    expect(s.gpr[0] == 0x80000000ULL && s.of && s.sf && !s.cf && !s.zf,
           "add: 32-bit result zero-extends, OF/SF set, CF clear");
    // sub eax, ecx with borrow: 0 - 1.
    const std::uint8_t sub[] = {0x29, 0xc8};
    (void)decode_insn(sub, sizeof(sub), kBase, i);
    s.gpr[0] = 0;
    s.gpr[1] = 1;
    Interpreter::execute(s, i);
    expect(s.gpr[0] == 0xFFFFFFFFULL && s.cf && s.sf && !s.of && !s.zf && s.pf,
           "sub: borrow sets CF, result 0xffffffff, PF of 0xff");
    // inc keeps CF and flags OF at INT_MAX.
    s.cf = true;
    const std::uint8_t inc[] = {0xff, 0xc0};
    (void)decode_insn(inc, sizeof(inc), kBase, i);
    s.gpr[0] = 0x7FFFFFFF;
    Interpreter::execute(s, i);
    expect(s.cf && s.of && s.gpr[0] == 0x80000000ULL, "inc: CF unchanged, OF at INT_MAX");
    // 8-bit write merges into the low byte only.
    const std::uint8_t mov8[] = {0x88, 0xc8};  // mov al, cl
    (void)decode_insn(mov8, sizeof(mov8), kBase, i);
    s.gpr[0] = 0x1122334455667788ULL;
    s.gpr[1] = 0xAB;
    Interpreter::execute(s, i);
    expect(s.gpr[0] == 0x11223344556677ABULL, "mov al, cl merges the low byte");
    // Signed conditions after cmp.
    const std::uint8_t cmp[] = {0x39, 0xd1};  // cmp ecx, edx
    (void)decode_insn(cmp, sizeof(cmp), kBase, i);
    s.gpr[1] = static_cast<std::uint32_t>(-5);
    s.gpr[2] = 3;
    Interpreter::execute(s, i);
    expect(eval_cond(s, Cond::L) && !eval_cond(s, Cond::GE) && eval_cond(s, Cond::LE) &&
               !eval_cond(s, Cond::B) && eval_cond(s, Cond::A),
           "cmp -5, 3: signed less, unsigned above");
}

void test_function_decode_and_run() {
    const DecodedFunction fn = decode_function(kSumBytes.data(), kBase, kBase + kSumBytes.size());
    expect(fn.error.empty() && fn.insns.size() == 9 && fn.block_starts.size() == 3 &&
               fn.block_starts[0] == kBase && fn.block_starts[1] == kBase + 6 &&
               fn.block_starts[2] == kBase + 0x14,
           "decode_function finds the three blocks of a counted loop");
    const std::uint8_t data[] = {1, 2, 3, 250, 7};
    bool ok = false;
    const std::uint64_t sum =
        run_fn(kSumBytes, reinterpret_cast<std::uintptr_t>(data), sizeof(data), 0, &ok);
    expect(ok && sum == 263, "interpreter runs sum_bytes over host memory");
    expect(run_fn(kSumBytes, 0, 0, 0, &ok) == 0 && ok, "sum_bytes with n = 0 skips the loop");

    expect(run_fn(kSmax, static_cast<std::uint32_t>(-7), 4) == 4 &&
               run_fn(kSmax, 9, static_cast<std::uint32_t>(-2)) == 9,
           "smax picks the signed maximum");
    expect(run_fn(kHomeArea, 0, 0, 0x1234567890ULL, &ok) == 0x1234567890ULL && ok,
           "home area store, push/pop and ret through the guest stack");

    // The R0 (c) synthetic function agrees with a plain C++ model of its blocks.
    const std::size_t blocks = 64;
    const std::vector<std::uint8_t> synth = make_synthetic_function(blocks);
    const DecodedFunction sfn = decode_function(synth.data(), kBase, kBase + synth.size());
    std::vector<std::uint8_t> buf(256);
    for (std::size_t k = 0; k < buf.size(); ++k) {
        buf[k] = static_cast<std::uint8_t>(k * 37U + 11U);
    }
    std::uint64_t rax = 0;
    std::uint64_t rdx = 0x1234;
    for (std::size_t b = 0; b < blocks; ++b) {
        std::uint64_t v = 0;
        std::memcpy(&v, buf.data() + ((b * 8U) & 0x78U), 8);
        rax += v;
        rdx ^= rax;
        rdx += 1;
        if (!(static_cast<std::int64_t>(rax) < static_cast<std::int64_t>(rdx))) {
            rax += 1;
        }
        rax -= rdx;
    }
    expect(sfn.error.empty() && sfn.insns.size() == 1 + blocks * 7 + 1 &&
               run_fn(synth, reinterpret_cast<std::uintptr_t>(buf.data()), 0x1234, 0, &ok) == rax && ok,
           "synthetic R0 (c) function decodes and matches its C++ model");

    std::vector<std::uint8_t> tail = {0xe9, 0x00, 0x10, 0x00, 0x00};  // jmp outside
    const DecodedFunction bad = decode_function(tail.data(), kBase, kBase + tail.size());
    expect(!bad.error.empty(), "decode_function reports a tail call out of range");
}

void put16(std::vector<std::uint8_t>& f, std::size_t off, std::uint16_t v) {
    std::memcpy(f.data() + off, &v, 2);
}
void put32(std::vector<std::uint8_t>& f, std::size_t off, std::uint32_t v) {
    std::memcpy(f.data() + off, &v, 4);
}
void put64(std::vector<std::uint8_t>& f, std::size_t off, std::uint64_t v) {
    std::memcpy(f.data() + off, &v, 8);
}

std::vector<std::uint8_t> make_base_synthetic_pe() {
    std::vector<std::uint8_t> f(0x600, 0);
    put16(f, 0, 0x5A4D);
    put32(f, 0x3C, 0x40);
    put32(f, 0x40, 0x00004550);
    put16(f, 0x44, 0x8664);
    put16(f, 0x46, 2);
    put16(f, 0x54, 0xF0);
    const std::size_t opt = 0x58;
    put16(f, opt, 0x20B);
    put32(f, opt + 16, 0x1000);
    put64(f, opt + 24, 0x140000000ULL);
    put32(f, opt + 32, 0x1000);
    put32(f, opt + 36, 0x200);
    put32(f, opt + 56, 0x3000);
    put32(f, opt + 60, 0x200);
    put32(f, opt + 108, 16);
    put32(f, opt + 112 + 3 * 8, 0x2000);
    put32(f, opt + 112 + 3 * 8 + 4, 12);
    const std::size_t sec = opt + 0xF0;
    std::memcpy(f.data() + sec, ".text", 5);
    put32(f, sec + 8, static_cast<std::uint32_t>(kSmax.size()));
    put32(f, sec + 12, 0x1000);
    put32(f, sec + 16, 0x200);
    put32(f, sec + 20, 0x200);
    put32(f, sec + 36, 0x60000020);
    std::memcpy(f.data() + sec + 40, ".pdata", 6);
    put32(f, sec + 40 + 8, 12);
    put32(f, sec + 40 + 12, 0x2000);
    put32(f, sec + 40 + 16, 0x200);
    put32(f, sec + 40 + 20, 0x400);
    std::memcpy(f.data() + 0x200, kSmax.data(), kSmax.size());
    put32(f, 0x400, 0x1000);
    put32(f, 0x404, static_cast<std::uint32_t>(0x1000 + kSmax.size()));
    put32(f, 0x408, 0);
    return f;
}

void test_pe_reader() {
    const std::vector<std::uint8_t> f = make_base_synthetic_pe();

    PeImage img;
    std::string err;
    const bool parsed = parse_pe(f.data(), f.size(), img, &err);
    expect(parsed && img.image_base == 0x140000000ULL && img.size_of_image == 0x3000 &&
               img.sections.size() == 2 && img.sections[0].name == ".text" &&
               img.functions.size() == 1 && img.functions[0].begin == 0x1000,
           "parse_pe reads base, sections and .pdata of a synthetic PE32+");
    std::vector<std::uint8_t> mapped(img.size_of_image);
    expect(parsed && map_pe_sections(f.data(), f.size(), img, mapped.data()) &&
               std::memcmp(mapped.data() + 0x1000, kSmax.data(), kSmax.size()) == 0 &&
               mapped[0x1000 + kSmax.size()] == 0,
           "map_pe_sections copies .text to its RVA and zero-fills the tail");
    if (parsed) {
        const PeFunctionRange& r = img.functions[0];
        const DecodedFunction fn = decode_function(mapped.data() + r.begin,
                                                   img.image_base + r.begin, img.image_base + r.end);
        expect(fn.error.empty() && fn.insns.size() == 5, "decode the .pdata function");
    }

    std::vector<std::uint8_t> trunc(f.begin(), f.begin() + 0x100);
    expect(!parse_pe(trunc.data(), trunc.size(), img, &err), "parse_pe rejects a truncated file");
    std::vector<std::uint8_t> i386 = f;
    put16(i386, 0x44, 0x14c);
    expect(!parse_pe(i386.data(), i386.size(), img, &err) &&
               err.find("x86-64") != std::string::npos,
           "parse_pe rejects a non-x86-64 image");
}

void test_r0_vectors() {
    const std::vector<R0IndexOfCase> a = make_r0_index_of_cases();
    const std::vector<R0IndexOfCase> b = make_r0_index_of_cases();
    bool same = a.size() == kR0CaseCount && b.size() == kR0CaseCount;
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = a[i].haystack == b[i].haystack && a[i].needle == b[i].needle &&
               a[i].start == b[i].start;
    }
    bool in_range = true;
    for (const R0IndexOfCase& c : a) {
        in_range = in_range && c.start >= 0 &&
                   static_cast<std::size_t>(c.start) <= c.haystack.size();
    }
    expect(same && in_range, "R0 indexOf cases are deterministic and keep start in [0, len]");
    expect(r0_index_of_model(a[0]) == 0 && r0_index_of_model(a[1]) == 1 &&
               r0_index_of_model(a[2]) == -1 && r0_index_of_model(a[3]) == -1 &&
               r0_index_of_model(a[4]) == 4 && r0_index_of_model(a[6]) == -1 &&
               r0_index_of_model(a[7]) == 2,
           "R0 indexOf model edge cases");
    RglStringFake s("hello");
    std::int32_t len = 0;
    char* buf = nullptr;
    std::memcpy(&len, s.bytes + kRglStringLengthOffset, 4);
    std::memcpy(&buf, s.bytes + kRglStringBufferOffset, sizeof(buf));
    expect(len == 5 && buf != nullptr && std::strcmp(buf, "hello") == 0,
           "RglStringFake lays out m_buffer and m_strLength");
    expect(fnv1a_results({}) == 0xcbf29ce484222325ULL && fnv1a_results({0}) != fnv1a_results({1}),
           "fnv1a_results");
}

bool is_reg_eq(const Operand& o, std::uint8_t size, Reg reg) noexcept {
    return o.kind == Operand::Kind::Reg && o.size == size && o.reg == reg;
}

bool is_mem_eq(const Operand& o, std::uint8_t size, Reg base, Reg index, std::uint8_t scale,
               std::int32_t disp) noexcept {
    return o.kind == Operand::Kind::Mem && o.size == size && o.base == base &&
           o.index == index && o.scale == scale && o.disp == disp;
}

bool is_imm_eq(const Operand& o, std::uint8_t size, std::int64_t imm) noexcept {
    return o.kind == Operand::Kind::Imm && o.size == size && o.imm == imm;
}

bool decode_vec(const std::vector<std::uint8_t>& bytes, Insn& out, std::uint64_t va = kBase,
                std::string* err = nullptr) {
    return decode_insn(bytes.data(), bytes.size(), va, out, err);
}

void test_decode_alu_forms() {
    struct AluSpec {
        std::uint8_t base;
        Op op;
    };
    static constexpr std::array<AluSpec, 6> kAlus = {{
        {0x00, Op::Add},
        {0x08, Op::Or},
        {0x20, Op::And},
        {0x28, Op::Sub},
        {0x30, Op::Xor},
        {0x38, Op::Cmp},
    }};

    Insn insn{};
    for (const AluSpec& s : kAlus) {
        const std::uint8_t b0 = s.base;
        const std::uint8_t b1 = static_cast<std::uint8_t>(s.base + 1U);
        const std::uint8_t b2 = static_cast<std::uint8_t>(s.base + 2U);
        const std::uint8_t b3 = static_cast<std::uint8_t>(s.base + 3U);
        const std::uint8_t b4 = static_cast<std::uint8_t>(s.base + 4U);
        const std::uint8_t b5 = static_cast<std::uint8_t>(s.base + 5U);

        // Form 0: r/m8, r8 (ModRM 0xCA: reg=rcx, rm=rdx; 0x0B: reg=rcx, rm=[rbx]).
        expect(decode_vec({b0, 0xCA}, insn) && insn.op == s.op && insn.length == 2 &&
                   is_reg_eq(insn.dst, 1, Reg::rdx) && is_reg_eq(insn.src, 1, Reg::rcx),
               "ALU form 0 (r/m8, r8) without REX");
        expect(decode_vec({0x48, b0, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::rdx) && is_reg_eq(insn.src, 1, Reg::rcx),
               "ALU form 0 keeps 8-bit size with REX.W");
        expect(decode_vec({0x45, b0, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::r10) && is_reg_eq(insn.src, 1, Reg::r9),
               "ALU form 0 with REX.R/REX.B reaches r8..r15");
        expect(decode_vec({0x45, b0, 0x0B}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_mem_eq(insn.dst, 1, Reg::r11, Reg::none, 1, 0) &&
                   is_reg_eq(insn.src, 1, Reg::r9),
               "ALU form 0 memory destination with REX.R/REX.B");

        // Form 1: r/m, r.
        expect(decode_vec({b1, 0xCA}, insn) && insn.op == s.op && insn.length == 2 &&
                   is_reg_eq(insn.dst, 4, Reg::rdx) && is_reg_eq(insn.src, 4, Reg::rcx),
               "ALU form 1 (r/m32, r32) without REX.W");
        expect(decode_vec({0x48, b1, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 8, Reg::rdx) && is_reg_eq(insn.src, 8, Reg::rcx),
               "ALU form 1 (r/m64, r64) with REX.W");
        expect(decode_vec({0x45, b1, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 4, Reg::r10) && is_reg_eq(insn.src, 4, Reg::r9),
               "ALU form 1 32-bit with REX.R/REX.B");
        expect(decode_vec({0x4D, b1, 0x0B}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_mem_eq(insn.dst, 8, Reg::r11, Reg::none, 1, 0) &&
                   is_reg_eq(insn.src, 8, Reg::r9),
               "ALU form 1 64-bit memory destination with REX.W/R/B");

        // Form 2: r8, r/m8.
        expect(decode_vec({b2, 0xCA}, insn) && insn.op == s.op && insn.length == 2 &&
                   is_reg_eq(insn.dst, 1, Reg::rcx) && is_reg_eq(insn.src, 1, Reg::rdx),
               "ALU form 2 (r8, r/m8) without REX");
        expect(decode_vec({0x48, b2, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::rcx) && is_reg_eq(insn.src, 1, Reg::rdx),
               "ALU form 2 keeps 8-bit size with REX.W");
        expect(decode_vec({0x45, b2, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::r9) && is_reg_eq(insn.src, 1, Reg::r10),
               "ALU form 2 with REX.R/REX.B reaches r8..r15");
        expect(decode_vec({0x45, b2, 0x0B}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::r9) &&
                   is_mem_eq(insn.src, 1, Reg::r11, Reg::none, 1, 0),
               "ALU form 2 memory source with REX.R/REX.B");

        // Form 3: r, r/m.
        expect(decode_vec({b3, 0xCA}, insn) && insn.op == s.op && insn.length == 2 &&
                   is_reg_eq(insn.dst, 4, Reg::rcx) && is_reg_eq(insn.src, 4, Reg::rdx),
               "ALU form 3 (r32, r/m32) without REX.W");
        expect(decode_vec({0x48, b3, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 8, Reg::rcx) && is_reg_eq(insn.src, 8, Reg::rdx),
               "ALU form 3 (r64, r/m64) with REX.W");
        expect(decode_vec({0x45, b3, 0xCA}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 4, Reg::r9) && is_reg_eq(insn.src, 4, Reg::r10),
               "ALU form 3 32-bit with REX.R/REX.B");
        expect(decode_vec({0x4D, b3, 0x0B}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 8, Reg::r9) &&
                   is_mem_eq(insn.src, 8, Reg::r11, Reg::none, 1, 0),
               "ALU form 3 64-bit memory source with REX.W/R/B");

        // Form 4: AL, imm8.
        expect(decode_vec({b4, 0x80}, insn) && insn.op == s.op && insn.length == 2 &&
                   is_reg_eq(insn.dst, 1, Reg::rax) && is_imm_eq(insn.src, 1, -128),
               "ALU form 4 (AL, imm8)");
        expect(decode_vec({0x48, b4, 0x7F}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::rax) && is_imm_eq(insn.src, 1, 127),
               "ALU form 4 keeps AL/imm8 with REX.W");
        expect(decode_vec({0x41, b4, 0xFE}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::rax) && is_imm_eq(insn.src, 1, -2),
               "ALU form 4 ignores REX.B on fixed AL");

        // Form 5: eAX/rAX, imm32.
        const std::int32_t imm32_neg = static_cast<std::int32_t>(0x82345678U);
        expect(decode_vec({b5, 0x78, 0x56, 0x34, 0x82}, insn) && insn.op == s.op &&
                   insn.length == 5 && is_reg_eq(insn.dst, 4, Reg::rax) &&
                   is_imm_eq(insn.src, 4, imm32_neg),
               "ALU form 5 (EAX, imm32) without REX.W");
        expect(decode_vec({0x48, b5, 0x78, 0x56, 0x34, 0x82}, insn) && insn.op == s.op &&
                   insn.length == 6 && is_reg_eq(insn.dst, 8, Reg::rax) &&
                   is_imm_eq(insn.src, 8, static_cast<std::int64_t>(imm32_neg)),
               "ALU form 5 (RAX, sign-extended imm32) with REX.W");
        expect(decode_vec({0x41, b5, 0x01, 0x00, 0x00, 0x00}, insn) && insn.op == s.op &&
                   insn.length == 6 && is_reg_eq(insn.dst, 4, Reg::rax) &&
                   is_imm_eq(insn.src, 4, 1),
               "ALU form 5 ignores REX.B on fixed EAX");
    }

    // adc (10..15) and sbb (18..1D), plus invalid gaps 16/17/1E/1F, must be rejected.
    static constexpr std::uint8_t kAdcSbb[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
    };
    for (const std::uint8_t op : kAdcSbb) {
        std::string err;
        expect(!decode_vec({op, 0xC0, 0, 0, 0, 0}, insn, kBase, &err) &&
                   err.find("adc/sbb") != std::string::npos,
               "adc/sbb rejected without REX");
        err.clear();
        expect(!decode_vec({0x48, op, 0xC0, 0, 0, 0, 0}, insn, kBase, &err) &&
                   err.find("adc/sbb") != std::string::npos,
               "adc/sbb rejected with REX.W");
    }
    static constexpr std::uint8_t kBadGaps[] = {0x16, 0x17, 0x1E, 0x1F};
    for (const std::uint8_t op : kBadGaps) {
        expect(!decode_vec({op, 0xC0, 0, 0, 0, 0}, insn), "unsupported 16/17/1E/1F rejected");
    }
}

void test_decode_group1() {
    struct Grp1Spec {
        std::uint8_t ext;
        Op op;
    };
    static constexpr std::array<Grp1Spec, 6> kGrp1 = {{
        {0, Op::Add},
        {1, Op::Or},
        {4, Op::And},
        {5, Op::Sub},
        {6, Op::Xor},
        {7, Op::Cmp},
    }};

    Insn insn{};
    const std::int32_t imm32_neg = static_cast<std::int32_t>(0x84332211U);
    for (const Grp1Spec& s : kGrp1) {
        const std::uint8_t modrm_reg = static_cast<std::uint8_t>(0xC3U | (s.ext << 3U));
        const std::uint8_t modrm_mem = static_cast<std::uint8_t>(0x01U | (s.ext << 3U));

        // 80 /n: 8-bit imm8.
        expect(decode_vec({0x80, modrm_reg, 0xFE}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 1, Reg::rbx) && is_imm_eq(insn.src, 1, -2),
               "Group 1 80 /n reg, imm8");
        expect(decode_vec({0x49, 0x80, modrm_reg, 0x7F}, insn) && insn.op == s.op &&
                   insn.length == 4 && is_reg_eq(insn.dst, 1, Reg::r11) &&
                   is_imm_eq(insn.src, 1, 127),
               "Group 1 80 /n REX.W|B r11b, imm8");
        expect(decode_vec({0x80, modrm_mem, 0x80}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_mem_eq(insn.dst, 1, Reg::rcx, Reg::none, 1, 0) &&
                   is_imm_eq(insn.src, 1, -128),
               "Group 1 80 /n mem8, imm8");

        // 81 /n: 32/64-bit imm32.
        expect(decode_vec({0x81, modrm_reg, 0x11, 0x22, 0x33, 0x84}, insn) && insn.op == s.op &&
                   insn.length == 6 && is_reg_eq(insn.dst, 4, Reg::rbx) &&
                   is_imm_eq(insn.src, 4, imm32_neg),
               "Group 1 81 /n r32, imm32");
        expect(decode_vec({0x48, 0x81, modrm_reg, 0x11, 0x22, 0x33, 0x84}, insn) &&
                   insn.op == s.op && insn.length == 7 && is_reg_eq(insn.dst, 8, Reg::rbx) &&
                   is_imm_eq(insn.src, 8, static_cast<std::int64_t>(imm32_neg)),
               "Group 1 81 /n r64, sign-extended imm32");
        expect(decode_vec({0x4D, 0x81, modrm_mem, 0x04, 0x03, 0x02, 0x01}, insn) &&
                   insn.op == s.op && insn.length == 7 &&
                   is_mem_eq(insn.dst, 8, Reg::r9, Reg::none, 1, 0) &&
                   is_imm_eq(insn.src, 8, 0x01020304),
               "Group 1 81 /n ignores REX.R for /ext and decodes [r9]");

        // 83 /n: 32/64-bit sign-extended imm8.
        expect(decode_vec({0x83, modrm_reg, 0x80}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 4, Reg::rbx) && is_imm_eq(insn.src, 4, -128),
               "Group 1 83 /n r32, negative imm8 sign-extended");
        expect(decode_vec({0x83, modrm_reg, 0x7F}, insn) && insn.op == s.op && insn.length == 3 &&
                   is_reg_eq(insn.dst, 4, Reg::rbx) && is_imm_eq(insn.src, 4, 127),
               "Group 1 83 /n r32, positive imm8");
        expect(decode_vec({0x48, 0x83, modrm_reg, 0xFC}, insn) && insn.op == s.op &&
                   insn.length == 4 && is_reg_eq(insn.dst, 8, Reg::rbx) &&
                   is_imm_eq(insn.src, 8, -4),
               "Group 1 83 /n r64, negative imm8 sign-extended");
        expect(decode_vec({0x49, 0x83, modrm_mem, 0xF8}, insn) && insn.op == s.op &&
                   insn.length == 4 && is_mem_eq(insn.dst, 8, Reg::r9, Reg::none, 1, 0) &&
                   is_imm_eq(insn.src, 8, -8),
               "Group 1 83 /n qword [r9], -8");
    }

    // /2 (adc) and /3 (sbb) rejected across 80, 81, 83; opcode 82 invalid in 64-bit mode.
    static constexpr std::uint8_t kGrp1Ops[] = {0x80, 0x81, 0x83};
    static constexpr std::uint8_t kBadExts[] = {2, 3};
    for (const std::uint8_t op : kGrp1Ops) {
        for (const std::uint8_t ext : kBadExts) {
            const std::uint8_t modrm = static_cast<std::uint8_t>(0xC0U | (ext << 3U));
            expect(!decode_vec({op, modrm, 0, 0, 0, 0}, insn),
                   "Group 1 /2 and /3 rejected without REX");
            expect(!decode_vec({0x48, op, modrm, 0, 0, 0, 0}, insn),
                   "Group 1 /2 and /3 rejected with REX.W");
        }
    }
    expect(!decode_vec({0x82, 0xC0, 0}, insn), "opcode 0x82 rejected in 64-bit mode");
}

void test_decode_modrm_sib() {
    Insn insn{};

    // [rsp] and [r12] require a SIB byte (rm=4).
    expect(decode_vec({0x48, 0x8B, 0x04, 0x24}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::rsp, Reg::none, 1, 0),
           "ModRM/SIB [rsp]");
    expect(decode_vec({0x49, 0x8B, 0x04, 0x24}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::r12, Reg::none, 1, 0),
           "ModRM/SIB [r12] with REX.B");
    expect(!decode_vec({0x48, 0x8B, 0x04}, insn), "ModRM rm=4 without SIB byte is rejected");

    // mod=00 rm=101 is RIP-relative, even when REX.B=1; mod=01/10 rm=101 selects rbp/r13.
    expect(decode_vec({0x48, 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12}, insn) && insn.length == 7 &&
               is_mem_eq(insn.src, 8, Reg::rip, Reg::none, 1, 0x12345678) &&
               Interpreter::address(CpuState{}, insn, insn.src) == kBase + 7 + 0x12345678ULL,
           "mod=00 rm=101 is RIP-relative (+disp32)");
    expect(decode_vec({0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF}, insn) && insn.length == 7 &&
               is_mem_eq(insn.src, 8, Reg::rip, Reg::none, 1, -16) &&
               Interpreter::address(CpuState{}, insn, insn.src) == kBase + 7 - 16,
           "mod=00 rm=101 negative disp32");
    expect(decode_vec({0x49, 0x8B, 0x05, 0x20, 0x00, 0x00, 0x00}, insn) && insn.length == 7 &&
               is_mem_eq(insn.src, 8, Reg::rip, Reg::none, 1, 0x20),
           "mod=00 rm=101 ignores REX.B (still RIP-relative, not r13)");
    expect(decode_vec({0x48, 0x8B, 0x45, 0xF8}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::rbp, Reg::none, 1, -8),
           "mod=01 rm=101 selects rbp + disp8");
    expect(decode_vec({0x49, 0x8B, 0x45, 0xF8}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::r13, Reg::none, 1, -8),
           "mod=01 rm=101 with REX.B selects r13 + disp8");
    expect(decode_vec({0x49, 0x8B, 0x85, 0x00, 0x02, 0x00, 0x00}, insn) && insn.length == 7 &&
               is_mem_eq(insn.src, 8, Reg::r13, Reg::none, 1, 0x200),
           "mod=10 rm=101 with REX.B selects r13 + disp32");

    // SIB base=101 with mod=00 means disp32 and no base (also with REX.B=1).
    expect(decode_vec({0x48, 0x8B, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::none, Reg::none, 1, 0x12345678) &&
               Interpreter::address(CpuState{}, insn, insn.src) == 0x12345678ULL,
           "SIB base=101 mod=00 means disp32 with no base");
    expect(decode_vec({0x49, 0x8B, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::none, Reg::none, 1, 0x12345678),
           "SIB base=101 mod=00 with REX.B still means disp32 with no base");
    expect(decode_vec({0x49, 0x8B, 0x04, 0x8D, 0xF0, 0xFF, 0xFF, 0xFF}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::none, Reg::rcx, 4, -16),
           "SIB base=101 mod=00 REX.B with scaled index [rcx*4 - 16]");
    expect(decode_vec({0x4B, 0x8B, 0x04, 0xA5, 0x00, 0x10, 0x00, 0x00}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::none, Reg::r12, 4, 0x1000),
           "SIB base=101 mod=00 REX.X|B [r12*4 + 0x1000]");
    expect(decode_vec({0x49, 0x8B, 0x44, 0x25, 0x00}, insn) && insn.length == 5 &&
               is_mem_eq(insn.src, 8, Reg::r13, Reg::none, 1, 0),
           "SIB base=101 mod=01 REX.B selects [r13 + 0]");
    expect(decode_vec({0x49, 0x8B, 0x84, 0x25, 0x00, 0x01, 0x00, 0x00}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::r13, Reg::none, 1, 0x100),
           "SIB base=101 mod=10 REX.B selects [r13 + 0x100]");

    // SIB index=100 means no index, but with REX.X it is r12.
    expect(decode_vec({0x48, 0x8B, 0x04, 0x23}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::none, 1, 0),
           "SIB index=100 without REX.X means no index");
    expect(decode_vec({0x4A, 0x8B, 0x04, 0x23}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::r12, 1, 0),
           "SIB index=100 with REX.X selects r12");
    expect(decode_vec({0x4B, 0x8B, 0x04, 0xE3}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::r11, Reg::r12, 8, 0),
           "SIB index=100 REX.X|B selects [r11 + r12*8]");

    // Scales 1/2/4/8.
    CpuState addr_state{};
    addr_state.gpr[static_cast<int>(Reg::rbx)] = 0x10000ULL;
    addr_state.gpr[static_cast<int>(Reg::rcx)] = 5ULL;
    for (std::uint8_t ss = 0; ss < 4; ++ss) {
        const std::uint8_t sib = static_cast<std::uint8_t>((ss << 6U) | (1U << 3U) | 3U);
        const std::uint8_t expected_scale = static_cast<std::uint8_t>(1U << ss);
        expect(decode_vec({0x48, 0x8B, 0x04, sib}, insn) && insn.length == 4 &&
                   is_mem_eq(insn.src, 8, Reg::rbx, Reg::rcx, expected_scale, 0) &&
                   Interpreter::address(addr_state, insn, insn.src) ==
                       0x10000ULL + 5ULL * expected_scale,
               "SIB scale 1/2/4/8");
    }

    // disp8 negative and disp32 (positive and negative).
    expect(decode_vec({0x48, 0x8B, 0x43, 0x80}, insn) && insn.length == 4 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::none, 1, -128) &&
               Interpreter::address(addr_state, insn, insn.src) == 0x10000ULL - 128ULL,
           "disp8 minimum (-128)");
    expect(decode_vec({0x48, 0x8B, 0x44, 0x8B, 0xF4}, insn) && insn.length == 5 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::rcx, 4, -12) &&
               Interpreter::address(addr_state, insn, insn.src) == 0x10000ULL + 20ULL - 12ULL,
           "SIB + negative disp8 (-12)");
    expect(decode_vec({0x48, 0x8B, 0x83, 0x78, 0x56, 0x34, 0x12}, insn) && insn.length == 7 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::none, 1, 0x12345678),
           "disp32 positive");
    expect(decode_vec({0x48, 0x8B, 0x84, 0x8B, 0x00, 0xF0, 0xFF, 0xFF}, insn) && insn.length == 8 &&
               is_mem_eq(insn.src, 8, Reg::rbx, Reg::rcx, 4, -4096) &&
               Interpreter::address(addr_state, insn, insn.src) == 0x10000ULL + 20ULL - 4096ULL,
           "SIB + negative disp32 (-4096)");
}

void test_decode_moves() {
    Insn insn{};

    // 88/89/8A/8B across 1, 4, 8 byte sizes.
    expect(decode_vec({0x88, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 2 &&
               is_reg_eq(insn.dst, 1, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "mov 88 r/m8, r8");
    expect(decode_vec({0x48, 0x88, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "mov 88 keeps 8-bit size with REX.W");
    expect(decode_vec({0x40, 0x88, 0xE7}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rdi) && is_reg_eq(insn.src, 1, Reg::rsp),
           "mov 88 dil, spl with plain REX 0x40");
    expect(decode_vec({0x45, 0x88, 0x0B}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_mem_eq(insn.dst, 1, Reg::r11, Reg::none, 1, 0) &&
               is_reg_eq(insn.src, 1, Reg::r9),
           "mov 88 byte [r11], r9b");

    expect(decode_vec({0x89, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 2 &&
               is_reg_eq(insn.dst, 4, Reg::rax) && is_reg_eq(insn.src, 4, Reg::rcx),
           "mov 89 eax, ecx");
    expect(decode_vec({0x48, 0x89, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 8, Reg::rcx),
           "mov 89 rax, rcx");
    expect(decode_vec({0x45, 0x89, 0x0B}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_mem_eq(insn.dst, 4, Reg::r11, Reg::none, 1, 0) &&
               is_reg_eq(insn.src, 4, Reg::r9),
           "mov 89 dword [r11], r9d");

    expect(decode_vec({0x8A, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 2 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && is_reg_eq(insn.src, 1, Reg::rax),
           "mov 8A cl, al");
    expect(decode_vec({0x48, 0x8A, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && is_reg_eq(insn.src, 1, Reg::rax),
           "mov 8A keeps 8-bit size with REX.W");
    expect(decode_vec({0x45, 0x8A, 0x0B}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::r9) &&
               is_mem_eq(insn.src, 1, Reg::r11, Reg::none, 1, 0),
           "mov 8A r9b, byte [r11]");

    expect(decode_vec({0x8B, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 2 &&
               is_reg_eq(insn.dst, 4, Reg::rcx) && is_reg_eq(insn.src, 4, Reg::rax),
           "mov 8B ecx, eax");
    expect(decode_vec({0x48, 0x8B, 0xC8}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rcx) && is_reg_eq(insn.src, 8, Reg::rax),
           "mov 8B rcx, rax");
    expect(decode_vec({0x45, 0x8B, 0x0B}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::r9) &&
               is_mem_eq(insn.src, 4, Reg::r11, Reg::none, 1, 0),
           "mov 8B r9d, dword [r11]");
    expect(decode_vec({0x4D, 0x8B, 0x0B}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::r9) &&
               is_mem_eq(insn.src, 8, Reg::r11, Reg::none, 1, 0),
           "mov 8B r9, qword [r11]");

    // C6 /0 and C7 /0 (imm32 sign-extended with REX.W).
    expect(decode_vec({0xC6, 0xC1, 0xFE}, insn) && insn.op == Op::Mov && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && is_imm_eq(insn.src, 1, -2),
           "mov C6 /0 cl, imm8");
    expect(decode_vec({0x40, 0xC6, 0xC4, 0x07}, insn) && insn.op == Op::Mov && insn.length == 4 &&
               is_reg_eq(insn.dst, 1, Reg::rsp) && is_imm_eq(insn.src, 1, 7),
           "mov C6 /0 spl, imm8");
    expect(decode_vec({0x48, 0xC6, 0xC0, 0x80}, insn) && insn.op == Op::Mov && insn.length == 4 &&
               is_reg_eq(insn.dst, 1, Reg::rax) && is_imm_eq(insn.src, 1, -128),
           "mov C6 /0 keeps 8-bit size with REX.W");

    const std::int32_t min32 = static_cast<std::int32_t>(0x80000000U);
    expect(decode_vec({0xC7, 0xC1, 0x00, 0x00, 0x00, 0x80}, insn) && insn.op == Op::Mov &&
               insn.length == 6 && is_reg_eq(insn.dst, 4, Reg::rcx) &&
               is_imm_eq(insn.src, 4, min32),
           "mov C7 /0 ecx, 0x80000000");
    CpuState st{};
    st.gpr[static_cast<int>(Reg::rcx)] = 0x1122334455667788ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[static_cast<int>(Reg::rcx)] == 0x0000000080000000ULL,
           "C7 /0 32-bit move zeroes upper half");

    expect(decode_vec({0x48, 0xC7, 0xC1, 0x00, 0x00, 0x00, 0x80}, insn) && insn.op == Op::Mov &&
               insn.length == 7 && is_reg_eq(insn.dst, 8, Reg::rcx) &&
               is_imm_eq(insn.src, 8, static_cast<std::int64_t>(min32)),
           "mov C7 /0 REX.W rcx, sign-extended imm32");
    st.gpr[static_cast<int>(Reg::rcx)] = 0;
    Interpreter::execute(st, insn);
    expect(st.gpr[static_cast<int>(Reg::rcx)] == 0xFFFFFFFF80000000ULL,
           "REX.W C7 /0 sign-extends imm32 to 64 bits");

    expect(decode_vec({0x49, 0xC7, 0x47, 0x08, 0xFF, 0xFF, 0xFF, 0xFF}, insn) &&
               insn.op == Op::Mov && insn.length == 8 &&
               is_mem_eq(insn.dst, 8, Reg::r15, Reg::none, 1, 8) && is_imm_eq(insn.src, 8, -1),
           "mov C7 /0 REX.W|B qword [r15+8], -1");
    expect(decode_vec({0x44, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00}, insn) && insn.op == Op::Mov &&
               insn.length == 7 && is_reg_eq(insn.dst, 4, Reg::rax) && is_imm_eq(insn.src, 4, 1),
           "mov C7 /0 ignores REX.R for /0");
    for (std::uint8_t ext = 1; ext < 8; ++ext) {
        const std::uint8_t modrm = static_cast<std::uint8_t>(0xC0U | (ext << 3U));
        expect(!decode_vec({0xC6, modrm, 0}, insn), "C6 with /reg != 0 rejected");
        expect(!decode_vec({0xC7, modrm, 0, 0, 0, 0}, insn), "C7 with /reg != 0 rejected");
    }

    // B8+r imm32 (zero-extended) and REX.W B8+r imm64; REX.B B8 = r8.
    const std::uint64_t imm64 = 0xFEDCBA9876543210ULL;
    for (std::uint8_t r = 0; r < 8; ++r) {
        const std::uint8_t op = static_cast<std::uint8_t>(0xB8U + r);
        const Reg lo = static_cast<Reg>(r);
        const Reg hi = static_cast<Reg>(r + 8U);
        expect(decode_vec({op, 0x01, 0x00, 0x00, 0x80}, insn) && insn.op == Op::Mov &&
                   insn.length == 5 && is_reg_eq(insn.dst, 4, lo) &&
                   is_imm_eq(insn.src, 4, 0x0000000080000001LL),
               "B8+r imm32 zero-extended (not sign-extended)");
        expect(decode_vec({0x41, op, 0xFF, 0xFF, 0xFF, 0xFF}, insn) && insn.op == Op::Mov &&
                   insn.length == 6 && is_reg_eq(insn.dst, 4, hi) &&
                   is_imm_eq(insn.src, 4, 0x00000000FFFFFFFFLL),
               "REX.B B8+r selects r8d..r15d with zero-extended imm32");
        expect(decode_vec({0x48, op, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE}, insn) &&
                   insn.op == Op::Mov && insn.length == 10 && is_reg_eq(insn.dst, 8, lo) &&
                   is_imm_eq(insn.src, 8, static_cast<std::int64_t>(imm64)),
               "REX.W B8+r imm64");
        expect(decode_vec({0x49, op, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE}, insn) &&
                   insn.op == Op::Mov && insn.length == 10 && is_reg_eq(insn.dst, 8, hi) &&
                   is_imm_eq(insn.src, 8, static_cast<std::int64_t>(imm64)),
               "REX.W|B B8+r r8..r15 imm64");
    }
    expect(decode_vec({0x41, 0xB8, 0x05, 0x00, 0x00, 0x00}, insn) &&
               is_reg_eq(insn.dst, 4, Reg::r8),
           "REX.B B8 explicitly targets r8");
}

void test_decode_extensions() {
    Insn insn{};
    std::string err;

    // 63 movsxd: rejected without REX.W.
    expect(!decode_vec({0x63, 0xC1}, insn, kBase, &err) &&
               err.find("REX.W") != std::string::npos,
           "movsxd 63 rejected without REX.W");
    expect(!decode_vec({0x41, 0x63, 0xC1}, insn), "movsxd 63 with only REX.B rejected");
    expect(!decode_vec({0x44, 0x63, 0x01}, insn), "movsxd 63 with only REX.R rejected");

    expect(decode_vec({0x48, 0x63, 0xC1}, insn) && insn.op == Op::Movsx && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 4, Reg::rcx),
           "movsxd rax, ecx");
    expect(decode_vec({0x4D, 0x63, 0xC1}, insn) && insn.op == Op::Movsx && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::r8) && is_reg_eq(insn.src, 4, Reg::r9),
           "movsxd r8, r9d");
    expect(decode_vec({0x48, 0x63, 0x41, 0x04}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::rax) &&
               is_mem_eq(insn.src, 4, Reg::rcx, Reg::none, 1, 4),
           "movsxd rax, dword [rcx+4]");

    // 0F B6 / 0F B7 movzx to 32- and 64-bit destinations.
    expect(decode_vec({0x0F, 0xB6, 0xC1}, insn) && insn.op == Op::Movzx && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "movzx eax, cl");
    expect(decode_vec({0x48, 0x0F, 0xB6, 0xC1}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "movzx rax, cl");
    expect(decode_vec({0x45, 0x0F, 0xB6, 0x01}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 4, Reg::r8) &&
               is_mem_eq(insn.src, 1, Reg::r9, Reg::none, 1, 0),
           "movzx r8d, byte [r9]");
    expect(decode_vec({0x4D, 0x0F, 0xB6, 0x01}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::r8) &&
               is_mem_eq(insn.src, 1, Reg::r9, Reg::none, 1, 0),
           "movzx r8, byte [r9]");

    expect(decode_vec({0x0F, 0xB7, 0xC4}, insn) && insn.op == Op::Movzx && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::rax) && is_reg_eq(insn.src, 2, Reg::rsp),
           "movzx eax, sp (16-bit reg 4 valid without REX)");
    expect(decode_vec({0x48, 0x0F, 0xB7, 0xC1}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 2, Reg::rcx),
           "movzx rax, cx");
    expect(decode_vec({0x45, 0x0F, 0xB7, 0x01}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 4, Reg::r8) &&
               is_mem_eq(insn.src, 2, Reg::r9, Reg::none, 1, 0),
           "movzx r8d, word [r9]");
    expect(decode_vec({0x4D, 0x0F, 0xB7, 0x01}, insn) && insn.op == Op::Movzx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::r8) &&
               is_mem_eq(insn.src, 2, Reg::r9, Reg::none, 1, 0),
           "movzx r8, word [r9]");

    // 0F BE / 0F BF movsx to 32- and 64-bit destinations.
    expect(decode_vec({0x0F, 0xBE, 0xC1}, insn) && insn.op == Op::Movsx && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "movsx eax, cl");
    expect(decode_vec({0x48, 0x0F, 0xBE, 0xC1}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 1, Reg::rcx),
           "movsx rax, cl");
    expect(decode_vec({0x45, 0x0F, 0xBE, 0x01}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 4, Reg::r8) &&
               is_mem_eq(insn.src, 1, Reg::r9, Reg::none, 1, 0),
           "movsx r8d, byte [r9]");
    expect(decode_vec({0x4D, 0x0F, 0xBE, 0x01}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::r8) &&
               is_mem_eq(insn.src, 1, Reg::r9, Reg::none, 1, 0),
           "movsx r8, byte [r9]");

    expect(decode_vec({0x0F, 0xBF, 0xC4}, insn) && insn.op == Op::Movsx && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::rax) && is_reg_eq(insn.src, 2, Reg::rsp),
           "movsx eax, sp");
    expect(decode_vec({0x48, 0x0F, 0xBF, 0xC1}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::rax) && is_reg_eq(insn.src, 2, Reg::rcx),
           "movsx rax, cx");
    expect(decode_vec({0x45, 0x0F, 0xBF, 0x01}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 4, Reg::r8) &&
               is_mem_eq(insn.src, 2, Reg::r9, Reg::none, 1, 0),
           "movsx r8d, word [r9]");
    expect(decode_vec({0x4D, 0x0F, 0xBF, 0x01}, insn) && insn.op == Op::Movsx && insn.length == 4 &&
               is_reg_eq(insn.dst, 8, Reg::r8) &&
               is_mem_eq(insn.src, 2, Reg::r9, Reg::none, 1, 0),
           "movsx r8, word [r9]");

    // Execute extension semantics in the interpreter.
    CpuState st{};
    st.gpr[1] = 0x11223344800080FFULL;

    (void)decode_vec({0x48, 0x63, 0xC1}, insn);
    st.gpr[0] = 0;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0xFFFFFFFF800080FFULL, "exec movsxd sign-extends bit 31");

    (void)decode_vec({0x0F, 0xB6, 0xC1}, insn);
    st.gpr[0] = ~0ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000000000FFULL, "exec movzx eax, cl");

    (void)decode_vec({0x48, 0x0F, 0xB7, 0xC1}, insn);
    st.gpr[0] = ~0ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000000080FFULL, "exec movzx rax, cx");

    (void)decode_vec({0x0F, 0xBE, 0xC1}, insn);
    st.gpr[0] = ~0ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000FFFFFFFFULL, "exec movsx eax, cl truncates to 32 bits");

    (void)decode_vec({0x48, 0x0F, 0xBE, 0xC1}, insn);
    st.gpr[0] = 0;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0xFFFFFFFFFFFFFFFFULL, "exec movsx rax, cl extends to 64 bits");

    (void)decode_vec({0x0F, 0xBF, 0xC1}, insn);
    st.gpr[0] = ~0ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000FFFF80FFULL, "exec movsx eax, cx truncates to 32 bits");

    (void)decode_vec({0x48, 0x0F, 0xBF, 0xC1}, insn);
    st.gpr[0] = 0;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0xFFFFFFFFFFFF80FFULL, "exec movsx rax, cx extends to 64 bits");
}

void test_decode_lea() {
    Insn insn{};

    // 64-bit base + index*scale + disp32: lea r9, [r10 + r11*8 + 0x12345678].
    expect(decode_vec({0x4F, 0x8D, 0x8C, 0xDA, 0x78, 0x56, 0x34, 0x12}, insn) &&
               insn.op == Op::Lea && insn.length == 8 && is_reg_eq(insn.dst, 8, Reg::r9) &&
               is_mem_eq(insn.src, 8, Reg::r10, Reg::r11, 8, 0x12345678),
           "lea r9, [r10 + r11*8 + 0x12345678]");
    CpuState st{};
    st.gpr[static_cast<int>(Reg::r10)] = 0x1000ULL;
    st.gpr[static_cast<int>(Reg::r11)] = 2ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[static_cast<int>(Reg::r9)] == 0x12346688ULL,
           "lea 64-bit computes base + index*scale + disp");

    // 32-bit lea eax, [rbx + rcx*4 - 16] truncates and zero-extends upper half.
    expect(decode_vec({0x8D, 0x44, 0x8B, 0xF0}, insn) && insn.op == Op::Lea &&
               insn.length == 4 && is_reg_eq(insn.dst, 4, Reg::rax) &&
               is_mem_eq(insn.src, 4, Reg::rbx, Reg::rcx, 4, -16),
           "lea eax, [rbx + rcx*4 - 16]");
    st.gpr[static_cast<int>(Reg::rax)] = 0xDEADBEEF11111111ULL;
    st.gpr[static_cast<int>(Reg::rbx)] = 0x0000000200000020ULL;
    st.gpr[static_cast<int>(Reg::rcx)] = 4ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[static_cast<int>(Reg::rax)] == 0x0000000000000020ULL,
           "lea 32-bit truncates effective address and zeroes upper 32 bits");

    // RIP-relative lea.
    expect(decode_vec({0x48, 0x8D, 0x05, 0xF0, 0xFF, 0xFF, 0xFF}, insn) && insn.op == Op::Lea &&
               insn.length == 7 && is_reg_eq(insn.dst, 8, Reg::rax) &&
               is_mem_eq(insn.src, 8, Reg::rip, Reg::none, 1, -16) &&
               Interpreter::address(CpuState{}, insn, insn.src) == kBase + 7 - 16,
           "lea rax, [rip - 16]");
    expect(decode_vec({0x4D, 0x8D, 0x05, 0x20, 0x00, 0x00, 0x00}, insn) && insn.op == Op::Lea &&
               insn.length == 7 && is_reg_eq(insn.dst, 8, Reg::r8) &&
               is_mem_eq(insn.src, 8, Reg::rip, Reg::none, 1, 0x20) &&
               Interpreter::address(CpuState{}, insn, insn.src) == kBase + 7 + 0x20,
           "lea r8, [rip + 0x20]");

    // Register operand (mod=3) rejected.
    expect(!decode_vec({0x8D, 0xC0}, insn), "lea with register operand 8D C0 rejected");
    expect(!decode_vec({0x48, 0x8D, 0xC8}, insn), "lea with register operand 48 8D C8 rejected");
    expect(!decode_vec({0x4F, 0x8D, 0xFF}, insn), "lea with register operand 4F 8D FF rejected");
}

void test_decode_stack_ret_nop() {
    Insn insn{};

    // push/pop 50..5F with and without REX.B.
    for (std::uint8_t r = 0; r < 8; ++r) {
        const std::uint8_t push_op = static_cast<std::uint8_t>(0x50U + r);
        const std::uint8_t pop_op = static_cast<std::uint8_t>(0x58U + r);
        const Reg lo = static_cast<Reg>(r);
        const Reg hi = static_cast<Reg>(r + 8U);

        expect(decode_vec({push_op}, insn) && insn.op == Op::Push && insn.length == 1 &&
                   is_reg_eq(insn.dst, 8, lo),
               "push 50+r");
        expect(decode_vec({0x41, push_op}, insn) && insn.op == Op::Push && insn.length == 2 &&
                   is_reg_eq(insn.dst, 8, hi),
               "REX.B push 50+r");
        expect(decode_vec({0x48, push_op}, insn) && insn.op == Op::Push && insn.length == 2 &&
                   is_reg_eq(insn.dst, 8, lo),
               "REX.W push 50+r");

        expect(decode_vec({pop_op}, insn) && insn.op == Op::Pop && insn.length == 1 &&
                   is_reg_eq(insn.dst, 8, lo),
               "pop 58+r");
        expect(decode_vec({0x41, pop_op}, insn) && insn.op == Op::Pop && insn.length == 2 &&
                   is_reg_eq(insn.dst, 8, hi),
               "REX.B pop 58+r");
        expect(decode_vec({0x49, pop_op}, insn) && insn.op == Op::Pop && insn.length == 2 &&
                   is_reg_eq(insn.dst, 8, hi),
               "REX.W|B pop 58+r");
    }

    // ret C3.
    expect(decode_vec({0xC3}, insn) && insn.op == Op::Ret && insn.length == 1 && insn.ends_block(),
           "ret C3");

    // nop 90 and 0F 1F /0.
    expect(decode_vec({0x90}, insn) && insn.op == Op::Nop && insn.length == 1, "nop 90");
    expect(decode_vec({0x40, 0x90}, insn) && insn.op == Op::Nop && insn.length == 2, "REX 90 nop");
    expect(decode_vec({0x48, 0x90}, insn) && insn.op == Op::Nop && insn.length == 2,
           "REX.W 90 nop");
    expect(decode_vec({0x44, 0x90}, insn) && insn.op == Op::Nop && insn.length == 2,
           "REX.R 90 nop");
    expect(decode_vec({0x0F, 0x1F, 0x00}, insn) && insn.op == Op::Nop && insn.length == 3,
           "3-byte 0F 1F /0 nop");
    expect(decode_vec({0x0F, 0x1F, 0x40, 0x00}, insn) && insn.op == Op::Nop && insn.length == 4,
           "4-byte 0F 1F /0 nop");
    expect(decode_vec({0x0F, 0x1F, 0x44, 0x00, 0x00}, insn) && insn.op == Op::Nop &&
               insn.length == 5,
           "5-byte 0F 1F /0 nop");
    expect(decode_vec({0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}, insn) && insn.op == Op::Nop &&
               insn.length == 7,
           "7-byte 0F 1F /0 nop");
    expect(decode_vec({0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, insn) &&
               insn.op == Op::Nop && insn.length == 8,
           "8-byte 0F 1F /0 nop");
    expect(decode_vec({0x0F, 0x1F, 0xC0}, insn) && insn.op == Op::Nop && insn.length == 3,
           "register-form 0F 1F C0 nop");

    // REX.B 90 (xchg r8, rax / r8d, eax) must be rejected.
    static constexpr std::uint8_t kRexB[] = {0x41, 0x43, 0x45, 0x49, 0x4F};
    for (const std::uint8_t rex : kRexB) {
        expect(!decode_vec({rex, 0x90}, insn), "REX.B 90 (xchg r8, rax) rejected");
    }
}

void test_decode_branches() {
    Insn insn{};

    // All 16 70+cc rel8 and 0F 80+cc rel32, forward and backward targets.
    for (std::uint8_t cc = 0; cc < 16; ++cc) {
        const Cond cond = static_cast<Cond>(cc);
        const std::uint8_t jcc8 = static_cast<std::uint8_t>(0x70U + cc);
        const std::uint8_t jcc32 = static_cast<std::uint8_t>(0x80U + cc);

        expect(decode_vec({jcc8, 0x2A}, insn) && insn.op == Op::Jcc && insn.cond == cond &&
                   insn.length == 2 && insn.ends_block() && insn.target == kBase + 2 + 0x2A,
               "70+cc rel8 forward");
        expect(decode_vec({jcc8, 0x80}, insn) && insn.op == Op::Jcc && insn.cond == cond &&
                   insn.length == 2 && insn.target == kBase + 2 - 128,
               "70+cc rel8 backward (-128)");
        expect(decode_vec({jcc8, 0xFE}, insn) && insn.op == Op::Jcc && insn.cond == cond &&
                   insn.length == 2 && insn.target == kBase,
               "70+cc rel8 self-loop (-2)");

        expect(decode_vec({0x0F, jcc32, 0x78, 0x56, 0x34, 0x12}, insn) && insn.op == Op::Jcc &&
                   insn.cond == cond && insn.length == 6 && insn.ends_block() &&
                   insn.target == kBase + 6 + 0x12345678ULL,
               "0F 80+cc rel32 forward");
        expect(decode_vec({0x0F, jcc32, 0xCC, 0xED, 0xFF, 0xFF}, insn) && insn.op == Op::Jcc &&
                   insn.cond == cond && insn.length == 6 && insn.target == kBase + 6 - 0x1234ULL,
               "0F 80+cc rel32 backward (-0x1234)");
    }

    // Unconditional EB (rel8) and E9 (rel32).
    expect(decode_vec({0xEB, 0x7F}, insn) && insn.op == Op::Jmp && insn.length == 2 &&
               insn.ends_block() && insn.target == kBase + 2 + 127,
           "EB rel8 forward (+127)");
    expect(decode_vec({0xEB, 0x80}, insn) && insn.op == Op::Jmp && insn.length == 2 &&
               insn.target == kBase + 2 - 128,
           "EB rel8 backward (-128)");
    expect(decode_vec({0xEB, 0xFE}, insn) && insn.op == Op::Jmp && insn.length == 2 &&
               insn.target == kBase,
           "EB rel8 self-loop (-2)");
    expect(decode_vec({0xE9, 0x78, 0x56, 0x34, 0x12}, insn) && insn.op == Op::Jmp &&
               insn.length == 5 && insn.ends_block() && insn.target == kBase + 5 + 0x12345678ULL,
           "E9 rel32 forward");
    expect(decode_vec({0xE9, 0xCC, 0xED, 0xFF, 0xFF}, insn) && insn.op == Op::Jmp &&
               insn.length == 5 && insn.target == kBase + 5 - 0x1234ULL,
           "E9 rel32 backward (-0x1234)");
}

void test_decode_test_insns() {
    Insn insn{};

    // 84: test r/m8, r8.
    expect(decode_vec({0x84, 0xC1}, insn) && insn.op == Op::Test && insn.length == 2 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && is_reg_eq(insn.src, 1, Reg::rax),
           "test 84 cl, al");
    expect(decode_vec({0x45, 0x84, 0xC1}, insn) && insn.op == Op::Test && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::r9) && is_reg_eq(insn.src, 1, Reg::r8),
           "test 84 r9b, r8b");
    expect(decode_vec({0x84, 0x11}, insn) && insn.op == Op::Test && insn.length == 2 &&
               is_mem_eq(insn.dst, 1, Reg::rcx, Reg::none, 1, 0) &&
               is_reg_eq(insn.src, 1, Reg::rdx),
           "test 84 byte [rcx], dl");

    // 85: test r/m, r.
    expect(decode_vec({0x85, 0xD2}, insn) && insn.op == Op::Test && insn.length == 2 &&
               is_reg_eq(insn.dst, 4, Reg::rdx) && is_reg_eq(insn.src, 4, Reg::rdx),
           "test 85 edx, edx");
    expect(decode_vec({0x48, 0x85, 0xD2}, insn) && insn.op == Op::Test && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rdx) && is_reg_eq(insn.src, 8, Reg::rdx),
           "test 85 rdx, rdx");
    expect(decode_vec({0x4D, 0x85, 0x0B}, insn) && insn.op == Op::Test && insn.length == 3 &&
               is_mem_eq(insn.dst, 8, Reg::r11, Reg::none, 1, 0) &&
               is_reg_eq(insn.src, 8, Reg::r9),
           "test 85 qword [r11], r9");

    // F6 /0 and F7 /0.
    expect(decode_vec({0xF6, 0xC1, 0x81}, insn) && insn.op == Op::Test && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && is_imm_eq(insn.src, 1, -127),
           "test F6 /0 cl, imm8");
    expect(decode_vec({0x41, 0xF6, 0x41, 0x08, 0x0F}, insn) && insn.op == Op::Test &&
               insn.length == 5 && is_mem_eq(insn.dst, 1, Reg::r9, Reg::none, 1, 8) &&
               is_imm_eq(insn.src, 1, 15),
           "test F6 /0 byte [r9+8], 0x0F");
    expect(decode_vec({0x44, 0xF6, 0xC0, 0x01}, insn) && insn.op == Op::Test && insn.length == 4 &&
               is_reg_eq(insn.dst, 1, Reg::rax) && is_imm_eq(insn.src, 1, 1),
           "test F6 /0 ignores REX.R for /0");

    const std::int32_t min32 = static_cast<std::int32_t>(0x80000000U);
    expect(decode_vec({0xF7, 0xC1, 0x00, 0x00, 0x00, 0x80}, insn) && insn.op == Op::Test &&
               insn.length == 6 && is_reg_eq(insn.dst, 4, Reg::rcx) &&
               is_imm_eq(insn.src, 4, min32),
           "test F7 /0 ecx, 0x80000000");
    expect(decode_vec({0x48, 0xF7, 0xC1, 0x00, 0x00, 0x00, 0x80}, insn) && insn.op == Op::Test &&
               insn.length == 7 && is_reg_eq(insn.dst, 8, Reg::rcx) &&
               is_imm_eq(insn.src, 8, static_cast<std::int64_t>(min32)),
           "test F7 /0 REX.W rcx, sign-extended imm32");
    expect(decode_vec({0x49, 0xF7, 0x00, 0xFF, 0x00, 0x00, 0x00}, insn) && insn.op == Op::Test &&
               insn.length == 7 && is_mem_eq(insn.dst, 8, Reg::r8, Reg::none, 1, 0) &&
               is_imm_eq(insn.src, 8, 0xFF),
           "test F7 /0 qword [r8], 0xFF");

    // F6 /1..7 and F7 /1..7 must be rejected.
    for (std::uint8_t ext = 1; ext < 8; ++ext) {
        const std::uint8_t modrm = static_cast<std::uint8_t>(0xC0U | (ext << 3U));
        expect(!decode_vec({0xF6, modrm, 0, 0, 0, 0}, insn), "F6 with /reg != 0 rejected");
        expect(!decode_vec({0xF7, modrm, 0, 0, 0, 0}, insn), "F7 with /reg != 0 rejected");
        expect(!decode_vec({0x48, 0xF7, modrm, 0, 0, 0, 0}, insn),
               "REX.W F7 with /reg != 0 rejected");
    }
}

void test_decode_inc_dec() {
    Insn insn{};

    // FE /0 /1 (8-bit).
    expect(decode_vec({0xFE, 0xC1}, insn) && insn.op == Op::Inc && insn.length == 2 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && insn.src.kind == Operand::Kind::None,
           "inc FE /0 cl");
    expect(decode_vec({0xFE, 0xC9}, insn) && insn.op == Op::Dec && insn.length == 2 &&
               is_reg_eq(insn.dst, 1, Reg::rcx) && insn.src.kind == Operand::Kind::None,
           "dec FE /1 cl");
    expect(decode_vec({0x40, 0xFE, 0xC4}, insn) && insn.op == Op::Inc && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::rsp),
           "inc FE /0 spl with REX");
    expect(decode_vec({0x41, 0xFE, 0xC0}, insn) && insn.op == Op::Inc && insn.length == 3 &&
               is_reg_eq(insn.dst, 1, Reg::r8),
           "inc FE /0 r8b");
    expect(decode_vec({0xFE, 0x00}, insn) && insn.op == Op::Inc && insn.length == 2 &&
               is_mem_eq(insn.dst, 1, Reg::rax, Reg::none, 1, 0),
           "inc FE /0 byte [rax]");
    expect(decode_vec({0xFE, 0x08}, insn) && insn.op == Op::Dec && insn.length == 2 &&
               is_mem_eq(insn.dst, 1, Reg::rax, Reg::none, 1, 0),
           "dec FE /1 byte [rax]");

    // FF /0 /1 (32-bit and 64-bit).
    expect(decode_vec({0xFF, 0xC1}, insn) && insn.op == Op::Inc && insn.length == 2 &&
               is_reg_eq(insn.dst, 4, Reg::rcx),
           "inc FF /0 ecx");
    expect(decode_vec({0xFF, 0xC9}, insn) && insn.op == Op::Dec && insn.length == 2 &&
               is_reg_eq(insn.dst, 4, Reg::rcx),
           "dec FF /1 ecx");
    expect(decode_vec({0x41, 0xFF, 0xC1}, insn) && insn.op == Op::Inc && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::r9),
           "inc FF /0 r9d");
    expect(decode_vec({0x48, 0xFF, 0xC1}, insn) && insn.op == Op::Inc && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rcx),
           "inc FF /0 rcx");
    expect(decode_vec({0x48, 0xFF, 0xC9}, insn) && insn.op == Op::Dec && insn.length == 3 &&
               is_reg_eq(insn.dst, 8, Reg::rcx),
           "dec FF /1 rcx");
    expect(decode_vec({0x49, 0xFF, 0x09}, insn) && insn.op == Op::Dec && insn.length == 3 &&
               is_mem_eq(insn.dst, 8, Reg::r9, Reg::none, 1, 0),
           "dec FF /1 qword [r9]");
    expect(decode_vec({0x44, 0xFF, 0xC0}, insn) && insn.op == Op::Inc && insn.length == 3 &&
               is_reg_eq(insn.dst, 4, Reg::rax),
           "inc FF /0 ignores REX.R for /0");

    // FE /2..7 and FF /2..7 must be rejected.
    for (std::uint8_t ext = 2; ext < 8; ++ext) {
        const std::uint8_t modrm_reg = static_cast<std::uint8_t>(0xC0U | (ext << 3U));
        const std::uint8_t modrm_mem = static_cast<std::uint8_t>(ext << 3U);
        expect(!decode_vec({0xFE, modrm_reg}, insn), "FE /2..7 rejected");
        expect(!decode_vec({0xFF, modrm_reg}, insn), "FF /2..7 reg rejected");
        expect(!decode_vec({0x48, 0xFF, modrm_reg}, insn), "REX.W FF /2..7 reg rejected");
        expect(!decode_vec({0xFF, modrm_mem}, insn), "FF /2..7 mem rejected");
    }
}

void test_decode_rejections() {
    Insn insn{};
    std::string err;

    // All 11 legacy prefixes must be rejected.
    static constexpr std::uint8_t kLegacyPrefixes[] = {
        0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x26, 0x36, 0x3E, 0x64, 0x65,
    };
    for (const std::uint8_t p : kLegacyPrefixes) {
        err.clear();
        expect(!decode_vec({p, 0x90}, insn, kBase, &err) &&
                   err.find("legacy prefix") != std::string::npos,
               "legacy prefix before nop rejected");
        err.clear();
        expect(!decode_vec({p, 0x89, 0xC8}, insn, kBase, &err) &&
                   err.find("legacy prefix") != std::string::npos,
               "legacy prefix before mov rejected");
    }

    // AH/CH/DH/BH byte registers (reg 4..7, size=1, no REX) must be rejected; with REX they are SPL/BPL/SIL/DIL.
    for (std::uint8_t r = 4; r < 8; ++r) {
        const std::uint8_t modrm_reg = static_cast<std::uint8_t>(0xC0U | (r << 3U));
        const std::uint8_t modrm_rm = static_cast<std::uint8_t>(0xC0U | r);
        expect(!decode_vec({0x88, modrm_reg}, insn), "88 mov al, AH..BH rejected");
        expect(!decode_vec({0x88, modrm_rm}, insn), "88 mov AH..BH, al rejected");
        expect(!decode_vec({0x8A, modrm_reg}, insn), "8A mov AH..BH, al rejected");
        expect(!decode_vec({0x80, modrm_rm, 1}, insn), "80 add AH..BH, 1 rejected");
        expect(!decode_vec({0xC6, modrm_rm, 1}, insn), "C6 mov AH..BH, 1 rejected");
        expect(!decode_vec({0xF6, modrm_rm, 1}, insn), "F6 test AH..BH, 1 rejected");
        expect(!decode_vec({0xFE, modrm_rm}, insn), "FE inc AH..BH rejected");
        expect(!decode_vec({0x0F, 0xB6, modrm_rm}, insn), "0F B6 movzx eax, AH..BH rejected");
        expect(!decode_vec({0x0F, 0xBE, modrm_rm}, insn), "0F BE movsx eax, AH..BH rejected");

        expect(decode_vec({0x40, 0x88, modrm_reg}, insn) &&
                   is_reg_eq(insn.src, 1, static_cast<Reg>(r)),
               "0x40 REX allows SPL/BPL/SIL/DIL");
        expect(decode_vec({0x40, 0x0F, 0xB6, modrm_rm}, insn) &&
                   is_reg_eq(insn.src, 1, static_cast<Reg>(r)),
               "0x40 REX allows movzx eax, spl/bpl/sil/dil");
    }
    // Memory [rsp] in an 8-bit mov must not be misidentified as AH.
    expect(decode_vec({0x88, 0x04, 0x24}, insn) && insn.length == 3 &&
               is_mem_eq(insn.dst, 1, Reg::rsp, Reg::none, 1, 0) &&
               is_reg_eq(insn.src, 1, Reg::rax),
           "mov byte [rsp], al allowed (SIB base rsp is not AH)");
    expect(!decode_vec({0x88, 0x24, 0x24}, insn), "mov byte [rsp], ah rejected");

    // Truncated instructions at every byte boundary of long instructions.
    const auto check_truncated = [&](const std::vector<std::uint8_t>& full, const char* label) {
        for (std::size_t n = 0; n < full.size(); ++n) {
            expect(!decode_insn(full.data(), n, kBase, insn, &err), label);
        }
        expect(decode_insn(full.data(), full.size(), kBase, insn, &err) &&
                   insn.length == full.size(),
               label);
    };
    check_truncated({0x48, 0xC7, 0x84, 0x24, 0x10, 0x20, 0x30, 0x40, 0x01, 0x02, 0x03, 0x04},
                    "12-byte mov [rsp+disp32], imm32 truncation boundaries");
    check_truncated({0x48, 0xB8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
                    "10-byte movabs rax, imm64 truncation boundaries");
    check_truncated({0x0F, 0x1F, 0x84, 0x00, 0x11, 0x22, 0x33, 0x44},
                    "8-byte 0F 1F nop truncation boundaries");
    check_truncated({0x48, 0x8B, 0x04, 0x25, 0x11, 0x22, 0x33, 0x44},
                    "8-byte SIB base=101 mod=00 truncation boundaries");
    check_truncated({0x48, 0x8D, 0x05, 0x11, 0x22, 0x33, 0x44},
                    "7-byte RIP-relative lea truncation boundaries");
    check_truncated({0x0F, 0x85, 0x11, 0x22, 0x33, 0x44},
                    "6-byte jcc rel32 truncation boundaries");
    expect(!decode_vec({0x0F}, insn), "lone 0F prefix rejected");
    expect(!decode_vec({0x48}, insn), "lone REX prefix rejected");

    // E8 call, 0F 05 syscall, and other unsupported opcodes.
    err.clear();
    expect(!decode_vec({0xE8, 0x00, 0x00, 0x00, 0x00}, insn, kBase, &err) &&
               err.find("e8") != std::string::npos,
           "E8 call rejected");
    err.clear();
    expect(!decode_vec({0x0F, 0x05}, insn, kBase, &err) &&
               err.find("0f") != std::string::npos && err.find("05") != std::string::npos,
           "0F 05 syscall rejected");
    expect(!decode_vec({0xC2, 0x08, 0x00}, insn), "C2 ret imm16 rejected");
    expect(!decode_vec({0xCC}, insn), "CC int3 rejected");
    expect(!decode_vec({0x0F, 0xAF, 0xC1}, insn), "0F AF imul rejected");
}

template <typename U>
bool indep_parity_even(U v) noexcept {
    const std::uint8_t low = static_cast<std::uint8_t>(v & 0xFFU);
    int ones = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if (((low >> bit) & 1U) != 0U) {
            ++ones;
        }
    }
    return (ones % 2) == 0;
}

template <typename U, typename S>
std::array<U, 14> make_flag_test_values() noexcept {
    constexpr U smax = static_cast<U>(std::numeric_limits<S>::max());
    constexpr U smin = static_cast<U>(std::numeric_limits<S>::min());
    constexpr U umax = std::numeric_limits<U>::max();
    constexpr U minus_one = static_cast<U>(static_cast<S>(-1));
    return {{
        U{0},
        U{1},
        U{2},
        U{3},
        U{7},
        minus_one,
        smax,
        static_cast<U>(smax - U{1}),
        smin,
        static_cast<U>(smin + U{1}),
        umax,
        static_cast<U>(umax - U{1}),
        static_cast<U>(0x55U),
        static_cast<U>(0xAAU),
    }};
}

template <typename U, typename S>
void check_flags_for_size(std::uint8_t size) {
    constexpr S kSmax = std::numeric_limits<S>::max();
    constexpr S kSmin = std::numeric_limits<S>::min();
    constexpr U kUmax = std::numeric_limits<U>::max();
    const auto vals = make_flag_test_values<U, S>();

    struct BinOpCase {
        Op op;
        std::uint8_t op8;
        std::uint8_t op32;
        bool writes_dst;
    };
    static constexpr std::array<BinOpCase, 7> kBinOps = {{
        {Op::Add, 0x00, 0x01, true},
        {Op::Sub, 0x28, 0x29, true},
        {Op::Cmp, 0x38, 0x39, false},
        {Op::And, 0x20, 0x21, true},
        {Op::Or, 0x08, 0x09, true},
        {Op::Xor, 0x30, 0x31, true},
        {Op::Test, 0x84, 0x85, false},
    }};

    for (const BinOpCase& c : kBinOps) {
        const std::uint8_t op_byte = (size == 1) ? c.op8 : c.op32;
        const std::vector<std::uint8_t> bytes =
            (size == 8) ? std::vector<std::uint8_t>{0x48, op_byte, 0xC8}
                        : std::vector<std::uint8_t>{op_byte, 0xC8};
        Insn insn{};
        expect(decode_vec(bytes, insn) && insn.op == c.op && insn.dst.size == size,
               "decode flag-test binary op");

        for (const U ua : vals) {
            for (const U ub : vals) {
                const S sa = static_cast<S>(ua);
                const S sb = static_cast<S>(ub);
                U exp_res = 0;
                bool exp_cf = false;
                bool exp_of = false;

                switch (c.op) {
                case Op::Add:
                    exp_res = static_cast<U>(ua + ub);
                    exp_cf = ua > static_cast<U>(kUmax - ub);
                    exp_of = (sb > 0 && sa > kSmax - sb) || (sb < 0 && sa < kSmin - sb);
                    break;
                case Op::Sub:
                case Op::Cmp:
                    exp_res = static_cast<U>(ua - ub);
                    exp_cf = ua < ub;
                    exp_of = (sb < 0 && sa > kSmax + sb) || (sb > 0 && sa < kSmin + sb);
                    break;
                case Op::And:
                case Op::Test:
                    exp_res = static_cast<U>(ua & ub);
                    exp_cf = false;
                    exp_of = false;
                    break;
                case Op::Or:
                    exp_res = static_cast<U>(ua | ub);
                    exp_cf = false;
                    exp_of = false;
                    break;
                case Op::Xor:
                    exp_res = static_cast<U>(ua ^ ub);
                    exp_cf = false;
                    exp_of = false;
                    break;
                default:
                    break;
                }

                const bool exp_zf = (exp_res == 0);
                const bool exp_sf = (static_cast<S>(exp_res) < 0);
                const bool exp_pf = indep_parity_even(exp_res);

                const std::uint64_t init_rax =
                    (size == 1) ? (0x1122334455667700ULL | static_cast<std::uint64_t>(ua))
                    : (size == 4) ? (0x1122334400000000ULL | static_cast<std::uint64_t>(ua))
                                  : static_cast<std::uint64_t>(ua);
                const std::uint64_t init_rcx =
                    (size == 1) ? (0xAABBCCDDEEFF9900ULL | static_cast<std::uint64_t>(ub))
                    : (size == 4) ? (0xAABBCCDD00000000ULL | static_cast<std::uint64_t>(ub))
                                  : static_cast<std::uint64_t>(ub);

                CpuState st{};
                st.rip = kBase;
                st.gpr[0] = init_rax;
                st.gpr[1] = init_rcx;
                // Pre-invert every flag to catch stale-flag bugs.
                st.cf = !exp_cf;
                st.zf = !exp_zf;
                st.sf = !exp_sf;
                st.of = !exp_of;
                st.pf = !exp_pf;

                Interpreter::execute(st, insn);

                std::uint64_t exp_rax = init_rax;
                if (c.writes_dst) {
                    exp_rax = (size == 1) ? ((init_rax & ~0xFFULL) |
                                             static_cast<std::uint64_t>(exp_res))
                                          : static_cast<std::uint64_t>(exp_res);
                }

                expect(st.cf == exp_cf && st.zf == exp_zf && st.sf == exp_sf && st.of == exp_of &&
                           st.pf == exp_pf && st.gpr[0] == exp_rax && st.gpr[1] == init_rcx &&
                           st.rip == kBase + insn.length,
                       "interpreter binary op flags and result match independent C++ oracle");
            }
        }
    }

    // inc and dec at sizes 1, 4, 8 (must preserve CF).
    static constexpr bool kBoolPair[] = {true, false};
    for (const bool is_inc : kBoolPair) {
        const std::uint8_t modrm = is_inc ? 0xC0 : 0xC8;
        const std::vector<std::uint8_t> bytes =
            (size == 1)   ? std::vector<std::uint8_t>{0xFE, modrm}
            : (size == 4) ? std::vector<std::uint8_t>{0xFF, modrm}
                          : std::vector<std::uint8_t>{0x48, 0xFF, modrm};
        Insn insn{};
        expect(decode_vec(bytes, insn) && insn.op == (is_inc ? Op::Inc : Op::Dec) &&
                   insn.dst.size == size,
               "decode inc/dec for flag table");

        for (const U ua : vals) {
            const S sa = static_cast<S>(ua);
            const U exp_res = is_inc ? static_cast<U>(ua + U{1}) : static_cast<U>(ua - U{1});
            const bool exp_of = is_inc ? (sa == kSmax) : (sa == kSmin);
            const bool exp_zf = (exp_res == 0);
            const bool exp_sf = (static_cast<S>(exp_res) < 0);
            const bool exp_pf = indep_parity_even(exp_res);

            const std::uint64_t init_rax =
                (size == 1) ? (0x1122334455667700ULL | static_cast<std::uint64_t>(ua))
                : (size == 4) ? (0x1122334400000000ULL | static_cast<std::uint64_t>(ua))
                              : static_cast<std::uint64_t>(ua);
            const std::uint64_t exp_rax =
                (size == 1) ? ((init_rax & ~0xFFULL) | static_cast<std::uint64_t>(exp_res))
                            : static_cast<std::uint64_t>(exp_res);

            for (const bool init_cf : kBoolPair) {
                CpuState st{};
                st.rip = kBase;
                st.gpr[0] = init_rax;
                st.cf = init_cf;
                st.zf = !exp_zf;
                st.sf = !exp_sf;
                st.of = !exp_of;
                st.pf = !exp_pf;

                Interpreter::execute(st, insn);

                expect(st.cf == init_cf && st.zf == exp_zf && st.sf == exp_sf && st.of == exp_of &&
                           st.pf == exp_pf && st.gpr[0] == exp_rax && st.rip == kBase + insn.length,
                       "interpreter inc/dec flags, CF preservation, and result match oracle");
            }
        }
    }
}

void test_interp_flags_exhaustive() {
    check_flags_for_size<std::uint8_t, std::int8_t>(1);
    check_flags_for_size<std::uint32_t, std::int32_t>(4);
    check_flags_for_size<std::uint64_t, std::int64_t>(8);
}

template <typename U, typename S>
void check_eval_cond_for_size(std::uint8_t size) {
    constexpr S kSmax = std::numeric_limits<S>::max();
    constexpr S kSmin = std::numeric_limits<S>::min();
    const auto vals = make_flag_test_values<U, S>();

    const std::vector<std::uint8_t> bytes =
        (size == 1)   ? std::vector<std::uint8_t>{0x38, 0xC8}
        : (size == 4) ? std::vector<std::uint8_t>{0x39, 0xC8}
                      : std::vector<std::uint8_t>{0x48, 0x39, 0xC8};
    Insn cmp_insn{};
    expect(decode_vec(bytes, cmp_insn) && cmp_insn.op == Op::Cmp && cmp_insn.dst.size == size,
           "decode cmp for eval_cond table");

    for (const U ua : vals) {
        for (const U ub : vals) {
            const S sa = static_cast<S>(ua);
            const S sb = static_cast<S>(ub);
            const U diff = static_cast<U>(ua - ub);
            const bool exp_of = (sb < 0 && sa > kSmax + sb) || (sb > 0 && sa < kSmin + sb);
            const bool exp_sf = (static_cast<S>(diff) < 0);
            const bool exp_pf = indep_parity_even(diff);

            CpuState st{};
            st.gpr[0] = static_cast<std::uint64_t>(ua);
            st.gpr[1] = static_cast<std::uint64_t>(ub);
            Interpreter::execute(st, cmp_insn);

            expect(eval_cond(st, Cond::O) == exp_of && eval_cond(st, Cond::NO) == !exp_of &&
                       eval_cond(st, Cond::B) == (ua < ub) &&
                       eval_cond(st, Cond::AE) == (ua >= ub) &&
                       eval_cond(st, Cond::E) == (ua == ub) &&
                       eval_cond(st, Cond::NE) == (ua != ub) &&
                       eval_cond(st, Cond::BE) == (ua <= ub) &&
                       eval_cond(st, Cond::A) == (ua > ub) &&
                       eval_cond(st, Cond::S) == exp_sf && eval_cond(st, Cond::NS) == !exp_sf &&
                       eval_cond(st, Cond::P) == exp_pf && eval_cond(st, Cond::NP) == !exp_pf &&
                       eval_cond(st, Cond::L) == (sa < sb) && eval_cond(st, Cond::GE) == (sa >= sb) &&
                       eval_cond(st, Cond::LE) == (sa <= sb) &&
                       eval_cond(st, Cond::G) == (sa > sb),
                   "eval_cond matches all 16 signed/unsigned comparisons after cmp");
        }
    }
}

void test_eval_cond_exhaustive() {
    check_eval_cond_for_size<std::uint8_t, std::int8_t>(1);
    check_eval_cond_for_size<std::uint32_t, std::int32_t>(4);
    check_eval_cond_for_size<std::uint64_t, std::int64_t>(8);
}

void test_register_and_memory_writes() {
    Insn insn{};
    CpuState st{};
    std::uint32_t mem32 = 0x80000001U;
    std::uint8_t mem8 = 0x99U;

    // 32-bit destinations zero the upper 32 bits.
    (void)decode_vec({0x89, 0xC8}, insn);  // mov eax, ecx
    st = CpuState{};
    st.gpr[0] = 0xDEADBEEF12345678ULL;
    st.gpr[1] = 0xCAFEBABE00000042ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x0000000000000042ULL, "32-bit mov zeroes upper half");

    (void)decode_vec({0x44, 0x8B, 0x01}, insn);  // mov r8d, dword [rcx]
    st = CpuState{};
    st.gpr[1] = reinterpret_cast<std::uintptr_t>(&mem32);
    st.gpr[8] = 0xFFFFFFFFFFFFFFFFULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[8] == 0x0000000080000001ULL, "32-bit load into r8d zeroes upper half");

    (void)decode_vec({0x41, 0xB9, 0x78, 0x56, 0x34, 0x82}, insn);  // mov r9d, 0x82345678
    st = CpuState{};
    st.gpr[9] = 0x1122334455667788ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[9] == 0x0000000082345678ULL, "32-bit B8+r into r9d zeroes upper half");

    (void)decode_vec({0x0F, 0xB6, 0xC1}, insn);  // movzx eax, cl
    st = CpuState{};
    st.gpr[0] = 0xFFFFFFFFFFFFFFFFULL;
    st.gpr[1] = 0xABCDULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000000000CDULL, "32-bit movzx zeroes upper half");

    (void)decode_vec({0x0F, 0xBE, 0xC1}, insn);  // movsx eax, cl
    st = CpuState{};
    st.gpr[0] = 0xFFFFFFFF00000000ULL;
    st.gpr[1] = 0x80ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000FFFFFF80ULL, "32-bit movsx zeroes upper half");

    (void)decode_vec({0x8D, 0x44, 0x51, 0x10}, insn);  // lea eax, [rcx + rdx*2 + 0x10]
    st = CpuState{};
    st.gpr[0] = 0xFFFFFFFF00000000ULL;
    st.gpr[1] = 0x100000000ULL;
    st.gpr[2] = 0x20ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x0000000000000050ULL, "32-bit lea zeroes upper half");

    (void)decode_vec({0x41, 0xFF, 0xC7}, insn);  // inc r15d
    st = CpuState{};
    st.gpr[15] = 0xDEADBEEF00000000ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[15] == 0x0000000000000001ULL, "32-bit inc zeroes upper half");

    (void)decode_vec({0xFF, 0xC8}, insn);  // dec eax
    st = CpuState{};
    st.gpr[0] = 0xDEADBEEF00000000ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x00000000FFFFFFFFULL, "32-bit dec zeroes upper half");

    (void)decode_vec({0x31, 0xC0}, insn);  // xor eax, eax
    st = CpuState{};
    st.gpr[0] = 0xDEADBEEF12345678ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0ULL, "32-bit xor eax, eax zeroes upper half");

    // 8-bit writes preserve bits 8..63.
    (void)decode_vec({0x40, 0x88, 0xCC}, insn);  // mov spl, cl
    st = CpuState{};
    st.gpr[4] = 0x1122334455667788ULL;
    st.gpr[1] = 0xABCDULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[4] == 0x11223344556677CDULL, "8-bit mov spl, cl keeps bits 8..63");

    (void)decode_vec({0x41, 0xC6, 0xC7, 0xEF}, insn);  // mov r15b, 0xEF
    st = CpuState{};
    st.gpr[15] = 0x123456789ABCDEF0ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[15] == 0x123456789ABCDEEFULL, "8-bit C6 into r15b keeps bits 8..63");

    (void)decode_vec({0x8A, 0x01}, insn);  // mov al, byte [rcx]
    st = CpuState{};
    st.gpr[0] = 0xAABBCCDDEEFF0011ULL;
    st.gpr[1] = reinterpret_cast<std::uintptr_t>(&mem8);
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0xAABBCCDDEEFF0099ULL, "8-bit memory load into al keeps bits 8..63");

    (void)decode_vec({0x04, 0x10}, insn);  // add al, 0x10
    st = CpuState{};
    st.gpr[0] = 0x11223344556677F8ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[0] == 0x1122334455667708ULL && st.cf,
           "8-bit add wrap does not carry into bits 8..63");

    (void)decode_vec({0x41, 0xFE, 0xC1}, insn);  // inc r9b
    st = CpuState{};
    st.gpr[9] = 0x11223344556677FFULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[9] == 0x1122334455667700ULL, "8-bit inc wrap keeps bits 8..63");

    (void)decode_vec({0x41, 0xFE, 0xC9}, insn);  // dec r9b
    st = CpuState{};
    st.gpr[9] = 0x1122334455667700ULL;
    Interpreter::execute(st, insn);
    expect(st.gpr[9] == 0x11223344556677FFULL, "8-bit dec borrow keeps bits 8..63");

    // Memory stores write exactly `size` bytes; neighboring sentinel bytes must be untouched.
    std::array<std::uint8_t, 24> baseline{};
    for (std::size_t k = 0; k < baseline.size(); ++k) {
        baseline[k] = static_cast<std::uint8_t>(0x10U + k);
    }
    const auto run_mem_store = [&](const std::vector<std::uint8_t>& code, std::uint64_t rdx,
                                   std::size_t expected_size, const std::vector<std::uint8_t>& expected_bytes,
                                   const char* label) {
        std::array<std::uint8_t, 24> buf = baseline;
        Insn store_insn{};
        expect(decode_vec(code, store_insn) && store_insn.dst.size == expected_size, label);
        CpuState ms{};
        ms.gpr[static_cast<int>(Reg::rcx)] = reinterpret_cast<std::uintptr_t>(buf.data() + 8);
        ms.gpr[static_cast<int>(Reg::rdx)] = rdx;
        Interpreter::execute(ms, store_insn);
        const bool prefix_ok = std::memcmp(buf.data(), baseline.data(), 8) == 0;
        const bool payload_ok = std::memcmp(buf.data() + 8, expected_bytes.data(), expected_size) == 0;
        const bool suffix_ok =
            std::memcmp(buf.data() + 8 + expected_size, baseline.data() + 8 + expected_size,
                        buf.size() - 8 - expected_size) == 0;
        expect(prefix_ok && payload_ok && suffix_ok, label);
    };

    run_mem_store({0x88, 0x11}, 0xAABBCCDDEEFF0042ULL, 1, {0x42}, "mem store 1B mov [rcx], dl");
    run_mem_store({0xC6, 0x01, 0x77}, 0, 1, {0x77}, "mem store 1B mov byte [rcx], imm8");
    run_mem_store({0x80, 0x01, 0x05}, 0, 1, {0x1D}, "mem store 1B add byte [rcx], 5");
    run_mem_store({0xFE, 0x01}, 0, 1, {0x19}, "mem store 1B inc byte [rcx]");
    run_mem_store({0xFE, 0x09}, 0, 1, {0x17}, "mem store 1B dec byte [rcx]");

    run_mem_store({0x89, 0x11}, 0x1122334455667788ULL, 4, {0x88, 0x77, 0x66, 0x55},
                  "mem store 4B mov [rcx], edx");
    run_mem_store({0xC7, 0x01, 0xAA, 0xBB, 0xCC, 0xDD}, 0, 4, {0xAA, 0xBB, 0xCC, 0xDD},
                  "mem store 4B mov dword [rcx], imm32");
    run_mem_store({0x29, 0x11}, 0x01010101ULL, 4, {0x17, 0x18, 0x19, 0x1A},
                  "mem store 4B sub [rcx], edx");
    run_mem_store({0xFF, 0x01}, 0, 4, {0x19, 0x19, 0x1A, 0x1B}, "mem store 4B inc dword [rcx]");

    run_mem_store({0x48, 0x89, 0x11}, 0x0102030405060708ULL, 8,
                  {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01},
                  "mem store 8B mov [rcx], rdx");
    run_mem_store({0x48, 0xC7, 0x01, 0xFE, 0xFF, 0xFF, 0xFF}, 0, 8,
                  {0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                  "mem store 8B mov qword [rcx], -2 sign-extended");
    run_mem_store({0x48, 0x31, 0x11}, 0x00FF00FF00FF00FFULL, 8,
                  {0xE7, 0x19, 0xE5, 0x1B, 0xE3, 0x1D, 0xE1, 0x1F},
                  "mem store 8B xor [rcx], rdx");
    run_mem_store({0x48, 0xFF, 0x09}, 0, 8, {0x17, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
                  "mem store 8B dec qword [rcx]");

    // push writes exact 8 bytes at [rsp - 8] and pop restores rsp without touching neighbors.
    std::array<std::uint8_t, 24> stack_buf = baseline;
    Insn push_insn{};
    Insn pop_insn{};
    expect(decode_vec({0x41, 0x52}, push_insn) && decode_vec({0x5B}, pop_insn),
           "decode push r10 / pop rbx");
    CpuState ps{};
    const std::uint64_t initial_rsp = reinterpret_cast<std::uintptr_t>(stack_buf.data() + 16);
    ps.gpr[static_cast<int>(Reg::rsp)] = initial_rsp;
    ps.gpr[static_cast<int>(Reg::r10)] = 0x0807060504030201ULL;
    Interpreter::execute(ps, push_insn);
    static constexpr std::uint8_t kExpectedPush[8] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    expect(ps.gpr[static_cast<int>(Reg::rsp)] == initial_rsp - 8 &&
               std::memcmp(stack_buf.data(), baseline.data(), 8) == 0 &&
               std::memcmp(stack_buf.data() + 8, kExpectedPush, 8) == 0 &&
               std::memcmp(stack_buf.data() + 16, baseline.data() + 16, 8) == 0,
           "push writes exact 8 bytes at [rsp - 8] leaving neighbors untouched");
    Interpreter::execute(ps, pop_insn);
    expect(ps.gpr[static_cast<int>(Reg::rsp)] == initial_rsp &&
               ps.gpr[static_cast<int>(Reg::rbx)] == 0x0807060504030201ULL,
           "pop reads pushed qword and restores rsp");
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
}

void append_cv_record(std::vector<std::uint8_t>& out, std::uint16_t kind,
                      const std::vector<std::uint8_t>& payload, bool pad4 = true) {
    const std::size_t raw_total = 4U + payload.size();
    const std::size_t pad = pad4 ? ((4U - (raw_total % 4U)) % 4U) : 0U;
    const std::uint16_t rec_len = static_cast<std::uint16_t>(2U + payload.size() + pad);
    append_u16(out, rec_len);
    append_u16(out, kind);
    out.insert(out.end(), payload.begin(), payload.end());
    for (std::size_t i = 0; i < pad; ++i) {
        out.push_back(0U);
    }
}

void append_s_pub32(std::vector<std::uint8_t>& out, std::uint32_t flags, std::uint16_t section,
                    std::uint32_t offset, std::string_view name, bool pad4 = true) {
    std::vector<std::uint8_t> payload;
    payload.reserve(10U + name.size() + 1U);
    append_u32(payload, flags);
    append_u32(payload, offset);
    append_u16(payload, section);
    payload.insert(payload.end(), name.begin(), name.end());
    payload.push_back('\0');
    append_cv_record(out, 0x110EU, payload, pad4);
}

std::vector<std::uint8_t> make_pdb_info_stream(const PdbGuidAge& ga, std::size_t total_size = 28U) {
    std::vector<std::uint8_t> s(total_size < 28U ? 28U : total_size, 0U);
    put32(s, 0U, 20000404U);
    put32(s, 4U, 0x12345678U);
    put32(s, 8U, ga.age);
    std::memcpy(s.data() + 12U, ga.guid.data(), 16U);
    return s;
}

std::vector<std::uint8_t> make_dbi_stream(std::int32_t version_signature,
                                          std::uint16_t sym_record_stream, std::uint32_t age = 1U,
                                          std::size_t total_size = 64U) {
    std::vector<std::uint8_t> s(total_size < 64U ? 64U : total_size, 0U);
    put32(s, 0U, static_cast<std::uint32_t>(version_signature));
    put32(s, 4U, 19990903U);
    put32(s, 8U, age);
    put16(s, 12U, 0xFFFFU);
    put16(s, 16U, 0xFFFFU);
    put16(s, 20U, sym_record_stream);
    put16(s, 58U, 0x8664U);
    return s;
}

struct SyntheticMsfStream {
    bool empty = false;
    std::vector<std::uint8_t> data;
};

struct BuiltMsf {
    std::vector<std::uint8_t> bytes;
    std::uint32_t block_size = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t block_map_addr = 0;
    std::vector<std::uint32_t> dir_blocks;
    std::vector<std::size_t> dir_stream_size_offsets;
    std::vector<std::vector<std::size_t>> dir_stream_block_offsets;
    std::vector<std::vector<std::uint32_t>> stream_blocks;
};

std::size_t msf_dir_file_off(const BuiltMsf& m, std::size_t dir_off) {
    const std::size_t blk_idx = dir_off / m.block_size;
    const std::size_t in_blk = dir_off % m.block_size;
    return static_cast<std::size_t>(m.dir_blocks[blk_idx]) * m.block_size + in_blk;
}

BuiltMsf build_synthetic_msf(std::uint32_t block_size,
                             const std::vector<SyntheticMsfStream>& streams) {
    BuiltMsf out;
    out.block_size = block_size;
    out.block_map_addr = 3U;

    const std::uint32_t num_streams = static_cast<std::uint32_t>(streams.size());
    std::vector<std::uint32_t> stream_block_counts(streams.size(), 0U);
    std::vector<std::size_t> active_streams;
    std::uint32_t total_stream_blocks = 0U;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (!streams[i].empty && !streams[i].data.empty()) {
            const std::uint32_t cnt = static_cast<std::uint32_t>(
                (streams[i].data.size() + block_size - 1U) / block_size);
            stream_block_counts[i] = cnt;
            total_stream_blocks += cnt;
            active_streams.push_back(i);
        }
    }

    const std::uint32_t dir_bytes = 4U + 4U * num_streams + 4U * total_stream_blocks;
    const std::uint32_t num_dir_blocks = (dir_bytes + block_size - 1U) / block_size;
    const std::uint32_t num_map_blocks = (num_dir_blocks * 4U + block_size - 1U) / block_size;
    const std::uint32_t needed_blocks = num_dir_blocks + total_stream_blocks;

    // Skip superblock (0), FPM1/FPM2 intervals, and the contiguous block map.
    const std::uint32_t fpm_interval = block_size * 8U;
    const std::uint32_t pool_target = needed_blocks + 4U;
    std::vector<std::uint32_t> pool;
    pool.reserve(pool_target);
    std::uint32_t cand = out.block_map_addr + num_map_blocks;
    while (pool.size() < pool_target) {
        const std::uint32_t mod = cand % fpm_interval;
        if (mod != 1U && mod != 2U) {
            pool.push_back(cand);
        }
        ++cand;
    }

    // Even slots then odd slots ensure multi-block streams and directories are non-contiguous,
    // leaving unused free blocks so the free block map contains both allocated and free bits.
    std::vector<std::uint32_t> alloc_order;
    alloc_order.reserve(needed_blocks);
    for (std::size_t i = 0; i < pool.size() && alloc_order.size() < needed_blocks; i += 2U) {
        alloc_order.push_back(pool[i]);
    }
    for (std::size_t i = 1U; i < pool.size() && alloc_order.size() < needed_blocks; i += 2U) {
        alloc_order.push_back(pool[i]);
    }

    out.num_blocks = pool.back() + 1U;
    out.bytes.assign(static_cast<std::size_t>(out.num_blocks) * block_size, 0U);

    out.dir_blocks.resize(num_dir_blocks);
    out.stream_blocks.resize(streams.size());
    for (const std::size_t i : active_streams) {
        out.stream_blocks[i].resize(stream_block_counts[i]);
    }

    std::size_t next_alloc = 0;
    std::uint32_t dir_assigned = 0;
    std::vector<std::uint32_t> stream_assigned(streams.size(), 0U);
    while (next_alloc < needed_blocks) {
        for (const std::size_t i : active_streams) {
            if (stream_assigned[i] < stream_block_counts[i]) {
                out.stream_blocks[i][stream_assigned[i]++] = alloc_order[next_alloc++];
            }
        }
        if (dir_assigned < num_dir_blocks) {
            out.dir_blocks[dir_assigned++] = alloc_order[next_alloc++];
        }
    }

    static constexpr std::uint8_t kMagic[32] = {
        'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ', 'C', '/', 'C', '+', '+', ' ',
        'M', 'S', 'F', ' ', '7', '.', '0', '0', '\r', '\n', 0x1A, 'D', 'S', 0x00, 0x00, 0x00,
    };
    std::memcpy(out.bytes.data(), kMagic, sizeof(kMagic));
    put32(out.bytes, 32U, block_size);
    put32(out.bytes, 36U, 1U);
    put32(out.bytes, 40U, out.num_blocks);
    put32(out.bytes, 44U, dir_bytes);
    put32(out.bytes, 48U, 0U);
    put32(out.bytes, 52U, out.block_map_addr);

    for (const std::size_t i : active_streams) {
        const auto& s = streams[i];
        std::size_t copied = 0;
        for (const std::uint32_t blk : out.stream_blocks[i]) {
            const std::size_t rem = s.data.size() - copied;
            const std::size_t chunk = rem < block_size ? rem : block_size;
            std::memcpy(out.bytes.data() + static_cast<std::size_t>(blk) * block_size,
                        s.data.data() + copied, chunk);
            copied += chunk;
        }
    }

    std::vector<std::uint8_t> dir(dir_bytes, 0U);
    put32(dir, 0U, num_streams);
    out.dir_stream_size_offsets.resize(streams.size());
    out.dir_stream_block_offsets.resize(streams.size());
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const std::size_t off = 4U + i * 4U;
        out.dir_stream_size_offsets[i] = off;
        const std::uint32_t sz = streams[i].empty
                                     ? 0xFFFFFFFFU
                                     : static_cast<std::uint32_t>(streams[i].data.size());
        put32(dir, off, sz);
    }
    std::size_t pos = 4U + static_cast<std::size_t>(num_streams) * 4U;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        for (const std::uint32_t blk : out.stream_blocks[i]) {
            out.dir_stream_block_offsets[i].push_back(pos);
            put32(dir, pos, blk);
            pos += 4U;
        }
    }

    std::size_t dir_copied = 0;
    for (const std::uint32_t blk : out.dir_blocks) {
        const std::size_t rem = dir.size() - dir_copied;
        const std::size_t chunk = rem < block_size ? rem : block_size;
        std::memcpy(out.bytes.data() + static_cast<std::size_t>(blk) * block_size,
                    dir.data() + dir_copied, chunk);
        dir_copied += chunk;
    }

    const std::size_t map_off = static_cast<std::size_t>(out.block_map_addr) * block_size;
    for (std::size_t i = 0; i < out.dir_blocks.size(); ++i) {
        put32(out.bytes, map_off + i * 4U, out.dir_blocks[i]);
    }

    std::vector<bool> used(out.num_blocks, false);
    used[0] = true;
    for (std::uint32_t base = 0; base < out.num_blocks; base += fpm_interval) {
        if (base + 1U < out.num_blocks) {
            used[base + 1U] = true;
        }
        if (base + 2U < out.num_blocks) {
            used[base + 2U] = true;
        }
    }
    for (std::uint32_t b = 0; b < num_map_blocks; ++b) {
        used[out.block_map_addr + b] = true;
    }
    for (const std::uint32_t blk : out.dir_blocks) {
        used[blk] = true;
    }
    for (const auto& blks : out.stream_blocks) {
        for (const std::uint32_t blk : blks) {
            used[blk] = true;
        }
    }
    for (std::uint32_t base = 0; base < out.num_blocks; base += fpm_interval) {
        for (const std::uint32_t fpm_offset : {1U, 2U}) {
            if (base + fpm_offset >= out.num_blocks) {
                continue;
            }
            const std::size_t fpm_byte_off =
                static_cast<std::size_t>(base + fpm_offset) * block_size;
            std::memset(out.bytes.data() + fpm_byte_off, 0xFF, block_size);
            for (std::uint32_t rel = 0; rel < fpm_interval && base + rel < out.num_blocks; ++rel) {
                if (used[base + rel]) {
                    out.bytes[fpm_byte_off + (rel / 8U)] &=
                        static_cast<std::uint8_t>(~(1U << (rel % 8U)));
                }
            }
        }
    }

    return out;
}

std::vector<std::uint8_t> make_synthetic_pe_with_codeview(
    const std::array<std::uint8_t, 16>& guid, std::uint32_t age, std::string_view pdb_path) {
    std::vector<std::uint8_t> f = make_base_synthetic_pe();
    f.resize(0x800, 0U);
    put16(f, 0x46, 3);
    const std::size_t opt = 0x58;
    put32(f, opt + 56, 0x4000);
    put32(f, opt + 112 + 6 * 8, 0x3000);
    put32(f, opt + 112 + 6 * 8 + 4, 56);

    const std::size_t sec3 = opt + 0xF0 + 2U * 40U;
    std::memcpy(f.data() + sec3, ".rdata", 6);
    put32(f, sec3 + 8, 0x100);
    put32(f, sec3 + 12, 0x3000);
    put32(f, sec3 + 16, 0x200);
    put32(f, sec3 + 20, 0x600);
    put32(f, sec3 + 36, 0x40000040);

    // Entry 0: non-CodeView entry (IMAGE_DEBUG_TYPE_VC_FEATURE = 12) must be skipped.
    put32(f, 0x600 + 12, 12U);

    // Entry 1: CodeView RSDS entry pointing to raw file offset 0x640.
    const std::uint32_t rsds_size = static_cast<std::uint32_t>(24U + pdb_path.size() + 1U);
    put32(f, 0x61C + 12, 2U);
    put32(f, 0x61C + 16, rsds_size);
    put32(f, 0x61C + 20, 0x3040U);
    put32(f, 0x61C + 24, 0x640U);

    std::memcpy(f.data() + 0x640, "RSDS", 4);
    std::memcpy(f.data() + 0x644, guid.data(), 16);
    put32(f, 0x654, age);
    if (!pdb_path.empty()) {
        std::memcpy(f.data() + 0x658, pdb_path.data(), pdb_path.size());
    }
    f[0x658 + pdb_path.size()] = '\0';
    return f;
}

bool has_non_contiguous_blocks(const std::vector<std::uint32_t>& blocks) {
    for (std::size_t i = 1U; i < blocks.size(); ++i) {
        if (blocks[i] != blocks[i - 1U] + 1U) {
            return true;
        }
    }
    return false;
}

void test_pdb_reader() {
    const PdbGuidAge expected_ga{
        {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
         0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F},
        3U,
    };

    // 1. Extended PE CodeView RSDS parsing and rejections.
    const std::string expected_pdb_path = "C:\\WSE2\\mb_warband_wse2_x64.pdb";
    const std::vector<std::uint8_t> pe_cv =
        make_synthetic_pe_with_codeview(expected_ga.guid, expected_ga.age, expected_pdb_path);
    PeImage pe_img;
    PeCodeView cv;
    std::string err;
    expect(parse_pe(pe_cv.data(), pe_cv.size(), pe_img, &err) && pe_img.sections.size() == 3U &&
               pe_img.debug_rva == 0x3000U && pe_img.debug_size == 56U,
           "parse_pe reads data directory 6 and .rdata section");
    expect(read_pe_codeview(pe_cv.data(), pe_cv.size(), pe_img, cv, &err) &&
               cv.guid == expected_ga.guid && cv.age == expected_ga.age &&
               cv.pdb_path == expected_pdb_path,
           "read_pe_codeview extracts RSDS GUID, age and PDB path");

    {
        const std::vector<std::uint8_t> base_pe = make_base_synthetic_pe();
        PeImage base_img;
        expect(parse_pe(base_pe.data(), base_pe.size(), base_img, &err) &&
                   !read_pe_codeview(base_pe.data(), base_pe.size(), base_img, cv, &err) &&
                   !err.empty(),
               "read_pe_codeview rejects PE without a debug directory");
    }
    {
        err.clear();
        expect(!read_pe_codeview(nullptr, 0U, pe_img, cv, &err) && !err.empty(),
               "read_pe_codeview rejects empty buffer");
    }
    {
        std::vector<std::uint8_t> no_cv = pe_cv;
        put32(no_cv, 0x61C + 12, 12U);
        err.clear();
        expect(!read_pe_codeview(no_cv.data(), no_cv.size(), pe_img, cv, &err) && !err.empty(),
               "read_pe_codeview rejects debug directory with no CodeView entry");
    }
    {
        std::vector<std::uint8_t> wrong_sig = pe_cv;
        std::memcpy(wrong_sig.data() + 0x640, "NB10", 4);
        err.clear();
        expect(!read_pe_codeview(wrong_sig.data(), wrong_sig.size(), pe_img, cv, &err) &&
                   !err.empty(),
               "read_pe_codeview rejects non-RSDS CodeView signature");
    }
    for (const std::uint32_t bad_len : {3U, 10U, 24U}) {
        std::vector<std::uint8_t> trunc_cv = pe_cv;
        put32(trunc_cv, 0x61C + 16, bad_len);
        err.clear();
        expect(!read_pe_codeview(trunc_cv.data(), trunc_cv.size(), pe_img, cv, &err) &&
                   !err.empty(),
               "read_pe_codeview rejects truncated RSDS record");
    }
    {
        std::vector<std::uint8_t> unterminated = pe_cv;
        put32(unterminated, 0x61C + 16, 28U);
        std::memcpy(unterminated.data() + 0x658, "abcd", 4);
        err.clear();
        expect(!read_pe_codeview(unterminated.data(), unterminated.size(), pe_img, cv, &err) &&
                   !err.empty(),
               "read_pe_codeview rejects unterminated RSDS PDB path");
    }
    {
        std::vector<std::uint8_t> oob_cv = pe_cv;
        put32(oob_cv, 0x61C + 16, 32U);
        put32(oob_cv, 0x61C + 24, 0x7F0U);
        err.clear();
        expect(!read_pe_codeview(oob_cv.data(), oob_cv.size(), pe_img, cv, &err) && !err.empty(),
               "read_pe_codeview rejects CodeView raw range past EOF");
    }
    {
        PeImage bad_dir_img = pe_img;
        bad_dir_img.debug_rva = 0x9000U;
        err.clear();
        expect(!read_pe_codeview(pe_cv.data(), pe_cv.size(), bad_dir_img, cv, &err) &&
                   !err.empty(),
               "read_pe_codeview rejects unbacked debug directory RVA");
    }

    // 2. Synthetic 512-byte MSF PDB: empty streams, multi-block non-contiguous streams and
    //    directory, interleaved non-S_PUB32 records, unaligned RecordLen, and VA resolution.
    std::vector<std::uint8_t> sym512;
    append_cv_record(sym512, 0x110DU, std::vector<std::uint8_t>(16U, 0xABU));  // S_GDATA32
    append_s_pub32(sym512, 2U, 1U, 0x10U, "?smax@@YAHHH@Z", true);
    append_cv_record(sym512, 0x0006U, {});  // RecordLen == 2, zero payload
    append_s_pub32(sym512, 0U, 2U, 0x04U, "__pdata_sym", false);  // unaligned (26 bytes)
    append_cv_record(sym512, 0x1108U, {1U, 0U, 0U, 0U, 'T', '\0'});  // S_UDT
    append_s_pub32(sym512, 1U, 3U, 0x40U, "??_C@_04HIBGFPH@RSDS?$AA@", true);
    append_s_pub32(sym512, 0U, 0U, 0x1234U, "__abs_sym", true);
    append_s_pub32(sym512, 0U, 99U, 0U, "__oob_sec_sym", true);
    for (int k = 0; k < 40; ++k) {
        append_cv_record(sym512, 0x1107U, std::vector<std::uint8_t>(24U, 0x55U));  // S_CONSTANT
    }

    std::vector<SyntheticMsfStream> streams512(140U);
    streams512[0].empty = true;
    streams512[1].data = make_pdb_info_stream(expected_ga, 600U);
    streams512[2].empty = true;
    streams512[3].data = make_dbi_stream(-1, 4U, expected_ga.age, 580U);
    streams512[4].data = sym512;
    for (std::size_t i = 5U; i < streams512.size(); ++i) {
        streams512[i].empty = true;
    }

    const BuiltMsf msf512 = build_synthetic_msf(512U, streams512);
    expect(msf512.dir_blocks.size() >= 2U && has_non_contiguous_blocks(msf512.dir_blocks) &&
               msf512.stream_blocks[1].size() >= 2U &&
               msf512.stream_blocks[3].size() >= 2U &&
               msf512.stream_blocks[4].size() >= 3U &&
               has_non_contiguous_blocks(msf512.stream_blocks[4]),
           "synthetic 512B MSF spans non-contiguous directory and stream blocks");

    std::vector<PdbPublic> publics;
    PdbGuidAge ga;
    expect(read_pdb_publics(msf512.bytes.data(), msf512.bytes.size(), publics, &ga, &err) &&
               ga.guid == expected_ga.guid && ga.age == expected_ga.age,
           "read_pdb_publics extracts matching GUID/age from stream 1 across 512B blocks");
    expect(publics.size() == 5U &&
               publics[0].name == "?smax@@YAHHH@Z" && publics[0].section == 1U &&
               publics[0].offset == 0x10U && publics[0].flags == 2U &&
               publics[1].name == "__pdata_sym" && publics[1].section == 2U &&
               publics[1].offset == 0x04U && publics[1].flags == 0U &&
               publics[2].name == "??_C@_04HIBGFPH@RSDS?$AA@" && publics[2].section == 3U &&
               publics[2].offset == 0x40U && publics[2].flags == 1U &&
               publics[3].name == "__abs_sym" && publics[3].section == 0U &&
               publics[4].name == "__oob_sec_sym" && publics[4].section == 99U,
           "read_pdb_publics collects S_PUB32 records and honours exact RecordLen");

    expect(pdb_public_va(publics, "?smax@@YAHHH@Z", pe_img.sections, pe_img.image_base) ==
                   0x140001010ULL &&
               pdb_public_va(publics, "__pdata_sym", pe_img.sections, pe_img.image_base) ==
                   0x140002004ULL &&
               pdb_public_va(publics, "??_C@_04HIBGFPH@RSDS?$AA@", pe_img.sections,
                             pe_img.image_base) == 0x140003040ULL &&
               !pdb_public_va(publics, "?smax", pe_img.sections, pe_img.image_base).has_value() &&
               !pdb_public_va(publics, "missing", pe_img.sections, pe_img.image_base).has_value() &&
               !pdb_public_va(publics, "__abs_sym", pe_img.sections, pe_img.image_base)
                    .has_value() &&
               !pdb_public_va(publics, "__oob_sec_sym", pe_img.sections, pe_img.image_base)
                    .has_value(),
           "pdb_public_va resolves 1-based sections and rejects absent or invalid sections");

    {
        std::vector<PdbPublic> publics_no_info;
        expect(read_pdb_publics(msf512.bytes.data(), msf512.bytes.size(), publics_no_info, nullptr,
                                &err) &&
                   publics_no_info.size() == 5U,
               "read_pdb_publics accepts null PdbGuidAge output pointer");
    }

    // 3. Block size 4096 with multi-block stream and multi-block directory, plus 1024 and 2048.
    {
        std::vector<std::uint8_t> sym4096;
        append_s_pub32(sym4096, 2U, 1U, 0x100U, "_first_4k");
        for (int k = 0; k < 180; ++k) {
            append_cv_record(sym4096, 0x1107U, std::vector<std::uint8_t>(24U, 0x33U));
        }
        append_s_pub32(sym4096, 0U, 3U, 0x200U, "_second_4k");

        std::vector<SyntheticMsfStream> streams4096(1105U);
        streams4096[0].empty = true;
        streams4096[1].data = make_pdb_info_stream(expected_ga);
        streams4096[2].empty = true;
        streams4096[3].data = make_dbi_stream(-1, 4U, expected_ga.age);
        streams4096[4].data = sym4096;
        for (std::size_t i = 5U; i < streams4096.size(); ++i) {
            streams4096[i].empty = true;
        }
        const BuiltMsf msf4096 = build_synthetic_msf(4096U, streams4096);
        expect(msf4096.dir_blocks.size() >= 2U && msf4096.stream_blocks[4].size() >= 2U &&
                   read_pdb_publics(msf4096.bytes.data(), msf4096.bytes.size(), publics, &ga,
                                    &err) &&
                   publics.size() == 2U && publics[0].name == "_first_4k" &&
                   publics[1].name == "_second_4k" && ga.guid == expected_ga.guid,
               "read_pdb_publics handles 4096-byte blocks with multi-block stream and directory");

        for (const std::uint32_t bs : {1024U, 2048U}) {
            std::vector<SyntheticMsfStream> streams_mid(5U);
            streams_mid[0].empty = true;
            streams_mid[1].data = make_pdb_info_stream(expected_ga);
            streams_mid[2].empty = true;
            streams_mid[3].data = make_dbi_stream(-1, 4U, expected_ga.age);
            append_s_pub32(streams_mid[4].data, 2U, 1U, 0x20U, "_mid_bs");
            const BuiltMsf msf_mid = build_synthetic_msf(bs, streams_mid);
            expect(read_pdb_publics(msf_mid.bytes.data(), msf_mid.bytes.size(), publics, &ga,
                                    &err) &&
                       publics.size() == 1U && publics[0].name == "_mid_bs",
                   "read_pdb_publics handles 1024- and 2048-byte block sizes");
        }
    }

    // 4. Multi-block block map (512B blocks, >128 directory blocks => >1 block-map block).
    {
        std::vector<SyntheticMsfStream> streams_map(16500U);
        streams_map[0].empty = true;
        streams_map[1].data = make_pdb_info_stream(expected_ga);
        streams_map[2].empty = true;
        streams_map[3].data = make_dbi_stream(-1, 4U, expected_ga.age);
        append_s_pub32(streams_map[4].data, 2U, 1U, 0x80U, "_multi_map");
        for (std::size_t i = 5U; i < streams_map.size(); ++i) {
            streams_map[i].empty = true;
        }
        const BuiltMsf msf_map = build_synthetic_msf(512U, streams_map);
        expect(msf_map.dir_blocks.size() > 128U &&
                   read_pdb_publics(msf_map.bytes.data(), msf_map.bytes.size(), publics, &ga,
                                    &err) &&
                   publics.size() == 1U && publics[0].name == "_multi_map" &&
                   ga.age == expected_ga.age,
               "read_pdb_publics handles a block map spanning multiple blocks");
    }

    // 5. Empty SymRecordStream (both 0xFFFFFFFF and zero-byte stream) yields 0 publics cleanly.
    {
        std::vector<SyntheticMsfStream> empty_sym_streams(5U);
        empty_sym_streams[0].empty = true;
        empty_sym_streams[1].data = make_pdb_info_stream(expected_ga);
        empty_sym_streams[2].empty = true;
        empty_sym_streams[3].data = make_dbi_stream(-1, 4U, expected_ga.age);
        empty_sym_streams[4].empty = true;
        const BuiltMsf msf_empty_ff = build_synthetic_msf(512U, empty_sym_streams);
        expect(read_pdb_publics(msf_empty_ff.bytes.data(), msf_empty_ff.bytes.size(), publics,
                                &ga, &err) &&
                   publics.empty() && ga.guid == expected_ga.guid,
               "read_pdb_publics accepts empty (0xFFFFFFFF) SymRecordStream");

        empty_sym_streams[4].empty = false;
        const BuiltMsf msf_empty_0 = build_synthetic_msf(512U, empty_sym_streams);
        expect(read_pdb_publics(msf_empty_0.bytes.data(), msf_empty_0.bytes.size(), publics, &ga,
                                &err) &&
                   publics.empty(),
               "read_pdb_publics accepts zero-byte SymRecordStream");
    }

    // 6. Rejections: every malformed MSF, DBI and CodeView case returns false with an error.
    const auto expect_pdb_reject = [&](const std::uint8_t* ptr, std::size_t len,
                                       const char* label) {
        std::vector<PdbPublic> out = {{"stale", 1U, 0U, 0U}};
        PdbGuidAge out_ga{};
        out_ga.age = 999U;
        std::string rej_err;
        const bool ok = read_pdb_publics(ptr, len, out, &out_ga, &rej_err);
        expect(!ok && out.empty() && out_ga.age == 0U && !rej_err.empty(), label);
    };

    std::vector<SyntheticMsfStream> base_streams(5U);
    base_streams[0].empty = true;
    base_streams[1].data = make_pdb_info_stream(expected_ga);
    base_streams[2].empty = true;
    base_streams[3].data = make_dbi_stream(-1, 4U, expected_ga.age);
    append_s_pub32(base_streams[4].data, 2U, 1U, 0x0U, "_ok");
    const BuiltMsf base_msf = build_synthetic_msf(512U, base_streams);

    expect_pdb_reject(nullptr, 0U, "read_pdb_publics rejects null buffer");
    expect_pdb_reject(base_msf.bytes.data(), 55U,
                      "read_pdb_publics rejects truncated superblock");

    {
        auto bad = base_msf.bytes;
        bad[0] = 'X';
        expect_pdb_reject(bad.data(), bad.size(), "read_pdb_publics rejects bad MSF magic");
    }
    for (const std::uint32_t bad_bs : {0U, 256U, 1536U, 8192U}) {
        auto bad = base_msf.bytes;
        put32(bad, 32U, bad_bs);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects unsupported block size");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, 40U, 0U);
        expect_pdb_reject(bad.data(), bad.size(), "read_pdb_publics rejects zero block count");
    }
    {
        auto bad = base_msf.bytes;
        bad.pop_back();
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects NumBlocks * BlockSize > size");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, 44U, 3U);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects NumDirectoryBytes < 4");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, 52U, base_msf.num_blocks);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects BlockMapAddr past end");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, static_cast<std::size_t>(base_msf.block_map_addr) * base_msf.block_size,
              base_msf.num_blocks);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects directory block index past end");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, msf_dir_file_off(base_msf, base_msf.dir_stream_block_offsets[4][0]),
              base_msf.num_blocks);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects stream block index past end");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, msf_dir_file_off(base_msf, base_msf.dir_stream_size_offsets[4]), 513U);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects stream size larger than its blocks");
    }
    {
        auto bad = base_msf.bytes;
        put32(bad, msf_dir_file_off(base_msf, base_msf.dir_stream_size_offsets[4]), 0xFFFFFFFEU);
        expect_pdb_reject(bad.data(), bad.size(),
                          "read_pdb_publics rejects stream size larger than file");
    }
    {
        auto bad_streams = base_streams;
        bad_streams[1].data.resize(27U);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects truncated PDB info stream");
    }
    {
        auto bad_streams = base_streams;
        bad_streams[3].data.resize(63U);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects truncated DBI stream header");
    }
    {
        auto bad_streams = base_streams;
        bad_streams[3].data = make_dbi_stream(0, 4U, expected_ga.age);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects DBI with bad VersionSignature");
    }
    for (const std::uint16_t bad_sym_idx : {std::uint16_t{5U}, std::uint16_t{0xFFFFU}}) {
        auto bad_streams = base_streams;
        bad_streams[3].data = make_dbi_stream(-1, bad_sym_idx, expected_ga.age);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects SymRecordStream index out of range");
    }
    for (const std::size_t tail : {1U, 2U, 3U}) {
        auto bad_streams = base_streams;
        bad_streams[4].data.insert(bad_streams[4].data.end(), tail, 0x01U);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects truncated trailing CodeView header");
    }
    {
        auto bad_streams = base_streams;
        append_u16(bad_streams[4].data, 1U);  // RecordLen < 2
        append_u16(bad_streams[4].data, 0x110EU);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects CodeView RecordLen < 2");
    }
    {
        auto bad_streams = base_streams;
        append_u16(bad_streams[4].data, 10U);  // payload 8 < 10 required for S_PUB32
        append_u16(bad_streams[4].data, 0x110EU);
        bad_streams[4].data.insert(bad_streams[4].data.end(), 8U, 0U);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects truncated S_PUB32 fields");
    }
    {
        auto bad_streams = base_streams;
        append_u16(bad_streams[4].data, 14U);  // 2 + 10 fixed + 2 non-NUL name bytes
        append_u16(bad_streams[4].data, 0x110EU);
        append_u32(bad_streams[4].data, 2U);
        append_u32(bad_streams[4].data, 0U);
        append_u16(bad_streams[4].data, 1U);
        bad_streams[4].data.push_back('a');
        bad_streams[4].data.push_back('b');
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects unterminated S_PUB32 name");
    }
    for (const std::uint16_t rec_kind : {std::uint16_t{0x110EU}, std::uint16_t{0x110DU}}) {
        auto bad_streams = base_streams;
        append_u16(bad_streams[4].data, 0x20U);
        append_u16(bad_streams[4].data, rec_kind);
        bad_streams[4].data.insert(bad_streams[4].data.end(), 12U, 0U);
        const BuiltMsf bad = build_synthetic_msf(512U, bad_streams);
        expect_pdb_reject(bad.bytes.data(), bad.bytes.size(),
                          "read_pdb_publics rejects CodeView record length past stream");
    }
}

// --- R1 runtime (`runtime.h`): plain C++ functions stand in for translated ones -------------

namespace rt_test {

constexpr std::uint64_t kLeafVa = 0x140010000ULL;
constexpr std::uint64_t kCallerVa = 0x140010100ULL;
constexpr std::uint64_t kSlotVa = 0x1400F0008ULL;

void guest_ret(CpuState* s) {
    std::uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(s->gpr[4]), 8);
    s->gpr[4] += 8;
    s->rip = ret;
}

void leaf(CpuState* s) {  // rax = rcx * 3
    s->gpr[0] = s->gpr[1] * 3;
    guest_ret(s);
}

// What the lifter emits for `call [dispatch target]`: push the return VA, flush, call the
// dispatch hook, then check `rip`.
void push_and_hook(CpuState* s, std::uint64_t ret_va, std::uint64_t target, bool import) {
    s->gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s->gpr[4]), &ret_va, 8);
    if (import) {
        s->hooks->import_call(s, target);
    } else {
        s->hooks->dispatch(s, target);
    }
    if (s->rip != ret_va) {
        s->hooks->bad_return(s, ret_va);
    }
}

void caller(CpuState* s) {  // rax = leaf(rcx) + 1 through the dispatcher
    push_and_hook(s, kCallerVa + 0x10, kLeafVa, false);
    s->gpr[0] += 1;
    guest_ret(s);
}

}  // namespace rt_test

void test_runtime_dispatch() {
    using namespace rt_test;
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    const FnRow rows[] = {{kCallerVa, &caller}, {kLeafVa, &leaf}, {kCallerVa, &leaf}};
    rt.add_table(rows, 3);
    expect(rt.function_count() == 2, "runtime: duplicate VAs collapse to one row");
    expect(rt.find(kLeafVa) == &leaf && rt.find(kCallerVa) == &caller,
           "runtime: unsorted rows sort; the first function of a duplicate VA wins");
    expect(rt.find(kLeafVa + 1) == nullptr && rt.find(0) == nullptr && rt.find(~0ULL) == nullptr,
           "runtime: VAs between, below and above the table are not found");

    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    s.gpr[1] = 14;
    const std::uint64_t rsp0 = s.gpr[4];
    expect(rt.run(s, kCallerVa) && s.gpr[0] == 43 && s.rip == GuestStack::kReturnSentinel &&
               s.gpr[4] == rsp0 + 8 && faults.empty(),
           "runtime: nested dispatch runs and returns to the sentinel with rsp popped");
    expect(s.hooks == rt.hooks() && rt.hooks()->context == &rt, "runtime: attach sets the hook table");

    stack.prepare(s);
    expect(!rt.run(s, 0x140099999ULL) && faults.size() == 1 &&
               faults[0].kind == FaultKind::missing_function && faults[0].va == 0x140099999ULL &&
               s.rip == Runtime::kFaultRip,
           "runtime: an unknown entry VA faults with its address");

    // A dispatched call to a missing VA faults once (the caller's return check stays quiet).
    faults.clear();
    stack.prepare(s);
    rt.attach(s);
    push_and_hook(&s, 0x140020000ULL, 0x140077777ULL, false);
    expect(faults.size() == 1 && faults[0].kind == FaultKind::missing_function &&
               faults[0].va == 0x140077777ULL,
           "runtime: a missing dispatch target reports once, not again as a bad return");

    // Imports: no shim -> fault; a shim pops the return VA like the real callee.
    faults.clear();
    stack.prepare(s);
    rt.attach(s);
    push_and_hook(&s, 0x140020010ULL, kSlotVa, true);
    expect(faults.size() == 1 && faults[0].kind == FaultKind::missing_import && faults[0].va == kSlotVa,
           "runtime: an import with no shim faults with its IAT slot");
    rt.set_import(kSlotVa, [](CpuState& st) {
        st.gpr[0] = 0x5151;
        Runtime::return_from_import(st);
    });
    faults.clear();
    stack.prepare(s);
    rt.attach(s);
    const std::uint64_t rsp1 = s.gpr[4];
    push_and_hook(&s, 0x140020020ULL, kSlotVa, true);
    expect(faults.empty() && s.gpr[0] == 0x5151 && s.rip == 0x140020020ULL && s.gpr[4] == rsp1,
           "runtime: an import shim returns to the caller with rsp balanced");

    // A callee that returns elsewhere is reported with the expected VA.
    faults.clear();
    stack.prepare(s);
    rt.attach(s);
    s.rip = 0x140030000ULL;
    s.hooks->bad_return(&s, 0x140020030ULL);
    expect(faults.size() == 1 && faults[0].kind == FaultKind::bad_return &&
               faults[0].va == 0x140020030ULL && faults[0].rip == 0x140030000ULL,
           "runtime: bad_return reports the expected VA and the actual rip");

    // Unsupported code carries the translator's reason.
    faults.clear();
    s.hooks->unsupported(&s, 0x140040000ULL, "fld");
    expect(faults.size() == 1 && faults[0].kind == FaultKind::unsupported && faults[0].what == "fld" &&
               s.rip == Runtime::kFaultRip,
           "runtime: unsupported reports the instruction VA and reason");

    // Segment bases.
    faults.clear();
    s.rip = 0x140050000ULL;
    expect(s.hooks->segment_base(&s, Runtime::kSegGs) == 0 && faults.size() == 1 &&
               faults[0].kind == FaultKind::segment && s.rip == 0x140050000ULL,
           "runtime: an unset segment base faults and keeps rip");
    rt.set_segment_base(Runtime::kSegGs, 0x7FF000000000ULL);
    rt.set_segment_base(Runtime::kSegFs, 0x1000ULL);
    expect(s.hooks->segment_base(&s, Runtime::kSegGs) == 0x7FF000000000ULL &&
               s.hooks->segment_base(&s, Runtime::kSegFs) == 0x1000ULL && faults.size() == 1,
           "runtime: set segment bases are returned");
    expect(rt.fault_count == 6, "runtime: every fault is counted");

    // div128: rdx:rax / divisor, quotient -> rax, remainder -> rdx, 0 on #DE.
    CpuState q;
    rt.attach(q);
    q.gpr[2] = 1;
    q.gpr[0] = 5;  // 2^64 + 5
    expect(q.hooks->div128(&q, 3, 0) == 1 && q.gpr[0] == 0x5555555555555557ULL && q.gpr[2] == 0,
           "runtime: div128 unsigned (2^64 + 5) / 3");
    q.gpr[2] = 7;
    q.gpr[0] = 0;
    expect(q.hooks->div128(&q, 7, 0) == 0 && q.gpr[2] == 7, "runtime: div128 unsigned quotient overflow is #DE");
    expect(q.hooks->div128(&q, 0, 0) == 0 && q.hooks->div128(&q, 0, 1) == 0, "runtime: div128 by zero is #DE");
    q.gpr[2] = ~0ULL;
    q.gpr[0] = 0xFFFFFFFF00000000ULL;  // -2^32 as a 128-bit value
    expect(q.hooks->div128(&q, static_cast<std::uint64_t>(-3), 1) == 1 && q.gpr[0] == 0x55555555ULL &&
               q.gpr[2] == static_cast<std::uint64_t>(-1),
           "runtime: div128 signed -2^32 / -3 = 1431655765 rem -1");
    q.gpr[2] = 0;
    q.gpr[0] = 0x8000000000000000ULL;  // +2^63: positive 128-bit value
    expect(q.hooks->div128(&q, 1, 1) == 0, "runtime: div128 signed 2^63 / 1 overflows (#DE)");
    q.gpr[2] = ~0ULL;
    q.gpr[0] = 0x8000000000000000ULL;  // -2^63
    expect(q.hooks->div128(&q, static_cast<std::uint64_t>(-1), 1) == 0,
           "runtime: div128 signed INT64_MIN / -1 overflows (#DE)");

    // cpuid: the fixed profile (vendor, SSE..SSE4.1 + POPCNT, no AVX / SSE4.2 / OSXSAVE).
    auto cpuid = [&](std::uint32_t leaf) {
        q.gpr[0] = leaf;
        q.gpr[1] = 0;
        q.hooks->cpuid(&q);
    };
    cpuid(0);
    char vendor[13] = {};
    std::memcpy(vendor, &q.gpr[3], 4);
    std::memcpy(vendor + 4, &q.gpr[2], 4);
    std::memcpy(vendor + 8, &q.gpr[1], 4);
    expect(q.gpr[0] == 1 && std::string(vendor) == "WSE2AArch64 ", "runtime: cpuid 0 vendor and max leaf");
    cpuid(1);
    const auto ecx = static_cast<std::uint32_t>(q.gpr[1]);
    const auto edx = static_cast<std::uint32_t>(q.gpr[2]);
    expect((edx >> 25 & 1U) == 1 && (edx >> 26 & 1U) == 1 && (ecx & 1U) == 1 && (ecx >> 9 & 1U) == 1 &&
               (ecx >> 19 & 1U) == 1 && (ecx >> 23 & 1U) == 1,
           "runtime: cpuid 1 advertises SSE SSE2 SSE3 SSSE3 SSE4.1 POPCNT");
    expect((ecx >> 20 & 1U) == 0 && (ecx >> 27 & 1U) == 0 && (ecx >> 28 & 1U) == 0 && (ecx >> 12 & 1U) == 0,
           "runtime: cpuid 1 hides SSE4.2, OSXSAVE, AVX and FMA");
    cpuid(7);
    expect(q.gpr[0] == 0 && q.gpr[3] == 0 && q.gpr[1] == 0 && q.gpr[2] == 0, "runtime: cpuid 7 (AVX2, BMI) reads zero");
    cpuid(0x80000001U);
    expect((q.gpr[2] >> 29 & 1U) == 1, "runtime: cpuid 0x80000001 advertises long mode");
}

// --- R2 rig (`r2_rig.h`): the same inputs and hashes on every side ----------------------------
void test_r2_rig() {
    namespace r2 = wse::recomp::r2;
    const r2::CaseInputs a = r2::make_case(0x140001000ULL, 3);
    const r2::CaseInputs b = r2::make_case(0x140001000ULL, 3);
    const r2::CaseInputs c = r2::make_case(0x140001000ULL, 4);
    bool same = std::memcmp(a.xmm, b.xmm, sizeof(a.xmm)) == 0;
    bool differs = std::memcmp(a.xmm, c.xmm, sizeof(a.xmm)) != 0;
    for (int i = 0; i < 4; ++i) {
        same = same && a.gpr[i].buffer == b.gpr[i].buffer && a.gpr[i].offset == b.gpr[i].offset &&
               a.gpr[i].literal == b.gpr[i].literal;
    }
    expect(same && differs, "r2: make_case is deterministic per (va, case) and differs between cases");
    const r2::Regions rg = r2::fixed_regions();
    expect(rg.buffer[0] == r2::kRegionBase + r2::kBufferBytes && rg.buffer[3] == r2::kRegionBase + 7 * r2::kBufferBytes &&
               r2::kRegionBase + r2::kRegionBytes <= r2::kStackBase && r2::kStackBase + r2::kStackBytes <= r2::kReturnVa &&
               (r2::kGuestRsp + 8) % 16 == 0 && r2::kStackClearLo >= r2::kStackBase,
           "r2: fixed layout: guarded buffers, then the stack, then the return VA; rsp + 8 is 16-aligned");
    std::vector<std::uint8_t> store(4 * r2::kBufferBytes);
    std::vector<std::uint8_t> keep(4 * r2::kBufferBytes);
    std::uint8_t* bufs[r2::kBuffers];
    const std::uint8_t* pristine[r2::kBuffers];
    for (int i = 0; i < r2::kBuffers; ++i) {
        bufs[i] = store.data() + i * r2::kBufferBytes;
        pristine[i] = keep.data() + i * r2::kBufferBytes;
    }
    r2::fill_buffers(0x140002000ULL, bufs, rg);
    keep = store;
    std::uint64_t w0 = 0;
    std::memcpy(&w0, bufs[1], 8);
    expect(w0 >= rg.buffer[0] && w0 < rg.buffer[3] + r2::kBufferBytes, "r2: even buffer words hold buffer pointers");
    const std::uint8_t* const cb[r2::kBuffers] = {bufs[0], bufs[1], bufs[2], bufs[3]};
    r2::Hasher h1;
    r2::Hasher h2;
    r2::Hasher h3;
    r2::Hasher hf;
    r2::hash_case(h1, false, 7, 9, cb, pristine, rg);
    r2::hash_case(h2, false, 7, 9, cb, pristine, rg);
    bufs[2][100] ^= 1;
    r2::hash_case(h3, false, 7, 9, cb, pristine, rg);
    r2::restore_buffers(bufs, pristine);
    r2::hash_case(hf, true, 7, 9, cb, pristine, rg);
    expect(h1.h == h2.h && h1.h != h3.h && hf.h != h1.h && store == keep,
           "r2: hash_case sees a changed byte and faults; restore_buffers undoes the change");
    expect(r2::normalize(rg.buffer[2] + 24, rg) == (0xB000000000000000ULL | (2ULL << 32) | 24) &&
               r2::normalize(0x140001000ULL, rg) == 0x140001000ULL,
           "r2: buffer pointers normalize to (region, offset); image addresses stay");
}

// --- R3 import bridge (`read_pe_imports`, `Runtime::set_import` by name, `call_guest`) -------
namespace bridge_test {

constexpr std::uint64_t kSum6Va = 0x140020000ULL;

// A stand-in guest function: rax = a1 + 2*a2 + ... + 6*a6 (Win64: four registers, two on the stack).
void sum6(CpuState* s) {
    std::uint64_t r = 0;
    for (int i = 0; i < 6; ++i) {
        r += static_cast<std::uint64_t>(i + 1) * win64_arg(*s, i);
    }
    s->gpr[1] = 0xDEAD;  // clobbers a volatile register; call_guest must restore it
    s->gpr[0] = r;
    rt_test::guest_ret(s);
}

}  // namespace bridge_test

void test_import_bridge() {
    using namespace bridge_test;
    // A synthetic mapped image: one descriptor, one import by name and one by ordinal.
    std::vector<std::uint8_t> img(0x2000, 0);
    auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(img.data() + off, &v, 4); };
    auto put64 = [&](std::size_t off, std::uint64_t v) { std::memcpy(img.data() + off, &v, 8); };
    put32(0x3C, 0x80);
    put32(0x80 + 24 + 112 + 8, 0x1000);   // import directory RVA
    put32(0x80 + 24 + 112 + 12, 40);
    put32(0x1000 + 0, 0x1100);            // OriginalFirstThunk
    put32(0x1000 + 12, 0x1200);           // Name
    put32(0x1000 + 16, 0x1300);           // FirstThunk
    put64(0x1100, 0x1400);                // -> hint/name
    put64(0x1108, 0x8000000000000005ULL); // ordinal 5
    std::memcpy(img.data() + 0x1200, "KERNEL32.dll", 13);
    std::memcpy(img.data() + 0x1402, "TlsAlloc", 9);
    std::vector<PeImport> imps;
    std::string err;
    expect(read_pe_imports(img.data(), img.size(), kBase, imps, &err) && imps.size() == 2 &&
               imps[0].slot_va == kBase + 0x1300 && imps[0].dll == "KERNEL32.dll" && imps[0].name == "TlsAlloc" &&
               imps[1].slot_va == kBase + 0x1308 && imps[1].name.empty() && imps[1].ordinal == 5,
           "imports: names, ordinals and IAT slots are read");
    put32(0x1000 + 12, 0x1FFC);  // a DLL name running off the end of the image
    std::memcpy(img.data() + 0x1FFC, "ABCD", 4);
    expect(!read_pe_imports(img.data(), img.size(), kBase, imps, &err), "imports: a name past the image fails");

    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    std::vector<PeImport> named(2);
    named[0] = {kBase + 0x1300, "KERNEL32.dll", "TlsAlloc", 0};
    named[1] = {kBase + 0x1308, "KERNEL32.dll", "", 5};
    rt.name_imports(named);
    expect(rt.set_import("kernel32.DLL", "TlsAlloc", [](CpuState& st) { win64_return(st, 7); }) == 1 &&
               rt.set_import("KERNEL32.dll", "TlsFree", [](CpuState& st) { win64_return(st, 0); }) == 0,
           "imports: a shim binds by name, case-insensitive DLL; an import the exe lacks binds nothing");
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    rt_test::push_and_hook(&s, 0x140030000ULL, kBase + 0x1300, true);
    expect(faults.empty() && s.gpr[0] == 7 && s.rip == 0x140030000ULL, "imports: the bound shim runs and returns");
    stack.prepare(s);
    rt.attach(s);
    rt_test::push_and_hook(&s, 0x140030010ULL, kBase + 0x1308, true);
    expect(faults.size() == 1 && faults[0].kind == FaultKind::missing_import && faults[0].what == "KERNEL32.dll!#5",
           "imports: an unbound import faults with its DLL and name (or ordinal)");

    // call_guest: Win64 arguments in registers and on the stack; registers restored after.
    const FnRow rows[] = {{kSum6Va, &sum6}};
    rt.add_table(rows, 1);
    faults.clear();
    stack.prepare(s);
    rt.attach(s);
    s.gpr[1] = 0x1111;
    s.gpr[3] = 0x3333;
    const std::uint64_t rsp0 = s.gpr[4];
    bool ok = false;
    const std::uint64_t r = rt.call_guest(s, kSum6Va, {1, 10, 100, 1000, 10000, 100000}, &ok);
    expect(ok && faults.empty() && r == 1 + 20 + 300 + 4000 + 50000 + 600000 && s.gpr[0] == r && s.gpr[1] == 0x1111 &&
               s.gpr[3] == 0x3333 && s.gpr[4] == rsp0,
           "call_guest: six Win64 arguments, rax returned, other registers and rsp restored");
}

// --- R3 thread environment (`teb.h`): TEB, static TLS, TLS/FLS slots, callbacks -------------------
namespace teb_test {

constexpr std::uint64_t kImgBase = 0x150000000ULL;
constexpr std::uint64_t kTlsCbVa = kImgBase + 0x1000;  // inside the image, as TLS callbacks are
constexpr std::uint64_t kFlsCbVa = 0x140022000ULL;
std::vector<std::uint64_t> g_tls_cb;  // base, reason per call
std::vector<std::uint64_t> g_fls_cb;  // values

void tls_callback(CpuState* s) {
    g_tls_cb.push_back(win64_arg(*s, 0));
    g_tls_cb.push_back(win64_arg(*s, 1));
    rt_test::guest_ret(s);
}
void fls_callback(CpuState* s) {
    g_fls_cb.push_back(win64_arg(*s, 0));
    rt_test::guest_ret(s);
}

}  // namespace teb_test

void test_thread_environment() {
    using namespace teb_test;
    namespace tb = wse::recomp::teb;
    std::vector<std::uint8_t> img(0x3000, 0);
    auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(img.data() + off, &v, 4); };
    auto put64 = [&](std::size_t off, std::uint64_t v) { std::memcpy(img.data() + off, &v, 8); };
    put32(0x3C, 0x80);
    put32(0x80 + 24 + 112 + 9 * 8, 0x2000);  // TLS directory RVA
    put32(0x80 + 24 + 112 + 9 * 8 + 4, 40);
    put64(0x2000, kImgBase + 0x2100);         // StartAddressOfRawData
    put64(0x2008, kImgBase + 0x2110);         // EndAddressOfRawData
    put64(0x2010, kImgBase + 0x2200);         // AddressOfIndex
    put64(0x2018, kImgBase + 0x2300);         // AddressOfCallBacks
    put32(0x2020, 16);                        // SizeOfZeroFill
    put32(0x2024, 0x00600000);                // IMAGE_SCN_ALIGN_32BYTES
    for (int i = 0; i < 16; ++i) {
        img[0x2100 + i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    put32(0x2200, 0xFFFFFFFFU);
    put64(0x2300, kTlsCbVa);  // one callback, then null
    PeTls tls;
    std::string err;
    expect(read_pe_tls(img.data(), img.size(), kImgBase, tls, &err) && tls.present &&
               tls.raw_end - tls.raw_begin == 16 && tls.index_va == kImgBase + 0x2200 && tls.zero_fill == 16 &&
               tls.alignment == 32 && tls.callbacks.size() == 1 && tls.callbacks[0] == kTlsCbVa,
           "teb: the TLS directory is read (template, index, zero fill, alignment, callbacks)");
    put64(0x2300, 0x1000);  // a callback outside the image
    expect(!read_pe_tls(img.data(), img.size(), kImgBase, tls, &err), "teb: a callback outside the image fails");
    put64(0x2300, kTlsCbVa);

    PeImage pe;
    pe.image_base = kImgBase;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    const char* names[] = {"TlsAlloc", "TlsFree", "TlsGetValue", "TlsSetValue", "FlsAlloc", "FlsFree",
                           "FlsGetValue", "FlsSetValue", "GetLastError", "SetLastError", "GetCurrentThreadId",
                           "GetCurrentThread"};
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(names); ++i) {
        named.push_back({0x140040000ULL + 8 * i, "KERNEL32.dll", names[i], 0});
    }
    rt.name_imports(named);
    const FnRow rows[] = {{kTlsCbVa, &tls_callback}, {kFlsCbVa, &fls_callback}};
    rt.add_table(rows, 2);
    tb::Process proc(rt, pe, img.data());
    expect(proc.init(&err) && proc.install() == std::size(names), "teb: the process initializes and binds its shims");
    std::uint32_t index = 1;
    std::memcpy(&index, img.data() + 0x2200, 4);
    expect(index == 0 && *reinterpret_cast<const std::uint64_t*>(proc.peb() + tb::pebo::image_base) == kImgBase,
           "teb: _tls_index is written 0; the PEB holds the image base");

    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& t1 = proc.create_thread(0x9000, 0x8000);
    tb::Process::attach(s, t1);
    auto rd = [](std::uint64_t va) { return *reinterpret_cast<const std::uint64_t*>(va); };
    const std::uint64_t block = rd(rd(t1.teb + tb::tebo::tls_pointer));
    bool tmpl = (block & 31) == 0;
    for (int i = 0; i < 32; ++i) {
        tmpl = tmpl && reinterpret_cast<const std::uint8_t*>(block)[i] == (i < 16 ? 0xA0 + i : 0);
    }
    expect(rd(t1.teb + tb::tebo::self) == t1.teb && rd(t1.teb + tb::tebo::peb) == proc.peb() &&
               rd(t1.teb + tb::tebo::stack_base) == 0x9000 && rd(t1.teb + tb::tebo::stack_limit) == 0x8000 &&
               rd(t1.teb + tb::tebo::client_tid) == t1.id && rd(t1.teb + tb::tebo::client_pid) == tb::kProcessId && tmpl,
           "teb: the TEB fields and the static TLS block (template, zero fill, 32-byte aligned)");
    expect(rt.hooks()->segment_base(&s, Runtime::kSegGs) == t1.teb && faults.empty(),
           "teb: gs: is the attached thread's TEB");

    auto call = [&](const char* name, std::uint64_t a0 = 0, std::uint64_t a1 = 0) {
        std::size_t i = 0;
        while (std::string(names[i]) != name) {
            ++i;
        }
        s.gpr[1] = a0;
        s.gpr[2] = a1;
        rt_test::push_and_hook(&s, 0x140030000ULL, 0x140040000ULL + 8 * i, true);
        return s.gpr[0];
    };
    const std::uint64_t slot0 = call("TlsAlloc");
    expect(slot0 == 0 && call("TlsSetValue", 0, 0x55) == 1 && call("TlsGetValue", 0) == 0x55 &&
               rd(t1.teb + tb::tebo::tls_slots) == 0x55 && call("GetLastError") == 0,
           "teb: TlsAlloc / TlsSetValue / TlsGetValue use the TEB slots");
    for (int i = 1; i <= 64; ++i) {
        (void)call("TlsAlloc");
    }
    expect(call("TlsSetValue", 64, 0x64) == 1 && call("TlsGetValue", 64) == 0x64 &&
               rd(rd(t1.teb + tb::tebo::tls_expansion)) == 0x64,
           "teb: slot 64 lives in the expansion array");
    tb::GuestThread& t2 = proc.create_thread(0, 0);
    tb::Process::attach(s, t2);
    expect(call("TlsGetValue", 0) == 0 && call("GetCurrentThreadId") == t2.id && t2.id != t1.id,
           "teb: a second thread has its own slots and id");
    tb::Process::attach(s, t1);
    expect(call("TlsFree", 0) == 1 && rd(t1.teb + tb::tebo::tls_slots) == 0 && call("TlsFree", 0) == 0 &&
               call("GetLastError") == tb::kErrorInvalidParameter && call("TlsGetValue", 5000) == 0 &&
               call("TlsAlloc") == 0,
           "teb: TlsFree clears the slot; a bad index fails with ERROR_INVALID_PARAMETER; the slot is reused");
    expect(call("SetLastError", 1234) == 0 && call("GetLastError") == 1234 &&
               call("GetCurrentThread") == tb::kCurrentThread,
           "teb: SetLastError / GetLastError and the current-thread pseudo-handle");

    g_fls_cb.clear();
    const std::uint64_t fls = call("FlsAlloc", kFlsCbVa);
    tb::Process::attach(s, t2);
    (void)call("FlsSetValue", fls, 0x88);
    tb::Process::attach(s, t1);
    expect(fls == 0 && call("FlsSetValue", fls, 0x77) == 1 && call("FlsGetValue", fls) == 0x77 &&
               call("FlsFree", fls) == 1 && g_fls_cb.size() == 2 && g_fls_cb[0] + g_fls_cb[1] == 0x77 + 0x88 &&
               call("FlsGetValue", fls) == 0 && call("GetLastError") == tb::kErrorInvalidParameter,
           "teb: FlsFree runs the callback for every thread's value, then the slot is invalid");

    // TLS callbacks: process attach, then thread exit.
    g_tls_cb.clear();
    g_fls_cb.clear();
    const std::uint64_t fls2 = call("FlsAlloc", kFlsCbVa);
    tb::Process::attach(s, t2);
    (void)call("FlsSetValue", fls2, 0x99);
    const bool attach_ok = proc.run_tls_callbacks(s, tb::kDllProcessAttach);
    proc.exit_thread(s, t2);
    expect(attach_ok && faults.empty() && g_tls_cb.size() == 4 && g_tls_cb[0] == kImgBase &&
               g_tls_cb[1] == tb::kDllProcessAttach && g_tls_cb[3] == tb::kDllThreadDetach && g_fls_cb.size() == 1 &&
               g_fls_cb[0] == 0x99,
           "teb: TLS callbacks get (base, reason); thread exit runs its FLS callbacks, then DLL_THREAD_DETACH");

    s.teb = 0;
    (void)call("TlsGetValue", 0);
    expect(!faults.empty() && faults[0].kind == FaultKind::unsupported, "teb: a shim on a thread with no TEB faults");
}

// --- R3 unwinder (`unwind.h`): hand-built functions, unwind codes and stacks ---------------------
void test_unwinder() {
    namespace uw = wse::recomp::unwind;
    std::vector<std::uint8_t> img(0x1000, 0x90);
    auto put = [&](std::size_t off, std::initializer_list<std::uint8_t> bytes) {
        std::size_t i = off;
        for (const std::uint8_t b : bytes) {
            img[i++] = b;
        }
    };
    // F: push rbx; push rbp; sub rsp, 0x28; ...; add rsp, 0x28; pop rbp; pop rbx; ret
    put(0x100, {0x53, 0x55, 0x48, 0x83, 0xEC, 0x28});
    put(0x130, {0x48, 0x83, 0xC4, 0x28, 0x5D, 0x5B, 0xC3});
    // UNWIND_INFO v1, prolog 6, 3 codes: ALLOC_SMALL 0x28 @6, PUSH rbp @2, PUSH rbx @1
    put(0x200, {0x01, 6, 3, 0, 0x06, 0x42, 0x02, 0x50, 0x01, 0x30, 0, 0});
    // G: push rbp; sub rsp, 0x40; mov [rsp+0x30], rsi; lea rbp, [rsp+0x20]; exception handler
    put(0x300, {0x55, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x89, 0x74, 0x24, 0x30, 0x48, 0x8D, 0x6C, 0x24, 0x20});
    // UNWIND_INFO v1, EHANDLER, prolog 15, 5 slots, frame rbp offset 2 (0x20):
    // SET_FPREG @15, SAVE_NONVOL rsi @10 (+6*8), ALLOC_SMALL 0x40 @5, PUSH rbp @1; handler 0x999
    put(0x280, {0x09, 15, 5, 0x25, 0x0F, 0x03, 0x0A, 0x64, 6, 0, 0x05, 0x72, 0x01, 0x50, 0, 0,
                0x99, 0x09, 0, 0, 0xAB, 0xCD, 0, 0});
    // H: a fragment of F (chained UNWIND_INFO pointing at F's RUNTIME_FUNCTION)
    put(0x500, {0x21, 0, 0, 0, 0x00, 0x01, 0, 0, 0x40, 0x01, 0, 0, 0x00, 0x02, 0, 0});
    const std::vector<PeFunctionRange> fns = {{0x100, 0x140, 0x200, 0}, {0x300, 0x380, 0x280, 0}, {0x400, 0x420, 0x500, 0}};
    const uw::Image image{img.data(), img.size(), kBase, &fns};

    expect(uw::lookup_function(image, kBase + 0x13F) == &fns[0] && uw::lookup_function(image, kBase + 0x140) == nullptr &&
               uw::lookup_function(image, kBase + 0x410) == &fns[2] && uw::lookup_function(image, kBase + 0x50) == nullptr,
           "unwind: lookup_function finds the containing .pdata entry only");

    std::uint64_t s[16] = {};
    const auto at = [&](int i) { return reinterpret_cast<std::uint64_t>(&s[i]); };
    s[5] = 0xB0B0;           // saved rbp
    s[6] = 0xB1B1;           // saved rbx
    s[7] = 0x140009999ULL;   // return address
    auto body = [&] {
        CpuState c;
        c.gpr[4] = at(0);
        c.gpr[3] = 1;
        c.gpr[5] = 2;
        return c;
    };
    CpuState c = body();
    c.rip = kBase + 0x110;
    uw::Result r = uw::virtual_unwind(image, c, uw::kFlagExceptionHandler);
    expect(r.ok && c.rip == 0x140009999ULL && c.gpr[4] == at(8) && c.gpr[3] == 0xB1B1 && c.gpr[5] == 0xB0B0 &&
               r.establisher_frame == at(0) && r.handler == 0,
           "unwind: body of F restores rbx, rbp, rsp and rip");

    c = body();
    c.gpr[4] = at(6);
    c.rip = kBase + 0x101;  // only `push rbx` has run
    r = uw::virtual_unwind(image, c, 0);
    expect(r.ok && c.rip == 0x140009999ULL && c.gpr[4] == at(8) && c.gpr[3] == 0xB1B1 && c.gpr[5] == 2,
           "unwind: a partly run prolog undoes only what ran");

    for (const std::uint64_t pc : {0x130ULL, 0x134ULL, 0x135ULL}) {
        c = body();
        c.gpr[4] = pc == 0x130 ? at(0) : (pc == 0x134 ? at(5) : at(6));
        if (pc == 0x135) {
            c.gpr[5] = 0xB0B0;  // rbp already popped
        }
        c.rip = kBase + pc;
        const std::uint64_t frame_rsp = c.gpr[4];
        r = uw::virtual_unwind(image, c, uw::kFlagExceptionHandler);
        expect(r.ok && c.rip == 0x140009999ULL && c.gpr[4] == at(8) && c.gpr[3] == 0xB1B1 && c.gpr[5] == 0xB0B0,
               "unwind: inside the epilog it is emulated from the instructions");
        expect(r.establisher_frame == frame_rsp && r.handler == 0,
               "unwind: in an epilog the establisher is the frame's rsp before it runs, and no handler");
    }

    c = body();
    c.gpr[4] = at(7);
    c.rip = kBase + 0x50;  // no .pdata: a leaf
    r = uw::virtual_unwind(image, c, 0);
    expect(r.ok && c.rip == 0x140009999ULL && c.gpr[4] == at(8), "unwind: a leaf returns through [rsp]");

    std::uint64_t t[16] = {};
    const auto tat = [&](int i) { return reinterpret_cast<std::uint64_t>(&t[i]); };
    t[6] = 0x5151;            // saved rsi at frame + 0x30
    t[8] = 0xBEEF;            // saved rbp at frame + 0x40
    t[9] = 0x14000AAAAULL;    // return address
    c = CpuState{};
    c.gpr[5] = tat(0) + 0x20;  // rbp = frame + 0x20
    c.gpr[4] = 0x1234;         // rsp is not trusted past SET_FPREG
    c.gpr[6] = 7;
    c.rip = kBase + 0x320;
    r = uw::virtual_unwind(image, c, uw::kFlagExceptionHandler);
    expect(r.ok && c.rip == 0x14000AAAAULL && c.gpr[4] == tat(10) && c.gpr[5] == 0xBEEF && c.gpr[6] == 0x5151 &&
               r.establisher_frame == tat(0) && r.handler == kBase + 0x999 && r.handler_data == kBase + 0x280 + 20,
           "unwind: frame register, SAVE_NONVOL and the exception handler with its data");
    c = CpuState{};
    c.gpr[5] = tat(0) + 0x20;
    c.rip = kBase + 0x320;
    r = uw::virtual_unwind(image, c, uw::kFlagTerminationHandler);
    expect(r.ok && r.handler == 0, "unwind: an exception-only handler is not returned while unwinding");

    c = body();
    c.rip = kBase + 0x410;
    r = uw::virtual_unwind(image, c, 0);
    expect(r.ok && c.rip == 0x140009999ULL && c.gpr[4] == at(8) && c.gpr[3] == 0xB1B1 && c.gpr[5] == 0xB0B0,
           "unwind: a chained fragment unwinds through its parent's codes");
}

}  // namespace

namespace wse::selftest {

int run_recomp_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_decoder_operands();
    test_interpreter_flags();
    test_function_decode_and_run();
    test_pe_reader();
    test_pdb_reader();
    test_r0_vectors();
    test_decode_alu_forms();
    test_decode_group1();
    test_decode_modrm_sib();
    test_decode_moves();
    test_decode_extensions();
    test_decode_lea();
    test_decode_stack_ret_nop();
    test_decode_branches();
    test_decode_test_insns();
    test_decode_inc_dec();
    test_decode_rejections();
    test_interp_flags_exhaustive();
    test_eval_cond_exhaustive();
    test_register_and_memory_writes();
    test_runtime_dispatch();
    test_r2_rig();
    test_import_bridge();
    test_thread_environment();
    test_unwinder();
    if (g_failures == 0) {
        std::printf("recomp tests: PASS\n");
        return 0;
    }
    std::printf("recomp tests: FAIL (%d)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_recomp_suite(nullptr);
}
#endif
