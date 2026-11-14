// Portable dispatch for WSE-extended statement opcodes.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   mbStatement::execute 0x1401c6150 decodes every operand through
//     mbStatement::parseOperand (0x1401c6070) into three fixed 32-element
//     arrays: int64 values, int32 truncations, and 4-byte tags. It masks the
//     opcode with 0x0FFFFFFF, dispatches opcodes 30..2767 through the native
//     jump table at 0x1401e22f0, and calls executeWSE for everything else.
//   mbStatement::executeWSE 0x1401e4dc0 receives those arrays, computes a
//     64-bit result in a handler, then writes it through operand 0 with
//     mbStatement::returnValue (0x1401c6100). A handler returning false is a
//     failed condition; a handler returning true without using returnValue is
//     a successful no-op (for example an out-of-range register).
//   Opcode 2800 is an equality-tested handler at pseudocode line 859 and
//     opcodes 2801..2809 are switch cases at lines 638..665.
//   The floating-point group starts at case 0x1130 (line 5192) and uses the
//     float array DAT_140698718 (g_basicGame+0x35d58), indexed by a register
//     number. mbCheckRegister
//     0x1401c4e60 accepts 0..127 (`cmp ecx,0x7f; ja`). DAT_140662bb8 is the
//     fixed-point scale and is set to 0x64 (100) at 0x140070ffa. fst uses
//     rglRound64 0x1402a1910: floorf(x) plus one when the fraction is > 0.5.
//     feq/fge/fle compare through rglAbs 0x1402a1850 with FLT_EPSILON.
//
// The complete 440-opcode structure is in docs/WSE_OPCODES.md and
// docs/wse_opcodes.json; wse_opcodes.h is generated from that map. This file
// implements the arithmetic/bitwise group 2800..2809, register group
// 3000..3017, 3020..3022, 3026..3028, 3030..3031, 3034..3038, 3040..3043,
// 3045, and 3600..3601/3608/3610/3619/3622, dictionary group 3200..3222,
// troop group 3700..3709, item/cur_item group 3800..3813, position vector
// group 4100 and 4102..4104, string group 4200..4224, 4226, 4231..4236,
// 4238..4246, options flags group 4300..4305, floating-point register group
// 4400..4438 (100%, including feval), edit-mode group 4600..4609, and dynamic
// array group 5000..5022 (222 of 440 opcodes). Every other known opcode
// reports WseDispatch::unimplemented instead of guessing.
//
// Register group evidence:
//   3000 register_get (line 628): mbCheckRegister(param_4[1]), then
//     (&g_globalVariables)[index] through the shared operand-0 store tail.
//   3001 register_set (line 870): mbCheckRegister(param_4[0]), then
//     (&g_globalVariables)[index] = param_3[1]; returns true, no store.
//   The bank is g_basicGame+0x270 (0x140662c30, confirmed at 0x1401e552d);
//     the 128-entry bound is the same mbCheckRegister used by the float group.
//   3002 store_wse_version (line 877, 0x1401e553a-0x1401e555b): selector 0
//     yields r15d = 5, selector 1 yields r15d = 0x483 (build 1155), and any
//     other selector falls through to the zeroed default.
//
// Script-context query evidence:
//   3014 store_current_trigger (0x1401e5917): return param_6 when it is <= 0,
//     else 0. mbStatement::execute passes its r8d context argument here; WSE
//     stores a negative value while inside a trigger (GetCurrentTrigger).
//   3016 store_num_return_values (0x1401e5979): g_numReturnValues at
//     0x140698ae8.
//   3017 store_return_value (0x1401e5985): 1-based index into the 16-slot
//     return-value bank at 0x140698af0; index 1..16 and <= g_numReturnValues
//     stores the slot, otherwise it logs and returns true without storing.
//   3035 store_application_time (0x1401e63a4): rglRound64((float)
//     DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f); FUN_1400b9690(5) returns QPC
//     seconds since the app-time baseline, and the constant at 0x140576b90 is
//     1000.0f.
//   3600 store_cur_mission_template_no (0x1401ea6b2): null
//     [0x14061f078] yields -1, otherwise the int32 at +0x9ae8.
//
// String group evidence:
//   String operands are decoded through mbCheckStringRegister (0x1401c51f0)
//   and the resolver at 0x140219c70. Tag 0 is the fixed 128-entry literal
//   table at 0x140665030 (stride 0x48); tag 0x16 addresses the local string
//   registers ([0x140621778], count [0x140621784], stride 0x90); tag 3 and
//   any unknown tag address the global string registers ([0x140621770],
//   count [0x140621780], stride 0x90). mbCheckStringRegister requires
//   0 <= index < count for tags 3/0x16, index <= 0x7f for tag 0, and accepts
//   unknown tags; a failed check logs and returns true without an effect.
//   4200 str_equals (0x1068 equality branch, 0x1401ed1bd): checks both
//     operands, then compares with wcscmp (0x1401f86b0) or _wcsicmp
//     (0x1401f86f0) when operand 2 is nonzero; equal means true.
//   4201 str_contains (0x1069): lower-cases both copies (0x1401f8b40) when
//     operand 2 is nonzero, then finds the needle (0x1401f8a80) from 0. A
//     null needle data pointer (empty string) yields -1, so an empty needle
//     is not contained.
//   4202/4203 str_starts_with/str_ends_with (0x106a/0x106b): take
//     Left/Right(needle length) (0x1401f8fd0/0x1401f8f20) and compare as in
//     str_equals.
//   4204..4206 str_is_alpha/digit/whitespace (0x106c..0x106e): iswalpha
//     (0x140498854, category 0x103), iswdigit (0x140498860, category 4), and
//     iswspace (0x14049886c, category 8). Fewer than 2 operands or index -1
//     scans the whole string; an out-of-bounds index logs and leaves the
//     initial true result.
//   4207 str_length (0x106f): length of operand 1 written through operand 0.
//   4208/4209 str_index_of/last_index_of (0x1070/0x1071): operand 1 is the
//     haystack, operand 2 the needle, operand 3 the start (default 0), and
//     operand 4 the end (default length-1). An empty needle yields -1; the
//     result is -1 unless the match is within [start, end].
//   4210 str_get_char (0x1072): UTF-16 code unit of operand 1 at operand 2;
//     an out-of-bounds index logs and stores nothing.
//   4211 str_to_num (0x1073): _wtof (0x140498878) of operand 1, multiplied by
//     DAT_140662bb8 (100.0) when operand 2 is nonzero, then truncated to
//     int64 (cvttsd2si).
//   4212 str_compare (0x1074, 0x1401ee042): operands 1/2 are strings, operand
//     3 selects the case-insensitive path. The case-sensitive wrapper
//     0x1401f86b0 normalises to -1/0/1; the case-insensitive path tail-jumps
//     to _wcsicmp (0x1404989d4), which returns the folded code-unit
//     difference. The result is written through operand 0.
//   4213 str_split (0x1075, 0x1401ee178): operand 1 is the destination tag-0
//     register (mbCheckRegister), operand 2 the source, operand 3 the
//     delimiter, operand 4 skip-empty, operand 5 max pieces (honoured only
//     when the statement has more than five operands). Operand 0 receives the
//     count. An empty delimiter cannot advance the retail loop; the port
//     stores the whole source once and terminates.
//   4214 str_sort (0x1076, 0x1401ee3eb): sorts operand 1 tag-0 registers from
//     operand 0; operand 2 selects _wcsicmp ordering and operand 3 reverses
//     the copy back into the table.
//   4215/4216 str_store_lower/upper (0x1077/0x1078): MakeLower
//     (0x1401f8b40) / MakeUpper (0x1401f8ac0) into tag-0 operand 0.
//   4217 str_store_trim (0x1079): operand 2 selects Trim (0x1401f8e20 =
//     right then left), TrimLeft (0x1401f8d00), or TrimRight (0x1401f8c40);
//     whitespace is iswspace (0x14049886c).
//   4218 str_store_replace (0x107a): replace-all (0x1401f8710) of operand 2
//     with operand 3, resuming after the replacement; an empty needle leaves
//     the string unchanged.
//   4219 str_store_md5 (0x107b, 0x1401ee956): converts UTF-16 registers to
//     UTF-8 (0xfde9) and hashes via CryptCreateHash(0x8003 = CALG_MD5) in
//     0x140330510, formatting 16 bytes with "%02x" (0x14056bca8). Ported with
//     a self-contained RFC 1321 MD5 implementation.
//   4220 str_store_substring (0x107c, 0x1401eea5a): start operand 2 defaults
//     to 0 and length operand 3 to the end; a negative or out-of-bounds range
//     logs and leaves the destination unchanged.
//   4221 str_store_reverse (0x107d): std::reverse (0x1404992b0).
//   4222 str_store_join (0x107e): join operand 2 tag-0 registers from operand
//     1 with operand 3 when it is present and valid; an invalid separator is
//     ignored rather than failing the handler.
//   4223/4224 str_store_replace_spaces_with_underscores and the reverse
//     (0x107f/0x1080): replace u" " (0x14055b3e0) with u"_" (0x14055b3dc)
//     or the reverse.
//   4233 str_store_float (0x1089, 0x1401ef3a8): formats (double)fp[operand 1]
//     with "%.*f" (0x14055b3e4) via 0x140200c40; defaults precision to 5 when
//     operand 2 is absent or < 0.
//   4234 str_sanitize (0x108a, 0x1401ef436): reads tag-0 operand 0 via
//     MultiByteToWideChar(0xfde9) (0x1403100d0), strips "{", "}", "<", ">"
//     (0x14055b3ec..0x14055b3f8 -> 0x14055ba68), and stores back.
//   4236 str_is_integer (0x108c, 0x1401ef55f): trims operand 0 (0x1401f8e20),
//     rejects empty string or bare '-', and tests digits with iswdigit
//     (0x140498860) after an optional leading '-'.
//   4238 str_store_server_password_rcon (0x108e -> 0x1401f49c1): retail jump
//     table maps directly to `return true` (no-op in v1.1.5.5).
//   4240..4243 str_regex_* (0x1090..0x1093, 0x1401ef741..0x1401efbc0):
//     std::regex (ECMAScript, 0x1400163b0) over UTF-8 text via 0x140200c70
//     (match), 0x140200db0 (search), 0x1401f92f0 (iterator, op 0 = start tag-0
//     register, op 1 = source, op 2 = regex, optional op 3 = max matches), and
//     0x140200df0 (replace). Catch blocks return false for predicates and
//     leave stores unaffected for action ops.
//   4244 str_decode_url (0x1094, 0x1401efd4d): calls SHLWAPI!UrlUnescapeA
//     (0x140501818) with dwFlags=0 in 0x1403108b0 (%XX -> byte, '+' unchanged,
//     malformed '%' literal, stops on NUL).
//   The tag-0 table at 0x140665030 is writable: 0x14030fd60 assigns a
//     register, while 0x140310970 copies a register into a temporary handle.
//
// Options flags evidence (0x10cc..0x10d1, 0x1401efff9..0x1401f003b):
//   4300/4301 options_{get,set}_verbose_casualties: bool byte at 0x1406173b7.
//   4302/4303 options_{get,set}_cheat_mode: bool byte at 0x14061ef50.
//   4304/4305 options_{get,set}_realistic_headshots: bool byte at 0x14061ef39.
//
// Floating-point additions & transcendental helpers:
//   4401 fld_str (0x1131, 0x1401f00c7): resolves operand 1 string, converts via
//     atof (0x1404989cc), and stores (float)value in fp[operand 0].
//   pow 0x1400b0400, log 0x1400a6c20, log10 0x1400a6c10, fmod 0x14006aac0,
//   atan2 0x14006f9b0. The call sites (0x1401f08ef, 0x1401f0cbc, 0x1401f0a28)
//   pass xmm0 = fp[operand 1] and
//   xmm1 = fp[operand 2], so fpow is pow(base, exponent), atan2 is
//   atan2(y, x), and fmod is fmod(dividend, divisor). flog reads its base from
//   the int32 operand 2 (a literal): fewer than 3 operands default to 10, and
//   a base below 2 logs "invalid logarithm base %d" and returns true without
//   storing. Division and modulo by zero also log and return true.
// - feval (4437 / 0x1155, 0x14006ff50..0x140070856, mu::Parser 0x1400a7280..
//   0x1400a84c7, vtable 0x14054f938): resolves operand 0 via mbBasicGame::parseString
//   (0x1400720b0..0x140072b73, '^' -> '\n', recursive {...}, {!/***}, {sN},
//   {regN}, {regN?a:b}), compiles/validates operators (<=,>=,!=,==,<,>,+,-,*,
//   /,^,&&,||,=,?:,,,unary +/- with prINFIX=6 so -2^2 == -4), constants (_pi,
//   _e), functions (sin..abs, ln=logf, rint, sign, deg2rad, rad2deg, floor,
//   ceil, fmod, atan2, clamp(lo>hi swap), sum/avg/min/max), and mutable
//   variables (fp<0..127>, pos<0..127>(sx|sy|sz|fx|fy|fz|ux|uy|uz|x|y|z)).
// - Unified object pool & array_* group (5000..5022 / 0x1388..0x139e,
//   0x1401e4dc0 L6427..6485 & L6802..7511): dictionaries and typed 1..14D
//   arrays (0=int32, 1=UTF-8 string, 2=rglTransform) share WSEObjectManager
//   (0x140698a90, 0x14034e340/3b0, 0x1400737a0/810). Binary .wsearray files
//   use magic 0x57534541 v1 with [o,f,u] transform order (0x1401c5910) and
//   orthonormalize() on load (0x1401c59f0). array_sort / array_sort_custom
//   use stable natural merge sort (0x1401fdd80, max n double-passes);
//   array_eq..array_le call 0x1401f58a0 (positions support ==/!= after
//   orthonormalize()).
// - Entity *_slot_gt group (3003..3013 / 0xbbb..0xbc5, 0x1401e55c5..0x1401e5912):
//   invalid entities or unsigned slot_no > 0xfffff return false (0x1401e4ec5),
//   out-of-range slots read 0 (0x1402183a0), and teams validate 0..7.
// - VM/time/HUD/mission scalars (3026/3030..3031/3036..3038/3045, 3601/3608/
//   3610/3619/3622): DAT_14061f038 (campaign_time), mbGame::m_mainPartyNo
//   (+0x420) & m_paused (+0x43c, gated on options.cheat_mode), is_party_skill
//   (skills[0..41].flags bit 4 at 0x14065f8ec), DAT_140657110 (hide_hud),
//   mbMission::m_waterLevel (+0xa4f4 * fixed_point_multiplier rounded via
//   0x1402a18a0), m_useObjectsTooltipEnabled[0..10] (+0x54a40),
//   DAT_14061ef1a (horse_friendly_fire), DAT_14061781a (show_crosshair = true).
// - Troop extended group (3700..3709 / 0xe74..0xe7d, 0x1401ebcee..0x1401ebf9a):
//   points at mbTroop +0x18c/+0x190/+0x194 (>= 0 on set), troop_has_flag bitwise
//   test at +0x128, troop_set_skill (0x1402460b0) and troop_set_attribute
//   (0x140246010) enforcing Ironflesh/STR HP cap, and troop_set_proficiency
//   clamped via 0x1400a61c0(val, 0, 1000) to [0, 999].
// - Item/cur_item group (3800..3813 / 0xed8..0xee5, 0x1401ebf9f..0x1401ec52b) +
//   str_store_item_{id,mesh_name} (4235/4239): low byte & damage-type bits on
//   mbItemKind +0x708/+0x70c, armor/speed/+0x738 particles/+0x740 blood_color/
//   +0x748 skeleton, and caller == -50 cur_item_* (0x1401c5e40) with
//   rglString::spacesToUnderscores (0x14030f270) on mesh/material names.
// - Config/skill/savegame string ops (4226/4231/4232/4245/4246): case-sensitive
//   INI/var maps (0x1402a7da0, 0x1402aa400), skills[0..41] name/desc with
//   _desc translation suffix (0x1401103d0, 0x140213520), and sg%d%d.sav MD5
//   hashing (0x14024f4c0, clears sreg first via 0x14030e9e0).
// - Edit mode group (4600..4609 / 0x11f8..0x1201, 0x1401f198c..0x1401f1c2c):
//   mbGame +0x16ed0..+0x16f50 active/mode/dialog/selection/highlighted index,
//   adjusting highlighted index on deselect, and DAT_14061efb8 option flag.

#pragma once

#include <cstdint>

#include "wse/vm/ops/agent_ext.h"
#include "wse/vm/ops/array.h"
#include "wse/vm/ops/bitwise.h"
#include "wse/vm/ops/dict.h"
#include "wse/vm/ops/fp.h"
#include "wse/vm/ops/lua_bridge.h"
#include "wse/vm/ops/player_mp.h"
#include "wse/vm/ops/position.h"
#include "wse/vm/ops/scene_mission.h"
#include "wse/vm/ops/string.h"
#include "wse/vm/ops/system.h"
#include "wse/vm/ops/ui_menu.h"
#include "wse/vm/ops/world.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

// `val_*` uses operand 0 as both source and destination; `store_*` reads
// operands 1.. and leaves operand 0 as the destination lvalue for the caller.
[[nodiscard]] inline WseDispatch execute_wse(std::uint32_t opcode,
                                             const WseOperands& operands,
                                             WseEffect& effect) {
    opcode &= kWseOpcodeMask;
    effect.store_value = false;

    using DispatchFn =
        WseDispatch (*)(std::uint32_t, const WseOperands&, WseEffect&);
    constexpr DispatchFn kDispatchers[] = {
        &execute_wse_bitwise,
        &execute_wse_system,
        &execute_wse_world,
        &execute_wse_string,
        &execute_wse_fp,
        &execute_wse_dict,
        &execute_wse_array,
        &execute_wse_position,
        &execute_wse_player_mp,
        &execute_wse_agent_ext,
        &execute_wse_scene_mission,
        &execute_wse_ui_menu,
        &execute_wse_lua_bridge,
    };
    for (const DispatchFn dispatch : kDispatchers) {
        const WseDispatch res = dispatch(opcode, operands, effect);
        if (res != WseDispatch::unimplemented) {
            return res;
        }
    }
    return WseDispatch::unimplemented;
}

}  // namespace wse::vm
