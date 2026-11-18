// Host self-tests for the Phase 5 recompiler core (R0, `docs/RECOMP_PLAN.md`): the x86-64
// decoder subset, the reference interpreter's flag rules, function decoding, the PE reader
// and the R0 (d) test vectors. Synthetic code only: every byte sequence below is written for
// this test (no retail code; that runs only in the local `tools/r0_pc_reference.cpp`).
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
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

void test_pe_reader() {
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

}  // namespace

namespace wse::selftest {

int run_recomp_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_decoder_operands();
    test_interpreter_flags();
    test_function_decode_and_run();
    test_pe_reader();
    test_r0_vectors();
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
