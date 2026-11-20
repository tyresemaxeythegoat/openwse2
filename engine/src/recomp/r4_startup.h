// The R4 startup runner (`docs/RECOMP_PLAN.md` "R4 design", "Phone run"): translates the user's
// `mb_warband_wse2_x64.exe` and `lua5164.dll` in cached batches, maps and links them, binds every
// shim, and runs the TLS callbacks, each DLL's DllMain and the exe's entry point on the recording
// D3D9 device. Shared by `tools/r4_run.cpp` (Linux x86-64, WSL) and the phone's Advanced >
// "R4 startup run" (`platform/android/r4_jni.cpp`, its own `:recomp` process).
//
// One run per process: the images are mapped at their fixed preferred bases, guest threads cannot
// be stopped, and the run ends the process (see `run_startup`). Retail code runs only on the
// user's machine; nothing here is run in CI.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wse::platform {
class Vfs;
}

namespace wse::recomp {

struct StartupOptions {
    std::vector<std::uint8_t> exe;  // mb_warband_wse2_x64.exe
    std::vector<std::uint8_t> lua;  // lua5164.dll
    std::vector<std::uint8_t> pdb;  // mb_warband_wse2_x64.pdb (must match the exe)
    wse::platform::Vfs* install = nullptr;  // the install (read-only), the game's working directory
    wse::platform::Vfs* user = nullptr;     // the writable layer (registry, profile, Documents)
    std::string so_dir;                     // translated batches; created if missing
    // Part of every batch's cache key: identifies the translator build (a hash of the translator
    // library on the phone; its size and time on the PC). A different id retranslates.
    std::string translator_id;
    std::string module = "Native";  // seeded as last_module_warband, and passed as --module
    unsigned jobs = 2;               // translation worker threads
    int opt = 2;                     // LLVM optimization level
    std::size_t batch_kib = 64;      // KiB of .text per translated batch
    bool retranslate = false;        // ignore every cached batch
    bool reuse = false;              // keep cached batches whatever their key (never their functions)
    bool quiet = false;              // no per-import / per-method first-call lines
    std::uint64_t frame_limit = 0;   // end the run after this many frames (0: until the exe exits)
    unsigned heartbeat_s = 20;       // a status line this often while the exe runs
    // Every line of the report (text may hold several lines). Default: stdout.
    std::function<void(const std::string& text)> log;
    // Translation progress: batches done of the batches to translate, per image ("exe", "lua5164").
    std::function<void(const std::string& image, std::size_t done, std::size_t total)> progress;
};

// Exit codes of the process (`run_startup` ends it with `_Exit` once the exe has started).
inline constexpr int kStartupFrameLimit = 0;  // `frame_limit` frames presented: the run passed
inline constexpr int kStartupSetupError = 1;  // returned, not exited: nothing was started
inline constexpr int kStartupFault = 2;       // a runtime fault (missing import, untranslated code, ...)
inline constexpr int kStartupHostSignal = 3;  // SIGSEGV / SIGBUS / SIGILL / SIGFPE in translated code or a shim
inline constexpr int kStartupExitProcess = 4; // the exe called ExitProcess (its code is in the report)
inline constexpr int kStartupReturned = 5;    // the entry point returned

// Runs the startup. Returns `kStartupSetupError` (after logging why) when nothing could be
// started; otherwise it never returns and ends the process with one of the codes above, the last
// report line saying which.
int run_startup(const StartupOptions& options);

}  // namespace wse::recomp
