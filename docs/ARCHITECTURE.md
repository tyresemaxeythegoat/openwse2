# WSE2 Architecture (Reconstructed)

Status: Phase 2 (Phase 1 binary triage complete; live counts are in `STATUS.md`) — statement-block VM (`block_vm.h` + extended native dispatchers `native_ops.h`, `native_entity_ops.h`, `native_mission_ops.h`, `native_campaign_ops.h`, `native_campaign_ops_2.h`, `native_world_ops.h`, `native_world_ops_2.h`, `native_world_ops_3.h`, including `NativeDispatch`/`failOnUnimplemented`, `dispatch_native_non_switch`, live entity iterators `11/12/16/17`, `try_for_dict_keys`, and `array_sort_custom` callback), world/entity/skill/item/quest/site/party/mission/edit-mode/subsystem runtime state (`wse/game/{world_state,world_subsystem_types,world_campaign_helpers}.h`), pure STL helpers (`wse/core/{math,encoding,json}.h`), script statement parse, script manager, module text readers, trigger runtime, binary savegames, the 440-operation WSE extended opcode map, and the per-family handler headers under `wse/vm/ops/` listed in `STATUS.md` §3 recovered.
Last updated: 2026-11-14.

This document records only facts derived from the shipped x64 binary and its PDB, or from the public
WSESDK headers. Anything not yet proven by decompilation is marked **open**.

The evidence baseline is the official **WSE2 v1.1.5.5** release (2026-11-12). It supersedes the
2026-10-14 build that earlier revisions of this document cited; every address below refers to v1.1.5.5.

## 1. Evidence provenance

| Artifact | Method | Result |
| --- | --- | --- |
| `mb_warband_wse2_x64.exe` | REA workflow / Ghidra 12.1.4 | v1.1.5.5, 7,060,992 B, SHA-256 `d4e97d56981a6dab3de0348e3a0513ae200e7b5701b86732a4654b0aa4e29709`; PE32+ `amd64`; image base `0x140000000` |
| Imports | REA `inspect_pe_imports` (`ev_a4af0bca…87fc6`) | 21 static + 2 delay-load DLLs, 546 functions (482 static + 64 delay) |
| `mb_warband_wse2_x64.pdb` | `tools/pdb_symbols.py` (MSF 7.0 + TPI + DBI) | GUID `fccf5e8d-4952-4a53-aa13-ac8b7ad38cfc`, age 449, RSDS; **156,846 TPI records**; **15,363 sized class/struct/union/enum layouts** (`14,758` unique named types, `types_x64.json`) |
| PDB symbols | `tools/pdb_symbols.py --globals` with PE section mapping | **48,658 symbols** with VAs where applicable (`globals_x64.json`) |
| RTTI + vtables | `tools/pe_evidence.py`, `tools/vtables.py` | **279 pointer runs ≥ 4** in `.rdata`/`.data`; **207** identified by destructor symbol; **73** belong to `rgl*`/`mb*`/`WSE*` classes (+ `2` `Parser@mu` + `1` `?$WSEDynMultiArray@VrglTransform` = **76** curated in `docs/SYMBOL_MAP.json`; `vtables_x64.json`) |
| Public SDK | WSE2 v1.1.5.5 `WSE2 SDK` (`header_operations_addon.py`, Lua headers) | 479 addon operations total, **440** in the extended range 2800–5202; used only as an external cross-reference, not copied code |

The PDB is treated as authoritative structural evidence. The game was compiled without RTTI for its
own classes, so class identity comes from PDB public symbols and vtable first entries
(`??_E<Class>`, `??_G<Class>`, `??1<Class>`), not from `type_info` names.

REA/Ghidra toolchain used for these runs:

- Node `26.12.1`, JDK `25.0.5+8`, Ghidra `12.1.4`, all extracted under `C:\rea-tools` after repeated
  `%TEMP%` wipes.
- REA invoked from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` via `scripts/rea.mjs`.
- 7,086 PDB-derived function names seeded into Ghidra (`seeds.tsv`).

## 2. Type inventory

Counts of unique sized type names in v1.1.5.5 (`14,758` unique named types across `15,363` sized TPI records; earlier 2026-10-14 build had `14,877` records):

| Prefix | Count | Interpretation |
| --- | ---: | --- |
| `rgl*` | 383 | RGL platform layer (711 TPI records: strings, streams, renderer, widgets, resources) |
| `mb*` | 215 | Mount & Blade game/module layer (272 TPI records) |
| `WSE*` | 24 | WSE2 extensions (Lua manager, dynamic arrays, M&B net) |
| `bt*` | 393 | Bullet Physics (statically linked) |
| `FMOD*` | 25 | FMOD Ex audio |
| `TiXml*` | 19 | TinyXML |
| `Json*` | 36 | JSON parser/serializer (jsoncpp-style diagnostics) |
| `mu*` | 26 | muParser expression engine |
| `lua*` | 3 | Lua 5.1 C API bindings |
| `std::*` | 8,387 | MSVC STL (`std::unordered_map`, `std::vector`, `std::basic_string`) |
| `Concurrency`/`ATL` | 306 / 229 | MSVC runtime/concurrency and ATL |

This supersedes the earlier ROADMAP assumptions of Havok and LuaJIT: the binary links **Bullet
Physics**, **FMOD Ex**, **TinyXML**, and stock **Lua 5.1** (`lua5164.dll`, 103 imports).

## 3. Subsystem map

| Layer | Representative classes | Evidence |
| --- | --- | --- |
| Platform / RGL | `rglString`, `rglStream`, `rglRenderer`, `rglWindowManager`, `rglGraphicsContext`, `rglWidget`, `rglMesh`, `rglMaterial` | PDB layouts; 73 `rgl*`/`mb*`/`WSE*` vtables (+3 `Parser@mu`/`?$WSEDynMultiArray` = 76 in `SYMBOL_MAP.json`) |
| Module system | `mbScriptManager`, `mbScript`, `mbStatementBlock`, `mbTriggerManager`, `mbTrigger`, `mbSimpleTriggerManager`, `mbSimpleTrigger` | PDB layouts; public function VAs |
| Game | `mbGame`, `mbMission`, `mbMissionTemplate`, `mbAgent`, `mbParty`, `mbFaction`, `mbItem`, `mbTroop` | PDB layouts |
| Lua extensions | `WSELuaManager`, `gameOperation`, `gameConstTable`, `WSEDynMultiArray` | PDB layouts; `g_luaManager` |
| Physics | `bt*` (393 types) | PDB type inventory |
| Audio | `FMOD*` (25 types), `fmodex64.dll` (54 delay imports) | PDB + imports |
| XML / config | `TiXml*` (19 types), `rglConfig`, `rglSectionlessIni` | PDB + `module.ini` string refs |
| Networking | `WS2_32.dll` (37), `WINHTTP.dll` (11), `WININET.dll`, `urlmon.dll` | imports |
| Rendering | `d3d9.dll`, `d3dx9_43.dll` (17), `rglRenderer` (61,312 bytes, type `0xad68`) | imports + PDB |

## 4. Core layouts (PDB TPI evidence)

All offsets below are byte offsets on x64. `type_index` refers to the PDB TPI type stream.

### `rglString` — 72 bytes, type 0x35f5

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_cursor` | `int32_t` |
| 0x08 | `m_buffer` | `char*` |
| 0x10 | `m_bufferLength` | `int32_t` |
| 0x14 | `m_strLength` | `int32_t` |
| 0x18 | `m_staticBuffer` | `char[48]` |

This is a small-string-optimized string: 48 inline bytes (`m_bufferLength`), spilling to heap
(`reallocate` `0x14030e960`) once length >= 48. Tokenization (`getNextToken` `0x14030e810`) skips
bytes `<= 0x20` and advances `m_cursor` across runs `> 0x20`; `swapUnderscoresWithSpaces`
(`0x14030f240`), `swapSpacesWithUnderscores` (`0x14030f270`), decimal parsers
(`toInt64Stream` `0x14030f6d0` / `toUInt64Stream` `0x14030f7b0`), and savegame log `"-1"` sentinel
semantics are implemented in `engine/include/wse/core/rgl_string.h`.

`rglString::indexOf(const rglString&, int) const` (`0x14030f630`, 145 bytes, `.pdata` leaf, no
calls or globals) is the Phase 5 R0 (d) function. Disassembly (Capstone, local): it reads
`m_buffer` (`+0x08`) and `m_strLength` (`+0x14`) of both strings, sign-extends `start`, and
returns the first `p` in `[start, hay_len - needle_len]` where the bytes match, else `-1`
(`or eax, -1`). `start` is not range-checked. On the PC, the native function, the reference
interpreter (`wse/recomp/x86_interp.h`) and that model agree on the 512 cases of
`wse/recomp/r0_inputs.h` (FNV-1a `0x7aeac64f5cbff537`, `tools/r0_pc_reference.cpp`).

### `rglStream` — 160 bytes, type 0x1b18a

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | vftable | — |
| 0x08 | `m_path` | `rglString` |
| 0x50 | `m_autoFlush` | `bool` |
| 0x54 | `m_cursor` | `int32_t` |
| 0x58 | `m_contents` | `rglString` |

`rglStream` is an in-memory stream over an `rglString`, not a `FILE*` wrapper. Its PDB method set is
`readStream/readString/readInt32/readUInt32/readInt64/readUInt64/readByte/readBool/readFloat` plus
the matching writers and `open/tryOpen/close/isOpen/clear/freeMemory/reset/atEnd/flush`.

These concrete subclasses matter:

| Class | Size | vftable | Adds |
| --- | ---: | --- | --- |
| `rglTextStream` | 160 | inherited | token reader implementations (`readFloat/readInt64/readUInt32/readString`); no standalone vftable is emitted |
| `rglInputTextFileStream` | 176 | `0x1405651e0` | `m_file` `FILE*` at `+0xA0`, `m_closed` `bool` at `+0xA8`; vtable `+0x50..+0x90` are the `rglTextStream` readers |
| `rglOutputTextFileStream` | 176 | `0x1405653e0` | `m_file`/`m_closed`/`m_fileOpen` at `+0xA0/+0xA8/+0xA9`; vtable `+0x00..+0x48` are the `rglTextStream` writers; read slots are stubs |
| `rglBinaryStream` | 160 | `0x14055c780` | 30-slot raw savegame stream; no text parsing, no per-value header |

`rglInputTextFileStream::tryOpen` reads the file into `m_contents`; every reader then skips bytes
`<= 0x20`, takes the next token, and parses it. Module files (`scripts.txt`, `triggers.txt`,
`simple_triggers.txt`, `mission_templates.txt`, …) are whitespace-delimited decimal text. The
separate `rglBinaryStream` (vftable `0x14055c780`, 30 slots) is used for savegames, not module
files. The binary readers use the embedded `m_contents.m_cursor` at `+0x58` and buffer at `+0x60`;
`rglStream::m_cursor` at `+0x54` is the text-stream cursor. `readInt32` (`0x1402aebe0`),
`readByte` (`0x1402aec00`), and `readInt64` (`0x1402aec20`) sit at `0x20`-byte strides.

`rglOutputBinaryFileStreamWithChecksum` (vtable `0x140565b40`, `0x1402c8a00`–`0x1402c8c00`)
accumulates every written unsigned byte modulo `2^64` for savegame read-back verification in
`mbGame::save` (`0x1400e16c0`). Whole-savegame `.sav` framing writes magic `0x52445257` (`"WRDR"`),
format version `1174` (`0x496`, accepted range `[802..1174]` in `mbLoadSaveWindow::updatePanels`
`0x140113c10`), `g_moduleVersion` (`0x14061f0c8` checked against
`g_compatibleSavegameModuleVersion` `0x14061f0c0`), the `mbSavegame` header (`fillSavegameHeader`
`0x1400e1e30` / `mbSavegame::readStream` `0x1400c73b0`), and `mbGame::{writeStream,readStream}`
(`0x1400c9a90`/`0x1400c8520`), staged through `new_game.sav` and backed up to
`last_savegame_backup.sav`; this container codec is implemented in `engine/include/wse/game/savegame.h`
over `wse/io/binary_stream.h`.

Exact reader behaviour (v1.1.5.5 `ev_1e3c2566…0fdb0b`, 18/18 functions):

| Slot | Method | Behaviour |
| --- | --- | --- |
| `+0x58` | `readFloat` | `strtod(token, nullptr)` (0x14049b238), cast to float |
| `+0x60` | `readBool` | `_stricmp("true"/"false")`, otherwise `strtol(token,10) != 0` |
| `+0x68` | `readUInt64` (`0x1402afbe0`) | `rglString::toUInt64Stream` (`0x14030f7b0`): sums `(c - '0') * 10^k` from the last character with unchecked 64-bit `imul`/`add`, so decimals above `UINT64_MAX` wrap modulo 2^64 and no digit is validated |
| `+0x70` | `readInt64` | `rglString::toInt64Stream` decimal parser |
| `+0x78` | `readUInt32` | 32-bit decimal loop (same code as `readInt32`) |
| `+0x80` | `readInt32` | 15-character manual loop with a leading `-` |
| `+0x88` | `readByte` | same manual decimal loop, 8-bit |
| `+0x90` | `readString` | copies the raw token, no quote stripping |

Every reader first skips bytes `<= 0x20` and calls `rglString::getNextToken`; reading past EOF
raises `"Unexpected End of File while reading file: %s"` with `m_path`. Writers append the decimal
text and a single `0x20` separator; `writeBool` emits `0`/`1`. The retail parsers are lenient
(`strtod` accepts a numeric prefix, the integer loops accept non-digits), so the portable reader
parses strictly and rejects malformed tokens instead.

### `mbStatementBlock` — 96 bytes, type `0x1913c`

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_statements` | `mbStatement*` |
| 0x08 | `m_numStatements` | `int32_t` |
| 0x0c | `m_analyzed` | `bool` |
| 0x10 | `m_numLocalVariablesUsed` | `int32_t` |
| 0x14 | `m_luaHookRef` | `int32_t` |
| 0x18 | `m_id` | `rglString` |

Methods: `readStream`, `analyze`, `execute`, `startLoop`, `endLoop`, `canLoop`.

### `mbStatement` — 272 bytes, type `0xb91e`

| Offset | Member | Type |
| ---: | --- | --- |
| 0x000 | `m_opcode` | `uint32_t` |
| 0x004 | `m_numOperands` | `int32_t` |
| 0x008 | `m_operands` | `__int64[32]` |
| 0x108 | `m_endStatement` | `int32_t` |

Methods: `readStream`, `parseOperand`, `returnValue`, `execute`, `executeWSE`, `arrayCompare`.
This is the decoded instruction record inside `mbStatementBlock::m_statements`; `executeWSE` is the
entry point for WSE2-extended opcodes. `gameOperation` (8 bytes: `opcode` at 0, `flags` at 4) is the
dispatch-table entry type, and `gameConstTable` (56 bytes) holds a named `std::vector<gameConst>`.

### WSE extended opcode map

`mbStatement::executeWSE` (`0x1401e4dc0`) receives the three decoded operand arrays and handles every
opcode above the native jump table. The SDK's extended operation range is 2800–5202.
`docs/wse_opcodes.json`, `docs/WSE_OPCODES.md`, and the generated
`engine/include/wse/vm/wse_opcodes.h` map 440/440 in-range SDK operations in WSE2 v1.1.5.5 to their
handlers (431 switch cases plus 9 equality-tested cases), recording the pseudocode line and handler
extent for each. The map is generated from the `executeWSE` pseudocode export
`ev_2aaa96a845fb5e0466129d8f6726e8172c13981585db951f98d8235a415cf0f9` (8,844 lines). No in-range
SDK opcode is unknown or unhandled. The 440 include the seven operations v1.1.5.5 added without
renumbering: 3911 and 4517–4522.

The first portable handler group covers 2800–2809: `val_shr`/`store_shr` use an arithmetic right
shift, `val_lshr`/`store_lshr` a logical right shift, `val_shl`/`store_shl` a left shift,
`val_xor`/`store_xor` a bitwise xor, and `val_not`/`store_not` a complement. Shift counts are
`(byte)count & 0x3F`; `store_*` reads the source from operand 1 (shift count from operand 2) and
leaves operand 0 as the destination lvalue.

The register group covers `register_get` (3000), `register_set` (3001), and `store_wse_version`
(3002). The bank is the 128-entry int64 array at `g_basicGame+0x270` (`0x140662c30`, the same bank
addressed by operand tag 1); `register_get` stores the indexed value through operand 0 and
`register_set` writes operand 1 into the register named by operand 0, then returns without a store.
Both validate the index with `mbCheckRegister` (`0x1401c4e60`) and fall through to `return true` on
failure. `store_wse_version` ignores its value operand: selector 0 yields the WSE version 5, selector
1 the Warband build `0x483` (1155), and any other selector the zeroed default
(`0x1401e553a`–`0x1401e555b`).

The script-context query group covers `store_current_trigger` (3014), `store_num_return_values`
(3016), `store_return_value` (3017), `store_application_time` (3035), and
`store_cur_mission_template_no` (3600). `store_current_trigger` returns the statement's context
argument only when it is negative — the in-trigger marker — and 0 otherwise, matching
`WSEOperationContext::GetCurrentTrigger`. The return-value bank is 16 int64 slots at
`0x140698af0` with its count at `0x140698ae8`; `store_return_value` takes a 1-based index, logs
`"invalid script result index %d"` or `"unset result value %d"`, and returns true without storing
when the slot is out of range or unset. `store_application_time` rounds
`(float)DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f` through `rglRound64`; `FUN_1400b9690(5)` returns
QPC seconds since the app-time baseline and the scale at `0x140576b90` is `1000.0f`.
`store_cur_mission_template_no` reads the int32 at `[0x14061f078]+0x9ae8`, or -1 when the current
mission pointer is null.

The string group covers `str_equals` (4200), `str_contains` (4201), `str_starts_with` (4202),
`str_ends_with` (4203), `str_is_alpha` (4204), `str_is_digit` (4205), `str_is_whitespace` (4206),
`str_length` (4207), `str_index_of` (4208), `str_last_index_of` (4209), `str_get_char` (4210),
`str_to_num` (4211), `str_compare` (4212), `str_split` (4213), `str_sort` (4214),
`str_store_lower` (4215), `str_store_upper` (4216), `str_store_trim` (4217),
`str_store_replace` (4218), `str_store_md5` (4219), `str_store_substring` (4220),
`str_store_reverse` (4221), `str_store_join` (4222),
`str_store_replace_spaces_with_underscores` (4223),
`str_store_replace_underscores_with_spaces` (4224), `str_store_float` (4233),
`str_sanitize` (4234), `str_is_integer` (4236), `str_store_server_password_rcon` (4238),
`str_regex_match` (4240), `str_regex_search` (4241), `str_regex_get_matches` (4242),
`str_store_regex_replace` (4243), and `str_decode_url` (4244). The resolver at `0x140219c70` maps tag 0 to the writable
128-entry string register table at `0x140665030` (stride 0x48; modeled as `WseStringBanks::registers`,
assigned via `0x14030fd60` and copied out via `0x140310970`), tag 0x16 to the local string
registers (`[0x140621778]`, count `[0x140621784]`, stride 0x90), and tag 3 or an unknown tag to the
global registers (`[0x140621770]`, count `[0x140621780]`, stride 0x90). `mbCheckStringRegister`
(`0x1401c51f0`) bounds tags 3/0x16 by their counts, tag 0 by `0x7f`, and accepts unknown tags; a
failed check logs and returns true without an effect. Destination tag-0 registers validate through
`mbCheckRegister` (`0..127`). Comparisons use `wcscmp` (`0x1401f86b0`) or `_wcsicmp`
(`0x1401f86f0`) when operand 2 is nonzero; `str_compare` (`0x1401ee042`) returns `-1/0/1` from
`0x1401f86b0` on the case-sensitive path and tail-jumps to `_wcsicmp` (`0x1404989d4`) to return the
raw folded code-unit difference when operand 3 is nonzero. `str_contains` lower-cases both strings
(`0x1401f8b40`) and treats an empty needle as not contained. The `str_is_*` predicates call
`iswalpha`/`iswdigit`/`iswspace` (`0x140498854`/`0x140498860`/`0x14049886c`), scan the whole string
when the index is absent or -1, and leave the initial true result on an out-of-bounds index.
`str_index_of`/`str_last_index_of` take operand 3 as the start (default 0) and operand 4 as the end
(default length-1); the last-index handler mirrors the retail loop, which searches again before
testing `end <= start`. `str_get_char` stores a UTF-16 code unit and stores nothing on an
out-of-bounds index. `str_to_num` calls `_wtof` (`0x140498878`), multiplies by the fixed-point
scale (`DAT_140662bb8` = 100) when operand 2 is nonzero, and truncates to int64. `str_split`
(`0x1401ee178`) writes pieces into consecutive tag-0 registers from operand 1 (capped at `0x7f`),
skips empty pieces when operand 4 is nonzero, honors `max` (operand 5) only when more than five
operands are present, and returns the count via operand 0; as a safety deviation, an empty delimiter
stores the whole source once rather than spinning forever. `str_sort` (`0x1401ee3eb`) copies a range
of tag-0 registers, sorts via `0x140200c20` or case-insensitive `0x140200c00` (operand 2), and
reverses on copy-back when operand 3 is nonzero. `str_store_lower`/`upper` call
`MakeLower`/`MakeUpper` (`0x1401f8b40`/`0x1401f8ac0`); `str_store_trim` dispatches mode 0/1/2 to
`0x1401f8e20`/`0x1401f8d00`/`0x1401f8c40` (`iswspace`); `str_store_replace` calls `0x1401f8710`
(advancing past the replacement); `str_store_md5` (`0x1401ee956`) hashes UTF-8 (`0xfde9`) bytes via
`CryptCreateHash(0x8003)` (`0x140330510`) and formats 16 bytes with `"%02x"` (`0x14056bca8`) using
a portable RFC 1321 implementation; `str_store_substring` (`0x1401eea5a`) uses `0x1401f8e40` with
defaults `start = 0` and `length = end` (leaving destination unchanged if negative or out of bounds);
`str_store_reverse` calls `std::reverse` (`0x1404992b0`); `str_store_join` appends tag-0 registers
(`0x1401f90b0`) with an optional valid separator; `4223`/`4224` (`0x1401eeece`/`0x1401eee00`)
replace `u" "` (`0x14055b3e0`) with `u"_"` (`0x14055b3dc`) or the reverse; `str_store_float`
(`0x1401ef3a8`) formats `(double)fp[op1]` with `"%.*f"` (`0x14055b3e4`) via `0x140200c40`, defaulting
precision to 5 when absent or negative; `str_sanitize` (`0x1401ef436`) converts tag-0 operand 0 via
`MultiByteToWideChar(0xfde9)` (`0x1403100d0`) and removes `u"{"`, `u"}"`, `u"<"`, `u">"`
(`0x14055b3ec`..`0x14055b3f8`); `str_is_integer` (`0x1401ef55f`) trims (`0x1401f8e20`), rejects empty
or bare `'-'`, and verifies `iswdigit` (`0x140498860`); `str_store_server_password_rcon` (`4238`,
`0x108e`) branches directly to `0x1401f49c1` (`return true`, no-op); `str_regex_*` (`4240`–`4243`,
`0x1401ef741`–`0x1401efbc0`) run ECMAScript `std::regex` over UTF-8 (`0x1400163b0`, `0x140200c70`,
`0x140200db0`, `0x1401f92f0`, `0x140200df0`), with `str_regex_get_matches` taking `<start_string_register>`
in operand 0 and returning false/no-op on exception; and `str_decode_url` (`4244`, `0x1401efd4d`)
models `SHLWAPI.dll!UrlUnescapeA` (`0x140501818`, `dwFlags=0`) in `0x1403108b0`.

The options flags group (`4300`–`4305`, `0x10cc`–`0x10d1`, `0x1401efff9`–`0x1401f003b`) reads and
writes the booleans `verbose_casualties` (`0x1406173b7`), `cheat_mode` (`0x14061ef50`), and
`realistic_headshots` (`0x14061ef39`) via `WseVmState::options`.

The floating-point group covers `fld`, `fld_str`, `fld_pos_x`, `fld_pos_y`, `fld_pos_z`, `fst`, `fcpy`, `feq`, `fgt`, `flt`, `fge`,
`fle`, `fsub`, `fmul`, `fdiv`, `fmin`, `fmax`, `fclamp`, `fadd`, `fsqrt`, `fabs`, `fceil`, `ffloor`,
`fexp`, `fpow`, `fln`, `flog`, `fmod`, `fsin`, `fcos`, `ftan`, `fsinh`, `fcosh`, `ftanh`, `fasin`,
`facos`, `fatan`, `feval`, and `fatan2` (4400–4438, 100%). `feval` (`4437`, `0x1401e4dc0` line 8353)
expands its string operand via `mbBasicGame::parseString` (`0x1400720b0`, converting literal `'^'` at
`0x140072338` into `'\n'` while leaving expanded `{sN}` contents un-reparsed) before passing the string to
`mu::Parser` (`0x1a146`, 376 B; `SetExpr` `0x1400a9980`, `Eval` `0x1400ad5a0`, unary `'-'` `prINFIX=6` at
`0x1400a8310` below `'^'`, `log` and `ln` both bound to `logf` at `0x1400a6c20`, `rint` `floorf(x+0.5)` at
`0x1400a6c50`), with full-AST compilation preventing partial mutations on errors. `fld_str` (`4401`, `0x1401f00c7`) resolves
operand 1, calls `atof` (`0x1404989cc`), and stores `(float)` into `fp[operand 0]`, while `fld_pos_{x,y,z}`
(`4402`–`4404`, `0x1401f0157`–`0x1401f01ff`) load `o.{x,y,z}` from `rglTransform[128]`. The float register bank is the 128-entry float array
`DAT_140698718` (`g_basicGame+0x35d58`); `mbCheckRegister` (`0x1401c4e60`) accepts indices 0–127 with
an unsigned comparison.
`fld` divides the fixed-point operand by the scale at `g_basicGame+0x1f8` (`0x140662bb8`, set to 100 at
`0x140070ffa` in `mbBasicGame::initialize`);
`fst` multiplies by 100 and rounds with `rglRound64` (`0x1402a1910`), which is `floorf(x)` plus one
when the fraction is greater than 0.5. Comparisons go through `rglAbs` (`0x1402a1850`) and
`FLT_EPSILON`. The call sites at `0x1401f08db`/`0x1401f08e5`, `0x1401f0ca8`/`0x1401f0cb2`, and
`0x1401f09f5`/`0x1401f0a1e` pass operand 1 in `xmm0` and operand 2 in `xmm1`, proving
`pow(base, exponent)`, `atan2(y, x)`, and `fmod(dividend, divisor)`. `flog` (pseudocode line 8201,
`0x1401f0968`–`0x1401f099f`) reads the
literal base from the int32 operand array, defaults to 10 when there are fewer than three operands,
rejects bases below 2 with "invalid logarithm base %d", and returns true without storing.
Division/modulo by zero log and return true.

Additional reconstructed handler groups:
- **Extended VM state, timers, MT19937, and profiler** (`3015`, `3020`–`3022`, `3027`–`3028`, `3034`, `3040`–`3043`):
  `return_values` (`0x1401e5a50`) populates up to 16 slots at `0x140698af0` / count `0x140698ae8`;
  `mtsrand`/`mtrand` (`0x1401e5b78`/`0x1401e5b85`) wrap `rglMersenneTwister` (`0x6556`, 5,000 B, `0x1402a24f0`/`0x1402a25e0`)
  with exact unsigned `[min, max)` rejection and zero-draw `span == 0`; `get_time` (`0x1401e5ba2`) returns 32-bit truncated UTC or local epoch seconds;
  `timer_reset`/`timer_get_elapsed_time` (`0x1401e5dab`/`0x1401e5df5`) track 128 × 16-byte timer slots (`0x140667b18`) against 100,000 Hz `g_timersTotal` (`0x14061f030`, `op1 != 0 ? 0 : 2`) rounded via `rglRound64`;
  `set_random_seed` (`0x1401e62bb`, `DAT_140617d54`); and `profiler_{start,stop,is_recording,mark}` (`0x14034f7d0`/`0x14034ff40`/`0x1403508a0`) drive `WSEProfiling` (`0xaf0a`, 66,072 B, gated by `g_profiling` `0x1406b4890`).
- **Dictionary container and `try_for_dict_keys` (`0x12`)** (`3200`–`3222`, `0x1401e6b30`–`0x1402ab900`):
  backed by `WSEObjectManager` (`0x1b4d3`, 64 B, `0x14034e340`/`0x14034e3b0` LIFO free-ID reuse) and ordered `rglSectionlessIni` (`0x5264`, 16 B) maps,
  with `.wsedict` binary, `.ini`, JSON (`0x1402a97f0`/`0x1402a9f70` with `\uXXXX` + surrogate pairs), and URL-encoded JSON (`0x1402aaea0`/`0x1402ab370`: `'+'` -> `' '` before `UrlUnescapeA`, percent-escape outside unreserved bytes keeping literal `'+'` as `%2B`),
  `dict_get_pos`/`dict_set_pos` (`0x1402aa5a0`/`0x1402aa920`, 9 comma-separated `"%f"` floats `(o, f, u)` with `default_pos`/identity fallback and `orthonormalize`), `dict_load_dict` mode 0 no-op vs loader mode 0 clear-then-load, and `mbCheckPath` path sandboxing. `try_for_dict_keys` (`0x1401f7692`/`0x1401f7dde`) iterates sorted map keys into tag-0 `stringRegisters` via `LoopFrame`.
- **Dynamic arrays** (`5000`–`5022`, `0x1388`–`0x139e`, `0x1401e4dc0` equality line 6427 & switch lines 6802–7510):
  shares unified `WSEObjectManager` (`0x1b4d3`, 64 B) / `WSEObjectContainer` (`0x18774`, 16 B) / `WSEObjectType` (`0x18026`, 32 B) handles (`WseObjectManager`) with dictionaries, backing `WSEBasicDynMultiArray` (`0x163ff`, 48 B) and `WSEDynMultiArray<int/rglString/rglTransform>` (`0x66db`/`0x677c`/`0x681f`, 48 B) across 1–14 dimensions;
  `.wsearray` save/load (`0x1401c5910`/`0x1401c59f0`, with `mbCheckPath` sandboxing) serializes transforms as 9 floats `[o.xyz, f.xyz, u.xyz]` and calls `orthonormalize()` on load;
  `array_sort`/`array_sort_custom` (`WSEArraySortOptions` `0x18fb1`, 8 B) use bottom-up natural merge sort (`0x1401fdd80`, aborting cleanly without mutation after `n` double-passes), with `array_sort_custom` invoking comparator scripts via `wse_invoke_script_callback`;
  and `array_eq..array_le` (`0x1401f58a0`, `0x1401f5d30`–`0x1401f606b`) compare elements lexicographically (normalizing position copies and comparing all 12 floats `s, f, u, o`).
- **Position vector group** (`4100`, `4102`–`4104` at `0x1401ecd5a`, `0x1401ed0c1`, `0x1401ed10f`, `0x1401ed158`):
  operates on `rglTransform[128]` (`0x140663030`, 64 B, `0x34ac`, four `rglVector4` `0x32d9`: `s, f, u, o`), computing fixed-point length/dot/cross products and `position_get_vector_to_position` orientation (`rotateZ`/`rotateX`).
- **World entity slot predicates, agent/party/troop/item groups, scalars, config/skills/savegame MD5, and edit-mode groups** (`3003`–`3013`, `3026`/`3030`–`3031`/`3036`–`3038`/`3045`, `3340`–`3341`, `3601`/`3608`/`3610`/`3619`/`3622`, `3700`–`3709`, `3800`–`3813`, `3900`–`3911`, `4226`/`4231`–`4232`/`4235`/`4239`/`4245`–`4246`, `4600`–`4609`):
  backed by `WseWorldState` (`engine/include/wse/game/world_state.h` + `wse/game/world_campaign_helpers.h`), wired into `VmContext::world` and `WseVmState::world`. Covers all 11 `*_slot_gt` validators (`0..0xfffff` slots, active party/agent/mission-object, `player.status == 2`, `team 0..7`); `agent_{set,get}_action_speed_modifier` (`3340`–`3341`); `party_stack_get_experience`..`party_get_current_terrain_variant` (`3900`–`3911`); `mbTroop` point getters/setters, `m_flags` mask, packed 4-bit `m_skills[6]` + shared Ironflesh/Strength HP cap (`0x140246010`/`0x1402460b0`), and clamped `[0, 999]` proficiencies (`0x1400a61c0`); `mbItemKind` damage/type nibbles, armors/speeds, particle/blood color, `rglMetaMesh` `caller == -50` `cur_item_*` mesh/material helpers with `spacesToUnderscores` (`0x14030f270`), and horse skeleton validation; case-sensitive module settings/game variables (`0x1402a7da0`/`0x1402aa400`), skill `_desc` translations (`0x1401103d0`/`0x140213520`), `"sg%d%d.sav"` MD5 (`0x14024f4c0`); and `mbGame` edit-mode selection/highlight/mode/dialog queries + option flags (`0x14061ef1a`, `0x14061781a` default `true`, `0x14061efb8`).

Handlers that do not write operand 0 report
`handled_no_store`; failed comparisons report `condition_false`; invalid registers are successful
no-ops (`ignored`), matching the retail `return true` fall-through. Every other mapped opcode reports
`WseDispatch::unimplemented` until transcribed (tally in `STATUS.md` §3).

### `mbScript` — 176 bytes, type `0x1f7b8`

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_id` | `rglString` |
| 0x48 | `m_interval` | `float` |
| 0x50 | `m_statementBlock` | `mbStatementBlock` |

Methods: `readStream(rglStream&, int)`, `execute()`.

### `mbScriptManager` — 272 bytes, type `0xaeea`

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_scripts` | `mbScript*` |
| 0x08 | `m_numScripts` | `int32_t` |
| 0x0c | `m_mappedScripts` | `int32_t[64]` |

Methods: `readStream(rglStream&)`, `mapScripts()`. Global instance `g_scriptManager` at
`0x14069fac0`.

### `mbTrigger` — 256 bytes, type 0x77c9

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_checkInterval` | `float` |
| 0x04 | `m_delayInterval` | `float` |
| 0x08 | `m_rearmInterval` | `float` |
| 0x0c | `m_status` | `int32_t` |
| 0x10 | `m_conditionsStatementBlock` | `mbStatementBlock` |
| 0x70 | `m_consequencesStatementBlock` | `mbStatementBlock` |
| 0xd0 | `m_checkTimer` | `rglAppTimer` |
| 0xe0 | `m_delayTimer` | `rglAppTimer` |
| 0xf0 | `m_rearmTimer` | `rglAppTimer` |

Methods: `readStream(rglStream&, char const*, int)`, `execute(int)`, savegame read/write, `setTimer`,
`randomizeIntervals`.

### `mbTriggerManager` — 24 bytes, type 0x1fcfd

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_numTriggers` | `int32_t` |
| 0x08 | `m_triggers` | `mbTrigger*` |
| 0x10 | `m_defaultTimerNo` | `int32_t` |

Methods: `readStream(rglStream&, char const*)`, `readSavegameStream`, `writeSavegameStream`,
`setTimer`, `randomizeIntervals`, `reset`, `execute(int)`, `hasTrigger`, `addTrigger`,
`removeTrigger`.

### `mbSimpleTrigger` — 120 bytes, type 0x71a6

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_interval` | `float` |
| 0x08 | `m_statementBlock` | `mbStatementBlock` |
| 0x68 | `m_timer` | `rglAppTimer` |

### `mbSimpleTriggerManager` — 24 bytes, type 0x1e676

Same three-member shape as `mbTriggerManager` with `mbSimpleTrigger*` elements.

### Trigger load flow (pseudocode-confirmed)

`mbGame::readTriggers` (`0x1400ccf20`) and `mbGame::readSimpleTriggers` (`0x1400cd2d0`) both:

1. Build an `rglInputTextFileStream` (vftable `0x1405651e0`).
2. Resolve `getModuleFilePath("triggers.txt")` / `"simple_triggers.txt"` and call `tryOpen` (slot
   `+0xA0`). Failure logs `"Unable to open file %s"`.
3. Discard exactly three whitespace-delimited header tokens; EOF logs
   `"Unexpected End of File while reading file: %s"`.
4. Call the manager reader with the literal `"Game"` (`0x1405529c8`) or `"Simple"` (`0x1405529e4`).

`mbTriggerManager::readStream` (`0x140244810`):

1. Read `int32_t m_numTriggers` (slot `+0x80`).
2. Replace `m_triggers` with a `count * 0x100` array of `mbTrigger` (array cookie + constructor).
3. For each index, seed all three `rglAppTimer`s with `m_defaultTimerNo` and
   `g_timersTotal[m_defaultTimerNo]` (`0x14061f030`, `uint64[]`), then call
   `mbTrigger::readStream(trigger, stream, prefix, index)`.

`mbTrigger::readStream` (`0x140244540`) order:

1. `readFloat` ×3 → `m_checkInterval`/`m_delayInterval`/`m_rearmInterval`.
2. `mbStatementBlock::readStream` → conditions at `+0x10`.
3. `mbStatementBlock::readStream` → consequences at `+0x70`.
4. Ids via `rglFormat("%s Trigger [%d] Conditions")` into the conditions `m_id` and
   `"%s Trigger [%d] Consequences"` into the consequences `m_id`.

`mbSimpleTriggerManager::readStream` (`0x14020fe60`) is the same with `count * 0x78` records and one
timer per record. `mbSimpleTrigger::readStream` (`0x14020fab0`) reads one float at `+0x0`, the
statement block at `+0x8`, then sets the id `"%s Trigger [%d]"`.

Safety: both managers index `g_timersTotal` with an unchecked `m_defaultTimerNo`. The Android port
rejects an out-of-range index instead of reading past the array.

### Trigger runtime (pseudocode-confirmed)

`mbTriggerManager::execute(int)` (`0x140244cd0`) iterates `m_triggers` with stride `0x100` and
**inlines** the `mbTrigger` state machine. The PDB declares `mbTrigger::execute`, but no separate
public symbol is emitted.

The integer argument is a trigger event key, not a clock:

- `key >= 0` enables interval triggers. `mbGame::frameMove` passes `1`, `mbMission::frameMove`
  passes `0x10`, and the slow-motion branch passes `0x10001`; the value itself is only tested for
  sign.
- `key < 0` matches an event trigger. Confirmed call sites: `mbAgent::updateExitStatus` `-112`,
  `updateRangedAttackAction` `-105`/`-106`, `die` `-26`/`-56`, `knockDown` `-27`,
  `updateBlow` `-28`, `fillMovementCapsule` `-110`, `fillCollisionCapsule` `-109`, `turn` `-102`,
  `playActionSound` `-113`.

Timers advance in `g_timersTotal` units of 100000 per second:
`elapsed = (float)(now - stored) / 100000.0`.

Per trigger:

1. status 2 (rearm): if `rearmInterval <= elapsed(rearmTimer)`, set status 0 and
   `checkTimer.value = now[checkTimer.timerNo]`.
2. status 0 (idle):
   - If `checkInterval <= -0.1`, compare `interval_key(checkInterval)` with the key. When they match
     and the conditions block returns true, set status 2, `rearmTimer.value = now`, and execute the
     consequences immediately.
   - Otherwise, when `key >= 0` and `checkInterval <= elapsed(checkTimer)`, set
     `checkTimer.value = now`; on conditions success set status 1 and `delayTimer.value = now`.
3. status 1 (delay): if `delayInterval <= elapsed(delayTimer)`, set status 2,
   `rearmTimer.value = now`, and execute the consequences.

`interval_key` is `floor(x)` plus one only when `x - floor(x) > 0.5`; exact halves round toward
negative infinity.

`mbSimpleTrigger::execute` (`0x14020fb80`): if `interval < -1.0`, require
`interval_key(interval) == key`; otherwise require `key >= 0` and `interval <= elapsed(timer)`, then
latch `timer.value = now`. Execute the single block; the return value is ignored.
`mbSimpleTriggerManager::execute` (`0x1402101a0`) aborts its loop when `key > 0` is odd and a
type-`0x12` queued window is at the back of `mbGame` `+0x04b0` (`0x1402101d2..0x1402101f7`);
reconstructed in `wse/vm/block_vm.h` (`execute_simple_trigger_manager`) checking
`ctx.world.m5.enqueued_windows.back().first == game::kQueuedWindowMenuType` (`0x12`).

### Trigger savegames

`rglBinaryStream` (vtable `0x14055c780`, 30 slots; v1.1.5.5 writer/stream evidence `ev_8cc48986…1dab2e7` (9/9),
reader evidence `ev_f09662c6…22777` / `ev_2ddf7a62…ed9b52`):

- The stream stores raw values with no header, version, length, or checksum. PDB aliases share
  addresses: `+0x20` `writeInt64/writeUInt64`, `+0x30` `writeInt32/writeUInt32/writeFloat`,
  `+0x38` `writeBool/writeByte`, `+0x68/+0x70` `readUInt64/readInt64`, `+0x80`
  `readInt32/readUInt32/readFloat`, `+0x88` `readBool/readByte`, `+0x90` `readString`.
- `writeFloat` (`0x1402ae980`) copies four raw bytes into `m_contents`, `writeInt64`
  (`0x1402ae9d0`) eight, and `writeBool` (`0x1402aea30`) one. `readInt32` (`0x1402aebe0`) loads
  four raw bytes from `m_contents.m_buffer` (`+0x60`) at the embedded `m_contents.m_cursor`
  (`+0x58`) and advances by four; `readByte` (`0x1402aec00`) and `readInt64` (`0x1402aec20`) are
  the same pattern with widths one and eight. The x86-64 copies are little-endian.
- Strings are an `int32` byte length followed by the bytes with no terminator: `writeString`
  (`0x1402aeb10`), `writeCString` (`0x1402aea80`), and `readString` (`0x1402aec40`, which reads
  the length through slot `+0x80`). Trigger savegames do not use strings.
- The retail readers perform no bounds check. The portable reader rejects truncated input.
- `mbTriggerManager::writeSavegameStream` (`0x140244b60`): `writeInt32(count)`; per trigger
  `writeInt32(status)`, `writeInt64(checkTimer.value)`, `writeInt64(delayTimer.value)`,
  `writeInt64(rearmTimer.value)`.
- `mbTriggerManager::readSavegameStream` (`0x140244970`): `readInt32(count)`; if the file count
  exceeds the manager count, call `rglError` (`0x14032f450`) with
  `"Load error: Number of triggers in file: %d, exceeds %d"`. `rglError` formats the message,
  shows a message box, and returns, so the loop still reads status and the three 64-bit values for
  every file entry and discards surplus entries. Timer numbers are not saved.
- `mbSimpleTriggerManager::readSavegameStream` (`0x14020ffa0`): `readInt32(count)`, error
  `"Load error: Number of simple triggers in file: %d, exceeds %d"`, then one `int64` timer value
  per trigger. No write counterpart appears in the binary.

Portable reconstruction: `engine/include/wse/io/binary_stream.h` plus
`read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` in
`engine/include/wse/game/trigger.h`. `SavegameResult::count_exceeded` corresponds to the retail
message-box path while still consuming every entry; `negative_count` and `truncated` are safety
deviations for malformed input.

### `WSELuaManager` — 21,504 bytes, type 0xadaf

| Offset | Member | Type |
| ---: | --- | --- |
| 0x0000 | `luaState` | `lua_State*` |
| 0x0008 | `operationMap` | `std::unordered_map<std::string, gameOperation*>` |
| 0x0048 | `gameConstTables` | `std::vector<gameConstTable>` |
| 0x0060 | `gvarMap` | `std::unordered_map<std::string, int>` |
| 0x00a0 | `operationHookLuaRefs` | `int32_t[5300]` |
| 0x5370 | `tStart` | `std::chrono::system_clock::time_point` |
| 0x5378 | `game_fail_stack` | `std::vector<bool>` |
| 0x5398 | `luaContext` | `int32_t` |
| 0x539c | `luaStateIsReady` | `bool` |
| 0x53a0 | `user_dir` | `std::string` |

The `operationHookLuaRefs[5300]` array proves the extended opcode space has at least 5,300 slots,
all initialized to `-2` (`LUA_NOREF`) in `WSELuaManager::WSELuaManager` (`0x14033c3d0`).
Global instance `g_luaManager` at `0x14069a6c0`.

Runtime startup (`WSELuaManager::initialize @ 0x14033ca80`):
- Creates `lua_State*` via `luaL_newstate()`, installs path sandbox (`sandbox_path @ 0x140341740`,
  allowing `%storage%` / `%savegames%`, Warband read-only traversal up to module/game root,
  and restricting writes to `lua\`), opens Lua stdlibs, and registers `wse_require_loader @ 0x14034adf0`
  via `registerWseRequireLoader @ 0x140340fc0`.
- `loadOperations @ 0x14033dff0`: parses `<module>\lua\msfiles\header_operations.py` into `operationMap`
  with 8-byte `gameOperation` entries (`uint32_t opcode` + flag bits `0x1` = LHS / `lhs_operations` /
  `global_lhs_operations`, `0x2` = CF / `can_fail_operations`).
- `loadGameConstants @ 0x14033f630`: parses `header_*.py` and `ID_*.py` into `gameConstTables`
  (`56`-byte `gameConstTable` = 32B `std::string` name + `std::vector<gameConst>` where `gameConst`
  is `40` bytes: 32B string name + float value + padding).
- `loadGlobalVars @ 0x14033f400`: parses `<module>\variables.txt` into `gvarMap` (`0`-based index).
- `initLGameTable @ 0x140340920`: creates `_G.game`, registers 36 `_G_game_callbacks` (`execOperation`,
  `getOperationFlags`, `getReg`, `setReg`, `getGvar`, `setGvar`, `getScriptNo`, `getString`,
  `getStringId`, `getCurTemplateNo`, `getCurTemplateId`, `getNumTemplates`, `getTemplateId`,
  `addTrigger`, `removeTrigger`, `getNumTriggers`, `addItemTrigger`, `removeItemTrigger`,
  `getNumItemTriggers`, `addScenePropTrigger`, `removeScenePropTrigger`, `getNumScenePropTriggers`,
  `addPrsnt`, `removePrsnt`, `addPsys`, `removePsys`, `getMeshId`, `getNumMeshes`, `partiesIt`,
  `agentsIt`, `propInstIt`, `playersIt`, `hookOperation`, `unhookOperation`, `hookScript`, `fail`)
  plus 7 `_G_callbacks` (`lsdir`, `mkdir`, `rmdir`, `printStack`, `_print`, `log_rgl`, `getTime`),
  populates `game.const` via `addGameConstantsToLState @ 0x140341d60`, and loads embedded resource
  `LuaGlobals` (PE resource `256/105`: `bit`, `table.*`, `string.*`, `game.mt`, `game.op`,
  `game.reg`/`sreg`/`preg`/`gvar`, `game.const.mt`, `game.script`/`string`/`qstring`, `vector3`,
  `game.rotation`, `game.pos`).
- `doMainScript @ 0x140341f10`: executes `<module>\lua\main.lua` if present, then sets `luaStateIsReady`.
- Hook and event dispatch: `OnOperationExecute @ 0x14033cdd0` (returns execute/CF/LHS overrides),
  `OnOperationMgrExecute @ 0x14033d090` (script hook / `game_fail_stack` / param replacement),
  `lc_execOperation @ 0x140344630` (bypasses op hooks via `isLua=true`, allocates string/pos temp
  args downward from register `127`, handles `call_script` directly), `OnPostWorldTriggers @ 0x14033d3c0`,
  `onRglLogMsg @ 0x14033d500`, `onChatMessageReceived @ 0x14033d650`, `onGameLoad @ 0x14033d840`,
  `onSave @ 0x14033d920`, `onLoadSave @ 0x14033da20`, and `onConversation{Start,Line,End} @ 0x14033dbd0/33dc80/33dd70`.

WSESDK cross-reference: `header_operations.py` declares 1,535 operation names (1,489 unique numeric
IDs, 0–5,202), and `header_operations_addon.py` adds 477 declarations (472 unique). The maximum SDK
opcode (5,202) fits inside the 5,300-slot hook array, corroborating that the array is indexed by
opcode ID.

### `mbGame` — 94,560 bytes (`0x17160`, selected members)

| Offset | Member | Type |
| ---: | --- | --- |
| 0x0008 | `m_savegameName` | `rglString` |
| 0x0058 | `m_moduleName` | `rglString` |
| 0x00a0 | `m_modulePath` | `rglString` |
| 0x0188 | `m_classNames` | `rglString[648]` |
| 0x0420 | `m_mainPartyNo` | `int32_t` |
| 0x043c | `m_paused` | `bool` |
| 0x13df8 | `m_triggers` | `mbTriggerManager` |
| 0x13e10 | `m_simpleTriggers` | `mbSimpleTriggerManager` |
| 0x16ed0 | `m_editModeActive` | `bool` |
| 0x16ed4 | `m_editModeMode` | `int32_t` |
| 0x16f30 | `m_editModeDialogOpen` | `bool` |
| 0x16f34 | `m_editModeHighlightedMissionObjectNo` | `int32_t` |
| 0x16f38 | `m_editModeSelectedMissionObjectNos` | `rglVector<int>` |
| 0x16f50 | `m_editModeSelectionChanged` | `bool` |

Global instance `g_game` at `0x14061f070` (with `+8` pointer interior offset for `mbGame` globals); `g_mission` at `0x14061f078`; `g_missionTemplates` at `0x14061f068`.

### World/entity, skill, item, mesh, and mission PDB layouts (v1.1.5.5)

Verified in [SYMBOL_MAP.json](SYMBOL_MAP.json) and modeled in [world_state.h](../engine/include/wse/game/world_state.h):

| Class | Size (bytes / hex) | Key Verified Members (`offset member : type`) |
| --- | ---: | --- |
| `mbSkill` | 224 (`0x00e0`) | `0x0 m_description : rglString`, `0x48 m_name : rglString`, `0x90 m_id : rglString`, `0xd8 m_maxLevel : int32_t`, `0xdc m_flags : uint32_t` |
| `mbTroop` | 2,600 (`0x0a28`) | `0x128 m_flags : unsigned __int64`, `0x140 m_attributes : int32_t[4]`, `0x150 m_skills : uint32_t[6]`, `0x168 m_proficiencies : float[7]`, `0x188 m_hitPoints : float`, `0x18c m_skillPoints : int32_t`, `0x190 m_attributePoints : int32_t`, `0x194 m_proficiencyPoints : int32_t`, `0xa08 m_slots : mbSlots` |
| `mbItemKind` | 1,872 (`0x0750`) | `0x90 m_id : rglString`, `0x120 m_slots : mbSlots`, `0x1c0 m_variations : mbItemKindVariation[1280]`, `0x6e8 m_speedRating` ([world_state.h](../engine/include/wse/game/world_state.h)), `0x6ec m_missileSpeed : int32_t`, `0x6f0..0x6f8 m_{head,body,leg}Armor : int32_t`, `0x708 m_thrustDamage : int32_t`, `0x70c m_swingDamage : int32_t`, `0x738 m_bloodParticleSystemNos : int32_t[2]`, `0x740 m_bloodColor : uint32_t`, `0x748 m_skeletonModel : rglSkeletonModel*` |
| `mbItemKindVariation` | 80 (`0x0050`) | `0x0 m_meshName : rglString`, `0x48 m_flags : unsigned __int64` |
| `mbPartyTemplate` | 296 (`0x0128`) | `0x0 m_name : rglString`, `0x48 m_id : rglString`, `0x110 m_slots : mbSlots` |
| `mbFaction` | 1,472 (`0x05c0`) | `0x0 m_name : rglString`, `0x50 m_id : rglString`, `0xe0 m_slots : mbSlots` |
| `mbQuest` | 1,720 (`0x06b8`) | `0x170 m_id : rglString`, `0x6a0 m_slots : mbSlots` |
| `mbSite` | 11,232 (`0x2be0`) | `0x10 m_id : rglString`, `0x58 m_name : rglString`, `0x190 m_waterLevel : float`, `0x2b80 m_slots : mbSlots` |
| `mbParty` | 22,544 (`0x5810`) | `0x10 m_name : rglString`, `0x60 m_id : rglString`, `0x52d0 m_slots : mbSlots` |
| `mbnetPlayer` | 1,032 (`0x0408`) | `0x0 m_status : int32_t`, `0xe0 m_name : rglString`, `0x268 m_slots : mbSlots` |
| `mbAgent` | 38,432 (`0x9620`) | `0x30 m_status : int32_t`, `0xf8 m_slots : mbSlots` |
| `mbMissionObject` | 544 (`0x0220`) | `0x170 m_id : rglString`, `0x1f8 m_slots : mbSlots` |
| `mbSceneProp` | 152 (`0x0098`) | `0x0 m_id : rglString`, `0x48 m_flags : unsigned __int64`, `0x50 m_hitPoints : int32_t` |
| `rglResource` | 96 (`0x0060`) | `0x8 m_name : rglString`, `0x50 m_inUse : int32_t`, `0x54 m_fileNo : int32_t`, `0x58 m_flags : uint32_t` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglResourceManager` | 1,880 (`0x0758`) | `g_resourceManager` (`0x140623a40`); `0x0 m_resourceFiles : rglVector<rglString>`, `0x20 m_metaMeshes`, `0x38 m_meshes`, `0x470 m_textures`, `0x4a0 m_shaders`, `0x4d0 m_materials`, `0x500 m_bodies`, `0x518 m_skeletonModels`, `0x530 m_skeletonAnimations`, `0x548 m_physicsMaterials`; loaders `0x1402eedf0` / `0x1402eeec0` / `0x1402ef030` / `0x1402efe10` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglTexture` | 288 (`0x0120`) | Inherits `rglResource` (`0x0`); `0x64 m_type`, `0x6c m_width`, `0x70 m_height`, `0xc8 m_path : rglString`; device resolution via `tryGetTexture` (`0x1402f2f10`) + `initDeviceObjects` (`0x140317710`) across `.dds`/`.jpg`/`.tga` ([dds.h](../engine/include/wse/asset/dds.h)) |
| `rglShader` | 504 (`0x01f8`) | Inherits `rglResource` (`0x0`); `0x64 m_requirementFlags`, `0x68 m_numAlternatives`, `0x70 m_alternatives : rglString[...]`, `0x148 m_techniqueName : rglString`; stream reader `0x140301940` ([brf.h](../engine/include/wse/asset/brf.h), [shader.h](../engine/include/wse/asset/shader.h)) |
| `rglMaterial` | 632 (`0x0278`) | Inherits `rglResource` (`0x0`); `0x68 m_shaderName : rglString`, `0xb0 m_specularColor : rglVector4`, `0xc0 m_specularPower : float`, `0xd0 m_textureNames : rglString[...]` (`5` slots); stream reader `0x1402b5970` ([brf.h](../engine/include/wse/asset/brf.h), [shader.h](../engine/include/wse/asset/shader.h)) |
| `rglPhysicsMaterial` | 104 (`0x0068`) | Inherits `rglResource` (`0x0`); `0x60 m_soundOcclusion : float` (default `0.5f`; vftable `0x1405639a8`, [brf.h](../engine/include/wse/asset/brf.h)) |
| `rglBody` / `rglBodyPart` | 104 (`0x0068`) / 88 (`0x0058`) | `rglBody +0x60 m_bodyPart : rglBodyPart*`; polymorphic body variants `rglSphere` (`128 B`), `rglCapsule` (`160 B`), `rglQuad` (`304 B`), `rglManifoldBody` (`160 B`, `rglManifoldBodyFace` `40 B`), `rglCompositeBody` (`104 B`); stream reader `0x1402581a0` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglSkeletonModel` / `rglSkeletonModelBone` | 152 (`0x0098`) / 360 (`0x0168`) | `rglSkeletonModel +0x60 m_numBones`, `+0x68 m_bones`; `rglSkeletonModelBone +0x0 m_parentBoneNo`, `+0x8 m_transform : rglTransform`, `+0x88 m_name : rglString`; stream reader `0x14030d230` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglSkeletonAnimation` / `rglSkeletonAnimationBone` | 144 (`0x0090`) / 24 (`0x0018`) | `0x60 m_bones : rglVector<rglSkeletonAnimationBone>`, `0x78 m_positionIpo : rglIpo<rglVector3>`; bone `+0x0 m_rotationIpo : rglIpo<rglQuaternion>` (`0x14030c320` reads `qy, qz, qw, qx` in stream order); stream reader `0x14030c130` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglMesh` | 872 (`0x0368`) | `0x68 m_materialName : rglString`, `0xc0 m_materialColor : uint32_t`, `0xd8 m_faceCorners` (`rglMeshFaceCorner` `68 B`), `0x128 m_vertices` (`rglMeshVertex` `24 B`), `0x140 m_vertexGroups` (`rglMeshVertexGroup` `32 B`), `0x158 m_faces` (`rglMeshFace` `6 B`), `0x1a8 m_vertexKeys` (`rglMeshVertexKey` `48 B`); stream reader `0x1402b84b0` ([brf.h](../engine/include/wse/asset/brf.h)) |
| `rglMetaMesh` | 344 (`0x0158`) | `0x48 m_lods : rglMetaMeshLod[...]` (`rglMetaMeshLod` `24 B`), `0xc0 m_numLods : int32_t`, `0x110 m_meshName : rglString`; grouped by `createMetaMesh` (`0x1402f29a0`, `.lod[1-4]`) ([brf.h](../engine/include/wse/asset/brf.h)) |
| `mbMission` | 346,704 (`0x54a50`) | `0x98d8 m_teams : mbTeam[512]` (opcodes validate teams `0..7`), `0xa4f4 m_waterLevel : float`, `0x54a40` use-objects tooltip flags ([world_state.h](../engine/include/wse/game/world_state.h)) |

### Entity container lifetimes (Phase 2E)

Retail functions the containers in [world_state.h](../engine/include/wse/game/world_state.h) follow
(entry VAs, all curated in [SYMBOL_MAP.json](SYMBOL_MAP.json)):

| Container | Allocate | Remove | Notes |
| --- | --- | --- | --- |
| `rglHashVector<mbParty>` | `getFreeItem` `0x1400e8730` | `mbGame::removeParty` `0x1400dd990` | Monotonic keys; removed parties stay as tombstones |
| `rglStableVector<mbAgent>` | `createItem` `0x14014b960` | `remove` `0x14014b810`, `mbMission::removeAgent` `0x140138590` | LIFO slot reuse, trailing slots trimmed; fade-out removal checked in `mbAgent::frameMove` `0x14002a220` |
| `rglStableVector<mbMissionObject>` | `createItem` `0x14014c030` | `remove` `0x14014bdc0`, `mbMission::removeMissionObject` `0x140132ba0` | LIFO slot reuse, trailing slots trimmed |
| `mbnetPlayer` roster | `create` `0x1401b8980` | `clear` `0x1401b7580`, `mbnetData::disconnectPlayer` `0x14017c650` | Fixed capacity (`kMaxPlayers` 250) |

The campaign window queue is consumed by `mbMapWindow::frameMove` (`0x140117f90`); type `0x12`
opens `mbMenuWindow` (`open` `0x140120240`), which clears the rest of the queue.

## 5. Module/script entry points

Verified public symbols (mangled names preserve the exact calling convention):

| Address | Symbol | Role |
| --- | --- | --- |
| `0x1400ccf20` | `mbGame::readTriggers()` | loads `triggers.txt` into `m_triggers` |
| `0x1400cd2d0` | `mbGame::readSimpleTriggers()` | loads `simple_triggers.txt` into `m_simpleTriggers` |
| `0x1400dbde0` | `mbGame::executeScript(int,int,int)` | executes a module-system script |
| `0x1400dbe70` | `mbGame::executeMappedScript(int,int,int)` | mapped-script overload |
| `0x1400dbf00` | `mbGame::executeMappedScript(int,int,int,int)` | mapped-script overload |
| `0x1400dbfa0` | `mbGame::executeMappedScript(int,int,int,int,int,int,int)` | mapped-script overload |
| `0x14020f6a0` | `mbScript::readStream(rglStream&, int)` | parses one script |
| `0x14020f7c0` | `mbScriptManager::readStream(rglStream&)` | parses `scripts.txt` |
| `0x14020f9a0` | `mbScriptManager::mapScripts()` | builds `m_mappedScripts[64]` |
| `0x140244540` | `mbTrigger::readStream(rglStream&, char const*, int)` | parses one trigger |
| `0x140244810` | `mbTriggerManager::readStream(rglStream&, char const*)` | parses `triggers.txt` |
| `0x140244cd0` | `mbTriggerManager::execute(int)` | per-frame trigger evaluation |
| `0x14020fab0` | `mbSimpleTrigger::readStream(rglStream&, char const*, int)` | parses one simple trigger |
| `0x14020fe60` | `mbSimpleTriggerManager::readStream(rglStream&, char const*)` | parses `simple_triggers.txt` |
| `0x1402101a0` | `mbSimpleTriggerManager::execute(int)` | per-frame simple-trigger evaluation |
| `0x140341f10` | `WSELuaManager::doMainScript()` | Lua main-script hook |
| `0x14034a050` | `lc_hookScript(lua_State*)` | Lua C hook for script execution |

String evidence in `.rdata`:

| Address | Literal |
| --- | --- |
| `0x14054e8d8` | `scripts.txt` |
| `0x1405529b8` | `triggers.txt` |
| `0x1405529d0` | `simple_triggers.txt` |
| `0x14054c2c8`, `0x14055da09` | `module.ini` |
| `0x14055b890` | `Unrecognized opcode %d` |
| `0x14056d2a0` | `opcode %d out of range` |
| `0x14055c680` | `Script [%d] %s` |
| `0x14055c690` | `Unable to map script %s` |

### Pseudocode-confirmed parse flow (Ghidra 12.1.4)

`mbScriptManager::readStream` (`0x14020f7c0`, pseudocode `decompile-index.json` entry):

1. Calls stream vtable `+0x90` twice into a temporary `rglString` (the values are consumed but not
   stored by the manager). In the input text stream (`0x1405651e0`), `+0x90` is `readString`, so the
   discarded values are the `scriptsfile`/`version` header words.
2. Calls stream vtable `+0x80` twice into 4-byte integers; the second destination is
   `g_scriptManager.m_numScripts` (`0x14069fac8`). Both are read as 32-bit integers.
3. Deletes the old `m_scripts` array with destructors and `operator delete[]`.
4. Allocates `8 + m_numScripts * 176` bytes (array cookie + `mbScript[]`), constructs the array, and
   calls `mbScript::readStream(&m_scripts[i], stream, i)` for every index.
5. Calls `mbScriptManager::mapScripts()`.

`mbScript::readStream` (`0x14020f6a0`) reads, in order:

1. `readString` into `m_id` (stream vtable `+0x90`).
2. `readFloat` into `m_interval` at `+0x48` (stream vtable `+0x58`).
3. `mbStatementBlock::readStream` into `m_statementBlock` at `+0x50`.
4. Formats `"Script [%d] %s"` with the script index and `m_id`, then stores it in
   `m_statementBlock.m_id` at `+0x68` (`mbScript + 0x50 + 0x18`).

`mbScriptManager::mapScripts` (`0x14020f9a0`) walks a static 65-entry name table at
`0x140615ad0` and linearly matches each name against every script's `m_id`
(`m_id.m_buffer` at `+0x8`, `m_id.m_strLength` at `+0x14`). It writes the first matching script
index into `m_mappedScripts` (`0x14069facc`, 65 × `int32_t`), or `-1` when absent, and calls
`rglInfo` with `"Unable to map script %s"` on failure. All 65 names are transcribed in retail order
in `engine/include/wse/game/script.h`; the table starts `game_start`, `game_quick_start`,
`game_set_multiplayer_mission_end`, `game_get_console_command` and ends `wse_get_server_info`,
`wse_initial_window_start`.

Portable reconstruction: `engine/include/wse/game/script.h`. `read_script_manager` discards the two
header strings and the version int32, rejects negative/oversized counts, parses every record, and
calls `map_scripts`. Validated against the 3,467,512-byte retail `scripts.txt` sample: 1,755 scripts
parsed, every token consumed, 52 of the 64 names mapped (the 12 absent names are WSE-only scripts
not defined by that module).

### Statement VM entry points

| Address | Symbol | Signature (mangled-name evidence) |
| --- | --- | --- |
| `0x1401c6070` | `mbStatement::parseOperand` | `int64_t(int64_t*, int, int&)` |
| `0x1401c6100` | `mbStatement::returnValue` | `void(int64_t*, int64_t const&)` |
| `0x1401c6150` | `mbStatement::execute` | `bool(int64_t*, int, bool)` |
| `0x1401e4dc0` | `mbStatement::executeWSE` | `bool(int64_t*, int64_t*, int*, int*, int)` |
| `0x1401f58a0` | `mbStatement::arrayCompare` | `bool(int*, int*, cmp_operation)` |
| `0x1401f6600` | `mbStatementBlock::readStream` | `void(rglStream&)` |
| `0x1401f6740` | `mbStatementBlock::analyze` | `void()` |
| `0x1401f69d0` | `mbStatementBlock::execute` | `bool(int,int,int,int64_t*, mbCallStack*)` |
| `0x1401f7290` | `mbStatementBlock::startLoop` | `void(int64_t*, mbLoopManager&, int&)` |
| `0x1401f7910` | `mbStatementBlock::endLoop` | `void(int64_t*, mbLoopManager&, int&)` |
| `0x1401f7f00` | `mbStatementBlock::canLoop` | `bool(int64_t*, mbLoopManager&)` |
| `0x14033cdd0` | `WSELuaManager::OnStatementExecute` | Statement-level Lua hook (`ev_55263589…b114`) |
| `0x14033d090` | `WSELuaManager::OnOperationMgrExecute` | `bool(mbStatementBlock*, int&, int64_t*, bool&)` |

`WSELuaManager::OnOperationMgrExecute` is the bridge between the native statement VM and the WSE
operation manager. Its pseudocode confirms that it reads a function from the Lua registry
(`lua_rawgeti(L, LUA_REGISTRYINDEX, hookRef)`), pushes the parsed operands as integers, calls it with
`lua_pcall(L, operandCount, LUA_MULTRET, 0)`, and interprets the returned boolean as either an
override value or a "handled" flag.

### Statement encoding (pseudocode-confirmed)

Module files are **whitespace-delimited decimal text**, not binary records. `mbGame::readTriggers`
(`0x1400ccf20`) and `mbGame::readSimpleTriggers` (`0x1400cd2d0`) construct an
`rglInputTextFileStream` (vftable `0x1405651e0`), open `triggers.txt`/`simple_triggers.txt`,
discard three header tokens (`triggersfile version 1` in the retail files) with
`rglString::getNextToken`, then hand the stream to the manager reader.

`rglInputTextFileStream` derives from `rglTextStream`; both keep the 160-byte `rglStream` base and
the file stream adds `FILE* m_file` at `+0xA0` and `bool m_closed` at `+0xA8`. The readers are token
parsers, proven from the `rglInputTextFileStream` vtable `0x1405651e0`:

| Vtable slot | Method | Statement use |
| --- | --- | --- |
| `+0x58` | `readFloat` | trigger/script intervals |
| `+0x70` | `readInt64` | one operand |
| `+0x78` | `readUInt32` | opcode word |
| `+0x80` | `readUInt32` (folded `readInt32`) | operand/statement count |
| `+0x90` | `readString` | script id |

`mbStatementBlock::readStream` (`0x1401f6600`) therefore reads:

1. `int32_t` statement count (`m_numStatements` at `+0x8`).
2. For each statement: `uint32_t` opcode, `int32_t` operand count, then that many `int64_t` decimal
   operands.
3. `m_endStatement` at `+0x108` is initialized to `-1` by the constructor (`0x1401c6060`) and filled
   by `analyze`; `m_analyzed` is cleared and `m_numLocalVariablesUsed` is seeded to `0x400`.

The in-memory record remains 272 bytes (`uint32` opcode, `int32` count, 32 × `int64` operands,
`int32` end). The original performs **no bounds check** on either count, so a malformed file
overruns the 32-operand array or requests an unbounded allocation. The Android port rejects negative
counts, operand counts above 32, and statement arrays above its cap.

Operand words use a tagged 64-bit encoding (proven in `mbStatementBlock::execute`):

| Bits | Meaning |
| --- | --- |
| 0–55 | payload |
| 56–62 | 7-bit tag (`(value >> 56) & 0x7f`) |

A negative `int64_t` is treated as an untagged immediate (the full 64-bit value is used). Observed
tags:

| Tag | Meaning | Bounds behaviour |
| ---: | --- | --- |
| `0` | immediate value (also all values `<= 0`) | none needed |
| `1` | global variable array `g_globalVariables` at `0x140662c30`, 8-byte elements | **not checked** in the original |
| `2` | global slot vector `g_slots` (`mbSlots`, 24 bytes at `0x140662c18`) | read checks `m_values` size, out-of-range yields `0`; write goes through `mbSlots::set` |
| `0x11` | local variable `m_locals[index]` | **not checked** on read; `analyze` sizes the array from the maximum index |

`mbStatement::parseOperand` (`0x1401c6070`) implements the decoder exactly:

1. Load `m_operands[index]`.
2. If the value is `<= 0` (signed), leave it as an immediate and report tag `0`.
3. Otherwise tag is `(value >> 56) & 0x7f`, payload is `value & 0x00FFFFFFFFFFFFFF`.
4. Resolve tag `1` from the global array, tag `2` from `mbSlots` (bounds-checked), or tag `0x11` from
   the local array; return the payload unchanged for tag `0`.

`mbStatement::returnValue` (`0x1401c6100`) is the matching write-back path. It treats **operand 0**
as the destination lvalue (tag byte at `this+0xf`, index at `this+8`):

- tag `1` → write `g_globalVariables[index]`
- tag `2` → `mbSlots::set(index, value)` (grows the vector as needed)
- tag `0x11` → write `m_locals[index]`
- anything else → `rglError("Illegal lvalue in statement")`

This is how operation results become the input of a following statement.

`analyze` (`0x1401f6740`) computes the local-variable count by scanning operands (clamped to 32) for
tag `0x11` and storing `maxIndex + 1`, clamped to `[0x20, 0x400]`, in `m_numLocalVariablesUsed`.
It also resolves control flow: opcode `3` (`try_end`), `4` (`try_begin`), `5` (`else_try`), and the
loop starters `6, 7, 0xb, 0xc, 0x10, 0x11, 0x12` maintain two 128-entry stacks, and every covered
statement gets `m_endStatement` set to the matching exit index (unmatched entries get the statement
count). Nesting beyond 128 is a hard error.

### Opcode word and dispatch

`mbStatement::execute` (`0x1401c6150`) masks the opcode word with `0x0FFFFFFF`; the top bits are
modifiers:

| Bit | Effect |
| --- | --- |
| `0x40000000` (30) | arms the global "stop/return" latch (`DAT_1405d0d9c`) after execution |
| `0x80000000` (31) | inverts the boolean result |

Dispatch order:

1. Parse every operand with `mbStatement::parseOperand` into parallel `int64_t[]` and `int32_t[]`
   arrays.
2. If the statement is not WSE-disabled and `operationHookTable[opcode] != -2`, call
   `WSELuaManager::OnOperationExecute`; a hook may supply the return value.
3. Opcodes `30..2767` (`0x1e..0xACF`) dispatch through a 2,738-entry intra-function jump table of
   32-bit RVAs at `0x1401e22f0` (image base `0x140000000`, guarded at `0x1401c63e0` by
   `add esi, -0x1e; cmp esi, 0xab1`). `docs/native_opcodes.json` and `docs/NATIVE_OPCODES.md`
   (generated by `tools/extract_native_opcodes.py`) classify every slot against the public
   `header_operations.py` into dedicated `.text` switch handlers (`switch_handler`, including 3
   native handlers with no SDK constant, carried under community names), shared `return true` no-op stubs at `0x1401e226b` (`noop_true`), and
   `0x1401e223e` fallbacks into `executeWSE` (live tallies in `STATUS.md`).
4. Opcodes `>= 2768` (`0xAD0`) go to `mbStatement::executeWSE` (`0x1401e4dc0`), a 319 KB / 8,844-line
   pseudocode function with nine top-level switch sites and 431 `case` labels. This is the WSE2
   extended opcode implementation and the largest single reconstruction target.

### Block execution

`mbStatementBlock::execute` (`0x1401f69d0`) is the interpreter loop:

- Pushes a frame on the global call stack (`DAT_1405d0da0`); frame storage is `0x1C08` bytes with a
  128-entry loop stack of `0x38`-byte frames and a depth field at `+0x1C00`.
- Calls `analyze()` on entry, then zeroes `clamp(m_numLocalVariablesUsed, 0x20, 0x400)`
  qwords of locals when `rglConfig::General::bLocalVariablesZeroInitialization` (`0x140617819`,
  checked at `0x1401f6c59`, loaded from `[General] bLocalVariablesZeroInitialization` at
  `0x14026f716` via `rglIni::getBool` `0x1402a84d0` with `.data` default `true`) is enabled.
  `VmContext::clearLocalsBetweenCalls` defaults to `true` to match retail.
- If `m_luaHookRef` (`+0x14`) is not `-2`, calls `WSELuaManager::OnOperationMgrExecute` for a
  block-level Lua hook; a `false` return aborts the block.
- Special opcodes handled in the loop:
  - `1` `call_script`: resolves operand 0 through the tag decoder, bounds-checks the script index
    against `m_numScripts`, and recursively executes `g_scriptManager.m_scripts[index] + 0x50`
    (`mbScript` stride `0xB0`, statement block at `+0x50`). Call depth is capped at `0xFF`.
  - `3` `try_end` → `endLoop`; `4` `try_begin` pushes a loop frame; `5` `else_try` jumps to
    `m_endStatement` when the previous condition was true; `8` `break_loop` and `9` `continue_loop`
    scan the loop stack for the enclosing loop frame.
  - `0x15`, `0x16`, `0x17` are script-parameter access/store operations using `parseOperand` and
    `returnValue`; invalid parameter IDs log `"Invalid Script Parameter ID: %d"`.
  - All other opcodes `<= 0x14` route to `startLoop`.
  - Opcodes `>= 0x1E` go to `mbStatement::execute`.

Verified supporting layouts added to `docs/SYMBOL_MAP.json` (58 classes, 2,914 symbols, 76 vtables, 4,157 function ranges):

- `mbBasicGame` (221,616 B, type `0xadd3`): `m_globalVariables` (`mbSlots`) at `+0x258`,
  `m_registers` (`__int64[128]`) at `+0x270` (`0x140662c30`). Operand tag `1`, `store_lvalue`,
  and WSE `register_{get,set}` share this 128-entry bank.
- `mbCallStack` (3,096 B, type `0x715b`), `mbLoop` (56 B, type `0x1f89a`), and
  `mbLoopManager` (7,176 B, type `0x1eb6b`).
- Timer, math, INI, container-owner, array, expression-parser, and profiler layouts: `rglVector4` (16 B, type `0x32d9`),
  `rglTransform` (64 B, type `0x34ac`), `rglMersenneTwister` (5,000 B, type `0x6556`),
  `rglSectionlessIni` (16 B, type `0x5264`), `WSEObjectManager` (64 B, type `0x1b4d3`),
  `WSEObjectContainer` (16 B, type `0x18774`), `WSEObjectType` (32 B, type `0x18026`),
  `WSEArraySortOptions` (8 B, type `0x18fb1`), `WSEBasicDynMultiArray` (48 B, type `0x163ff`),
  `WSEDynMultiArray<int/rglString/rglTransform>` (`0x66db`/`0x677c`/`0x681f`, 48 B),
  `mu::Parser` (376 B, type `0x1a146`), `mu::ParserBase` (376 B, type `0x1a181`), and
  `WSEProfiling` (66,072 B, type `0xaf0a`).

Portable reconstruction lives in `engine/include/wse/vm/block_vm.h` and `engine/include/wse/vm/native_*.h`: `VmContext`,
`analyze_statement_block`, `execute_statement`, `execute_statement_block` (`DepthGuard` tracking `currentDepth` / preserving `currentCaller` + `wse_invoke_script_callback`), loop helpers
(`start_loop`/`can_loop`/`end_loop` including live entity iterators `11/12/16/17` and `core_op::kTryForDictKeys` `0x12`), and `821` switch native handlers across
`native_ops.h` (`33` core comparisons/shifts/math ops `30–33`, `2100–2133` + `112` Milestone 1 handlers across `entity_slots`, `positions_and_math`, `arithmetic_registers_and_vars`, `strings`, and entity helpers),
`native_entity_ops.h` (`108` Milestone 2 player/MP, quest, audio/UI/overlay, site/faction, troop, and party ops),
`native_mission_ops.h` (`66` Milestone 2 agent, team/mission combat, scene prop, and mission timer ops),
`native_campaign_ops.h` (`156` Milestone 3 campaign/world/troop/party handlers `36–1694`),
`native_campaign_ops_2.h` (`128` Milestone 3 agent/mission/item/party/face-key handlers `1695–2751`),
`native_world_ops.h` (`69` Milestone 4 handlers across input, ray/LOS, strings, troops, parties, face keys, and `53`/`2180`/`2749`),
`native_world_ops_2.h` (`69` Milestone 4 handlers across teams, audio/particles/tableau, and game_state_and_stores`), and
`native_world_ops_3.h` (`80` Milestone 4 `other_native` handlers) plus `dispatch_native_non_switch` (`473/477/1004/1070/1104/1302/1491`), `call_script`,
`store_script_param*`, `this_or_next`/`neg`, `dispatch_wse_statement` (forwarding `outTag` for
string operands and wiring `WseVmState` position, timer, RNG, profiler, object-manager, and script-callback state),
and VM-backed `execute_script`, `execute_mapped_script`, `execute_trigger`,
`execute_trigger_manager`, `execute_simple_trigger`, and `execute_simple_trigger_manager`.

## 6. External dependencies (import evidence)

| DLL | Functions | Kind | Port strategy |
| --- | ---: | --- | --- |
| `KERNEL32.dll` | 223 | static | Replace with C++20 `std::filesystem`, `std::thread`, POSIX/Android shims |
| `lua5164.dll` | 103 | static | Link Lua 5.1.x for arm64 (or a compatible 5.1-compatible VM); **not** LuaJIT |
| `fmodex64.dll` | 54 | delay | Replace with Oboe/SDL audio; FMOD Ex is not redistributable here |
| `USER32.dll` | 40 | static | Replace with SDL3 input and windowing |
| `WS2_32.dll` | 37 | static | BSD sockets (available on Android) |
| `d3dx9_43.dll` | 17 | static | Replace D3DX helpers (math, texture, shader) |
| `WINHTTP.dll` | 11 | static | Replace with `HttpURLConnection`/libcurl |
| `ADVAPI32.dll` | 10 | static | Registry/crypto shims; registry not used on Android |
| `steam_api64.dll` | 10 | delay | Exclude; no Steam on Android |
| `dbghelp.dll` | 8 | static | Optional crash reporting; replace with `backtrace`/Breakpad |
| `ole32.dll`, `OLEAUT32.dll` | 5 + 5 | static | COM shims only if required by SDK paths |
| `IMM32.dll` | 5 | static | IME/text input via Android `InputMethodManager` |
| `SHLWAPI.dll`, `SHELL32.dll` | 4 + 2 | static | Replace path/shell helpers |
| `WINMM.dll` | 3 | static | Timers -> `std::chrono`/SDL |
| `XINPUT1_3.dll` | 3 | static | SDL game controller API |
| `PSAPI.DLL`, `GDI32.dll`, `d3d9.dll`, `DINPUT8.dll`, `urlmon.dll`, `WININET.dll` | 1 each | static | Replace per subsystem |

### v1.1.5.5 image facts used by the recompiler (Phase 5 R0)

Measured locally with `wse/recomp/pe_image.h` (`tools/r0_pc_reference.cpp`) on the pinned exe
(7,060,992 B, SHA-256 `d4e97d56…4e29709`):

| Fact | Value |
| --- | --- |
| Preferred base / SizeOfImage | `0x140000000` / 8,232,960 B <!-- citation-check: ignore-line --> |
| `.text` virtual size | 5,242,293 B |
| `.pdata` functions | 14,618 (4,929,618 bytes of code) |
| Sections | `.text`, `.rdata`, `.data`, `.pdata`, `.tls`, `.rsrc`, `.reloc` |
| R0 decoder subset (`x86_decode.h`) | 837 of 14,618 functions decode completely (16,615 bytes): the rest need SSE, calls, prefixes or other opcodes (R1) |

## 7. Android port architecture (planned)

```
Kotlin/Java UI (SAF folder picker, touch overlay, settings)
        │ JNI
C++20 core (libwse2.so, arm64-v8a)
  ├─ rgl platform layer: rglString/rglStream/rglConfig (portable, no Win32)
  ├─ module loader: reads user-selected Modules/ via SAF file descriptors
  ├─ script/trigger VM: mbScriptManager + mbTriggerManager + mbSimpleTriggerManager
  ├─ WSE Lua layer: WSELuaManager + gameOperation table on Lua 5.1
  ├─ platform seam: wse/platform (Vfs, Clock, LogSink, Rhi, AudioDevice, Input; Sensor later)
  ├─ renderer: Vulkan RHI; driver chosen at runtime (system, PanVK on Mali, Turnip on Adreno)
  ├─ physics: Bullet (arm64 build) — imported type inventory already matches
  └─ audio: SDL3 audio stream + mixer
```

The slice plan for this stack is `docs/PLATFORM_PLAN.md`. GPU drivers are user-supplied at runtime
and never bundled. Existing `runtime/android` PanVK work remains valid for the renderer path: the loader deliberately
bypasses platform `libvulkan.so`, patches the ICD's `libhardware.so` dependency to `libpanvk_hw.so`,
and requires `extractNativeLibs=true`. It is independent of the reconstructed game code.

## 8. Evidence gaps / next measurements

1. ~~Portable savegame support: the binary `rglBinaryStream` slot semantics and savegame file framing
   (header/version) are documented only at the trigger-record level.~~ **Resolved (Phase 2A):**
   `mbGame::save` (`0x1400e16c0`), `fillSavegameHeader` (`0x1400e1e30`), `mbSavegame::readStream`
   (`0x1400c73b0`), `mbLoadSaveWindow::updatePanels` (`0x140113c10`), `rglOutputBinaryFileStreamWithChecksum`
   (`0x140565b40`), `mbNotes::{writeStream,readStream}` (`0x1401b9f30`/`0x1401b9e90`, scalar
   `+0x4c0`/`+0x4c4` written per entry of the 16-item loop), `rglHashVector<mbParty>::{readStream,writeStream}`
   (`0x1400e7b80`/`0x1400e7de0`, monotonic `m_nextKey` `+0x138f0`), and all `mbGame::{writeStream,readStream}`
   (`0x1400c9a90`/`0x1400c8520`) section codecs plus the four `[802..1174]` version gates
   (`mbGame::readStream` `0x1400c856d`, `mbParty::readStream` `0x1401ba9d1`/`0x1401baa20`/`0x1401baa7f`) —
   while `rglHashVector<mbParty>::readStream` (`0x1400e7b80`), `rglStableVector<mbEvent>::readStream`
   (`0x1400e7640`), and `mbEvent::readStream` (`0x1400bae70`) are verified version-invariant across
   `[802..1174]` (passing `version` only into `mbParty::readStream`) —
   are verified and implemented in `wse/io/binary_stream.h` and `wse/game/savegame.h`.
2. ~~`mbSimpleTriggerManager::execute` early-out depends on `mbGame` event-queue state~~ **Resolved (Phase 2C + 2A follow-up + 2E audit):**
   disassembly at `0x1402101a0` (`0x1402101d2..0x1402101f7`) shows the check tests positive odd
   event keys when the queued-window stack (`g_game` `0x14061f070` `+0x04b0`) back entry has type
   `0x12` (`mbMenuWindow`, enqueued by `jump_to_menu` `0x1401dc2b1`); `mbMapWindow::frameMove`
   (`0x140117f90`, `0x140118987..0x140118a80`) pops back entries only for `0x12` (`mbMenuWindow`,
   `mbMenuWindow::open` `0x140120240` clears remaining entries), `0x0f` (`0x16` presentation, `0x1401cbf02`),
   or `0x10` (`0x15` notes, `0x1401dc108`), leaving other queued types intact while syncing
   `g_gameScreen->m_windowStack` (`0x14069a560`, `mbGameScreen::isWindowOpen` `0x1400f0c50`), wired into
   `execute_simple_trigger_manager` (`wse/vm/block_vm.h`), `native_mission_ops.h`, `native_campaign_ops_2.h`,
   `native_ui_ops.h`, and `WseWorldState::consume_campaign_queued_window`.
3. ~~Locate `gameOperation` table construction in `WSELuaManager`.~~ **Resolved (2F):** Disassembly of
   `WSELuaManager::initialize` (`0x14033ca80`), `loadOperations` (`0x14033dff0`), `loadGameConstants`
   (`0x14033f630`), `loadGlobalVars` (`0x14033f400`), `initLGameTable` (`0x140340920`), `doMainScript`
   (`0x140341f10`), `lc_execOperation` (`0x140344630`), `OnOperationExecute` (`0x14033cdd0`),
   `OnOperationMgrExecute` (`0x14033d090`), and PE resource `LuaGlobals` (`256/105`) mapped all 36
   `game.*` C callbacks, 7 `_G.*` callbacks, `gameOperation` (`0x1`/`0x2` LHS/CF flags),
   `gameConstTable`, `sandbox_path` (`0x140341740`), and event callbacks; both `executeWSE` and the
   2,738-slot native jump table at `0x1401e22f0` are mapped in `docs/wse_opcodes.json` and
   `docs/native_opcodes.json`.
4. Confirm `rglConfig`/`module.ini` key names and defaults from string xrefs.
5. ~~Confirm renderer resource formats (`.brf`, `.dds`) from `rglMesh`/`rglMaterial` loaders before
   writing asset code.~~ **Confirmed (Slice P3):** PE/PDB layouts for `rglResourceManager` and all 8
   `.brf` resource types plus `rglTexture` / `.dds` texture path search are verified in
   `docs/SYMBOL_MAP.json` and implemented in `engine/include/wse/asset/{brf,dds}.h`.
6. Validate every reconstructed offset against decompiled member accesses before committing C++.
7. ~~Reconcile this document's own bookkeeping (`STATUS.md` §6)~~ **Resolved (B7):** §1–§3 and §9 now use
   consistent `*_x64.json` names and v1.1.5.5 counts/hashes (`73` `rgl*`/`mb*`/`WSE*` vtables,
   `+3` `Parser@mu`/`?$WSEDynMultiArray` = `76` curated in `SYMBOL_MAP.json`; `14,758` unique named
   types across `15,363` TPI records), generated cleanly with `tools/vtables.py`.
8. ~~Handler-site addresses cited only in `engine/include` source comments~~ **Resolved (B7):**
   `tools/build_symbol_map.py` exports `function_ranges`, `global_objects`, and `superseded_vas`,
   and `tools/check_citations.py` verifies all `1,494` unique header VAs (`1,714` VA refs) across `54` headers (plus REA `ev_*` IDs) in CI.
9. ~~Measure v1.1.5.5 module effect loading and material/shader technique resolution for P5.~~
   **Resolved (Slice P5):** PE/PDB analysis verified effect search & postFX loading
   (`rglEffectManager::initDeviceObjects` `0x14028bce0`, `rglGraphicsContext::checkLoadingPostEffects`
   `0x14029c930`, `rglRenderer::initPostFxShaders` `0x1402db690`, `D3DXCreateEffectFromFileA`
   import `0x140501b70`), material texture slot normalization (`initMaterialTextures` `0x1402f5390`),
   `wse2_shaders.brf` + `wse2_shaders.ini` fallback pairs (`loadOptionalShaderResource` `0x1402eeec0`,
   `initShaderPairs` `0x1402ed3c0`, `initShaderTechniques` `0x1402ecd30`), shader
   suitability/alternatives (`rglShader::isSuitable` `0x140301ab0`, `getSuitableShader` `0x1402f3db0`,
   `getSuitableShaderImpl` `0x1402f3eb0`, `getDefaultShader` `0x1402f1ef0`), and `VertexDeclType` /
   blend / alpha-test dispatch (`0x1402baa30`, `0x14029f590`, `0x1402a00b0`, `0x1402e7f50`), recorded
   in `docs/SYMBOL_MAP.json` and implemented in `engine/include/wse/asset/shader.h`.
   *Correction (P5 review):* this entry previously cited `0x1402a5a90` (no function at all) and <!-- citation-check: ignore-line -->
   `0x1402a5ef0`, `0x1402eec40`, `0x1402eeac0`, `0x1402f09c0`, `0x1402f2b30`, `0x1402f50a0`, `0x1402f0b50`, <!-- citation-check: ignore-line -->
   which fall inside unrelated functions (`rglTechnique::initDeviceObjects`, `loadResourceBlacklist`,
   `getFormatName`, `createMetaMesh`, `gammaCorrectMeshes`, `dumpUsageInfo`) rather than the
   functions they were labeled as. The citation checker accepts any address inside a known
   function, so it cannot catch a mislabeled interior VA; cite function entry VAs. Subsequent
   v1.1.5.5 verification confirmed `rglMesh::updateRenderOrder` (`0x1402b9e50`, `0x1402b9ef5`
   signed nibble `-8..+7`), per-draw blend `7` vertex-alpha dispatch (`0x1402e913f` /
   `0x1402b9ece`), `D3DRS_ALPHAREF` persistence on blended materials (`0x1402e8439`), the
   `0x20` shader specular gate (`0x1402e8454`), `FrontFace::counter_clockwise` (`0x1402e8ccd` /
   `0x14025ac20` / `0x140501b60`), and shader-managed gamma (`rglMesh::gammaCorrect`
   `0x1402c02b0`, no DX9 `D3DSAMP_SRGBTEXTURE` / `D3DRS_SRGBWRITEENABLE`).
10. ~~Audit v1.1.5.5 FMOD Ex audio call sites (`mbSoundManager`, `mbMusicManager`) for P6 replacement.~~
   **Resolved (Slice P6):** PE/PDB analysis verified `rglConfig::load` (`0x140264580`),
   `mbSoundSample::readStream` (`0x1403317c0`: 4-step `<module>/Sounds/languages/<lang>`,
   `<module>/Sounds`, `Sounds/languages/<lang>`, `Sounds` lookup; 3D min=`2.0f`/max=`80.0f`),
   `mbSoundSample::playSound` (`0x140331cb0`: 2D/looping priority `0`, else distance+nibble
   priority `[2, 255]`), `mbSoundManager::readStream` (`0x1403322e0`), `playSoundAtPosition`
   (`0x140332ac0`: `0x1000000` LOD fallback >`15m`, culture variant filter),
   `setChannelPropertiesAndPlay` (`0x140332d80`: volume nibble / `15.0f`, random start pos,
   freq variation), `setTransform` (`0x140332970`: `-u` forward, `+f` up), `initializeFmod`
   (`0x140332f80`: `64` audible / `100` virtual channels, `0.47f` distance priority factor),
   `updateOcclusionDistances` (`0x140333260`: `0.9`/`1.1` hysteresis around `80m`), `update`
   (`0x140333490`), `stopSound`/`getSoundProgress`/`stopAllSounds` (`0x140332690`/`0x1403326d0`/
   `0x140332770`), `mbMusicTrack::readStream` (`0x140290710`: `continue_flags |= flags`,
   culture fallback `0x3f`), `mbMusicManager` (`0x140290ad0`, `frameMove` `0x140290c40`,
   `startPlaying`/`stopPlaying`/`update` `0x140290e60`/`0x140290f20`/`0x140290f60`: `0.5s` tick,
   `5.0s` fade, `500.0f` recency weight), and callers (`mbAgent::playSound` `0x140057010`
   hashing `m_no`, `m_troopNo`, `m_partyNo` with `0.3f` scale and asymmetric `1/(1-delta)` negative
   pitch branch, keeping player agent at `1.0f`; `setChannelPropertiesAndPlay` `0x140332d80` having
   no random frequency jitter; `initializeFmod` `0x140332f80` mapping `bDistanceFilter` to
   `FMOD_INIT_DISTANCE_FILTERING` `0x200` and `bHrtfFilter` to `FMOD_INIT_HRTF_LOWPASS` `0x10`;
   `mbGame::playSound` `0x1400e3900`; `mbMissionObject::playSound` `0x140154340`), implemented
   without FMOD via [audio.h](../engine/include/wse/asset/audio.h),
   [platform/audio.h](../engine/include/wse/platform/audio.h),
   [world_audio.h](../engine/include/wse/game/world_audio.h), and `wse_audio` (`stb_vorbis` + `dr_mp3`).
11. ~~Reconstruct v1.1.5.5 input key tables and `mbGameKeyManager` binding semantics for P7.~~
    **Resolved (Slice P7):** Disassembly verified `rglInputManager::getKeyName` (`0x1402ad360`: `125`
    named DirectInput/mouse/gamepad raw keys across `1..255`), `g_gameKeyManager` (`0x14069fbd0`: `95`
    × 32-byte slots holding primary + alt `{key, num_modifiers, mod[2]}` chords),
    `mbGameKeyManager::reset` (`0x1400e96a0`: `75` built-in keys `MoveForward`..`TakeScreenshot`,
    `Left Ctrl` modifiers on `54..56`, `13` gamepad alt bindings `0xf0..0xff`, `Custom1..20`),
    `keyDown`/`keyClicked`/`keyRaised` (`0x1400eedf0`/`0x1400eee90`/`0x1400eef50`), `getKeyName`/
    `getKeyData` (`0x1400ef130`/`0x1400ef190`: `<mod0>+<mod1>+<key>`), `storeKeys`/`restoreKeys`/
    `setKey` (`0x1400ef3a0`/`0x1400ef460`/`0x1400ef510`), and `3100 game_key_get_key`
    (`0x1401e6a42`), implemented in [input.h](../engine/include/wse/platform/input.h),
    [world_input.h](../engine/include/wse/game/world_input.h), and
    [sdl_input.cpp](../platform/sdl/sdl_input.cpp).
12. ~~Reconstruct Phase 2A world/scene/skeleton loaders and Slice P9 Part 1 skeletal evaluation/skinning.~~
    **Resolved (Phase 2A completion + Slice P9 Part 1):** Verified `mbGame::readMap` (`0x1400d9a00`),
    `rglManifold::readStream` (`0x1402b1220`), `rglManifoldFace::readStream` (`0x1402b1190`),
    `rglManifold::computeBounds` (`0x1402b1d00`), `mbSite::readSceneObjFile` (`0x140210be0`, magic
    `0xfffffd33`, `mbMissionObject::readStream` `0x140152be0` `"spr_"` normalization & `rglTransform::readStream`
    `0x1402b5b20`, `rglAiMesh::readStream` `0x140252600`, `0xff4ad1a6` ground paint), and
    `mbGame::readSkeletons` (`0x1400cbb70` / `mbSkeletonManager::readFile` `0x140211ba0` /
    `mbSkeleton::readXMLElement` `0x1402119c0` / `mbSkeletonBone::readXMLElement` `0x140211480` /
    `rglSkeletonModelBody::readXMLElement` `0x14030ca30`, noting `dialog_states.txt` has zero runtime
    references in retail); verified `rglIpo<rglQuaternion>::readStream` (`0x14030c320`) wire order
    `(qx, qy, qz, qw)` into `rglQuaternion{w,x,y,z}` (`0x1402b65d0` `rglTransform::setRotation`),
    `rglQuaternion::interpolate` (`0x1402d6bb0`), `rglSkeletonModel::readStream` (`0x14030d230`),
    `rglSkeleton::getBoneTransformForAnimation` (`0x14030a770`), `rglBone::getSkinningTransform`
    (`0x140258800`), `rglMesh::updateSkinData` (`0x1402b9930`), and `rglSkeleton::skinMesh` (`0x14030b8f0`);
    implemented in [scene_loaders.h](../engine/include/wse/game/scene_loaders.h),
    [skeleton.h](../engine/include/wse/asset/skeleton.h), and
    [rhi_scene.h](../engine/include/wse/platform/rhi_scene.h).
13. ~~Reconstruct v1.1.5.5 bitmap font, localization, widget layout, and window/presentation flow for Slice P9 Part 2.~~
    **Resolved (Slice P9 Part 2):** Verified `rglFont::readFile` (`0x1402978c0`), `rglFont::drawText`
    (`0x140297fa0`), `rglFont::measureText` (`0x14029b4f0`), and `rglFont::drawGlyph` (`0x14029b670`:
    bottom-up `uv.y = (height - v + 4)/height`, `v_bottom = (height - h + padding - 6)/height`,
    `<c=0xRRGGBB>` forcing alpha `0xFF`, alignment flags `0x8`/`0x10`/`0x100`/`0x200`/`0x800`/`0x4000`/`0x10000`);
    verified `mbLanguageManager::readLanguageFiles` (`0x14028d6f0`) and `getText` (`0x14028e300`),
    `rglReadWidgetProperties` (`0x140328680`), `rglWidget::drawText` (`0x14031e5a0`),
    `rglWidget::setMeshStretched` (`0x14031d310`), `rglCreateImageWidget` (`0x14031ec20`),
    `rglCreateSimpleButtonWidget` (`0x14031f110`), `mbMenuWindow::open`/`frameMove`
    (`0x140120240`/`0x140121110`), `mbPresentationContainer::startPresentation`/`frameMove`
    (`0x1402097b0`/`0x140208ec0`), `mbPresentationWindow::frameMove` (`0x140174ed0`),
    `mbNotesWindow::frameMove` (`0x140161520`), and `mbGameScreen::setWindow` (`0x1400f0c80`);
    implemented in [xml.h](../engine/include/wse/io/xml.h),
    [font.h](../engine/include/wse/asset/font.h),
    [ui_runtime.h](../engine/include/wse/game/ui_runtime.h), and
    [rhi_ui.h](../engine/include/wse/platform/rhi_ui.h).
    The `afe751c` review fixes (presentation trigger parameters in `trigger_params`,
    `presentation_set_duration` countdown, overlay type / colour / size handling) cite no new
    retail VAs; their behaviour notes and port approximations are in
    [engine/README.md](../engine/README.md#ui-runtime-bitmap-fonts--rhi-widgets-p9-part-2) and the
    open items in [STATUS.md](../STATUS.md) §5.
14. ~~Find where `prsntf_manual_end_only` (`0x2`) is tested.~~
    **Resolved:** `mbPresentation::readStream` (`0x1401c3b80`) reads the id string, the flags
    (`readUInt32` into `+0x48`), and the mesh (`readInt32` into `+0x68`);
    `mbPresentationContainer::startPresentation` (`0x1402097b0`) copies the flags to container
    `+0x1c` and tests bit `0x1` (read-only, 10 s duration). The only test of bit `0x2` in `.text`
    (direct and register-loaded bit-test sweeps over both offsets) is in
    `mbConversationWindow::updatePresentations` (`0x1400888c0`): for each presentation number
    queued by `mbConversationWindow::startPresentation` (`0x1400888a0`, pushed onto the int vector
    at window `+0x2298`), it walks the container vector at `+0x2280` from the back and destroys
    every container whose presentation number (`+0x18`) differs and whose flags lack `0x2`, or
    whose number matches (restart), then creates and starts a new container.
    `mbPresentationWindow::frameMove` (`0x140174ed0`) closes only when the duration expires or a
    queued window replaces it; its vtable (`0x1405542b0`) has no key handler (folded empty
    stubs), and no `mbGameScreen` method special-cases window `0x16`. Escape therefore never
    closes a presentation in retail.
    `rglString::toUInt64Stream` (`0x14030f7b0`, behind `rglTextStream::readUInt64` `0x1402afbe0`)
    is recorded in §2's text-stream table.
15. ~~Check overlay size semantics against the retail overlay widgets.~~
    **Resolved (mostly):** `mbPresentationContainer::setOverlaySize` (`0x14020e0c0`) calls the
    widget's virtual `setScale` (slot `+0x1f8`). `rglWidget::setScale` (`0x14031e7e0`) stores the
    vector at `+0x14c`, sets the text size to `scale * 0.02` (`rglWidget::setTextSize`
    `0x14031d1c0`), and redraws the text (`rglWidget::redrawText` `0x14031dfa0`, which returns
    the `rglFont::drawText` extent plus the text offset). `rglImageWidget::setScale`
    (`0x1402a6bd0`) rescales the mesh by `new / old`. `createButtonOverlay` (`0x14020abe0`)
    builds an `rglSimpleButtonWidget` with text flags `(flags & 0x181c) | 0x200`; with `0x200`
    set and `0x100` clear, `rglWidget::setText` (`0x14031e330`) sets the upper-right corner to
    the laid-out text extent. `createGameButtonOverlay` (`0x14020c3e0`) and
    `createIngameButtonOverlay` (`0x14020c7d0`) build an `rglGameButtonWidget` with text flags
    `0x10`, meshes `medium_button` / `medium_button_down` (scaled `0.5556`) or
    `mp_inventory_button`, and a `0.148 x 0.04` / `0.2123 x 0.04` box stored as the initial scale;
    `rglGameButtonWidget::setScale` (`0x14029bfb0`) rescales both meshes by `new / old` and makes
    the new vector the box. Implemented in
    [ui_runtime.h](../engine/include/wse/game/ui_runtime.h) (`sync_overlays_from_world`).
16. ~~Find what `overlay_set_color` / `overlay_set_alpha` do to mesh overlays.~~
    **Resolved:** `setOverlayColor` / `setOverlayAlpha` (`0x14020e040` / `0x14020e060`) jump to the
    widget's `setColor` / `setAlpha` (slots `+0x208` / `+0x210`). `rglImageWidget::setColor`
    (`0x1402a6c50`) replaces the RGB bytes of its mesh's colour (`rglMesh+0xc0`, mesh at widget
    `+0x178`) and of the widget colour (`+0x240`); `rglImageWidget::setAlpha` (`0x1402a6c90`)
    replaces the mesh colour's alpha byte. `rglImageButtonWidget::setColor` (`0x1402a6a80`) does
    the same for both state meshes (`+0x198`, `+0x190`). `rglGameButtonWidget` and
    `rglSimpleButtonWidget` use `rglWidget::setColor` (`0x14031e890`), which does not touch a mesh.
    Implemented as `MeshDrawCall::tint_rgba` in [rhi.h](../engine/include/wse/platform/rhi.h).
17. ~~Trace how presentation overlays receive and react to pointer input.~~
    **Resolved (mostly):** v1.1.5.5 entry VAs below. Implemented in
    [ui_runtime.h](../engine/include/wse/game/ui_runtime.h) (`UiSession::handle_input`).

    | Function | VA | Finding |
    | --- | --- | --- |
    | `rglWindowManager::frameMove` | `0x14032a0d0` | left / right press -> widget states `0x10` / `0x40`, release -> `0x8` / `0x20`; sent to the focused widget, else the window tree |
    | `rglContainerWidget::triggerState` | `0x14028af30` | hover enter `0x2` / leave `0x4` to every visible, enabled child the cursor enters or leaves (any type); presses to children under the cursor top-down until one returns true; disabled or hidden children skipped |
    | `rglWidget::triggerState` | `0x14031dd10` | latches states through `rglWidget::setState` `0x14031d120` |
    | `rglWidget::checkState` | `0x14031d100` | tests and clears a latched state |
    | `mbPresentationContainer::frameMove` | `0x140208ec0` | per overlay, in index order: `0x2` -> `-63` (0), `0x4` -> `-63` (1), `0x10000` -> `-62` (value by `getType`: types 2 / 3 float value truncated, 5 / 11 / 12 / 14 int value, 6 / 7 text copied to `s0`, 9 / 10 toggle state when the toggle flag is set), then `0x10` -> `-64` (0), `0x40` -> `-64` (1), `0x100` -> `-64` (2); the wheel scrolls the type 8 scrollable nearest the cursor. `getType`: slider 2, combo button 5, text box 6, simple text box 7, scrollable 8, image button 9, game button 10, number box 11, list box 12, table 13, combo label 14 |
    | `rglSimpleButtonWidget::triggerState` | `0x140307770` | plain button arms on press, disarms on leave, raises `0x10000` on release while armed; toggle (flag `+0x284` bit 0) flips on press and raises `0x10000`; consumes left presses |
    | `createCheckBoxOverlay` | `0x14020bc50` | image button + toggle flag |
    | `createSliderOverlay` | `0x14020af30` | width 0.25, knob margin 0.008, horizontal, starts at min |
    | `rglSliderWidget::triggerState` | `0x14030d610` | knob tolerance margin + 0.01; while focused (dragging) every move sets the value and raises `0x10000` |
    | `rglSliderWidget::setAbsolutePosition` | `0x14030da60` | cursor position -> value |
    | `rglNumberBoxWidget::triggerState` | `0x1402c8310` | arrow press steps 1; hold repeats after 0.5 s every 0.1 s with step 1 / 3 / 10 / 50 (up to 2.5 s / 5 s / 7.5 s / beyond); clamped |
    | `rglComboLabelWidget::triggerState` | `0x140262cd0` | arrows step +-1 with wrap over the item count, always raise `0x10000` |
    | `rglComboLabelWidget::setIntValue` | `0x140262c70` | value setter used by the arrows |
    | `rglComboButtonWidget::triggerState` | `0x140261340` | press opens the popup when there are items |
    | `createTextOverlay` | `0x140209a90` | text flag bits 17-19 (`0xE0000`) create an `rglScrollableWidget` (`getType` `0x140300270`) |
    | `mbPresentationContainer::setOverlayValue` | `0x14020e180` | per-type value setter (reference) |

    Port approximations (not traced): number-box / combo-label arrows are the outer quarters of a
    fixed box; list boxes act like combo buttons (the combo popup is item 20); the middle-press (`0x100`)
    sender is untraced. Details in [engine/README.md](../engine/README.md).

18. Overlay animation, text box entry, and game-button mesh scale (P9 Part 2 review items 2, 4, 5).
    **Resolved:** v1.1.5.5 entry VAs below. Implemented in [ui_runtime.h](../engine/include/wse/game/ui_runtime.h)
    (`UiSession::step_presentation`, `native_ui_start_anim`, text box input) and [rhi_ui.h](../engine/include/wse/platform/rhi_ui.h)
    (game-button mesh draw). Details in [engine/README.md](../engine/README.md).

    | Function | VA | Finding |
    | --- | --- | --- |
    | `rglWidget::frameMove` | `0x14031c110` | per-frame animation step: `(start*(d-t) + target*t)/d`; colour bytes and alpha rounded half-up, size and position unrounded, target set when time is up |
    | `rglWidget::animateToColor` | `0x14031cbc0` | starts the colour channel toward a target |
    | `rglWidget::animateToAlpha` | `0x14031cc00` | starts the alpha channel toward a target |
    | `rglWidget::animateToScale` | `0x14031ccc0` | starts the scale channel toward a target |
    | `rglWidget::animateToPosition` | `0x14031cd00` | starts the position channel toward a target |
    | `rglTextBoxWidget::triggerState` | `0x140316bd0` | left press focuses the box; while focused it takes every press and character |
    | `rglTextBoxWidget::obtainFocus` | `0x140317250` | focus acquisition |
    | `rglTextBoxWidget::loseFocus` | `0x140317220` | commit on Enter, Escape, or a left press outside; raises `0x10000` |
    | `mbPresentationContainer::frameMove` | `0x140208ec0` | copies the committed text into string register 0 of `mbBasicGame` (`m_stringRegisters` at `+0x2670`, PDB layout) before `-62` |
    | `createGameButtonOverlay` | `0x14020c3e0` | `medium_button` meshes scaled uniformly by 5/9 (`0x3f0e38e4`); stores scale (0.148, 0.04); highlight text `0xff007700` (940) |
    | `createIngameButtonOverlay` | `0x14020c7d0` | `mp_inventory_button` kept at native size; stores scale (5.3075, 1.0) (`0x40a9d70a`); highlight text `0xff00ff00` (941) |
    | `rglGameButtonWidget::setScale` | `0x14029bfb0` | multiplies the meshes by new / stored scale per axis; the box stays the hit area |

    Port approximations (not traced): no IME composition (the 28-character cap and the focus
    commit are item 19; numeric mode and placeholders are item 20).
    Details in [engine/README.md](../engine/README.md).

19. Mission presentations, text box cap and focus commit, slider track, headless audio, and SAF rename (P9 Part 2 review items 1, 3, 5; P4 review item 6; P6 review items 1, 2).
    **Resolved:** v1.1.5.5 entry VAs below. Implemented in [ui_runtime.h](../engine/include/wse/game/ui_runtime.h)
    (`UiSession::update_mission_presentations`, text box limit and focus commit, slider track),
    [ui_frame.h](../engine/include/wse/platform/ui_frame.h) (input glue),
    [saf_vfs.h](../engine/include/wse/platform/saf_vfs.h) (rename / move), and
    [headless_runner.h](../engine/include/wse/game/headless_runner.h) (headless audio). Details in
    [engine/README.md](../engine/README.md).

    | Function | VA | Finding |
    | --- | --- | --- |
    | `start_conversation_presentation` (4907) | `0x1401f296e` | also queues the presentation for the mission side (`m6.ui.pending_conversation_presentations`) |
    | `mbConversationWindow::startPresentation` | `0x1400888a0` | pushes the presentation onto the pending list (`+0x2298`) |
    | `mbConversationWindow::updatePresentations` | `0x1400888c0` | per frame: each pending start ends same-presentation or non-`prsntf_manual_end_only` containers, then opens one and runs `-60`; every container runs `-61` and closes when its duration runs out |
    | `mbPresentationContainer::startPresentation` | `0x1402097b0` | copies the flags and sets the default duration: 10 s for read-only presentations, 100 s otherwise |
    | `getPresentationContainer` | `0x140088f00` | the conversation window's container for a presentation (used by the 902 mission branch) |
    | `presentation_set_duration` (902), mission branch | `0x1401cc016` | writes the duration through `m6.ui.mission_duration_requests` |
    | `is_presentation_active` (903), mission branch | `0x1401cc135` | true for any open conversation-window container of the presentation |
    | `Native` 900 | `0x1401cbf02` | queues only `0x0f`; no mission branch |
    | `rglTextBoxWidget` constructor | `0x140316900` | `+0x294` caps a text box at 28 characters (code points) |
    | `rglTextBoxWidget::loseFocus` | `0x140317220` | the previously focused box commits when `overlay_obtain_focus` (949) takes focus |
    | `rglTextBoxWidget::obtainFocus` | `0x140317250` | focus acquisition (reference) |
    | `rglSliderWidget::getPercentage` | `0x14030dae0` | knob position from the value (`slider_hor` track, `handle_hor` knob) |
    | `mbSoundManager::setTransform` | `0x140332970` | listener position from the camera transform (headless `--audio`) |

    Port approximations (not traced): mission containers draw in stack order and take input top
    container first, but a pressed container is not raised to the top; the
    mission pass runs once per frame and uses the frame's `dt` for its clock; the mixer runs on a
    port worker thread behind one mutex (`AudioMixerWorker`) instead of FMOD's; `wse_headless`
    links no audio codec. Details in
    [engine/README.md](../engine/README.md).

20. Number-box text entry, number-box range, combo popup item height, and shader warm-up (P9 Part 2 review item 5; P5 lowering review item 11).
    **Resolved:** v1.1.5.5 entry VAs below. Implemented in [ui_runtime.h](../engine/include/wse/game/ui_runtime.h)
    (`set_number_box_value`, `step_number_box`, numeric typing in `handle_input`, the combo popup)
    and [shader.h](../engine/include/wse/asset/shader.h) (`SpirvShaderCache::warm_up`, port
    infrastructure). Details in [engine/README.md](../engine/README.md).

    | Function | VA | Finding |
    | --- | --- | --- |
    | `rglNumberBoxWidget::updateWidgets` | `0x1402c8670` | the number box holds a `rglSimpleTextBoxWidget` (`+0x2e0`) with numeric mode `+0x298 = 1` and the box's range (`+0x25c` min, `+0x260` max) |
    | `rglTextBoxWidget::setIntValue` | `0x140316a10` | numeric mode: with `max > min` the value clamps to `[min, max - 1]` (max excluded, as the module header says) and the text becomes the decimal value; with `max <= min` the value is -1 and the text empty |
    | `rglTextBoxWidget::triggerState` | `0x140316bd0` | numeric typing: a digit sets `strtol(text) * 10 + digit` through `setIntValue` with the minimum forced to 0; backspace erases the last character; other characters are ignored; Enter / Escape / losing focus read the text back through `setIntValue`. A placeholder (`+0x292`) is cleared on the focusing press (empty text, colour `0xff000000`) |
    | `rglTextBoxWidget::rglTextBoxWidget` | `0x140316900` | `+0x292` / `+0x298` start cleared; only the character and terrain-generator windows and the number box set them, so module overlays never have a placeholder |
    | `rglNumberBoxWidget::triggerState` | `0x1402c8310` | arrows: `min(v + step, max)` / `max(v - step, min)` through `setIntValue`, then raise `0x10000` on the text box; `checkState` / `getIntValue` forward to the text box, so a committed typed value reaches `-62` |
    | `rglComboButtonWidget::openPopup` | `0x140261d10` | the popup starts at the box's bottom edge and grows downwards; each item is `size.y * scale.y` high (the combo box height) |

    Port approximations (not traced): the number box's text box is the middle half of the box
    (between the arrow quarters); typed values saturate where retail's 32-bit `value * 10 + digit`
    would wrap before the clamp; popups that would leave the screen are not moved or made
    scrollable (retail builds a scrollable popup there).

21. Placeholder audit: party size limits, troop skill levels, and four confirmed handlers.
    **Resolved:** v1.1.5.5 entry VAs below. Implemented in [block_vm.h](../engine/include/wse/vm/block_vm.h)
    (`troop_skill_level`, `party_max_companions`, `party_max_prisoners`) and
    [world_campaign_helpers.h](../engine/include/wse/game/world_campaign_helpers.h) (the
    script-less fallback). Details in [engine/README.md](../engine/README.md).

    | Function | VA | Finding |
    | --- | --- | --- |
    | `mbParty::getMaxNumCompanions` | `0x1401c1720` | mapped script `0x15` (party, 0) wins via the trigger result; else `iBaseCompanionLimit` (20) + leader leadership × `iLeadershipSkillBonus` (3) + leader charisma (`mbTroop` `+0x14c`, `m_attributes[3]`) |
    | `mbParty::getMaxNumPrisoners` | `0x1401c17a0` | mapped script `0x16` (party, 0) wins; else leader prisoner management × `iPrisonerManagementSkillBonus` (5) |
    | `mbGame::getTroopSkill` | `0x1400dcbf0` | level nibble capped by the skill's maximum; with the flag set, adds `game_get_skill_modifier_for_troop` (slot `0x1b`, trigger result zeroed first) and clamps to `[0, max level]` |
    | `mbGame::executeMappedScript` | `0x1400dbe70` | `(slot, param1, param2)`; returns whether the module defines the script |

    `store_skill_level` (2170, `0x1401dd8e0`) passes the flag too. A script started by a handler runs
    in a fresh frame, as `mbStatementBlock::execute` (`0x1401f69d0`) takes locals and loop frames
    from a per-call pool; the port's `VmContext::activeBlocks` puts such calls at `currentDepth + 1`.

## 9. Evidence index

REA evidence records. The first table is the current WSE2 v1.1.5.5 baseline; the second is the
superseded 2026-10-14 build, kept only to explain older citations.

**v1.1.5.5 (current)** — analysis profile digest `9b537ee6fdad293e1c8ea50e7b50d0b6b22972a83032bca093d279ab7e770ce4`:

| ID | Operation | Notes |
| --- | --- | --- |
| `ev_2aaa96a845fb5e0466129d8f6726e8172c13981585db951f98d8235a415cf0f9` | `decompile_all --address-start 0x1401e4dc0 --address-end 0x1401e4dc0` | 1 procedure written, 0 failures; 8,844-line `executeWSE` pseudocode from which 440/440 opcodes are mapped |
| `ev_7641bf02b4fb1b7a257143d21ebbc3db30c63508b4e651c54f7df703201f8413` | `decompile_all --name mbStatement` | 0 selected, 20,268 excluded: the new PDB names are not seeded into Ghidra, so name search cannot be used; address ranges are required |
| `ev_e323351db6dcd0c5ee6971adff8ece53dd20436dc59b424d88e46d882168cd0a` | v1.1.5.5 `binary_overview` | 9 segments, 20,276 procedures, 7,448 strings (`ev_fa7fc7d68d7977c7c161b00f64de44258e1e974d7358980b949cb2aa2f04d2e4`) |
| `ev_a4af0bca5d107b626454d91f4c69c2aef5d4f21b466cbb174333d7c8cd687fc6` | v1.1.5.5 `inspect_pe_imports` | 21 static + 2 delay DLLs (23 total), 482 + 64 = 546 functions |
| `ev_3ebcea855452bcdf0c3325185a7803f0ab38d671cdbdae50289c5be298015986` | v1.1.5.5 `stmt_core` (`0x1401c5f90`–`0x1401c6150`) | 4/4 procedures (`mbCallStack::print`, `parseOperand`, `returnValue`, `execute`) |
| `ev_ffc38aa54dea8de3e505a585b91ac665291570c4257be680c2ea8f20da2cb4d8` | v1.1.5.5 `stmt_block` (`0x1401f6600`–`0x1401f7f00`) | 10/10 procedures (`readStream`, `analyze`, `execute`, `startLoop`, `endLoop`, `canLoop`, …) |
| `ev_8d37a0c8ff4172b27be70bc38d0a325287abd882f115359535ef213a2601e995` | v1.1.5.5 `script` (`0x14020f6a0`–`0x14020f9a0`) | 4/4 procedures (`mbScript` / `mbScriptManager`) |
| `ev_3157906d86eb2e91fde12ab398ac456985e8ddc8b446b09c38bc04ae85fdfcfb` | v1.1.5.5 `simple_trigger` | 12/12 procedures (`mbSimpleTrigger::execute`, manager, serialization) |
| `ev_f62843ac4a6fc192dce93d0c44b029607a38d68ecaa72909b908275472a0a688` | v1.1.5.5 `trigger` | 12/12 procedures (`mbTriggerManager::execute`, serialization) |
| `ev_fc9bd1a4a59df50e5043ba9e6cc9a8e8abe92e0f206060e19e569d623f54193d` | v1.1.5.5 `game_scripts` (`0x1400dbde0`, `0x1400dbe70`) | 4/4 procedures (`executeScript`, `executeMappedScript`) |
| `ev_052f2d86c898108ec8770657941dd3034c6860ffa565dea31236c1a546509c96` | v1.1.5.5 `game_triggers` | 2/2 procedures |
| `ev_8cc48986006b9c7e64b7e7b94e4b37bb06c9950ecba367a50b0b601381dab2e7` | v1.1.5.5 `streams_bin` | 9/9 `rglBinaryStream` procedures |
| `ev_1e3c25667502f1b1e6ce2e296ecc792c1cd592a4e85440bf9b7e535e8e0fdb0b` | v1.1.5.5 `streams_txt` | 18/18 `rglTextStream` procedures |
| `ev_f09662c6d0cc3ac83fd7d6958bae56ed569f98784c32065e6d90a9a0f6d22777` | v1.1.5.5 `read_bytes` (`0x1401e22f0`) | binary byte reader + `0x140615ad0` (`ev_2ddf7a62e571269d791d6ec829d50b48c3c15367d99dec63005158fe42ed9b52`) |
| `ev_b1f807d9…bada` / `ev_a267813e…5bba` / `ev_55263589…b114` / `ev_266b9f4c…5351` / `ev_94c53440…ec9a` / `ev_bd739bd0…056e` | v1.1.5.5 singles | `mbBasicGame::initialize` (`0x140070f90`), `mbSlots::set` (`0x1402183d0`), `WSELuaManager::OnStatementExecute` (`0x14033cdd0`), `OnOperationMgrExecute` (`0x14033d090`), `rglAppTimer::reset` (`0x140318970`), `0x1403189e0` |

### Superseded 2026-10-14 evidence IDs (retained for historical traceability only)

| Evidence ID | Command / Query | Notes |
| --- | --- | --- |
| `ev_1665f9ad51202515ef1aacbb7bf7b387cca77cbc8d89b9f1aafe2a938dd5db43` | `binary_overview` (superseded) | 19,759 procedures, 7,157 strings |
| `ev_db4cc10aa916c9f0b08683c292a14a086adf2d721408cc587a76f6e98879fd70` | `inspect_pe_imports` (superseded) | 23 DLLs, 542 functions |
| `ev_7cd99ccc2639d6a0c4aab91f8cf6b1c8bd64971ccab2c1b77e8c0e8d8c157d7a` | `decompile_all --name mbScript` (superseded) | 5 procedures, 0 failures |
| `ev_8d91b1752f2c9586db78cc7c89b816dabba31faef0df0f5bd42ba27040199b11` | `decompile_all --name mbStatement` (superseded) | 14 procedures written, 0 failures, 0 skipped |
| `ev_25469ff40260f5b7e5a9bac7c035b8cd2f0d880c536e40f566c9587993184af8` | `decompile_all --address-start 0x1401c20f0 --address-end 0x1401c21c0` (superseded) | `parseOperand`, `returnValue`, `mbStatement` ctor; 3 written, 0 failures |
| `ev_e3530f99c32b3f59ecc82debcfff9db29d8641fd14d5f7576ccf12eb4cbe8d05` | `decompile_all --name Trigger` (superseded) | 52 procedures written, 0 failures; trigger layouts, loaders, runtime, and savegames |
| `ev_cebc7ae0bef7bccaddbcbb53c560d5d2020a5acf60c5bc03a77c496077e8c8d3` | `decompile_all --name rglTextStream` (superseded) | 18 procedures written, 0 failures; exact token reader/writer semantics |
| `ev_24eecfa4fc91587dd7b5b29ca767a78c8747a538e953f37b295746c67f18c0b6` | `decompile_all --name rglBinaryStream` (superseded) | 9 procedures written, 0 failures; raw 8/4/1-byte writers and length-prefixed strings |
| `ev_130d8023206760c8a441e940113e7a231d384634f4a73d4813a3ae6b412c62e6` | `decompile_all --address-start 0x1402a1d00 --address-end 0x1402a1d7f` (superseded) | 2 procedures written, 0 failures; 4-byte raw reader (`FUN_1402a1d00`); adjacent 1/8-byte entries confirmed by PDB aliases and disassembly |

Derived PDB/PE artifacts (retained outside the repository; hashes pin the exact v1.1.5.5 inputs unless noted):

| Artifact | Bytes | SHA-256 / Notes |
| --- | ---: | --- |
| `types_x64.json` (`types_new.json`) | 3,036,982 | `fca4211129ff508400eb9005b2a2a4c14c4e45cbb4555716a79d909802df3373` (v1.1.5.5) |
| `globals_x64.json` (`globals_new.json`) | 6,893,559 | `04fadbcb035936a3dfbe884cbf9a093cd4e690eccb261dd2ed57716ba81fa30e` (v1.1.5.5) |
| `vtables_x64.json` (`vtables_new.json`) | 476,811 | `4cddef9237ceac2949fb0d43c39110db8977bd9a3730b5992b5f402c335141da` (v1.1.5.5, reproducible via `tools/vtables.py`) |
| `imports_x64.json` *(2026-10-14)* | 69,259 | `8dd4b0747bc0a1a5ff930e8f99ccb073cf861e8ec21de3281d5492762ce2a646` *(superseded; v1.1.5.5 uses `ev_a4af0bca…`)* |
| `analysis_x64.json` *(2026-10-14)* | 8,200 | `891bceb598a01a0a9d490f926ac6bddf8cf37ba8c4e21a6f4568015beed5344d` *(superseded; v1.1.5.5 uses `ev_e323351d…`)* |

## 10. Legal guardrail

No game assets, retail binaries, PDBs, or derived disassembly are checked into the repository.
The Android app is BYOA: it asks the user to select a legally owned Warband installation and reads
`Modules/` from that location. No DRM or license check is bypassed.
