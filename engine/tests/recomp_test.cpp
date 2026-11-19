// Host self-tests for the Phase 5 recompiler core (R0, `docs/RECOMP_PLAN.md`): the x86-64
// decoder subset, the reference interpreter's flag rules, function decoding, the PE reader
// and the R0 (d) test vectors. Synthetic code only: every byte sequence below is written for
// this test (no retail code; that runs only in the local `tools/r0_pc_reference.cpp`).
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
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
#include "wse/recomp/shim_advapi.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/shim_d3d9.h"
#include "wse/recomp/shim_d3dx.h"
#include "wse/recomp/shim_files.h"
#include "wse/recomp/shim_memory.h"
#include "wse/recomp/shim_modules.h"
#include "wse/recomp/shim_net.h"
#include "wse/recomp/shim_strings.h"
#include "wse/recomp/shim_system.h"
#include "wse/recomp/shim_time.h"
#include "wse/recomp/shim_user32.h"
#include "wse/recomp/teb.h"
#include "wse/recomp/threadpool.h"
#include "wse/recomp/threads.h"
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

    // fill_iat: a slot in real memory holds its own VA; calling that value through `dispatch`
    // (an import loaded into a register) runs the import.
    alignas(8) static std::uint64_t iat_slot = 0x1234;
    const auto iat_va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&iat_slot));
    Runtime rt2;
    std::vector<Fault> faults2;
    rt2.on_fault = [&](const Fault& f) { faults2.push_back(f); };
    rt2.name_imports({PeImport{iat_va, "KERNEL32.dll", "GetTickCount", 0}});
    (void)rt2.set_import("KERNEL32.dll", "GetTickCount", [](CpuState& st) { win64_return(st, 0x7777); });
    rt2.fill_iat();
    stack.prepare(s);
    rt2.attach(s);
    rt_test::push_and_hook(&s, 0x140030020ULL, iat_slot, false);
    expect(iat_slot == iat_va && faults2.empty() && s.gpr[0] == 0x7777 && s.rip == 0x140030020ULL,
           "imports: fill_iat writes each slot's VA, and an indirect call to it runs the import");
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

// --- R3 threads and sync (`threads.h`, `kernel.h`): stand-in guest functions on host threads ----
namespace thr_test {

constexpr std::uint64_t kSlotBase = 0x140050000ULL;
const char* const kNames[] = {
    "CreateThread", "ExitThread", "ResumeThread", "GetExitCodeThread", "OpenThread", "TerminateThread",
    "SetThreadPriority", "GetThreadPriority", "WaitForSingleObject", "CloseHandle", "DuplicateHandle", "Sleep",
    "CreateEventA", "CreateEventW", "SetEvent", "ResetEvent", "CreateMutexW", "ReleaseMutex", "CreateSemaphoreA",
    "ReleaseSemaphore", "InitializeCriticalSection", "EnterCriticalSection", "TryEnterCriticalSection",
    "LeaveCriticalSection", "DeleteCriticalSection", "SleepConditionVariableCS", "WakeAllConditionVariable",
    "InitializeSListHead", "InterlockedPushEntrySList", "InterlockedPopEntrySList", "InterlockedFlushSList",
    "QueryDepthSList", "GetCurrentThreadId", "GetLastError", "CreateThreadpoolTimer", "SetThreadpoolTimer",
    "WaitForThreadpoolTimerCallbacks", "CloseThreadpoolTimer", "CreateThreadpoolWait", "SetThreadpoolWait",
    "CloseThreadpoolWait", "RegisterWaitForSingleObject", "UnregisterWait", "UnregisterWaitEx",
    "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount", "GetSystemTimeAsFileTime",
    "SystemTimeToFileTime", "FileTimeToSystemTime", "GetTimeZoneInformation", "SystemTimeToTzSpecificLocalTime",
    "EncodePointer", "DecodePointer", "IsProcessorFeaturePresent", "GetCommandLineA", "GetCommandLineW",
    "GetEnvironmentStringsW", "FreeEnvironmentStringsW", "SetEnvironmentVariableA", "InitOnceExecuteOnce",
    "ExitProcess", "GetSystemInfo", "GetLogicalProcessorInformation", "GetVersionExW", "GlobalMemoryStatusEx",
    "SetUnhandledExceptionFilter", "UnhandledExceptionFilter", "CreateTimerQueue", "CreateTimerQueueTimer",
    "ChangeTimerQueueTimer", "DeleteTimerQueueTimer", "GetACP", "GetOEMCP", "MultiByteToWideChar",
    "WideCharToMultiByte", "LCMapStringEx", "CompareStringEx", "GetLocaleInfoEx", "FormatMessageA", "LocalFree",
    "GetProcessHeap", "HeapAlloc", "HeapFree", "HeapSize", "HeapReAlloc", "VirtualAlloc", "VirtualQuery", "VirtualFree",
    "CreateFileA", "ReadFile", "WriteFile", "DeleteFileA", "GetFileAttributesA", "FindFirstFileA", "FindNextFileA",
    "FindClose", "GetCurrentDirectoryA", "GetTempPathA", "CreateDirectoryA"};

std::uint64_t slot(const char* name) {
    for (std::size_t i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(kNames[i], name) == 0) {
            return kSlotBase + 8 * i;
        }
    }
    return 0;
}

// Calls an import as translated code would: Win64 arguments (the 5th and up above the shadow
// area), the return address pushed, then the import hook. Returns rax; `s.rip` tells whether the
// shim returned (a thread exit leaves `unwind_rsp` set instead).
std::uint64_t call(CpuState& s, const char* name, std::initializer_list<std::uint64_t> args) {
    constexpr std::uint64_t kRet = 0x140031000ULL;
    const std::uint64_t rsp0 = s.gpr[4];
    s.gpr[4] = (rsp0 - 0x80) & ~std::uint64_t{15};
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    std::size_t i = 0;
    for (const std::uint64_t a : args) {
        if (i < 4) {
            s.gpr[kArgReg[i]] = a;
        } else {
            std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20 + 8 * (i - 4)), &a, 8);
        }
        ++i;
    }
    s.gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s.gpr[4]), &kRet, 8);
    s.hooks->import_call(&s, slot(name));
    const std::uint64_t rax = s.gpr[0];
    if (s.unwind_rsp == 0) {
        s.gpr[4] = rsp0;
    }
    return rax;
}

constexpr std::uint64_t kDouble = 0x140060000ULL, kExit = 0x140060010ULL, kSleeper = 0x140060020ULL,
                        kMutexTaker = 0x140060030ULL, kCounter = 0x140060040ULL, kWaiter = 0x140060050ULL;
std::atomic<int> g_ran{0};
std::uint64_t g_cs = 0;          // guest address of a CRITICAL_SECTION
std::uint64_t g_cv = 0;          // guest address of a CONDITION_VARIABLE
std::uint64_t g_shared = 0;      // incremented under g_cs
volatile int g_flag = 0;
std::uint64_t g_mutex = 0;

// rax = 2 * parameter; checks the TEB is attached and names this thread.
void entry_double(CpuState* s) {
    const std::uint64_t p = win64_arg(*s, 0);
    ++g_ran;
    const std::uint64_t tid = call(*s, "GetCurrentThreadId", {});
    s->gpr[0] = (s->teb != 0 && tid == wse::recomp::teb::current(*s)->id) ? 2 * p : 0xBAD;
    rt_test::guest_ret(s);
}
void entry_exit(CpuState* s) {
    (void)call(*s, "ExitThread", {77});
    if (s->unwind_rsp != 0) {
        return;  // as translated code: every frame returns at once
    }
    s->gpr[0] = 0xBAD;
    rt_test::guest_ret(s);
}
void entry_sleeper(CpuState* s) {
    ++g_ran;
    (void)call(*s, "Sleep", {0xFFFFFFFFULL});
    if (s->unwind_rsp != 0) {
        return;
    }
    s->gpr[0] = 0xBAD;
    rt_test::guest_ret(s);
}
void entry_mutex_taker(CpuState* s) {
    s->gpr[0] = call(*s, "WaitForSingleObject", {g_mutex, 0xFFFFFFFFULL});  // never released
    rt_test::guest_ret(s);
}
void entry_counter(CpuState* s) {
    for (int i = 0; i < 2000; ++i) {
        (void)call(*s, "EnterCriticalSection", {g_cs});
        g_shared = g_shared + 1;
        (void)call(*s, "LeaveCriticalSection", {g_cs});
    }
    s->gpr[0] = 1;
    rt_test::guest_ret(s);
}
void entry_waiter(CpuState* s) {
    (void)call(*s, "EnterCriticalSection", {g_cs});
    while (g_flag == 0) {
        (void)call(*s, "SleepConditionVariableCS", {g_cv, g_cs, 0xFFFFFFFFULL});
    }
    (void)call(*s, "LeaveCriticalSection", {g_cs});
    s->gpr[0] = 42;
    rt_test::guest_ret(s);
}

}  // namespace thr_test

void test_threads_and_sync() {
    using namespace thr_test;
    namespace tb = wse::recomp::teb;
    namespace kn = wse::recomp::kernel;
    std::vector<std::uint8_t> img(0x1000, 0);
    img[0x3C] = 0x80;
    PeImage pe;
    pe.image_base = 0x150000000ULL;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::mutex fault_m;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) {
        std::lock_guard<std::mutex> lock(fault_m);
        faults.push_back(f);
    };
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(kNames); ++i) {
        named.push_back({kSlotBase + 8 * i, "KERNEL32.dll", kNames[i], 0});
    }
    rt.name_imports(named);
    const FnRow rows[] = {{kDouble, &entry_double},        {kExit, &entry_exit},       {kSleeper, &entry_sleeper},
                          {kMutexTaker, &entry_mutex_taker}, {kCounter, &entry_counter}, {kWaiter, &entry_waiter}};
    rt.add_table(rows, std::size(rows));
    std::string err;
    tb::Process proc(rt, pe, img.data());
    kn::Kernel kern(rt);
    wse::recomp::threads::Threads thr(rt, proc, kern);
    expect(proc.init(&err) && proc.install() >= 2 && kern.install() >= 20 && thr.install() >= 6,
           "threads: the process, kernel and thread shims bind");

    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& main_thread = proc.create_thread(0, 0);
    tb::Process::attach(s, main_thread);
    const std::uint64_t inf = 0xFFFFFFFFULL;

    alignas(8) std::uint32_t tid = 0;
    const std::uint64_t h1 = call(s, "CreateThread", {0, 0, kDouble, 21, 0, reinterpret_cast<std::uintptr_t>(&tid)});
    alignas(8) std::uint32_t code = 0;
    const auto pcode = reinterpret_cast<std::uintptr_t>(&code);
    expect(h1 != 0 && tid != 0 && tid != main_thread.id && call(s, "WaitForSingleObject", {h1, inf}) == kn::kWaitObject0 &&
               call(s, "GetExitCodeThread", {h1, pcode}) == 1 && code == 42,
           "threads: CreateThread runs the entry on its own thread with its TEB; the handle signals with rax");
    expect(call(s, "CloseHandle", {h1}) == 1 && call(s, "CloseHandle", {h1}) == 0 &&
               call(s, "GetLastError", {}) == kn::kErrorInvalidHandle,
           "threads: CloseHandle once; a closed handle is invalid");

    g_ran = 0;
    const std::uint64_t h2 = call(s, "CreateThread", {0, 0, kDouble, 1, threads::kCreateSuspended, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool still = call(s, "GetExitCodeThread", {h2, pcode}) == 1 && code == kn::kStillActive && g_ran == 0;
    expect(still && call(s, "ResumeThread", {h2}) == 1 && call(s, "WaitForSingleObject", {h2, inf}) == 0 && g_ran == 1,
           "threads: CREATE_SUSPENDED waits for ResumeThread");

    const std::uint64_t h3 = call(s, "CreateThread", {0, 0, kExit, 0, 0, 0});
    expect(call(s, "WaitForSingleObject", {h3, inf}) == 0 && call(s, "GetExitCodeThread", {h3, pcode}) == 1 && code == 77,
           "threads: ExitThread unwinds out of the entry with its code");

    g_ran = 0;
    const std::uint64_t h4 = call(s, "CreateThread", {0, 0, kSleeper, 0, 0, 0});
    while (g_ran == 0) {
        std::this_thread::yield();
    }
    expect(call(s, "WaitForSingleObject", {h4, 20}) == kn::kWaitTimeout && call(s, "TerminateThread", {h4, 99}) == 1 &&
               call(s, "WaitForSingleObject", {h4, inf}) == 0 && call(s, "GetExitCodeThread", {h4, pcode}) == 1 &&
               code == 99,
           "threads: TerminateThread ends a sleeping thread at its wait (deferred), with the code");

    // Events, named objects, semaphores, mutexes.
    const std::uint64_t ev = call(s, "CreateEventW", {0, 0, 0, 0});
    expect(call(s, "WaitForSingleObject", {ev, 0}) == kn::kWaitTimeout && call(s, "SetEvent", {ev}) == 1 &&
               call(s, "WaitForSingleObject", {ev, 0}) == 0 && call(s, "WaitForSingleObject", {ev, 0}) == kn::kWaitTimeout,
           "kernel: an auto-reset event signals one wait");
    const char name[] = "wse_test_event";
    const auto pname = reinterpret_cast<std::uintptr_t>(name);
    const std::uint64_t n1 = call(s, "CreateEventA", {0, 1, 1, pname});
    const std::uint64_t e1 = call(s, "GetLastError", {});
    const std::uint64_t n2 = call(s, "CreateEventA", {0, 1, 0, pname});
    expect(n1 != 0 && n2 != 0 && n1 != n2 && e1 == 0 && call(s, "GetLastError", {}) == kn::kErrorAlreadyExists &&
               call(s, "ResetEvent", {n1}) == 1 && call(s, "WaitForSingleObject", {n2, 0}) == kn::kWaitTimeout,
           "kernel: a second CreateEvent with the same name opens the same event (ERROR_ALREADY_EXISTS)");
    alignas(8) std::int32_t prev = -1;
    const std::uint64_t sem = call(s, "CreateSemaphoreA", {0, 1, 2, 0});
    expect(call(s, "ReleaseSemaphore", {sem, 1, reinterpret_cast<std::uintptr_t>(&prev)}) == 1 && prev == 1 &&
               call(s, "ReleaseSemaphore", {sem, 1, 0}) == 0 && call(s, "GetLastError", {}) == kn::kErrorTooManyPosts &&
               call(s, "WaitForSingleObject", {sem, 0}) == 0 && call(s, "WaitForSingleObject", {sem, 0}) == 0 &&
               call(s, "WaitForSingleObject", {sem, 0}) == kn::kWaitTimeout,
           "kernel: a semaphore counts to its maximum");
    const std::uint64_t mx = call(s, "CreateMutexW", {0, 1, 0});
    expect(call(s, "ReleaseMutex", {mx}) == 1 && call(s, "ReleaseMutex", {mx}) == 0, "kernel: ReleaseMutex by the owner only");
    g_mutex = mx;
    const std::uint64_t h5 = call(s, "CreateThread", {0, 0, kMutexTaker, 0, 0, 0});
    expect(call(s, "WaitForSingleObject", {h5, inf}) == 0 && call(s, "WaitForSingleObject", {mx, inf}) == kn::kWaitAbandoned,
           "kernel: a mutex its owner never released is abandoned when the owner exits");
    alignas(8) std::uint64_t dup = 0;
    expect(call(s, "DuplicateHandle", {tb::kCurrentProcess, tb::kCurrentThread, tb::kCurrentProcess,
                                       reinterpret_cast<std::uintptr_t>(&dup), 0, 0, 0}) == 1 &&
               dup != 0 && call(s, "GetExitCodeThread", {dup, pcode}) == 1 && code == kn::kStillActive &&
               call(s, "OpenThread", {0, 0, main_thread.id}) != 0,
           "kernel: DuplicateHandle of the current-thread pseudo-handle; OpenThread by id");

    // Critical sections: recursion, ownership fields, TryEnter from another thread, mutual exclusion.
    alignas(16) std::uint8_t cs[40] = {};
    alignas(16) std::uint8_t cv[8] = {};
    g_cs = reinterpret_cast<std::uintptr_t>(cs);
    g_cv = reinterpret_cast<std::uintptr_t>(cv);
    (void)call(s, "InitializeCriticalSection", {g_cs});
    (void)call(s, "EnterCriticalSection", {g_cs});
    (void)call(s, "EnterCriticalSection", {g_cs});
    std::int32_t rec = 0;
    std::uint64_t owner = 0;
    std::memcpy(&rec, cs + 12, 4);
    std::memcpy(&owner, cs + 16, 8);
    std::uint64_t other_try = 9;
    std::thread other([&] {
        GuestStack st2;
        CpuState s2;
        st2.prepare(s2);
        rt.attach(s2);
        tb::GuestThread& t2 = proc.create_thread(0, 0);
        tb::Process::attach(s2, t2);
        other_try = call(s2, "TryEnterCriticalSection", {g_cs});
        proc.exit_thread(s2, t2);
    });
    other.join();
    (void)call(s, "LeaveCriticalSection", {g_cs});
    (void)call(s, "LeaveCriticalSection", {g_cs});
    expect(rec == 2 && owner == main_thread.id && other_try == 0, "kernel: a critical section is recursive and owned");
    g_shared = 0;
    std::uint64_t hs[4];
    for (std::uint64_t& h : hs) {
        h = call(s, "CreateThread", {0, 0, kCounter, 0, 0, 0});
    }
    for (int i = 0; i < 2000; ++i) {
        (void)call(s, "EnterCriticalSection", {g_cs});
        g_shared = g_shared + 1;
        (void)call(s, "LeaveCriticalSection", {g_cs});
    }
    for (const std::uint64_t h : hs) {
        (void)call(s, "WaitForSingleObject", {h, inf});
    }
    expect(g_shared == 5 * 2000, "kernel: EnterCriticalSection excludes (five threads, 10000 increments)");

    // A condition variable (zero-initialized word) with the critical section.
    g_flag = 0;
    const std::uint64_t hw = call(s, "CreateThread", {0, 0, kWaiter, 0, 0, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)call(s, "EnterCriticalSection", {g_cs});
    g_flag = 1;
    (void)call(s, "LeaveCriticalSection", {g_cs});
    (void)call(s, "WakeAllConditionVariable", {g_cv});
    expect(call(s, "WaitForSingleObject", {hw, inf}) == 0 && call(s, "GetExitCodeThread", {hw, pcode}) == 1 && code == 42,
           "kernel: SleepConditionVariableCS releases the section and wakes on WakeAll");
    (void)call(s, "DeleteCriticalSection", {g_cs});

    // SLists: LIFO push / pop / flush and the depth.
    alignas(16) std::uint8_t head[16] = {};
    alignas(16) std::uint8_t entries[3][16] = {};
    const auto ph = reinterpret_cast<std::uintptr_t>(head);
    (void)call(s, "InitializeSListHead", {ph});
    for (auto& e : entries) {
        (void)call(s, "InterlockedPushEntrySList", {ph, reinterpret_cast<std::uintptr_t>(e)});
    }
    const std::uint64_t depth3 = call(s, "QueryDepthSList", {ph});
    const std::uint64_t popped = call(s, "InterlockedPopEntrySList", {ph});
    const std::uint64_t rest = call(s, "InterlockedFlushSList", {ph});
    expect(depth3 == 3 && popped == reinterpret_cast<std::uintptr_t>(entries[2]) &&
               rest == reinterpret_cast<std::uintptr_t>(entries[1]) &&
               *reinterpret_cast<const std::uint64_t*>(entries[1]) == reinterpret_cast<std::uintptr_t>(entries[0]) &&
               call(s, "QueryDepthSList", {ph}) == 0 && call(s, "InterlockedPopEntrySList", {ph}) == 0,
           "kernel: SList push / pop / flush are LIFO with the depth");

    thr.join_all();
    expect(faults.empty(), "threads: no faults");
    if (!faults.empty()) {
        std::printf("  first fault: %s %s\n", fault_name(faults[0].kind), faults[0].what.c_str());
    }
}

// --- R3 thread-pool timers and waits (`threadpool.h`) --------------------------------------------
namespace pool_test {

constexpr std::uint64_t kTimerCb = 0x140070000ULL, kWaitCb = 0x140070010ULL, kRegCb = 0x140070020ULL;
std::mutex g_m;
std::vector<std::uint64_t> g_timer;  // context, timer, worker tid per call
std::vector<std::uint64_t> g_wait;   // context, wait result per call
std::vector<std::uint64_t> g_reg;    // context, TimerOrWaitFired per call

std::uint64_t tid_of(CpuState* s) {
    const wse::recomp::teb::GuestThread* t = wse::recomp::teb::current(*s);
    return t == nullptr ? 0 : t->id;
}
void timer_cb(CpuState* s) {  // (instance, context, timer)
    std::lock_guard<std::mutex> lock(g_m);
    g_timer.insert(g_timer.end(), {win64_arg(*s, 1), win64_arg(*s, 2), tid_of(s)});
    rt_test::guest_ret(s);
}
void wait_cb(CpuState* s) {  // (instance, context, wait, result)
    std::lock_guard<std::mutex> lock(g_m);
    g_wait.insert(g_wait.end(), {win64_arg(*s, 1), win64_arg(*s, 3) & 0xFFFFFFFF});
    rt_test::guest_ret(s);
}
void reg_cb(CpuState* s) {  // (context, timerOrWaitFired)
    std::lock_guard<std::mutex> lock(g_m);
    g_reg.insert(g_reg.end(), {win64_arg(*s, 0), win64_arg(*s, 1) & 0xFF});
    rt_test::guest_ret(s);
}
// Polls until `n` entries are in `v` or 2 s pass.
bool until(const std::vector<std::uint64_t>& v, std::size_t n) {
    for (int i = 0; i < 400; ++i) {
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (v.size() >= n) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
std::size_t size_of(const std::vector<std::uint64_t>& v) {
    std::lock_guard<std::mutex> lock(g_m);
    return v.size();
}

}  // namespace pool_test

void test_threadpool() {
    using namespace pool_test;
    using thr_test::call;
    namespace tb = wse::recomp::teb;
    namespace kn = wse::recomp::kernel;
    std::vector<std::uint8_t> img(0x1000, 0);
    img[0x3C] = 0x80;
    PeImage pe;
    pe.image_base = 0x150000000ULL;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::mutex fault_m;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) {
        std::lock_guard<std::mutex> lock(fault_m);
        faults.push_back(f);
    };
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(thr_test::kNames); ++i) {
        named.push_back({thr_test::kSlotBase + 8 * i, "KERNEL32.dll", thr_test::kNames[i], 0});
    }
    rt.name_imports(named);
    const FnRow rows[] = {{kTimerCb, &timer_cb}, {kWaitCb, &wait_cb}, {kRegCb, &reg_cb}};
    rt.add_table(rows, std::size(rows));
    std::string err;
    tb::Process proc(rt, pe, img.data());
    kn::Kernel kern(rt);
    wse::recomp::threadpool::Pool pool(rt, proc, kern);
    expect(proc.init(&err) && kern.install() >= 10 && pool.install() >= 10, "pool: the thread-pool shims bind");
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& main_thread = proc.create_thread(0, 0);
    tb::Process::attach(s, main_thread);

    // A one-shot timer 10 ms out, then a periodic one, then cancelled.
    alignas(8) std::int64_t due = -100000;  // 10 ms, relative
    const auto pdue = reinterpret_cast<std::uintptr_t>(&due);
    const std::uint64_t t = call(s, "CreateThreadpoolTimer", {kTimerCb, 0xC0, 0});
    (void)call(s, "SetThreadpoolTimer", {t, pdue, 0, 0});
    const bool once = until(g_timer, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(t != 0 && once && size_of(g_timer) == 3 && g_timer[0] == 0xC0 && g_timer[1] == t && g_timer[2] != 0 &&
               g_timer[2] != main_thread.id,
           "pool: a one-shot timer calls back once on its own guest thread (context, timer)");
    (void)call(s, "SetThreadpoolTimer", {t, pdue, 5, 0});
    const bool periodic = until(g_timer, 3 * 4);
    (void)call(s, "SetThreadpoolTimer", {t, 0, 0, 0});
    (void)call(s, "WaitForThreadpoolTimerCallbacks", {t, 1});
    const std::size_t after_cancel = size_of(g_timer);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    expect(periodic && size_of(g_timer) == after_cancel, "pool: a periodic timer repeats until a null due time cancels it");
    (void)call(s, "CloseThreadpoolTimer", {t});

    // A thread-pool wait on an auto-reset event, then one that times out.
    const std::uint64_t ev = call(s, "CreateEventW", {0, 0, 0, 0});
    const std::uint64_t w = call(s, "CreateThreadpoolWait", {kWaitCb, 0xD0, 0});
    (void)call(s, "SetThreadpoolWait", {w, ev, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool none_yet = size_of(g_wait) == 0;
    (void)call(s, "SetEvent", {ev});
    const bool fired = until(g_wait, 2);
    expect(none_yet && fired && g_wait[0] == 0xD0 && g_wait[1] == kn::kWaitObject0 &&
               call(s, "WaitForSingleObject", {ev, 0}) == kn::kWaitTimeout,
           "pool: a thread-pool wait fires when the event is set, and consumes the auto-reset event");
    alignas(8) std::int64_t timeout = -200000;  // 20 ms
    (void)call(s, "SetThreadpoolWait", {w, ev, reinterpret_cast<std::uintptr_t>(&timeout)});
    expect(until(g_wait, 4) && g_wait[3] == kn::kWaitTimeout, "pool: a thread-pool wait with a timeout reports WAIT_TIMEOUT");
    (void)call(s, "CloseThreadpoolWait", {w});

    // RegisterWaitForSingleObject: repeats per signal until unregistered; WT_EXECUTEONLYONCE.
    alignas(8) std::uint64_t reg = 0;
    const auto preg = reinterpret_cast<std::uintptr_t>(&reg);
    const bool registered = call(s, "RegisterWaitForSingleObject", {preg, ev, kRegCb, 0xE0, 0xFFFFFFFFULL, 0}) == 1;
    (void)call(s, "SetEvent", {ev});
    const bool r1 = until(g_reg, 2);
    (void)call(s, "SetEvent", {ev});
    const bool r2 = until(g_reg, 4);
    const bool unreg = call(s, "UnregisterWaitEx", {reg, threadpool::kInvalidHandle}) == 1;
    (void)call(s, "SetEvent", {ev});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(registered && reg != 0 && r1 && r2 && unreg && size_of(g_reg) == 4 && g_reg[0] == 0xE0 && g_reg[1] == 0 &&
               g_reg[3] == 0,
           "pool: a registered wait calls back on every signal until UnregisterWaitEx");
    (void)call(s, "ResetEvent", {ev});
    alignas(8) std::uint64_t reg2 = 0;
    (void)call(s, "RegisterWaitForSingleObject",
               {reinterpret_cast<std::uintptr_t>(&reg2), ev, kRegCb, 0xE1, 10, threadpool::kExecuteOnlyOnce});
    const bool timed = until(g_reg, 6);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    expect(timed && size_of(g_reg) == 6 && g_reg[4] == 0xE1 && g_reg[5] == 1 && call(s, "UnregisterWait", {reg2}) == 1,
           "pool: WT_EXECUTEONLYONCE with a timeout calls back once with TimerOrWaitFired = TRUE");
    expect(faults.empty(), "pool: no faults");
}

// --- R4 time, CRT support, system information, timer queues (`shim_time.h`, `shim_crt.h`) -------
namespace crt_test {

constexpr std::uint64_t kInitFn = 0x140080000ULL, kFilter = 0x140080010ULL, kQueueCb = 0x140080020ULL;
std::atomic<int> g_init_calls{0};
std::mutex g_m;
std::vector<std::uint64_t> g_queue;  // parameter, fired per call

void init_fn(CpuState* s) {  // BOOL CALLBACK (once, param, ctx)
    ++g_init_calls;
    s->gpr[0] = win64_arg(*s, 1) == 0x77 ? 1 : 0;
    rt_test::guest_ret(s);
}
void filter(CpuState* s) {  // LONG (EXCEPTION_POINTERS*)
    s->gpr[0] = win64_arg(*s, 0) == 0x1234 ? 1 : 0;  // EXCEPTION_EXECUTE_HANDLER
    rt_test::guest_ret(s);
}
void queue_cb(CpuState* s) {  // (parameter, TimerOrWaitFired)
    std::lock_guard<std::mutex> lock(g_m);
    g_queue.insert(g_queue.end(), {win64_arg(*s, 0), win64_arg(*s, 1) & 0xFF});
    rt_test::guest_ret(s);
}

}  // namespace crt_test

void test_time_crt_sysinfo() {
    using namespace crt_test;
    using thr_test::call;
    namespace tb = wse::recomp::teb;
    namespace sh = wse::recomp::shim;
    std::vector<std::uint8_t> img(0x1000, 0);
    img[0x3C] = 0x80;
    PeImage pe;
    pe.image_base = 0x150000000ULL;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::vector<Fault> faults;
    std::mutex fault_m;
    rt.on_fault = [&](const Fault& f) {
        std::lock_guard<std::mutex> lock(fault_m);
        faults.push_back(f);
    };
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(thr_test::kNames); ++i) {
        named.push_back({thr_test::kSlotBase + 8 * i, "KERNEL32.dll", thr_test::kNames[i], 0});
    }
    rt.name_imports(named);
    const FnRow rows[] = {{kInitFn, &init_fn}, {kFilter, &filter}, {kQueueCb, &queue_cb}};
    rt.add_table(rows, std::size(rows));
    std::string err;
    tb::Process proc(rt, pe, img.data());
    wse::recomp::kernel::Kernel kern(rt);
    wse::recomp::threadpool::Pool pool(rt, proc, kern);
    sh::Time tm(rt);
    sh::ProcessConfig cfg;
    cfg.command_line = "mb_warband_wse2_x64.exe --module Native";
    cfg.environment["WSE_A"] = "1";
    cfg.environment["Path"] = "C:\\Windows";
    cfg.processors = 4;
    sh::Crt crt(rt, cfg);
    expect(proc.init(&err) && proc.install() >= 2 && kern.install() >= 10 && pool.install() >= 13 && tm.install() >= 8 &&
               crt.install() >= 16,
           "crt: the time, CRT and system-information shims bind");
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& main_thread = proc.create_thread(0, 0);
    tb::Process::attach(s, main_thread);
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };

    // Counters: 10 MHz, monotonic, and consistent with GetTickCount.
    alignas(8) std::int64_t f = 0, c0 = 0, c1 = 0;
    (void)call(s, "QueryPerformanceFrequency", {va(&f)});
    (void)call(s, "QueryPerformanceCounter", {va(&c0)});
    const std::uint64_t t0 = call(s, "GetTickCount", {});
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    (void)call(s, "QueryPerformanceCounter", {va(&c1)});
    const std::uint64_t t1 = call(s, "GetTickCount", {});
    expect(f == 10000000 && c1 - c0 >= 200000 && c1 - c0 < 50000000 && t1 - t0 >= 20 && t1 - t0 < 5000,
           "time: QueryPerformanceFrequency is 10 MHz; the counter and GetTickCount advance with real time");

    // SYSTEMTIME <-> FILETIME: 2000-01-01 00:00:00 UTC is 125911584000000000 and a Saturday.
    alignas(8) std::uint16_t st[8] = {2000, 1, 0, 1, 0, 0, 0, 0};
    alignas(8) std::uint64_t ft = 0;
    const bool to_ft = call(s, "SystemTimeToFileTime", {va(st), va(&ft)}) == 1;
    alignas(8) std::uint16_t back[8] = {};
    ft += 10000ULL * (((13 * 60 + 45) * 60 + 30) * 1000ULL + 123) + 864000000000ULL * 59;  // 2000-02-29 13:45:30.123
    const bool to_st = call(s, "FileTimeToSystemTime", {va(&ft), va(back)}) == 1;
    alignas(8) std::uint16_t bad[8] = {2023, 2, 0, 29, 0, 0, 0, 0};
    alignas(8) std::uint64_t dummy = 0;
    expect(to_ft && to_st && back[0] == 2000 && back[1] == 2 && back[3] == 29 && back[2] == 2 && back[4] == 13 &&
               back[5] == 45 && back[6] == 30 && back[7] == 123 && call(s, "SystemTimeToFileTime", {va(bad), va(&dummy)}) == 0 &&
               call(s, "GetLastError", {}) == tb::kErrorInvalidParameter,
           "time: SystemTimeToFileTime / FileTimeToSystemTime (2000-01-01 is 125911584000000000; Feb 29 2023 fails)");
    {
        alignas(8) std::uint64_t ft2000 = 0;
        alignas(8) std::uint16_t y2k[8] = {2000, 1, 0, 1, 0, 0, 0, 0};
        (void)call(s, "SystemTimeToFileTime", {va(y2k), va(&ft2000)});
        alignas(8) std::uint64_t now = 0;
        (void)call(s, "GetSystemTimeAsFileTime", {va(&now)});
        expect(ft2000 == 125911584000000000ULL && now > 133000000000000000ULL, "time: the FILETIME epoch and the current time");
    }
    tm.utc_offset_minutes = 120;
    alignas(8) std::uint8_t tz[172] = {};
    alignas(8) std::uint16_t utc[8] = {2024, 12, 0, 31, 23, 30, 0, 0};
    alignas(8) std::uint16_t local[8] = {};
    std::int32_t bias = 0;
    const std::uint64_t tz_id = call(s, "GetTimeZoneInformation", {va(tz)});
    std::memcpy(&bias, tz, 4);
    expect(tz_id == sh::kTimeZoneIdUnknown && bias == -120 &&
               call(s, "SystemTimeToTzSpecificLocalTime", {0, va(utc), va(local)}) == 1 && local[0] == 2025 &&
               local[1] == 1 && local[3] == 1 && local[4] == 1 && local[5] == 30 && local[2] == 3,
           "time: a +2 h zone: Bias -120, 2024-12-31 23:30 UTC is 2025-01-01 01:30 (a Wednesday)");

    // Pointer encoding, processor features, command line, environment.
    bool encodes = true;
    for (const std::uint64_t p : {0ULL, 1ULL, 0x140001234ULL, ~0ULL}) {
        const std::uint64_t e = call(s, "EncodePointer", {p});
        encodes = encodes && e != p && call(s, "DecodePointer", {e}) == p;
    }
    expect(encodes && call(s, "IsProcessorFeaturePresent", {10}) == 1 && call(s, "IsProcessorFeaturePresent", {37}) == 1 &&
               call(s, "IsProcessorFeaturePresent", {38}) == 0 && call(s, "IsProcessorFeaturePresent", {39}) == 0,
           "crt: EncodePointer / DecodePointer round-trip; features match the CPU profile (SSE4.1 yes, SSE4.2 and AVX no)");
    const auto* cmd_a = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(call(s, "GetCommandLineA", {})));
    const auto* cmd_w = reinterpret_cast<const char16_t*>(static_cast<std::uintptr_t>(call(s, "GetCommandLineW", {})));
    expect(std::string(cmd_a) == cfg.command_line && std::u16string(cmd_w) == u"mb_warband_wse2_x64.exe --module Native",
           "crt: GetCommandLineA / W");
    const char unset[] = "wse_a";
    (void)call(s, "SetEnvironmentVariableA", {va(unset), 0});
    const char name[] = "NEW";
    const char value[] = "x";
    (void)call(s, "SetEnvironmentVariableA", {va(name), va(value)});
    const std::uint64_t block = call(s, "GetEnvironmentStringsW", {});
    std::u16string all;
    for (const auto* p = reinterpret_cast<const char16_t*>(static_cast<std::uintptr_t>(block)); *p != 0;) {
        const std::u16string entry(p);
        all += entry + u";";
        p += entry.size() + 1;
    }
    expect(all == u"NEW=x;Path=C:\\Windows;" && call(s, "FreeEnvironmentStringsW", {block}) == 1 &&
               call(s, "FreeEnvironmentStringsW", {block}) == 0,
           "crt: SetEnvironmentVariableA (case-insensitive delete) and the environment block");

    // InitOnceExecuteOnce runs its callback once; the top-level exception filter.
    alignas(8) std::uint64_t once = 0;
    g_init_calls = 0;
    const bool i1 = call(s, "InitOnceExecuteOnce", {va(&once), kInitFn, 0x77, 0}) == 1;
    const bool i2 = call(s, "InitOnceExecuteOnce", {va(&once), kInitFn, 0x77, 0}) == 1;
    expect(i1 && i2 && g_init_calls == 1 && once == 2, "crt: InitOnceExecuteOnce runs the callback once");
    expect(call(s, "UnhandledExceptionFilter", {0x1234}) == 0 && call(s, "SetUnhandledExceptionFilter", {kFilter}) == 0 &&
               call(s, "SetUnhandledExceptionFilter", {kFilter}) == kFilter && call(s, "UnhandledExceptionFilter", {0x1234}) == 1,
           "crt: UnhandledExceptionFilter calls the filter SetUnhandledExceptionFilter installed");

    // System information.
    alignas(8) std::uint8_t si[48] = {};
    (void)call(s, "GetSystemInfo", {va(si)});
    std::uint32_t page = 0, cpus = 0;
    std::memcpy(&page, si + 4, 4);
    std::memcpy(&cpus, si + 32, 4);
    alignas(8) std::uint32_t len = 8;
    alignas(8) std::uint8_t lpi[4 * 32] = {};
    const bool small = call(s, "GetLogicalProcessorInformation", {va(lpi), va(&len)}) == 0 &&
                       call(s, "GetLastError", {}) == sh::kErrorInsufficientBuffer && len == 128;
    const bool full = call(s, "GetLogicalProcessorInformation", {va(lpi), va(&len)}) == 1 && lpi[32] == 2;
    alignas(8) std::uint8_t ver[276] = {};
    ver[0] = 20;  // 276 = 0x114
    ver[1] = 1;
    alignas(8) std::uint8_t mem[64] = {64};
    const bool v = call(s, "GetVersionExW", {va(ver)}) == 1 && ver[4] == 6 && ver[8] == 2 && ver[12] == 0xF0 && ver[13] == 0x23;
    const bool m = call(s, "GlobalMemoryStatusEx", {va(mem)}) == 1 && mem[8 + 4] == 2;  // 8 GiB = 2 << 32
    expect(page == 4096 && cpus == 4 && small && full && v && m,
           "sysinfo: GetSystemInfo, GetLogicalProcessorInformation (sizes), GetVersionExW 6.2.9200, GlobalMemoryStatusEx");

    // Timer queues: a one-shot timer, then a periodic one deleted with INVALID_HANDLE_VALUE.
    const std::uint64_t q = call(s, "CreateTimerQueue", {});
    alignas(8) std::uint64_t t = 0;
    const bool made = call(s, "CreateTimerQueueTimer", {va(&t), q, kQueueCb, 0xF0, 5, 0, 0}) == 1 && t != 0;
    const bool fired = pool_test::until(g_queue, 2);
    alignas(8) std::uint64_t t2 = 0;
    (void)call(s, "CreateTimerQueueTimer", {va(&t2), q, kQueueCb, 0xF1, 1, 2, 0});
    const bool periodic = pool_test::until(g_queue, 8);
    const bool deleted = call(s, "DeleteTimerQueueTimer", {q, t2, threadpool::kInvalidHandle}) == 1;
    const std::size_t after = pool_test::size_of(g_queue);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(made && fired && g_queue[0] == 0xF0 && g_queue[1] == 1 && periodic && deleted &&
               pool_test::size_of(g_queue) == after && call(s, "DeleteTimerQueueTimer", {q, t, 0}) == 1,
           "timer queues: one-shot and periodic CreateTimerQueueTimer; DeleteTimerQueueTimer stops it");

    // ExitProcess: the exit callback gets the code and the thread unwinds out.
    std::uint32_t exit_seen = 0;
    crt.on_exit = [&](std::uint32_t code) { exit_seen = code; };
    (void)call(s, "ExitProcess", {3});
    expect(exit_seen == 3 && crt.exited() && crt.exit_code() == 3 && s.unwind_rsp == tb::kThreadExitUnwind,
           "crt: ExitProcess reports the code and unwinds the calling thread");
    expect(faults.empty(), "crt: no faults");
}

// --- R4 strings, locale and memory (`shim_strings.h`, `shim_memory.h`) ---------------------------
void test_strings_memory() {
    using thr_test::call;
    namespace tb = wse::recomp::teb;
    namespace sh = wse::recomp::shim;
    std::vector<std::uint8_t> img(0x1000, 0);
    img[0x3C] = 0x80;
    PeImage pe;
    pe.image_base = 0x150000000ULL;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(thr_test::kNames); ++i) {
        named.push_back({thr_test::kSlotBase + 8 * i, "KERNEL32.dll", thr_test::kNames[i], 0});
    }
    rt.name_imports(named);
    std::string err;
    tb::Process proc(rt, pe, img.data());
    sh::Memory mem(rt);
    sh::Strings str(rt, mem);
    expect(proc.init(&err) && proc.install() >= 2 && mem.install() >= 7 && str.install() >= 8,
           "strings: the string, locale and memory shims bind");
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& t = proc.create_thread(0, 0);
    tb::Process::attach(s, t);
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };

    expect(call(s, "GetACP", {}) == 1252 && call(s, "GetOEMCP", {}) == 437,
           "strings: the process runs with ANSI code page 1252 and OEM code page 437 (English defaults)");
    // CP_ACP / CP_OEMCP resolve to those: 0x80 is the euro sign in 1252 and C-cedilla in 437.
    const char hi[] = {static_cast<char>(0x80), 0};
    alignas(4) char16_t w[4] = {};
    const bool acp = call(s, "MultiByteToWideChar", {0, 0, va(hi), 1, va(w), 4}) == 1 && w[0] == 0x20AC;
    const bool oem = call(s, "MultiByteToWideChar", {1, 0, va(hi), 1, va(w), 4}) == 1 && w[0] == 0xC7;
    expect(acp && oem, "strings: CP_ACP is 1252 and CP_OEMCP is 437");
    // A string comparison the corpus also checks against Windows: "_a" < "ab" < "a-b".
    const char16_t u1[] = u"_a", u2[] = u"ab", u3[] = u"a-b";
    expect(call(s, "CompareStringEx", {0, 0, va(u1), ~0ULL, va(u2), ~0ULL}) == 1 &&
               call(s, "CompareStringEx", {0, 0, va(u2), ~0ULL, va(u3), ~0ULL}) == 1,
           "strings: word-sort order (punctuation first, hyphens ignored until the tie-break)");

    // FormatMessage into a buffer it allocates, freed with LocalFree.
    alignas(8) std::uint64_t text = 0;
    const std::uint64_t n = call(s, "FormatMessageA", {0x100 | 0x200 | 0x1000, 0, 5, 0, va(&text), 0, 0});
    expect(n == 19 && text != 0 &&
               std::string(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(text))) == "Access is denied.\r\n" &&
               call(s, "LocalFree", {text}) == 0,
           "strings: FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER) for error 5, then LocalFree");

    // Heap blocks: a foreign pointer is not freed; growing keeps contents; VirtualQuery.
    const std::uint64_t heap = call(s, "GetProcessHeap", {});
    const std::uint64_t p = call(s, "HeapAlloc", {heap, 8, 24});
    alignas(16) static std::uint8_t foreign[64];
    const bool freed_foreign = call(s, "HeapFree", {heap, 0, va(foreign + 16)}) != 0;
    *reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(p)) = 0x42;
    const std::uint64_t q = call(s, "HeapReAlloc", {heap, 8, p, 4096});
    expect(p != 0 && !freed_foreign && q != 0 && call(s, "HeapSize", {heap, 0, q}) == 4096 &&
               *reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(q)) == 0x42 &&
               *reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(q + 4000)) == 0 && call(s, "HeapFree", {heap, 0, q}) == 1,
           "memory: HeapReAlloc with HEAP_ZERO_MEMORY keeps contents and zeroes the growth; a foreign pointer is refused");
    const std::uint64_t v = call(s, "VirtualAlloc", {0, 3 * 65536, 0x3000, 0x04});
    alignas(8) std::uint64_t mbi[6] = {};
    const bool q1 = call(s, "VirtualQuery", {v + 0x1234, va(mbi), 48}) == 48 && mbi[0] == v + 0x1000 && mbi[1] == v &&
                    mbi[3] == 3 * 65536 - 0x1000;
    expect(v != 0 && (v & 0xFFFF) == 0 && q1 && call(s, "VirtualFree", {v, 0, 0x8000}) == 1,
           "memory: VirtualAlloc is 64 KiB aligned; VirtualQuery reports the region; VirtualFree releases it");
    expect(faults.empty(), "strings: no faults");
}

// --- R4 files (`shim_files.h`): mounts, the install overlay, virtual directories ------------------
void test_files() {
    using thr_test::call;
    namespace tb = wse::recomp::teb;
    namespace sh = wse::recomp::shim;
    std::vector<std::uint8_t> img(0x1000, 0);
    img[0x3C] = 0x80;
    PeImage pe;
    pe.image_base = 0x150000000ULL;
    pe.size_of_image = static_cast<std::uint32_t>(img.size());
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    std::vector<PeImport> named;
    for (std::size_t i = 0; i < std::size(thr_test::kNames); ++i) {
        named.push_back({thr_test::kSlotBase + 8 * i, "KERNEL32.dll", thr_test::kNames[i], 0});
    }
    named.push_back({thr_test::kSlotBase + 8 * std::size(thr_test::kNames), "SHELL32.dll", "SHGetFolderPathA", 0});
    rt.name_imports(named);
    const std::uint64_t sh_slot = thr_test::kSlotBase + 8 * std::size(thr_test::kNames);
    std::string err;
    tb::Process proc(rt, pe, img.data());
    wse::recomp::kernel::Kernel kern(rt);
    wse::platform::MemoryVfs install;
    wse::platform::MemoryVfs user;
    (void)install.write_text("Modules/Native/module.ini", "native");
    (void)install.write_text("Modules/Native/Data/old.txt", "old");
    sh::FilesConfig cfg;
    cfg.install = &install;
    cfg.user = &user;
    sh::Files files(rt, kern, cfg);
    expect(proc.init(&err) && proc.install() >= 2 && kern.install() >= 10 && files.install() >= 11,
           "files: the file shims bind");
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    tb::GuestThread& t = proc.create_thread(0, 0);
    tb::Process::attach(s, t);
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };

    char cwd[64] = {};
    expect(call(s, "GetCurrentDirectoryA", {64, va(cwd)}) == 7 && std::string(cwd) == "C:\\WSE2" &&
               files.canonical("modules\\.\\native\\..\\Native\\module.ini.") == "C:\\WSE2\\modules\\Native\\module.ini",
           "files: the process starts in C:\\WSE2; paths are made absolute and canonical (trailing dot dropped)");

    // Reading from the install (case-insensitively), writing to the layer, the install untouched.
    const char ini[] = "MODULES\\NATIVE\\Module.INI";
    const std::uint64_t h = call(s, "CreateFileA", {va(ini), 0xC0000000ULL, 0, 0, 3, 0x80, 0});
    char buf[16] = {};
    alignas(4) std::uint32_t n = 0;
    const bool read = h != ~0ULL && call(s, "ReadFile", {h, va(buf), 16, va(&n), 0}) == 1 && n == 6 &&
                      std::string(buf, 6) == "native";
    const char more[] = "!";
    (void)call(s, "WriteFile", {h, va(more), 1, va(&n), 0});
    (void)call(s, "CloseHandle", {h});
    std::string layer;
    std::string original;
    expect(read && user.read_text("install/Modules/Native/module.ini", layer) && layer == "native!" &&
               install.read_text("Modules/Native/module.ini", original) && original == "native",
           "files: an install file reads through the overlay; a change goes to the writable layer only");

    // Deleting an install file hides it; the install still has it.
    const char old[] = "C:\\WSE2\\Modules\\Native\\Data\\old.txt";
    expect(call(s, "DeleteFileA", {va(old)}) == 1 && call(s, "GetFileAttributesA", {va(old)}) == 0xFFFFFFFFU &&
               install.is_file("Modules/Native/Data/old.txt"),
           "files: deleting an install file hides it from the guest without touching the install");

    // Ancestors of mounts are read-only directories that list the mounts below them.
    alignas(4) std::uint8_t fd[320] = {};
    const char users[] = "C:\\Users\\*";
    const std::uint64_t f = call(s, "FindFirstFileA", {va(users), va(fd)});
    std::vector<std::string> names;
    if (f != ~0ULL) {
        do {
            names.emplace_back(reinterpret_cast<const char*>(fd + 44));
        } while (call(s, "FindNextFileA", {f, va(fd)}) == 1);
        (void)call(s, "FindClose", {f});
    }
    const char root[] = "C:\\";
    const char newdir[] = "C:\\NewTop";
    expect(names == std::vector<std::string>{".", "..", "Player"} && (call(s, "GetFileAttributesA", {va(root)}) & 0x10) != 0 &&
               call(s, "CreateDirectoryA", {va(newdir), 0}) == 0,
           "files: C:\\Users lists the profile; C:\\ is a directory; nothing can be created outside the mounts");

    // SHGetFolderPathA(CSIDL_PERSONAL) names the layer's Documents, which then exists.
    char docs[260] = {};
    s.gpr[1] = 0;
    s.gpr[2] = 5;
    s.gpr[8] = 0;
    s.gpr[9] = 0;
    {
        // Five arguments: the fifth (the buffer) above the shadow area, as `thr_test::call` lays out.
        const std::uint64_t rsp0 = s.gpr[4];
        s.gpr[4] = (rsp0 - 0x80) & ~std::uint64_t{15};
        const std::uint64_t buf_va = va(docs);
        std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20), &buf_va, 8);
        rt_test::push_and_hook(&s, 0x140031000ULL, sh_slot, true);
        s.gpr[4] = rsp0;
    }
    expect(s.gpr[0] == 0 && std::string(docs) == "C:\\Users\\Player\\Documents" && user.is_dir("Documents"),
           "files: SHGetFolderPathA(CSIDL_PERSONAL) is the profile's Documents in the writable layer");
    expect(faults.empty(), "files: no faults");
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

// --- R4 modules (`pe_image.h` exports, relocations, resources; `runtime.h` late binding;
// `shim_modules.h`): synthetic images in host memory, plain C++ functions for their code ----------
namespace mod_test {

// A mapped image in host memory, 64 KiB-aligned: PE32+ headers with 16 empty data directories.
struct Img {
    static constexpr std::uint32_t kSize = 0x4000;
    std::vector<std::uint8_t> raw;
    std::uint8_t* p = nullptr;
    std::uint64_t base = 0;
    explicit Img(std::uint64_t preferred) : raw(kSize + 0x10000, 0) {
        const auto a = reinterpret_cast<std::uintptr_t>(raw.data());
        base = (static_cast<std::uint64_t>(a) + 0xFFFF) & ~std::uint64_t{0xFFFF};
        p = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(base));
        put16(0, 0x5A4D);
        put32(0x3C, 0x80);
        put32(0x80, 0x4550);
        put16(0x84, 0x8664);
        put16(0x80 + 20, 0xF0);
        put16(0x80 + 24, 0x20B);
        put64(0x80 + 24 + 24, preferred);
        put32(0x80 + 24 + 56, kSize);
        put32(0x80 + 24 + 108, 16);
    }
    void put16(std::size_t off, std::uint16_t v) { std::memcpy(p + off, &v, 2); }
    void put32(std::size_t off, std::uint32_t v) { std::memcpy(p + off, &v, 4); }
    void put64(std::size_t off, std::uint64_t v) { std::memcpy(p + off, &v, 8); }
    [[nodiscard]] std::uint64_t get64(std::size_t off) const {
        std::uint64_t v = 0;
        std::memcpy(&v, p + off, 8);
        return v;
    }
    [[nodiscard]] std::uint32_t get32(std::size_t off) const {
        std::uint32_t v = 0;
        std::memcpy(&v, p + off, 4);
        return v;
    }
    void dir(unsigned i, std::uint32_t rva, std::uint32_t size) {
        put32(0x80 + 24 + 112 + 8 * std::size_t{i}, rva);
        put32(0x80 + 24 + 112 + 8 * std::size_t{i} + 4, size);
    }
    void str(std::size_t off, const char* s) { std::memcpy(p + off, s, std::strlen(s) + 1); }
    [[nodiscard]] PeImage pe(std::uint32_t entry_rva = 0) const {
        PeImage i;
        i.image_base = base;
        i.size_of_image = kSize;
        i.entry_rva = entry_rva;
        return i;
    }
};

std::uint64_t g_main_args[4];
int g_main_calls = 0;
void mod_alpha(CpuState* s) {
    s->gpr[0] = 77;
    rt_test::guest_ret(s);
}
void mod_six(CpuState* s) {
    s->gpr[0] = 6;
    rt_test::guest_ret(s);
}
void mod_dllmain(CpuState* s) {  // DllMain(h, reason, reserved) -> TRUE
    g_main_args[0] = s->gpr[1];
    g_main_args[1] = s->gpr[2];
    g_main_args[2] = s->gpr[8];
    ++g_main_calls;
    s->gpr[0] = 1;
    rt_test::guest_ret(s);
}

// Calls the shim registered as KERNEL32.dll!`name` as translated code would (Win64 arguments, the
// return address pushed). Returns rax; `returned` is false when the shim did not return.
std::uint64_t call(Runtime& rt, CpuState& s, const char* name, std::initializer_list<std::uint64_t> args,
                   bool* returned = nullptr) {
    constexpr std::uint64_t kRet = 0x140031000ULL;
    const std::uint64_t rsp0 = s.gpr[4];
    s.gpr[4] = (rsp0 - 0x80) & ~std::uint64_t{15};
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    std::size_t i = 0;
    for (const std::uint64_t a : args) {
        if (i < 4) {
            s.gpr[kArgReg[i]] = a;
        } else {
            std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20 + 8 * (i - 4)), &a, 8);
        }
        ++i;
    }
    s.gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s.gpr[4]), &kRet, 8);
    const Runtime::ImportShim* shim = rt.named_shim("KERNEL32.dll", name);
    if (shim != nullptr) {
        (*shim)(s);
    }
    if (returned != nullptr) {
        *returned = shim != nullptr && s.rip == kRet;
    }
    s.gpr[4] = rsp0;
    return s.gpr[0];
}

std::uint64_t va(const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }

}  // namespace mod_test

void test_modules() {
    using namespace mod_test;
    namespace sh = wse::recomp::shim;
    std::string err;

    // --- PE readers -------------------------------------------------------------------------
    // The DLL: exports alpha/alias (ordinal 5), an ordinal-only 6, a forwarder fwd (ordinal 7).
    Img dll(0x180000000ULL);
    dll.dir(0, 0x1000, 0x100);
    dll.put32(0x100C, 0x1080);  // Name
    dll.str(0x1080, "TEST.dll");
    dll.put32(0x1010, 5);  // Base
    dll.put32(0x1014, 4);  // NumberOfFunctions
    dll.put32(0x1018, 3);  // NumberOfNames
    dll.put32(0x101C, 0x1040);
    dll.put32(0x1020, 0x1060);
    dll.put32(0x1024, 0x1070);
    dll.put32(0x1040, 0x2000);
    dll.put32(0x1044, 0x2010);
    dll.put32(0x1048, 0x10C0);  // inside the export directory: a forwarder
    dll.put32(0x104C, 0);
    dll.put32(0x1060, 0x10A0);
    dll.put32(0x1064, 0x10A8);
    dll.put32(0x1068, 0x10B0);
    dll.put16(0x1070, 0);
    dll.put16(0x1072, 0);
    dll.put16(0x1074, 2);
    dll.str(0x10A0, "alias");
    dll.str(0x10A8, "alpha");
    dll.str(0x10B0, "fwd");
    dll.str(0x10C0, "KERNEL32.GetTickCount");
    PeExports ex;
    expect(read_pe_exports(dll.p, Img::kSize, ex, &err) && ex.dll_name == "TEST.dll" && ex.entries.size() == 4 &&
               ex.entries[0].name == "alias" && ex.entries[1].name == "alpha" && ex.entries[1].ordinal == 5 &&
               ex.entries[1].rva == 0x2000 && ex.entries[2].name.empty() && ex.entries[2].ordinal == 6 &&
               ex.entries[3].name == "fwd" && ex.entries[3].forwarder == "KERNEL32.GetTickCount",
           "exports: names, ordinals, an ordinal-only entry and a forwarder are read");
    // Base relocations: a DIR64 and a HIGHLOW, moving the DLL from 0x180000000 to where it is.
    dll.dir(5, 0x1400, 0x10);
    dll.put32(0x1400, 0x2000);
    dll.put32(0x1404, 0x10);
    dll.put16(0x1408, static_cast<std::uint16_t>((10U << 12) | 0x100));
    dll.put16(0x140A, static_cast<std::uint16_t>((3U << 12) | 0x108));
    dll.put64(0x2100, 0x180002000ULL);
    dll.put32(0x2108, 0x11223344U);
    const std::uint64_t delta = dll.base - 0x180000000ULL;
    std::size_t applied = 0;
    expect(apply_pe_relocations(dll.p, Img::kSize, dll.base, &applied, &err) && applied == 2 &&
               dll.get64(0x2100) == dll.base + 0x2000 &&
               dll.get32(0x2108) == static_cast<std::uint32_t>(0x11223344U + delta) && dll.get64(0x80 + 24 + 24) == dll.base,
           "relocations: DIR64 and HIGHLOW move by the delta, and the header's ImageBase follows");
    expect(apply_pe_relocations(dll.p, Img::kSize, dll.base, &applied, &err) && applied == 0,
           "relocations: an image already at its base is left alone");
    {
        Img bad(0x180000000ULL);
        bad.dir(5, 0x1400, 0x0C);
        bad.put32(0x1400, 0x2000);
        bad.put32(0x1404, 0x0C);
        bad.put16(0x1408, static_cast<std::uint16_t>((4U << 12) | 0x10));
        expect(!apply_pe_relocations(bad.p, Img::kSize, bad.base, &applied, &err), "relocations: an unknown type fails");
        bad.put16(0x80 + 22, 1);  // IMAGE_FILE_RELOCS_STRIPPED
        expect(!apply_pe_relocations(bad.p, Img::kSize, bad.base, &applied, &err) &&
                   err.find("stripped") != std::string::npos,
               "relocations: an image with relocations stripped cannot move");
    }

    // The exe: imports TEST.dll!alpha, TEST.dll!#6, TEST.dll!missing and KERNEL32.dll!GetTickCount;
    // resources: type 256 { "ABC" (0x409), 7 (0x407 "de", 0x409 "en") }.
    Img exe(0x140000000ULL);
    exe.dir(1, 0x1000, 60);
    exe.put32(0x1000, 0x1100);
    exe.put32(0x100C, 0x1200);
    exe.put32(0x1010, 0x1300);
    exe.put32(0x1014, 0x1140);
    exe.put32(0x1020, 0x1210);
    exe.put32(0x1024, 0x1340);
    exe.str(0x1200, "TEST.dll");
    exe.str(0x1210, "KERNEL32.dll");
    exe.put64(0x1100, 0x1400);
    exe.put64(0x1108, 0x8000000000000006ULL);
    exe.put64(0x1110, 0x1420);
    exe.put64(0x1140, 0x1410);
    exe.str(0x1402, "alpha");
    exe.str(0x1412, "GetTickCount");
    exe.str(0x1422, "missing");
    for (const std::size_t slot : {std::size_t{0x1300}, std::size_t{0x1308}, std::size_t{0x1310}, std::size_t{0x1340}}) {
        exe.put64(slot, 0xDEAD);  // the loader overwrites these
    }
    exe.dir(2, 0x2000, 0x300);
    exe.put16(0x2000 + 14, 1);  // root: one ID entry
    exe.put32(0x2010, 256);
    exe.put32(0x2014, 0x80000000U | 0x18);
    exe.put16(0x2018 + 12, 1);  // type 256: one named, one ID entry
    exe.put16(0x2018 + 14, 1);
    exe.put32(0x2028, 0x80000000U | 0x100);
    exe.put32(0x202C, 0x80000000U | 0x40);
    exe.put32(0x2030, 7);
    exe.put32(0x2034, 0x80000000U | 0x60);
    exe.put16(0x2040 + 14, 1);  // "ABC": 0x409
    exe.put32(0x2050, 0x409);
    exe.put32(0x2054, 0xA0);
    exe.put16(0x2060 + 14, 2);  // 7: 0x407, 0x409
    exe.put32(0x2070, 0x407);
    exe.put32(0x2074, 0xB0);
    exe.put32(0x2078, 0x409);
    exe.put32(0x207C, 0xC0);
    exe.put32(0x20A0, 0x2200);
    exe.put32(0x20A4, 3);
    exe.put32(0x20B0, 0x2210);
    exe.put32(0x20B4, 2);
    exe.put32(0x20C0, 0x2220);
    exe.put32(0x20C4, 2);
    exe.put16(0x2100, 3);
    exe.put16(0x2102, u'A');
    exe.put16(0x2104, u'B');
    exe.put16(0x2106, u'C');
    exe.str(0x2200, "abc");
    exe.str(0x2210, "de");
    exe.str(0x2220, "en");
    PeResourceKey t256;
    t256.id = 256;
    PeResourceKey k7;
    k7.id = 7;
    PeResourceKey abc;
    abc.name = u"abc";
    PeResourceMiss miss = PeResourceMiss::none;
    expect(find_pe_resource(exe.p, Img::kSize, t256, k7, -1, &miss) == 0x20C0 &&
               find_pe_resource(exe.p, Img::kSize, t256, k7, 0x407, &miss) == 0x20B0 &&
               find_pe_resource(exe.p, Img::kSize, t256, abc, -1, &miss) == 0x20A0 && miss == PeResourceMiss::none,
           "resources: by ID and by case-insensitive name; no neutral language picks en-US, or the language asked");
    PeResourceKey k8;
    k8.id = 8;
    PeResourceKey t9;
    t9.id = 9;
    expect(find_pe_resource(exe.p, Img::kSize, t256, k7, 0x40C, &miss) == 0 && miss == PeResourceMiss::language &&
               find_pe_resource(exe.p, Img::kSize, t256, k8, -1, &miss) == 0 && miss == PeResourceMiss::name &&
               find_pe_resource(exe.p, Img::kSize, t9, k7, -1, &miss) == 0 && miss == PeResourceMiss::type &&
               find_pe_resource(dll.p, Img::kSize, t256, k7, -1, &miss) == 0 && miss == PeResourceMiss::no_directory,
           "resources: a missing language, name, type or directory says which");

    // --- the registry -------------------------------------------------------------------------
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    const FnRow rows[] = {{dll.base + 0x2000, &mod_alpha}, {dll.base + 0x2010, &mod_six}, {dll.base + 0x2200, &mod_dllmain}};
    rt.add_table(rows, 3);
    // Late binding: a shim registered before any image imports it binds when the image is named.
    expect(rt.set_import("KERNEL32.dll", "GetTickCount", [](CpuState& st) { win64_return(st, 1234); }) == 0 &&
               rt.has_shims_for("kernel32.DLL") && !rt.has_shims_for("user32.dll"),
           "late binding: a shim registers by name before any image imports it");
    sh::Modules mods(rt);
    expect(mods.add_image("C:\\WSE2\\game.exe", exe.pe(), exe.p, true, &err) != nullptr &&
               mods.add_image("C:\\WSE2\\TEST.DLL", dll.pe(0x2200), dll.p, false, &err) != nullptr,
           "modules: the exe and a DLL register");
    std::vector<std::string> unresolved;
    expect(mods.link(&unresolved) == 2 && unresolved.size() == 1 && unresolved[0] == "TEST.dll!missing",
           "modules: imports from a translated DLL bind to its exports (by name and ordinal); a missing one is listed");
    rt.fill_iat();
    expect(exe.get64(0x1300) == dll.base + 0x2000 && exe.get64(0x1308) == dll.base + 0x2010 &&
               exe.get64(0x1340) == exe.base + 0x1340,
           "modules: the IAT holds the DLL's code for its imports and the slot itself for a shim");
    (void)mods.install();  // binds no slot here (the exe imports none): the shims register by name
    expect(rt.named_shim("KERNEL32.dll", "GetProcAddress") != nullptr &&
               rt.named_shim("KERNELBASE.dll", "LoadLibraryExW") != nullptr &&
               rt.named_shim("ntdll.dll", "RtlPcToFileHeader") != nullptr,
           "modules: the module shims register (KERNEL32, KERNELBASE, ntdll spellings)");
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    alignas(16) static std::uint8_t teb[0x2000];
    s.teb = va(teb);
    auto last_error = [&]() {
        std::uint32_t e = 0;
        std::memcpy(&e, teb + 0x68, 4);
        return e;
    };
    rt_test::push_and_hook(&s, 0x140030000ULL, exe.base + 0x1300, true);
    const std::uint64_t r_alpha = s.gpr[0];
    rt_test::push_and_hook(&s, 0x140030010ULL, exe.base + 0x1340, true);
    expect(faults.empty() && r_alpha == 77 && s.gpr[0] == 1234 && s.rip == 0x140030010ULL,
           "modules: a call through the IAT reaches the DLL's code, and a late-bound shim");
    g_main_calls = 0;
    expect(mods.attach_dlls(s) && g_main_calls == 1 && g_main_args[0] == dll.base && g_main_args[1] == 1 &&
               g_main_args[2] != 0 && mods.attach_dlls(s) && g_main_calls == 1,
           "modules: DllMain runs once with (handle, DLL_PROCESS_ATTACH, non-null for a static load)");

    // GetModuleHandle*
    const std::uint64_t k32 = call(rt, s, "GetModuleHandleA", {va("kernel32.dll")});
    expect(call(rt, s, "GetModuleHandleA", {va("test")}) == dll.base &&
               call(rt, s, "GetModuleHandleA", {va("TEST.DLL")}) == dll.base &&
               call(rt, s, "GetModuleHandleW", {va(u"Test")}) == dll.base && call(rt, s, "GetModuleHandleA", {0}) == exe.base &&
               k32 != 0 && (k32 & 0xFFFF) == 0 &&
               call(rt, s, "GetModuleHandleA", {va("api-ms-win-core-synch-l1-2-0")}) == k32 &&
               call(rt, s, "GetModuleHandleA", {va("C:\\Windows\\System32\\KERNEL32.DLL")}) == k32,
           "GetModuleHandle: by name (case, extension, path), NULL is the exe, API sets are KERNEL32");
    expect(call(rt, s, "GetModuleHandleA", {va("nope.dll")}) == 0 && last_error() == 126,
           "GetModuleHandle: an unloaded module fails with ERROR_MOD_NOT_FOUND");
    std::uint64_t h = 1;
    expect(call(rt, s, "GetModuleHandleExW", {4, dll.base + 0x2005, va(&h)}) == 1 && h == dll.base &&
               call(rt, s, "GetModuleHandleExW", {0, 0, va(&h)}) == 1 && h == exe.base &&
               call(rt, s, "GetModuleHandleExW", {0, va(u"nope"), va(&h)}) == 0 && h == 0 && last_error() == 126,
           "GetModuleHandleEx: from an address, NULL, a missing name (handle NULL, ERROR_MOD_NOT_FOUND)");
    std::uint64_t pc_base = 1;
    expect(call(rt, s, "RtlPcToFileHeader", {dll.base + 0x2005, va(&pc_base)}) == dll.base && pc_base == dll.base &&
               call(rt, s, "RtlPcToFileHeader", {0x10, va(&pc_base)}) == 0 && pc_base == 0,
           "RtlPcToFileHeader: the module containing the address, or NULL");

    // GetProcAddress
    const std::uint64_t tick = call(rt, s, "GetProcAddress", {k32, va("GetTickCount")});
    expect(call(rt, s, "GetProcAddress", {dll.base, va("alpha")}) == dll.base + 0x2000 &&
               call(rt, s, "GetProcAddress", {dll.base, va("alias")}) == dll.base + 0x2000 &&
               call(rt, s, "GetProcAddress", {dll.base, 6}) == dll.base + 0x2010 &&
               call(rt, s, "GetProcAddress", {dll.base, 5}) == dll.base + 0x2000 && tick > k32 && tick < k32 + 0x10000 &&
               call(rt, s, "GetProcAddress", {dll.base, va("fwd")}) == tick &&
               call(rt, s, "GetProcAddress", {k32, va("GetTickCount")}) == tick,
           "GetProcAddress: by name, alias, ordinal; a forwarder and KERNEL32 give the same shim address");
    expect(call(rt, s, "GetProcAddress", {dll.base, va("Alpha")}) == 0 && last_error() == 127 &&
               call(rt, s, "GetProcAddress", {k32, va("WseNoSuchExport")}) == 0 && last_error() == 127 &&
               call(rt, s, "GetProcAddress", {dll.base, 9}) == 0,
           "GetProcAddress: names are case-sensitive; a missing export fails with ERROR_PROC_NOT_FOUND");
    s.gpr[0] = 0;
    rt_test::push_and_hook(&s, 0x140030020ULL, tick, false);
    expect(faults.empty() && s.gpr[0] == 1234 && s.rip == 0x140030020ULL,
           "GetProcAddress: calling a system export through a pointer runs its shim");
    rt_test::push_and_hook(&s, 0x140030030ULL, k32 + 0x1000 + 16 * 500, false);
    expect(faults.size() == 1 && faults[0].kind == FaultKind::missing_function,
           "modules: a call into a system module where no export was handed out faults");
    faults.clear();

    // LoadLibrary / FreeLibrary
    expect(call(rt, s, "LoadLibraryA", {va("user32.dll")}) == 0 && last_error() == 126,
           "LoadLibrary: a DLL with no shims is not found");
    (void)rt.set_import("USER32.dll", "GetFocus", [](CpuState& st) { win64_return(st, 0); });
    const std::uint64_t u32 = call(rt, s, "LoadLibraryExW", {va(u"USER32"), 0, 0});
    expect(u32 != 0 && u32 != k32 && call(rt, s, "GetProcAddress", {u32, va("GetFocus")}) != 0 &&
               call(rt, s, "LoadLibraryA", {va("TEST.dll")}) == dll.base && g_main_calls == 1 &&
               call(rt, s, "FreeLibrary", {dll.base}) == 1 && call(rt, s, "FreeLibrary", {0x10}) == 0,
           "LoadLibrary: a DLL with shims loads; a loaded DLL returns its handle without a second DllMain");

    // GetModuleFileName
    char buf[64];
    char16_t wbuf[64];
    expect(call(rt, s, "GetModuleFileNameA", {dll.base, va(buf), 64}) == 16 && std::strcmp(buf, "C:\\WSE2\\TEST.DLL") == 0,
           "GetModuleFileName: the path the module was registered with");
    expect(call(rt, s, "GetModuleFileNameA", {dll.base, va(buf), 4}) == 4 && std::strcmp(buf, "C:\\") == 0 &&
               last_error() == 122,
           "GetModuleFileName: a short buffer truncates, returns its size and sets ERROR_INSUFFICIENT_BUFFER");
    expect(call(rt, s, "GetModuleFileNameW", {0, va(wbuf), 64}) == 16 && std::u16string(wbuf) == u"C:\\WSE2\\game.exe" &&
               call(rt, s, "GetModuleFileNameA", {k32, va(buf), 64}) == 32 &&
               std::strcmp(buf, "C:\\Windows\\System32\\KERNEL32.dll") == 0,
           "GetModuleFileName: NULL is the exe (wide); a system DLL is in System32");

    // Resources
    const std::uint64_t r7 = call(rt, s, "FindResourceA", {exe.base, 7, 256});
    expect(r7 == exe.base + 0x20C0 && call(rt, s, "LoadResource", {exe.base, r7}) == exe.base + 0x2220 &&
               call(rt, s, "SizeofResource", {exe.base, r7}) == 2 &&
               call(rt, s, "LockResource", {exe.base + 0x2220}) == exe.base + 0x2220 &&
               call(rt, s, "FindResourceA", {0, va("#7"), va("#256")}) == r7 &&
               call(rt, s, "FindResourceW", {exe.base, va(u"abc"), 256}) == exe.base + 0x20A0 &&
               call(rt, s, "FindResourceExW", {exe.base, 256, 7, 0x407}) == exe.base + 0x20B0 &&
               call(rt, s, "FindResourceExW", {exe.base, 256, 7, 0}) == r7,
           "FindResource: by ID, \"#n\", name; FindResourceEx by language (0: the default choice)");
    expect(call(rt, s, "FindResourceExW", {exe.base, 256, 7, 0x40C}) == 0 && last_error() == 1815 &&
               call(rt, s, "FindResourceA", {exe.base, 8, 256}) == 0 && last_error() == 1814 &&
               call(rt, s, "FindResourceA", {exe.base, 7, 257}) == 0 && last_error() == 1813 &&
               call(rt, s, "FindResourceA", {dll.base, 7, 256}) == 0 && last_error() == 1812 &&
               call(rt, s, "FindResourceA", {k32, 7, 256}) == 0 && last_error() == 1812,
           "FindResource: language, name, type missing; no resources (DLL, system DLL)");
    expect(faults.empty(), "modules: no faults");
}

// --- R4 ADVAPI32 (`shim_advapi.h`): the registry hive and Crypt* hashes ------------------------
namespace adv_test {

std::uint64_t call(Runtime& rt, CpuState& s, const char* name, std::initializer_list<std::uint64_t> args) {
    constexpr std::uint64_t kRet = 0x140031000ULL;
    const std::uint64_t rsp0 = s.gpr[4];
    s.gpr[4] = (rsp0 - 0x80) & ~std::uint64_t{15};
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    std::size_t i = 0;
    for (const std::uint64_t a : args) {
        if (i < 4) {
            s.gpr[kArgReg[i]] = a;
        } else {
            std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20 + 8 * (i - 4)), &a, 8);
        }
        ++i;
    }
    s.gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s.gpr[4]), &kRet, 8);
    const Runtime::ImportShim* shim = rt.named_shim("ADVAPI32.dll", name);
    s.gpr[0] = 0xBADBADBAD;
    if (shim != nullptr) {
        (*shim)(s);
    }
    s.gpr[4] = rsp0;
    return s.gpr[0];
}

std::string hex(const std::vector<std::uint8_t>& v) {
    std::string out;
    for (const std::uint8_t b : v) {
        char x[3];
        std::snprintf(x, sizeof(x), "%02x", b);
        out += x;
    }
    return out;
}

}  // namespace adv_test

void test_advapi() {
    using adv_test::call;
    using adv_test::hex;
    namespace sh = wse::recomp::shim;
    const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
    std::vector<std::uint8_t> million(1000000, 'a');
    expect(hex(sh::md5({})) == "d41d8cd98f00b204e9800998ecf8427e" && hex(sh::md5(abc)) == "900150983cd24fb0d6963f7d28e17f72" &&
               hex(sh::sha1(abc)) == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
               hex(sh::sha1(million)) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f" &&
               hex(sh::sha256(abc)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
               hex(sh::sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "advapi: MD5, SHA-1 and SHA-256 match the RFC 1321 / FIPS 180 test vectors");

    Runtime rt;
    wse::platform::MemoryVfs user;
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    alignas(16) static std::uint8_t teb[0x2000];
    s.teb = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(teb));
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };
    auto last_error = [&]() {
        std::uint32_t e = 0;
        std::memcpy(&e, teb + 0x68, 4);
        return e;
    };
    {
        sh::Advapi adv(rt, &user);
        (void)adv.install();
        std::uint64_t key = 0;
        std::uint32_t disp = 0;
        expect(call(rt, s, "RegCreateKeyExA", {sh::kHkeyCurrentUser, va("Software\\WSE2\\Test"), 0, 0, 0, 0xF003F, 0, va(&key), va(&disp)}) == 0 &&
                   key != 0 && disp == sh::kRegCreatedNewKey,
               "registry: RegCreateKeyExA creates the key path");
        std::uint64_t key2 = 0;
        expect(call(rt, s, "RegCreateKeyExA", {sh::kHkeyCurrentUser, va("software\\wse2\\TEST"), 0, 0, 0, 0xF003F, 0, va(&key2), va(&disp)}) == 0 &&
                   disp == sh::kRegOpenedExistingKey,
               "registry: key names compare case-insensitively");
        const char text[] = "hello";
        const std::uint32_t dword = 0x12345678;
        expect(call(rt, s, "RegSetValueExA", {key, va("Greeting"), 0, sh::kRegSz, va(text), sizeof(text)}) == 0 &&
                   call(rt, s, "RegSetValueExA", {key2, va("Count"), 0, sh::kRegDword, va(&dword), 4}) == 0,
               "registry: RegSetValueExA stores string and DWORD values");
        char buf[16] = {};
        std::uint32_t type = 0;
        std::uint32_t size = sizeof(buf);
        expect(call(rt, s, "RegQueryValueExA", {key2, va("GREETING"), 0, va(&type), va(buf), va(&size)}) == 0 && type == sh::kRegSz &&
                   size == 6 && std::strcmp(buf, "hello") == 0,
               "registry: RegQueryValueExA reads a value back (name case-insensitive)");
        size = 3;
        expect(call(rt, s, "RegQueryValueExA", {key, va("Greeting"), 0, 0, va(buf), va(&size)}) == sh::kErrorMoreData && size == 6,
               "registry: a short buffer gets ERROR_MORE_DATA and the size needed");
        size = 0;
        expect(call(rt, s, "RegQueryValueExA", {key, va("Count"), 0, va(&type), 0, va(&size)}) == 0 && size == 4 && type == sh::kRegDword &&
                   call(rt, s, "RegQueryValueExA", {key, va("Missing"), 0, 0, 0, va(&size)}) == 2,
               "registry: a NULL buffer asks the size; a missing value is ERROR_FILE_NOT_FOUND");
        expect(call(rt, s, "RegCloseKey", {key}) == 0 && call(rt, s, "RegCloseKey", {key}) == 6 &&
                   call(rt, s, "RegCloseKey", {sh::kHkeyLocalMachine}) == 0 &&
                   call(rt, s, "RegQueryValueExA", {key, va("Count"), 0, 0, 0, va(&size)}) == 6,
               "registry: RegCloseKey closes once; a closed handle is ERROR_INVALID_HANDLE");

        // Crypt* over the same hashes.
        std::uint64_t prov = 0;
        std::uint64_t hash = 0;
        expect(call(rt, s, "CryptAcquireContextA", {va(&prov), 0, 0, 1, 0xF0000000}) == 1 &&
                   call(rt, s, "CryptCreateHash", {prov, sh::kCalgMd5, 0, 0, va(&hash)}) == 1 &&
                   call(rt, s, "CryptHashData", {hash, va("ab"), 2, 0}) == 1 && call(rt, s, "CryptHashData", {hash, va("c"), 1, 0}) == 1,
               "crypt: acquire, create an MD5 hash, hash data in two parts");
        std::uint8_t digest[32] = {};
        std::uint32_t len = 0;
        expect(call(rt, s, "CryptGetHashParam", {hash, sh::kHpHashVal, 0, va(&len), 0}) == 1 && len == 16 &&
                   call(rt, s, "CryptGetHashParam", {hash, sh::kHpHashVal, va(digest), va(&len), 0}) == 1 &&
                   hex(std::vector<std::uint8_t>(digest, digest + 16)) == "900150983cd24fb0d6963f7d28e17f72",
               "crypt: CryptGetHashParam(HP_HASHVAL) gives the size, then the MD5 of the data");
        expect(call(rt, s, "CryptHashData", {hash, va("x"), 1, 0}) == 0 && last_error() == 0x8009000BU &&
                   call(rt, s, "CryptDestroyHash", {hash}) == 1 && call(rt, s, "CryptDestroyHash", {hash}) == 0,
               "crypt: a finished hash takes no more data (NTE_BAD_HASH_STATE); destroy once");
        std::uint64_t h2 = 0;
        len = 4;
        expect(call(rt, s, "CryptCreateHash", {prov, sh::kCalgSha1, 0, 0, va(&h2)}) == 1 &&
                   call(rt, s, "CryptGetHashParam", {h2, sh::kHpHashSize, va(digest), va(&len), 0}) == 1 && digest[0] == 20 &&
                   call(rt, s, "CryptCreateHash", {prov, 0x6610, 0, 0, va(&h2)}) == 0 && last_error() == sh::kNteBadAlgid &&
                   call(rt, s, "CryptReleaseContext", {prov, 0}) == 1,
               "crypt: HP_HASHSIZE of SHA-1 is 20; an unknown algorithm is NTE_BAD_ALGID");
    }
    // The hive persists in the writable layer and loads into a new instance.
    sh::Advapi again(rt, &user);
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
    expect(user.is_file(sh::Advapi::kHiveFile) && again.get_value("HKCU\\Software\\WSE2\\Test", "count", type, data) &&
               type == sh::kRegDword && data.size() == 4 && data[0] == 0x78 && data[3] == 0x12 &&
               again.get_value("HKEY_CURRENT_USER\\software\\wse2\\test", "Greeting", type, data) && data.size() == 6,
           "registry: the hive is written to NTUSER.DAT and read back");
}

// --- R4 USER32 (`shim_user32.h`): classes, fake windows, message queues --------------------------
namespace u32_test {

constexpr std::uint64_t kProcVa = 0x140070000ULL;
struct Seen {
    std::uint64_t hwnd, msg, w, l;
};
std::vector<Seen> g_seen;
std::uint64_t g_cs_cx = 0;
std::uint64_t g_cs_param = 0;

void wndproc(CpuState* s) {  // LRESULT proc(hwnd, msg, wParam, lParam)
    const Seen e{s->gpr[1], s->gpr[2] & 0xFFFFFFFF, s->gpr[8], s->gpr[9]};
    g_seen.push_back(e);
    if (e.msg == wse::recomp::shim::kWmCreate) {
        std::int32_t cx = 0;
        std::memcpy(&cx, reinterpret_cast<const void*>(e.l + 36), 4);
        g_cs_cx = static_cast<std::uint64_t>(cx);
        std::memcpy(&g_cs_param, reinterpret_cast<const void*>(e.l), 8);
    }
    s->gpr[0] = e.msg == wse::recomp::shim::kWmNcCreate ? 1 : (e.msg == wse::recomp::shim::kWmUser ? 0x55 : 0);
    rt_test::guest_ret(s);
}

std::uint64_t call(Runtime& rt, CpuState& s, const char* dll, const char* name, std::initializer_list<std::uint64_t> args) {
    constexpr std::uint64_t kRet = 0x140031000ULL;
    const std::uint64_t rsp0 = s.gpr[4];
    s.gpr[4] = (rsp0 - 0x100) & ~std::uint64_t{15};
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    std::size_t i = 0;
    for (const std::uint64_t a : args) {
        if (i < 4) {
            s.gpr[kArgReg[i]] = a;
        } else {
            std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20 + 8 * (i - 4)), &a, 8);
        }
        ++i;
    }
    s.gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s.gpr[4]), &kRet, 8);
    const Runtime::ImportShim* shim = rt.named_shim(dll, name);
    s.gpr[0] = 0xBADBADBAD;
    if (shim != nullptr) {
        (*shim)(s);
    }
    s.gpr[4] = rsp0;
    return s.gpr[0];
}

}  // namespace u32_test

void test_user32() {
    using namespace u32_test;
    namespace sh = wse::recomp::shim;
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    const FnRow rows[] = {{kProcVa, &wndproc}};
    rt.add_table(rows, 1);
    sh::User32 user(rt);
    (void)user.install();
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    alignas(16) static std::uint8_t teb[0x2000];
    s.teb = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(teb));
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };
    auto last_error = [&]() {
        std::uint32_t e = 0;
        std::memcpy(&e, teb + 0x68, 4);
        return e;
    };
    auto U = [&](const char* name, std::initializer_list<std::uint64_t> args) { return call(rt, s, "USER32.dll", name, args); };

    alignas(8) std::uint8_t wc[80] = {};
    const std::uint32_t wc_size = 80;
    std::memcpy(wc, &wc_size, 4);
    std::memcpy(wc + 8, &kProcVa, 8);
    const std::uint64_t cname = va(u"WSE2Test");
    std::memcpy(wc + 64, &cname, 8);
    const std::uint64_t atom = U("RegisterClassExW", {va(wc)});
    expect(atom >= 0xC000 && U("RegisterClassExW", {va(wc)}) == 0 && last_error() == sh::kErrorClassAlreadyExists,
           "user32: RegisterClassExW returns an atom; registering the name twice fails");
    g_seen.clear();
    const std::uint64_t hwnd = U("CreateWindowExW", {0, va(u"wse2test"), va(u"Mount&Blade"), 0x00CF0000, sh::kCwUseDefault, 0,
                                                     sh::kCwUseDefault, 0, 0, 0, 0, 0x1234});
    expect(hwnd != 0 && g_seen.size() == 2 && g_seen[0].msg == sh::kWmNcCreate && g_seen[1].msg == sh::kWmCreate &&
               g_seen[1].hwnd == hwnd && g_cs_cx == 1920 && g_cs_param == 0x1234 && faults.empty(),
           "user32: CreateWindowExW sends WM_NCCREATE and WM_CREATE with the CREATESTRUCT (default size: the screen)");
    expect(U("CreateWindowExW", {0, va(u"NoSuchClass"), 0, 0, 0, 0, 10, 10, 0, 0, 0, 0}) == 0 &&
               last_error() == sh::kErrorCannotFindWndClass,
           "user32: an unknown class fails with ERROR_CANNOT_FIND_WND_CLASS");
    g_seen.clear();
    expect(U("ShowWindow", {hwnd, 1}) == 0 && g_seen.size() == 2 && g_seen[0].msg == sh::kWmShowWindow &&
               g_seen[1].msg == sh::kWmSize && g_seen[1].l == (1920U | (1080U << 16)) && U("ShowWindow", {hwnd, 1}) == 1 &&
               g_seen.size() == 2,
           "user32: ShowWindow sends WM_SHOWWINDOW and WM_SIZE once, and returns the previous visibility");
    std::int32_t r[4] = {};
    g_seen.clear();
    expect(U("SetWindowPos", {hwnd, 0, 10, 20, 800, 600, 0}) == 1 && g_seen.size() == 1 && g_seen[0].msg == sh::kWmSize &&
               U("GetClientRect", {hwnd, va(r)}) == 1 && r[0] == 0 && r[2] == 800 && r[3] == 600 &&
               U("GetWindowRect", {hwnd, va(r)}) == 1 && r[0] == 10 && r[1] == 20 && r[2] == 810 && r[3] == 620,
           "user32: SetWindowPos moves and resizes (WM_SIZE); the client rect is the window's size");
    std::int32_t pt[2] = {5, 6};
    expect(U("ClientToScreen", {hwnd, va(pt)}) == 1 && pt[0] == 15 && pt[1] == 26 && U("ScreenToClient", {hwnd, va(pt)}) == 1 &&
               pt[0] == 5 && pt[1] == 6,
           "user32: ClientToScreen and ScreenToClient offset by the window position");
    expect(U("SetWindowLongA", {hwnd, static_cast<std::uint64_t>(-21), 0xABCD}) == 0 &&
               U("GetWindowLongA", {hwnd, static_cast<std::uint64_t>(-21)}) == 0xABCD &&
               U("GetWindowLongA", {hwnd, static_cast<std::uint64_t>(-16)}) == 0x00CF0000,
           "user32: window longs (GWL_USERDATA, GWL_STYLE)");

    // Messages: queued by the embedder, peeked, removed and dispatched to the procedure.
    alignas(8) std::uint8_t msg[48] = {};
    expect(user.post(hwnd, sh::kWmKeyDown, 0x41, 0x1E0001) && U("PeekMessageW", {va(msg), 0, 0, 0, 0}) == 1 &&
               U("PeekMessageW", {va(msg), 0, 0, 0, sh::kPmRemove}) == 1 && U("PeekMessageW", {va(msg), 0, 0, 0, sh::kPmRemove}) == 0,
           "user32: PeekMessageW without PM_REMOVE leaves the message; with it, takes it once");
    U("PeekMessageW", {va(msg), 0, 0, 0, 0});
    user.post(hwnd, sh::kWmKeyDown, 0x42, 7);
    std::uint32_t mtype = 0;
    (void)U("PeekMessageW", {va(msg), 0, 0, 0, sh::kPmRemove});
    std::memcpy(&mtype, msg + 8, 4);
    g_seen.clear();
    expect(mtype == sh::kWmKeyDown && U("DispatchMessageW", {va(msg)}) == 0 && g_seen.size() == 1 && g_seen[0].w == 0x42 &&
               g_seen[0].l == 7,
           "user32: DispatchMessageW calls the window procedure with the message");
    expect(U("SendMessageA", {hwnd, sh::kWmUser, 1, 2}) == 0x55, "user32: SendMessageA returns the procedure's result");
    user.post(hwnd, sh::kWmMouseMove, 0, 0);
    user.post(hwnd, sh::kWmKeyDown, 0, 0);
    expect(U("PeekMessageW", {va(msg), 0, sh::kWmKeyDown, sh::kWmKeyDown, sh::kPmRemove}) == 1 &&
               std::memcmp(msg + 8, &sh::kWmKeyDown, 4) == 0 && U("PeekMessageW", {va(msg), 0, 0, 0, sh::kPmRemove}) == 1 &&
               std::memcmp(msg + 8, &sh::kWmMouseMove, 4) == 0,
           "user32: a message range filter picks a later message and leaves the others in order");
    // GetMessageW blocks until another thread posts, and returns 0 for WM_QUIT.
    std::thread poster([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        user.post(hwnd, sh::kWmUser + 1, 9, 0);
    });
    const std::uint64_t got = U("GetMessageW", {va(msg), 0, 0, 0});
    poster.join();
    std::uint64_t wparam = 0;
    std::memcpy(&wparam, msg + 16, 8);
    expect(got == 1 && wparam == 9, "user32: GetMessageW waits for a message posted from another thread");
    U("PostQuitMessage", {7});
    expect(U("GetMessageW", {va(msg), 0, 0, 0}) == 0 && std::memcmp(msg + 8, &sh::kWmQuit, 4) == 0 && msg[16] == 7,
           "user32: PostQuitMessage makes GetMessageW return 0 with the exit code");

    // Metrics, cursor, clipboard, message box, IMM32.
    std::string boxed;
    user.on_message_box = [&](const std::string& c, const std::string& t) { boxed = c + ": " + t; };
    std::int32_t cur[2] = {};
    expect(U("GetSystemMetrics", {0}) == 1920 && U("GetSystemMetrics", {1}) == 1080 && U("SetCursorPos", {100, 200}) == 1 &&
               U("GetCursorPos", {va(cur)}) == 1 && cur[0] == 100 && cur[1] == 200 &&
               U("MessageBoxA", {hwnd, va("text"), va("Caption"), 0}) == 1 && boxed == "Caption: text",
           "user32: screen metrics, the cursor position, MessageBoxA (reported, IDOK)");
    char clip[] = "copied";
    expect(U("OpenClipboard", {hwnd}) == 1 && U("EmptyClipboard", {}) == 1 && U("SetClipboardData", {1, va(clip)}) == va(clip) &&
               U("IsClipboardFormatAvailable", {1}) == 1 && U("IsClipboardFormatAvailable", {13}) == 0 &&
               std::strcmp(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(U("GetClipboardData", {1}))), "copied") == 0 &&
               U("CloseClipboard", {}) == 1,
           "user32: text through the clipboard");
    expect(call(rt, s, "IMM32.dll", "ImmGetContext", {hwnd}) == 0 && call(rt, s, "IMM32.dll", "ImmAssociateContextEx", {hwnd, 0, 0}) == 1 &&
               call(rt, s, "GDI32.dll", "GetStockObject", {4}) != 0,
           "user32: no IME context; GDI stock objects are handles");
    g_seen.clear();
    expect(U("DestroyWindow", {hwnd}) == 1 && g_seen.size() == 2 && g_seen[0].msg == sh::kWmDestroy &&
               g_seen[1].msg == sh::kWmNcDestroy && U("GetClientRect", {hwnd, va(r)}) == 0 &&
               last_error() == sh::kErrorInvalidWindowHandle,
           "user32: DestroyWindow sends WM_DESTROY and WM_NCDESTROY; the handle is then invalid");
    expect(faults.empty(), "user32: no faults");
}

// --- R4 offline network and small system DLLs (`shim_net.h`, `shim_system.h`) ------------------
void test_net_system() {
    using u32_test::call;
    namespace sh = wse::recomp::shim;
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    // Imports by ordinal bind to shims registered as "#n" (WS2_32 htons is ordinal 9).
    alignas(8) static std::uint64_t slots[2] = {0, 0};
    const auto slot0 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&slots[0]));
    rt.name_imports({PeImport{slot0, "WS2_32.dll", "", 9}, PeImport{slot0 + 8, "OLEAUT32.dll", "", 2}});
    sh::Net net(rt);
    sh::SystemDlls sys(rt);
    expect(net.install() >= 1 && sys.install() >= 1, "net/system: imports by ordinal bind to shims registered as \"#n\"");
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    alignas(16) static std::uint8_t teb[0x2000];
    s.teb = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(teb));
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };
    s.gpr[1] = 0x1234;
    rt_test::push_and_hook(&s, 0x140030000ULL, slot0, true);
    expect(faults.empty() && s.gpr[0] == 0x3412, "net: htons through an import by ordinal");
    auto W = [&](const char* name, std::initializer_list<std::uint64_t> args) { return call(rt, s, "WS2_32.dll", name, args); };
    alignas(8) std::uint8_t wsa[408] = {};
    expect(W("WSAStartup", {0x0202, va(wsa)}) == 0 && wsa[0] == 2 && wsa[1] == 2 && W("socket", {2, 1, 6}) == sh::kInvalidSocket &&
               W("WSAGetLastError", {}) == sh::kWsaeNetDown && W("connect", {5, 0, 16}) == sh::kSocketError &&
               W("WSAGetLastError", {}) == sh::kWsaeNotSock && W("gethostbyname", {va("example.com")}) == 0 &&
               W("WSAGetLastError", {}) == sh::kWsaHostNotFound && W("WSACleanup", {}) == 0 && W("WSACleanup", {}) == sh::kSocketError,
           "net: Winsock starts, but sockets and lookups fail with WSAENETDOWN / WSAENOTSOCK / WSAHOST_NOT_FOUND");
    const std::uint64_t addr = W("inet_addr", {va("192.168.1.20")});
    std::uint32_t pton = 0;
    char ntop[16] = {};
    const std::uint32_t loop = 0x0100007FU;
    expect(addr == 0x1401A8C0U && W("inet_addr", {va("1.2.3")}) == 0xFFFFFFFFU && W("inet_addr", {va("256.1.1.1")}) == 0xFFFFFFFFU &&
               std::strcmp(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(W("inet_ntoa", {addr}))), "192.168.1.20") == 0 &&
               W("inet_pton", {2, va("10.0.0.1"), va(&pton)}) == 1 && pton == 0x0100000AU &&
               W("inet_ntop", {2, va(&loop), va(ntop), sizeof(ntop)}) == va(ntop) && std::strcmp(ntop, "127.0.0.1") == 0 &&
               W("htonl", {0x11223344}) == 0x44332211U,
           "net: inet_addr / inet_ntoa / inet_pton / inet_ntop / htonl are real");
    expect(call(rt, s, "WINHTTP.dll", "WinHttpOpen", {0, 0, 0, 0, 0}) != 0 &&
               call(rt, s, "WINHTTP.dll", "WinHttpSendRequest", {1, 0, 0, 0, 0, 0, 0}) == 0 &&
               call(rt, s, "urlmon.dll", "URLDownloadToFileA", {0, va("http://x"), va("f"), 0, 0}) == sh::kInetEResourceNotFound,
           "net: WinHTTP handles exist but a request fails; URLDownloadToFileA fails");

    // OLEAUT32 BSTRs (#2 SysAllocString, #149 SysStringByteLen, #6 SysFreeString), SHLWAPI, COM, XInput.
    const std::uint64_t bstr = call(rt, s, "OLEAUT32.dll", "#2", {va(u"abc")});
    std::uint16_t c0 = 0;
    std::memcpy(&c0, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(bstr)), 2);
    expect(bstr != 0 && c0 == u'a' && call(rt, s, "OLEAUT32.dll", "#149", {bstr}) == 6 && call(rt, s, "OLEAUT32.dll", "#6", {bstr}) == 0,
           "system: BSTRs carry their byte length and free once");
    char p1[] = "C:\\dir.x\\file.txt";
    char p2[] = "C:\\dir\\name.ext";
    char url[64] = {};
    std::uint32_t n = sizeof(url);
    call(rt, s, "SHLWAPI.dll", "PathRemoveExtensionA", {va(p1)});
    call(rt, s, "SHLWAPI.dll", "PathStripPathA", {va(p2)});
    expect(std::strcmp(p1, "C:\\dir.x\\file") == 0 && std::strcmp(p2, "name.ext") == 0 &&
               call(rt, s, "SHLWAPI.dll", "UrlUnescapeA", {va("a%20b%2Fc"), va(url), va(&n), 0}) == 0 && std::strcmp(url, "a b/c") == 0 &&
               n == 5,
           "system: PathRemoveExtensionA, PathStripPathA, UrlUnescapeA");
    std::uint64_t obj = 1;
    expect(call(rt, s, "ole32.dll", "CoInitializeEx", {0, 2}) == 0 && call(rt, s, "ole32.dll", "CoInitializeEx", {0, 2}) == 1 &&
               call(rt, s, "ole32.dll", "CoCreateInstance", {0, 0, 1, 0, va(&obj)}) == sh::kRegdbEClassNotReg && obj == 0 &&
               call(rt, s, "XINPUT1_3.dll", "#2", {0, va(wsa)}) == sh::kErrorDeviceNotConnected,
           "system: COM starts but creates nothing; no XInput controller");
    expect(faults.empty(), "net/system: no faults");
}

// --- R4 D3DX math (`shim_d3dx.h`; also compared with d3dx9_43.dll on Windows) ---------------------
void test_d3dx_math() {
    namespace dx = wse::recomp::shim::d3dx;
    auto near = [](const float* a, const float* b, int n) {
        for (int i = 0; i < n; ++i) {
            if (std::fabs(a[i] - b[i]) > 1e-5F * std::fmax(1.0F, std::fabs(b[i]))) return false;
        }
        return true;
    };
    const float m[16] = {2, 0, 0, 0, 0, 4, 0, 0, 0, 0, 8, 0, 1, 2, 3, 1};  // scale then translate
    const float want_inv[16] = {0.5F, 0, 0, 0, 0, 0.25F, 0, 0, 0, 0, 0.125F, 0, -0.5F, -0.5F, -0.375F, 1};
    float inv[16];
    float det = 7;
    float id[16];
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    expect(dx::inverse(inv, &det, m) && det == 64.0F && near(inv, want_inv, 16), "d3dx: inverse of scale+translate, determinant");
    dx::multiply(id, m, inv);
    expect(near(id, ident, 16), "d3dx: M * inverse(M) = I (output may alias)");
    const float sing[16] = {1, 2, 3, 4, 2, 4, 6, 8, 0, 1, 0, 0, 0, 0, 1, 0};
    det = 5;
    expect(!dx::inverse(inv, &det, sing) && det == 5, "d3dx: a singular matrix gives NULL and leaves the determinant");
    float t[16];
    dx::transpose(t, m);
    expect(t[3] == 1 && t[7] == 2 && t[11] == 3 && t[12] == 0, "d3dx: transpose");
    float o[16];
    dx::ortho_off_center_rh(o, 0, 800, 600, 0, 0, 1);  // a 2D screen projection
    const float v[3] = {800, 600, 0};
    float out[4];
    dx::transform(out, v, 3, o);
    expect(near(out, std::array<float, 4>{1, -1, 0, 1}.data(), 4), "d3dx: OrthoOffCenterRH maps the screen corner to (1, -1)");
    float p[16];
    dx::perspective_off_center_rh(p, -1, 1, -1, 1, 1, 100);
    const float far_pt[3] = {0, 0, -100};
    dx::transform(out, far_pt, 3, p);
    expect(std::fabs(out[2] / out[3] - 1.0F) < 1e-5F && out[3] == 100.0F, "d3dx: PerspectiveOffCenterRH maps the far plane to z = 1");
    const float vv[4] = {3, 0, 4, 9};
    float n3[3];
    float pl[4];
    dx::normalize(n3, vv, 3, 3);
    dx::normalize(pl, vv, 4, 3);
    expect(near(n3, std::array<float, 3>{0.6F, 0, 0.8F}.data(), 3) && near(pl, std::array<float, 4>{0.6F, 0, 0.8F, 1.8F}.data(), 4),
           "d3dx: Vec3Normalize and PlaneNormalize (by |(a,b,c)|)");
}

// --- R4 recording D3D9 device, D3DX objects, DirectInput (`shim_d3d9.h`) ------------------------
namespace d3d_test {

// Calls method `name` of COM object `obj` as translated code does: the vtable slot through dispatch.
std::uint64_t com(CpuState& s, std::uint64_t obj, const wse::recomp::com::Interface& iface, const char* name,
                  std::initializer_list<std::uint64_t> args) {
    std::size_t index = iface.count;
    for (std::size_t i = 0; i < iface.count; ++i) {
        if (std::strcmp(iface.methods[i], name) == 0) index = i;
    }
    if (index == iface.count) return 0xDEADDEAD;
    std::uint64_t vtbl = 0;
    std::uint64_t fn = 0;
    std::memcpy(&vtbl, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(obj)), 8);
    std::memcpy(&fn, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(vtbl + 8 * index)), 8);
    constexpr std::uint64_t kRet = 0x140031000ULL;
    const std::uint64_t rsp0 = s.gpr[4];
    s.gpr[4] = (rsp0 - 0x100) & ~std::uint64_t{15};
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    std::size_t i = 0;
    for (const std::uint64_t a : std::initializer_list<std::uint64_t>{obj}) s.gpr[kArgReg[i++]] = a;
    for (const std::uint64_t a : args) {
        if (i < 4) {
            s.gpr[kArgReg[i]] = a;
        } else {
            std::memcpy(reinterpret_cast<void*>(s.gpr[4] + 0x20 + 8 * (i - 4)), &a, 8);
        }
        ++i;
    }
    s.gpr[4] -= 8;
    std::memcpy(reinterpret_cast<void*>(s.gpr[4]), &kRet, 8);
    s.gpr[0] = 0xBADBADBAD;
    s.hooks->dispatch(&s, fn);
    s.gpr[4] = rsp0;
    return s.gpr[0] & 0xFFFFFFFFFFFFFFFFULL;
}

}  // namespace d3d_test

void test_d3d9() {
    using d3d_test::com;
    using u32_test::call;
    namespace sh = wse::recomp::shim;
    namespace ci = wse::recomp::com;
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    sh::D3d9 d3d(rt);
    std::vector<std::string> recorded;
    d3d.on_first_call = [&](const std::string& m) { recorded.push_back(m); };
    (void)d3d.install();
    GuestStack stack(256 * 1024);
    CpuState s;
    stack.prepare(s);
    rt.attach(s);
    auto va = [](const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };
    auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };

    const std::uint64_t d9 = call(rt, s, "d3d9.dll", "Direct3DCreate9", {32});
    std::uint32_t mode[4] = {};
    alignas(8) std::uint8_t caps[304] = {};
    expect(d9 != 0 && com(s, d9, ci::kIDirect3D9, "GetAdapterCount", {}) == 1 &&
               com(s, d9, ci::kIDirect3D9, "GetAdapterModeCount", {0, sh::kD3dFmtX8R8G8B8}) == 8 &&
               com(s, d9, ci::kIDirect3D9, "EnumAdapterModes", {0, sh::kD3dFmtX8R8G8B8, 7, va(mode)}) == 0 && mode[0] == 1920 &&
               mode[1] == 1080 && lo(com(s, d9, ci::kIDirect3D9, "EnumAdapterModes", {0, sh::kD3dFmtX8R8G8B8, 8, va(mode)})) == sh::kD3dErrInvalidCall &&
               com(s, d9, ci::kIDirect3D9, "GetDeviceCaps", {0, 1, va(caps)}) == 0,
           "d3d9: Direct3DCreate9, adapter count, display modes, caps");
    std::uint32_t vs_version = 0;
    std::memcpy(&vs_version, caps + 196, 4);
    expect(vs_version == 0xFFFE0300U, "d3d9: caps report vertex shader 3.0 at D3DCAPS9.VertexShaderVersion");
    // D3DPRESENT_PARAMETERS: 800 x 600, auto depth D24S8.
    alignas(8) std::uint8_t pp[64] = {};
    const std::uint32_t w = 800, h = 600, one = 1, d24 = sh::kD3dFmtD24S8;
    std::memcpy(pp, &w, 4);
    std::memcpy(pp + 4, &h, 4);
    std::memcpy(pp + 44, &one, 4);
    std::memcpy(pp + 48, &d24, 4);
    std::uint64_t device = 0;
    expect(com(s, d9, ci::kIDirect3D9, "CreateDevice", {0, 1, 0x10012, 0x40, va(pp), va(&device)}) == 0 && device != 0,
           "d3d9: CreateDevice");
    std::uint64_t bb = 0;
    std::uint32_t desc[8] = {};
    expect(com(s, device, ci::kIDirect3DDevice9, "GetBackBuffer", {0, 0, 0, va(&bb)}) == 0 && bb != 0 &&
               com(s, bb, ci::kIDirect3DSurface9, "GetDesc", {va(desc)}) == 0 && desc[6] == 800 && desc[7] == 600,
           "d3d9: the back buffer is the size the device was created with");
    std::uint64_t tex = 0;
    expect(com(s, device, ci::kIDirect3DDevice9, "CreateTexture", {256, 128, 0, 0, sh::kD3dFmtA8R8G8B8, 1, va(&tex), 0}) == 0 &&
               com(s, tex, ci::kIDirect3DTexture9, "GetLevelCount", {}) == 9 &&
               com(s, tex, ci::kIDirect3DTexture9, "GetLevelDesc", {1, va(desc)}) == 0 && desc[6] == 128 && desc[7] == 64,
           "d3d9: CreateTexture with a full mip chain; level sizes halve");
    alignas(8) std::uint8_t lr[16] = {};
    alignas(8) std::uint8_t lr2[16] = {};
    std::uint64_t surf = 0;
    std::int32_t pitch = 0;
    std::uint64_t bits = 0;
    std::uint64_t bits2 = 0;
    expect(com(s, tex, ci::kIDirect3DTexture9, "LockRect", {0, va(lr), 0, 0}) == 0 &&
               com(s, tex, ci::kIDirect3DTexture9, "GetSurfaceLevel", {0, va(&surf)}) == 0 &&
               com(s, surf, ci::kIDirect3DSurface9, "LockRect", {va(lr2), 0, 0}) == 0,
           "d3d9: a texture level locks, directly and through its surface");
    std::memcpy(&pitch, lr, 4);
    std::memcpy(&bits, lr + 8, 8);
    std::memcpy(&bits2, lr2 + 8, 8);
    expect(pitch == 1024 && bits != 0 && bits == bits2, "d3d9: the lock gives the level's pitch and the same memory both ways");
    std::uint32_t value = 0;
    std::uint64_t vb = 0;
    std::uint64_t vbmem = 0;
    expect(com(s, device, ci::kIDirect3DDevice9, "SetRenderState", {7, 1}) == 0 &&
               com(s, device, ci::kIDirect3DDevice9, "GetRenderState", {7, va(&value)}) == 0 && value == 1 &&
               com(s, device, ci::kIDirect3DDevice9, "CreateVertexBuffer", {1024, 0, 0, 0, va(&vb), 0}) == 0 &&
               com(s, vb, ci::kIDirect3DVertexBuffer9, "Lock", {0, 0, va(&vbmem), 0}) == 0 && vbmem != 0 &&
               com(s, vb, ci::kIDirect3DVertexBuffer9, "Unlock", {}) == 0,
           "d3d9: render state is stored; vertex buffers lock to real memory");
    recorded.clear();
    expect(com(s, device, ci::kIDirect3DDevice9, "BeginScene", {}) == 0 &&
               com(s, device, ci::kIDirect3DDevice9, "DrawPrimitive", {4, 0, 2}) == 0 &&
               com(s, device, ci::kIDirect3DDevice9, "EndScene", {}) == 0 &&
               com(s, device, ci::kIDirect3DDevice9, "Present", {0, 0, 0, 0}) == 0 && d3d.frames() == 1 && recorded.size() == 3 &&
               recorded[1] == "IDirect3DDevice9::DrawPrimitive" && faults.empty(),
           "d3d9: drawing is recorded; Present counts frames");
    std::uint64_t self = 0;
    const unsigned char bogus[16] = {1, 2, 3};
    expect(com(s, device, ci::kIDirect3DDevice9, "QueryInterface", {va(ci::kIDirect3DDevice9.iid), va(&self)}) == 0 && self == device &&
               lo(com(s, device, ci::kIDirect3DDevice9, "QueryInterface", {va(bogus), va(&self)})) == sh::kENoInterface && self == 0,
           "d3d9: QueryInterface answers its own IID and refuses others");
    (void)com(s, device, ci::kIDirect3DDevice9, "GetLight", {0, 0});
    expect(faults.size() == 1 && faults[0].kind == FaultKind::unsupported &&
               faults[0].what.find("IDirect3DDevice9::GetLight") != std::string::npos,
           "d3d9: a method the recording device lacks faults with its name");
    faults.clear();

    // D3DX: an effect, and a texture sized from a DDS header (64 x 32, DXT5, 3 levels).
    std::uint64_t effect = 0;
    std::uint32_t passes = 0;
    expect(call(rt, s, "d3dx9_43.dll", "D3DXCreateEffectFromFileA", {device, va("x.fx"), 0, 0, 0, 0, va(&effect), 0}) == 0 &&
               effect != 0 && com(s, effect, ci::kID3DXEffect, "GetParameterByName", {0, va("world")}) != 0 &&
               com(s, effect, ci::kID3DXEffect, "Begin", {va(&passes), 0}) == 0 && passes == 1 &&
               com(s, effect, ci::kID3DXEffect, "SetMatrix", {1, va(caps)}) == 0,
           "d3dx: effects give parameter handles, one pass, and accept values");
    std::uint8_t dds[128] = {'D', 'D', 'S', ' '};
    const std::uint32_t dh = 32, dw = 64, mips = 3, fourcc = 4, dxt5 = 0x35545844U;
    std::memcpy(dds + 12, &dh, 4);
    std::memcpy(dds + 16, &dw, 4);
    std::memcpy(dds + 28, &mips, 4);
    std::memcpy(dds + 80, &fourcc, 4);
    std::memcpy(dds + 84, &dxt5, 4);
    std::uint32_t info[7] = {};
    std::uint64_t ftex = 0;
    expect(call(rt, s, "d3dx9_43.dll", "D3DXCreateTextureFromFileInMemoryEx",
                {device, va(dds), sizeof(dds), sh::kD3dxDefault, sh::kD3dxDefault, 0xFFFFFFFEU, 0, 0, 1, sh::kD3dxDefault,
                 sh::kD3dxDefault, 0, va(info), 0, va(&ftex)}) == 0 &&
               info[0] == 64 && info[1] == 32 && info[3] == 3 && info[4] == dxt5 &&
               com(s, ftex, ci::kIDirect3DTexture9, "GetLevelCount", {}) == 3 &&
               com(s, ftex, ci::kIDirect3DTexture9, "LockRect", {0, va(lr), 0, 0}) == 0,
           "d3dx: a texture from a DDS file in memory takes its size, format and levels");
    std::memcpy(&pitch, lr, 4);
    expect(pitch == 16 * 16, "d3d9: a DXT5 level's pitch is per 4x4 block row (16 blocks x 16 bytes)");

    // DirectInput 8: a keyboard with nothing pressed.
    std::uint64_t di = 0;
    std::uint64_t kbd = 0;
    std::uint8_t keys[256];
    std::memset(keys, 0xFF, sizeof(keys));
    expect(call(rt, s, "DINPUT8.dll", "DirectInput8Create", {0, 0x800, va(ci::kIDirectInput8A.iid), va(&di), 0}) == 0 && di != 0 &&
               com(s, di, ci::kIDirectInput8A, "CreateDevice", {va(bogus), va(&kbd), 0}) == 0 &&
               com(s, kbd, ci::kIDirectInputDevice8A, "SetCooperativeLevel", {0x10012, 6}) == 0 &&
               com(s, kbd, ci::kIDirectInputDevice8A, "Acquire", {}) == 0 &&
               com(s, kbd, ci::kIDirectInputDevice8A, "GetDeviceState", {256, va(keys)}) == 0 && keys[0] == 0 && keys[255] == 0 &&
               lo(call(rt, s, "DINPUT8.dll", "DirectInput8Create", {0, 0x800, va(bogus), va(&di), 0})) == sh::kENoInterface,
           "dinput: DirectInput8Create (A), a device with nothing pressed; an unknown IID is refused");
    expect(faults.empty(), "d3d9: no faults");
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
    test_threads_and_sync();
    test_threadpool();
    test_time_crt_sysinfo();
    test_strings_memory();
    test_files();
    test_modules();
    test_advapi();
    test_user32();
    test_net_system();
    test_d3dx_math();
    test_d3d9();
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
