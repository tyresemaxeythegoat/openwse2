# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/io/text_stream.h` | whitespace token reader and typed decimal parsers | `rglInputTextFileStream` vftable `0x1405651e0`, `rglTextStream` readers `0x1402afa80`–`0x1402afe60`, `mbGame::readTriggers` `0x1400ccf20` |
| `include/wse/io/binary_stream.h` | little-endian savegame reader/writer | `rglBinaryStream` vftable `0x14055c780`; writers `0x1402ae980/0x1402ae9d0/0x1402aea30`; readers `0x1402aebe0/0x1402aec00/0x1402aec20` |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c6070`, `mbStatement::returnValue` `0x1401c6100`, `mbStatementBlock::execute` `0x1401f69d0`, `analyze` `0x1401f6740` |
| `include/wse/vm/slots.h` | `mbSlots` (`std::vector<int64_t>`) get/set semantics | PDB type `0x1b0f8`; `parseOperand` bounds check; `returnValue` write path |
| `include/wse/vm/statement.h` | statement record and text reader | `mbStatementBlock::readStream` `0x1401f6600`, PDB type `0xb91e` |
| `include/wse/vm/block_vm.h` | statement-block VM, loop frames, core native ops, script/trigger VM bridge | `0x1401c6150`, `0x1401f6740`–`0x1401f7f00`, `0x1400dbde0`/`0x1400dbe70`, `0x140244cd0`, `0x14020fb80`/`0x1402101a0` |
| `include/wse/game/trigger.h` | trigger/simple-trigger records, loaders, savegames, and event-key runtime state machines | `0x140244810`, `0x140244540`, `0x14020fe60`, `0x14020fab0`, `0x1400ccf20`, `0x1400cd2d0`, `0x140244970`, `0x140244b60`, `0x14020ffa0`, `0x140244cd0`, `0x14020fb80`, `0x1402101a0` |
| `include/wse/game/script.h` | `mbScript`/`mbScriptManager` readers, 65-entry name table, and script mapping | `0x14020f6a0`, `0x14020f7c0`, `0x14020f9a0`; globals `0x14069fac0`/`0x14069fac8`/`0x14069facc`; table `0x140615ad0`; formats `0x14055c680`/`0x14055c690` |
| `include/wse/vm/wse_opcodes.h` | generated enum of the 440 WSE-extended operation names in the retail dispatch range | `mbStatement::executeWSE` `0x1401e4dc0`; names from `header_operations_addon.py` |
| `include/wse/vm/execute_wse.h` | opcode mask, decoded operand view, dispatch result, and 93/440 handlers (2800–2809, 3000–3002, 3014–3600, string 4200–4224/4233–4234/4236/4238/4240–4244, options 4300–4305, floating-point 4400–4401/4405–4438) | `executeWSE` `0x1401e4dc0` (lines 628, 638–665, 859, 870–877, 4670, 5192, 5202–6420, 7881–8380); `mbStatement::execute` `0x1401c6150` |
| `tests/operand_test.cpp` | host tests for tags, decode, store, slots, tokenizer, statements | local test doubles only |
| `tests/trigger_test.cpp` | host tests for trigger loaders and event-key state machines | local test doubles only |
| `tests/savegame_test.cpp` | host tests for the binary stream and trigger savegames | local test doubles only |
| `tests/script_test.cpp` + `fixtures/scripts.txt` | host tests for script records, header errors, duplicate mapping, and missing-name reporting | local fixture mirroring the retail grammar |
| `tests/wse_test.cpp` | host tests for opcode enum values, shift/bitwise handlers, operand bounds, and opcode masking | local test doubles only |
| `tests/block_vm_test.cpp` | host tests for analyze/modifiers/loops/core math/scripts/recursion/trigger VM integration | local test doubles and module fixtures |

## Module text format

Module files are whitespace-delimited decimal text. The retail header is three tokens
(`triggersfile version 1`, `simple_triggersfile version 1`, `scriptsfile version 1`, …); the readers
skip bytes `<= 0x20`, take the next token, and parse it. There are no binary records.

Operands are 64-bit words. Values `<= 0` are immediates; otherwise bits 56–62 hold the tag and
bits 0–55 the payload.

| Tag | Meaning |
| ---: | --- |
| `0x00` | immediate |
| `0x01` | global variable array (unchecked in the original) |
| `0x02` | `mbSlots` vector (missing slot reads as 0) |
| `0x11` | local variable (unchecked in the original) |

A statement is `<opcode> <operandCount> <operand>...`. `m_endStatement` is initialized to `-1` by
the constructor and filled by `analyze`.

A trigger is three floats, the conditions block, and the consequences block. A simple trigger is
one float followed by its statement block. Manager records seed every `rglAppTimer` from
`m_defaultTimerNo` and `g_timersTotal[m_defaultTimerNo]`.

Booleans accept `true`/`false` case-insensitively, otherwise a nonzero decimal integer. The retail
float reader uses `strtod`; this port uses `std::from_chars` and rejects trailing garbage. Valid
retail module files parse identically.

## Runtime

`execute_trigger` / `execute_trigger_manager` and their simple-trigger counterparts reproduce the
inlined retail state machines. The `int` argument is an **event key**, not a clock: non-negative
keys run interval triggers from `g_timersTotal`, while negative keys match the rounded negative
check interval (`-112` = agent exit status, `-105`/`-106` = ranged attack, `-26`/`-56` = death, …).
`trigger.h` retains callback-based overloads alongside the state transitions, while
`wse/vm/block_vm.h` connects triggers and scripts directly to the reconstructed
`mbStatementBlock::execute` interpreter (`execute_trigger`, `execute_trigger_manager`,
`execute_simple_trigger`, `execute_simple_trigger_manager`, `execute_script`, `execute_mapped_script`).

## Savegames

`rglBinaryStream` stores raw little-endian values with no header or per-value metadata. Trigger
savegames are an `int32` count, then per manager trigger `int32` status plus `int64` check, delay,
and rearm timer values; simple triggers store one `int64` timer value. The portable
`read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` use
`SavegameResult`: `count_exceeded` reproduces the retail message-box path while consuming every
entry, and `negative_count`/`truncated` reject malformed input. Retail performs no bounds checks on
binary reads, so truncated files would read past the buffer there.

## Scripts

`scripts.txt` is `scriptsfile version <int32>` followed by the script count and that many
`<id> <interval-seconds> <statement block>` records. `mbScriptManager::readStream` discards both
header strings and the version, replaces the script array, parses every record, then maps the 64
static names. Mapping uses exact string equality and the first script in file order wins, so
duplicate ids resolve to the earliest record. A missing name maps to `-1` and reports through
`ScriptMapCallbacks::reportMissing` (retail logs `"Unable to map script %s"`). Each statement block
id becomes `"Script [<index>] <id>"`.

The portable reader was also run against the retail 3,467,512-byte `scripts.txt` sample: 1,755
scripts, every token consumed, 52 of 64 names mapped.

## WSE extended opcodes

`docs/wse_opcodes.json` and `docs/WSE_OPCODES.md` map every SDK operation in the retail extended range
2800–5202 to the `executeWSE` handler that implements it: 440/440 operations are covered (431 switch
cases and 9 equality-tested cases, with no unknown or unhandled opcode in range). Each entry records the
pseudocode line and the handler's call extent. `tools/extract_wse_opcodes.py` regenerates both files and
`include/wse/vm/wse_opcodes.h` from the 8,844-line WSE2 v1.1.5.5 pseudocode dump and the public
`header_operations_addon.py` names.

`execute_wse` masks the opcode with `0x0FFFFFFF` and currently implements six groups. The
arithmetic/bitwise group 2800–2809 covers arithmetic right shift (`val_shr`/`store_shr`), logical
right shift (`val_lshr`/`store_lshr`), left shift (`val_shl`/`store_shl`), xor (`val_xor`/`store_xor`),
and complement (`val_not`/`store_not`); shift counts are masked with `& 0x3F`, and `store_*` reads
the value from operand 1 and leaves operand 0 as the destination lvalue.

The register group covers `register_get` (3000), `register_set` (3001), and `store_wse_version` (3002).
The register bank is the 128-entry int64 array at `g_basicGame+0x270` (`0x140662c30`), the same bank
that operand tag 1 addresses; `register_get` writes the indexed value through operand 0, and
`register_set` stores operand 1 into the register named by operand 0 and returns without a store.
Both reject an out-of-range index with `WseDispatch::ignored`, matching `mbCheckRegister`
(`0x1401c4e60`) and the retail `return true` fall-through. `store_wse_version` ignores its value
operand: selector 0 yields the WSE version 5, selector 1 yields the Warband build `0x483` (1155), and
any other selector yields the zeroed default (`0x1401e553a`–`0x1401e555b`).

The script-context query group covers `store_current_trigger` (3014),
`store_num_return_values` (3016), `store_return_value` (3017), `store_application_time` (3035), and
`store_cur_mission_template_no` (3600). `store_current_trigger` returns the statement's context
argument only while it is negative (the in-trigger marker; outside a trigger it is 0), matching
`WSEOperationContext::GetCurrentTrigger`. `store_num_return_values` reads the count at
`0x140698ae8`; `store_return_value` reads a 1-based slot from the 16-entry bank at `0x140698af0`,
and an out-of-range or unset index logs and returns true without storing.
`store_application_time` is `rglRound64((float)DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f)` — the
call `FUN_1400b9690(5)` returns QPC seconds since the app-time baseline and the constant at
`0x140576b90` is `1000.0f`. `store_cur_mission_template_no` reads `[0x14061f078]+0x9ae8`, or -1
when there is no current mission.

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
`str_store_regex_replace` (4243), and `str_decode_url` (4244). Operands resolve through `0x140219c70`: tag 0 (`WseStringBanks::registers`)
is the writable 128-entry string register table at `0x140665030` (assigned by `0x14030fd60` and
copied out by `0x140310970`), tag 0x16 the local string registers (`[0x140621778]`, count
`[0x140621784]`), and tag 3 or an unknown tag the global registers (`[0x140621770]`, count
`[0x140621780]`). `mbCheckStringRegister` (`0x1401c51f0`) validates operand indices and logs on
failure (`WseDispatch::ignored`), while destination tag-0 registers use `mbCheckRegister` (`0..127`).
Comparisons use `wcscmp`/`_wcsicmp`; `str_compare` normalizes the case-sensitive path to `-1/0/1`
(`0x1401f86b0`) and returns the raw folded code-unit difference from `_wcsicmp` (`0x1404989d4`) when
operand 3 is nonzero. `str_contains` lower-cases both strings when operand 2 is nonzero and treats an
empty needle as not contained. The predicates scan the whole string when the index is absent or -1;
an out-of-bounds index logs and leaves the true result. `str_index_of`/`str_last_index_of` take an
optional start (operand 3) and end (operand 4); the last-index handler mirrors the retail loop's
search-before-bound-check order. `str_to_num` uses `_wtof` and truncates after applying the
fixed-point scale when operand 2 is nonzero. `str_split` writes into consecutive tag-0 registers
starting at operand 1 (capped at 127), honors `max` (operand 5) only when more than five operands
are supplied, skips empty pieces when operand 4 is nonzero, and avoids the retail infinite loop on an
empty delimiter by storing the whole source once. `str_sort` sorts a range of tag-0 registers
(case-insensitive on operand 2, reversed copy-back on operand 3). The `str_store_*` handlers write
into tag-0 operand 0: lower/upper case, trim (`0` both, `1` left, `2` right via `iswspace`),
non-overlapping replace-all, lowercase RFC 1321 MD5 (`4219`), `"%.*f"` float formatting (`4233`,
default precision 5), sanitize (`4234`, removes `{`, `}`, `<`, `>`), bounded substring (defaults
`start = 0`, `length = end`; out-of-bounds leaves destination unchanged), UTF-16 code-unit reverse,
range join (with optional valid separator), `" "`/`"_"` conversions, and percent-decoding (`4244`,
matching `UrlUnescapeA(..., 0)`). `str_is_integer` (`4236`) trims both ends and checks `iswdigit`
with an optional leading `'-'`. `str_store_server_password_rcon` (`4238`) is a no-op in v1.1.5.5.
`str_regex_*` (`4240`–`4243`) evaluate ECMAScript `std::regex` over UTF-8 text; invalid patterns
evaluate false or leave stores untouched.

The options flags group (`4300`–`4305`, `0x10cc`–`0x10d1`) exposes `verbose_casualties`,
`cheat_mode`, and `realistic_headshots` via `WseVmState::options`.

The floating-point group covers `fld`, `fld_str`, `fst`, `fcpy`, `feq`, `fgt`, `flt`, `fge`, `fle`, `fsub`,
`fmul`, `fdiv`, `fmin`, `fmax`, `fclamp`, `fadd`, and the transcendental/rounding helpers
`fsqrt`, `fabs`, `fceil`, `ffloor`, `fexp`, `fpow`, `fln`, `flog`, `fmod`, `fsin`, `fcos`, `ftan`,
`fsinh`, `fcosh`, `ftanh`, `fasin`, `facos`, `fatan`, and `fatan2` (4400–4401, 4405–4438). Registers are
the 128-entry float array `DAT_140698718` (`g_basicGame+0x35d58`); `mbCheckRegister` (`0x1401c4e60`) accepts indices 0–127.
`fld` divides the fixed-point operand by 100 (`g_basicGame+0x1f8` = `0x140662bb8`, set to `0x64` at
`0x140070ffa` in `mbBasicGame::initialize`); `fld_str` (`4401`) parses operand 1 with `atof`
(`0x1404989cc`);
`fst` multiplies by 100 and rounds with `rglRound64` (`0x1402a1910`): `floorf(x)` plus one when the
fraction is greater than 0.5. Float comparisons use `rglAbs` and `FLT_EPSILON`. `fpow`, `fmod`, and
`fatan2` take operand 1 in `xmm0` and operand 2 in `xmm1`; `flog` reads a literal base from operand 2
(default 10, minimum 2). Handlers that do not write operand 0 return `WseDispatch::handled_no_store`;
a failed comparison returns `WseDispatch::condition_false`; an invalid register is a successful no-op
(`WseDispatch::ignored`), matching the retail `return true` fall-through.

Currently 93 of 440 mapped opcodes are implemented; every other mapped opcode returns
`WseDispatch::unimplemented` until its handler is transcribed.

## Safety deviations

The retail code trusts operand/statement/script/trigger counts and timer indices. This port rejects
negative counts, operands above 32, oversized arrays, out-of-range timer indices, and truncated
binary savegames; none of these checks changes valid module behaviour. In `str_split` (4213), an
empty delimiter cannot advance in retail and would loop indefinitely; the port stores the source as
a single piece (respecting `skip_empty`) and terminates.

## Build and test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Expected tail:

```
operand tests: PASS
trigger tests: PASS
savegame tests: PASS
script tests: PASS
wse tests: PASS
block_vm tests: PASS
engine host tests: PASS
```

The Android half of the gate (`clang++ -target aarch64-linux-android29 -Wall -Werror`) cannot run
until an NDK is installed; see `STATUS.md`.
