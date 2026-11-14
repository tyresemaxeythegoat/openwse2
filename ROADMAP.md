# ROADMAP — Clean-Room WSE2 Android Port

Active plan for the WSE2 interoperability port. It supersedes the static-recompilation approach
described in the historical half of `README.md` and in `analysis/`, `proof/`, and `runtime/`; those
artifacts remain as CPU/ABI reference only.

This file answers **"what are we building and in what order"**. It deliberately holds no live
numbers:

| Question | Where to look |
| --- | --- |
| Is the gate green? How many opcodes are done? What is blocked? | `STATUS.md` |
| How do I work in this repo (rules, gates, tooling quirks)? | `AGENTS.md` |
| What does the binary look like (layouts, addresses, evidence IDs)? | `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` |
| What does each portable module implement? | `engine/README.md` |

## Guardrails

1. **Evidence-backed decompilation only.** Never hallucinate memory offsets, struct sizes, or
   function behavior. Every reconstructed class, vtable, and function must cite v1.1.5.5 evidence
   (function VA, PDB TPI type index/size, RTTI/vtable symbol, string cross-reference, or REA `ev_*`
   record).
2. **Bring Your Own Assets (BYOA).** This is an engine reimplementation for interoperability. Do
   not bundle proprietary game assets and do not implement DRM circumvention. The Android app must
   prompt the user to select their own legally owned Mount & Blade: Warband PC game directory and
   `Modules/` folders.
3. **Incremental compilation gate.** A subsystem is not finished until it compiles cleanly under
   both the host tests and the Android NDK (`aarch64-linux-android29-clang++ -std=c++20 -O2 -Wall
   -Wextra -Werror`).
   **Gate status:** enforced in `.github/workflows/ci.yml` (`host-linux` + `android-arm64`) as of
   commit `117436a` (Backlog B1). On-device Android runtime execution remains blocked until hardware
   or an emulator is available (see `STATUS.md` §4).

## Target

- Analysis binary: official **WSE2 v1.1.5.5** `mb_warband_wse2_x64.exe` (PDB-backed; image base
  `0x140000000`). The 32-bit `mb_warband_wse2.exe` in `WSE2.zip` is out of scope.
- Reference device in the legacy docs: OPPO Reno 11, Mali-G68 (PanVK). No device is attached.

## Phases

Legend: `[x]` done and host-tested · `[~]` partly done · `[ ]` not started.

### Phase 1 — Binary Triage & SDK Alignment (complete)

- [x] Load the binary through REA/Ghidra; extract imports, strings, vtables, and global tables.
- [x] Cross-reference the public WSE SDK (`header_operations_addon.py`) against the binary to
      locate the opcode dispatcher, trigger managers, and script loaders; map all 440 extended
      operations to `mbStatement::executeWSE` (`0x1401e4dc0`).
- [x] Write `docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json` before engine code.
- [x] Retarget every citation from the 2026-10-14 build to v1.1.5.5.

### Phase 2 — Core Data Structures & Module System VM (current)

**2A. Streams and module text**
- [x] `rglTextStream` token reader (`wse/io/text_stream.h`)
- [x] `rglBinaryStream` little-endian reader/writer (`wse/io/binary_stream.h`)
- [ ] Whole-savegame framing (only the trigger records are reconstructed)
- [ ] Remaining module files: `troops.txt`, `item_kinds1.txt`, `factions.txt`, `parties.txt`,
      `party_templates.txt`, `strings.txt`, `quick_strings.txt`, `skills.txt`, `quests.txt`,
      `scenes.txt`, `mission_templates.txt`, `menus.txt`, `presentations.txt`, `module.ini`, …

**2B. Statement VM**
- [x] Operand tags, decode, and lvalue write-back (`wse/vm/operand.h`, `slots.h`)
- [x] Statement record and reader (`wse/vm/statement.h`)
- [x] `analyze` / `execute` / loop frames / `call_script` recursion (`wse/vm/block_vm.h`)
- [~] Native opcodes: `619/1,053` implemented across `wse/vm/block_vm.h` and
      `wse/vm/native_{ops,entity_ops,mission_ops,campaign_ops,campaign_ops_2}.h` (control flow,
      comparisons, integer/fixed-point/trig/RNG math, `entity_slots` `500–510`/`520–530`/`540–550`/`560–570`,
      `positions_and_math` `700–746`/`791`, `strings` `2318–2355`, player/MP, quest, audio/UI,
      site/faction, troop/party ops in `native_entity_ops.h`, agent/team/prop/mission/timer
      ops in `native_mission_ops.h`, and `284` Milestone 3 campaign/item/troop/party/mission ops in
      `native_campaign_ops.h` & `native_campaign_ops_2.h`)
- [x] Native opcode map generated from the `0x1401e22f0` jump table (**B2**, `docs/native_opcodes.json`)
- [x] Entity iterators (`try_for_parties`, `try_for_agents`, `try_for_prop_instances`,
      `try_for_players`) — live against `WseWorldState` containers and preserving `currentCaller`

**2C. Scripts and triggers**
- [x] `mbScriptManager` / `mbScript` readers and the 65-name mapped-script table (`wse/game/script.h`)
- [x] Trigger and simple-trigger loaders, event-key state machines, and savegames (`wse/game/trigger.h`)
- [ ] `mbSimpleTriggerManager::execute` early-out (depends on the `mbGame` event queue)

**2D. WSE extended opcodes**
- [x] Opcode map and generated enum (`docs/wse_opcodes.json`, `wse/vm/wse_opcodes.h`)
- [x] Pure-computation families: shifts, registers, dictionaries, strings, options, floating point
      (incl. `feval`), position vectors, dynamic arrays
- [x] State-backed families against the in-memory model: slots, troops, items, parties (`3900–3911`, 12/12), edit mode
- [~] Partial engine-backed families: agent (`3340–3341` `action_speed_modifier`), mission/props (`3600` etc.)
- [ ] Remaining engine-backed families: agent (rest), player, mission/props (rest), scene, multiplayer/server,
      menus, overlays/presentations, input keys, reload, skins, Lua bridge

**2E. Game-world state**
- [~] `WseWorldState` containers and validators (`wse/game/world_state.h` + `wse/game/world_campaign_helpers.h`: players, troops, items,
      skills, factions, quests, sites/visitors, parties/stacks, agents, mission props/timers, teams,
      presentations/overlays, audio events) — in-memory model only
- [ ] Loaders that populate it from a user-supplied module (depends on 2A)
- [ ] Agent, party, mission-object, and player containers with real lifetimes

**2F. Lua 5.1 layer**
- [ ] Link stock Lua 5.1 for arm64; reconstruct `WSELuaManager`, the `gameOperation` registry, and
      the statement/operation hooks

**2G. Core RGL value types**
- [~] Transform / vector math (`WseVec2` length/`get_angle`, `WseVec4`, and `WseTransform`
      local/global rotations and parent/local transforms in `wse/core/math.h`; 35 native position
      ops `700–746` in `wse/vm/native_ops.h`; WSE vector ops `4100/4102–4104` in
      `wse/vm/ops/position.h`)
- [ ] `rglString` semantics as a shared type; `.brf` mesh and texture loaders

**2H. Phase exit: headless `Native` run**
- [ ] Load a user-supplied `Native` module and execute `game_start` plus one simulated frame of
      triggers with **zero** unimplemented-opcode hits, on host and on arm64

### Phase 3 — Cross-Platform Replacement (Win32/DX9/FMOD → C++20/SDL3/Vulkan)

Runs **in parallel** with Phase 2 as the platform track. Design, layout, slice contents, and risks
are in [`docs/PLATFORM_PLAN.md`](docs/PLATFORM_PLAN.md); the `P` numbers below are its slices.

- [x] **P0** Root `CMakeLists.txt` + CTest and GitHub Actions host + NDK `aarch64-linux-android29`
      gates (`117436a`, backlog B1).
- [~] **P1** Platform seam and app shell.
  - [x] `wse/platform/{vfs,clock,log}.h` with host implementations and the `platform` suite
  - [ ] Route the engine's direct file, clock, and thread calls through the seam
  - [ ] Gradle + SDL3 `SDLActivity` + `libwse2.so` with an on-device self-test screen; CI builds the APK
- [ ] **P3** Asset decoders: `.brf`, `.dds`, module resource lists (evidence exports first).
- [ ] **P4** Vulkan RHI and runtime driver loader: system driver, PanVK (Mali), Turnip (Adreno) —
      drivers are user-supplied, never bundled.
- [ ] **P5** Module shaders → SPIR-V at import time, cached; D3D9 conventions handled in the RHI.
- [ ] **P6** Audio: SDL3 audio-stream backend and mixer replacing FMOD Ex.
- [ ] **P9** Convergence with Phase 2: scene/terrain/skeletal rendering, widgets, Bullet, Lua link,
      and the 2H headless run on device.

### Phase 4 — Native Android Features

- [ ] **P2 Asset & mod loader UI:** SAF folder picker for the user's Warband install, persisted
      permission, module list and mod switcher (equivalent to `wse2_launcher.exe`), GPU driver
      picker; native `SafVfs` with a one-time case-insensitive index.
- [ ] **P7 Dynamic touch controls:** context-sensitive overlay (virtual movement stick, 4-direction
      combat swipe/block zones, battle command radial menu) that hides during menu/inventory
      screens or when a controller is connected.
- [ ] **P8 Sensor & performance hooks:** optional gyro aiming, pinch-to-zoom on the campaign map,
      thermal-throttling awareness, and configurable frame-rate caps (60/90/120 Hz).

## Engineering backlog (ordered)

Work that is not a new subsystem but decides how fast the phases above can move. Do these in order;
B1–B3 come before any further opcode transcription.

| # | Item | Why it is next | Done when |
| --- | --- | --- | --- |
| B1 | **CI + cross-platform build.** One `CMakeLists.txt` + CTest for `engine/`, and a GitHub Actions workflow running the six host suites on Linux and an NDK `aarch64-linux-android29` compile of every header. | The arm64 gate has never run and is the project's stated definition of done. GitHub's Ubuntu runners ship an NDK, so the "no NDK" blocker disappears without any local install. The host suites already pass on Linux clang 18. | **DONE (`117436a`).** Workflow green on `main`; `STATUS.md` §2 shows both Linux CMake/CTest and NDK `aarch64-linux-android29` gates PASS |
| B2 | **Native opcode map.** Generate `docs/native_opcodes.json` / `.md` from the jump table at `0x1401e22f0` and `header_operations.py`, mirroring `tools/extract_wse_opcodes.py`. | The 440 WSE operations are counted to the unit, but the ~2,700-slot native table — the bulk of what any module actually calls — has no inventory, so the real size of Phase 2 is unknown. | **DONE.** `tools/extract_native_opcodes.py` generated `docs/native_opcodes.json` and `docs/NATIVE_OPCODES.md` with SDK names, handler VAs, and `implemented` flags scanned from `block_vm.h` (`STATUS.md` §3) |
| B3 | **Demand ranking.** A host tool that reads a user-supplied module's `scripts.txt` / `triggers.txt` / `simple_triggers.txt` and prints opcode frequency × implemented status. | Handlers have been transcribed in numeric order. A frequency table from real modules says which 50 opcodes unlock most scripts. The readers already parse the full retail `scripts.txt`. | **DONE.** `tools/demand_rank.py` added; counts-only ranking checked into `STATUS.md` §3 |
| B4 | **Generated implementation status.** Have `tools/extract_wse_opcodes.py` (or a sibling) scan `execute_wse.h` and write an `implemented` flag into `docs/wse_opcodes.json` and a Done column into `docs/WSE_OPCODES.md`. | The `N/440` figure and the list of ranges were hand-copied into eight places per milestone and had already drifted. `STATUS.md` is now the single tally; generating it removes the last manual copy. | **DONE.** `tools/extract_wse_opcodes.py --sync-status` scans `engine/include/wse/vm/**/*.h` and updates `docs/wse_opcodes.json`, `docs/WSE_OPCODES.md`, and `STATUS.md` §1/§3 (`--check` enforced in CI) |
| B5 | **Split `execute_wse.h`.** One header per opcode family (`wse/vm/ops/string.h`, `dict.h`, `array.h`, `fp.h`, …), shared value types (`WseTransform`, UTF-8/16, MD5, JSON) moved to `wse/core/`, and `wse_test.cpp` split to match. | The file is 7,448 lines with one 3,350-line `switch`; its test file is 3,653 lines. Each takes ~15 s to compile, every handler edit touches the same file, and the remaining 218 handlers would roughly double it. | **DONE.** Split into `wse/core/{math,encoding,json}.h`, `wse/vm/wse_state.h`, `wse/vm/ops/*.h`, and slim `execute_wse.h` (all 24 engine headers `<= 1,301` lines); `wse_test.cpp` split into `wse_test_common.h` + 6 host suites |
| B6 | **Loud unimplemented opcodes.** Surface `VmContext::unimplementedCount` as a per-opcode histogram, and give native opcodes the same explicit `unimplemented` result WSE opcodes have instead of returning `true`. | An unimplemented native condition currently passes silently, so a script can take the wrong branch with no signal. | **DONE (`8131b16`).** Added `NativeDispatch` (`handled`/`condition_false`/`unimplemented`), `VmContext::unimplementedOpcodeCounts`, and `VmContext::failOnUnimplemented` loud abort mode (`block_vm_test.cpp`) |
| B7 | **Citation check.** A script that verifies every `0x14…` address in `engine/include` resolves to a symbol or function range in the v1.1.5.5 evidence, and rejects superseded-build addresses. | The evidence rule is the project's core promise, and nothing enforces it. Commit `tools/vtables.py` at the same time so `SYMBOL_MAP.json` is reproducible. | **DONE.** Committed `tools/vtables.py` (exact match to v1.1.5.5 `vtables_new.json`), expanded `tools/build_symbol_map.py` / `docs/SYMBOL_MAP.json` (2,909 symbols, 4,152 function ranges), fixed 3 stale citations under `engine/include/wse/`, reconciled `docs/ARCHITECTURE.md` §1–§3/§8–§9, and wired `tools/check_citations.py --self-test` into `.github/workflows/ci.yml` |
| B8 | **Exception and platform audit of handlers.** `execute_wse()` is `noexcept` but allocates, does file I/O, and calls `std::regex`; `iswalpha`-family predicates are locale-dependent and differ between MSVCRT and Bionic; `sleep_ms` blocks the calling thread. | A thrown `std::bad_alloc` or `filesystem_error` terminates the process on Android; character-class differences change script results. | **DONE.** Dropped `noexcept` from `execute_wse()`, `DispatchFn`, and all 8 `execute_wse_*` family dispatchers; replaced `<cctype>`/`<cwctype>` locale predicates with deterministic `"C"`-locale ASCII `constexpr` helpers; added optional `WseVmState::sleep_ms_fn`/`sleep_ms_user` (`WseSleepMsFn`) platform hook with `std::this_thread::sleep_for` fallback (`wse_string_test.cpp`, `wse_system_test.cpp`, `engine/README.md`) |

## Milestone log

Newest first. One line per landed milestone; details belong in the commit and in `STATUS.md`.

| Date | Commit | Milestone |
| --- | --- | --- |
| 2026-11-14 | — | Phase 2 Milestone 3: expand `WseWorldState` + `world_campaign_helpers.h`, implement `14` WSE party/agent opcodes (`3340–3341`, `3900–3911`; `236/440`) and `284` native campaign/mission/item/troop/party opcodes across `native_campaign_ops.h` and `native_campaign_ops_2.h` (`619/1,053` live handlers), default `VmContext::clearLocalsBetweenCalls` to `true` (`0x140617819`), raising `Native` `scripts.txt` coverage from `97.76%` to `99.54%` (`1,545/1,755` zero-unimplemented scripts, `0` unimplemented WSE calls) |
| 2026-11-14 | — | Code-review fixes: `try_for_dict_keys` and `array_sort_custom` no longer hold pointers into script-mutable objects (two ASan-confirmed use-after-frees), loaders allocate records incrementally, dynamic arrays capped at `kWseArrayMaxElements`, control opcodes masked consistently, `HostVfs` refuses symlink escapes; CMake defaults to `-O2`, CI adds an ASan/UBSan job and runs on every branch |
| 2026-11-14 | `7a23109` | Phase 2 Milestone 2: expand `WseWorldState`, activate the 4 entity iterators (`11/12/16/17`) preserving `currentCaller`, and reconstruct `174` high-demand native opcodes (`108` in `native_entity_ops.h` + `66` in `native_mission_ops.h`, `335/1,053` live handlers), raising `Native` `scripts.txt` statement coverage from `93.0%` to `97.76%` (`1,041/1,755` zero-unimplemented scripts) |
| 2026-11-14 | `025b06f` | Phase 2 Milestone 1: reconstruct `112` high-demand native Warband opcodes (`161/1,053` live native handlers across `block_vm.h` + `native_ops.h`), raising `Native` `scripts.txt` coverage from `66.2%` to `93.0%` (`685/1,755` zero-unimplemented scripts) |
| 2026-11-14 | `4bd4066` | `tools/vtables.py` + `tools/check_citations.py --self-test` in CI + `SYMBOL_MAP.json` expansion (`B7`); non-`noexcept` `execute_wse*`, ASCII `"C"`-locale char helpers, and `WseVmState::sleep_ms_fn` hook (`B8`) |
| 2026-11-14 | `4092c7c` | Phase 3/4 planned as platform slices P0–P9 (`docs/PLATFORM_PLAN.md`: SDL3, Vulkan with user-supplied PanVK/Turnip); platform seam `wse/platform/{vfs,clock,log}.h` + `platform` suite (`cc69cfa`, P1 first part; PR `#2`) |
| 2026-11-14 | `8131b16` | `NativeDispatch`, per-opcode unimplemented histogram (`unimplementedOpcodeCounts`), and strict `failOnUnimplemented` abort mode (`B6`) |
| 2026-11-14 | `e4109fe` | Generated WSE opcode status + CI `--check` (`B4`), `execute_wse.h` split into `wse/core/` + `wse/vm/ops/*.h`, and `wse_test.cpp` split into 6 suites (`B5`) |
| 2026-11-13 | `7e9c9df` | Native opcode map (`docs/native_opcodes.json`, B2), module demand ranking tool (`tools/demand_rank.py`, B3), and `kTryForParties`/`kTryForAgents` (`0xB`/`0xC`) VM fix (`STATUS.md` §3) |
| 2026-11-13 | `117436a` | Root `CMakeLists.txt`/CTest and GitHub Actions Linux clang++ + Android NDK `aarch64-linux-android29` CI (`B1`) |
| 2026-11-13 | `75b4b22` | Project review: docs restructured around single ownership; host suites confirmed on Linux clang 18; backlog B1–B8 recorded |
| 2026-11-13 | `2e17f3c` | `WseWorldState` and 64 world/troop/item/config/edit-mode handlers (222/440) |
| 2026-11-13 | `6a70856` | `feval`, dynamic arrays, unified `WseObjectManager` (158/440) |
| 2026-11-13 | `86192dc` | Timers, RNG, profiler, position math, dictionary VM (134/440) |
| 2026-11-13 | `c9f52c1` | Statement-block VM; v1.1.5.5 symbol map and docs |
| 2026-11-13 | `50b4f03` | String MD5/regex/URL, options flags, `fld_str` (93/440) |
| 2026-11-13 | `6857306` | String register and store ops (76/440) |
| 2026-11-13 | `8a9d6e2` | Evidence baseline retargeted to WSE2 v1.1.5.5; 440-opcode map regenerated |
| 2026-11-13 | `0ad7236`, `6a0d56d` | Floating-point handlers and dispatch effect statuses |
| 2026-11-12 | `34b3710` | WSE extended opcode map; shift/bitwise dispatch |
| 2026-11-12 | `d651e37` | `mbScript` manager and `scripts.txt` reader |
| 2026-11-12 | `ef6e7a8` | Binary savegame stream and trigger savegames |
| 2026-11-12 | `7ed50c2`, `a372f01` | Module text token format, trigger loaders, trigger runtime state machines |
| 2026-11-12 | `325dd12` | Portable operand decoder and statement reader with host tests |
| 2026-11-12 | `0a750cf` … `1104fb4` | Evidence-backed triage, architecture, symbol map, statement VM encoding |
| 2026-11-11 | `5a8cca5` … `5ef90e1` | Legacy static-recompilation proofs, runtime skeleton, PanVK loader (superseded approach) |
