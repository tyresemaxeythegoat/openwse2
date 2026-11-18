# Gameplay plan (Phase 5) — superseded 2026-11-18

> **Superseded by `docs/RECOMP_PLAN.md`** (the user chose recompiling the retail code over decompiling it). The oracles below (savegame round-trip, simulation comparison) are reused there.

This document owns the design of Phase 5: everything between "the module's scripts run" and
"you can play a campaign on the phone". `ROADMAP.md` holds only the checklist; `STATUS.md`
holds progress. Platform slices (P-numbers) stay in `docs/PLATFORM_PLAN.md`.

## Where we are (2026-11-18)

Done and host-tested: every module file loader, the world model (`WseWorldState`), all 1,053
native and 440 WSE opcodes, triggers, the Lua layer, savegame framing and section codecs
(`savegame.h`), `.brf` / `.dds` / skeleton / scene-file decoding, shaders compiled to SPIR-V,
audio mixing, the module UI (menus, presentations) in the app.

Not built: **nothing moves on its own.** The opcodes read and write the world model, but no
code advances it. There is no campaign-map simulation (time, party movement, pathfinding,
party AI, spotting, encounters), no mission runtime (agents, animation, collision, combat,
missiles, horses, battle AI), and the app draws no 3D (the UI is 2D geometry through
`SDL_Renderer`; the Vulkan RHI is used only by the driver probe).

## Decisions (from the 2026-11-18 interview)

| Question | Decision |
| --- | --- |
| Fidelity | **1:1 with WSE2 v1.1.5.5.** Every gameplay function is reconstructed from the retail binary and cited, like the VM. No "feels close" approximations. |
| Who codes | Claude reverse-engineers each function and writes a spec with VA citations and host tests; Antigravity implements from the spec; Claude reviews the diff and runs the gate. Evidence work is never delegated (`AGENTS.md`). |
| First playable | Order is set by dependencies, not taste (below). |
| Saves | **Byte-compatible both ways** with retail WSE2 saves. |
| Multiplayer | Out of scope until single-player is complete. Revisit then. |
| Scale | WSE2 battles go past 1,000 agents. No fixed FPS target: optimize as far as possible without changing behaviour. |
| Device | Mali-G68 (system Vulkan driver) is the primary device; Adreno must also work. |

## How "1:1" is checked

"Matches retail" has to be something a test can fail. Three layers:

1. **Function tests from the decompiled code.** Each reconstructed function gets host tests
   that walk its branches with inputs chosen from the pseudocode (for example every attack
   direction and armour case of `mbMission::calculateDamage`). The spec lists the branches;
   the tests must cover them.
2. **Savegame round-trip oracle.** A save written by retail WSE2 must load and re-save
   **byte-identical**, and a save we write must load in retail WSE2. This proves every
   reconstructed field is stored the same way and gives the next layer its inputs. Needs a
   small corpus of your PC saves (early, mid and late campaign; Native and one big mod).
3. **Simulation oracle (campaign map).** Load retail save S0, advance N in-game hours with
   no player input, and compare against retail save S1 made on the PC after waiting the same
   N hours from S0. Whether this can be exact depends on how retail steps map time and
   consumes random numbers; G0 finds out (below). If retail is not reproducible, the oracle
   drops to per-function tests plus a statistical comparison (party counts, positions within
   a tolerance, faction strengths) — decided once, written here.

Battles are real-time and driven by input, so they have no whole-run oracle; they rely on
layer 1 and on scenario tests (a fixed duel, a fixed 20-vs-20 fight) whose outcomes are
checked against the decompiled rules.

## Evidence map

Method counts from `docs/SYMBOL_MAP.json` (PDB public symbols and function ranges), to show
the size of the job. "Closure" means the functions reachable from that slice's entry points.

| Class | Methods in the map | Entry points that define a slice |
| --- | ---: | --- |
| `mbGame` | 180 | `frameMove`, `updateHour`, `updateDay`, `updatePartySpotting`, `updateTracks`, `startEncounter`, `startBattle`, `calculateBattleAdvantage`, `createPartiesAfterBattle`, `spawnPartyAroundPosition`, `updateMapMeshes` |
| `mbParty` | 69 | `frameMove`, `update`, `aiSelectBehavior`, `aiUpdateBehavior`, `aiUpdateMovement`, `aiComputePath`, `aiNavigatePath`, `getMovementSpeed`, `updateHour`, `leaveBattle` |
| `rglManifold` / `rglAiMesh` | 14 / 19 | `computePath`, `checkPath`, `checkPathFast` (map and scene pathfinding) |
| `mbMission` | 147 | `frameMove`, `spawnAgentFromTroop`, `spawnHumanAgent`, `spawnHorseAgent`, `spawnEntryGroups`, `findSpawnPoint`, `checkAttackCollision`, `calculateDamage`, `spawnMissile`, `updateDivisions`, `giveAttackOrder`, `updateCamera`, `updateCorpses` |
| `mbAgent` | 231 | `frameMove`, `fillMovementCapsule`, `computeAttackType`, `getMeleeAttackValues`, `damageShield`, `ai*` (40 methods: targeting, formations, ranged, pathing) |
| `mbMissionGrid` | 18 | spatial grid for agent queries |
| `rglCompositeBody` / `rglScene` / `rglSkeleton` | 14 / 98 / 91 | collision bodies, scene graph, animation |
| `rglRenderer` | 41 | `render`, `renderContext`, `renderMesh`, post-FX |

Each slice starts with an **evidence pass** (Claude, one Ghidra job at a time on the 8 GB
host): walk the call graph from the slice's entry points, decompile the closure, record
layouts and VAs in `docs/ARCHITECTURE.md` / `SYMBOL_MAP.json`, and write the unit specs.
Only then does implementation start.

## Order and why

The retail game is one loop: `mbGame::frameMove` runs either the map (`mbMapWindow`) or a
mission (`mbMission::frameMove`). The order below follows what each piece needs.

| Slice | What you can do at the end | Needs |
| --- | --- | --- |
| **G0 Oracles** | Nothing new on screen. Retail saves load and re-save byte-identical; we know whether the map simulation can be reproduced exactly. | — |
| **G1 3D on the phone** | See a scene and the world map in 3D on Mali-G68 with the module's own shaders, with the UI drawn on top. | P4/P5 (done) |
| **G2 Campaign map** | Start a new game, ride around the world map, watch parties move, enter towns and villages (their menus open), fight by auto-resolve, wait / camp, save and load. | G0, G1 |
| **G3 Walking scenes** | Walk around a town, castle or village scene; mission triggers and dialogs run; doors and props work. | G1 |
| **G4 Combat** | Fight in a scene: melee, blocking, shields, ranged weapons, horses, damage, death. | G3 |
| **G5 Battles at scale** | Field battles from the map: troop AI, formations and orders, reinforcements, retreat, back to the map with casualties applied. 1,000+ agents. | G2, G4 |
| **G6 Whole game** | Sieges, tournaments, character creation, every remaining menu path, save anywhere both ways. Single-player complete. | G5 |

P11 (in-game UI and touch controls, `docs/PLATFORM_PLAN.md`) must land before G3 is usable on
a phone; its map buttons are needed in G2.

### G0 — Oracles and the evidence pipeline
- Savegame corpus: you copy 4–6 retail saves (Native and one large WSE2 mod) into a local
  folder that is never committed. Test: load → save → compare bytes. Every mismatch is a field
  we decode wrongly; fix until all are identical. Add the reverse check: a save we write loads
  in retail on your PC.
- Determinism investigation: decompile how `mbGame::frameMove` advances map time (fixed step
  or frame delta?), where random numbers come from (one generator? is its state saved?), and
  whether waiting in camp steps time in fixed increments. Result: "exact oracle possible"
  or "statistical oracle", written into "How 1:1 is checked" above.
- Tooling: a call-graph closure script over the existing capstone helpers (resyncing on
  padding, per `AGENTS.md` §5), and the evidence-spec template Antigravity implements from.

### G1 — 3D on the phone
- Present through Vulkan in the app (system driver first; PanVK / Turnip stay selectable):
  swapchain from the SDL window, the UI's geometry batches drawn with a Vulkan pipeline instead
  of `SDL_Renderer`.
- Draw scenes and the world map with `BrfSceneRenderer` on `VulkanRhiDevice`, using the
  module's shaders (P5) and the retail render state order from `rglRenderer::renderContext`.
- GPU skinning (the CPU skinning in `skeleton.h` stays as the test reference); texture budget
  from P10.7.
- Done when: the Native world map and one town scene render on Mali-G68, matching a PC
  screenshot from the same camera within a pixel-diff threshold set in the spec.

### G2 — Campaign map
- Time: `updateHour` / `updateDay`, sun direction, map relief shading.
- Movement: player click-to-move and `mbParty::frameMove` / `update` / `getMovementSpeed`,
  pathfinding on the map manifold (`rglManifold::computePath`), terrain speed factors.
- Party AI: `aiSelectBehavior`, `aiUpdateBehavior`, `aiUpdateMovement`, escort / go-to /
  patrol, spotting (`updatePartySpotting`) and tracks.
- Encounters: `startEncounter` into the game menus that already run, auto-resolve
  (`calculateBattleAdvantage`, `createPartiesAfterBattle`).
- Done when: the G0 oracle passes for a 72-hour wait from the corpus saves, and a new Native
  campaign is playable on the map for an hour with saves loading on the PC.

### G3 — Walking scenes
- Mission start from menus (`set_jump_mission` already reaches the VM): scene load (terrain
  from the scene code, `.sco` objects, props), spawning (`findSpawnPoint`,
  `spawnEntryGroups`, `spawnAgentFromTroop`), the player camera (`updateCamera`).
- Agent locomotion and the animation state machine from `actions.txt` (`mbAgent::frameMove`),
  collision (`fillMovementCapsule`, `rglCompositeBody`), scene pathfinding
  (`rglAiMesh::computePath`) for NPCs.
- This **replaces the P9 "Bullet physics" item**: retail uses its own collision, so a
  third-party physics engine would break 1:1.
- Done when: you can walk through a Native town, talk to NPCs, and leave by the exit.

### G4 — Combat
- Attack flow (`computeAttackType`, `getMeleeAttackValues`, `checkAttackCollision`,
  `calculateDamage`, `damageShield`), blocking, missiles (`spawnMissile`), horses
  (`spawnHorseAgent`, mounting, charge damage), death and corpses (`updateCorpses`).
- Done when: function tests cover every branch listed in the specs, and a scripted duel
  scenario produces the damage numbers the decompiled formulas predict.

### G5 — Battles at scale
- Battle AI (`mbAgent::ai*`), divisions and orders (`updateDivisions`, `giveAttackOrder`,
  `getGroupMovementOrder`), `mbMissionGrid`, reinforcements, battle end, `leaveBattle` and
  casualties back on the map.
- Performance, without changing results: profile on Mali-G68 first, then
  - keep the retail update order and random-number use exactly (behaviour must not change);
  - move work that does not affect game state off the game thread: skinning, animation pose
    sampling for rendering, culling, draw recording;
  - GPU instancing for repeated meshes, LOD from the module's meshes, the texture budget;
  - memory layout changes only where they don't change iteration order.
- Done when: a 1,000-agent Native field battle runs to the end on Mali-G68 with frame times
  recorded per subsystem.

### G6 — Whole game
- Sieges (siege towers, ladders and scene props), tournaments and arenas, character creation,
  the remaining presentations and menu paths, save anywhere.
- Done when: a full Native campaign reaches the end state, and saves move between phone and
  PC at any point.

## Risks

| Risk | Plan |
| --- | --- |
| Evidence throughput (about 700 methods across the slices, one Ghidra job at a time on 8 GB) | Closure per slice, not the whole binary; reuse decompilation exports; Antigravity only writes code from finished specs. |
| Map simulation can't be reproduced exactly | Decided in G0; the statistical oracle is the fallback, written down before G2 starts. |
| 1,000+ agents on a phone | Measured in G5; only behaviour-neutral optimizations; battle size stays a setting. |
| Mali system driver differences | PanVK and Turnip stay selectable (`driver_loader`); the probe in the launcher reports which works. |
| Scope creep into multiplayer | Excluded until G6 is done. |
