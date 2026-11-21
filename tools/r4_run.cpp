// R4 bring-up run (`docs/RECOMP_PLAN.md` "R4 design", "Bring-up tool"). LOCAL ONLY: it reads the
// user's own install and runs retail code translated on this machine; never run in CI, and
// nothing it reads or writes is committed. Linux x86-64 (WSL, `tools/wsl_gate.sh` builds it).
//
//   r4_run <install dir> [--work <dir>] [--retranslate] [--batch <n>] [--opt <0-3>] [--jobs <n>]
//          [--reuse] [--quiet] [--module <name>] [--frames <n>] [--for-phone <dir>]
//
// The runner itself is `engine/src/recomp/r4_startup.{h,cpp}` (shared with the phone's Advanced >
// "R4 startup run"); this tool reads the exe, `lua5164.dll` and the PDB from the install, mounts the
// install and `<work>/user` as host directories, and keeps the translated batches in `<work>/so`,
// each reused while the image, the options and the translator library are unchanged. `--reuse`
// keeps every batch already built whatever its key, for when only a non-lifter file of the
// translator library changed. `--frames <n>` ends the run after n frames (exit code 0). A fault (a
// missing import names its DLL and function: the next thing to implement) ends the run with the
// fault and the last imports called.
//
// `--for-phone <dir>` (2026-11-21): translate for the phone instead (the portable AArch64 Android
// target the app also uses, `portable_android_target`) into `<dir>`, run nothing, and exit 0. The
// batches are the user's own exe translated on their own PC for their own phone: the app imports
// them ("Import prepared game files") instead of translating for hours. Wrapped for players by
// `tools/prepare_for_phone.ps1`.

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../engine/src/recomp/r4_startup.h"
#include "../engine/src/recomp/recomp_llvm.h"
#include "wse/platform/vfs.h"

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A file in `dir` whose name matches `name` case-insensitively (Windows installs mix cases).
fs::path find_ci(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string a = e.path().filename().string();
        std::string b = name;
        for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (a == b) return e.path();
    }
    return dir / name;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <install dir> [--work <dir>] [--retranslate] [--batch <n>] [--opt <0-3>] [--jobs <n>] "
                     "[--reuse] [--quiet] [--module <name>] [--frames <n>] [--for-phone <dir>]\n",
                     argv[0]);
        return 2;
    }
    const fs::path install = argv[1];
    fs::path work = "/root/wse/r4";
    wse::recomp::StartupOptions o;
    fs::path for_phone;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--work" && i + 1 < argc) work = argv[++i];
        else if (a == "--retranslate") o.retranslate = true;
        else if (a == "--batch" && i + 1 < argc) o.batch_kib = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--opt" && i + 1 < argc) o.opt = std::atoi(argv[++i]);
        else if (a == "--jobs" && i + 1 < argc) o.jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--reuse") o.reuse = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--module" && i + 1 < argc) o.module = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.frame_limit = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--for-phone" && i + 1 < argc) for_phone = argv[++i];
    }
    std::error_code ec;
    fs::create_directories(work / "user", ec);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    o.exe = read_file(find_ci(install, "mb_warband_wse2_x64.exe"));
    o.lua = read_file(find_ci(install, "lua5164.dll"));
    o.pdb = read_file(find_ci(install, "mb_warband_wse2_x64.pdb"));
    o.so_dir = (work / "so").string();
    if (!for_phone.empty()) {
        wse::recomp::set_code_target(wse::recomp::portable_android_target());
        o.translate_only = true;
        o.so_dir = for_phone.string();
    }
    static wse::platform::HostVfs install_vfs(install);
    static wse::platform::HostVfs user_vfs(work / "user");
    o.install = &install_vfs;
    o.user = &user_vfs;
    return wse::recomp::run_startup(o);
}
