# Agent & Contributor Guide (`wse2-android`)

This repository builds a clean-room, evidence-backed C++20 reconstruction of the WSE2 (Warband Script Enhancer 2) engine targeting native Android `arm64-v8a`.

Start here, then read `STATUS.md` (where things stand) and `ROADMAP.md` (what comes next).

## 1. Evidence & Clean-Room Rules

- **Baseline version:** Official **WSE2 v1.1.5.6** (`mb_warband_wse2_x64.exe`, PDB + PE + Ghidra 12.1.4 REA analysis).
- **Superseded baseline:** The `2026-10-14` x64 build is superseded. Do not cite its addresses or TPI type IDs in new code or active documentation outside explicitly labeled historical/superseded sections.
- **Superseded `v1.1.5.5` build:** Rebased to v1.1.5.6 on 2026-11-19 with `tools/rebase_build.py`; do not cite un-rebased v1.1.5.5 addresses in new code or active documentation outside historical records (curated class TPI type indices and sizes are identical in both builds).
- **Evidence citation:** Every reconstructed struct, field offset, function, or opcode handler must cite concrete v1.1.5.6 evidence (function VA, PDB TPI type index/size, and/or REA `ev_*` evidence hash).
- **Documented safety deviations:** Bounds checks, null checks, loop guards (e.g., `str_split` empty delimiter), and error callbacks may replace retail unchecked indexing/crashing paths, provided valid module behavior is preserved and deviations are documented.
- **BYOA only:** Never bundle TaleWorlds/Warband assets, retail executables, PDBs, DRM bypasses, or emulators/compatibility layers (no Wine, Box64, or Dynarmic). Android builds load user-supplied Warband installs and `Modules/`.
- **Recompilation (Phase 5, `docs/RECOMP_PLAN.md`, decided 2026-11-18):** the app translates the user's own WSE2 x64 exe (any build whose shipped PDB matches it; v1.1.5.6 is the verified one) and its shipped x64 DLLs (`lua5164`, `fmodex64`) to ARM64 ahead of time on the device with embedded LLVM. Native shims for the system DLLs the exe imports are allowed. Still banned: Wine, Box64, Dynarmic, QEMU, translating code during play (JIT), and committing or bundling retail binaries, PDBs, or anything lifted from them. Tests on retail code run only locally; CI uses synthetic test binaries.

## 2. Build & Verification Gates

- **Host C++20 gate (required on every engine edit):**
  - Windows:
    ```powershell
    powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
    ```
    Compiles and runs all host test suites (`operand`, `trigger`, `savegame`, `script`, `wse_bitwise_fp`, `wse_system`, `wse_string`, `wse_dict`, `wse_array`, `wse_world`, `block_vm`, `native_world`, `native_m5`, `wse_m6`, `platform`, `module_loaders`, `headless`, `lua`, `asset`, `audio`, `input`, `scene`, `ui`, `recomp`) plus the `lua51`, `wse_lua`, and `wse_audio` static libraries, and builds `wse_headless` and `wse_brf` via Zig `clang++ -std=c++20 -O2 -Wall -Wextra -Werror` and must finish with `engine host tests: PASS`.
  - Linux / cloud sessions (no PowerShell or Zig needed; verified with Ubuntu `clang++` 18.1.3). Run from the repository root and build outside the tree. The `wse_dict`, `wse_array`, `platform`, `module_loaders`, and `headless` suites write fixed-name scratch files under the system temp directory, so do not run two copies of the gate at once:
    ```sh
    out="$(mktemp -d)"; root="$PWD"
    for t in operand trigger savegame script wse_bitwise_fp wse_system wse_string wse_dict wse_array wse_world block_vm native_world native_m5 wse_m6 module_loaders headless input scene ui recomp; do
      clang++ -std=c++20 -O2 -Wall -Wextra -Werror -I engine/include \
        -o "$out/${t}_test" "engine/tests/${t}_test.cpp" || exit 1
    done
    cd "$out" && for t in operand trigger savegame wse_bitwise_fp wse_system wse_string wse_dict wse_array wse_world block_vm native_world native_m5 wse_m6 module_loaders headless input scene ui recomp; do ./${t}_test || exit 1; done \
      && ./script_test "$root/engine/fixtures/scripts.txt" && echo "engine host tests: PASS"
    ```
    Every suite must print `<name> tests: PASS`. The `platform` suite touches the real host Vulkan loader (and any GPU driver it finds) only when `WSE_TEST_SYSTEM_VULKAN=1` is set; otherwise it uses the in-process mock ICD. `wse_string`, `wse_dict`, `wse_array`, and `block_vm` take ~5–15 s each to compile.

    The loop above covers the header-only suites (each `*_test.cpp` keeps its own `main()` for it). The `lua`, `audio`, `asset`, and `platform` suites (and `wse_headless` / `wse_brf`) build through CMake (`lua_test` / `wse_headless` link `lua51` + `wse_lua`; `audio_test` links `wse_audio`; `asset_test`, `platform_test`, and `wse_brf` link `wse_shader` + `wse_glslang`, and `platform_test` also `wse_vulkan`) — always run them; they are required on every edit under `engine/src/`, the Lua hooks in `block_vm.h` / `lua_bridge.h`, the audio headers (`wse/asset/audio*.h`, `wse/platform/audio.h`, `wse/platform/audio_worker.h`, `wse/game/{audio_runtime,world_audio}.h`, and the sound opcodes), or `wse/asset/shader*.h`:
    ```sh
    cmake -S . -B "$out/cmake" -DCMAKE_CXX_COMPILER=clang++ && cmake --build "$out/cmake" -j4 --target lua_test audio_test asset_test platform_test wse_headless wse_brf \
      && ctest --test-dir "$out/cmake" -R '^(lua|audio|asset|platform)$' --output-on-failure
    ```
    `WSE_DUMP_SPIRV_DIR=<dir> ./asset_test` writes every SPIR-V module the suite compiles; CI validates them with `spirv-val --target-env vulkan1.0`.
    The CMake build compiles each suite once into the `wse_selftest` static library; every `<name>_test` executable is `engine/tests/suite_main.cpp` linked against it, and `selftest_host_test` (CTest `selftest_host`) runs all 24 in one process, as the Android self-test APK does. A new suite needs a `run_<name>_suite` entry in `engine/tests/wse_selftest.{h,cpp}` and in both `CMakeLists.txt` lists.

    CMake downloads `lua-5.1.5.tar.gz` from `www.lua.org` (SHA-256 pinned). Where that host is blocked, unpack the same tarball (the Ubuntu archive's `lua5.1_5.1.5.orig.tar.gz` is byte-identical) and pass `-DWSE_LUA_SOURCE_DIR=<dir containing src/lua.h>`. Lua is compiled **as C++** so Lua errors unwind as exceptions; never wrap `<lua.h>` in `extern "C"`.

    CMake also fetches `stb_vorbis.c` and `dr_mp3.h` (pinned commits) from `raw.githubusercontent.com` into the build directory, and re-checks their SHA-256 on every configure. Offline, put both files in one directory and pass `-DWSE_AUDIO_THIRD_PARTY_DIR=<dir>` (a mismatch there is fatal); `engine/build/third_party/audio/` (filled by `build_host.ps1`) is picked up automatically.

    CMake also downloads the Khronos `glslang` `16.6.0` source archive from `github.com` (SHA-256 pinned) and builds it as `wse_glslang`. Offline, pass `-DWSE_GLSLANG_SOURCE_DIR=<dir containing glslang/Public/ShaderLang.h>` (a checkout of tag `16.6.0` works); `engine/build/third_party/glslang-16.6.0/` (filled by `build_host.ps1`) is picked up automatically. `wse_shader` uses glslang's internal `glslang/Include/Types.h`, so a distro `glslang-dev` package cannot stand in for the source tree.
- **Sanitizer gate (CI job `host-sanitizers`, required when touching VM control flow, loaders, or object lifetimes):**
  ```sh
  cmake -S . -B build-san -DCMAKE_CXX_COMPILER=clang++ -DWSE_SANITIZE=ON && cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
  ```
  Builds every suite with AddressSanitizer + UndefinedBehaviorSanitizer. The unmodified third-party `lua51` target alone is built with `-fno-sanitize=float-cast-overflow` (stock `lua_number2int` casts any number key to `int`); do not widen that exclusion or apply it to first-party targets. If the local clang has no sanitizer runtime, `g++` works with `-DCMAKE_CXX_FLAGS=-Wno-maybe-uninitialized` (libstdc++ `<regex>` trips that warning under `-O2`).
- **Recompiler gate (`WSE_ENABLE_LLVM`, Linux/CI):**
  ```sh
  CXX=clang++-18 cmake -S . -B build-recomp -DWSE_ENABLE_LLVM=ON \
    -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DLLD_DIR=/usr/lib/llvm-18/lib/cmake/lld \
    -DWSE_REQUIRE_RECOMP_CORPUS=ON
  cmake --build build-recomp -j"$(nproc)" --target recomp_llvm_test recomp_test
  ./build-recomp/recomp_test
  ./build-recomp/recomp_llvm_test
  ```
  Requires `llvm-18-dev`, `liblld-18-dev`, `clang-18` and `lld-18` (plus their transitive dev packages; CI job `recomp-host`, synthetic code only). CMake downloads Zydis 4.1.0 (SHA-256 pinned; offline: `-DWSE_ZYDIS_SOURCE_DIR=<dir with Zydis.c>`). `clang` and `lld-link` build the R1 differential corpus (`engine/tests/recomp_corpus/*_corpus.c`) into a Win64 PE; `recomp_llvm_test` must print `differential: N/N exports match`. A corpus function reached only through a pointer must be exported (discovery follows direct calls only). Translated modules link with `-z defs`: an LLVM lowering that needs libm, libatomic or compiler-rt fails the link and must be kept inline or moved behind a `RuntimeHooks` entry.
- **Android NDK gate (CI-verified for every pushed commit on any branch):**
  ```sh
  aarch64-linux-android29-clang++ -std=c++20 -O2 -Wall -Wextra -Werror ...
  ```
  Runs automatically in `.github/workflows/ci.yml` (`android-arm64`) against the Ubuntu runner's NDK, compiling every header under `engine/include/` standalone and cross-compiling the CMake tree (see `STATUS.md` §2). Job `android-apk` then builds the P1 self-test APK (`gradle -p android assembleDebug`, NDK and CMake versions pinned in the workflow, SDL3 AAR SHA-256 pinned in `android/app/build.gradle.kts`) and fails if the APK contains any asset other than `assets/scripts.txt`. Its native code is compiled at `-O2 -g` even though Gradle passes `CMAKE_BUILD_TYPE=Debug` (`WSE_ANDROID_OPTIMIZE_DEBUG`, default on); never measure device performance with that option off. On-device Android runtime tests remain blocked without hardware/`adb`.
- **Regenerating artifacts:**
  - `docs/SYMBOL_MAP.json`: regenerated by `tools/build_symbol_map.py` (uses `vtables_x64.json` emitted by `tools/vtables.py`).
  - `docs/wse_opcodes.json`, `docs/WSE_OPCODES.md`, `engine/include/wse/vm/wse_opcodes.h`: generated by `tools/extract_wse_opcodes.py`. Never hand-edit these three files.
  - Inline `0x14…` citations in `engine/include` and in the six living docs: checked in CI by `tools/check_citations.py --self-test --docs`. A doc VA that `SYMBOL_MAP.json` does not curate fails unless it is in `tools/docs_citation_baseline.txt` (pre-existing image-base/`.data`/historical VAs; regenerate with `--write-docs-baseline` only after checking each new entry) or its line carries `<!-- citation-check: ignore-line -->` (quoting a corrected citation). The checker accepts any VA inside a known function, so cite function entry VAs and check the name matches.
  - **A new WSE2 release:** `python tools/rebase_build.py --old-exe … --old-pdb … --new-exe … --new-pdb … --release vX --work <dir outside repo>` (local only; needs `pefile` + `capstone`). Run it once without `--apply` and read `<work>/report.md` + `<work>/packets/`; resolve every unresolved VA by review and record it with its evidence in `tools/rebase_overrides/<old>-to-<new>.json`; then run once with `--apply --overrides …` (never twice: a second pass would move the already-moved addresses), then `tools/extract_wse_opcodes.py --rebase-map <work>/va_map.json --release vX`, re-emit the native opcode map from the new exe, and `tools/check_citations.py --self-test --docs` (regenerate the docs baseline only after checking each entry). Then update the prose that names the build, keeping dated history as it was.
  - Retail corpus checks in `asset_test` run when `WSE_WARBAND_ROOT` (default `C:/GOG Games/Mount and Blade - Warband`) holds an install.

## 3. Repository Hygiene & Git Rules

- **Never commit:**
  - Retail `.exe`, `.pdb`, or game asset files.
  - PanVK `.so` binaries or local NDK/SDK artifacts.
  - Uncommitted PanVK work in `runtime/android/` unless explicitly requested by the user.
  - Tokens, credentials, or scratch dumps outside `repo/`.
- Do not merge a pull request while any `Engine CI` job is failing; fix the job or the code first.
- Always inspect `git diff --cached --name-only` and `git status --short` before committing.
- Preserve legacy static-recompilation CPU/ABI docs (`README.md` historical section, `analysis/*`, `proof/*`, `runtime/README.md`) as historical reference unless explicitly instructed otherwise.

### Repository map

| Path | Status | Contents |
| --- | --- | --- |
| `engine/` | **active** | Clean-room C++20 headers (`include/wse/{core,io,vm,game,platform,asset}`), host tests, CLI tools, fixtures |
| `docs/` | **active** | `PLATFORM_PLAN.md` (Phase 3/4 design), `ARCHITECTURE.md`, `SYMBOL_MAP.json`, generated native & WSE opcode maps |
| `tools/build_symbol_map.py`, `check_citations.py`, `demand_rank.py`, `extract_native_opcodes.py`, `extract_wse_opcodes.py`, `pdb_symbols.py`, `pe_evidence.py`, `symbol_inventory.py`, `vtables.py` | **active** | Evidence extraction, citation verification, generators, and demand analysis |
| `tools/r0_pc_reference.cpp` | **active** (local only) | Phase 5 R0 (d) PC reference (Windows only, never run in CI; maps the user's v1.1.5.6 exe and runs retail code) |
| `tools/pe_catalog.py`, `check_panvk.py`, `patch_panvk.py` | legacy | Static-recompilation catalog and PanVK helpers |
| `analysis/`, `proof/`, `runtime/` | legacy | Superseded static-recompilation dossier, lifter proofs, guest runtime, PanVK loader |

## 4. Required Documentation Maintenance

Whenever an engine milestone or evidence export lands, keep all five living project documents synchronized:

1. `AGENTS.md` — workflow, gates, constraints, and environment guardrails.
2. `ROADMAP.md` — phase checklists, ordered engineering backlog, and the one-line milestone log.
3. `docs/ARCHITECTURE.md` — verified v1.1.5.6 layouts, PE/REA evidence tables, module contracts, and implemented subsystems.
4. `docs/SYMBOL_MAP.json` — authoritative PDB layout + symbol index (`tools/build_symbol_map.py`).
5. `STATUS.md` (plus `engine/README.md`) — gate pass/fail ledger, coverage counts, blockers, next tasks, and confirmed pseudocode/disassembly facts.

### One owner per fact

Each kind of fact is written in exactly one file; every other file links to it. Repeating a number
in several files is what made the documents drift before.

| Fact | Owner | Everyone else |
| --- | --- | --- |
| Current phase, gate results, `N/440` and other coverage counts, blockers, next tasks | `STATUS.md` §1–§5 | Link to `STATUS.md`; do **not** restate the number |
| Phase checklists, backlog order, milestone log | `ROADMAP.md` | — |
| Phase 3/4 design: seam, target layout, slice contents, platform risks | `docs/PLATFORM_PLAN.md` | `ROADMAP.md` holds only the checklist |
| Phase 5 design: recompilation slices, runtime model, shims, decisions | `docs/RECOMP_PLAN.md` (`docs/GAMEPLAY_PLAN.md` is superseded) | `ROADMAP.md` holds only the checklist; progress in `STATUS.md` |
| Struct layouts, offsets, function VAs, evidence IDs | `docs/ARCHITECTURE.md` + `docs/SYMBOL_MAP.json` | Source comments cite the VA; prose docs link |
| Per-handler behavior notes and safety deviations | `engine/README.md` and the header comment next to the handler | — |
| Opcode numbers and names | generated `docs/wse_opcodes.json` | Never typed by hand |
| Machine setup, tooling quirks | `STATUS.md` §8 (state) and `AGENTS.md` §5 (rules) | — |

Commit subjects may carry the running count (`… (222/440)`); that is a log entry, not a second owner.

### Milestone checklist

1. Run the host gate (and the NDK gate once it exists); paste nothing into docs that you did not see pass.
2. `STATUS.md`: update the date, §1 row(s), the §3 coverage row for the family you touched, and §5 if the next task changed.
3. `ROADMAP.md`: tick the phase checkbox if a whole item finished; add one line to the milestone log.
4. `engine/README.md`: add or amend the handler notes and safety deviations for what you implemented.
5. `docs/ARCHITECTURE.md` / `docs/SYMBOL_MAP.json`: add any new layout, VA, or `ev_*` record you relied on.
6. Check that no superseded-build address slipped in, then `git status --short` and commit.

## 5. Windows / REA / Tooling Guardrails

- **Python working directory:** Never run Python from `%TEMP%\opencode\wse2_pc` because an `inspect.py` script there shadows Python's standard library. Run Python from inside `repo/` or another clean working directory.
- **PowerShell:** `pwsh` is not installed; use Windows PowerShell (`powershell`) with `;` instead of `&&`. Note `Select-String` is case-insensitive unless `-CaseSensitive` is specified.
- **Git push stderr:** `git push` prints progress to `stderr`, causing Windows PowerShell to raise `NativeCommandError` even when the push succeeds; verify exit code `0` and that local `HEAD` matches the pushed remote branch.
- **Ghidra / REA (8 GB RAM host):** Run only one Ghidra analysis at a time, launch heavy exports detached/background, and use `--address-start` / `--address-end` rather than `--name` queries on the v1.1.5.6 binary.
- **Do not switch commits under a running build:** an object that started compiling before a `git checkout` and finished after it is newer than the changed headers, so CMake keeps it; mixed `WseWorldState` layouts then crash `selftest_host` (every suite passing alone). Delete `CMakeFiles/wse_selftest.dir/**/*.o` (or the build tree) after such a switch.
- **Disassembly sweeps:** Capstone linear sweeps halt at invalid padding bytes; scan with per-instruction resync or targeted function bounds.
- **Parallel agent units:** give each unit its own worktree under `repo/.wt/<unit>` (`.wt/` and `.verify/` are in `.git/info/exclude`) with the spec in `.verify/SPEC.md`. Build single suites there with `wse2_pc/verify_suite.sh <suite>` (per-worktree `TMP`, so the fixed-name scratch files never collide); only one full gate at a time on this host. A delegated agent's `--dir` resolves against the spawning shell's current directory: return to the repo root before spawning.
