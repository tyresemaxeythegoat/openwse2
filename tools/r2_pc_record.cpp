// R2 PC recorder (Windows x64, local only: it executes the user's retail code natively).
// `docs/RECOMP_PLAN.md` "R2 design".
//
//   zig c++ -std=c++20 -O2 -target x86_64-windows-gnu -I engine/include -I <zydis dir>
//       -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -o r2_pc_record.exe
//       tools/r2_pc_record.cpp engine/src/recomp/discover.cpp zydis.o
//   r2_pc_record.exe <mb_warband_wse2_x64.exe> <mb_warband_wse2_x64.pdb> <out.inc> [work dir]
//
// Maps the exe at its base, selects the self-contained functions (`is_self_contained`, the rule
// the phone uses), calls each natively on the `r2_rig.h` cases through an assembly thunk that
// loads every argument register, `xmm0`-`xmm3` and the stack arguments, and writes one result
// hash per function to `out.inc` (`engine/src/recomp/r2_reference_v1156.inc`). A fault is a
// result (a vectored exception handler resumes the thunk). A function running over 2 s is
// recorded as skipped and the worker process is restarted, resuming where it stopped.
// Only hashes leave this tool: no code or bytes from the exe.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../engine/src/recomp/discover.h"
#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r2_rig.h"

using namespace wse::recomp;

// --- native call thunk ---------------------------------------------------------------------
struct NativeIn {
    std::uint64_t fn;    // 0
    std::uint64_t rsp;   // 8: guest rsp + 8 (16-aligned); `call` pushes the return VA below it
    std::uint64_t gpr[4];  // 16: rcx rdx r8 r9
    std::uint8_t xmm[4][16];  // 48
};
struct NativeOut {
    std::uint64_t rax;    // 0
    std::uint64_t xmm0;   // 8
    std::uint64_t fault;  // 16
};
static_assert(offsetof(NativeIn, gpr) == 16 && offsetof(NativeIn, xmm) == 48);

extern "C" {
std::uint64_t g_r2_saved_rsp = 0;
NativeOut* g_r2_out = nullptr;
void r2_call_native(const NativeIn* in, NativeOut* out);
void r2_fault_resume();
void r2_after_call();
}

// Saves the Win64 callee-saved state (GPRs, xmm6-15, MXCSR), switches to the guest stack, loads
// the arguments and calls; `r2_fault_resume` is where the exception handler sends a fault.
__asm__(R"(
    .intel_syntax noprefix
    .text
    .globl r2_call_native
r2_call_native:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0xB8
    movdqu [rsp + 0x00], xmm6
    movdqu [rsp + 0x10], xmm7
    movdqu [rsp + 0x20], xmm8
    movdqu [rsp + 0x30], xmm9
    movdqu [rsp + 0x40], xmm10
    movdqu [rsp + 0x50], xmm11
    movdqu [rsp + 0x60], xmm12
    movdqu [rsp + 0x70], xmm13
    movdqu [rsp + 0x80], xmm14
    movdqu [rsp + 0x90], xmm15
    stmxcsr [rsp + 0xA0]
    mov [rip + g_r2_out], rdx
    mov [rip + g_r2_saved_rsp], rsp
    mov dword ptr [rsp + 0xA4], 0x1F80
    ldmxcsr [rsp + 0xA4]
    mov rax, [rcx + 0]
    mov rsp, [rcx + 8]
    movdqu xmm0, [rcx + 48]
    movdqu xmm1, [rcx + 64]
    movdqu xmm2, [rcx + 80]
    movdqu xmm3, [rcx + 96]
    mov rdx, [rcx + 24]
    mov r8, [rcx + 32]
    mov r9, [rcx + 40]
    mov rcx, [rcx + 16]
    cld
    mov r11, 0x230000000
    push r11
    jmp rax
    .globl r2_after_call
r2_after_call:
    mov rcx, [rip + g_r2_out]
    mov [rcx + 0], rax
    movq [rcx + 8], xmm0
    mov qword ptr [rcx + 16], 0
    jmp r2_epilogue
    .globl r2_fault_resume
r2_fault_resume:
    cld
    mov rcx, [rip + g_r2_out]
    mov qword ptr [rcx + 16], 1
r2_epilogue:
    mov rsp, [rip + g_r2_saved_rsp]
    ldmxcsr [rsp + 0xA0]
    movdqu xmm6, [rsp + 0x00]
    movdqu xmm7, [rsp + 0x10]
    movdqu xmm8, [rsp + 0x20]
    movdqu xmm9, [rsp + 0x30]
    movdqu xmm10, [rsp + 0x40]
    movdqu xmm11, [rsp + 0x50]
    movdqu xmm12, [rsp + 0x60]
    movdqu xmm13, [rsp + 0x70]
    movdqu xmm14, [rsp + 0x80]
    movdqu xmm15, [rsp + 0x90]
    add rsp, 0xB8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
    .att_syntax prefix
)");

namespace {

std::atomic<bool> g_in_guest{false};
std::atomic<std::uint64_t> g_call_start_ms{0};
std::atomic<std::uint64_t> g_current_va{0};

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    if (!g_in_guest.load()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_STACK_CHECK:
        e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&r2_fault_resume);
        e->ContextRecord->Rsp = g_r2_saved_rsp;
        return EXCEPTION_CONTINUE_EXECUTION;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

std::uint64_t now_ms() { return GetTickCount64(); }

std::vector<std::uint8_t> slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// "<va hex> <hash hex>" per line, or "<va hex> skip".
std::map<std::uint64_t, std::string> read_progress(const std::string& path) {
    // A worker stopped by the watchdog may leave a partial last line: skip malformed lines.
    std::map<std::uint64_t, std::string> m;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        unsigned long long va = 0;
        char v[32] = {};
        if (std::sscanf(line.c_str(), "%llx %31s", &va, v) != 2) {
            continue;
        }
        const std::string value = v;
        if (value == "skip" ||
            (value.size() == 16 && value.find_first_not_of("0123456789abcdef") == std::string::npos)) {
            m[va] = value;  // a later skip wins over an earlier partial result
        }
    }
    return m;
}

int run_worker(const std::string& exe_path, const std::string& pdb_path, const std::string& progress_path,
               std::vector<std::uint64_t>* selected_out) {
    const auto file = slurp(exe_path);
    const auto pdb = slurp(pdb_path);
    PeImage img;
    std::string err;
    PeCodeView cv;
    std::vector<PdbPublic> publics;
    PdbGuidAge ga;
    if (!parse_pe(file.data(), file.size(), img, &err) || !read_pe_codeview(file.data(), file.size(), img, cv, &err) ||
        !read_pdb_publics(pdb.data(), pdb.size(), publics, &ga, &err) || ga.guid != cv.guid) {
        std::fprintf(stderr, "exe/PDB: %s\n", err.empty() ? "GUID does not match" : err.c_str());
        return 2;
    }
    auto* base = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(img.image_base), img.size_of_image,
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (base != reinterpret_cast<std::uint8_t*>(img.image_base) || !map_pe_sections(file.data(), file.size(), img, base)) {
        std::fprintf(stderr, "could not map the exe at its base\n");
        return 2;
    }
    // Writable sections, to detect and undo writes into the image.
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> pristine;
    for (const PeSection& s : img.sections) {
        if ((s.characteristics & 0x80000000U) != 0) {
            pristine.emplace_back(s.virtual_address,
                                  std::vector<std::uint8_t>(base + s.virtual_address, base + s.virtual_address + s.virtual_size));
        }
    }

    const auto [text_b, text_e] = text_range(img);
    ImageView view(base, img.image_base, img.size_of_image, text_b, text_e);
    const std::set<std::uint64_t> entries = collect_function_entries(view, img, base, publics);
    Discoverer disc(view);
    std::vector<std::uint64_t> selected;
    for (const std::uint64_t e : entries) {
        if (is_self_contained(disc.discover(e, entries))) {
            selected.push_back(e);
        }
    }
    if (selected_out != nullptr) {
        *selected_out = selected;
        return 0;
    }
    std::map<std::uint64_t, std::string> done = read_progress(progress_path);
    std::printf("worker: %zu entries, %zu self-contained, %zu already done\n", entries.size(), selected.size(), done.size());

    // Buffers between no-access guard regions and the guest stack, at the fixed addresses the
    // phone uses too (`r2_rig.h`).
    auto* region = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(r2::kRegionBase), r2::kRegionBytes,
                                                           MEM_RESERVE, PAGE_NOACCESS));
    auto* stack = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(r2::kStackBase), r2::kStackBytes,
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (region != reinterpret_cast<std::uint8_t*>(r2::kRegionBase) ||
        stack != reinterpret_cast<std::uint8_t*>(r2::kStackBase)) {
        std::fprintf(stderr, "could not reserve the fixed R2 buffer/stack addresses\n");
        return 2;
    }
    std::uint8_t* buffers[r2::kBuffers];
    const r2::Regions regions = r2::fixed_regions();
    for (int b = 0; b < r2::kBuffers; ++b) {
        buffers[b] = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(r2::buffer_address(b)),
                                                             r2::kBufferBytes, MEM_COMMIT, PAGE_READWRITE));
    }
    const std::uint64_t guest_rsp = r2::kGuestRsp;
    std::vector<std::uint8_t> pristine_buf[r2::kBuffers];
    const std::uint8_t* pristine_ptr[r2::kBuffers];
    for (int b = 0; b < r2::kBuffers; ++b) {
        pristine_buf[b].resize(r2::kBufferBytes);
        pristine_ptr[b] = pristine_buf[b].data();
    }
    // The fixed return address: `jmp [rip + 0]` to `r2_after_call` (no register changes).
    auto* ret_stub = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(r2::kReturnVa), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (ret_stub != reinterpret_cast<std::uint8_t*>(r2::kReturnVa)) {
        std::fprintf(stderr, "could not map the return stub\n");
        return 2;
    }
    const std::uint8_t jmp_rip0[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto after = reinterpret_cast<std::uint64_t>(&r2_after_call);
    std::memcpy(ret_stub, jmp_rip0, 6);
    std::memcpy(ret_stub + 6, &after, 8);

    AddVectoredExceptionHandler(1, &on_exception);
    std::thread watchdog([&] {
        for (;;) {
            Sleep(100);
            if (g_in_guest.load() && now_ms() - g_call_start_ms.load() > 2000) {
                std::FILE* f = std::fopen(progress_path.c_str(), "a");
                std::fprintf(f, "\n%llx skip\n",static_cast<unsigned long long>(g_current_va.load()));
                std::fclose(f);
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    });
    watchdog.detach();

    std::FILE* progress = std::fopen(progress_path.c_str(), "a");
    std::size_t n = 0;
    for (const std::uint64_t va : selected) {
        if (done.count(va) != 0) {
            continue;
        }
        r2::Hasher h;
        r2::fill_buffers(va, buffers, regions);
        for (int b = 0; b < r2::kBuffers; ++b) {
            std::memcpy(pristine_buf[b].data(), buffers[b], r2::kBufferBytes);
        }
        for (int k = 0; k < r2::kCases; ++k) {
            const r2::CaseInputs c = r2::make_case(va, k);
            std::memset(reinterpret_cast<void*>(r2::kStackClearLo), 0, r2::kStackClearHi - r2::kStackClearLo);
            for (int i = 0; i < r2::kStackArgs; ++i) {
                const std::uint64_t v = r2::materialize(c.stack[i], regions);
                std::memcpy(reinterpret_cast<void*>(guest_rsp + 0x28 + 8 * i), &v, 8);
            }
            NativeIn in{};
            in.fn = va;
            in.rsp = guest_rsp + 8;
            for (int i = 0; i < 4; ++i) {
                in.gpr[i] = r2::materialize(c.gpr[i], regions);
            }
            std::memcpy(in.xmm, c.xmm, sizeof(in.xmm));
            NativeOut out{};
            g_current_va = va;
            g_call_start_ms = now_ms();
            g_in_guest = true;
            r2_call_native(&in, &out);
            g_in_guest = false;
            const std::uint8_t* const cb[r2::kBuffers] = {buffers[0], buffers[1], buffers[2], buffers[3]};
            r2::hash_case(h, out.fault != 0, out.rax, out.xmm0, cb, pristine_ptr, regions);
            r2::restore_buffers(buffers, pristine_ptr);
        }
        // Writes into the image: part of the result, then undone.
        for (auto& [rva, bytes] : pristine) {
            if (std::memcmp(base + rva, bytes.data(), bytes.size()) != 0) {
                h.add(0x1A6E000ULL + rva);
                std::memcpy(base + rva, bytes.data(), bytes.size());
            }
        }
        std::fprintf(progress, "%llx %016llx\n", static_cast<unsigned long long>(va), static_cast<unsigned long long>(h.h));
        std::fflush(progress);  // the watchdog may stop this process at any time
        if (++n % 200 == 0) {
            std::printf("  %zu functions\n", n);
        }
    }
    std::fclose(progress);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: r2_pc_record <exe> <pdb> <out.inc> [work dir]\n");
        return 2;
    }
    const std::string work = argc > 4 ? argv[4] : ".";
    const std::string progress = work + "/r2_progress.txt";
    if (argc > 5 && std::string(argv[5]) == "--worker") {
        return run_worker(argv[1], argv[2], progress, nullptr);
    }
    // Supervisor: run workers until one finishes without being stopped by the watchdog.
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::string cmd = "\"" + std::string(argv[0]) + "\" \"" + argv[1] + "\" \"" + argv[2] + "\" \"" + argv[3] +
                          "\" \"" + work + "\" --worker";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "CreateProcess failed\n");
            return 2;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0) {
            break;
        }
        if (code != 3) {
            std::fprintf(stderr, "worker exited with %lu\n", code);
            return 2;
        }
        std::printf("worker restarted after a timeout\n");
    }
    std::vector<std::uint64_t> selected;
    if (run_worker(argv[1], argv[2], progress, &selected) != 0) {
        return 2;
    }
    const auto done = read_progress(progress);
    std::size_t recorded = 0;
    std::size_t skipped = 0;
    std::string rows;
    for (const std::uint64_t va : selected) {
        const auto it = done.find(va);
        if (it == done.end() || it->second == "skip") {
            ++skipped;
            continue;
        }
        ++recorded;
        char line[80];
        std::snprintf(line, sizeof(line), "    {0x%llxULL, 0x%sULL},\n", static_cast<unsigned long long>(va),
                      it->second.c_str());
        rows += line;
    }
    std::FILE* out = std::fopen(argv[3], "w");
    char date[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&t));
    std::fprintf(out,
                 "// Generated by tools/r2_pc_record.cpp on %s from the user's WSE2 v1.1.5.6 exe (native run on the PC).\n"
                 "// One result hash per self-contained function (`is_self_contained`, `wse/recomp/r2_rig.h`);\n"
                 "// no code or bytes from the exe. Do not edit: rerun the tool.\n"
                 "// %zu functions selected, %zu recorded, %zu skipped (ran over 2 s).\n"
                 "inline constexpr std::size_t kR2V1156Selected = %zu;\n"
                 "inline constexpr R2Reference kR2V1156Reference[] = {\n%s};\n",
                 date, selected.size(), recorded, skipped, selected.size(), rows.c_str());
    std::fclose(out);
    std::printf("selected %zu, recorded %zu, skipped %zu -> %s\n", selected.size(), recorded, skipped, argv[3]);
    return 0;
}
