# STATUS

Last updated: 2026-11-13.
Phase: **1 — Binary Triage & SDK Alignment** (script VM, module text/binary streams, trigger loaders/runtime/savegames recovered).

## Passing

| Check | Result | Evidence |
| --- | --- | --- |
| Target extraction | PASS | v1.1.5.5 `mb_warband_wse2_x64.exe` 7,060,992 B, SHA-256 `d4e97d56…4e29709`; official `WSE2.zip` `c45197e8…276e28b1` |
| REA/Ghidra analysis | PASS | v1.1.5.5 analysis (`ev_e323351d…` / `ev_fa7fc7d6…`): 9 segments, 20,276 procedures, 7,448 strings; image base `0x140000000`; PDB `fccf5e8d…8cfc` age 449 |
| PE import inventory | PASS (v1.1.5.5) | `ev_a4af0bca5d107b626454d91f4c69c2aef5d4f21b466cbb174333d7c8cd687fc6`: 23 DLLs (21 static + 2 delay), 482 + 64 = 546 functions |
| PDB parse (MSF 7.0/TPI/DBI) | PASS | 156,846 TPI records, 15,363 sized layouts |
| PDB symbol map | PASS | 48,658 symbols with VAs (`globals_new.json`) |
| Vtable scan | PASS | 279 pointer runs; 207 class-identified; 73 `rgl*`/`mb*`/`WSE*` |
| Tracking documents | PASS | `AGENTS.md`, `ROADMAP.md`, `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (29 classes, 1,782 symbols, 73 vtables; adds `mbBasicGame`, `mbCallStack`, `mbLoop`, `mbLoopManager`), v1.1.5.5 target |
| v1.1.5.5 REA VM/stream batches | PASS | 75 procedures across 9 `decompile_all` exports (`stmt_core` `ev_3ebcea85…` 4/4, `stmt_block` `ev_ffc38aa5…` 10/10, `script` `ev_8d37a0c8…` 4/4, `simple_trigger` `ev_3157906d…` 12/12, `trigger` `ev_f62843ac…` 12/12, `game_scripts` `ev_fc9bd1a4…` 4/4, `game_triggers` `ev_052f2d86…` 2/2, `streams_bin` `ev_8cc48986…` 9/9, `streams_txt` `ev_1e3c2566…` 18/18), plus `read_bytes`/`0x140615ad0` (`ev_f09662c6…`/`ev_2ddf7a62…`) and 6 single-procedure exports |
| Superseded 2026-10-14 REA runs | PASS (superseded) | Historical 2026-10-14 entries kept for provenance only: `ev_7cd99ccc…157d7a` (`mbScript` 5), `ev_8d91b175…99b11` (`mbStatement` 14), `ev_25469ff4…84af8` (3), `ev_e3530f99…e8d05` (`Trigger` 52), `ev_cebc7ae0…e8c8d3` (`rglTextStream` 18), `ev_24eecfa4…f18c0b6` (`rglBinaryStream` 9), `ev_130d8023…2c62e6` (2) |
| Binary savegame format | PASS | `rglBinaryStream` raw little-endian 8/4/1-byte values; strings are `int32` length + bytes; no header or checksum |
| Module stream identification | PASS | `0x1405651e0` = `rglInputTextFileStream` (readers at `+0x58/+0x70/+0x78/+0x80/+0x90`); `0x1405653e0` = `rglOutputTextFileStream` writers; module files are decimal token text |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Portable engine host tests | PASS | `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse tests: PASS`, `block_vm tests: PASS`, `engine host tests: PASS` (Zig clang++ `-std=c++20 -O2 -Wall -Wextra -Werror`) |
| Statement-block VM & trigger/script bridge | PASS | `engine/include/wse/vm/block_vm.h` reconstructed and host-tested (`analyze`, `execute`, `startLoop`/`endLoop`/`canLoop`, core native ops `30–33`/`2100–2133`, `call_script` up to depth 255, `store_script_param*`, `this_or_next`/`neg`, script & trigger execution) |
| WSE extended opcode map | PASS | v1.1.5.5 `executeWSE` (`0x1401e4dc0`) pseudocode `ev_2aaa96a8…5cf0f9`: 440/440 in-range operations 2800–5202 (431 switch + 9 equality), no unknown or unhandled opcode; 8,844-line dump |
| Retail `scripts.txt` parse | PASS | Full 3,467,512-byte sample: 1,755 scripts, every token consumed, 52 of the 64 then-known names (the rest are WSE-only scripts absent from that module); v1.1.5.5 adds a 65th name |
| WSE2 v1.1.5.5 release delta | PASS (inventory) | Official `WSE2.zip` SHA-256 `c45197e8…276e28b1`; x64 exe 7,060,992 B with file version 1.1.5.5; SDK adds 7 operations (3911, 4517–4522), no renumbering; 440 in-range ops; mapped script table grows to 65 with `wse_camp_button_clicked` |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## In progress

| Task | State |
| --- | --- |
| Portable trigger & script VM runtime | Event-key state machines and script execution (`block_vm.h`) run reconstructed conditions/consequences through the VM and pass host tests; callback overloads in `trigger.h` remain available; world entity iterators (`try_for_agents`/etc.) cleanly iterate 0 times until entity containers are reconstructed |
| Portable trigger savegames | Binary reader/writer and simple-trigger reader reconstructed and host-tested, including count-exceeded consumption and truncation rejection |
| `executeWSE` case mapping | Complete: 440/440 SDK operations 2800–5202 resolved to switch/equality handlers; no unknown in-range opcode (v1.1.5.5) |
| Portable `executeWSE` handlers | 93 of 440 opcodes transcribed and host-tested: 2800–2809 shifts/xor/complement, 3000–3002 register get/set and version query, 3014/3016/3017/3035/3600 script-context queries, string group 4200–4224 (including portable RFC 1321 MD5 for 4219), 4233–4234, 4236, 4238, and 4240–4244 (`str_regex_*`/`str_decode_url`), 4300–4305 options flags, and 4400–4401/4405–4438 floating-point load/store/compare/arithmetic plus the transcendental/rounding group; the other 347 report `WseDispatch::unimplemented` |

## Confirmed by pseudocode

- `mbScriptManager::readStream` (`0x14020f7c0`): two `readString` calls (discarded), two 32-bit reads,
  the second into `m_numScripts`; frees the old array, allocates `8 + count*176`, constructs
  `mbScript[]`, calls `mbScript::readStream(index)` per entry, then `mapScripts()`.
- `mbScript::readStream` (`0x14020f6a0`) order: `m_id` (`readString`), `m_interval`
  (`readFloat` @ `+0x48`), `mbStatementBlock::readStream` (@ `+0x50`), then sets
  `m_statementBlock.m_id` to `rglFormat("Script [%d] %s", index, m_id)`.
- `mbScriptManager::mapScripts` (`0x14020f9a0`): 65-entry static name table at `0x140615ad0`
  (`game_start` … `wse_initial_window_start`, `wse_camp_button_clicked`), linear match on `m_id` with the first script in file
  order winning, writes the index or `-1` into `m_mappedScripts` (`0x14069facc`), and calls `rglInfo`
  with `"Unable to map script %s"` on failure. All 65 names are transcribed in `script.h` and were
  verified byte-for-byte against the table pointers in the v1.1.5.5 image.
- Module files are whitespace-delimited decimal text, not binary. `mbGame::readTriggers`
  (`0x1400ccf20`) and `readSimpleTriggers` (`0x1400cd2d0`) build `rglInputTextFileStream`
  (vftable `0x1405651e0`), open `triggers.txt`/`simple_triggers.txt`, discard three header tokens
  (`triggersfile version 1`), and pass the stream to the manager.
- `rglStream` (160 B) holds `m_path` at `+0x8`, `m_cursor` at `+0x54`, and the whole file as
  `m_contents` at `+0x58`. `rglTextStream` (160 B) supplies the token readers but emits no
  standalone vftable. `rglInputTextFileStream` (176 B, vftable `0x1405651e0`) adds `FILE* m_file` at
  `+0xA0` and `bool m_closed` at `+0xA8`; `rglOutputTextFileStream` (176 B, vftable `0x1405653e0`)
  adds the same plus `m_fileOpen` at `+0xA9`. Reader slots: `+0x50 readStream`, `+0x58 readFloat`,
  `+0x60 readBool`, `+0x68 readUInt64`, `+0x70 readInt64`, `+0x78/+0x80` the folded 32-bit parser,
  `+0x88 readByte`, `+0x90 readString`.
- `rglTextStream` reader behaviour (v1.1.5.5 `ev_1e3c2566…0fdb0b`, 18/18 functions): `readFloat` is `strtod(token)` cast to
  float; `readBool` is `_stricmp("true"/"false")` else `strtol(token) != 0`; the integer readers use
  manual decimal loops (15-character cap, leading `-`); `readString` copies the raw token, with no
  quote handling. Writers append a single `0x20` after each value and write booleans as `0`/`1`.
  The portable reader parses strictly and rejects malformed tokens as a safety deviation.
- `mbStatementBlock::readStream` (`0x1401f6600`): reads `int32` statement count, allocates
  `count * 0x110`, then per statement reads `uint32` opcode, `int32` operand count, and that many
  `int64` operand tokens; sets every `m_endStatement` (`+0x108`) to `-1`, clears `m_analyzed`, and
  seeds `m_numLocalVariablesUsed` to `0x400`. **No bounds check** on either count.
- `mbStatementBlock` layout: `m_statements` `+0x0`, `m_numStatements` `+0x8`, `m_analyzed` `+0xc`,
  `m_numLocalVariablesUsed` `+0x10`, `m_luaHookRef` `+0x14`, `m_id` `+0x18` (96 bytes total).
- `analyze` (`0x1401f6740`): matches opcodes `3/4/5` and loop starters `6,7,0xb,0xc,0x10,0x11,0x12`
  with two 128-entry stacks, writes `m_endStatement` jump targets, caps nesting at 128, and computes
  `m_numLocalVariablesUsed` from operand tag `0x11`, clamped to `[0x20, 0x400]`.
- Operand encoding: 7-bit tag in bits 56–62, 56-bit payload. Tags `0` immediate, `1` global variable
  array `g_globalVariables` (`0x140662c30`, unchecked), `2` global `mbSlots` vector
  (`0x140662c18` begin / `0x140662c20` end / `0x140662c28` capacity, read bounds-checked),
  `0x11` local variable. Negative 64-bit values are untagged immediates.
- `mbStatement::parseOperand` (`0x1401c6070`) implements the decoder: `<= 0` is an immediate,
  otherwise tag = `(value >> 56) & 0x7f` and payload = `value & 0x00FFFFFFFFFFFFFF`.
- `mbStatement::returnValue` (`0x1401c6100`) treats operand 0 as the destination lvalue and writes
  the result to the global array (tag 1), `mbSlots::set` (tag 2), or the local array (tag `0x11`);
  other tags raise `"Illegal lvalue in statement"`.
- `mbStatement` constructor (`0x1401c6060`) only initializes `m_endStatement` to `-1`; `readStream`
  fills the opcode and operands.
- `mbStatement::execute` (`0x1401c6150`): parses operands, masks opcode with `0x0FFFFFFF`, calls
  `WSELuaManager::OnOperationExecute` when `operationHookTable[opcode] != -2`, then dispatches
  opcodes `30..2767` through the 2,738-entry RVA jump table at `0x1401e22f0` and opcodes `>= 2768`
  to `executeWSE`. Bit 31 inverts the result; bit 30 arms the stop latch (`g_skipStatement`,
  `0x14061eec4`).
- `docs/wse_opcodes.json` / `docs/WSE_OPCODES.md` / generated `wse_opcodes.h` cover all 440 SDK
  operations in the extended range 2800–5202. 431 are switch cases in `executeWSE`; the other 9 are
  equality-tested handlers at 2800, 3000, 3300, 3600, 3900, 4200, 4400, 4700, and 5000. No in-range
  SDK operation lacks a handler. The map is generated from the v1.1.5.5 pseudocode
  (`ev_2aaa96a845fb5e0466129d8f6726e8172c13981585db951f98d8235a415cf0f9`).
- The 2800–2809 group is proven by the pseudocode switch at lines 638–665 (`0xaf1`–`0xaf9`) and the
  equality branch at line 859 (`0xaf0`): `val_shr`/`store_shr`
  cast to signed before `>>` (arithmetic); `val_lshr`/`store_lshr` shift the `ulonglong` operand
  directly (logical); `val_shl`/`store_shl` shift left; `val_xor`/`store_xor` combine operands 0/1 or
  1/2; `val_not`/`store_not` complement operand 0 or 1. Every shift masks the count with `& 0x3F`.
- The 3000–3002 group is proven by the equality branch at line 628 (`register_get`) and the switch at
  lines 870–877: `register_get` checks `param_4[1]` with `mbCheckRegister`, then loads
  `(&g_globalVariables)[index]`; `register_set` checks `param_4[0]` and stores `param_3[1]` into the
  same bank. The bank is `g_basicGame+0x270` (`0x140662c30`, confirmed by
  `mov qword ptr [r14 + rcx*8 + 0x270], rax` at `0x1401e552d`). `store_wse_version` ignores its value
  operand: selector 0 yields 5, selector 1 yields `0x483` (1155), and any other selector uses the
  zeroed `r15` default (`0x1401e553a`–`0x1401e555b`).
- The script-context query group is proven by five separate handler sites:
  `store_current_trigger` (`0x1401e5917`) returns `param_6` only when it is `<= 0` (`cmovle`),
  and `mbStatement::execute` passes its `r8d` context argument as `param_6` at `0x1401e223e`/
  `0x1401e2262`; WSE's `GetCurrentTrigger` uses the same negative-context rule.
  `store_num_return_values` (`0x1401e5979`) reads `0x140698ae8`. `store_return_value`
  (`0x1401e5985`) accepts a 1-based index 1–16, requires it to be `<= 0x140698ae8`, and reads
  `0x140698af0 + (index-1)*8`; out-of-range/unset indices log at `0x14055afe8`/`0x14055b008` and
  return true without storing. `store_application_time` (`0x1401e63a4`) calls
  `FUN_1400b9690(5)` (QPC seconds since the app-time baseline), converts to float, multiplies by
  `1000.0f` (`0x140576b90`), and rounds through `rglRound64`. `store_cur_mission_template_no`
  (`0x1401ea6b2`) reads `[0x14061f078]+0x9ae8` or yields -1 when the pointer is null.
- The string group covers `4200`–`4224`, `4233`–`4234`, `4236`, `4238`, and `4240`–`4244`, proven by
  the equality branch at `0x1068` (`0x1401ed1bd`) and the switch cases decoded from the jump
  table at `0x1401ed2ef` (`0x1069`–`0x1094`). String
  operands resolve through `0x140219c70`: tag 0 is the writable 128-entry string register table at
  `0x140665030` (stride 0x48, assigned by `0x14030fd60` and copied by `0x140310970`), tag 0x16 is
  the local register bank (`[0x140621778]`, count `[0x140621784]`, stride 0x90), and tag 3 or an
  unknown tag is the global bank (`[0x140621770]`, count `[0x140621780]`, stride 0x90).
  `mbCheckStringRegister` (`0x1401c51f0`) bounds tags 3/0x16 by their counts, tag 0 by `0x7f`, and
  accepts unknown tags; tag-0 destination registers use `mbCheckRegister` (`0x1401c4e60`).
  `str_equals`/`contains`/`starts_with`/`ends_with` use `wcscmp` (`0x1401f86b0`) or `_wcsicmp`
  (`0x1401f86f0`) when operand 2 is nonzero; `str_contains` lower-cases both strings
  (`0x1401f8b40`) and treats an empty needle as not contained. `str_is_alpha/digit/whitespace`
  call `iswalpha`/`iswdigit`/`iswspace` (`0x140498854`/`0x140498860`/`0x14049886c`), scan the whole
  string when the index is absent or -1, and leave the initial true result on an out-of-bounds
  index. `str_index_of`/`str_last_index_of` take operand 3 as the start (default 0) and operand 4
  as the end (default length-1); `str_last_index_of` mirrors the retail loop, which searches again
  before testing `end <= start`. `str_get_char` returns a UTF-16 code unit or stores nothing when
  the index is out of bounds. `str_to_num` calls `_wtof` (`0x140498878`), scales by
  `DAT_140662bb8` (100) when operand 2 is nonzero, and truncates with `cvttsd2si`.
  `str_compare` (`0x1401ee042`) normalizes to `-1/0/1` via `0x1401f86b0` or tail-jumps to `_wcsicmp`
  (`0x1404989d4`) for the raw folded code-unit difference when operand 3 is nonzero. `str_split`
  (`0x1401ee178`) writes pieces into tag-0 registers (capped at `0x7f`), skips empty pieces on
  operand 4, honors `max` (operand 5) only when >5 operands are present, and avoids the retail
  infinite loop on an empty delimiter. `str_sort` (`0x1401ee3eb`) sorts via `0x140200c20`/
  `0x140200c00` and reverses copy-back when operand 3 is nonzero. `str_store_lower`/`upper`/`trim`/
  `replace`/`substring`/`reverse`/`join` and `4223`/`4224` call `0x1401f8b40`/`0x1401f8ac0`/
  `0x1401f8e20`/`0x1401f8d00`/`0x1401f8c40`/`0x1401f8710`/`0x1401f8e40`/`0x1404992b0`/`0x1401f90b0`
  and use `DAT_14055b3e0` (`u" "`) and `DAT_14055b3dc` (`u"_"`). `str_store_md5` (`0x1401ee956`)
  hashes UTF-8 (`0xfde9`) bytes via `CryptCreateHash(0x8003)` (`0x140330510`) and formats 16 digest
  bytes with `"%02x"` (`0x14056bca8`). `str_store_float` (`0x1401ef3a8`) formats `(double)fp[op1]`
  with `"%.*f"` (`0x14055b3e4`) via `0x140200c40`, defaulting negative or missing precision to `5`.
  `str_sanitize` (`0x1401ef436`) converts tag-0 operand 0 via `MultiByteToWideChar(0xfde9)`
  (`0x1403100d0`) and strips `u"{"`, `u"}"`, `u"<"`, `u">"` (`0x14055b3ec`..`0x14055b3f8`).
  `str_is_integer` (`0x1401ef55f`) trims via `0x1401f8e20`, rejects empty or bare `'-'`, and checks
  digits with `iswdigit` (`0x140498860`). `str_store_server_password_rcon` (`4238`, `0x108e`) maps in
  the jump table directly to the shared no-op exit `0x1401f49c1`. `str_regex_*` (`4240`–`4243`,
  `0x1401ef741`–`0x1401efbc0`) use ECMAScript `std::regex` over UTF-8 (`0x1400163b0`, `0x140200c70`,
  `0x140200db0`, `0x1401f92f0`, `0x140200df0`), taking `<start_string_register>` as operand 0 in
  `str_regex_get_matches` (`4242`); exceptions evaluate false/no-store. `str_decode_url` (`4244`,
  `0x1401efd4d`) calls `SHLWAPI.dll!UrlUnescapeA` (`0x140501818`, `dwFlags=0`) in `0x1403108b0`.
- Options flags `4300`–`4305` (`0x10cc`–`0x10d1`, `0x1401efff9`–`0x1401f003b`) get/set the booleans
  `verbose_casualties` (`0x1406173b7`), `cheat_mode` (`0x14061ef50`), and `realistic_headshots`
  (`0x14061ef39`) via `WseVmState::options`.
- The floating-point cases at lines 5192 and 7881–8380 index the 128-float array `DAT_140698718`
  (`g_basicGame+0x35d58`), adding `4401` `fld_str` (`0x1401f00c7`), which parses operand 1 with
  `atof` (`0x1404989cc`).
  `mbCheckRegister` (`0x1401c4e60`) is `cmp ecx,0x7f; ja` (unsigned 0–127); a failed check returns
  true without doing anything. The fixed-point scale (`g_basicGame+0x1f8`, `0x140662bb8`) is set to
  `0x64` (100) at `0x140070ffa` inside `mbBasicGame::initialize` (`0x140070f90`); `fld` divides by
  it, `fst` multiplies by it.
- `rglRound64` (`0x1402a1910`) calls `floorf` (`0x1404d7f78`), subtracts to get the fraction, and adds
  one only when the fraction is greater than `0.5f` (constant at `0x14057647c`). `rglAbs`
  (`0x1402a1850`) clears the sign bit; `feq`/`fge`/`fle` compare through it with `FLT_EPSILON`.
- `fclamp` orders its two bounds first (`min`/`max` of operands 2 and 3), then returns the lower bound,
  the value, or the upper bound. `fdiv`/`fmod` report `"Division by zero"`/`"Modulo by zero"` and
  leave the destination unchanged.
- The math call sites prove the argument order: `fpow` loads operand 2 into `xmm1` and operand 1 into
  `xmm0` at `0x1401f08db`/`0x1401f08e5` (`pow(base, exponent)`); `fatan2` does the same at
  `0x1401f0ca8`/`0x1401f0cb2` (`atan2(y, x)`); `fmod` loads operand 2 as the divisor and operand 1 as
  the dividend at `0x1401f09f5`/`0x1401f0a1e`. The one-operand helpers read `fp[operand 1]` and the
  shared tail at `0x1401f062e` stores `xmm0` through operand 0.
- `flog` (line 8201, `0x1401f0968`–`0x1401f099f`): fewer than three operands selects base 10
  (`mov ebx,0xa`); otherwise the base is the int32 literal in operand 2. A base below 2 jumps to the
  logger with `"invalid logarithm base %d"` and returns true without storing. The result is
  `log10(fp[operand 1]) / log10((float)base)`.
- WSE2 v1.1.5.5 keeps every existing opcode number and adds seven SDK operations, all present in the
  recovered dispatch: `party_get_current_terrain_variant` 3911 (line 5020),
  `scene_set_region_variant` 4517, `scene_set_height_fog` 4518, `scene_set_water_waves` 4519,
  `scene_set_flora_wind` 4520, `scene_rebake_sky_cube` 4521, and `scene_set_weather_direction` 4522
  (lines 8658–8725).
- `mbStatementBlock::analyze` (`0x1401f6740`): maintains two 128-entry stacks (`tryStack`, `startStack`) so `else_try` sets `endStatement = elseIdx` on preceding unresolved inner statements while keeping `startStack[sp - 1]` anchored at the opener until `try_end` resolves the opener, `else_try`s, and final branch statements; computes `m_numLocalVariablesUsed` clamped to `[32, 1024]`.
- `mbStatementBlock::execute` (`0x1401f69d0`): resets `g_skipStatement` once at block entry (`0x1401f6cab`), interpreter loop with `0x1C08`-byte frame, 128 × `0x38` loop frames (`mbLoop`), locals zeroing, block-level Lua hook, `call_script` recursion into `m_scripts[index] + 0x50` (stride `0xB0`, max depth 255), and special handling for opcodes `3/4/5/8/9`, `0x15/0x16/0x17`. In `startLoop`, failing initial `canLoop` jumps directly to `stmt.endStatement` (`0x1401f78ac`) so `endLoop` pops the loop frame cleanly.
- Native core ops & `mbBasicGame`: `mbBasicGame` (`0xadd3`, 221,616 B) stores `m_globalVariables` (`mbSlots`) at `+0x258` and `m_registers` (`__int64[128]`) at `+0x270` (`0x140662c30`), confirming operand tag `1` and WSE `register_{get,set}` share one register bank. Disassembly of core ops confirms `val_div` (`2108`) / `store_div` (`2123`) write sentinel `40000000` (`0x2625a00`) on division by zero and return `true`, `val_mod` (`2109`) / `store_mod` (`2119`) leave operand 0 unchanged on modulo by zero, and `val_clamp` (`2112`) clamps to half-open `[lower, upper)` (`upper - 1`).
- `WSELuaManager::OnOperationMgrExecute` (`0x14033d090`): fetches the hook from the Lua registry,
  pushes operands as integers, `lua_pcall(L, nargs, LUA_MULTRET, 0)`, and interprets the boolean
  result as an override or handled flag (`WSELuaManager::OnStatementExecute` at `0x14033cdd0`).
- `mbTriggerManager::readStream` (`0x140244810`): reads `m_numTriggers`, allocates
  `count * 0x100`, seeds all three `rglAppTimer`s per trigger with `m_defaultTimerNo` and
  `g_timersTotal[m_defaultTimerNo]` (`0x14061f030`), then calls `mbTrigger::readStream`.
- `mbTrigger::readStream` (`0x140244540`): three `readFloat` intervals (`+0x0/+0x4/+0x8`), the
  conditions block (`+0x10`), the consequences block (`+0x70`), then ids
  `"%s Trigger [%d] Conditions"` / `"%s Trigger [%d] Consequences"`.
- `mbSimpleTriggerManager::readStream` (`0x14020fe60`): the same shape with `count * 0x78` records
  and one timer each; `mbSimpleTrigger::readStream` (`0x14020fab0`) reads one float, the statement
  block, and formats `"%s Trigger [%d]"`.
- `mbGame::readTriggers` passes the prefix `"Game"` (`0x1405529c8`); `readSimpleTriggers` passes
  `"Simple"` (`0x1405529e4`).
- `mbTriggerManager::execute` (`0x140244cd0`) **inlines** the `mbTrigger` state machine; the PDB
  declares `mbTrigger::execute`, but no separate public symbol is emitted. The integer argument is a
  trigger event key: non-negative keys run interval triggers; negative keys match
  `interval_key(checkInterval)`. Confirmed call sites pass `-112` (exit status), `-105`/`-106`
  (ranged attack), `-26`/`-56` (death), `-27` (knockdown), `-28` (blow), `-110`/`-109` (movement and
  collision capsules), `-102` (turn), and `-113` (action sound); frame code passes `1` and `0x10`.
- Trigger statuses are `0` idle, `1` waiting for delay, `2` waiting for rearm. Timers advance in
  `g_timersTotal` units of 100000/second and elapsed is `(float)(now - value) / 100000.0`. Rearm
  expiry resets the check timer; idle conditions success starts the delay; delay expiry runs the
  consequences; a matching negative event key runs the consequences immediately.
- Savegames use `rglBinaryStream` (vtable `0x14055c780`, 30 slots). Writers store raw
  little-endian values (8 bytes for `writeInt64`, 4 for `writeInt32`/`writeFloat`, 1 for
  `writeBool`/`writeByte`); strings are an `int32` byte count followed by the bytes. There is no
  header, version, or checksum. `mbTriggerManager::writeSavegameStream` (`0x140244b60`) writes an
  `int32` count, then `int32` status and the `int64` check/delay/rearm timer values per trigger.
  `readSavegameStream` (`0x140244970`) reads the same order; when the file count is larger it calls
  `rglError` (`0x14032f450`) with `"Load error: Number of triggers in file: %d, exceeds %d"` (a
  message box that returns) and still reads and discards the surplus entries.
  `mbSimpleTriggerManager::readSavegameStream` (`0x14020ffa0`) reads one `int64` timer value per
  record with the analogous `"Load error: Number of simple triggers in file: %d, exceeds %d"`.
  The retail readers do not bounds-check; the portable reader rejects truncation.

## Blocked

| Blocker | Impact |
| --- | --- |
| No Android NDK/SDK, `adb`, or device | Native arm64 build and on-device validation cannot run; only host/static checks |
| REA not registered with OpenCode | REA/Ghidra tools are not exposed through MCP; CLI works directly from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` |

## Environment notes

- `%TEMP%` extraction directories are wiped on server restarts. Node 26, JDK 25, and Ghidra 12.1.4
  are now extracted to the persistent `C:\rea-tools` tree; the zips remain in `%TEMP%\opencode\tools`.
- Required environment for CLI runs:
  `JAVA_HOME=C:\rea-tools\jdk-25.0.5+8`,
  `GHIDRA_INSTALL_DIR=C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC`.
- `msdia140.dll` is now COM-registered system-wide (`regsvr32`, elevated) from
  `C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC\Ghidra\Features\PDB\os\win_x86_64\`. Ghidra can
  load the retail PDB directly; the earlier hanging `RegSvr32` dialogs were orphaned manual
  registration attempts and are resolved.
- `decompile-all` refuses a pre-created output directory; let the tool create it. Use `--resume`
  only when a previous export manifest exists.
- Each export re-runs full Ghidra auto-analysis (~15–20 min). Do not run two exports concurrently
  on this 8 GB machine; a second OpenCode session using Ghidra drives free RAM below 1 GB.

## Next concrete task

1. Transcribe the next `executeWSE` handler groups from `docs/WSE_OPCODES.md` beyond 93/440 (e.g., agent/party/mission/player/network ops after extracting their target layouts and globals), extending the host test gate as each group lands.
2. Expand entity/container iteration (`try_for_agents`, `try_for_parties`, `try_for_prop_instances`, `try_for_players`, `try_for_dict_keys`) and Lua 5.1 hook wiring once the underlying entity tables and `gameOperation` registry are reconstructed.
3. Run the `clang++ -target aarch64-linux-android29 -Wall -Werror` arm64 compile gate once an Android NDK is installed, and keep `AGENTS.md`, `ROADMAP.md`, `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json`, and this file current at each milestone.
