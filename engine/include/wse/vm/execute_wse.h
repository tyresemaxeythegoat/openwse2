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
//   0x1400a84c7, 0x1405e9a20): resolves operand 0 via mbBasicGame::parseString
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

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "wse/game/world_state.h"
#include "wse/vm/wse_opcodes.h"

namespace wse::vm {

inline constexpr std::uint32_t kWseOpcodeMask = 0x0FFFFFFFu;

// mbCheckRegister accepts unsigned indices 0..0x7f.
inline constexpr std::size_t kFpRegisterCount = 128;

// DAT_140662bb8, fixed-point values per unit (set to 100 at 0x140070ffa).
inline constexpr float kWseFixedPointScale = 100.0f;

// String banks addressed by the str_* handlers. The spans hold views into the
// runtime's string registers; a missing entry resolves to the empty string,
// matching the static empty rgl::string returned by 0x140219c70. Tag 0 is
// writable because str_store_* and str_split/str_sort assign into it.
struct WseStringBanks {
    // Tag 0: the 128-entry string register table at 0x140665030 (stride 0x48).
    // Script literals live here too; str_store_* overwrite entries.
    std::span<std::u16string> registers{};
    // Tag 3 (and unknown tags): [0x140621770], count [0x140621780].
    std::span<const std::u16string_view> global{};
    // Tag 0x16: [0x140621778], count [0x140621784].
    std::span<const std::u16string_view> local{};
};

// Runtime option booleans queried and mutated by opcodes 4300..4305, 3619,
// 3622, and 4606.
struct WseOptionsState {
    bool verbose_casualties = false;   // 0x1406173b7
    bool cheat_mode = false;           // 0x14061ef50
    bool realistic_headshots = false;  // 0x14061ef39
    bool show_crosshair = true;        // 0x14061781a
    bool horse_friendly_fire = false;  // 0x14061ef1a
    bool edit_mode_enabled = false;    // 0x14061efb8
};

// 64-byte position frame (rglTransform at 0x140663030, stride 0x40;
// Identity at 0x140621730): side s, forward f, up u, origin o.
struct WseVec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    [[nodiscard]] constexpr WseVec4 operator-(const WseVec4& rhs) const noexcept {
        return {x - rhs.x, y - rhs.y, z - rhs.z, 0.0f};
    }

    [[nodiscard]] float length() const noexcept {
        return std::sqrt(x * x + y * y + z * z);
    }

    [[nodiscard]] constexpr float dot(const WseVec4& rhs) const noexcept {
        return x * rhs.x + y * rhs.y + z * rhs.z;
    }

    [[nodiscard]] constexpr WseVec4 cross(const WseVec4& rhs) const noexcept {
        return {y * rhs.z - z * rhs.y,
                z * rhs.x - x * rhs.z,
                x * rhs.y - y * rhs.x,
                0.0f};
    }

    // Mirrors rglVector4::normalize (0x140318a10): threshold 1e-10f (0x1405760f4),
    // fallback (0, 1, 0) while preserving w.
    void normalize() noexcept {
        const float len = length();
        if (len <= 1.0e-10f) {
            x = 0.0f;
            y = 1.0f;
            z = 0.0f;
            return;
        }
        const float inv = 1.0f / len;
        x *= inv;
        y *= inv;
        z *= inv;
    }
};

struct WseTransform {
    WseVec4 s{1.0f, 0.0f, 0.0f, 0.0f};
    WseVec4 f{0.0f, 1.0f, 0.0f, 0.0f};
    WseVec4 u{0.0f, 0.0f, 1.0f, 0.0f};
    WseVec4 o{0.0f, 0.0f, 0.0f, 1.0f};

    [[nodiscard]] static constexpr WseTransform identity() noexcept {
        return {};
    }

    // 0x1402b5be0: rotates forward f and up u around local side axis s.
    void rotate_x(float angle) noexcept {
        const float c = std::cos(angle);
        const float s_val = std::sin(angle);
        const WseVec4 nf{f.x * c + u.x * s_val,
                         f.y * c + u.y * s_val,
                         f.z * c + u.z * s_val,
                         f.w};
        const WseVec4 nu{u.x * c - f.x * s_val,
                         u.y * c - f.y * s_val,
                         u.z * c - f.z * s_val,
                         u.w};
        f = nf;
        u = nu;
    }

    // 0x1402b5ea0: rotates side s and forward f around local up axis u.
    void rotate_z(float angle) noexcept {
        const float c = std::cos(angle);
        const float s_val = std::sin(angle);
        const WseVec4 ns{s.x * c + f.x * s_val,
                         s.y * c + f.y * s_val,
                         s.z * c + f.z * s_val,
                         s.w};
        const WseVec4 nf{f.x * c - s.x * s_val,
                         f.y * c - s.y * s_val,
                         f.z * c - s.z * s_val,
                         f.w};
        s = ns;
        f = nf;
    }

    // 0x1402b64b0: normalize f, s = normalize(f x u), u = s x f.
    void orthonormalize() noexcept {
        f.normalize();
        s = f.cross(u);
        s.normalize();
        u = s.cross(f);
    }
};

// Default zeroed rglTransform for array_create / array_resize_dim (0-position).
[[nodiscard]] constexpr WseTransform wse_zero_transform() noexcept {
    return {{0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f}};
}

// Mirrors rglTransform compare in FUN_1401f58a0 (0x1401f601d): orthonormalizes
// copies of both transforms, then compares the 12 basis + origin floats.
[[nodiscard]] inline bool wse_transforms_equal(const WseTransform& lhs,
                                               const WseTransform& rhs) noexcept {
    WseTransform a = lhs;
    WseTransform b = rhs;
    a.orthonormalize();
    b.orthonormalize();
    return a.s.x == b.s.x && a.s.y == b.s.y && a.s.z == b.s.z &&
           a.f.x == b.f.x && a.f.y == b.f.y && a.f.z == b.f.z &&
           a.u.x == b.u.x && a.u.y == b.u.y && a.u.z == b.u.z &&
           a.o.x == b.o.x && a.o.y == b.o.y && a.o.z == b.o.z;
}

// 16-byte timer slot (0x140667b18 / 0x140667b20, stride 0x10).
// Ticks run at 100,000 Hz (0x1401e5e2f divides delta by 100000.0f -> seconds).
struct WseAppTimer {
    std::int32_t kind = 0;
    std::int64_t start_ticks = 0;
};

// Standard MT19937 PRNG mirroring rglMersenneTwister (0x1402a24f0/5e0/6b0/780).
struct WseMersenneTwister {
    std::array<std::uint32_t, 624> state{};
    std::size_t index = 624;

    WseMersenneTwister() noexcept {
        seed(5489U);
    }

    void seed(std::uint32_t value) noexcept {
        state[0] = value;
        for (std::size_t i = 1; i < state.size(); ++i) {
            const std::uint32_t prev = state[i - 1];
            state[i] = 0x6c078965U * (prev ^ (prev >> 30U)) +
                       static_cast<std::uint32_t>(i);
        }
        index = state.size();
    }

    [[nodiscard]] std::uint32_t next_u32() noexcept {
        if (index >= state.size()) {
            twist();
        }
        std::uint32_t y = state[index++];
        y ^= y >> 11U;
        y ^= (y << 7U) & 0x9d2c5680U;
        y ^= (y << 15U) & 0xefc60000U;
        y ^= y >> 18U;
        return y;
    }

    // Exact match for 0x1402a25e0: uniform [min, max) using 32-bit unsigned span.
    [[nodiscard]] std::int32_t get_int(std::int32_t min,
                                       std::int32_t max) noexcept {
        const std::uint32_t b = static_cast<std::uint32_t>(max) - 1U;
        const std::uint32_t span = b - static_cast<std::uint32_t>(min);
        if (span == 0xffffffffU) {
            return static_cast<std::int32_t>(next_u32() +
                                             static_cast<std::uint32_t>(min));
        }
        const std::uint32_t range = span + 1U;
        const std::uint32_t q = 0xffffffffU / range;
        const std::uint32_t rem = 0xffffffffU % range;
        std::uint32_t r = 0U;
        if (span != 0U) {
            for (;;) {
                r = next_u32();
                if ((r / range) < q || rem == span) {
                    break;
                }
            }
        }
        return static_cast<std::int32_t>((r % range) +
                                         static_cast<std::uint32_t>(min));
    }

private:
    void twist() noexcept {
        constexpr std::size_t n = 624;
        constexpr std::size_t m = 397;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t y = (state[i] & 0x80000000U) |
                                    (state[(i + 1) % n] & 0x7fffffffU);
            state[i] = state[(i + m) % n] ^ (y >> 1U);
            if ((y & 1U) != 0U) {
                state[i] ^= 0x9908b0dfU;
            }
        }
        index = 0;
    }
};

// Ordered UTF-8 string map mirroring rglSectionlessIni.
struct WseDictionary {
    std::map<std::string, std::string> entries{};

    [[nodiscard]] bool empty() const noexcept {
        return entries.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries.size();
    }

    [[nodiscard]] bool has_key(const std::string& key) const noexcept {
        return entries.find(key) != entries.end();
    }

    void clear() noexcept {
        entries.clear();
    }

    void set(const std::string& key, std::string value) {
        entries[key] = std::move(value);
    }

    void merge(const std::map<std::string, std::string>& source,
               bool overwrite) {
        for (const auto& [key, value] : source) {
            if (overwrite || entries.find(key) == entries.end()) {
                entries[key] = value;
            }
        }
    }
};

enum class WseArrayType : std::int32_t {
    integer = 0,
    string = 1,
    position = 2,
};

inline constexpr std::int32_t kWseArrayMaxDims = 14;
inline constexpr std::uint32_t kWseArrayBinaryMagic = 0x57534541U;
inline constexpr std::int32_t kWseArrayBinaryVersion = 1;

// Recursive slice storage backing 1..14D typed dynamic arrays
// (WSEBasicDynMultiArray / WSEDynMultiArray<T>).
struct WseArraySlice {
    std::vector<std::unique_ptr<WseArraySlice>> children{};
    std::vector<std::int32_t> ints{};
    std::vector<std::string> strings{};
    std::vector<WseTransform> positions{};

    [[nodiscard]] std::unique_ptr<WseArraySlice> clone(
        WseArrayType type, std::size_t depth, std::size_t dim_count) const {
        auto copy = std::make_unique<WseArraySlice>();
        if (depth + 1 < dim_count) {
            copy->children.reserve(children.size());
            for (const auto& child : children) {
                copy->children.push_back(
                    child ? child->clone(type, depth + 1, dim_count)
                          : nullptr);
            }
            return copy;
        }
        switch (type) {
        case WseArrayType::integer:
            copy->ints = ints;
            break;
        case WseArrayType::string:
            copy->strings = strings;
            break;
        case WseArrayType::position:
            copy->positions = positions;
            break;
        }
        return copy;
    }

    void fill_all(WseArrayType type,
                  std::size_t depth,
                  std::size_t dim_count,
                  std::int32_t int_val,
                  const std::string& str_val,
                  const WseTransform& pos_val) {
        if (depth + 1 < dim_count) {
            for (auto& child : children) {
                if (child) {
                    child->fill_all(type, depth + 1, dim_count, int_val,
                                    str_val, pos_val);
                }
            }
            return;
        }
        switch (type) {
        case WseArrayType::integer:
            std::fill(ints.begin(), ints.end(), int_val);
            break;
        case WseArrayType::string:
            std::fill(strings.begin(), strings.end(), str_val);
            break;
        case WseArrayType::position:
            std::fill(positions.begin(), positions.end(), pos_val);
            break;
        }
    }
};

struct WseDynArray {
    WseArrayType type = WseArrayType::integer;
    std::vector<std::int32_t> dim_sizes{};
    std::unique_ptr<WseArraySlice> root{};

    [[nodiscard]] static bool dims_valid(
        std::span<const std::int32_t> dims) noexcept {
        if (dims.empty() ||
            static_cast<std::int32_t>(dims.size()) > kWseArrayMaxDims) {
            return false;
        }
        if (dims[0] < 0) {
            return false;
        }
        for (std::size_t i = 1; i < dims.size(); ++i) {
            if (dims[i] < 1) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::unique_ptr<WseArraySlice> make_slice(
        WseArrayType type, std::span<const std::int32_t> dims) {
        if (dims.empty()) {
            return nullptr;
        }
        auto slice = std::make_unique<WseArraySlice>();
        const auto count = static_cast<std::size_t>(dims[0]);
        if (dims.size() == 1) {
            switch (type) {
            case WseArrayType::integer:
                slice->ints.resize(count, 0);
                break;
            case WseArrayType::string:
                slice->strings.resize(count);
                break;
            case WseArrayType::position:
                slice->positions.resize(count, wse_zero_transform());
                break;
            }
            return slice;
        }
        slice->children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            slice->children.push_back(make_slice(type, dims.subspan(1)));
        }
        return slice;
    }

    [[nodiscard]] static std::unique_ptr<WseDynArray> create(
        WseArrayType type, std::span<const std::int32_t> dims) {
        if (!dims_valid(dims)) {
            return nullptr;
        }
        auto arr = std::make_unique<WseDynArray>();
        arr->type = type;
        arr->dim_sizes.assign(dims.begin(), dims.end());
        arr->root = make_slice(type, arr->dim_sizes);
        return arr;
    }

    [[nodiscard]] std::unique_ptr<WseDynArray> clone() const {
        auto copy = std::make_unique<WseDynArray>();
        copy->type = type;
        copy->dim_sizes = dim_sizes;
        copy->root =
            root ? root->clone(type, 0, dim_sizes.size()) : nullptr;
        return copy;
    }

    [[nodiscard]] std::int32_t type_id() const noexcept {
        return static_cast<std::int32_t>(type);
    }

    [[nodiscard]] std::int32_t dim_count() const noexcept {
        return static_cast<std::int32_t>(dim_sizes.size());
    }

    [[nodiscard]] std::int32_t dim_size(
        std::int32_t dim_index) const noexcept {
        if (dim_index < 0 ||
            static_cast<std::size_t>(dim_index) >= dim_sizes.size()) {
            return -1;
        }
        return dim_sizes[static_cast<std::size_t>(dim_index)];
    }

    [[nodiscard]] const WseArraySlice* leaf_for(
        std::span<const std::int32_t> indices,
        std::size_t& last_index) const noexcept {
        if (!root || indices.size() != dim_sizes.size()) {
            return nullptr;
        }
        for (std::size_t d = 0; d < dim_sizes.size(); ++d) {
            if (indices[d] < 0 || indices[d] >= dim_sizes[d]) {
                return nullptr;
            }
        }
        const WseArraySlice* cur = root.get();
        for (std::size_t d = 0; d + 1 < dim_sizes.size(); ++d) {
            const auto idx = static_cast<std::size_t>(indices[d]);
            if (idx >= cur->children.size() || !cur->children[idx]) {
                return nullptr;
            }
            cur = cur->children[idx].get();
        }
        last_index = static_cast<std::size_t>(indices.back());
        return cur;
    }

    [[nodiscard]] WseArraySlice* leaf_for(
        std::span<const std::int32_t> indices,
        std::size_t& last_index) noexcept {
        return const_cast<WseArraySlice*>(
            std::as_const(*this).leaf_for(indices, last_index));
    }

    [[nodiscard]] WseArraySlice* slice_along_dim0(
        std::size_t row,
        std::span<const std::int32_t> tail_indices,
        std::size_t& last_index) noexcept {
        if (!root || tail_indices.size() + 1 != dim_sizes.size() ||
            row >= static_cast<std::size_t>(dim_sizes[0])) {
            return nullptr;
        }
        if (dim_sizes.size() == 1) {
            last_index = row;
            return root.get();
        }
        for (std::size_t k = 0; k < tail_indices.size(); ++k) {
            if (tail_indices[k] < 0 || tail_indices[k] >= dim_sizes[k + 1]) {
                return nullptr;
            }
        }
        if (row >= root->children.size() || !root->children[row]) {
            return nullptr;
        }
        WseArraySlice* cur = root->children[row].get();
        for (std::size_t k = 0; k + 1 < tail_indices.size(); ++k) {
            const auto idx = static_cast<std::size_t>(tail_indices[k]);
            if (idx >= cur->children.size() || !cur->children[idx]) {
                return nullptr;
            }
            cur = cur->children[idx].get();
        }
        last_index = static_cast<std::size_t>(tail_indices.back());
        return cur;
    }

    [[nodiscard]] const WseArraySlice* slice_along_dim0(
        std::size_t row,
        std::span<const std::int32_t> tail_indices,
        std::size_t& last_index) const noexcept {
        return const_cast<WseDynArray*>(this)->slice_along_dim0(
            row, tail_indices, last_index);
    }

    [[nodiscard]] bool resize_dim(std::int32_t dim_index,
                                  std::int32_t new_size) {
        if (!root || dim_index < 0 ||
            static_cast<std::size_t>(dim_index) >= dim_sizes.size() ||
            new_size < 1) {
            return false;
        }
        const auto target_dim = static_cast<std::size_t>(dim_index);
        dim_sizes[target_dim] = new_size;
        resize_recursive(*root, 0, target_dim, static_cast<std::size_t>(new_size));
        return true;
    }

    [[nodiscard]] bool push_subarray(const WseDynArray& src) {
        if (!root || dim_sizes.size() <= 1 || src.type != type ||
            src.dim_sizes.size() + 1 != dim_sizes.size() || !src.root) {
            return false;
        }
        for (std::size_t k = 0; k < src.dim_sizes.size(); ++k) {
            if (src.dim_sizes[k] != dim_sizes[k + 1]) {
                return false;
            }
        }
        root->children.push_back(
            src.root->clone(type, 1, dim_sizes.size()));
        dim_sizes[0] = static_cast<std::int32_t>(root->children.size());
        return true;
    }

    [[nodiscard]] std::unique_ptr<WseDynArray> pop_subarray() {
        if (!root || dim_sizes.size() <= 1 || root->children.empty()) {
            return nullptr;
        }
        auto out = std::make_unique<WseDynArray>();
        out->type = type;
        out->dim_sizes.assign(dim_sizes.begin() + 1, dim_sizes.end());
        out->root = std::move(root->children.back());
        root->children.pop_back();
        dim_sizes[0] = static_cast<std::int32_t>(root->children.size());
        return out;
    }

private:
    void resize_recursive(WseArraySlice& slice,
                          std::size_t depth,
                          std::size_t target_dim,
                          std::size_t new_size) {
        if (depth + 1 == dim_sizes.size()) {
            switch (type) {
            case WseArrayType::integer:
                slice.ints.resize(new_size, 0);
                break;
            case WseArrayType::string:
                slice.strings.resize(new_size);
                break;
            case WseArrayType::position:
                slice.positions.resize(new_size, wse_zero_transform());
                break;
            }
            return;
        }
        if (depth == target_dim) {
            const std::size_t old_size = slice.children.size();
            slice.children.resize(new_size);
            const auto child_dims =
                std::span<const std::int32_t>(dim_sizes).subspan(depth + 1);
            for (std::size_t i = old_size; i < new_size; ++i) {
                slice.children[i] = make_slice(type, child_dims);
            }
            return;
        }
        for (auto& child : slice.children) {
            if (child) {
                resize_recursive(*child, depth + 1, target_dim, new_size);
            }
        }
    }
};

enum class WseObjectKind : std::uint8_t {
    empty = 0,
    dictionary,
    array,
};

struct WseObjectSlot {
    WseObjectKind kind = WseObjectKind::empty;
    std::unique_ptr<WseDictionary> dictionary{};
    std::unique_ptr<WseDynArray> array{};
};

// Unified handle allocator mirroring WSEObjectManager
// (0x140698a90, 0x14034e340/3b0, 0x1400737a0/810) using LIFO free-ID reuse
// shared by dictionaries (3200..3222) and dynamic arrays (5000..5022).
struct WseObjectManager {
    std::vector<WseObjectSlot> slots{};
    std::vector<std::int32_t> free_ids{};
    std::filesystem::path root_dir{};

    [[nodiscard]] std::int32_t create() {
        WseObjectSlot slot{};
        slot.kind = WseObjectKind::dictionary;
        slot.dictionary = std::make_unique<WseDictionary>();
        return alloc_slot(std::move(slot));
    }

    [[nodiscard]] std::int32_t create_array(std::unique_ptr<WseDynArray> arr) {
        if (!arr) {
            return -1;
        }
        WseObjectSlot slot{};
        slot.kind = WseObjectKind::array;
        slot.array = std::move(arr);
        return alloc_slot(std::move(slot));
    }

    void destroy(std::int32_t id) noexcept {
        release_slot(id, WseObjectKind::dictionary);
    }

    void destroy_array(std::int32_t id) noexcept {
        release_slot(id, WseObjectKind::array);
    }

    [[nodiscard]] WseDictionary* get(std::int32_t id) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::dictionary ? slot.dictionary.get()
                                                      : nullptr;
    }

    [[nodiscard]] const WseDictionary* get(std::int32_t id) const noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::dictionary ? slot.dictionary.get()
                                                      : nullptr;
    }

    [[nodiscard]] WseDynArray* get_array(std::int32_t id) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::array ? slot.array.get() : nullptr;
    }

    [[nodiscard]] const WseDynArray* get_array(std::int32_t id) const noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::array ? slot.array.get() : nullptr;
    }

private:
    [[nodiscard]] std::int32_t alloc_slot(WseObjectSlot slot) {
        if (!free_ids.empty()) {
            const std::int32_t id = free_ids.back();
            free_ids.pop_back();
            slots[static_cast<std::size_t>(id)] = std::move(slot);
            return id;
        }
        const auto id = static_cast<std::int32_t>(slots.size());
        slots.push_back(std::move(slot));
        return id;
    }

    void release_slot(std::int32_t id, WseObjectKind expected_kind) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        if (slot.kind == expected_kind) {
            slot.dictionary.reset();
            slot.array.reset();
            slot.kind = WseObjectKind::empty;
            free_ids.push_back(id);
        }
    }
};

using WseDictionaryStore = WseObjectManager;

using WseExecuteScriptFn = bool (*)(void* user,
                                    std::int32_t script_index,
                                    std::span<const std::int64_t> params);

// Runtime state read by the script-context query handlers. The defaults model
// a statement executed outside any trigger, with no return values, zero
// application time, and no active mission.
struct WseVmState {
    // param_6 of executeWSE: the statement block's context flags. WSE stores a
    // negative value while inside a trigger (0 outside), so store_current_trigger
    // returns it only when it is <= 0.
    std::int32_t caller = 0;
    // 0x140698af0: NUM_RETURN_VALUES (16) int64 slots written by return_values.
    std::span<const std::int64_t> return_values{};
    // 0x140698ae8: number of slots currently set by return_values.
    std::int32_t num_return_values = 0;
    // DXUtil_Timer(TIMER_GETAPPTIME) in seconds; 3035 scales it by 1000.
    double app_time_seconds = 0.0;
    // [0x14061f078] + 0x9ae8: cur_mission->cur_mission_template_no. A null
    // pointer means there is no current mission, which yields -1.
    const std::int32_t* current_mission_template_no = nullptr;
    // String registers used by the string group (4200-4224, 4233-4244).
    WseStringBanks strings{};
    // Runtime option flags used by options_{get,set}_* (4300-4305).
    WseOptionsState* options = nullptr;
    // Writable return-value buffer/count for 3015 return_values.
    std::span<std::int64_t> return_values_out{};
    std::int32_t* num_return_values_out = nullptr;
    // DAT_140662bb8: script fixed-point scale (default 100).
    std::int32_t fixed_point_multiplier = 100;
    // rglTransform[128] position registers (0x140663030, stride 0x40).
    std::span<WseTransform> positions{};
    // App timers (0x140667b18, 128 slots) and global tick clocks (0x14061f030).
    std::span<WseAppTimer> timers{};
    std::span<const std::int64_t> timers_total{};
    // PRNG and rand_seed state for 3020/3021/3034.
    WseMersenneTwister* rng = nullptr;
    std::int32_t* rand_seed = nullptr;
    // Profiler recording flag and marker log for 3040..3043.
    bool* profiler_recording = nullptr;
    std::vector<std::string>* profiler_markers = nullptr;
    // Unified dictionary/array handle store (WSEObjectManager) for 3200..3222
    // and 5000..5022.
    WseDictionaryStore* dictionaries = nullptr;
    // Script invocation callback and loaded script count for 5016
    // array_sort_custom.
    std::size_t num_scripts = 0;
    WseExecuteScriptFn execute_script_fn = nullptr;
    void* execute_script_user = nullptr;
    // Game-world, mission, config, skill, and edit-mode state.
    wse::game::WseWorldState* world = nullptr;
};

// Decoded operands, matching the arrays built by mbStatement::execute.
struct WseOperands {
    std::span<const std::int64_t> wide{};     // param_3, full int64 values
    std::span<const std::int32_t> narrow{};   // param_4, int32 truncations
    std::span<const std::uint8_t> tags{};     // param_5, parseOperand tags
    std::span<float> fp{};                    // DAT_140698718, 128 floats
    // g_basicGame+0x270 (0x140662c30): the 128 int64 registers shared with
    // operand tag 1 and used by register_get/register_set (3000/3001).
    std::span<std::int64_t> registers{};
    // Script-context state for the global query handlers (3014-3600).
    WseVmState state{};
};

// Result of a successful handler. Value-writing handlers (mbStatement's
// common LAB_1401e0e0e tail) write effect.value through operand 0; the
// floating-point and comparison handlers write their own destinations.
struct WseEffect {
    std::int64_t value = 0;
    bool store_value = true;
};

enum class WseDispatch : std::uint8_t {
    handled = 0,                 // success; write effect.value through operand 0
    handled_no_store,            // success; handler wrote its own destination
    condition_false,             // executeWSE returned false (failed condition)
    ignored,                     // success; retail returned true without effect
    unimplemented,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(WseDispatch result) noexcept {
    switch (result) {
    case WseDispatch::handled:
        return "handled";
    case WseDispatch::handled_no_store:
        return "handled without operand-0 store";
    case WseDispatch::condition_false:
        return "false condition";
    case WseDispatch::ignored:
        return "ignored (invalid index)";
    case WseDispatch::unimplemented:
        return "unimplemented WSE opcode";
    case WseDispatch::operand_count_out_of_range:
        return "WSE opcode operand count out of range";
    }
    return "unknown WSE dispatch result";
}

// The shift amount is `(byte)operand & 0x3F`, exactly as the retail handlers
// use it (x86-64 masks a 64-bit shift by 63).
[[nodiscard]] constexpr unsigned wse_shift(std::int64_t value) noexcept {
    return static_cast<unsigned>(value & 0x3F);
}

// mbCheckRegister: unsigned comparison against 0x7f.
[[nodiscard]] constexpr bool wse_register_ok(std::int32_t index) noexcept {
    return static_cast<std::uint32_t>(index) <= 0x7Fu;
}

// rglRound64 (0x1402a1910): floorf(x), plus one when the fraction is > 0.5.
// Out-of-range values become INT64_MIN because the retail conversion is
// cvttss2si; this avoids undefined behaviour on the host.
[[nodiscard]] inline std::int64_t wse_round64(float value) noexcept {
    const float base = std::floor(value);
    if (!std::isfinite(base) ||
        base < static_cast<float>(std::numeric_limits<std::int64_t>::min()) ||
        base >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    auto result = static_cast<std::int64_t>(base);
    if (value - base > 0.5f) {
        ++result;
    }
    return result;
}

// cvttsd2si semantics: NaN and out-of-range values become INT64_MIN.
[[nodiscard]] inline std::int64_t wse_trunc64(double value) noexcept {
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

// mbCheckStringRegister (0x1401c51f0): tags 3 and 0x16 are bounded by the
// runtime register counts, tag 0 by the fixed 128-entry literal table, and
// unknown tags are accepted.
[[nodiscard]] inline bool wse_string_register_ok(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    switch (tag) {
    case 0:
        return static_cast<std::uint32_t>(index) <= 0x7Fu;
    case 3:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.global.size();
    case 0x16:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.local.size();
    default:
        return true;
    }
}

// 0x140219c70: tag 0 reads the string register table, tag 0x16 the local
// bank, and tag 3 or an unknown tag the global bank. Out-of-range reads
// resolve to the static empty rgl::string (0x140621818 / 0x140621788).
[[nodiscard]] inline std::u16string_view wse_resolve_string(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    if (tag == 0) {
        if (static_cast<std::uint32_t>(index) <= 0x7Fu &&
            static_cast<std::size_t>(index) < banks.registers.size()) {
            return banks.registers[static_cast<std::size_t>(index)];
        }
        return {};
    }
    const auto bank = tag == 0x16 ? banks.local : banks.global;
    if (index >= 0 && static_cast<std::size_t>(index) < bank.size()) {
        return bank[static_cast<std::size_t>(index)];
    }
    return {};
}

// 0x14030fd60 assigns a tag-0 string register. A missing table entry cannot
// be written, so the handler reports the retail invalid-register no-op.
[[nodiscard]] inline bool wse_store_string(WseStringBanks& banks,
                                           std::int32_t index,
                                           std::u16string_view value) {
    if (index < 0 || static_cast<std::uint32_t>(index) > 0x7Fu ||
        static_cast<std::size_t>(index) >= banks.registers.size()) {
        return false;
    }
    banks.registers[static_cast<std::size_t>(index)].assign(value);
    return true;
}

// Missing tags and operands read as zero: mbStatement::execute zeroes the
// first five tag and operand slots before parsing (0x1401c6219-0x1401c626c).
[[nodiscard]] constexpr std::uint8_t wse_tag_at(
    std::span<const std::uint8_t> tags, std::size_t index) noexcept {
    return index < tags.size() ? tags[index] : std::uint8_t{0};
}

// _wcsicmp folds one UTF-16 code unit through towlower; keep ASCII folding
// deterministic and fall back to the C library for the rest of the BMP.
[[nodiscard]] inline char16_t wse_fold_case(char16_t value) noexcept {
    if (value >= u'A' && value <= u'Z') {
        return static_cast<char16_t>(value + (u'a' - u'A'));
    }
    if (value < 0x80) {
        return value;
    }
    const long folded =
        static_cast<long>(std::towlower(static_cast<std::wint_t>(value)));
    if (folded >= 0 && folded <= 0xFFFF) {
        return static_cast<char16_t>(folded);
    }
    return value;
}

[[nodiscard]] inline bool wse_string_equals(std::u16string_view lhs,
                                            std::u16string_view rhs,
                                            bool case_insensitive) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const char16_t left = case_insensitive ? wse_fold_case(lhs[i]) : lhs[i];
        const char16_t right = case_insensitive ? wse_fold_case(rhs[i]) : rhs[i];
        if (left != right) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::u16string wse_fold_string(std::u16string_view text) {
    std::u16string folded;
    folded.reserve(text.size());
    for (const char16_t value : text) {
        folded.push_back(wse_fold_case(value));
    }
    return folded;
}

[[nodiscard]] inline bool wse_string_contains(std::u16string_view haystack,
                                              std::u16string_view needle,
                                              bool case_insensitive) {
    if (needle.empty()) {
        // The retail find receives a null data pointer for an empty needle and
        // returns -1 (0x1069).
        return false;
    }
    if (case_insensitive) {
        return wse_fold_string(haystack).find(wse_fold_string(needle)) !=
               std::u16string::npos;
    }
    return haystack.find(needle) != std::u16string_view::npos;
}

[[nodiscard]] inline bool wse_string_starts_with(
    std::u16string_view haystack, std::u16string_view needle,
    bool case_insensitive) noexcept {
    return needle.size() <= haystack.size() &&
           wse_string_equals(haystack.substr(0, needle.size()), needle,
                             case_insensitive);
}

[[nodiscard]] inline bool wse_string_ends_with(std::u16string_view haystack,
                                               std::u16string_view needle,
                                               bool case_insensitive) noexcept {
    return needle.size() <= haystack.size() &&
           wse_string_equals(haystack.substr(haystack.size() - needle.size()),
                             needle, case_insensitive);
}

// _wtof (0x140498878): the CRT parses the longest decimal/inf/nan prefix and
// returns 0.0 when there is none. strtod gives the same correctly-rounded
// result; the numeric grammar is ASCII.
[[nodiscard]] inline double wse_wtof(std::u16string_view text) noexcept {
    std::string narrow;
    narrow.reserve(text.size());
    for (const char16_t value : text) {
        if (value == 0 || value > 0x7F) {
            break;
        }
        narrow.push_back(static_cast<char>(value));
    }
    return std::strtod(narrow.c_str(), nullptr);
}

// towupper counterpart of wse_fold_case for str_store_upper (0x1401f8ac0).
[[nodiscard]] inline char16_t wse_upper_case(char16_t value) noexcept {
    if (value >= u'a' && value <= u'z') {
        return static_cast<char16_t>(value - (u'a' - u'A'));
    }
    if (value < 0x80) {
        return value;
    }
    const long folded =
        static_cast<long>(std::towupper(static_cast<std::wint_t>(value)));
    if (folded >= 0 && folded <= 0xFFFF) {
        return static_cast<char16_t>(folded);
    }
    return value;
}

// mbString comparison behind str_compare (0x1074). The case-sensitive wrapper
// 0x1401f86b0 normalises to -1/0/1; the case-insensitive path tail-jumps to
// _wcsicmp (0x1404989d4), which returns the folded code-unit difference.
[[nodiscard]] inline int wse_string_compare(std::u16string_view lhs,
                                            std::u16string_view rhs,
                                            bool case_insensitive) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < common; ++i) {
        const char16_t left =
            case_insensitive ? wse_fold_case(lhs[i]) : lhs[i];
        const char16_t right =
            case_insensitive ? wse_fold_case(rhs[i]) : rhs[i];
        if (left != right) {
            if (case_insensitive) {
                return static_cast<int>(left) - static_cast<int>(right);
            }
            return left < right ? -1 : 1;
        }
    }
    if (lhs.size() == rhs.size()) {
        return 0;
    }
    if (!case_insensitive) {
        return lhs.size() < rhs.size() ? -1 : 1;
    }
    const char16_t left =
        lhs.size() > common ? wse_fold_case(lhs[common]) : char16_t{0};
    const char16_t right =
        rhs.size() > common ? wse_fold_case(rhs[common]) : char16_t{0};
    return static_cast<int>(left) - static_cast<int>(right);
}

// 0x1401f8710: replace every non-overlapping occurrence of `needle`, resuming
// after the inserted replacement. A null/empty needle returns the input
// unchanged (the retail function returns before touching the string).
[[nodiscard]] inline std::u16string wse_replace_all(
    std::u16string text, std::u16string_view needle,
    std::u16string_view replacement) {
    if (needle.empty()) {
        return text;
    }
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::u16string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return text;
}

[[nodiscard]] inline bool wse_is_space(char16_t value) noexcept {
    return std::iswspace(static_cast<std::wint_t>(value)) != 0;
}

// 0x1401f8d00 removes leading whitespace.
inline void wse_trim_left(std::u16string& text) {
    std::size_t first = 0;
    while (first < text.size() && wse_is_space(text[first])) {
        ++first;
    }
    text.erase(0, first);
}

// 0x1401f8c40 removes trailing whitespace.
inline void wse_trim_right(std::u16string& text) {
    std::size_t last = text.size();
    while (last > 0 && wse_is_space(text[last - 1])) {
        --last;
    }
    text.erase(last);
}

// 0x1401f8e20 trims both ends (right first, then left).
inline void wse_trim(std::u16string& text) {
    wse_trim_right(text);
    wse_trim_left(text);
}

// Encodes UTF-16 into UTF-8 (matching 0x14030fd60 / CP_UTF8 0xfde9), replacing
// lone surrogates with U+FFFD.
[[nodiscard]] inline std::string wse_utf16_to_utf8(std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(text[i]);
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i + 1 < text.size()) {
                const std::uint32_t low =
                    static_cast<std::uint16_t>(text[i + 1]);
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    ++i;
                } else {
                    cp = 0xFFFDu;
                }
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

// Decodes UTF-8 into UTF-16 (matching MultiByteToWideChar CP_UTF8 0xfde9 in
// 0x140310970 / 0x1403100d0), replacing malformed sequences with U+FFFD.
[[nodiscard]] inline std::u16string wse_utf8_to_utf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<std::uint8_t>(text[i]);
        if (b0 < 0x80u) {
            out.push_back(static_cast<char16_t>(b0));
            ++i;
            continue;
        }
        std::uint32_t cp = 0xFFFDu;
        std::size_t len = 1;
        if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x1Fu) << 6) |
                    (static_cast<std::uint32_t>(b1) & 0x3Fu);
                if (decoded >= 0x80u) {
                    cp = decoded;
                    len = 2;
                }
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x0Fu) << 12) |
                    ((static_cast<std::uint32_t>(b1) & 0x3Fu) << 6) |
                    (static_cast<std::uint32_t>(b2) & 0x3Fu);
                if (decoded >= 0x800u &&
                    (decoded < 0xD800u || decoded > 0xDFFFu)) {
                    cp = decoded;
                    len = 3;
                }
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2]);
            const auto b3 = static_cast<std::uint8_t>(text[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u &&
                (b3 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x07u) << 18) |
                    ((static_cast<std::uint32_t>(b1) & 0x3Fu) << 12) |
                    ((static_cast<std::uint32_t>(b2) & 0x3Fu) << 6) |
                    (static_cast<std::uint32_t>(b3) & 0x3Fu);
                if (decoded >= 0x10000u && decoded <= 0x10FFFFu) {
                    cp = decoded;
                    len = 4;
                }
            }
        }
        i += len;
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const std::uint32_t adjusted = cp - 0x10000u;
            out.push_back(
                static_cast<char16_t>(0xD800u + (adjusted >> 10)));
            out.push_back(
                static_cast<char16_t>(0xDC00u + (adjusted & 0x3FFu)));
        }
    }
    return out;
}

// RFC 1321 MD5 returning 32 lowercase hex digits ("%.2x" / 0x14056bca8).
[[nodiscard]] inline std::u16string wse_md5_hex(std::string_view input) {
    static constexpr std::array<std::uint32_t, 64> kShifts = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static constexpr std::array<std::uint32_t, 64> kConstants = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
        0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
        0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
        0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
        0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
        0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
        0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
        0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };

    std::uint32_t a0 = 0x67452301u;
    std::uint32_t b0 = 0xefcdab89u;
    std::uint32_t c0 = 0x98badcfeu;
    std::uint32_t d0 = 0x10325476u;

    auto process_block = [&](const std::uint8_t* block) {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5u * i + 1u) & 15u;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3u * i + 5u) & 15u;
            } else {
                f = c ^ (b | (~d));
                g = (7u * i) & 15u;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            const std::uint32_t sum = a + f + kConstants[i] + m[g];
            const std::uint32_t s = kShifts[i];
            b = b + ((sum << s) | (sum >> (32u - s)));
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const std::size_t full_blocks = input.size() / 64u;
    for (std::size_t block_index = 0; block_index < full_blocks; ++block_index) {
        process_block(reinterpret_cast<const std::uint8_t*>(input.data()) +
                      block_index * 64u);
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = input.size() - full_blocks * 64u;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] = static_cast<std::uint8_t>(input[full_blocks * 64u + i]);
    }
    tail[remainder] = 0x80u;
    const std::size_t padded_bytes = remainder < 56u ? 64u : 128u;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8u;
    for (std::size_t i = 0; i < 8u; ++i) {
        tail[padded_bytes - 8u + i] =
            static_cast<std::uint8_t>((bit_length >> (i * 8u)) & 0xFFu);
    }
    process_block(tail.data());
    if (padded_bytes == 128u) {
        process_block(tail.data() + 64u);
    }

    static constexpr std::u16string_view kHexDigits = u"0123456789abcdef";
    std::u16string hex;
    hex.reserve(32);
    for (const std::uint32_t word : {a0, b0, c0, d0}) {
        for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
            const auto byte =
                static_cast<std::uint8_t>((word >> (byte_index * 8u)) & 0xFFu);
            hex.push_back(kHexDigits[byte >> 4u]);
            hex.push_back(kHexDigits[byte & 0x0Fu]);
        }
    }
    return hex;
}

// 0x1401ef436: strips "{", "}", "<", ">" from a tag-0 string register.
[[nodiscard]] inline std::u16string wse_sanitize_string(std::u16string_view text) {
    std::u16string cleaned;
    cleaned.reserve(text.size());
    for (const char16_t ch : text) {
        if (ch != u'{' && ch != u'}' && ch != u'<' && ch != u'>') {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

[[nodiscard]] inline int wse_hex_nibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

// Models SHLWAPI.dll!UrlUnescapeA(..., dwFlags=0) in 0x1403108b0: decodes %XX
// hex bytes, preserves '+' and malformed '%' sequences, and terminates on NUL.
[[nodiscard]] inline std::string wse_url_unescape(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '\0') {
            break;
        }
        if (ch == '%' && i + 2 < input.size()) {
            const int hi = wse_hex_nibble(input[i + 1]);
            const int lo = wse_hex_nibble(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                const auto decoded = static_cast<std::uint8_t>((hi << 4) | lo);
                if (decoded == 0) {
                    break;
                }
                out.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        out.push_back(ch);
    }
    return out;
}

// 0x140200c40: formats (double)value with "%.*f" (0x14055b3e4).
[[nodiscard]] inline std::u16string wse_format_float(
    float value, std::int32_t precision) {
    const int prec = precision < 0 ? 5 : static_cast<int>(precision);
    std::array<char, 512> stack_buf{};
    const int needed =
        std::snprintf(stack_buf.data(), stack_buf.size(), "%.*f", prec,
                      static_cast<double>(value));
    if (needed < 0) {
        return {};
    }
    if (static_cast<std::size_t>(needed) < stack_buf.size()) {
        return wse_utf8_to_utf16(
            std::string_view(stack_buf.data(), static_cast<std::size_t>(needed)));
    }
    std::string dynamic_buf(static_cast<std::size_t>(needed) + 1u, '\0');
    const int written =
        std::snprintf(dynamic_buf.data(), dynamic_buf.size(), "%.*f", prec,
                      static_cast<double>(value));
    if (written < 0) {
        return {};
    }
    return wse_utf8_to_utf16(
        std::string_view(dynamic_buf.data(), static_cast<std::size_t>(written)));
}

// Shared ECMAScript std::regex predicate for str_regex_match (4240) and
// str_regex_search (4241). Invalid patterns throw std::regex_error, caught to
// match the retail SEH result false.
[[nodiscard]] inline WseDispatch wse_regex_predicate(
    std::u16string_view text, std::u16string_view pattern,
    bool full_match) noexcept {
    try {
        const std::string text_utf8 = wse_utf16_to_utf8(text);
        const std::regex re(wse_utf16_to_utf8(pattern));
        const bool matched = full_match ? std::regex_match(text_utf8, re)
                                        : std::regex_search(text_utf8, re);
        return matched ? WseDispatch::handled_no_store
                       : WseDispatch::condition_false;
    } catch (...) {
        return WseDispatch::condition_false;
    }
}

[[nodiscard]] inline bool* wse_option_flag(WseOptionsState* options,
                                           std::uint32_t opcode) noexcept {
    if (!options) {
        return nullptr;
    }
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::options_get_verbose_casualties:
    case WseOpcode::options_set_verbose_casualties:
        return &options->verbose_casualties;
    case WseOpcode::options_get_cheat_mode:
    case WseOpcode::options_set_cheat_mode:
        return &options->cheat_mode;
    case WseOpcode::options_get_realistic_headshots:
    case WseOpcode::options_set_realistic_headshots:
        return &options->realistic_headshots;
    case WseOpcode::set_horse_friendly_fire:
        return &options->horse_friendly_fire;
    case WseOpcode::set_show_crosshair:
        return &options->show_crosshair;
    case WseOpcode::edit_mode_set_enabled:
        return &options->edit_mode_enabled;
    default:
        return nullptr;
    }
}

// Shared body of str_is_alpha/digit/whitespace. `has_index == false` or
// index -1 scans the whole string; an out-of-bounds index logs and returns
// the initial true result.
template <typename Predicate>
[[nodiscard]] inline WseDispatch wse_string_test(std::u16string_view text,
                                                 std::int32_t index,
                                                 bool has_index,
                                                 Predicate predicate) noexcept {
    if (!has_index || index == -1) {
        for (const char16_t value : text) {
            if (!predicate(value)) {
                return WseDispatch::condition_false;
            }
        }
        return WseDispatch::handled_no_store;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= text.size()) {
        return WseDispatch::ignored;
    }
    return predicate(text[static_cast<std::size_t>(index)])
               ? WseDispatch::handled_no_store
               : WseDispatch::condition_false;
}

[[nodiscard]] constexpr std::int64_t wse_days_from_civil(
    std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Mirrors 3022 local time conversion (_localtime64_s + _mkgmtime64).
[[nodiscard]] inline std::int64_t wse_local_epoch_seconds(
    std::time_t utc_time) noexcept {
    std::tm local_tm{};
#ifdef _WIN32
    if (localtime_s(&local_tm, &utc_time) != 0) {
        return static_cast<std::int64_t>(utc_time);
    }
#else
    if (localtime_r(&utc_time, &local_tm) == nullptr) {
        return static_cast<std::int64_t>(utc_time);
    }
#endif
    const std::int64_t days = wse_days_from_civil(
        static_cast<std::int64_t>(local_tm.tm_year) + 1900,
        static_cast<std::int64_t>(local_tm.tm_mon) + 1,
        static_cast<std::int64_t>(local_tm.tm_mday));
    return days * 86400 + static_cast<std::int64_t>(local_tm.tm_hour) * 3600 +
           static_cast<std::int64_t>(local_tm.tm_min) * 60 +
           static_cast<std::int64_t>(local_tm.tm_sec);
}

[[nodiscard]] inline std::string_view wse_trim_ascii(
    std::string_view sv) noexcept {
    while (!sv.empty() &&
           std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           std::isspace(static_cast<unsigned char>(sv.back())) != 0) {
        sv.remove_suffix(1);
    }
    return sv;
}

// Mirrors mbCheckPath (0x1401c5640): rejects "<>:\"/\\|?*" (0x14055ac20),
// trims whitespace, strips leading '.', trims again, and rejects empty names.
[[nodiscard]] inline bool wse_check_path(std::string& name) {
    constexpr std::string_view kBadChars = "<>:\"/\\|?*";
    for (const char ch : name) {
        if (ch == '\0' || kBadChars.find(ch) != std::string_view::npos) {
            return false;
        }
    }
    std::string_view sv = wse_trim_ascii(name);
    while (!sv.empty() && sv.front() == '.') {
        sv.remove_prefix(1);
    }
    sv = wse_trim_ascii(sv);
    if (sv.empty() || sv == "." || sv == "..") {
        return false;
    }
    name.assign(sv);
    return true;
}

[[nodiscard]] inline bool wse_resolve_utf8_operand(
    std::size_t idx, const WseOperands& operands, std::string& out) {
    if (idx >= operands.narrow.size()) {
        return false;
    }
    const std::uint8_t tag = wse_tag_at(operands.tags, idx);
    if (!wse_string_register_ok(operands.narrow[idx], tag,
                                operands.state.strings)) {
        return false;
    }
    out = wse_utf16_to_utf8(wse_resolve_string(
        operands.narrow[idx], tag, operands.state.strings));
    return true;
}

[[nodiscard]] inline std::string wse_url_escape(std::string_view input) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (const unsigned char uc : input) {
        const bool unreserved =
            (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') ||
            (uc >= '0' && uc <= '9') || uc == '-' || uc == '_' || uc == '.' ||
            uc == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(uc));
        } else {
            out.push_back('%');
            out.push_back(kHex[(uc >> 4U) & 0x0fU]);
            out.push_back(kHex[uc & 0x0fU]);
        }
    }
    return out;
}

inline void wse_append_utf8_codepoint(std::string& out,
                                      std::uint32_t cp) {
    if (cp <= 0x7fU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | ((cp >> 6U) & 0x1fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | ((cp >> 12U) & 0x0fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0x10ffffU) {
        out.push_back(static_cast<char>(0xf0U | ((cp >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
}

inline void wse_append_json_string(std::string& out, std::string_view sv) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char uc : sv) {
        switch (uc) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (uc < 0x20U) {
                out.append("\\u00");
                out.push_back(kHex[(uc >> 4U) & 0x0fU]);
                out.push_back(kHex[uc & 0x0fU]);
            } else {
                out.push_back(static_cast<char>(uc));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] inline std::string wse_serialize_json_object(
    const std::map<std::string, std::string>& entries, bool pretty) {
    if (entries.empty()) {
        return pretty ? "{}\n" : "{}";
    }
    std::string out;
    out.push_back('{');
    if (pretty) {
        out.push_back('\n');
    }
    bool first = true;
    for (const auto& [key, value] : entries) {
        if (!first) {
            out.push_back(',');
            if (pretty) {
                out.push_back('\n');
            }
        }
        first = false;
        if (pretty) {
            out.append("  ");
        }
        wse_append_json_string(out, key);
        out.push_back(':');
        if (pretty) {
            out.push_back(' ');
        }
        wse_append_json_string(out, value);
    }
    if (pretty) {
        out.append("\n}\n");
    } else {
        out.push_back('}');
    }
    return out;
}

[[nodiscard]] inline bool wse_parse_unsigned_index(
    std::u16string_view sv,
    std::size_t& pos,
    std::size_t& out_index) noexcept {
    if (pos >= sv.size() || sv[pos] < u'0' || sv[pos] > u'9') {
        return false;
    }
    std::size_t val = 0;
    while (pos < sv.size() && sv[pos] >= u'0' && sv[pos] <= u'9') {
        const auto digit = static_cast<std::size_t>(sv[pos] - u'0');
        if (val <= 1000000U) {
            val = val * 10U + digit;
        }
        ++pos;
    }
    out_index = val;
    return true;
}

[[nodiscard]] inline std::u16string wse_parse_string(
    std::u16string_view input,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks);

// Mirrors mbBasicGame::parseToken / parseTokenReg / parseTokenStr
// (0x1400723a0..0x140072b73): strips "!" / "***" comments, expands "reg<N>"
// or "reg<N>?<then>:<else>" (backward ':' scan >= '?'), and returns raw s[N].
[[nodiscard]] inline std::u16string wse_parse_token(
    std::u16string_view token,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks) {
    if (token.empty() || token.starts_with(u"!") ||
        token.starts_with(u"***")) {
        return {};
    }
    if (token.starts_with(u"s")) {
        std::size_t pos = 1;
        std::size_t index = 0;
        if (wse_parse_unsigned_index(token, pos, index) &&
            pos == token.size() && index < banks.registers.size()) {
            return banks.registers[index];
        }
    }
    if (token.starts_with(u"reg")) {
        std::size_t pos = 3;
        std::size_t index = 0;
        if (wse_parse_unsigned_index(token, pos, index) &&
            index < registers.size()) {
            if (pos == token.size()) {
                const std::string dec = std::to_string(registers[index]);
                return wse_utf8_to_utf16(dec);
            }
            if (token[pos] == u'?') {
                std::size_t colon = std::u16string_view::npos;
                std::size_t brace_depth = 0;
                for (std::size_t k = token.size(); k > pos + 1; --k) {
                    const char16_t ch = token[k - 1];
                    if (ch == u'}') {
                        ++brace_depth;
                    } else if (ch == u'{') {
                        if (brace_depth > 0) {
                            --brace_depth;
                        }
                    } else if (ch == u':' && brace_depth == 0) {
                        colon = k - 1;
                        break;
                    }
                }
                if (colon != std::u16string_view::npos) {
                    const std::u16string_view branch =
                        registers[index] != 0
                            ? token.substr(pos + 1, colon - (pos + 1))
                            : token.substr(colon + 1);
                    return wse_parse_string(branch, registers, banks);
                }
            }
        }
    }
    std::u16string out = u"UNRECOGNIZED TOKEN <";
    out.append(token);
    out.push_back(u'>');
    return out;
}

// Mirrors mbBasicGame::parseString (0x1400720b0): turns '^' into '\n' and
// evaluates balanced {...} tokens; expanded s[N] text is NOT re-parsed.
[[nodiscard]] inline std::u16string wse_parse_string(
    std::u16string_view input,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks) {
    std::u16string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        const char16_t ch = input[i];
        if (ch == u'^') {
            out.push_back(u'\n');
            ++i;
            continue;
        }
        if (ch == u'{') {
            std::size_t depth = 1;
            std::size_t j = i + 1;
            while (j < input.size() && depth > 0) {
                if (input[j] == u'{') {
                    ++depth;
                } else if (input[j] == u'}') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
                ++j;
            }
            if (depth != 0 || j >= input.size()) {
                break;
            }
            if (j > i + 1) {
                const std::u16string_view token =
                    input.substr(i + 1, j - (i + 1));
                out.append(wse_parse_token(token, registers, banks));
            }
            i = j + 1;
            continue;
        }
        out.push_back(ch);
        ++i;
    }
    return out;
}

[[nodiscard]] inline bool wse_resolve_parsed_utf8_operand(
    std::size_t idx, const WseOperands& operands, std::string& out) {
    if (idx >= operands.narrow.size()) {
        return false;
    }
    const std::uint8_t tag = wse_tag_at(operands.tags, idx);
    if (!wse_string_register_ok(operands.narrow[idx], tag,
                                operands.state.strings)) {
        return false;
    }
    const std::u16string_view raw = wse_resolve_string(
        operands.narrow[idx], tag, operands.state.strings);
    const std::u16string expanded =
        wse_parse_string(raw, operands.registers, operands.state.strings);
    out = wse_utf16_to_utf8(expanded);
    return true;
}

enum class WseExprOp : std::uint8_t {
    literal = 0,
    fp_var,
    pos_var,
    unary_neg,
    unary_pos,
    assign,
    sequence,
    ternary,
    log_or,
    log_and,
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    add,
    sub,
    mul,
    div,
    pow,
    fn_sin,
    fn_cos,
    fn_tan,
    fn_asin,
    fn_acos,
    fn_atan,
    fn_sinh,
    fn_cosh,
    fn_tanh,
    fn_asinh,
    fn_acosh,
    fn_atanh,
    fn_log2,
    fn_log10,
    fn_ln,
    fn_exp,
    fn_sqrt,
    fn_sign,
    fn_rint,
    fn_abs,
    fn_deg2rad,
    fn_rad2deg,
    fn_floor,
    fn_ceil,
    fn_fmod,
    fn_atan2,
    fn_clamp,
    fn_sum,
    fn_avg,
    fn_min,
    fn_max,
};

struct WseExprNode {
    WseExprOp op = WseExprOp::literal;
    float literal_value = 0.0f;
    std::uint16_t reg_index = 0;
    std::uint8_t pos_comp = 0;
    std::size_t a = 0;
    std::size_t b = 0;
    std::size_t c = 0;
};

struct WseExprCompiler {
    std::string_view src{};
    std::size_t pos = 0;
    std::vector<WseExprNode> nodes{};
    std::vector<std::size_t> call_args{};
    std::size_t depth = 0;

    static constexpr std::size_t kMaxParseDepth = 256;

    void skip_ws() noexcept {
        while (pos < src.size() &&
               std::isspace(static_cast<unsigned char>(src[pos])) != 0) {
            ++pos;
        }
    }

    [[nodiscard]] std::size_t add_node(WseExprNode node) {
        const std::size_t id = nodes.size();
        nodes.push_back(node);
        return id;
    }

    [[nodiscard]] static bool parse_uint_suffix(
        std::string_view sv, std::size_t& cur, std::uint16_t& out) noexcept {
        if (cur >= sv.size() || sv[cur] < '0' || sv[cur] > '9') {
            return false;
        }
        std::uint32_t v = 0;
        while (cur < sv.size() && sv[cur] >= '0' && sv[cur] <= '9') {
            v = v * 10U + static_cast<std::uint32_t>(sv[cur] - '0');
            if (v > 127U) {
                return false;
            }
            ++cur;
        }
        out = static_cast<std::uint16_t>(v);
        return true;
    }

    [[nodiscard]] static bool match_pos_var(
        std::string_view ident,
        std::uint16_t& reg_out,
        std::uint8_t& comp_out) noexcept {
        if (!ident.starts_with("pos")) {
            return false;
        }
        std::size_t cur = 3;
        if (!parse_uint_suffix(ident, cur, reg_out)) {
            return false;
        }
        const std::string_view comp = ident.substr(cur);
        constexpr std::array<std::string_view, 12> kComps = {
            "sx", "sy", "sz", "fx", "fy", "fz",
            "ux", "uy", "uz", "x",  "y",  "z"};
        for (std::size_t i = 0; i < kComps.size(); ++i) {
            if (comp == kComps[i]) {
                comp_out = static_cast<std::uint8_t>(i);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_number(std::size_t& node_out) {
        const std::size_t start = pos;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            std::size_t epos = pos + 1;
            if (epos < src.size() && (src[epos] == '+' || src[epos] == '-')) {
                ++epos;
            }
            if (epos < src.size() && src[epos] >= '0' && src[epos] <= '9') {
                pos = epos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                    ++pos;
                }
            }
        }
        const std::string token(src.substr(start, pos - start));
        char* endptr = nullptr;
        const float val = std::strtof(token.c_str(), &endptr);
        if (endptr != token.data() + token.size()) {
            return false;
        }
        WseExprNode node{};
        node.op = WseExprOp::literal;
        node.literal_value = val;
        node_out = add_node(node);
        return true;
    }

    [[nodiscard]] static bool lookup_builtin(
        std::string_view name,
        WseExprOp& op_out,
        std::size_t& min_args,
        std::size_t& max_args) noexcept {
        struct FuncEntry {
            std::string_view name;
            WseExprOp op;
            std::size_t min_a;
            std::size_t max_a;
        };
        constexpr std::size_t kMaxArgs = std::numeric_limits<std::size_t>::max();
        constexpr std::array<FuncEntry, 32> kTable = {{
            {"sin", WseExprOp::fn_sin, 1, 1},
            {"cos", WseExprOp::fn_cos, 1, 1},
            {"tan", WseExprOp::fn_tan, 1, 1},
            {"asin", WseExprOp::fn_asin, 1, 1},
            {"acos", WseExprOp::fn_acos, 1, 1},
            {"atan", WseExprOp::fn_atan, 1, 1},
            {"sinh", WseExprOp::fn_sinh, 1, 1},
            {"cosh", WseExprOp::fn_cosh, 1, 1},
            {"tanh", WseExprOp::fn_tanh, 1, 1},
            {"asinh", WseExprOp::fn_asinh, 1, 1},
            {"acosh", WseExprOp::fn_acosh, 1, 1},
            {"atanh", WseExprOp::fn_atanh, 1, 1},
            {"log2", WseExprOp::fn_log2, 1, 1},
            {"log10", WseExprOp::fn_log10, 1, 1},
            {"log", WseExprOp::fn_ln, 1, 1},
            {"ln", WseExprOp::fn_ln, 1, 1},
            {"exp", WseExprOp::fn_exp, 1, 1},
            {"sqrt", WseExprOp::fn_sqrt, 1, 1},
            {"sign", WseExprOp::fn_sign, 1, 1},
            {"rint", WseExprOp::fn_rint, 1, 1},
            {"abs", WseExprOp::fn_abs, 1, 1},
            {"deg2rad", WseExprOp::fn_deg2rad, 1, 1},
            {"rad2deg", WseExprOp::fn_rad2deg, 1, 1},
            {"floor", WseExprOp::fn_floor, 1, 1},
            {"ceil", WseExprOp::fn_ceil, 1, 1},
            {"fmod", WseExprOp::fn_fmod, 2, 2},
            {"atan2", WseExprOp::fn_atan2, 2, 2},
            {"clamp", WseExprOp::fn_clamp, 3, 3},
            {"sum", WseExprOp::fn_sum, 1, kMaxArgs},
            {"avg", WseExprOp::fn_avg, 1, kMaxArgs},
            {"min", WseExprOp::fn_min, 1, kMaxArgs},
            {"max", WseExprOp::fn_max, 1, kMaxArgs},
        }};
        for (const auto& entry : kTable) {
            if (name == entry.name) {
                op_out = entry.op;
                min_args = entry.min_a;
                max_args = entry.max_a;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_primary(std::size_t& node_out) {
        if (++depth > kMaxParseDepth) {
            return false;
        }
        struct DepthPop {
            std::size_t& d;
            ~DepthPop() noexcept { --d; }
        } pop{depth};

        skip_ws();
        if (pos >= src.size()) {
            return false;
        }
        const char ch = src[pos];
        if (ch == '-') {
            ++pos;
            std::size_t inner = 0;
            if (!parse_prec(9, inner)) {
                return false;
            }
            node_out = add_node({WseExprOp::unary_neg, 0.0f, 0, 0, inner, 0, 0});
            return true;
        }
        if (ch == '+') {
            ++pos;
            std::size_t inner = 0;
            if (!parse_prec(9, inner)) {
                return false;
            }
            node_out = add_node({WseExprOp::unary_pos, 0.0f, 0, 0, inner, 0, 0});
            return true;
        }
        if (ch == '(') {
            ++pos;
            if (!parse_prec(0, node_out)) {
                return false;
            }
            skip_ws();
            if (pos >= src.size() || src[pos] != ')') {
                return false;
            }
            ++pos;
            return true;
        }
        if ((ch >= '0' && ch <= '9') ||
            (ch == '.' && pos + 1 < src.size() && src[pos + 1] >= '0' &&
             src[pos + 1] <= '9')) {
            return parse_number(node_out);
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            ch == '_') {
            const std::size_t start = pos;
            while (pos < src.size()) {
                const char c = src[pos];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                    ++pos;
                } else {
                    break;
                }
            }
            const std::string_view ident = src.substr(start, pos - start);
            skip_ws();
            if (pos < src.size() && src[pos] == '(') {
                WseExprOp fn_op{};
                std::size_t min_a = 0;
                std::size_t max_a = 0;
                if (!lookup_builtin(ident, fn_op, min_a, max_a)) {
                    return false;
                }
                ++pos;
                std::vector<std::size_t> args;
                skip_ws();
                if (pos < src.size() && src[pos] != ')') {
                    for (;;) {
                        std::size_t arg_node = 0;
                        if (!parse_prec(1, arg_node)) {
                            return false;
                        }
                        args.push_back(arg_node);
                        skip_ws();
                        if (pos >= src.size()) {
                            return false;
                        }
                        if (src[pos] == ')') {
                            break;
                        }
                        if (src[pos] != ',') {
                            return false;
                        }
                        ++pos;
                    }
                }
                if (pos >= src.size() || src[pos] != ')') {
                    return false;
                }
                ++pos;
                if (args.size() < min_a || args.size() > max_a) {
                    return false;
                }
                const std::size_t offset = call_args.size();
                call_args.insert(call_args.end(), args.begin(), args.end());
                WseExprNode node{};
                node.op = fn_op;
                node.a = offset;
                node.b = args.size();
                node_out = add_node(node);
                return true;
            }
            if (ident == "_pi") {
                node_out =
                    add_node({WseExprOp::literal, 3.1415927f, 0, 0, 0, 0, 0});
                return true;
            }
            if (ident == "_e") {
                node_out =
                    add_node({WseExprOp::literal, 2.7182817f, 0, 0, 0, 0, 0});
                return true;
            }
            if (ident.starts_with("fp")) {
                std::size_t cur = 2;
                std::uint16_t idx = 0;
                if (parse_uint_suffix(ident, cur, idx) && cur == ident.size()) {
                    node_out =
                        add_node({WseExprOp::fp_var, 0.0f, idx, 0, 0, 0, 0});
                    return true;
                }
            }
            std::uint16_t pos_idx = 0;
            std::uint8_t pos_comp = 0;
            if (match_pos_var(ident, pos_idx, pos_comp)) {
                node_out = add_node(
                    {WseExprOp::pos_var, 0.0f, pos_idx, pos_comp, 0, 0, 0});
                return true;
            }
            return false;
        }
        return false;
    }

    struct InfixToken {
        int prec = 0;
        bool right_assoc = false;
        std::size_t len = 0;
        WseExprOp op = WseExprOp::add;
    };

    [[nodiscard]] bool peek_infix(InfixToken& tok) const noexcept {
        if (pos >= src.size()) {
            return false;
        }
        const std::string_view rest = src.substr(pos);
        if (rest.starts_with("<=")) {
            tok = {6, false, 2, WseExprOp::le};
            return true;
        }
        if (rest.starts_with(">=")) {
            tok = {6, false, 2, WseExprOp::ge};
            return true;
        }
        if (rest.starts_with("!=")) {
            tok = {5, false, 2, WseExprOp::ne};
            return true;
        }
        if (rest.starts_with("==")) {
            tok = {5, false, 2, WseExprOp::eq};
            return true;
        }
        if (rest.starts_with("&&")) {
            tok = {4, false, 2, WseExprOp::log_and};
            return true;
        }
        if (rest.starts_with("||")) {
            tok = {3, false, 2, WseExprOp::log_or};
            return true;
        }
        switch (rest[0]) {
        case ',':
            tok = {0, false, 1, WseExprOp::sequence};
            return true;
        case '=':
            tok = {1, true, 1, WseExprOp::assign};
            return true;
        case '?':
            tok = {2, true, 1, WseExprOp::ternary};
            return true;
        case '<':
            tok = {6, false, 1, WseExprOp::lt};
            return true;
        case '>':
            tok = {6, false, 1, WseExprOp::gt};
            return true;
        case '+':
            tok = {7, false, 1, WseExprOp::add};
            return true;
        case '-':
            tok = {7, false, 1, WseExprOp::sub};
            return true;
        case '*':
            tok = {8, false, 1, WseExprOp::mul};
            return true;
        case '/':
            tok = {8, false, 1, WseExprOp::div};
            return true;
        case '^':
            tok = {10, true, 1, WseExprOp::pow};
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool parse_prec(int min_prec, std::size_t& lhs_out) {
        if (++depth > kMaxParseDepth) {
            return false;
        }
        struct DepthPop {
            std::size_t& d;
            ~DepthPop() noexcept { --d; }
        } pop{depth};

        if (!parse_primary(lhs_out)) {
            return false;
        }
        for (;;) {
            skip_ws();
            InfixToken tok{};
            if (!peek_infix(tok) || tok.prec < min_prec) {
                break;
            }
            pos += tok.len;
            if (tok.op == WseExprOp::ternary) {
                std::size_t then_node = 0;
                if (!parse_prec(1, then_node)) {
                    return false;
                }
                skip_ws();
                if (pos >= src.size() || src[pos] != ':') {
                    return false;
                }
                ++pos;
                std::size_t else_node = 0;
                if (!parse_prec(tok.prec, else_node)) {
                    return false;
                }
                lhs_out = add_node(
                    {WseExprOp::ternary, 0.0f, 0, 0, lhs_out, then_node,
                     else_node});
                continue;
            }
            const int next_min = tok.right_assoc ? tok.prec : tok.prec + 1;
            std::size_t rhs = 0;
            if (!parse_prec(next_min, rhs)) {
                return false;
            }
            if (tok.op == WseExprOp::assign) {
                const WseExprOp lop = nodes[lhs_out].op;
                if (lop != WseExprOp::fp_var && lop != WseExprOp::pos_var) {
                    return false;
                }
            }
            lhs_out = add_node({tok.op, 0.0f, 0, 0, lhs_out, rhs, 0});
        }
        return true;
    }

    [[nodiscard]] static float* pos_comp_ptr(
        std::span<WseTransform> positions,
        std::size_t index,
        std::uint8_t comp) noexcept {
        if (index >= positions.size()) {
            return nullptr;
        }
        WseTransform& t = positions[index];
        switch (comp) {
        case 0:
            return &t.s.x;
        case 1:
            return &t.s.y;
        case 2:
            return &t.s.z;
        case 3:
            return &t.f.x;
        case 4:
            return &t.f.y;
        case 5:
            return &t.f.z;
        case 6:
            return &t.u.x;
        case 7:
            return &t.u.y;
        case 8:
            return &t.u.z;
        case 9:
            return &t.o.x;
        case 10:
            return &t.o.y;
        case 11:
            return &t.o.z;
        default:
            return nullptr;
        }
    }

    [[nodiscard]] bool eval_node(std::size_t node_id,
                                 std::span<float> fp,
                                 std::span<WseTransform> positions,
                                 float& out_val) const noexcept {
        const WseExprNode& n = nodes[node_id];
        auto arg = [&](std::size_t idx, float& dst) noexcept -> bool {
            return eval_node(call_args[n.a + idx], fp, positions, dst);
        };
        switch (n.op) {
        case WseExprOp::literal:
            out_val = n.literal_value;
            return true;
        case WseExprOp::fp_var:
            if (n.reg_index >= fp.size()) {
                return false;
            }
            out_val = fp[n.reg_index];
            return true;
        case WseExprOp::pos_var: {
            const float* p = pos_comp_ptr(positions, n.reg_index, n.pos_comp);
            if (!p) {
                return false;
            }
            out_val = *p;
            return true;
        }
        case WseExprOp::unary_neg: {
            float v = 0.0f;
            if (!eval_node(n.a, fp, positions, v)) {
                return false;
            }
            out_val = -v;
            return true;
        }
        case WseExprOp::unary_pos:
            return eval_node(n.a, fp, positions, out_val);
        case WseExprOp::assign: {
            float rhs = 0.0f;
            if (!eval_node(n.b, fp, positions, rhs)) {
                return false;
            }
            const WseExprNode& dst = nodes[n.a];
            if (dst.op == WseExprOp::fp_var) {
                if (dst.reg_index >= fp.size()) {
                    return false;
                }
                fp[dst.reg_index] = rhs;
                out_val = rhs;
                return true;
            }
            if (dst.op == WseExprOp::pos_var) {
                float* p = pos_comp_ptr(positions, dst.reg_index, dst.pos_comp);
                if (!p) {
                    return false;
                }
                *p = rhs;
                out_val = rhs;
                return true;
            }
            return false;
        }
        case WseExprOp::sequence: {
            float unused = 0.0f;
            if (!eval_node(n.a, fp, positions, unused)) {
                return false;
            }
            return eval_node(n.b, fp, positions, out_val);
        }
        case WseExprOp::ternary: {
            float cond = 0.0f;
            if (!eval_node(n.a, fp, positions, cond)) {
                return false;
            }
            return eval_node(cond != 0.0f ? n.b : n.c, fp, positions, out_val);
        }
        case WseExprOp::log_or: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            out_val = (l != 0.0f || r != 0.0f) ? 1.0f : 0.0f;
            return true;
        }
        case WseExprOp::log_and: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            out_val = (l != 0.0f && r != 0.0f) ? 1.0f : 0.0f;
            return true;
        }
        case WseExprOp::eq:
        case WseExprOp::ne:
        case WseExprOp::lt:
        case WseExprOp::le:
        case WseExprOp::gt:
        case WseExprOp::ge:
        case WseExprOp::add:
        case WseExprOp::sub:
        case WseExprOp::mul:
        case WseExprOp::div:
        case WseExprOp::pow: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            switch (n.op) {
            case WseExprOp::eq:
                out_val = (l == r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::ne:
                out_val = (l != r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::lt:
                out_val = (l < r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::le:
                out_val = (l <= r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::gt:
                out_val = (l > r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::ge:
                out_val = (l >= r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::add:
                out_val = l + r;
                return true;
            case WseExprOp::sub:
                out_val = l - r;
                return true;
            case WseExprOp::mul:
                out_val = l * r;
                return true;
            case WseExprOp::div:
                out_val = l / r;
                return true;
            case WseExprOp::pow:
                out_val = std::pow(l, r);
                return true;
            default:
                return false;
            }
        }
        case WseExprOp::fn_sin:
        case WseExprOp::fn_cos:
        case WseExprOp::fn_tan:
        case WseExprOp::fn_asin:
        case WseExprOp::fn_acos:
        case WseExprOp::fn_atan:
        case WseExprOp::fn_sinh:
        case WseExprOp::fn_cosh:
        case WseExprOp::fn_tanh:
        case WseExprOp::fn_asinh:
        case WseExprOp::fn_acosh:
        case WseExprOp::fn_atanh:
        case WseExprOp::fn_log2:
        case WseExprOp::fn_log10:
        case WseExprOp::fn_ln:
        case WseExprOp::fn_exp:
        case WseExprOp::fn_sqrt:
        case WseExprOp::fn_sign:
        case WseExprOp::fn_rint:
        case WseExprOp::fn_abs:
        case WseExprOp::fn_deg2rad:
        case WseExprOp::fn_rad2deg:
        case WseExprOp::fn_floor:
        case WseExprOp::fn_ceil: {
            float x = 0.0f;
            if (!arg(0, x)) {
                return false;
            }
            constexpr float kPi = 3.1415927f;
            switch (n.op) {
            case WseExprOp::fn_sin:
                out_val = std::sin(x);
                return true;
            case WseExprOp::fn_cos:
                out_val = std::cos(x);
                return true;
            case WseExprOp::fn_tan:
                out_val = std::tan(x);
                return true;
            case WseExprOp::fn_asin:
                out_val = std::asin(x);
                return true;
            case WseExprOp::fn_acos:
                out_val = std::acos(x);
                return true;
            case WseExprOp::fn_atan:
                out_val = std::atan(x);
                return true;
            case WseExprOp::fn_sinh:
                out_val = std::sinh(x);
                return true;
            case WseExprOp::fn_cosh:
                out_val = std::cosh(x);
                return true;
            case WseExprOp::fn_tanh:
                out_val = std::tanh(x);
                return true;
            case WseExprOp::fn_asinh:
                out_val = std::asinh(x);
                return true;
            case WseExprOp::fn_acosh:
                out_val = std::acosh(x);
                return true;
            case WseExprOp::fn_atanh:
                out_val = std::atanh(x);
                return true;
            case WseExprOp::fn_log2:
                out_val = std::log2(x);
                return true;
            case WseExprOp::fn_log10:
                out_val = std::log10(x);
                return true;
            case WseExprOp::fn_ln:
                out_val = std::log(x);
                return true;
            case WseExprOp::fn_exp:
                out_val = std::exp(x);
                return true;
            case WseExprOp::fn_sqrt:
                out_val = std::sqrt(x);
                return true;
            case WseExprOp::fn_sign:
                out_val = (x < 0.0f) ? -1.0f : (x > 0.0f ? 1.0f : 0.0f);
                return true;
            case WseExprOp::fn_rint:
                out_val = std::floor(x + 0.5f);
                return true;
            case WseExprOp::fn_abs:
                out_val = std::fabs(x);
                return true;
            case WseExprOp::fn_deg2rad:
                out_val = x * (kPi / 180.0f);
                return true;
            case WseExprOp::fn_rad2deg:
                out_val = x * (180.0f / kPi);
                return true;
            case WseExprOp::fn_floor:
                out_val = std::floor(x);
                return true;
            case WseExprOp::fn_ceil:
                out_val = std::ceil(x);
                return true;
            default:
                return false;
            }
        }
        case WseExprOp::fn_fmod: {
            float x = 0.0f;
            float y = 0.0f;
            if (!arg(0, x) || !arg(1, y)) {
                return false;
            }
            out_val = std::fmod(x, y);
            return true;
        }
        case WseExprOp::fn_atan2: {
            float y = 0.0f;
            float x = 0.0f;
            if (!arg(0, y) || !arg(1, x)) {
                return false;
            }
            out_val = std::atan2(y, x);
            return true;
        }
        case WseExprOp::fn_clamp: {
            float v = 0.0f;
            float lo = 0.0f;
            float hi = 0.0f;
            if (!arg(0, v) || !arg(1, lo) || !arg(2, hi)) {
                return false;
            }
            if (lo > hi) {
                std::swap(lo, hi);
            }
            out_val = v < lo ? lo : (v > hi ? hi : v);
            return true;
        }
        case WseExprOp::fn_sum:
        case WseExprOp::fn_avg:
        case WseExprOp::fn_min:
        case WseExprOp::fn_max: {
            float acc = 0.0f;
            if (!arg(0, acc)) {
                return false;
            }
            for (std::size_t i = 1; i < n.b; ++i) {
                float v = 0.0f;
                if (!arg(i, v)) {
                    return false;
                }
                if (n.op == WseExprOp::fn_min) {
                    if (v < acc) {
                        acc = v;
                    }
                } else if (n.op == WseExprOp::fn_max) {
                    if (v > acc) {
                        acc = v;
                    }
                } else {
                    acc += v;
                }
            }
            out_val = (n.op == WseExprOp::fn_avg)
                          ? (acc / static_cast<float>(n.b))
                          : acc;
            return true;
        }
        }
        return false;
    }
};

// Mirrors mu::Parser expression evaluation for feval (4437 / 0x1155): compiles
// the entire expression into AST before executing so syntax errors leave fp and
// positions untouched.
[[nodiscard]] inline bool wse_eval_float_expr(
    std::string_view expr,
    std::span<float> fp,
    std::span<WseTransform> positions) {
    WseExprCompiler compiler{};
    compiler.src = expr;
    std::size_t root = 0;
    if (!compiler.parse_prec(0, root)) {
        return false;
    }
    compiler.skip_ws();
    if (compiler.pos != compiler.src.size()) {
        return false;
    }
    float unused_result = 0.0f;
    return compiler.eval_node(root, fp, positions, unused_result);
}

inline void wse_skip_json_ws(std::string_view src, std::size_t& pos) noexcept {
    while (pos < src.size()) {
        const char ch = src[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++pos;
        } else {
            break;
        }
    }
}

[[nodiscard]] inline bool wse_parse_hex4(std::string_view src,
                                         std::size_t pos,
                                         std::uint32_t& out) noexcept {
    if (pos + 4 > src.size()) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const int d = wse_hex_nibble(src[pos + i]);
        if (d < 0) {
            return false;
        }
        value = (value << 4U) | static_cast<std::uint32_t>(d);
    }
    out = value;
    return true;
}

[[nodiscard]] inline bool wse_parse_json_string(std::string_view src,
                                                std::size_t& pos,
                                                std::string& out) {
    if (pos >= src.size() || src[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < src.size()) {
        const char ch = src[pos++];
        if (ch == '"') {
            return true;
        }
        if (static_cast<unsigned char>(ch) < 0x20U) {
            return false;
        }
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (pos >= src.size()) {
            return false;
        }
        const char esc = src[pos++];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            out.push_back(esc);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            std::uint32_t cp = 0;
            if (!wse_parse_hex4(src, pos, cp)) {
                return false;
            }
            pos += 4;
            if (cp >= 0xd800U && cp <= 0xdbffU && pos + 6 <= src.size() &&
                src[pos] == '\\' && src[pos + 1] == 'u') {
                std::uint32_t low = 0;
                if (wse_parse_hex4(src, pos + 2, low) && low >= 0xdc00U &&
                    low <= 0xdfffU) {
                    pos += 6;
                    cp = 0x10000U +
                         (((cp - 0xd800U) << 10U) | (low - 0xdc00U));
                }
            }
            wse_append_utf8_codepoint(out, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

[[nodiscard]] inline bool wse_parse_json_scalar(std::string_view src,
                                                std::size_t& pos,
                                                std::string& out) {
    if (pos >= src.size()) {
        return false;
    }
    if (src[pos] == '"') {
        return wse_parse_json_string(src, pos, out);
    }
    if (src.substr(pos).starts_with("true")) {
        pos += 4;
        out = "true";
        return true;
    }
    if (src.substr(pos).starts_with("false")) {
        pos += 5;
        out = "false";
        return true;
    }
    if (src.substr(pos).starts_with("null")) {
        pos += 4;
        out.clear();
        return true;
    }
    const std::size_t start = pos;
    if (src[pos] == '-') {
        ++pos;
    }
    if (pos >= src.size()) {
        return false;
    }
    if (src[pos] == '0') {
        ++pos;
    } else if (src[pos] >= '1' && src[pos] <= '9') {
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    } else {
        return false;
    }
    if (pos < src.size() && src[pos] == '.') {
        ++pos;
        if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
            return false;
        }
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
        ++pos;
        if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
            ++pos;
        }
        if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
            return false;
        }
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    }
    out.assign(src.substr(start, pos - start));
    return true;
}

// Parses top-level JSON object (0x1402a97f0 requires Json::objectValue).
[[nodiscard]] inline bool wse_parse_json_object(
    std::string_view src, std::map<std::string, std::string>& out) {
    if (src.starts_with("\xEF\xBB\xBF")) {
        src.remove_prefix(3);
    }
    std::size_t pos = 0;
    wse_skip_json_ws(src, pos);
    if (pos >= src.size() || src[pos] != '{') {
        return false;
    }
    ++pos;
    std::map<std::string, std::string> parsed;
    wse_skip_json_ws(src, pos);
    if (pos < src.size() && src[pos] == '}') {
        ++pos;
        wse_skip_json_ws(src, pos);
        if (pos != src.size()) {
            return false;
        }
        out = std::move(parsed);
        return true;
    }
    for (;;) {
        wse_skip_json_ws(src, pos);
        std::string key;
        if (!wse_parse_json_string(src, pos, key)) {
            return false;
        }
        wse_skip_json_ws(src, pos);
        if (pos >= src.size() || src[pos] != ':') {
            return false;
        }
        ++pos;
        wse_skip_json_ws(src, pos);
        std::string value;
        if (!wse_parse_json_scalar(src, pos, value)) {
            return false;
        }
        parsed[std::move(key)] = std::move(value);
        wse_skip_json_ws(src, pos);
        if (pos >= src.size()) {
            return false;
        }
        if (src[pos] == '}') {
            ++pos;
            break;
        }
        if (src[pos] != ',') {
            return false;
        }
        ++pos;
    }
    wse_skip_json_ws(src, pos);
    if (pos != src.size()) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

[[nodiscard]] inline bool wse_dict_read_file_bytes(
    const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return false;
    }
    const auto end_pos = in.tellg();
    if (end_pos < 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(end_pos));
    in.seekg(0, std::ios::beg);
    if (!out.empty() &&
        !in.read(out.data(), static_cast<std::streamsize>(out.size()))) {
        out.clear();
        return false;
    }
    return true;
}

[[nodiscard]] inline bool wse_dict_write_file_bytes(
    const std::filesystem::path& path, std::string_view data) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    if (!data.empty()) {
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    return out.good();
}

// Mirrors rglSectionlessIni::read (INI format).
[[nodiscard]] inline bool wse_dict_load_ini(
    const std::filesystem::path& path,
    std::map<std::string, std::string>& out) {
    std::string data;
    if (!wse_dict_read_file_bytes(path, data)) {
        return false;
    }
    std::string_view view(data);
    bool first_line = true;
    while (!view.empty()) {
        const std::size_t nl = view.find('\n');
        std::string_view line =
            nl == std::string_view::npos ? view : view.substr(0, nl);
        view = nl == std::string_view::npos ? std::string_view{}
                                            : view.substr(nl + 1);
        if (first_line && line.starts_with("\xEF\xBB\xBF")) {
            line.remove_prefix(3);
        }
        first_line = false;
        line = wse_trim_ascii(line);
        std::size_t comment = line.size();
        for (const std::string_view marker :
             {std::string_view{"//"}, std::string_view{";"},
              std::string_view{"#"}}) {
            const std::size_t hit = line.find(marker);
            if (hit != std::string_view::npos && hit < comment) {
                comment = hit;
            }
        }
        line = line.substr(0, comment);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = wse_trim_ascii(line.substr(0, eq));
        const std::string_view val = wse_trim_ascii(line.substr(eq + 1));
        out[std::string(key)] = std::string(val);
    }
    return true;
}

inline void wse_dict_save_ini(
    const std::filesystem::path& path,
    const std::map<std::string, std::string>& entries) {
    std::string text;
    for (const auto& [key, value] : entries) {
        text.append(key);
        text.push_back('=');
        text.append(value);
        text.push_back('\n');
    }
    (void)wse_dict_write_file_bytes(path, text);
}

[[nodiscard]] constexpr std::uint32_t wse_read_le32(
    const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

inline void wse_append_le32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<char>((value >> 16U) & 0xffU));
    out.push_back(static_cast<char>((value >> 24U) & 0xffU));
}

// Binary .wsedict format: LE magic 0x57534544, version <= 2, count,
// and count pairs of (int32 klen, int32 vlen, kbytes, vbytes).
inline constexpr std::uint32_t kWseDictBinaryMagic = 0x57534544U;
inline constexpr std::int32_t kWseDictBinaryVersion = 2;

[[nodiscard]] inline bool wse_dict_load_binary(
    const std::filesystem::path& path,
    std::map<std::string, std::string>& out) {
    std::string bytes;
    if (!wse_dict_read_file_bytes(path, bytes) || bytes.size() < 12) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::uint32_t magic = wse_read_le32(raw);
    const auto version = static_cast<std::int32_t>(wse_read_le32(raw + 4));
    const auto count = static_cast<std::int32_t>(wse_read_le32(raw + 8));
    if (magic != kWseDictBinaryMagic || version < 0 ||
        version > kWseDictBinaryVersion || count < 0) {
        return false;
    }
    std::size_t offset = 12;
    for (std::int32_t i = 0; i < count; ++i) {
        if (bytes.size() - offset < 8) {
            return false;
        }
        const auto klen =
            static_cast<std::int32_t>(wse_read_le32(raw + offset));
        const auto vlen =
            static_cast<std::int32_t>(wse_read_le32(raw + offset + 4));
        offset += 8;
        if (klen < 0 || vlen < 0) {
            return false;
        }
        const auto uk = static_cast<std::size_t>(klen);
        const auto uv = static_cast<std::size_t>(vlen);
        if (bytes.size() - offset < uk ||
            bytes.size() - offset - uk < uv) {
            return false;
        }
        std::string key(bytes.substr(offset, uk));
        offset += uk;
        std::string value(bytes.substr(offset, uv));
        offset += uv;
        out[std::move(key)] = std::move(value);
    }
    return true;
}

inline void wse_dict_save_binary(
    const std::filesystem::path& path,
    const std::map<std::string, std::string>& entries) {
    std::string out;
    wse_append_le32(out, kWseDictBinaryMagic);
    wse_append_le32(out, static_cast<std::uint32_t>(kWseDictBinaryVersion));
    wse_append_le32(out, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [key, value] : entries) {
        wse_append_le32(out, static_cast<std::uint32_t>(key.size()));
        wse_append_le32(out, static_cast<std::uint32_t>(value.size()));
        out.append(key);
        out.append(value);
    }
    (void)wse_dict_write_file_bytes(path, out);
}

[[nodiscard]] inline bool wse_dict_target_path(
    const WseDictionaryStore* store,
    std::string name,
    std::string_view ext,
    std::filesystem::path& out_path) {
    if (!store || store->root_dir.empty() || !wse_check_path(name)) {
        return false;
    }
    name.append(ext);
    out_path = store->root_dir / name;
    return true;
}

// Shared loader applying 0x1401e6c18..0x1401e7f48 mode semantics:
// mode == 0 clears then inserts all; mode == 1 overwrites existing;
// any other mode inserts only missing keys.
inline void wse_dict_apply_load(
    WseDictionary& dict,
    const std::map<std::string, std::string>& loaded,
    std::int32_t mode) {
    if (mode == 0) {
        dict.clear();
        dict.merge(loaded, false);
    } else if (mode == 1) {
        dict.merge(loaded, true);
    } else {
        dict.merge(loaded, false);
    }
}

// 0x1402aa920: serializes o.xyz, f.xyz, u.xyz using "%f" separated by ','.
inline void wse_dict_set_transform(WseDictionary& dict,
                                   const std::string& key,
                                   const WseTransform& t) {
    const std::array<float, 9> values = {t.o.x, t.o.y, t.o.z, t.f.x, t.f.y,
                                         t.f.z, t.u.x, t.u.y, t.u.z};
    std::string encoded;
    std::array<char, 64> buf{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            encoded.push_back(',');
        }
        const int written =
            std::snprintf(buf.data(), buf.size(), "%f",
                          static_cast<double>(values[i]));
        if (written > 0) {
            encoded.append(buf.data(), static_cast<std::size_t>(written));
        }
    }
    dict.set(key, std::move(encoded));
}

// 0x1402aa5a0: splits on ',' preserving empty tokens, parses 9 floats into
// (o, f, u) and orthonormalizes, or returns `fallback` when token count != 9.
[[nodiscard]] inline WseTransform wse_dict_get_transform(
    const WseDictionary& dict,
    const std::string& key,
    const WseTransform& fallback) {
    const auto it = dict.entries.find(key);
    if (it == dict.entries.end()) {
        return fallback;
    }
    std::vector<std::string_view> tokens;
    const std::string_view raw = it->second;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = raw.find(',', start);
        if (comma == std::string_view::npos) {
            tokens.push_back(raw.substr(start));
            break;
        }
        tokens.push_back(raw.substr(start, comma - start));
        start = comma + 1;
    }
    if (tokens.size() != 9) {
        return fallback;
    }
    std::array<float, 9> parsed{};
    for (std::size_t i = 0; i < 9; ++i) {
        std::string tok(tokens[i]);
        parsed[i] = static_cast<float>(std::strtod(tok.c_str(), nullptr));
    }
    WseTransform out{};
    out.o = {parsed[0], parsed[1], parsed[2], 1.0f};
    out.f = {parsed[3], parsed[4], parsed[5], 0.0f};
    out.u = {parsed[6], parsed[7], parsed[8], 0.0f};
    out.orthonormalize();
    return out;
}

[[nodiscard]] inline WseDispatch wse_store_dict_string(
    WseStringBanks& strings,
    std::int32_t sreg,
    std::string_view utf8_value) {
    return wse_store_string(strings, sreg, wse_utf8_to_utf16(utf8_value))
               ? WseDispatch::handled_no_store
               : WseDispatch::ignored;
}

inline void wse_append_le_f32(std::string& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    wse_append_le32(out, bits);
}

[[nodiscard]] inline float wse_read_le_f32(const std::uint8_t* p) noexcept {
    const std::uint32_t bits = wse_read_le32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void wse_array_serialize_slice(const WseDynArray& arr,
                                      const WseArraySlice& slice,
                                      std::size_t depth,
                                      std::string& out) {
    if (depth + 1 < arr.dim_sizes.size()) {
        for (const auto& child : slice.children) {
            if (child) {
                wse_array_serialize_slice(arr, *child, depth + 1, out);
            }
        }
        return;
    }
    switch (arr.type) {
    case WseArrayType::integer:
        for (const std::int32_t v : slice.ints) {
            wse_append_le32(out, 4U);
            wse_append_le32(out, static_cast<std::uint32_t>(v));
        }
        break;
    case WseArrayType::string:
        for (const auto& s : slice.strings) {
            wse_append_le32(out, static_cast<std::uint32_t>(s.size()));
            out.append(s);
        }
        break;
    case WseArrayType::position:
        // 0x1401c5910 writes o.xyz, f.xyz, u.xyz (9 floats = 36 bytes).
        for (const auto& t : slice.positions) {
            wse_append_le32(out, 36U);
            wse_append_le_f32(out, t.o.x);
            wse_append_le_f32(out, t.o.y);
            wse_append_le_f32(out, t.o.z);
            wse_append_le_f32(out, t.f.x);
            wse_append_le_f32(out, t.f.y);
            wse_append_le_f32(out, t.f.z);
            wse_append_le_f32(out, t.u.x);
            wse_append_le_f32(out, t.u.y);
            wse_append_le_f32(out, t.u.z);
        }
        break;
    }
}

[[nodiscard]] inline bool wse_array_save_binary(
    const std::filesystem::path& path, const WseDynArray& arr) {
    if (!arr.root || arr.dim_sizes.empty()) {
        return false;
    }
    std::string out;
    wse_append_le32(out, kWseArrayBinaryMagic);
    wse_append_le32(out, static_cast<std::uint32_t>(kWseArrayBinaryVersion));
    wse_append_le32(out, static_cast<std::uint32_t>(arr.type_id()));
    wse_append_le32(out, static_cast<std::uint32_t>(arr.dim_count()));
    for (const std::int32_t dim : arr.dim_sizes) {
        wse_append_le32(out, static_cast<std::uint32_t>(dim));
    }
    wse_array_serialize_slice(arr, *arr.root, 0, out);
    return wse_dict_write_file_bytes(path, out);
}

[[nodiscard]] inline bool wse_array_deserialize_slice(
    WseDynArray& arr,
    WseArraySlice& slice,
    std::size_t depth,
    const std::string& bytes,
    std::size_t& offset) {
    if (depth + 1 < arr.dim_sizes.size()) {
        for (auto& child : slice.children) {
            if (!child ||
                !wse_array_deserialize_slice(arr, *child, depth + 1, bytes,
                                             offset)) {
                return false;
            }
        }
        return true;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    switch (arr.type) {
    case WseArrayType::integer:
        for (auto& dst : slice.ints) {
            if (bytes.size() - offset < 8U) {
                return false;
            }
            const std::uint32_t len = wse_read_le32(raw + offset);
            if (len != 4U) {
                return false;
            }
            dst = static_cast<std::int32_t>(wse_read_le32(raw + offset + 4U));
            offset += 8U;
        }
        return true;
    case WseArrayType::string:
        for (auto& dst : slice.strings) {
            if (bytes.size() - offset < 4U) {
                return false;
            }
            const auto slen =
                static_cast<std::int32_t>(wse_read_le32(raw + offset));
            offset += 4U;
            if (slen < 0 ||
                static_cast<std::size_t>(slen) > bytes.size() - offset) {
                return false;
            }
            const auto uslen = static_cast<std::size_t>(slen);
            dst.assign(bytes.data() + offset, uslen);
            offset += uslen;
        }
        return true;
    case WseArrayType::position:
        // 0x1401c59f0 reads o, f, u (36 bytes) and orthonormalizes.
        for (auto& dst : slice.positions) {
            if (bytes.size() - offset < 40U) {
                return false;
            }
            const std::uint32_t len = wse_read_le32(raw + offset);
            if (len != 36U) {
                return false;
            }
            offset += 4U;
            dst = wse_zero_transform();
            dst.o = {wse_read_le_f32(raw + offset + 0U),
                     wse_read_le_f32(raw + offset + 4U),
                     wse_read_le_f32(raw + offset + 8U), 1.0f};
            dst.f = {wse_read_le_f32(raw + offset + 12U),
                     wse_read_le_f32(raw + offset + 16U),
                     wse_read_le_f32(raw + offset + 20U), 0.0f};
            dst.u = {wse_read_le_f32(raw + offset + 24U),
                     wse_read_le_f32(raw + offset + 28U),
                     wse_read_le_f32(raw + offset + 32U), 0.0f};
            dst.orthonormalize();
            offset += 36U;
        }
        return true;
    }
    return false;
}

[[nodiscard]] inline bool wse_array_load_binary(
    const std::filesystem::path& path,
    std::unique_ptr<WseDynArray>& out,
    bool& version_too_new,
    bool& bad_type_id) {
    version_too_new = false;
    bad_type_id = false;
    std::string bytes;
    if (!wse_dict_read_file_bytes(path, bytes) || bytes.size() < 16U) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::uint32_t magic = wse_read_le32(raw);
    const auto version = static_cast<std::int32_t>(wse_read_le32(raw + 4U));
    const auto type_id = static_cast<std::int32_t>(wse_read_le32(raw + 8U));
    const auto dim_count = static_cast<std::int32_t>(wse_read_le32(raw + 12U));
    if (magic != kWseArrayBinaryMagic || version < 0) {
        return false;
    }
    if (version > kWseArrayBinaryVersion) {
        version_too_new = true;
        return false;
    }
    if (type_id < 0 || type_id > 2) {
        bad_type_id = true;
        return false;
    }
    if (dim_count < 1 || dim_count > kWseArrayMaxDims) {
        return false;
    }
    std::size_t offset = 16U;
    const std::size_t dims_bytes = static_cast<std::size_t>(dim_count) * 4U;
    if (bytes.size() - offset < dims_bytes) {
        return false;
    }
    std::vector<std::int32_t> dims(static_cast<std::size_t>(dim_count));
    for (std::int32_t d = 0; d < dim_count; ++d) {
        dims[static_cast<std::size_t>(d)] =
            static_cast<std::int32_t>(wse_read_le32(raw + offset));
        offset += 4U;
    }
    if (!WseDynArray::dims_valid(dims)) {
        return false;
    }
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t min_elem_bytes =
        type_id == 0 ? 8U : (type_id == 2 ? 40U : 4U);
    std::size_t total_elems = 1;
    for (const std::int32_t dim : dims) {
        if (dim == 0) {
            total_elems = 0;
            break;
        }
        const auto udim = static_cast<std::size_t>(dim);
        if (udim > (remaining / min_elem_bytes) / total_elems) {
            return false;
        }
        total_elems *= udim;
    }
    auto loaded =
        WseDynArray::create(static_cast<WseArrayType>(type_id), dims);
    if (!loaded || !loaded->root ||
        !wse_array_deserialize_slice(*loaded, *loaded->root, 0, bytes,
                                     offset)) {
        return false;
    }
    out = std::move(loaded);
    return true;
}

[[nodiscard]] inline int wse_array_compare_bytes_cs(
    std::string_view lhs, std::string_view rhs) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    if (common > 0) {
        const int cmp = std::memcmp(lhs.data(), rhs.data(), common);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    return lhs.size() > rhs.size() ? 1 : 0;
}

[[nodiscard]] inline int wse_array_compare_bytes_ci(
    std::string_view lhs, std::string_view rhs) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < common; ++i) {
        const int a = std::tolower(static_cast<unsigned char>(lhs[i]));
        const int b = std::tolower(static_cast<unsigned char>(rhs[i]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    return lhs.size() > rhs.size() ? 1 : 0;
}

// Mirrors 0x1401fde40 / 0x1401fddb2: stable natural merge sort over dim 0,
// aborting without mutating `arr` if not sorted within `n` double-passes.
template <typename BeforeOrEqual>
[[nodiscard]] inline bool wse_array_natural_merge_sort(
    WseDynArray& arr, BeforeOrEqual before_or_equal) {
    if (!arr.root || arr.dim_sizes.empty() || arr.dim_sizes[0] <= 1) {
        return false;
    }
    const auto n = static_cast<std::size_t>(arr.dim_sizes[0]);
    std::vector<std::size_t> buf_a(n);
    for (std::size_t i = 0; i < n; ++i) {
        buf_a[i] = i;
    }
    std::vector<std::size_t> buf_b(n);

    auto run_pass = [&](const std::vector<std::size_t>& src,
                        std::vector<std::size_t>& dst) -> bool {
        struct Run {
            std::size_t begin;
            std::size_t end;
        };
        std::vector<Run> runs;
        std::size_t start = 0;
        while (start < n) {
            std::size_t end = start + 1;
            while (end < n && before_or_equal(src[end - 1], src[end])) {
                ++end;
            }
            runs.push_back({start, end});
            start = end;
        }
        if (runs.size() <= 1U) {
            return true;
        }
        std::size_t out_idx = 0;
        for (std::size_t r = 0; r + 1 < runs.size(); r += 2) {
            std::size_t i = runs[r].begin;
            std::size_t j = runs[r + 1].begin;
            const std::size_t i_end = runs[r].end;
            const std::size_t j_end = runs[r + 1].end;
            while (i < i_end && j < j_end) {
                if (before_or_equal(src[i], src[j])) {
                    dst[out_idx++] = src[i++];
                } else {
                    dst[out_idx++] = src[j++];
                }
            }
            while (i < i_end) {
                dst[out_idx++] = src[i++];
            }
            while (j < j_end) {
                dst[out_idx++] = src[j++];
            }
        }
        if ((runs.size() & 1U) != 0U) {
            const Run last = runs.back();
            for (std::size_t i = last.begin; i < last.end; ++i) {
                dst[out_idx++] = src[i];
            }
        }
        return false;
    };

    const std::vector<std::size_t>* sorted = nullptr;
    if (run_pass(buf_a, buf_b)) {
        sorted = &buf_a;
    } else {
        for (std::size_t double_passes = 0;;) {
            if (run_pass(buf_b, buf_a)) {
                sorted = &buf_b;
                break;
            }
            if (++double_passes >= n) {
                return false;
            }
            if (run_pass(buf_a, buf_b)) {
                sorted = &buf_a;
                break;
            }
        }
    }

    const auto& order = *sorted;
    if (arr.dim_sizes.size() > 1U) {
        std::vector<std::unique_ptr<WseArraySlice>> reordered(n);
        for (std::size_t i = 0; i < n; ++i) {
            reordered[i] = std::move(arr.root->children[order[i]]);
        }
        arr.root->children = std::move(reordered);
    } else {
        switch (arr.type) {
        case WseArrayType::integer: {
            std::vector<std::int32_t> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = arr.root->ints[order[i]];
            }
            arr.root->ints = std::move(reordered);
            break;
        }
        case WseArrayType::string: {
            std::vector<std::string> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = std::move(arr.root->strings[order[i]]);
            }
            arr.root->strings = std::move(reordered);
            break;
        }
        case WseArrayType::position: {
            std::vector<WseTransform> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = arr.root->positions[order[i]];
            }
            arr.root->positions = std::move(reordered);
            break;
        }
        }
    }
    return true;
}

[[nodiscard]] inline bool wse_array_validate_sort_subdims(
    const WseDynArray& arr,
    std::span<const std::int32_t> subdims) noexcept {
    if (arr.dim_sizes.empty() || arr.dim_sizes[0] <= 1 ||
        subdims.size() + 1 != arr.dim_sizes.size()) {
        return false;
    }
    for (std::size_t k = 0; k < subdims.size(); ++k) {
        if (subdims[k] < 0 || subdims[k] >= arr.dim_sizes[k + 1]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool wse_array_sort_builtin(
    WseDynArray& arr,
    std::int32_t mode,
    std::span<const std::int32_t> subdims) {
    if (!wse_array_validate_sort_subdims(arr, subdims)) {
        return false;
    }
    if (arr.type == WseArrayType::integer) {
        if (mode != 0 && mode != 1) {
            return false;
        }
        const bool desc = mode == 1;
        return wse_array_natural_merge_sort(
            arr, [&](std::size_t row_a, std::size_t row_b) noexcept {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = arr.slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = arr.slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return true;
                }
                const std::int32_t va = sa->ints[leaf_a];
                const std::int32_t vb = sb->ints[leaf_b];
                return desc ? (va >= vb) : (va <= vb);
            });
    }
    if (arr.type == WseArrayType::string) {
        if (mode < 0 || mode > 3) {
            return false;
        }
        const bool desc = (mode & 1) != 0;
        const bool ci = (mode & 2) != 0;
        return wse_array_natural_merge_sort(
            arr, [&](std::size_t row_a, std::size_t row_b) noexcept {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = arr.slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = arr.slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return true;
                }
                const int cmp =
                    ci ? wse_array_compare_bytes_ci(sa->strings[leaf_a],
                                                    sb->strings[leaf_b])
                       : wse_array_compare_bytes_cs(sa->strings[leaf_a],
                                                    sb->strings[leaf_b]);
                return desc ? (cmp >= 0) : (cmp <= 0);
            });
    }
    return false;
}

[[nodiscard]] inline WseDispatch wse_array_compare_op(
    const WseOperands& operands, std::int32_t cmp_mode) {
    if (operands.narrow.size() < 3) {
        return WseDispatch::operand_count_out_of_range;
    }
    const WseDynArray* arr =
        operands.state.dictionaries != nullptr
            ? operands.state.dictionaries->get_array(operands.narrow[0])
            : nullptr;
    if (!arr) {
        return WseDispatch::condition_false;
    }
    std::size_t leaf_idx = 0;
    const WseArraySlice* leaf =
        arr->leaf_for(operands.narrow.subspan(2), leaf_idx);
    if (!leaf) {
        return WseDispatch::condition_false;
    }
    auto eval_ordered = [cmp_mode](int rel) noexcept -> bool {
        switch (cmp_mode) {
        case 0:
            return rel == 0;
        case 1:
            return rel != 0;
        case 2:
            return rel > 0;
        case 3:
            return rel >= 0;
        case 4:
            return rel < 0;
        case 5:
            return rel <= 0;
        default:
            return false;
        }
    };
    switch (arr->type) {
    case WseArrayType::integer: {
        const std::int32_t lhs = leaf->ints[leaf_idx];
        const std::int32_t rhs = operands.narrow[1];
        const int rel = (lhs < rhs) ? -1 : (lhs > rhs ? 1 : 0);
        return eval_ordered(rel) ? WseDispatch::handled_no_store
                                 : WseDispatch::condition_false;
    }
    case WseArrayType::string: {
        std::string rhs;
        if (!wse_resolve_parsed_utf8_operand(1, operands, rhs)) {
            return WseDispatch::condition_false;
        }
        const int rel = wse_array_compare_bytes_cs(leaf->strings[leaf_idx], rhs);
        return eval_ordered(rel) ? WseDispatch::handled_no_store
                                 : WseDispatch::condition_false;
    }
    case WseArrayType::position: {
        if ((cmp_mode != 0 && cmp_mode != 1) ||
            !wse_register_ok(operands.narrow[1]) ||
            static_cast<std::size_t>(operands.narrow[1]) >=
                operands.state.positions.size()) {
            return WseDispatch::condition_false;
        }
        const bool eq = wse_transforms_equal(
            leaf->positions[leaf_idx],
            operands.state.positions[static_cast<std::size_t>(
                operands.narrow[1])]);
        const bool ok = cmp_mode == 0 ? eq : !eq;
        return ok ? WseDispatch::handled_no_store
                  : WseDispatch::condition_false;
    }
    }
    return WseDispatch::condition_false;
}

// Mirrors 0x14030f270 (rglString::spacesToUnderscores): in-place ' ' -> '_'.
inline void wse_spaces_to_underscores(std::string& text) noexcept {
    std::replace(text.begin(), text.end(), ' ', '_');
}

// Shared entity slot lookup for opcodes 3003..3013 (0x1401e55c5..0x1401e5912).
[[nodiscard]] inline const Slots* wse_entity_slots_by_opcode(
    const wse::game::WseWorldState& world,
    std::uint32_t opcode,
    std::int32_t id) noexcept {
    if (id < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(id);
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::item_slot_gt:
        return world.is_valid_item(id) ? &world.items[idx].slots : nullptr;
    case WseOpcode::party_template_slot_gt:
        return world.is_valid_party_template(id)
                   ? &world.party_templates[idx].slots
                   : nullptr;
    case WseOpcode::troop_slot_gt:
        return world.is_valid_troop(id) ? &world.troops[idx].slots : nullptr;
    case WseOpcode::faction_slot_gt:
        return world.is_valid_faction(id) ? &world.factions[idx].slots : nullptr;
    case WseOpcode::quest_slot_gt:
        return world.is_valid_quest(id) ? &world.quests[idx].slots : nullptr;
    case WseOpcode::scene_slot_gt:
        return world.is_valid_site(id) ? &world.sites[idx].slots : nullptr;
    case WseOpcode::party_slot_gt:
        return world.is_valid_party(id) ? &world.parties[idx].slots : nullptr;
    case WseOpcode::player_slot_gt:
        return world.is_valid_player(id) ? &world.players[idx].slots : nullptr;
    case WseOpcode::team_slot_gt:
        return world.is_valid_team(id) ? &world.teams[idx].slots : nullptr;
    case WseOpcode::agent_slot_gt:
        return world.is_valid_agent(id) ? &world.agents[idx].slots : nullptr;
    case WseOpcode::scene_prop_slot_gt:
        return world.is_valid_mission_object(id)
                   ? &world.mission_objects[idx].slots
                   : nullptr;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline WseDispatch wse_entity_slot_gt(
    const WseOperands& operands,
    std::uint32_t opcode) noexcept {
    if (operands.narrow.size() < 3 || operands.wide.size() < 3) {
        return WseDispatch::operand_count_out_of_range;
    }
    if (operands.state.world == nullptr) {
        return WseDispatch::condition_false;
    }
    const Slots* slots = wse_entity_slots_by_opcode(
        *operands.state.world, opcode, operands.narrow[0]);
    if (slots == nullptr ||
        static_cast<std::uint32_t>(operands.narrow[1]) >
            static_cast<std::uint32_t>(wse::game::kMaxSlotIndex)) {
        return WseDispatch::condition_false;
    }
    return slots->get(operands.narrow[1]) > operands.wide[2]
               ? WseDispatch::handled_no_store
               : WseDispatch::condition_false;
}

[[nodiscard]] inline std::int32_t wse::game::WseTroop::*
wse_troop_points_member(std::uint32_t opcode) noexcept {
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::troop_get_skill_points:
    case WseOpcode::troop_set_skill_points:
        return &wse::game::WseTroop::skill_points;
    case WseOpcode::troop_get_attribute_points:
    case WseOpcode::troop_set_attribute_points:
        return &wse::game::WseTroop::attribute_points;
    case WseOpcode::troop_get_proficiency_points:
    case WseOpcode::troop_set_proficiency_points:
        return &wse::game::WseTroop::proficiency_points;
    default:
        return nullptr;
    }
}

// Mirrors 3800..3803 (0x1401ebf9f..0x1401ec0cf): low byte vs bits 8..9.
inline void wse_set_item_damage_part(wse::game::WseItemKind& item,
                                     std::uint32_t opcode,
                                     std::int32_t value) noexcept {
    const auto op = static_cast<WseOpcode>(opcode);
    std::int32_t& target =
        (op == WseOpcode::item_set_thrust_damage ||
         op == WseOpcode::item_set_thrust_damage_type)
            ? item.thrust_damage
            : item.swing_damage;
    const auto bits = static_cast<std::uint32_t>(target);
    const auto uval = static_cast<std::uint32_t>(value);
    if (op == WseOpcode::item_set_thrust_damage ||
        op == WseOpcode::item_set_swing_damage) {
        target = static_cast<std::int32_t>((bits & ~0xffu) | (uval & 0xffu));
    } else {
        target = static_cast<std::int32_t>((bits & 0xffu) | ((uval & 3u) << 8));
    }
}

[[nodiscard]] inline std::int32_t wse::game::WseItemKind::*
wse_item_int_member(std::uint32_t opcode) noexcept {
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::item_set_head_armor:
        return &wse::game::WseItemKind::head_armor;
    case WseOpcode::item_set_body_armor:
        return &wse::game::WseItemKind::body_armor;
    case WseOpcode::item_set_leg_armor:
        return &wse::game::WseItemKind::leg_armor;
    case WseOpcode::item_set_speed_rating:
        return &wse::game::WseItemKind::speed_rating;
    case WseOpcode::item_set_missile_speed:
        return &wse::game::WseItemKind::missile_speed;
    default:
        return nullptr;
    }
}

// Shared implementation for 4232 str_store_skill_name (0x1401103d0) and 4245
// str_store_skill_desc (0x140213520, adding "_desc" before translation lookup).
[[nodiscard]] inline WseDispatch wse_store_skill_text(
    const wse::game::WseWorldState* world,
    WseStringBanks& strings,
    std::int32_t sreg,
    std::int32_t skill_no,
    bool is_desc) {
    if (!wse_register_ok(sreg) || world == nullptr || skill_no < 0 ||
        skill_no >= wse::game::kMaxSkills) {
        return WseDispatch::ignored;
    }
    const auto& skill = world->skills[static_cast<std::size_t>(skill_no)];
    if (!skill.id.empty()) {
        const std::string key = is_desc ? (skill.id + "_desc") : skill.id;
        if (const auto it = world->translations.find(key);
            it != world->translations.end()) {
            return wse_store_dict_string(strings, sreg, it->second);
        }
    }
    return wse_store_dict_string(strings, sreg,
                                 is_desc ? skill.description : skill.name);
}

// Mirrors 0x14024f4c0 ("sg%d%d.sav" using slot/10 and slot%10) and 4246
// (0x1401efadc..0x1401efc8d). Clears destination sreg before file open.
[[nodiscard]] inline WseDispatch wse_store_savegame_md5(
    const wse::game::WseWorldState* world,
    WseStringBanks& strings,
    std::int32_t sreg,
    std::int32_t slot_no) {
    if (!wse_register_ok(sreg)) {
        return WseDispatch::ignored;
    }
    if (!wse_store_string(strings, sreg, u"")) {
        return WseDispatch::ignored;
    }
    if (world == nullptr || world->savegames_dir.empty()) {
        return WseDispatch::handled_no_store;
    }
    const std::int32_t q = slot_no / 10;
    const std::int32_t r = slot_no - q * 10;
    const std::string filename =
        "sg" + std::to_string(q) + std::to_string(r) + ".sav";
    std::ifstream in(world->savegames_dir / filename,
                     std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return WseDispatch::handled_no_store;
    }
    const std::streamoff byte_count = in.tellg();
    if (byte_count <= 0) {
        return WseDispatch::handled_no_store;
    }
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(byte_count), '\0');
    if (in.read(bytes.data(), byte_count)) {
        (void)wse_store_string(strings, sreg, wse_md5_hex(bytes));
    }
    return WseDispatch::handled_no_store;
}

[[nodiscard]] inline std::int32_t wse_edit_mode_find_selected(
    const wse::game::WseEditModeState& edit,
    std::int32_t instance_no) noexcept {
    for (std::size_t i = 0; i < edit.selected_prop_instances.size(); ++i) {
        if (edit.selected_prop_instances[i] == instance_no) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

// `val_*` uses operand 0 as both source and destination; `store_*` reads
// operands 1.. and leaves operand 0 as the destination lvalue for the caller.
[[nodiscard]] inline WseDispatch execute_wse(std::uint32_t opcode,
                                             const WseOperands& operands,
                                             WseEffect& effect) noexcept {
    opcode &= kWseOpcodeMask;
    effect.store_value = false;
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto tags = operands.tags;
    const auto fp = operands.fp;
    const auto registers = operands.registers;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::val_shr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] >> wse_shift(wide[1]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] >> wse_shift(wide[2]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_lshr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) >> wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_lshr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) >> wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_shl: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) << wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shl: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) << wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_xor: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] ^ wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_xor: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] ^ wide[2];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_not: {
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[0];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_not: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::register_get: {
        // 3000 (pseudocode line 628): checked index in the int64 register
        // bank, then the shared operand-0 store tail.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        effect.value = registers[index];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::register_set: {
        // 3001 (line 870): the destination is the register named by operand 0;
        // the value is operand 1. Retail returns true without returnValue.
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        registers[index] = wide[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::store_wse_version: {
        // 3002 (line 877): 0 yields the WSE version (5), 1 the Warband build
        // (0x483 = 1155); any other argument yields the zeroed r15 default.
        if (narrow.size() < 2 || wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t which = narrow[1];
        effect.value = which == 0 ? 5 : (which == 1 ? 0x483 : 0);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::item_slot_gt:
    case WseOpcode::party_template_slot_gt:
    case WseOpcode::troop_slot_gt:
    case WseOpcode::faction_slot_gt:
    case WseOpcode::quest_slot_gt:
    case WseOpcode::scene_slot_gt:
    case WseOpcode::party_slot_gt:
    case WseOpcode::player_slot_gt:
    case WseOpcode::team_slot_gt:
    case WseOpcode::agent_slot_gt:
    case WseOpcode::scene_prop_slot_gt:
        return wse_entity_slot_gt(operands, opcode);
    case WseOpcode::store_current_trigger: {
        // 3014 (0x1401e5917): return the context flags while inside a trigger
        // (negative), otherwise 0. WSEOperationContext::GetCurrentTrigger uses
        // exactly this `m_context_flags < 0 ? m_context_flags : 0` rule.
        effect.value = operands.state.caller <= 0 ? operands.state.caller : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_num_return_values: {
        // 3016 (0x1401e5979): g_numReturnValues at 0x140698ae8.
        effect.value = operands.state.num_return_values;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_return_value: {
        // 3017 (0x1401e5985): operand 1 is a 1-based index into the 16-slot
        // return-value bank at 0x140698af0. Out-of-range or unset indices log
        // ("invalid script result index %d" / "unset result value %d") and
        // return true without storing.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t index = narrow[1];
        if (index < 1 || index > 16) {
            return WseDispatch::ignored;
        }
        if (index > operands.state.num_return_values) {
            return WseDispatch::ignored;
        }
        const auto slot = static_cast<std::size_t>(index - 1);
        if (slot >= operands.state.return_values.size()) {
            return WseDispatch::ignored;
        }
        effect.value = operands.state.return_values[slot];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_application_time: {
        // 3035 (0x1401e63a4): rglRound64((float)DXUtil_Timer(5) * 1000.0f).
        // FUN_1400b9690(5) returns QPC seconds since the app-time baseline.
        effect.value = wse_round64(
            static_cast<float>(operands.state.app_time_seconds) * 1000.0f);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_cur_mission_template_no: {
        // 3600 (0x1401ea6b2): null g_currentMission (0x14061f078) yields -1,
        // otherwise the int32 at +0x9ae8 (cur_mission_template_no).
        effect.value = operands.state.current_mission_template_no
                           ? *operands.state.current_mission_template_no
                           : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::sleep_ms: {
        // 3026 (0xbd2, 0x1401e62a0 / L1188): negative skips Sleep().
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (narrow[0] < 0) {
            return WseDispatch::ignored;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(narrow[0]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::set_main_party: {
        // 3030 (0xbd6, L1284): validates party via 0x1401c4ed0.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->main_party_no = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_main_party: {
        // 3031 (0xbd7, L1291): reads mbGame::m_mainPartyNo (g_game + 0x418).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game) {
            return WseDispatch::ignored;
        }
        effect.value = world->main_party_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::is_party_skill: {
        // 3036 (0xbdc, 0x1401e63c4): skill > 41 logs and returns true; else
        // tests bit 4 of mbSkill::m_flags (0x14065f8ec).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr ||
            static_cast<std::uint32_t>(narrow[0]) >=
                static_cast<std::uint32_t>(wse::game::kMaxSkills)) {
            return WseDispatch::ignored;
        }
        const auto idx = static_cast<std::size_t>(narrow[0]);
        return (world->skills[idx].flags & wse::game::kSkillPartyFlagBit) != 0
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::get_campaign_time: {
        // 3037 (0xbdd, 0x1401e63f0): reads DAT_14061f038.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = world->campaign_time;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::set_campaign_time: {
        // 3038 (0xbde, 0x1401e63fc): writes DAT_14061f038 from wide[0].
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->campaign_time = wide[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::show_hud: {
        // 3045 (0xbe5, L1571): DAT_140657110 = (*param_3 == 0).
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->hide_hud = (wide[0] == 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::set_show_use_tooltip: {
        // 3601 (0xe11, 0x1401ea71d): 0 <= type < 11 writes mbMission +0x54a40.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || narrow[0] < 0 ||
            narrow[0] >= wse::game::kNumUseTooltipTypes) {
            return WseDispatch::ignored;
        }
        world->use_objects_tooltip_enabled[static_cast<std::size_t>(
            narrow[0])] = (narrow.size() >= 2 && narrow[1] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::stop_time: {
        // 3608 (0xe18, 0x1401ea9da): requires cheat_mode (0x14061ef50).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            operands.state.options == nullptr ||
            !operands.state.options->cheat_mode) {
            return WseDispatch::ignored;
        }
        world->paused = (narrow[0] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_water_level: {
        // 3610 (0xe1a, 0x1401eabd6): rglRound64(fixed_point * water_level).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            static_cast<float>(operands.state.fixed_point_multiplier) *
            world->water_level);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::troop_get_skill_points:
    case WseOpcode::troop_get_attribute_points:
    case WseOpcode::troop_get_proficiency_points: {
        // 3700/3702/3704: read mbTroop +0x18c/+0x190/+0x194.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        const auto member = wse_troop_points_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_troop(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->troops[static_cast<std::size_t>(narrow[1])].*member;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::troop_set_skill_points:
    case WseOpcode::troop_set_attribute_points:
    case WseOpcode::troop_set_proficiency_points: {
        // 3701/3703/3705: write non-negative points into mbTroop.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        const auto member = wse_troop_points_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_troop(narrow[0]) || narrow[1] < 0) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].*member = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_has_flag: {
        // 3706 (0xe7a, L4431): invalid troop logs and returns ignored; else
        // tests (mbTroop::m_flags & wide[1]) != 0.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto flags =
            world->troops[static_cast<std::size_t>(narrow[0])].flags;
        return (flags & static_cast<std::uint64_t>(wide[1])) != 0
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::troop_set_skill: {
        // 3707 (0xe7b, L4438 -> 0x1402460b0): clamps to skill m_maxLevel,
        // adjusts HP for Ironflesh increase, and applies shared HP cap.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxSkills) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_skill(
            narrow[1], narrow[2],
            world->skills[static_cast<std::size_t>(narrow[1])].max_level,
            world->skills[static_cast<std::size_t>(wse::game::kSkillIronflesh)]
                .max_level);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_set_attribute: {
        // 3708 (0xe7c, L4451 -> 0x140246010): clamps [0, INT_MAX] and caps HP.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxAttributes) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_attribute(
            narrow[1], narrow[2],
            world->skills[static_cast<std::size_t>(wse::game::kSkillIronflesh)]
                .max_level);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_set_proficiency: {
        // 3709 (0xe7d, L4464 -> 0x1400a61c0): clamps [0, 999] into float.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxProficiencies) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_proficiency(
            narrow[1], narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_thrust_damage:
    case WseOpcode::item_set_thrust_damage_type:
    case WseOpcode::item_set_swing_damage:
    case WseOpcode::item_set_swing_damage_type: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        wse_set_item_damage_part(
            world->items[static_cast<std::size_t>(narrow[0])], opcode,
            narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_head_armor:
    case WseOpcode::item_set_body_armor:
    case WseOpcode::item_set_leg_armor:
    case WseOpcode::item_set_speed_rating:
    case WseOpcode::item_set_missile_speed: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        const auto member = wse_item_int_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])].*member = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_blood_particles: {
        // 3809 (0xee1, L4555): requires valid item and two particle systems.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0]) ||
            !world->is_valid_particle_system(narrow[1]) ||
            !world->is_valid_particle_system(narrow[2])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])]
            .blood_particle_system_nos = {narrow[1], narrow[2]};
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_blood_color: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])].blood_color =
            static_cast<std::uint32_t>(narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::cur_item_mesh_set_color: {
        // 3811 (0xee3, L4579): caller == -50, active rglMetaMesh (0x1401c5e40).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (operands.state.caller != -50 || world == nullptr ||
            world->cur_item_meta_mesh == nullptr) {
            return WseDispatch::ignored;
        }
        world->cur_item_meta_mesh->set_mesh_color(
            narrow[0], static_cast<std::uint32_t>(narrow[1]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::cur_item_add_mesh_with_material: {
        // 3812 (0xee4, L4602): parses mesh & material, converts spaces to
        // underscores (0x14030f270), clones meshes per LOD [begin, end).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_string_register_ok(narrow[0], wse_tag_at(tags, 0), strings) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        auto* const world = operands.state.world;
        if (operands.state.caller != -50 || world == nullptr ||
            world->cur_item_meta_mesh == nullptr) {
            return WseDispatch::ignored;
        }
        std::string mesh_name;
        std::string material_name;
        if (!wse_resolve_parsed_utf8_operand(0, operands, mesh_name) ||
            !wse_resolve_parsed_utf8_operand(1, operands, material_name)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(mesh_name);
        wse_spaces_to_underscores(material_name);
        const auto mesh_it = world->resource_meshes.find(mesh_name);
        if (mesh_it == world->resource_meshes.end()) {
            return WseDispatch::handled_no_store;
        }
        const auto mat_it = world->resource_materials.find(material_name);
        const bool mat_found =
            mat_it != world->resource_materials.end() && mat_it->second;
        auto& meta = *world->cur_item_meta_mesh;
        const std::int32_t lod_begin = narrow.size() >= 3 ? narrow[2] : 0;
        std::int32_t lod_end = narrow.size() >= 4 ? narrow[3] : 0;
        if (lod_end == 0) {
            lod_end = static_cast<std::int32_t>(meta.lods.size());
        }
        const std::int32_t color = narrow.size() >= 5 ? narrow[4] : 0;
        if (lod_begin >= 0) {
            for (std::int32_t lod = lod_begin;
                 lod < lod_end &&
                 static_cast<std::size_t>(lod) < mesh_it->second.size() &&
                 static_cast<std::size_t>(lod) <
                     wse::game::WseMetaMesh::kMaxLods;
                 ++lod) {
                wse::game::WseMesh clone =
                    mesh_it->second[static_cast<std::size_t>(lod)];
                clone.set_material(mat_found ? std::string_view{material_name}
                                             : std::string_view{});
                if (color > 0) {
                    clone.set_color(static_cast<std::uint32_t>(color));
                }
                const auto u_lod = static_cast<std::size_t>(lod);
                if (meta.lods.size() <= u_lod) {
                    meta.lods.resize(u_lod + 1);
                }
                meta.lods[u_lod].push_back(std::move(clone));
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_skeleton_model: {
        // 3813 (0xee5, L4645): parses skeleton name and updates m_skeletonModel
        // only when resolved in resource registry (0x1402f2150).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        std::string skel_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, skel_name)) {
            return WseDispatch::ignored;
        }
        if (const auto it = world->resource_skeleton_models.find(skel_name);
            it != world->resource_skeleton_models.end() && it->second) {
            world->items[static_cast<std::size_t>(narrow[0])].skeleton_model =
                std::move(skel_name);
        }
        return WseDispatch::handled_no_store;
    }

    // String group 4200..4211. A failed mbCheckStringRegister returns true
    // without an effect; condition handlers therefore report `ignored`, which
    // the trigger runtime treats as a passing condition.
    case WseOpcode::str_equals: {
        // 4200 (0x1068 equality branch, 0x1401ed1bd): compare operands 0 and
        // 1 with wcscmp or _wcsicmp when operand 2 is nonzero.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_equals(wse_resolve_string(narrow[0], tag0, strings),
                                 wse_resolve_string(narrow[1], tag1, strings),
                                 case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_contains: {
        // 4201 (0x1069): lower-case both strings when operand 2 is nonzero,
        // then find the needle from 0. An empty needle is not contained.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_contains(wse_resolve_string(narrow[0], tag0, strings),
                                   wse_resolve_string(narrow[1], tag1, strings),
                                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_starts_with: {
        // 4202 (0x106a): compare Left(haystack, needle length) with the
        // needle, using the same case rule as str_equals.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_starts_with(
                   wse_resolve_string(narrow[0], tag0, strings),
                   wse_resolve_string(narrow[1], tag1, strings),
                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_ends_with: {
        // 4203 (0x106b): compare Right(haystack, needle length) with the
        // needle, using the same case rule as str_equals.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_ends_with(
                   wse_resolve_string(narrow[0], tag0, strings),
                   wse_resolve_string(narrow[1], tag1, strings),
                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_is_alpha: {
        // 4204 (0x106c): iswalpha over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswalpha(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_is_digit: {
        // 4205 (0x106d): iswdigit over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswdigit(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_is_whitespace: {
        // 4206 (0x106e): iswspace over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswspace(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_length: {
        // 4207 (0x106f): length of operand 1 written through operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            wse_resolve_string(narrow[1], tag, strings).size());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_index_of: {
        // 4208 (0x1070): operand 1 is the haystack, operand 2 the needle,
        // operand 3 the start (default 0), operand 4 the end (default
        // length-1). The result is -1 unless the match is inside [start, end].
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t haystack_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], haystack_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto haystack =
            wse_resolve_string(narrow[1], haystack_tag, strings);
        const auto needle = wse_resolve_string(narrow[2], needle_tag, strings);
        const std::int32_t start = narrow.size() >= 4 ? narrow[3] : 0;
        const std::int32_t end =
            narrow.size() >= 5
                ? narrow[4]
                : static_cast<std::int32_t>(haystack.size()) - 1;
        std::int64_t result = -1;
        if (start >= 0 && end >= 0 &&
            static_cast<std::size_t>(start) < haystack.size() &&
            static_cast<std::size_t>(end) < haystack.size() && start <= end &&
            !needle.empty()) {
            const auto found =
                haystack.find(needle, static_cast<std::size_t>(start));
            if (found != std::u16string_view::npos &&
                found <= static_cast<std::size_t>(end)) {
                result = static_cast<std::int64_t>(found);
            }
        }
        effect.value = result;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_last_index_of: {
        // 4209 (0x1071): mirror the retail loop, which advances past each
        // match and returns the previous match when the next search starts at
        // or beyond `end`; the final result must also be <= end.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t haystack_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], haystack_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto haystack =
            wse_resolve_string(narrow[1], haystack_tag, strings);
        const auto needle = wse_resolve_string(narrow[2], needle_tag, strings);
        const std::int32_t start = narrow.size() >= 4 ? narrow[3] : 0;
        const std::int32_t end =
            narrow.size() >= 5
                ? narrow[4]
                : static_cast<std::int32_t>(haystack.size()) - 1;
        std::int64_t result = -1;
        if (start >= 0 && end >= 0 &&
            static_cast<std::size_t>(start) < haystack.size() &&
            static_cast<std::size_t>(end) < haystack.size() && start <= end) {
            std::int64_t found = -1;
            std::int64_t previous = -1;
            std::int32_t position = start;
            for (;;) {
                previous = found;
                if (end <= position) {
                    break;
                }
                if (needle.empty()) {
                    found = -1;
                } else {
                    const auto at = haystack.find(
                        needle, static_cast<std::size_t>(position));
                    found = at == std::u16string_view::npos
                                ? -1
                                : static_cast<std::int64_t>(at);
                }
                position = static_cast<std::int32_t>(found + 1);
                if (found == -1) {
                    break;
                }
            }
            if (previous <= end) {
                result = previous;
            }
        }
        effect.value = result;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_get_char: {
        // 4210 (0x1072): UTF-16 code unit of operand 1 at operand 2. An
        // out-of-bounds index logs and stores nothing.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_resolve_string(narrow[1], tag, strings);
        const std::int32_t index = narrow.size() >= 3 ? narrow[2] : 0;
        if (index < 0 || static_cast<std::size_t>(index) >= text.size()) {
            return WseDispatch::ignored;
        }
        effect.value =
            static_cast<std::int64_t>(text[static_cast<std::size_t>(index)]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_to_num: {
        // 4211 (0x1073): _wtof of operand 1, scaled by the fixed-point factor
        // when operand 2 is nonzero, then truncated to int64.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        double value = wse_wtof(wse_resolve_string(narrow[1], tag, strings));
        if (narrow.size() >= 3 && narrow[2] != 0) {
            value *= static_cast<double>(kWseFixedPointScale);
        }
        effect.value = wse_trunc64(value);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_compare: {
        // 4212 (0x1074): operand 1 and 2 are strings, operand 3 selects
        // _wcsicmp (0x1401f86f0) instead of the normalised compare
        // (0x1401f86b0). The result is written through operand 0.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        const std::uint8_t tag2 = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], tag1, strings) ||
            !wse_string_register_ok(narrow[2], tag2, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 4 && narrow[3] != 0;
        effect.value = wse_string_compare(
            wse_resolve_string(narrow[1], tag1, strings),
            wse_resolve_string(narrow[2], tag2, strings), case_insensitive);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_split: {
        // 4213 (0x1075): split operand 2 by operand 3 into tag-0 registers
        // starting at operand 1, at most operand 5 pieces, skipping empty
        // pieces when operand 4 is nonzero. Operand 0 receives the count.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t source_tag = wse_tag_at(tags, 2);
        const std::uint8_t delimiter_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[2], source_tag, strings) ||
            !wse_string_register_ok(narrow[3], delimiter_tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::u16string source(
            wse_resolve_string(narrow[2], source_tag, strings));
        const std::u16string delimiter(
            wse_resolve_string(narrow[3], delimiter_tag, strings));
        const bool skip_empty = narrow.size() >= 5 && narrow[4] != 0;
        const std::int32_t max_pieces =
            narrow.size() >= 6 ? narrow[5] : 0;
        std::int32_t destination = narrow[1];
        std::int32_t count = 0;

        if (delimiter.empty()) {
            // The retail loop cannot advance when the delimiter is empty and
            // would spin (forever when skip_empty is set). Treat the input as
            // a single piece so the port always terminates.
            const bool keep = !skip_empty || !source.empty();
            if (keep && !wse_store_string(strings, destination, source)) {
                return WseDispatch::ignored;
            }
            effect.value = keep ? 1 : 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }

        std::int32_t position = 0;
        while (position <= static_cast<std::int32_t>(source.size())) {
            const auto at =
                source.find(delimiter, static_cast<std::size_t>(position));
            const std::int32_t found =
                at == std::u16string::npos
                    ? static_cast<std::int32_t>(source.size())
                    : static_cast<std::int32_t>(at);
            if (narrow.size() >= 6 && max_pieces != 0 &&
                count >= max_pieces) {
                break;
            }
            if (destination > 0x7F) {
                break;
            }
            const auto piece = std::u16string_view(source).substr(
                static_cast<std::size_t>(position),
                static_cast<std::size_t>(found - position));
            if (!skip_empty || !piece.empty()) {
                if (!wse_store_string(strings, destination, piece)) {
                    break;
                }
                ++destination;
                ++count;
            }
            position = found + static_cast<std::int32_t>(delimiter.size());
        }
        effect.value = count;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_sort: {
        // 4214 (0x1076): sort operand 1 tag-0 registers starting at operand 0.
        // Operand 2 selects the case-insensitive comparator; operand 3 copies
        // the sorted range back in reverse order (descending).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::int32_t count = narrow.size() >= 2 ? narrow[1] : 0;
        if (count < 1) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t first = narrow[0];
        if (first + count > 0x80) {
            count = 0x80 - first;
        }
        if (static_cast<std::size_t>(first + count) > strings.registers.size()) {
            count = static_cast<std::int32_t>(strings.registers.size()) - first;
        }
        if (count < 1) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        const bool descending = narrow.size() >= 4 && narrow[3] != 0;
        auto* begin = strings.registers.data() + first;
        auto* end = begin + count;
        std::sort(begin, end,
                  [case_insensitive](const std::u16string& left,
                                     const std::u16string& right) {
                      return wse_string_compare(left, right, case_insensitive) <
                             0;
                  });
        if (descending) {
            std::reverse(begin, end);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_lower: {
        // 4215 (0x1077): MakeLower (0x1401f8b40) of operand 1 into tag-0
        // register operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        for (char16_t& value : text) {
            value = wse_fold_case(value);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_upper: {
        // 4216 (0x1078): MakeUpper (0x1401f8ac0) of operand 1 into tag-0
        // register operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        for (char16_t& value : text) {
            value = wse_upper_case(value);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_trim: {
        // 4217 (0x1079): operand 2 selects Trim (0x1401f8e20), TrimLeft
        // (0x1401f8d00), or TrimRight (0x1401f8c40); other values copy the
        // source unchanged.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            wse_trim(text);
        } else if (mode == 1) {
            wse_trim_left(text);
        } else if (mode == 2) {
            wse_trim_right(text);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace: {
        // 4218 (0x107a): replace occurrences of operand 2 in operand 1 with
        // operand 3 (0x1401f8710) and store the result in tag-0 operand 0.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t source_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        const std::uint8_t replacement_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[1], source_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings) ||
            !wse_string_register_ok(narrow[3], replacement_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], source_tag, strings)),
            wse_resolve_string(narrow[2], needle_tag, strings),
            wse_resolve_string(narrow[3], replacement_tag, strings));
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_md5: {
        // 4219 (0x107b, 0x1401ee956): UTF-8 MD5 digest formatted as 32
        // lowercase hex digits in tag-0 operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto digest = wse_md5_hex(
            wse_utf16_to_utf8(wse_resolve_string(narrow[1], tag, strings)));
        return wse_store_string(strings, narrow[0], digest)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_substring: {
        // 4220 (0x107c): substring of operand 1 from operand 2 (default 0)
        // with operand 3 characters (default to the end). An out-of-bounds
        // range logs and leaves the destination unchanged.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::u16string source(
            wse_resolve_string(narrow[1], tag, strings));
        const std::int32_t start = narrow.size() >= 3 ? narrow[2] : 0;
        const std::int32_t length =
            narrow.size() >= 4
                ? narrow[3]
                : static_cast<std::int32_t>(source.size()) - start;
        if (start < 0 || length < 0 ||
            static_cast<std::int64_t>(start) + length >
                static_cast<std::int64_t>(source.size())) {
            return WseDispatch::ignored;
        }
        const auto piece = std::u16string_view(source).substr(
            static_cast<std::size_t>(start),
            static_cast<std::size_t>(length));
        return wse_store_string(strings, narrow[0], piece)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_reverse: {
        // 4221 (0x107d): reverse the UTF-16 code units of operand 1 into
        // tag-0 operand 0 (std::reverse at 0x1404992b0).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        std::reverse(text.begin(), text.end());
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_join: {
        // 4222 (0x107e): join operand 2 tag-0 registers starting at operand 1
        // with operand 3 (only when the statement supplies it and the
        // separator register is valid) into operand 0.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1])) {
            return WseDispatch::ignored;
        }
        const std::int32_t count = narrow[2];
        if (count < 1) {
            return WseDispatch::handled_no_store;
        }
        std::u16string separator;
        bool has_separator = false;
        if (narrow.size() >= 4) {
            const std::uint8_t separator_tag = wse_tag_at(tags, 3);
            if (wse_string_register_ok(narrow[3], separator_tag, strings)) {
                separator.assign(
                    wse_resolve_string(narrow[3], separator_tag, strings));
                has_separator = true;
            }
        }
        std::int32_t end = narrow[1] + count;
        if (end > 0x80) {
            end = 0x80;
        }
        if (static_cast<std::size_t>(end) > strings.registers.size()) {
            end = static_cast<std::int32_t>(strings.registers.size());
        }
        std::u16string joined;
        for (std::int32_t index = narrow[1]; index < end; ++index) {
            joined.append(strings.registers[static_cast<std::size_t>(index)]);
            if (has_separator && index < end - 1) {
                joined.append(separator);
            }
        }
        return wse_store_string(strings, narrow[0], joined)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace_spaces_with_underscores: {
        // 4223 (0x107f): replace u" " (0x14055b3e0) with u"_"
        // (0x14055b3dc) in operand 1 and store the result in operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], tag, strings)), u" ",
            u"_");
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace_underscores_with_spaces: {
        // 4224 (0x1080): the reverse of 4223, replacing u"_" with u" ".
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], tag, strings)), u"_",
            u" ");
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_float: {
        // 4233 (0x1089, 0x1401ef3a8): format fp[operand 1] with "%.*f"
        // (0x14055b3e4) into tag-0 operand 0; precision defaults to 5 when
        // fewer than 3 operands or negative.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto fp_index = static_cast<std::size_t>(narrow[1]);
        if (fp_index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const std::int32_t precision =
            (narrow.size() < 3 || narrow[2] < 0) ? 5 : narrow[2];
        return wse_store_string(strings, narrow[0],
                                wse_format_float(fp[fp_index], precision))
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_sanitize: {
        // 4234 (0x108a, 0x1401ef436): strip '{', '}', '<', '>' from tag-0
        // register operand 0 in place.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= strings.registers.size()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        strings.registers[index] = wse_sanitize_string(strings.registers[index]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_is_integer: {
        // 4236 (0x108c, 0x1401ef55f): trim both ends, reject empty or bare
        // '-', and check remaining code units with iswdigit (0x140498860).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[0], tag, strings));
        wse_trim(text);
        if (text.empty()) {
            return WseDispatch::condition_false;
        }
        std::size_t index = text[0] == u'-' ? 1 : 0;
        if (index >= text.size()) {
            return WseDispatch::condition_false;
        }
        for (; index < text.size(); ++index) {
            if (std::iswdigit(static_cast<std::wint_t>(text[index])) == 0) {
                return WseDispatch::condition_false;
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_server_password_rcon: {
        // 4238 (0x108e -> 0x1401f49c1): retail jump table exits immediately
        // returning true with no side effects.
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_regex_match:
    case WseOpcode::str_regex_search: {
        // 4240/4241 (0x1090/0x1091, 0x1401ef741/0x1401ef84d): ECMAScript regex
        // match/search over UTF-8 operand 0 with pattern operand 1.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 0);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], text_tag, strings) ||
            !wse_string_register_ok(narrow[1], pattern_tag, strings)) {
            return WseDispatch::ignored;
        }
        const bool full_match =
            static_cast<WseOpcode>(opcode) == WseOpcode::str_regex_match;
        return wse_regex_predicate(
            wse_resolve_string(narrow[0], text_tag, strings),
            wse_resolve_string(narrow[1], pattern_tag, strings), full_match);
    }
    case WseOpcode::str_regex_get_matches: {
        // 4242 (0x1092, 0x1401ef959): write full regex matches of operand 2
        // against operand 1 into tag-0 registers starting at operand 0,
        // stopping after optional operand 3 matches when nonzero or at s127.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 1);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], text_tag, strings) ||
            !wse_string_register_ok(narrow[2], pattern_tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::string text_utf8 = wse_utf16_to_utf8(
            wse_resolve_string(narrow[1], text_tag, strings));
        const std::string pattern_utf8 = wse_utf16_to_utf8(
            wse_resolve_string(narrow[2], pattern_tag, strings));
        const std::int32_t max_matches = narrow.size() >= 4 ? narrow[3] : 0;
        std::int32_t dest = narrow[0];
        std::int32_t count = 0;
        try {
            const std::regex re(pattern_utf8);
            std::sregex_iterator it(text_utf8.begin(), text_utf8.end(), re);
            const std::sregex_iterator end;
            for (; it != end; ++it) {
                if ((max_matches != 0 && count == max_matches) || dest >= 0x80 ||
                    static_cast<std::size_t>(dest) >= strings.registers.size()) {
                    break;
                }
                strings.registers[static_cast<std::size_t>(dest)] =
                    wse_utf8_to_utf16(it->str(0));
                ++dest;
                ++count;
            }
        } catch (...) {
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_regex_replace: {
        // 4243 (0x1093, 0x1401efbc0): regex_replace(operand 1, operand 2,
        // operand 3) stored in tag-0 operand 0.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 1);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 2);
        const std::uint8_t replacement_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[1], text_tag, strings) ||
            !wse_string_register_ok(narrow[2], pattern_tag, strings) ||
            !wse_string_register_ok(narrow[3], replacement_tag, strings)) {
            return WseDispatch::ignored;
        }
        try {
            const std::string text_utf8 = wse_utf16_to_utf8(
                wse_resolve_string(narrow[1], text_tag, strings));
            const std::regex re(wse_utf16_to_utf8(
                wse_resolve_string(narrow[2], pattern_tag, strings)));
            const std::string replacement_utf8 = wse_utf16_to_utf8(
                wse_resolve_string(narrow[3], replacement_tag, strings));
            const std::string replaced =
                std::regex_replace(text_utf8, re, replacement_utf8);
            return wse_store_string(strings, narrow[0],
                                    wse_utf8_to_utf16(replaced))
                       ? WseDispatch::handled_no_store
                       : WseDispatch::ignored;
        } catch (...) {
            return WseDispatch::handled_no_store;
        }
    }
    case WseOpcode::str_decode_url: {
        // 4244 (0x1094, 0x1401efd4d): UrlUnescapeA (0x1403108b0) on UTF-8
        // operand 1 stored into tag-0 operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto decoded = wse_utf8_to_utf16(wse_url_unescape(
            wse_utf16_to_utf8(wse_resolve_string(narrow[1], tag, strings))));
        return wse_store_string(strings, narrow[0], decoded)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_module_setting: {
        // 4226 (0x1082, 0x1401ef0ef / L6003): (sreg, setting, section).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings) ||
            !wse_string_register_ok(narrow[2], wse_tag_at(tags, 2), strings)) {
            return WseDispatch::ignored;
        }
        std::string setting;
        std::string section;
        if (!wse_resolve_parsed_utf8_operand(1, operands, setting) ||
            !wse_resolve_parsed_utf8_operand(2, operands, section)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(setting);
        wse_spaces_to_underscores(section);
        std::string_view found{};
        if (const auto* const world = operands.state.world; world != nullptr) {
            if (const auto sec_it = world->module_settings.find(section);
                sec_it != world->module_settings.end()) {
                if (const auto val_it = sec_it->second.find(setting);
                    val_it != sec_it->second.end()) {
                    found = val_it->second;
                }
            }
        }
        return wse_store_dict_string(strings, narrow[0], found);
    }
    case WseOpcode::str_store_game_variable: {
        // 4231 (0x1087, L6075): (sreg, variable) from game_variables.txt.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        std::string variable;
        if (!wse_resolve_parsed_utf8_operand(1, operands, variable)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(variable);
        std::string_view found{};
        if (const auto* const world = operands.state.world; world != nullptr) {
            if (const auto it = world->game_variables.find(variable);
                it != world->game_variables.end()) {
                found = it->second;
            }
        }
        return wse_store_dict_string(strings, narrow[0], found);
    }
    case WseOpcode::str_store_skill_name:
    case WseOpcode::str_store_skill_desc: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_store_skill_text(
            operands.state.world, strings, narrow[0], narrow[1],
            static_cast<WseOpcode>(opcode) == WseOpcode::str_store_skill_desc);
    }
    case WseOpcode::str_store_item_id:
    case WseOpcode::str_store_item_mesh_name: {
        // 4235 (0x108b, 0x1401ef52e) / 4239 (0x108f, 0x1401ef710).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (!wse_register_ok(narrow[0]) || world == nullptr ||
            !world->is_valid_item(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& item = world->items[static_cast<std::size_t>(narrow[1])];
        if (static_cast<WseOpcode>(opcode) == WseOpcode::str_store_item_id) {
            return wse_store_dict_string(strings, narrow[0], item.id);
        }
        const std::string_view mesh_name =
            item.variations.empty() ? std::string_view{}
                                    : std::string_view{item.variations[0].mesh_name};
        return wse_store_dict_string(strings, narrow[0], mesh_name);
    }
    case WseOpcode::str_store_savegame_md5: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_store_savegame_md5(operands.state.world, strings, narrow[0],
                                      narrow[1]);
    }

    // Options flags (0x10cc..0x10d1, 0x1401efff9..0x1401f003b) and bool
    // config setters (3619/3622/4606).
    case WseOpcode::options_get_verbose_casualties:
    case WseOpcode::options_get_cheat_mode:
    case WseOpcode::options_get_realistic_headshots: {
        const bool* flag = wse_option_flag(operands.state.options, opcode);
        effect.value = (flag && *flag) ? 1 : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::options_set_verbose_casualties:
    case WseOpcode::options_set_cheat_mode:
    case WseOpcode::options_set_realistic_headshots:
    case WseOpcode::set_horse_friendly_fire:
    case WseOpcode::set_show_crosshair:
    case WseOpcode::edit_mode_set_enabled: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        bool* flag = wse_option_flag(operands.state.options, opcode);
        if (!flag) {
            return WseDispatch::ignored;
        }
        *flag = narrow[0] != 0;
        return WseDispatch::handled_no_store;
    }
    // doing anything; a failed check falls through to `return true`, which is
    // a successful no-op.
    case WseOpcode::fld: {
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[index] = static_cast<float>(wide[1]) / kWseFixedPointScale;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fld_str: {
        // 4401 (0x1131, 0x1401f00c7): atof (0x1404989cc) of operand 1 stored as
        // float in fp[operand 0].
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        fp[index] = static_cast<float>(
            wse_wtof(wse_resolve_string(narrow[1], tag, strings)));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fld_pos_x:
    case WseOpcode::fld_pos_y:
    case WseOpcode::fld_pos_z: {
        // 4402..4404 (0x1132..0x1134, 0x1401f0157..0x1401f01ff):
        // copies pos[op1].o.{x,y,z} into fp[op0].
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            fp.empty() || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto fp_idx = static_cast<std::size_t>(narrow[0]);
        const auto pos_idx = static_cast<std::size_t>(narrow[1]);
        if (fp_idx >= fp.size() || pos_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        const WseVec4& origin = positions[pos_idx].o;
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fld_pos_x:
            fp[fp_idx] = origin.x;
            break;
        case WseOpcode::fld_pos_y:
            fp[fp_idx] = origin.y;
            break;
        case WseOpcode::fld_pos_z:
            fp[fp_idx] = origin.z;
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fst: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(kWseFixedPointScale * fp[index]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::fcpy: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[destination] = fp[source];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::feq: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float difference = std::fabs(fp[lhs] - fp[rhs]);
        return difference <= FLT_EPSILON ? WseDispatch::handled_no_store
                                         : WseDispatch::condition_false;
    }
    case WseOpcode::fgt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[rhs] <= fp[lhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::flt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[lhs] <= fp[rhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::fge: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (a < b || a == b) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fle: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (b < a || b == a) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fsub:
    case WseOpcode::fmul:
    case WseOpcode::fdiv:
    case WseOpcode::fmin:
    case WseOpcode::fmax:
    case WseOpcode::fadd: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs = static_cast<std::size_t>(narrow[1]);
        const auto rhs = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsub:
            fp[destination] = a - b;
            break;
        case WseOpcode::fmul:
            fp[destination] = a * b;
            break;
        case WseOpcode::fdiv:
            if (b == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = a / b;
            break;
        case WseOpcode::fmin:
            fp[destination] = b <= a ? b : a;
            break;
        case WseOpcode::fmax:
            fp[destination] = a <= b ? b : a;
            break;
        case WseOpcode::fadd:
            fp[destination] = a + b;
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fclamp: {
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || !wse_register_ok(narrow[3]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto value = static_cast<std::size_t>(narrow[1]);
        const auto first = static_cast<std::size_t>(narrow[2]);
        const auto second = static_cast<std::size_t>(narrow[3]);
        if (destination >= fp.size() || value >= fp.size() || first >= fp.size() ||
            second >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float v = fp[value];
        const float bound_a = fp[first];
        const float bound_b = fp[second];
        const float low = bound_b <= bound_a ? bound_b : bound_a;
        const float high = bound_a <= bound_b ? bound_b : bound_a;
        fp[destination] = v < low ? low : (v <= high ? v : high);
        return WseDispatch::handled_no_store;
    }

    // Transcendental unary register operations: operand 0 is the destination,
    // operand 1 the source. All register checks happen before evaluation, and
    // a failed check is the retail `return true` no-op.
    case WseOpcode::fsqrt:
    case WseOpcode::fabs:
    case WseOpcode::fceil:
    case WseOpcode::ffloor:
    case WseOpcode::fexp:
    case WseOpcode::fln:
    case WseOpcode::fsin:
    case WseOpcode::fcos:
    case WseOpcode::ftan:
    case WseOpcode::fsinh:
    case WseOpcode::fcosh:
    case WseOpcode::ftanh:
    case WseOpcode::fasin:
    case WseOpcode::facos:
    case WseOpcode::fatan: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float value = fp[source];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsqrt:
            fp[destination] = std::sqrt(value);
            break;
        case WseOpcode::fabs:
            fp[destination] = std::fabs(value);
            break;
        case WseOpcode::fceil:
            fp[destination] = std::ceil(value);
            break;
        case WseOpcode::ffloor:
            fp[destination] = std::floor(value);
            break;
        case WseOpcode::fexp:
            fp[destination] = std::exp(value);
            break;
        case WseOpcode::fln:
            fp[destination] = std::log(value);
            break;
        case WseOpcode::fsin:
            fp[destination] = std::sin(value);
            break;
        case WseOpcode::fcos:
            fp[destination] = std::cos(value);
            break;
        case WseOpcode::ftan:
            fp[destination] = std::tan(value);
            break;
        case WseOpcode::fsinh:
            fp[destination] = std::sinh(value);
            break;
        case WseOpcode::fcosh:
            fp[destination] = std::cosh(value);
            break;
        case WseOpcode::ftanh:
            fp[destination] = std::tanh(value);
            break;
        case WseOpcode::fasin:
            fp[destination] = std::asin(value);
            break;
        case WseOpcode::facos:
            fp[destination] = std::acos(value);
            break;
        case WseOpcode::fatan:
            fp[destination] = std::atan(value);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    // Binary register operations. xmm0 carries operand 1, xmm1 operand 2:
    // pow(base, exponent), atan2(y, x), and fmod(dividend, divisor).
    case WseOpcode::fpow:
    case WseOpcode::fmod:
    case WseOpcode::fatan2: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs_index = static_cast<std::size_t>(narrow[1]);
        const auto rhs_index = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs_index >= fp.size() ||
            rhs_index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float lhs = fp[lhs_index];
        const float rhs = fp[rhs_index];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fpow:
            fp[destination] = std::pow(lhs, rhs);
            break;
        case WseOpcode::fmod:
            if (rhs == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = std::fmod(lhs, rhs);
            break;
        case WseOpcode::fatan2:
            fp[destination] = std::atan2(lhs, rhs);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    case WseOpcode::feval: {
        // 4437 (0x1155, 0x1401e4dc0 L8353..8365): (expression_string).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string expr;
        if (!wse_resolve_parsed_utf8_operand(0, operands, expr)) {
            return WseDispatch::ignored;
        }
        (void)wse_eval_float_expr(expr, fp, operands.state.positions);
        return WseDispatch::handled_no_store;
    }

    // flog: log10(fp[operand 1]) / log10(base). The base is the int32 literal
    // in operand 2; with fewer than three operands the retail default is 10.
    case WseOpcode::flog: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        std::int32_t base = 10;
        if (narrow.size() >= 3) {
            base = narrow[2];
        }
        if (base < 2) {
            return WseDispatch::handled_no_store;  // "invalid logarithm base %d"
        }
        fp[destination] =
            std::log10(fp[source]) / std::log10(static_cast<float>(base));
        return WseDispatch::handled_no_store;
    }

    // VM return values / PRNG / time / timers / profiler (0xbc7..0xbe3).
    case WseOpcode::return_values: {
        // 3015 (0xbc7, 0x1401e5a50): writes up to 16 int64 return slots.
        const std::size_t max_slots =
            std::min<std::size_t>(operands.state.return_values_out.size(), 16U);
        const std::size_t count = std::min<std::size_t>(wide.size(), max_slots);
        for (std::size_t i = 0; i < count; ++i) {
            operands.state.return_values_out[i] = wide[i];
        }
        if (operands.state.num_return_values_out != nullptr) {
            *operands.state.num_return_values_out = static_cast<std::int32_t>(
                std::min<std::size_t>(wide.size(), 16U));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::mtsrand: {
        // 3020 (0xbcc, 0x1401e5b78): seeds rglMersenneTwister.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rng == nullptr) {
            return WseDispatch::ignored;
        }
        operands.state.rng->seed(static_cast<std::uint32_t>(narrow[0]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::mtrand: {
        // 3021 (0xbcd, 0x1401e5b85): rglMersenneTwister::getInt(min, max).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rng == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            operands.state.rng->get_int(narrow[1], narrow[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_time: {
        // 3022 (0xbce, 0x1401e5ba2): UTC Unix epoch seconds (or local-adjusted
        // wall-clock seconds when operand 1 != 0), 32-bit int truncated.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::time_t now = std::time(nullptr);
        std::int64_t seconds =
            static_cast<std::int64_t>(static_cast<std::int32_t>(now));
        if (narrow.size() >= 2 && narrow[1] != 0) {
            seconds = static_cast<std::int64_t>(static_cast<std::int32_t>(
                wse_local_epoch_seconds(static_cast<std::time_t>(seconds))));
        }
        effect.value = seconds;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::timer_reset: {
        // 3027 (0xbd3, 0x1401e5dab): op1 != 0 -> app clock (0), else game (2).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= operands.state.timers.size() ||
            operands.state.timers_total.size() < 3) {
            return WseDispatch::ignored;
        }
        const std::int32_t kind =
            (narrow.size() >= 2 && narrow[1] != 0) ? 0 : 2;
        operands.state.timers[static_cast<std::size_t>(narrow[0])] = {
            kind, operands.state.timers_total[static_cast<std::size_t>(kind)]};
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::timer_get_elapsed_time: {
        // 3028 (0xbd4, 0x1401e5df5): rounded elapsed seconds from 100kHz clock.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) ||
            static_cast<std::size_t>(narrow[1]) >= operands.state.timers.size()) {
            return WseDispatch::ignored;
        }
        const WseAppTimer& timer =
            operands.state.timers[static_cast<std::size_t>(narrow[1])];
        if (timer.kind < 0 ||
            static_cast<std::size_t>(timer.kind) >=
                operands.state.timers_total.size()) {
            return WseDispatch::ignored;
        }
        const std::int64_t total_now =
            operands.state.timers_total[static_cast<std::size_t>(timer.kind)];
        const float elapsed =
            static_cast<float>(total_now - timer.start_ticks) / 100000.0f;
        effect.value = wse_round64(elapsed);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::set_random_seed: {
        // 3034 (0xbda, 0x1401e62bb): stores script rand_seed (DAT_140617d54).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rand_seed == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.rand_seed = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_start: {
        // 3040 (0xbe0, 0x14034f7d0): sets g_profiling flag.
        if (operands.state.profiler_recording == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.profiler_recording = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_stop: {
        // 3041 (0xbe1, 0x14034ff40): clears g_profiling flag.
        if (operands.state.profiler_recording == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.profiler_recording = false;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_is_recording: {
        // 3042 (0xbe2): returns g_profiling as boolean condition.
        const bool recording = operands.state.profiler_recording != nullptr &&
                               *operands.state.profiler_recording;
        return recording ? WseDispatch::handled_no_store
                         : WseDispatch::condition_false;
    }
    case WseOpcode::profiler_mark: {
        // 3043 (0xbe3, 0x1403508a0): appends marker when profiling is active.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string marker;
        if (!wse_resolve_utf8_operand(0, operands, marker)) {
            return WseDispatch::ignored;
        }
        if (operands.state.profiler_recording != nullptr &&
            *operands.state.profiler_recording &&
            operands.state.profiler_markers != nullptr) {
            operands.state.profiler_markers->push_back(std::move(marker));
        }
        return WseDispatch::handled_no_store;
    }

    // Dictionary group (3200..3222 / 0xc80..0xc96).
    case WseOpcode::dict_create: {
        // 3200 (0xc80, 0x1401e6b30): allocates handle in WSEObjectManager.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value =
            static_cast<std::int64_t>(operands.state.dictionaries->create());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_free: {
        // 3201 (0xc81, 0x1401e6b6c): destroys dictionary and frees ID.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries != nullptr) {
            operands.state.dictionaries->destroy(narrow[0]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_file: {
        // 3202 (0xc82): (dict, file_str, [mode], [format]); format 1 = .ini,
        // otherwise binary .wsedict.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        const std::int32_t format = narrow.size() >= 4 ? narrow[3] : 0;
        std::filesystem::path target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  format == 1 ? ".ini" : ".wsedict", target)) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        std::map<std::string, std::string> loaded;
        if (format == 1) {
            (void)wse_dict_load_ini(target, loaded);
        } else {
            (void)wse_dict_load_binary(target, loaded);
        }
        wse_dict_apply_load(*dict, loaded, mode);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_dict: {
        // 3203 (0xc83): (dst, src, [mode]); mode 0 is no-op, 1 overwrites,
        // 2 inserts missing keys.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        WseDictionary* dst = operands.state.dictionaries->get(narrow[0]);
        const WseDictionary* src = operands.state.dictionaries->get(narrow[1]);
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (!dst || !src || mode == 0 || dst == src) {
            return WseDispatch::handled_no_store;
        }
        if (mode == 1 || mode == 2) {
            dst->merge(src->entries, mode == 1);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_save: {
        // 3204 (0xc84): (dict, file_str, [format]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        const std::int32_t format = narrow.size() >= 3 ? narrow[2] : 0;
        std::filesystem::path target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  format == 1 ? ".ini" : ".wsedict", target)) {
            return WseDispatch::handled_no_store;
        }
        if (format == 1) {
            wse_dict_save_ini(target, dict->entries);
        } else {
            wse_dict_save_binary(target, dict->entries);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_clear: {
        // 3205 (0xc85).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->clear();
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_is_empty: {
        // 3206 (0xc86): invalid dict returns true matching executeWSE.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        if (!dict || dict->empty()) {
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::condition_false;
    }
    case WseOpcode::dict_has_key: {
        // 3207 (0xc87).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        if (!dict) {
            return WseDispatch::condition_false;
        }
        return dict->has_key(key) ? WseDispatch::handled_no_store
                                  : WseDispatch::condition_false;
    }
    case WseOpcode::dict_get_size: {
        // 3208 (0xc88): (dest, dict).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        effect.value = dict ? static_cast<std::int64_t>(dict->size()) : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_delete_file: {
        // 3209 (0xc89): (file_str, [format]).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const std::int32_t format = narrow.size() >= 2 ? narrow[1] : 0;
        std::filesystem::path target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 format == 1 ? ".ini" : ".wsedict", target)) {
            std::error_code ec;
            (void)std::filesystem::remove(target, ec);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_get_str: {
        // 3210 (0xc8a): (sreg, dict, key_str, [default_str]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        std::string def_value;
        if (narrow.size() >= 4 &&
            !wse_resolve_utf8_operand(3, operands, def_value)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const auto it = dict->entries.find(key);
        return wse_store_dict_string(
            strings, narrow[0],
            it != dict->entries.end() ? std::string_view(it->second)
                                      : std::string_view(def_value));
    }
    case WseOpcode::dict_get_int: {
        // 3211 (0xc8b): (dest, dict, key_str, [default_int]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        std::int32_t value = 0;
        if (dict) {
            const auto it = dict->entries.find(key);
            if (it != dict->entries.end()) {
                value = static_cast<std::int32_t>(
                    std::strtol(it->second.c_str(), nullptr, 10));
            } else {
                value = narrow.size() >= 4 ? narrow[3] : 0;
            }
        }
        effect.value = static_cast<std::int64_t>(value);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_set_str: {
        // 3212 (0xc8c): (dict, key_str, val_str).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        std::string value;
        if (!wse_resolve_utf8_operand(1, operands, key) ||
            !wse_resolve_utf8_operand(2, operands, value)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->set(key, std::move(value));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_set_int: {
        // 3213 (0xc8d): (dict, key_str, int_val).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->set(key, std::to_string(narrow[2]));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_get_key_by_iterator: {
        // 3214 (0xc8e, 0x1402aae20): (sreg, dict, [index]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t steps = narrow.size() >= 3 ? narrow[2] : 0;
        auto it = dict->entries.begin();
        for (std::int32_t i = 0; i < steps && it != dict->entries.end(); ++i) {
            ++it;
        }
        return wse_store_dict_string(
            strings, narrow[0],
            it != dict->entries.end() ? std::string_view(it->first)
                                      : std::string_view{});
    }
    case WseOpcode::dict_get_pos: {
        // 3215 (0xc8f, 0x1402aa5a0): (pos_dest, dict, key_str, [default_pos]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || positions.empty() ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        WseTransform fallback = WseTransform::identity();
        if (narrow.size() >= 4) {
            if (!wse_register_ok(narrow[3]) ||
                static_cast<std::size_t>(narrow[3]) >= positions.size()) {
                return WseDispatch::ignored;
            }
            fallback = positions[static_cast<std::size_t>(narrow[3])];
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        positions[static_cast<std::size_t>(narrow[0])] =
            wse_dict_get_transform(*dict, key, fallback);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_set_pos: {
        // 3216 (0xc90, 0x1402aa920): (dict, key_str, pos_no).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[2]) || positions.empty() ||
            static_cast<std::size_t>(narrow[2]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            wse_dict_set_transform(
                *dict, key, positions[static_cast<std::size_t>(narrow[2])]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_file_json: {
        // 3217 (0xc91, 0x1402a97f0): (dict, file_str, [mode]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        std::filesystem::path target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  ".json", target)) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        std::string raw_json;
        std::map<std::string, std::string> loaded;
        if (wse_dict_read_file_bytes(target, raw_json)) {
            (void)wse_parse_json_object(raw_json, loaded);
        }
        wse_dict_apply_load(*dict, loaded, mode);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_save_json: {
        // 3218 (0xc92, 0x1402a9f70): (dict, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        std::filesystem::path target;
        if (dict && wse_dict_target_path(operands.state.dictionaries, file_name,
                                         ".json", target)) {
            (void)wse_dict_write_file_bytes(
                target, wse_serialize_json_object(dict->entries, true));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_from_url_encoded_json: {
        // 3219 (0xc93, 0x1402aaea0): (dict, encoded_str, [mode]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string encoded;
        if (!wse_resolve_utf8_operand(1, operands, encoded)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        for (char& ch : encoded) {
            if (ch == '+') {
                ch = ' ';
            }
        }
        const std::string json_text = wse_url_unescape(encoded);
        std::map<std::string, std::string> loaded;
        if (wse_parse_json_object(json_text, loaded)) {
            wse_dict_apply_load(*dict, loaded, mode);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_to_url_encoded_json: {
        // 3220 (0xc94, 0x1402ab370): (sreg, dict).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::string compact =
            wse_serialize_json_object(dict->entries, false);
        return wse_store_dict_string(strings, narrow[0],
                                     wse_url_escape(compact));
    }
    case WseOpcode::dict_erase: {
        // 3221 (0xc95, 0x1402ab900): (dict, key_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->entries.erase(key);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_delete_file_json: {
        // 3222 (0xc96): (file_str).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::filesystem::path target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 ".json", target)) {
            std::error_code ec;
            (void)std::filesystem::remove(target, ec);
        }
        return WseDispatch::handled_no_store;
    }

    // Edit mode group (4600..4605, 4607..4609; 4606 shares wse_option_flag).
    case WseOpcode::edit_mode_get_num_selected_prop_instances: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = (world != nullptr && world->has_game &&
                        world->edit_mode.active)
                           ? static_cast<std::int64_t>(
                                 world->edit_mode.selected_prop_instances.size())
                           : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_get_selected_prop_instance: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = -1;
        if (world != nullptr && world->has_game && world->edit_mode.active &&
            narrow[1] >= 0 &&
            static_cast<std::size_t>(narrow[1]) <
                world->edit_mode.selected_prop_instances.size()) {
            effect.value =
                world->edit_mode
                    .selected_prop_instances[static_cast<std::size_t>(narrow[1])];
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_select_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_mission_object(narrow[0]) ||
            !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        if (wse_edit_mode_find_selected(world->edit_mode, narrow[0]) < 0) {
            world->edit_mode.selected_prop_instances.push_back(narrow[0]);
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_deselect_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_mission_object(narrow[0]) ||
            !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        const std::int32_t idx =
            wse_edit_mode_find_selected(world->edit_mode, narrow[0]);
        if (idx >= 0) {
            if (world->edit_mode.highlighted_index == idx) {
                world->edit_mode.highlighted_index = -1;
            } else if (world->edit_mode.highlighted_index > idx) {
                --world->edit_mode.highlighted_index;
            }
            world->edit_mode.selected_prop_instances.erase(
                world->edit_mode.selected_prop_instances.begin() + idx);
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_get_highlighted_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = -1;
        if (world != nullptr && world->has_game && world->edit_mode.active &&
            world->edit_mode.highlighted_index >= 0 &&
            static_cast<std::size_t>(world->edit_mode.highlighted_index) <
                world->edit_mode.selected_prop_instances.size()) {
            effect.value =
                world->edit_mode.selected_prop_instances
                    [static_cast<std::size_t>(world->edit_mode.highlighted_index)];
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_set_highlighted_prop_instance: {
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game || !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        const std::int32_t target = narrow.empty() ? -1 : narrow[0];
        const std::int32_t idx =
            wse_edit_mode_find_selected(world->edit_mode, target);
        if (world->edit_mode.highlighted_index != idx) {
            world->edit_mode.highlighted_index = idx;
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_in_edit_objects_mode: {
        const auto* const world = operands.state.world;
        return (world != nullptr && world->has_game && world->edit_mode.active &&
                world->edit_mode.mode == 0)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::edit_mode_get_mode: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = (world != nullptr && world->has_game)
                           ? world->edit_mode.mode
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_window_open: {
        const auto* const world = operands.state.world;
        return (world != nullptr && world->has_game &&
                world->edit_mode.dialog_open)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }

    // Dynamic array group (5000..5022 / 0x1388..0x139e).
    case WseOpcode::array_create: {
        // 5000 (0x1388): (dst, type_id, dim0, [dim1..dim13]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        const std::int32_t type_id = narrow[1];
        const auto dims = narrow.subspan(2);
        if (type_id < 0 || type_id > 2 || !WseDynArray::dims_valid(dims)) {
            effect.value = 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        auto arr =
            WseDynArray::create(static_cast<WseArrayType>(type_id), dims);
        effect.value = arr ? static_cast<std::int64_t>(
                                 operands.state.dictionaries->create_array(
                                     std::move(arr)))
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_free: {
        // 5001 (0x1389): (array_id).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries != nullptr) {
            operands.state.dictionaries->destroy_array(narrow[0]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_copy: {
        // 5002 (0x138a): (dst, src_array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* src =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!src) {
            return WseDispatch::handled_no_store;
        }
        auto copy = src->clone();
        effect.value = copy ? static_cast<std::int64_t>(
                                  operands.state.dictionaries->create_array(
                                      std::move(copy)))
                            : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_save_file: {
        // 5003 (0x138b): (array_id, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        std::filesystem::path target;
        if (arr && wse_dict_target_path(operands.state.dictionaries, file_name,
                                        ".wsearray", target)) {
            (void)wse_array_save_binary(target, *arr);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_load_file: {
        // 5004 (0x138c): (dst, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::filesystem::path target;
        if (!wse_dict_target_path(operands.state.dictionaries, file_name,
                                  ".wsearray", target)) {
            return WseDispatch::handled_no_store;
        }
        std::unique_ptr<WseDynArray> loaded;
        bool version_too_new = false;
        bool bad_type_id = false;
        if (!wse_array_load_binary(target, loaded, version_too_new,
                                   bad_type_id)) {
            if (bad_type_id) {
                return WseDispatch::handled_no_store;
            }
            effect.value = 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        effect.value = static_cast<std::int64_t>(
            operands.state.dictionaries->create_array(std::move(loaded)));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_delete_file: {
        // 5005 (0x138d): (file_str).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::filesystem::path target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 ".wsearray", target)) {
            std::error_code ec;
            (void)std::filesystem::remove(target, ec);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_set_val: {
        // 5006 (0x138e): (array_id, value, index0..13).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        std::string str_val;
        if (arr->type == WseArrayType::string) {
            if (!wse_resolve_parsed_utf8_operand(1, operands, str_val)) {
                return WseDispatch::ignored;
            }
        } else if (arr->type == WseArrayType::position) {
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
        }
        std::size_t leaf_idx = 0;
        WseArraySlice* leaf = arr->leaf_for(narrow.subspan(2), leaf_idx);
        if (!leaf) {
            return WseDispatch::handled_no_store;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            leaf->ints[leaf_idx] = narrow[1];
            break;
        case WseArrayType::string:
            leaf->strings[leaf_idx] = std::move(str_val);
            break;
        case WseArrayType::position:
            leaf->positions[leaf_idx] =
                operands.state.positions[static_cast<std::size_t>(narrow[1])];
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_set_val_all: {
        // 5007 (0x138f): (array_id, value).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!arr || !arr->root) {
            return WseDispatch::handled_no_store;
        }
        std::int32_t int_val = 0;
        std::string str_val;
        WseTransform pos_val{};
        switch (arr->type) {
        case WseArrayType::integer:
            int_val = narrow[1];
            break;
        case WseArrayType::string:
            if (!wse_resolve_parsed_utf8_operand(1, operands, str_val)) {
                return WseDispatch::ignored;
            }
            break;
        case WseArrayType::position:
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            pos_val =
                operands.state.positions[static_cast<std::size_t>(narrow[1])];
            break;
        }
        arr->root->fill_all(arr->type, 0, arr->dim_sizes.size(), int_val,
                            str_val, pos_val);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_get_val: {
        // 5008 (0x1390): (dst, array_id, index0..13).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        if ((arr->type == WseArrayType::string ||
             arr->type == WseArrayType::position) &&
            !wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::size_t leaf_idx = 0;
        const WseArraySlice* leaf = arr->leaf_for(narrow.subspan(2), leaf_idx);
        if (!leaf) {
            return WseDispatch::handled_no_store;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            effect.value = static_cast<std::int64_t>(leaf->ints[leaf_idx]);
            effect.store_value = true;
            return WseDispatch::handled;
        case WseArrayType::string:
            return wse_store_dict_string(strings, narrow[0],
                                         leaf->strings[leaf_idx]);
        case WseArrayType::position:
            if (static_cast<std::size_t>(narrow[0]) >=
                operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            operands.state.positions[static_cast<std::size_t>(narrow[0])] =
                leaf->positions[leaf_idx];
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_push: {
        // 5009 (0x1391): (array_id, src_val_or_subarray).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* dst =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!dst || !dst->root) {
            return WseDispatch::handled_no_store;
        }
        if (dst->dim_sizes.size() > 1U) {
            const WseDynArray* src =
                operands.state.dictionaries->get_array(narrow[1]);
            if (src) {
                (void)dst->push_subarray(*src);
            }
            return WseDispatch::handled_no_store;
        }
        switch (dst->type) {
        case WseArrayType::integer:
            dst->root->ints.push_back(narrow[1]);
            dst->dim_sizes[0] = static_cast<std::int32_t>(dst->root->ints.size());
            break;
        case WseArrayType::string: {
            std::string val;
            if (!wse_resolve_parsed_utf8_operand(1, operands, val)) {
                return WseDispatch::ignored;
            }
            dst->root->strings.push_back(std::move(val));
            dst->dim_sizes[0] =
                static_cast<std::int32_t>(dst->root->strings.size());
            break;
        }
        case WseArrayType::position:
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            dst->root->positions.push_back(
                operands.state.positions[static_cast<std::size_t>(narrow[1])]);
            dst->dim_sizes[0] =
                static_cast<std::int32_t>(dst->root->positions.size());
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_pop: {
        // 5010 (0x1392): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr || !arr->root) {
            return WseDispatch::handled_no_store;
        }
        if (arr->dim_sizes.size() > 1U) {
            auto sub = arr->pop_subarray();
            if (!sub) {
                return WseDispatch::handled_no_store;
            }
            effect.value = static_cast<std::int64_t>(
                operands.state.dictionaries->create_array(std::move(sub)));
            effect.store_value = true;
            return WseDispatch::handled;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            if (arr->root->ints.empty()) {
                return WseDispatch::handled_no_store;
            }
            effect.value = static_cast<std::int64_t>(arr->root->ints.back());
            arr->root->ints.pop_back();
            arr->dim_sizes[0] = static_cast<std::int32_t>(arr->root->ints.size());
            effect.store_value = true;
            return WseDispatch::handled;
        case WseArrayType::string: {
            if (!wse_register_ok(narrow[0])) {
                return WseDispatch::ignored;
            }
            if (arr->root->strings.empty()) {
                return WseDispatch::handled_no_store;
            }
            std::string popped = std::move(arr->root->strings.back());
            if (!wse_store_string(strings, narrow[0],
                                  wse_utf8_to_utf16(popped))) {
                arr->root->strings.back() = std::move(popped);
                return WseDispatch::ignored;
            }
            arr->root->strings.pop_back();
            arr->dim_sizes[0] =
                static_cast<std::int32_t>(arr->root->strings.size());
            return WseDispatch::handled_no_store;
        }
        case WseArrayType::position:
            if (!wse_register_ok(narrow[0]) ||
                static_cast<std::size_t>(narrow[0]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            if (arr->root->positions.empty()) {
                return WseDispatch::handled_no_store;
            }
            operands.state.positions[static_cast<std::size_t>(narrow[0])] =
                arr->root->positions.back();
            arr->root->positions.pop_back();
            arr->dim_sizes[0] =
                static_cast<std::int32_t>(arr->root->positions.size());
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_resize_dim: {
        // 5011 (0x1393): (array_id, dim_index, size >= 1).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (arr) {
            (void)arr->resize_dim(narrow[1], narrow[2]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_get_dim_size: {
        // 5012 (0x1394): (dst, array_id, dim_index).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->dim_size(narrow[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_get_dim_count: {
        // 5013 (0x1395): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->dim_count());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_get_type_id: {
        // 5014 (0x1396): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->type_id());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_sort: {
        // 5015 (0x1397): (array_id, mode, [idx1..idx13]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (arr) {
            (void)wse_array_sort_builtin(*arr, narrow[1], narrow.subspan(2));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_sort_custom: {
        // 5016 (0x1398): (array_id, cmp_script, [idx1..idx13]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        const std::int32_t cmp_script = narrow[1];
        const auto subdims = narrow.subspan(2);
        if (!arr || !wse_array_validate_sort_subdims(*arr, subdims) ||
            cmp_script < 0 ||
            static_cast<std::size_t>(cmp_script) >=
                operands.state.num_scripts ||
            operands.state.execute_script_fn == nullptr ||
            operands.state.num_return_values_out == nullptr ||
            operands.state.return_values_out.empty()) {
            return WseDispatch::handled_no_store;
        }
        const bool banks_ok =
            (arr->type == WseArrayType::integer && registers.size() >= 2U) ||
            (arr->type == WseArrayType::string &&
             strings.registers.size() >= 2U) ||
            (arr->type == WseArrayType::position &&
             operands.state.positions.size() >= 2U);
        if (!banks_ok) {
            return WseDispatch::handled_no_store;
        }
        (void)wse_array_natural_merge_sort(
            *arr, [&](std::size_t row_a, std::size_t row_b) -> bool {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = arr->slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = arr->slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return false;
                }
                switch (arr->type) {
                case WseArrayType::integer:
                    registers[0] =
                        static_cast<std::int64_t>(sa->ints[leaf_a]);
                    registers[1] =
                        static_cast<std::int64_t>(sb->ints[leaf_b]);
                    break;
                case WseArrayType::string:
                    strings.registers[0] =
                        wse_utf8_to_utf16(sa->strings[leaf_a]);
                    strings.registers[1] =
                        wse_utf8_to_utf16(sb->strings[leaf_b]);
                    break;
                case WseArrayType::position:
                    operands.state.positions[0] = sa->positions[leaf_a];
                    operands.state.positions[1] = sb->positions[leaf_b];
                    break;
                }
                *operands.state.num_return_values_out = 0;
                const std::array<std::int64_t, 2> params = {
                    static_cast<std::int64_t>(row_a),
                    static_cast<std::int64_t>(row_b)};
                (void)operands.state.execute_script_fn(
                    operands.state.execute_script_user, cmp_script, params);
                return *operands.state.num_return_values_out == 1 &&
                       operands.state.return_values_out[0] != 0;
            });
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_eq:
        return wse_array_compare_op(operands, 0);
    case WseOpcode::array_neq:
        return wse_array_compare_op(operands, 1);
    case WseOpcode::array_gt:
        return wse_array_compare_op(operands, 2);
    case WseOpcode::array_ge:
        return wse_array_compare_op(operands, 3);
    case WseOpcode::array_lt:
        return wse_array_compare_op(operands, 4);
    case WseOpcode::array_le:
        return wse_array_compare_op(operands, 5);

    // Position vector operations (4100, 4102..4104 / 0x1004, 0x1006..0x1008).
    case WseOpcode::position_get_vector_to_position: {
        // 4100 (0x1004, 0x1401ecd5a): (dest_dist, pos_no, pos1, pos2).
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || !wse_register_ok(narrow[2]) ||
            !wse_register_ok(narrow[3]) || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto dst_idx = static_cast<std::size_t>(narrow[1]);
        const auto p1_idx = static_cast<std::size_t>(narrow[2]);
        const auto p2_idx = static_cast<std::size_t>(narrow[3]);
        if (dst_idx >= positions.size() || p1_idx >= positions.size() ||
            p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        const WseVec4 origin = positions[p1_idx].o;
        const WseVec4 diff = positions[p2_idx].o - origin;
        WseTransform out = WseTransform::identity();
        out.o = origin;
        out.rotate_z(-std::atan2(diff.x, diff.y));
        const float horiz = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        out.rotate_x(std::atan2(diff.z, horiz));
        positions[dst_idx] = out;
        effect.value = wse_round64(
            diff.length() *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::position_get_length: {
        // 4102 (0x1006, 0x1401ed0c1): (dest_len, pos_no) -> length(pos.o).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || positions.empty() ||
            static_cast<std::size_t>(narrow[1]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            positions[static_cast<std::size_t>(narrow[1])].o.length() *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_dot_product_of_positions: {
        // 4103 (0x1007, 0x1401ed10f): (dest, pos1, pos2) -> dot(p1.o, p2.o).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || !wse_register_ok(narrow[2]) ||
            positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto p1_idx = static_cast<std::size_t>(narrow[1]);
        const auto p2_idx = static_cast<std::size_t>(narrow[2]);
        if (p1_idx >= positions.size() || p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            positions[p1_idx].o.dot(positions[p2_idx].o) *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_cross_product_of_positions: {
        // 4104 (0x1008, 0x1401ed158): (dest_pos, pos1, pos2) -> cross -> dest.o.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto dst_idx = static_cast<std::size_t>(narrow[0]);
        const auto p1_idx = static_cast<std::size_t>(narrow[1]);
        const auto p2_idx = static_cast<std::size_t>(narrow[2]);
        if (dst_idx >= positions.size() || p1_idx >= positions.size() ||
            p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        positions[dst_idx].o = positions[p1_idx].o.cross(positions[p2_idx].o);
        return WseDispatch::handled_no_store;
    }

    default:
        break;
    }
    return WseDispatch::unimplemented;
}

}  // namespace wse::vm
