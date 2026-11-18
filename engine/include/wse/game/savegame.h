// Clean-room Phase 2A whole-savegame container framing, header, checksum control,
// and world-state serialization matching retail v1.1.5.5 binary `.sav` framing.
//
// Evidence (WSE2 v1.1.5.5 PE + PDB):
//   Container save flow & checksum verification:
//     mbGame::save                           0x1400e1820
//       writes magic 0x52445257 ("WRDR"), format version 0x496 (1174),
//       g_moduleVersion, mbSavegame header fields, then calls mbGame::writeStream,
//       closes rglOutputBinaryFileStreamWithChecksum, verifies the 64-bit byte-sum
//       checksum on read-back, backs up the previous save to last_savegame_backup.sav,
//       and renames new_game.sav into the target slot.
//     mbSavegame (168 B, type 0x51f8):
//       mbSavegame::readStream               0x1400c7510
//       mbGame::fillSavegameHeader           0x1400e1f90
//       mbLoadSaveWindow::updatePanels       0x140113d70 (checks at 0x1401140be..0x1401140fa:
//         magic == 0x52445257, savegame_version in [0x322..0x496] = [802..1174],
//         module_version >= g_compatibleSavegameModuleVersion)
//   World body & section codecs:
//     mbGame::writeStream / readStream       0x1400c9bf0 / 0x1400c8680
//     mbGame::saveGame / loadGame            0x1400c9eb0 / 0x1400c8a90
//     saveQuests / loadQuests                0x1400ca230 / 0x1400c8dd0
//     saveInfoPages / loadInfoPages          0x1400ca340 / 0x1400c8f50
//     saveSites / loadSites                  0x1400ca3d0 / 0x1400c8fe0
//     saveFactions / loadFactions            0x1400ca460 / 0x1400c90c0
//     saveTracks / loadTracks                0x1400ca580 / 0x1400c9240
//     savePartyTemplates / loadPartyTemplates 0x1400ca6b0 / 0x1400c9300
//     saveParties / loadParties              0x1400ca750 / 0x1400c9420
//     saveTroops / loadTroops                0x1400ca8e0 / 0x1400c97b0
//     loadItemKinds                          0x1400c9b10
//     mbSlots::writeStream                   0x140218490
//     mbNotes::writeStream / readStream      0x1401ba0b0 / 0x1401ba010
//     rglHashVector<mbParty>::write/read     0x1400e7f40 / 0x1400e7ce0
//     mbParty::writeStream / readStream      0x1401bad80 / 0x1401ba760
//     rglStableVector<mbEvent>::write/read   0x1400e7a80 / 0x1400e77a0
//     mbEvent::writeStream / readStream      0x1400bb0f0 / 0x1400bafd0
//     mbTroop::setItem (mbItem packing)      0x140246920
//
// Safety deviations:
//   - Retail readers perform no bounds checks on file counts or string lengths;
//     portable codecs validate counts against remaining payload bytes and cap
//     decoded container arrays against `kMaxSavegameArrayCount` to prevent OOM.
//   - Retail count mismatches trigger `rglError` popups and discard/keep surplus
//     or missing records; portable codecs return explicit `SavegameContainerResult`
//     status codes (plus the `count_exceeded` warning bit when surplus entries
//     were safely consumed and skipped, or a party record named an out-of-range key).
//   - Loads are transactional: `read_savegame_container` parses into staged copies of
//     the world, global variables, and trigger managers and commits them only when the
//     whole body parsed, so a corrupt or truncated file leaves the live game untouched.
//   - `rglHashVector` / `rglStableVector` record-present flags other than 0 or 1 are
//     rejected as `corrupt_record` instead of being read as empty (which desynchronizes
//     every following field).
//   - Version-gated savegame bodies `[802..1174]` match all four `g_compatibleSavegameVersion`
//     branches in retail v1155 `.text`: `mbGame::readStream` (`0x1400c86cd`, `> 1136` combat options),
//     `mbParty::readStream` (`0x1401bab51`, `(900..999) || >= 1020` `is_marshall`; `0x1401baba0`,
//     `> 1136` `extra_icon_*`; `0x1401babff`, `> 1160` `ignore_with_player_party`). `mbNotes::readStream`
//     (`0x1401ba010`), `loadQuests`..`loadItemKinds`, `mbEvent::readStream`, and trigger streams are
//     version-invariant across `[802..1174]`.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/core/rgl_string.h"
#include "wse/game/trigger.h"
#include "wse/game/world_state.h"
#include "wse/io/binary_stream.h"
#include "wse/platform/vfs.h"
#include "wse/vm/slots.h"

namespace wse::game {

inline constexpr std::uint32_t kSavegameMagic = 0x52445257U;  // "WRDR" LE
inline constexpr std::int32_t kSavegameMinVersion = 802;      // 0x322
inline constexpr std::int32_t kSavegameFormatVersion = 1174;  // 0x496
// Bodies written by versions strictly above 0x470 (1136) carry the five combat-option
// int32 fields after the scalar preamble (`mbGame::readStream` 0x1400c8680).
// Version gates matching retail savegame stream readers (v1155 .text):
// - mbParty::readStream (`0x1401bab51`): `(900..999) || >= 1020` -> `is_marshall` (`+0x17c`).
// - mbParty::readStream (`0x1401baba0`) & mbGame::readStream (`0x1400c86cd`): `> 1136` (`0x470`)
//   -> `extra_icon_no` + 4 `extra_icon_*` floats, and 5 combat options.
// - mbParty::readStream (`0x1401babff`): `> 1160` (`0x488`) -> `ignore_with_player_party` (`+0x124`).
// Across `[802..1174]`, all other world/container/event/trigger readers are version-invariant.
inline constexpr std::int32_t kSavegamePartyMarshallRangeMin = 900;
inline constexpr std::int32_t kSavegamePartyMarshallRangeMax = 999;
inline constexpr std::int32_t kSavegamePartyMarshallAfterOrEq = 1020;
inline constexpr std::int32_t kSavegameCombatOptionsAfterVersion = 0x470;       // 1136
inline constexpr std::int32_t kSavegamePartyExtraIconAfterVersion = 0x470;      // 1136
inline constexpr std::int32_t kSavegamePartyIgnorePlayerAfterVersion = 0x488;   // 1160

[[nodiscard]] constexpr bool savegame_has_combat_options(std::int32_t v) noexcept {
    return v > kSavegameCombatOptionsAfterVersion;
}
[[nodiscard]] constexpr bool savegame_has_party_marshall(std::int32_t v) noexcept {
    return (v >= kSavegamePartyMarshallRangeMin && v <= kSavegamePartyMarshallRangeMax) ||
           v >= kSavegamePartyMarshallAfterOrEq;
}
[[nodiscard]] constexpr bool savegame_has_party_extra_icon(std::int32_t v) noexcept {
    return v > kSavegamePartyExtraIconAfterVersion;
}
[[nodiscard]] constexpr bool savegame_has_party_ignore_player(std::int32_t v) noexcept {
    return v > kSavegamePartyIgnorePlayerAfterVersion;
}
inline constexpr std::size_t kMaxSavegameArrayCount = (1U << 20U);

enum class SavegameContainerResult : std::uint8_t {
    ok = 0,
    invalid_magic,
    unsupported_version,
    incompatible_module_version,
    truncated,
    negative_count,
    count_limit_exceeded,
    io_error,
    checksum_mismatch,
    corrupt_record,
    // No longer produced: every header version in [802..1174] has a reconstructed body
    // layout. Kept so the enum values and `describe` stay stable for callers.
    legacy_layout_unsupported,
};

[[nodiscard]] constexpr std::string_view describe(SavegameContainerResult result) noexcept {
    switch (result) {
    case SavegameContainerResult::ok:
        return "ok";
    case SavegameContainerResult::invalid_magic:
        return "invalid savegame magic (expected WRDR)";
    case SavegameContainerResult::unsupported_version:
        return "unsupported savegame format version";
    case SavegameContainerResult::incompatible_module_version:
        return "savegame module version is older than compatible version";
    case SavegameContainerResult::truncated:
        return "truncated binary savegame container";
    case SavegameContainerResult::negative_count:
        return "negative section record count in savegame";
    case SavegameContainerResult::count_limit_exceeded:
        return "savegame section record count exceeds safety limit";
    case SavegameContainerResult::io_error:
        return "savegame VFS I/O failure";
    case SavegameContainerResult::checksum_mismatch:
        return "savegame byte-sum checksum verification failed";
    case SavegameContainerResult::corrupt_record:
        return "savegame record-present flag is neither 0 nor 1";
    case SavegameContainerResult::legacy_layout_unsupported:
        return "savegame body layout older than version 1174 is not reconstructed yet";
    }
    return "unknown savegame container result";
}

// Reconstructed `mbSavegame` (168 B, type 0x51f8).
struct SavegameHeader {
    std::uint32_t magic = kSavegameMagic;
    std::int32_t savegame_version = kSavegameFormatVersion;
    std::int32_t module_version = 0;
    std::string name{};
    std::string player_name{};
    std::int32_t player_level = 1;
    float game_date_hours = 0.0f;
};

struct SavegameWriteResult {
    bool ok = false;
    SavegameContainerResult status = SavegameContainerResult::io_error;
    std::uint64_t checksum = 0U;
    std::size_t bytes_written = 0U;
};

struct SavegameLoadReport {
    SavegameContainerResult status = SavegameContainerResult::truncated;
    SavegameHeader header{};
    bool count_exceeded = false;  // e.g. file trigger/quest/faction count > loaded module
    std::size_t bytes_consumed = 0U;
    std::uint64_t computed_checksum = 0U;

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return status == SavegameContainerResult::ok;
    }
};

struct SavegameRuntimeContext {
    WseWorldState* world = nullptr;
    wse::vm::Slots* global_variables = nullptr;
    TriggerManager* triggers = nullptr;
    SimpleTriggerManager* simple_triggers = nullptr;
    std::span<const std::int64_t> timers_total{};
    bool dont_load_regular_troop_inventories = false;  // rglConfig::Campaign::bDontLoadRegularTroopInventories
};

struct SavegameConstContext {
    const WseWorldState* world = nullptr;
    const wse::vm::Slots* global_variables = nullptr;
    const TriggerManager* triggers = nullptr;
    const SimpleTriggerManager* simple_triggers = nullptr;
    std::span<const std::int64_t> timers_total{};
    bool dont_load_regular_troop_inventories = false;
};

// Mirrors mbGame::fillSavegameHeader (0x1400e1f90) + override check in mbGame::save (0x1400e1992).
[[nodiscard]] inline SavegameHeader fill_savegame_header(
    const WseWorldState& world,
    std::span<const std::int64_t> timers_total,
    std::string_view override_name = {}) {
    SavegameHeader header{};
    header.magic = kSavegameMagic;
    header.savegame_version = kSavegameFormatVersion;
    header.module_version = world.module_version;

    const std::int32_t player_troop = world.player_troop_no;
    if (world.is_valid_troop(player_troop)) {
        const WseTroop& tr = world.troops[static_cast<std::size_t>(player_troop)];
        header.player_name = tr.name;
        header.player_level = tr.level;
    }

    if (!override_name.empty()) {
        header.name.assign(override_name);
    } else if (!world.savegame_name.empty()) {
        header.name = world.savegame_name;
    } else {
        header.name = header.player_name;
    }

    std::int64_t elapsed_ticks = -world.date_timer_value;
    if (world.date_timer_no >= 0 &&
        static_cast<std::size_t>(world.date_timer_no) < timers_total.size()) {
        elapsed_ticks =
            timers_total[static_cast<std::size_t>(world.date_timer_no)] - world.date_timer_value;
    }
    // Divide in double: int64 -> float first drops tick precision after ~2^24 ticks.
    header.game_date_hours =
        static_cast<float>(static_cast<double>(elapsed_ticks) / 100000.0);
    return header;
}

inline void write_savegame_header(io::BinaryWriter& w, const SavegameHeader& header) {
    w.write_u32(header.magic);
    w.write_i32(header.savegame_version);
    w.write_i32(header.module_version);
    w.write_string(header.name);
    w.write_string(header.player_name);
    w.write_i32(header.player_level);
    w.write_f32(header.game_date_hours);
}

[[nodiscard]] inline SavegameContainerResult validate_savegame_header(
    const SavegameHeader& header,
    std::int32_t compatible_module_version) noexcept {
    if (header.magic != kSavegameMagic) {
        return SavegameContainerResult::invalid_magic;
    }
    if (header.savegame_version < kSavegameMinVersion ||
        header.savegame_version > kSavegameFormatVersion) {
        return SavegameContainerResult::unsupported_version;
    }
    if (header.module_version < compatible_module_version) {
        return SavegameContainerResult::incompatible_module_version;
    }
    return SavegameContainerResult::ok;
}

// Mirrors mbSavegame::readStream (0x1400c7510) + slot compatibility check (0x1401140d0..0x1401140fa).
[[nodiscard]] inline SavegameContainerResult read_savegame_header(
    io::BinaryReader& r,
    SavegameHeader& out_header,
    std::int32_t compatible_module_version = std::numeric_limits<std::int32_t>::min()) {
    SavegameHeader parsed{};
    if (!r.read_u32(parsed.magic) || !r.read_i32(parsed.savegame_version) ||
        !r.read_i32(parsed.module_version) || !r.read_string(parsed.name) ||
        !r.read_string(parsed.player_name) || !r.read_i32(parsed.player_level) ||
        !r.read_f32(parsed.game_date_hours)) {
        return SavegameContainerResult::truncated;
    }
    const SavegameContainerResult validity =
        validate_savegame_header(parsed, compatible_module_version);
    out_header = std::move(parsed);
    return validity;
}

namespace savegame_detail {

[[nodiscard]] inline SavegameContainerResult read_checked_count(
    io::BinaryReader& r, std::int32_t& out_count, std::size_t min_bytes_per_entry = 0) noexcept {
    if (!r.read_i32(out_count)) {
        return SavegameContainerResult::truncated;
    }
    if (out_count < 0) {
        return SavegameContainerResult::negative_count;
    }
    const auto count = static_cast<std::size_t>(out_count);
    if (count > kMaxSavegameArrayCount) {
        return SavegameContainerResult::count_limit_exceeded;
    }
    if (min_bytes_per_entry > 0U && count > (r.remaining() / min_bytes_per_entry)) {
        return SavegameContainerResult::truncated;
    }
    return SavegameContainerResult::ok;
}

inline void write_slots(io::BinaryWriter& w, const wse::vm::Slots& slots) {
    const std::span<const std::int64_t> vals = slots.values();
    w.write_i32(static_cast<std::int32_t>(vals.size()));
    for (const std::int64_t v : vals) {
        w.write_i64(v);
    }
}

[[nodiscard]] inline SavegameContainerResult read_slots(io::BinaryReader& r,
                                                        wse::vm::Slots* out_slots) {
    std::int32_t count = 0;
    const SavegameContainerResult rc = read_checked_count(r, count, 8U);
    if (rc != SavegameContainerResult::ok) {
        return rc;
    }
    if (out_slots == nullptr) {
        // Skip without allocating; read_checked_count already proved the bytes exist.
        for (std::int32_t i = 0; i < count; ++i) {
            std::int64_t ignored = 0;
            if (!r.read_i64(ignored)) {
                return SavegameContainerResult::truncated;
            }
        }
        return SavegameContainerResult::ok;
    }
    std::vector<std::int64_t> tmp(static_cast<std::size_t>(count), 0);
    for (std::int32_t i = 0; i < count; ++i) {
        if (!r.read_i64(tmp[static_cast<std::size_t>(i)])) {
            return SavegameContainerResult::truncated;
        }
    }
    out_slots->assign(std::move(tmp));
    return SavegameContainerResult::ok;
}

// Retail `mbNotes::writeStream` (`0x1401ba0b0`) / `mbNotes::readStream` (`0x1401ba010`) loop
// 16 times over `m_texts[i]` (`+0x40`, stride `0x48`) and `m_dates[i]` (`+0`, stride `4`),
// reading/writing the single scalar `m_tableauMaterialNo` (`+0x4c0`) and `m_available`
// (`+0x4c4`) on each iteration.
template <typename NoteT>
inline void write_notes(io::BinaryWriter& w,
                        const std::array<NoteT, kNumQuestNotes>& notes,
                        std::int32_t tableau_material_no,
                        bool available) {
    for (std::size_t i = 0; i < kNumQuestNotes; ++i) {
        w.write_string(wse::core::wse_utf16_to_utf8(notes[i].text));
        w.write_i32(notes[i].date);
        w.write_i32(tableau_material_no);
        w.write_bool(available);
    }
}

template <typename NoteT>
[[nodiscard]] inline SavegameContainerResult read_notes(
    io::BinaryReader& r,
    std::array<NoteT, kNumQuestNotes>* notes,
    std::int32_t* out_tableau,
    bool* out_available) {
    for (std::size_t i = 0; i < kNumQuestNotes; ++i) {
        std::string txt;
        std::int32_t date = -1;
        std::int32_t tableau = -1;
        bool avail = false;
        if (!r.read_string(txt) || !r.read_i32(date) || !r.read_i32(tableau) ||
            !r.read_bool(avail)) {
            return SavegameContainerResult::truncated;
        }
        if (notes != nullptr) {
            (*notes)[i].text = wse::core::wse_utf8_to_utf16(txt);
            (*notes)[i].date = date;
        }
        if (out_tableau != nullptr) {
            *out_tableau = tableau;
        }
        if (out_available != nullptr) {
            *out_available = avail;
        }
    }
    return SavegameContainerResult::ok;
}

// Retail `mbItem` keeps one item amount (`troop_inventory_slot_{get,set}_item_amount`
// 0x1401d0ce4 / 0x1401d0a43, i.e. `WseInventorySlot::ammo`) next to the modifier. The
// portable `hit_points` field has no retail counterpart in the slot, so it is never
// written; folding it into the amount turned shield hit points into ammo on reload.
[[nodiscard]] inline std::uint32_t pack_inventory_flags(const WseInventorySlot& slot) noexcept {
    const std::uint32_t mod_bits =
        (static_cast<std::uint32_t>(std::max<std::int32_t>(0, slot.modifier)) & 0xFFU) << 24U;
    const std::uint32_t low_bits =
        static_cast<std::uint32_t>(std::max<std::int32_t>(0, slot.ammo)) & 0xFFFFU;
    return mod_bits | low_bits;
}

// `hit_points` is not persisted (no retail slot field); it reloads as 0, matching
// `WseTroop::set_item` (`mbTroop::setItem` 0x140246920).
inline void unpack_inventory_flags(std::int32_t item_no,
                                   std::uint32_t packed,
                                   WseInventorySlot& out) noexcept {
    out.item_no = item_no;
    out.modifier = static_cast<std::int32_t>((packed >> 24U) & 0xFFU);
    out.ammo = static_cast<std::int32_t>(packed & 0xFFFFU);
    out.hit_points = 0;
}

// Party icons live in the low byte of `mbParty::m_flags` (the module loader derives
// `icon_no` from `flags & 0xff`); `party_set_icon` updates only `icon_no`, so fold it back.
[[nodiscard]] inline std::uint64_t party_flags_with_icon(const WseParty& p) noexcept {
    return (p.flags & ~std::uint64_t{0xFFU}) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.icon_no)) & 0xFFU);
}

// Float -> int64 conversion that is defined for NaN, infinities, and out-of-range values.
[[nodiscard]] inline std::int64_t saturating_float_to_i64(float value) noexcept {
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0;
    }
    // 2^63 is exactly representable as float; anything at or above it saturates.
    if (value >= 9223372036854775808.0f) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] inline SavegameContainerResult map_trigger_result(SavegameResult r) noexcept {
    switch (r) {
    case SavegameResult::ok:
    case SavegameResult::count_exceeded:
        return SavegameContainerResult::ok;
    case SavegameResult::negative_count:
        return SavegameContainerResult::negative_count;
    case SavegameResult::truncated:
        return SavegameContainerResult::truncated;
    }
    return SavegameContainerResult::truncated;
}

inline void write_save_game_section(io::BinaryWriter& w, const WseWorldState& world) {
    // mbGame::saveGame (0x1400c9eb0)
    w.write_i64(world.date_timer_value);
    w.write_i32(world.current_hours);
    w.write_i32(world.game_day);
    w.write_i32(world.game_week);
    w.write_i32(world.game_month);
    w.write_i32(world.game_year);
    w.write_i32(0);  // unused legacy field
    w.write_f32(world.cloud_amount);
    w.write_f32(world.haze_amount);
    w.write_f32(world.misc.average_game_difficulty);
    w.write_f32(world.misc.difficulty_time_accum);
    w.write_string({});
    w.write_bool(false);
    w.write_u32(world.tutorial_flags);
    w.write_i32(world.in_meta_mission ? 1 : 0);
    w.write_i32(world.encountered_party_no);
    w.write_i32(world.encountered_party_2_no);
    w.write_i32(world.party_creation_max_random);
    w.write_i32(world.jump_menu_no);
    w.write_i32(world.screen_troop_no);
    w.write_i32(world.current_site_no);
    w.write_i32(world.misc.rest_interactive ? 1 : 0);
    w.write_i32(world.misc.rest_time_speed);
    w.write_string(wse::core::RglString::encode_savegame_string(world.game_log));
    for (int i = 0; i < 6; ++i) {
        w.write_i32(0);
    }
    w.write_i64(world.campaign_time);
    w.write_f32(world.rest_period);
    w.write_i32(world.rest_no_attack);
    w.write_i32(world.misc.rest_interactive ? 1 : 0);
    w.write_i32(world.misc.rest_time_speed);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kNumDivisions); ++i) {
        w.write_string(world.class_names[i]);
    }
}

[[nodiscard]] inline SavegameContainerResult read_save_game_section(io::BinaryReader& r,
                                                                    WseWorldState& world) {
    std::int32_t unused_i = 0;
    std::string unused_s;
    bool unused_b = false;
    std::int32_t meta_mission_i = 0;
    std::int32_t rest_inter_1 = 0;
    std::int32_t rest_speed_1 = 0;
    std::int32_t rest_inter_2 = 0;
    std::int32_t rest_speed_2 = 0;
    std::string encoded_log;

    if (!r.read_i64(world.date_timer_value) || !r.read_i32(world.current_hours) ||
        !r.read_i32(world.game_day) || !r.read_i32(world.game_week) ||
        !r.read_i32(world.game_month) || !r.read_i32(world.game_year) ||
        !r.read_i32(unused_i) || !r.read_f32(world.cloud_amount) ||
        !r.read_f32(world.haze_amount) || !r.read_f32(world.misc.average_game_difficulty) ||
        !r.read_f32(world.misc.difficulty_time_accum) || !r.read_string(unused_s) ||
        !r.read_bool(unused_b) || !r.read_u32(world.tutorial_flags) ||
        !r.read_i32(meta_mission_i) || !r.read_i32(world.encountered_party_no) ||
        !r.read_i32(world.encountered_party_2_no) || !r.read_i32(world.party_creation_max_random) ||
        !r.read_i32(world.jump_menu_no) || !r.read_i32(world.screen_troop_no) ||
        !r.read_i32(world.current_site_no) || !r.read_i32(rest_inter_1) ||
        !r.read_i32(rest_speed_1) || !r.read_string(encoded_log)) {
        return SavegameContainerResult::truncated;
    }
    world.in_meta_mission = meta_mission_i != 0;
    world.game_log = wse::core::RglString::decode_savegame_string(encoded_log);

    for (int i = 0; i < 6; ++i) {
        if (!r.read_i32(unused_i)) {
            return SavegameContainerResult::truncated;
        }
    }
    if (!r.read_i64(world.campaign_time) || !r.read_f32(world.rest_period) ||
        !r.read_i32(world.rest_no_attack) || !r.read_i32(rest_inter_2) ||
        !r.read_i32(rest_speed_2)) {
        return SavegameContainerResult::truncated;
    }
    world.misc.rest_interactive = rest_inter_2 != 0;
    world.misc.rest_time_speed = rest_speed_2;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kNumDivisions); ++i) {
        if (!r.read_string(world.class_names[i])) {
            return SavegameContainerResult::truncated;
        }
    }
    return SavegameContainerResult::ok;
}

inline void write_party_record(io::BinaryWriter& w,
                               const WseParty& p,
                               std::int32_t savegame_version = kSavegameFormatVersion) {
    // mbParty::writeStream (0x1401bad80)
    w.write_string(p.id);
    w.write_string(p.name);
    w.write_u64(party_flags_with_icon(p));
    w.write_i32(p.menu_no);
    w.write_i32(p.template_no);
    w.write_i32(p.faction_no);
    w.write_i32(static_cast<std::int32_t>(p.personality));
    w.write_i32(p.default_behavior);
    w.write_i32(p.behavior);
    w.write_i32(p.default_behavior_object);
    w.write_i32(p.behavior_object);
    w.write_f32(p.default_behavior_position.x);
    w.write_f32(p.default_behavior_position.y);
    w.write_f32(p.behavior_position.x);
    w.write_f32(p.behavior_position.y);
    w.write_f32(p.position.o.x);
    w.write_f32(p.position.o.y);
    w.write_f32(p.position.o.z);

    w.write_i32(static_cast<std::int32_t>(p.stacks.size()));
    for (const WsePartyStack& st : p.stacks) {
        w.write_i32(st.troop_no);
        w.write_i32(st.num_troops);
        w.write_i32(st.num_wounded_troops);
        w.write_u32(st.flags);
    }

    w.write_f32(p.bearing);
    w.write_bool(p.renamed);
    w.write_string(p.extra_text);
    w.write_f32(p.morale);
    w.write_f32(0.0f);  // spare zero fields at 0x1401baf6d..0x1401baf7c
    w.write_f32(0.0f);
    w.write_f32(p.patrol_radius);
    w.write_f32(p.ai_initiative);
    w.write_f32(p.helpfulness);
    w.write_i32(p.ai_state);
    w.write_f32(static_cast<float>(p.bandit_attraction) / 100.0f);
    if (savegame_has_party_marshall(savegame_version)) {
        w.write_i32(p.is_marshall ? 1 : 0);
    }
    w.write_i64(saturating_float_to_i64(p.ignore_player_timer));
    w.write_i32(p.banner_icon_no);
    if (savegame_has_party_extra_icon(savegame_version)) {
        w.write_i32(p.extra_icon_no);
        w.write_f32(p.extra_icon_up_down_distance);
        w.write_f32(p.extra_icon_up_down_frequency);
        w.write_f32(p.extra_icon_rotate_frequency);
        w.write_f32(p.extra_icon_fade_distance);
    }
    if (savegame_has_party_ignore_player(savegame_version)) {
        w.write_i32(p.ignore_with_player_party ? 1 : 0);
    }
    w.write_i32(p.attached_to_party_no);
    w.write_bool(p.attached_to_party_no >= 0);

    w.write_i32(static_cast<std::int32_t>(p.attached_party_nos.size()));
    for (const std::int32_t child : p.attached_party_nos) {
        w.write_i32(child);
    }
    w.write_i32(static_cast<std::int32_t>(p.particle_system_nos.size()));
    for (const std::int32_t ps : p.particle_system_nos) {
        w.write_i32(ps);
    }
    write_notes(w, p.notes, p.note_tableau_material_no, p.note_available);
    write_slots(w, p.slots);
}

[[nodiscard]] inline SavegameContainerResult read_party_record(io::BinaryReader& r,
                                                               std::int32_t savegame_version,
                                                               WseParty* out_party) {
    WseParty p{};
    std::int32_t pers = 0;
    std::int32_t stack_count = 0;
    std::int32_t is_marshall_i = 0;
    std::int64_t ignore_timer_i = 0;
    std::int32_t ignore_player_i = 0;
    bool attached_b = false;
    float dummy_f0 = 0.0f;
    float dummy_f1 = 0.0f;
    float bandit_attraction_f = 0.0f;

    if (!r.read_string(p.id) || !r.read_string(p.name) || !r.read_u64(p.flags) ||
        !r.read_i32(p.menu_no) || !r.read_i32(p.template_no) || !r.read_i32(p.faction_no) ||
        !r.read_i32(pers) || !r.read_i32(p.default_behavior) || !r.read_i32(p.behavior) ||
        !r.read_i32(p.default_behavior_object) || !r.read_i32(p.behavior_object) ||
        !r.read_f32(p.default_behavior_position.x) ||
        !r.read_f32(p.default_behavior_position.y) || !r.read_f32(p.behavior_position.x) ||
        !r.read_f32(p.behavior_position.y) || !r.read_f32(p.position.o.x) ||
        !r.read_f32(p.position.o.y) || !r.read_f32(p.position.o.z)) {
        return SavegameContainerResult::truncated;
    }
    p.personality = static_cast<std::uint32_t>(pers);
    p.icon_no = static_cast<std::int32_t>(p.flags & 0xFFULL);

    const SavegameContainerResult sc_rc = read_checked_count(r, stack_count, 16U);
    if (sc_rc != SavegameContainerResult::ok) {
        return sc_rc;
    }
    p.stacks.resize(static_cast<std::size_t>(stack_count));
    for (std::int32_t s = 0; s < stack_count; ++s) {
        WsePartyStack& st = p.stacks[static_cast<std::size_t>(s)];
        if (!r.read_i32(st.troop_no) || !r.read_i32(st.num_troops) ||
            !r.read_i32(st.num_wounded_troops) || !r.read_u32(st.flags)) {
            return SavegameContainerResult::truncated;
        }
    }

    if (!r.read_f32(p.bearing) || !r.read_bool(p.renamed) || !r.read_string(p.extra_text) ||
        !r.read_f32(p.morale) || !r.read_f32(dummy_f0) || !r.read_f32(dummy_f1) ||
        !r.read_f32(p.patrol_radius) || !r.read_f32(p.ai_initiative) ||
        !r.read_f32(p.helpfulness) || !r.read_i32(p.ai_state) ||
        !r.read_f32(bandit_attraction_f)) {
        return SavegameContainerResult::truncated;
    }
    is_marshall_i = 0;
    if (savegame_has_party_marshall(savegame_version)) {
        if (!r.read_i32(is_marshall_i)) {
            return SavegameContainerResult::truncated;
        }
    }
    if (!r.read_i64(ignore_timer_i) || !r.read_i32(p.banner_icon_no)) {
        return SavegameContainerResult::truncated;
    }
    p.extra_icon_no = -1;
    p.extra_icon_up_down_distance = 0.0f;
    p.extra_icon_up_down_frequency = 0.0f;
    p.extra_icon_rotate_frequency = 0.0f;
    p.extra_icon_fade_distance = 0.0f;
    if (savegame_has_party_extra_icon(savegame_version)) {
        if (!r.read_i32(p.extra_icon_no) || !r.read_f32(p.extra_icon_up_down_distance) ||
            !r.read_f32(p.extra_icon_up_down_frequency) ||
            !r.read_f32(p.extra_icon_rotate_frequency) || !r.read_f32(p.extra_icon_fade_distance)) {
            return SavegameContainerResult::truncated;
        }
    }
    ignore_player_i = 0;
    if (savegame_has_party_ignore_player(savegame_version)) {
        if (!r.read_i32(ignore_player_i)) {
            return SavegameContainerResult::truncated;
        }
    }
    if (!r.read_i32(p.attached_to_party_no) ||
        !r.read_bool(attached_b)) {
        return SavegameContainerResult::truncated;
    }
    p.bandit_attraction =
        std::isfinite(bandit_attraction_f)
            ? static_cast<std::int32_t>(std::lround(
                  std::clamp(bandit_attraction_f * 100.0f, -2147483520.0f, 2147483520.0f)))
            : 0;
    p.is_marshall = is_marshall_i != 0;
    p.ignore_player_timer = static_cast<float>(ignore_timer_i);
    p.ignore_with_player_party = ignore_player_i != 0;
    (void)attached_b;

    std::int32_t att_count = 0;
    SavegameContainerResult rc = read_checked_count(r, att_count, 4U);
    if (rc != SavegameContainerResult::ok) {
        return rc;
    }
    p.attached_party_nos.resize(static_cast<std::size_t>(att_count));
    for (std::int32_t i = 0; i < att_count; ++i) {
        if (!r.read_i32(p.attached_party_nos[static_cast<std::size_t>(i)])) {
            return SavegameContainerResult::truncated;
        }
    }

    std::int32_t ps_count = 0;
    rc = read_checked_count(r, ps_count, 4U);
    if (rc != SavegameContainerResult::ok) {
        return rc;
    }
    p.particle_system_nos.resize(static_cast<std::size_t>(ps_count));
    for (std::int32_t i = 0; i < ps_count; ++i) {
        if (!r.read_i32(p.particle_system_nos[static_cast<std::size_t>(i)])) {
            return SavegameContainerResult::truncated;
        }
    }

    rc = read_notes(r, &p.notes, &p.note_tableau_material_no, &p.note_available);
    if (rc != SavegameContainerResult::ok) {
        return rc;
    }
    rc = read_slots(r, &p.slots);
    if (rc != SavegameContainerResult::ok) {
        return rc;
    }
    p.active = true;
    if (out_party != nullptr) {
        *out_party = std::move(p);
    }
    return SavegameContainerResult::ok;
}

inline void write_world_body_sections(
    io::BinaryWriter& w,
    const SavegameConstContext& ctx,
    std::int32_t savegame_version = kSavegameFormatVersion) {
    const WseWorldState& world = *ctx.world;
    const wse::vm::Slots empty_slots{};
    const TriggerManager empty_triggers{};
    const SimpleTriggerManager empty_simple{};

    // mbGame::writeStream scalar preamble (0x1400c9bf0)
    w.write_i64(world.campaign_time);
    w.write_i32(world.seed);
    w.write_u32(world.encounter_flags);
    if (savegame_has_combat_options(savegame_version)) {
        w.write_i32(world.combat_damage_to_self);
        w.write_i32(world.combat_damage_to_friends);
        w.write_i32(world.reduce_battle_ai);
        w.write_i32(world.reduce_campaign_ai);
        w.write_i32(world.combat_speed);
    }

    write_save_game_section(w, world);
    write_slots(w, ctx.global_variables != nullptr ? *ctx.global_variables : empty_slots);
    write_trigger_savegame(ctx.triggers != nullptr ? *ctx.triggers : empty_triggers, w);
    write_simple_trigger_savegame(
        ctx.simple_triggers != nullptr ? *ctx.simple_triggers : empty_simple, w);

    // saveQuests (0x1400ca230)
    w.write_i32(static_cast<std::int32_t>(world.quests.size()));
    for (std::size_t qi = 0; qi < world.quests.size(); ++qi) {
        const WseQuest& q = world.quests[qi];
        std::string text = q.text;
        std::string giver = q.giver_name;
        if (text.empty() || giver.empty()) {
            const auto it = world.misc.quest_strings.find(static_cast<std::int32_t>(qi));
            if (it != world.misc.quest_strings.end()) {
                if (text.empty()) text = it->second.text;
                if (giver.empty()) giver = it->second.giver_name;
            }
        }
        w.write_u32(q.progression_flags);
        w.write_i32(q.giver_troop_no);
        w.write_i32(q.number);
        w.write_f32(q.start_date);
        w.write_string(q.title);
        w.write_string(text);
        w.write_string(giver);
        write_notes(w, q.notes, q.note_tableau_material_no, q.note_available);
        write_slots(w, q.slots);
    }

    // saveInfoPages (0x1400ca340)
    w.write_i32(static_cast<std::int32_t>(world.info_pages.size()));
    for (const WseInfoPage& ip : world.info_pages) {
        write_notes(w, ip.notes, ip.note_tableau_material_no, ip.note_available);
    }

    // saveSites (0x1400ca3d0)
    w.write_i32(static_cast<std::int32_t>(world.sites.size()));
    for (const WseSite& site : world.sites) {
        write_slots(w, site.slots);
    }

    // saveFactions (0x1400ca460)
    const std::int32_t num_factions = static_cast<std::int32_t>(world.factions.size());
    w.write_i32(num_factions);
    for (const WseFaction& fac : world.factions) {
        write_slots(w, fac.slots);
        for (std::int32_t j = 0; j < num_factions; ++j) {
            w.write_f32(fac.get_relation(static_cast<std::size_t>(j)));
        }
        w.write_string(fac.name);
        w.write_bool(fac.renamed);
        w.write_u32(fac.color);
        w.write_i32(0);
        write_notes(w, fac.notes, fac.note_tableau_material_no, fac.note_available);
    }

    // saveTracks (0x1400ca580)
    w.write_i32(static_cast<std::int32_t>(world.tracks.size()));
    for (const WseSavegameTrack& trk : world.tracks) {
        w.write_f32(trk.position.x);
        w.write_f32(trk.position.y);
        w.write_f32(trk.position.z);
        w.write_f32(trk.rotation_z);
        w.write_f32(trk.age);
        w.write_u32(trk.flags);
    }

    // savePartyTemplates (0x1400ca6b0)
    w.write_i32(static_cast<std::int32_t>(world.party_templates.size()));
    for (const WsePartyTemplate& pt : world.party_templates) {
        w.write_i32(pt.num_created);
        w.write_i32(pt.num_destroyed);
        w.write_i32(pt.num_destroyed_by_player);
        write_slots(w, pt.slots);
    }

    // saveParties (0x1400ca750 + rglHashVector 0x1400e7f40: m_numSlots +0x138bc, m_nextKey +0x138f0)
    const std::int32_t num_parties = static_cast<std::int32_t>(world.parties.size());
    const std::int32_t next_party_key = std::max(num_parties, world.next_party_key);
    w.write_i32(num_parties);
    w.write_i32(next_party_key);
    for (std::int32_t i = 0; i < num_parties; ++i) {
        const WseParty& p = world.parties[static_cast<std::size_t>(i)];
        if (p.active) {
            w.write_i32(1);
            w.write_i32(i);
            w.write_i32(i);
            write_party_record(w, p, savegame_version);
        } else {
            w.write_i32(0);
        }
    }
    if (world.is_valid_party(world.main_party_no)) {
        const WseParty& main_party = world.parties[static_cast<std::size_t>(world.main_party_no)];
        for (std::size_t s = 0; s < main_party.stacks.size(); ++s) {
            const WsePartyStack& st = main_party.stacks[s];
            const bool hero = world.is_valid_troop(st.troop_no) &&
                              world.troops[static_cast<std::size_t>(st.troop_no)].is_hero();
            if (!hero) {
                w.write_f32(st.xp);
                w.write_i32(st.num_upgradeable);
                if (s < 32U) {
                    for (std::size_t k = 0; k < 32U; ++k) {
                        w.write_i32(world.main_party_dnas[s * 32U + k]);
                    }
                }
            }
        }
    }

    // m_events (rglStableVector<mbEvent> 0x1400e7a80 + mbEvent 0x1400bb0f0)
    const std::int32_t num_events = static_cast<std::int32_t>(world.events.size());
    w.write_i32(num_events);
    w.write_i32(0);
    for (const WseSavegameEvent& ev : world.events) {
        if (ev.active != 0) {
            w.write_i32(1);
            w.write_i32(0);
            w.write_string(ev.extra);
            w.write_i32(ev.active);
            w.write_f32(ev.position.x);
            w.write_f32(ev.position.y);
            w.write_f32(ev.position.z);
            w.write_f32(ev.position.w);
            w.write_f32(0.0f);
            w.write_f32(0.0f);
            w.write_i32(ev.party_1_no);
            w.write_i32(ev.party_2_no);
            w.write_i64(ev.timer_value);
            w.write_f32(ev.duration);
        } else {
            w.write_i32(0);
        }
    }

    // saveTroops (0x1400ca8e0)
    w.write_i32(static_cast<std::int32_t>(world.troops.size()));
    for (const WseTroop& tr : world.troops) {
        write_slots(w, tr.slots);
        for (const std::int32_t attr : tr.attributes) w.write_i32(attr);
        for (const float prof : tr.proficiencies) w.write_f32(prof);
        for (const std::uint32_t skill_word : tr.skills) w.write_u32(skill_word);
        write_notes(w, tr.notes, tr.note_tableau_material_no, tr.note_available);
        w.write_u64(tr.flags);
        const std::uint32_t site_entry =
            ((static_cast<std::uint32_t>(tr.home_entry_no) & 0xFFFFU) << 16U) |
            (static_cast<std::uint32_t>(tr.home_site_no) & 0xFFFFU);
        w.write_u32(site_entry);
        w.write_i32(tr.skill_points);
        w.write_i32(tr.attribute_points);
        w.write_i32(tr.proficiency_points);
        w.write_i32(tr.level);
        if (tr.is_hero() || !ctx.dont_load_regular_troop_inventories) {
            w.write_i32(tr.gold);
            w.write_i32(tr.xp);
            w.write_f32(tr.hit_points);
            w.write_i32(tr.faction_no);
            for (std::size_t i = static_cast<std::size_t>(kNumTroopEquipmentSlots);
                 i < static_cast<std::size_t>(kTotalTroopInventoryCapacity); ++i) {
                w.write_i32(tr.inventory[i].item_no);
                w.write_u32(pack_inventory_flags(tr.inventory[i]));
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(kNumTroopEquipmentSlots); ++i) {
                w.write_i32(tr.inventory[i].item_no);
                w.write_u32(pack_inventory_flags(tr.inventory[i]));
            }
            for (const std::uint64_t fk : tr.face_keys[0]) w.write_u64(fk);
            w.write_bool(tr.renamed);
            if (tr.renamed) {
                w.write_string(tr.name);
                w.write_string(tr.plural_name);
            }
            w.write_i32(tr.class_no);
        }
    }
    for (std::int32_t s = 0; s < kMaxSkills; ++s) {
        w.write_i32(world.skills[static_cast<std::size_t>(s)].max_level);
    }

    // Item-kind slots + player tail (0x1400c9db1..0x1400c9e9d)
    w.write_i32(static_cast<std::int32_t>(world.items.size()));
    for (const WseItemKind& item : world.items) {
        write_slots(w, item.slots);
    }
    std::array<std::uint64_t, 4> player_fk{};
    if (world.is_valid_troop(world.player_troop_no)) {
        player_fk = world.troops[static_cast<std::size_t>(world.player_troop_no)].face_keys[0];
    }
    w.write_u64(player_fk[0]);
    w.write_u64(player_fk[1]);
    w.write_i32(world.player_kill_count);
    w.write_i32(world.player_teamkill_count);
    w.write_i32(world.player_wound_count);
    w.write_i32(world.player_teamwound_count);
}

[[nodiscard]] inline SavegameContainerResult read_quests_section(
    io::BinaryReader& r, WseWorldState& world, bool& out_count_exceeded) {
    std::int32_t file_count = 0;
    // u32 flags + i32 giver + i32 number + f32 date + 3 string lengths.
    const SavegameContainerResult rc = read_checked_count(r, file_count, 28U);
    if (rc != SavegameContainerResult::ok) return rc;
    const std::size_t avail = world.quests.size();
    if (static_cast<std::size_t>(file_count) > avail) out_count_exceeded = true;

    for (std::int32_t i = 0; i < file_count; ++i) {
        WseQuest parsed{};
        if (!r.read_u32(parsed.progression_flags) || !r.read_i32(parsed.giver_troop_no) ||
            !r.read_i32(parsed.number) || !r.read_f32(parsed.start_date) ||
            !r.read_string(parsed.title) || !r.read_string(parsed.text) ||
            !r.read_string(parsed.giver_name)) {
            return SavegameContainerResult::truncated;
        }
        if (read_notes(r, &parsed.notes, &parsed.note_tableau_material_no,
                       &parsed.note_available) != SavegameContainerResult::ok) {
            return SavegameContainerResult::truncated;
        }
        const SavegameContainerResult s_rc = read_slots(r, &parsed.slots);
        if (s_rc != SavegameContainerResult::ok) return s_rc;
        if (static_cast<std::size_t>(i) < avail) {
            WseQuest& dst = world.quests[static_cast<std::size_t>(i)];
            dst.progression_flags = parsed.progression_flags;
            dst.giver_troop_no = parsed.giver_troop_no;
            dst.number = parsed.number;
            dst.start_date = parsed.start_date;
            dst.title = std::move(parsed.title);
            dst.text = std::move(parsed.text);
            dst.giver_name = std::move(parsed.giver_name);
            dst.notes = parsed.notes;
            dst.note_tableau_material_no = parsed.note_tableau_material_no;
            dst.note_available = parsed.note_available;
            dst.slots = std::move(parsed.slots);
            world.misc.quest_strings[i] = WseQuestStrings{dst.text, dst.giver_name};
        }
    }
    return SavegameContainerResult::ok;
}

[[nodiscard]] inline SavegameContainerResult read_world_body_sections(
    io::BinaryReader& r,
    std::int32_t savegame_version,
    SavegameRuntimeContext& ctx,
    bool& out_count_exceeded) {
    WseWorldState& world = *ctx.world;
    if (!r.read_i64(world.campaign_time) || !r.read_i32(world.seed) ||
        !r.read_u32(world.encounter_flags)) {
        return SavegameContainerResult::truncated;
    }
    if (savegame_has_combat_options(savegame_version)) {
        if (!r.read_i32(world.combat_damage_to_self) ||
            !r.read_i32(world.combat_damage_to_friends) || !r.read_i32(world.reduce_battle_ai) ||
            !r.read_i32(world.reduce_campaign_ai) || !r.read_i32(world.combat_speed)) {
            return SavegameContainerResult::truncated;
        }
    }
    // Bodies <= 1136 carry no combat options: like the retail branch, nothing is read and the
    // live (staged) values are kept. Whether retail then re-applies config defaults is open
    // (STATUS.md §5).
    if (const auto rc = read_save_game_section(r, world); rc != SavegameContainerResult::ok) {
        return rc;
    }
    if (const auto rc = read_slots(r, ctx.global_variables); rc != SavegameContainerResult::ok) {
        return rc;
    }
    if (ctx.triggers != nullptr) {
        const SavegameResult tr = read_trigger_savegame(*ctx.triggers, r);
        if (tr == SavegameResult::count_exceeded) out_count_exceeded = true;
        if (const auto rc = map_trigger_result(tr); rc != SavegameContainerResult::ok) return rc;
    } else {
        TriggerManager scratch;
        const SavegameResult tr = read_trigger_savegame(scratch, r);
        if (const auto rc = map_trigger_result(tr); rc != SavegameContainerResult::ok) return rc;
    }
    if (ctx.simple_triggers != nullptr) {
        const SavegameResult sr = read_simple_trigger_savegame(*ctx.simple_triggers, r);
        if (sr == SavegameResult::count_exceeded) out_count_exceeded = true;
        if (const auto rc = map_trigger_result(sr); rc != SavegameContainerResult::ok) return rc;
    } else {
        SimpleTriggerManager scratch;
        const SavegameResult sr = read_simple_trigger_savegame(scratch, r);
        if (const auto rc = map_trigger_result(sr); rc != SavegameContainerResult::ok) return rc;
    }
    if (const auto rc = read_quests_section(r, world, out_count_exceeded);
        rc != SavegameContainerResult::ok) {
        return rc;
    }

    // info_pages
    {
        std::int32_t count = 0;
        // Each info page is 16 notes of (string length, date, int, bool).
        const auto rc =
            read_checked_count(r, count, static_cast<std::size_t>(kNumQuestNotes) * 13U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.info_pages.size()) out_count_exceeded = true;
        for (std::int32_t i = 0; i < count; ++i) {
            WseInfoPage* dst = static_cast<std::size_t>(i) < world.info_pages.size()
                                   ? &world.info_pages[static_cast<std::size_t>(i)]
                                   : nullptr;
            if (read_notes(r, dst != nullptr ? &dst->notes : nullptr,
                           dst != nullptr ? &dst->note_tableau_material_no : nullptr,
                           dst != nullptr ? &dst->note_available : nullptr) !=
                SavegameContainerResult::ok) {
                return SavegameContainerResult::truncated;
            }
        }
    }

    // sites
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 4U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.sites.size()) out_count_exceeded = true;
        for (std::int32_t i = 0; i < count; ++i) {
            wse::vm::Slots* dst = static_cast<std::size_t>(i) < world.sites.size()
                                      ? &world.sites[static_cast<std::size_t>(i)].slots
                                      : nullptr;
            if (const auto src_rc = read_slots(r, dst); src_rc != SavegameContainerResult::ok) {
                return src_rc;
            }
        }
    }

    // factions
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 4U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.factions.size()) out_count_exceeded = true;
        for (std::int32_t i = 0; i < count; ++i) {
            WseFaction tmp{};
            if (const auto src_rc = read_slots(r, &tmp.slots);
                src_rc != SavegameContainerResult::ok) {
                return src_rc;
            }
            tmp.relations.resize(static_cast<std::size_t>(count), 0.0f);
            for (std::int32_t j = 0; j < count; ++j) {
                if (!r.read_f32(tmp.relations[static_cast<std::size_t>(j)])) {
                    return SavegameContainerResult::truncated;
                }
            }
            std::int32_t unused = 0;
            if (!r.read_string(tmp.name) || !r.read_bool(tmp.renamed) || !r.read_u32(tmp.color) ||
                !r.read_i32(unused) ||
                read_notes(r, &tmp.notes, &tmp.note_tableau_material_no, &tmp.note_available) !=
                    SavegameContainerResult::ok) {
                return SavegameContainerResult::truncated;
            }
            if (static_cast<std::size_t>(i) < world.factions.size()) {
                WseFaction& dst = world.factions[static_cast<std::size_t>(i)];
                dst.slots = std::move(tmp.slots);
                dst.relations = std::move(tmp.relations);
                dst.name = std::move(tmp.name);
                dst.renamed = tmp.renamed;
                dst.color = tmp.color;
                dst.notes = tmp.notes;
                dst.note_tableau_material_no = tmp.note_tableau_material_no;
                dst.note_available = tmp.note_available;
            }
        }
    }

    // tracks
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 24U);
        if (rc != SavegameContainerResult::ok) return rc;
        world.tracks.resize(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            WseSavegameTrack& trk = world.tracks[static_cast<std::size_t>(i)];
            if (!r.read_f32(trk.position.x) || !r.read_f32(trk.position.y) ||
                !r.read_f32(trk.position.z) || !r.read_f32(trk.rotation_z) ||
                !r.read_f32(trk.age) || !r.read_u32(trk.flags)) {
                return SavegameContainerResult::truncated;
            }
        }
    }

    // party templates
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 16U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.party_templates.size()) {
            out_count_exceeded = true;
        }
        for (std::int32_t i = 0; i < count; ++i) {
            std::int32_t created = 0;
            std::int32_t destroyed = 0;
            std::int32_t destroyed_player = 0;
            wse::vm::Slots tmp_slots{};
            if (!r.read_i32(created) || !r.read_i32(destroyed) || !r.read_i32(destroyed_player)) {
                return SavegameContainerResult::truncated;
            }
            if (const auto src_rc = read_slots(r, &tmp_slots);
                src_rc != SavegameContainerResult::ok) {
                return src_rc;
            }
            if (static_cast<std::size_t>(i) < world.party_templates.size()) {
                WsePartyTemplate& pt = world.party_templates[static_cast<std::size_t>(i)];
                pt.num_created = created;
                pt.num_destroyed = destroyed;
                pt.num_destroyed_by_player = destroyed_player;
                pt.slots = std::move(tmp_slots);
            }
        }
    }

    // parties (rglHashVector<mbParty>::readStream 0x1400e7ce0 + main party extra XP/DNAs)
    {
        std::int32_t num_slots = 0;
        std::int32_t next_key = 0;
        if (!r.read_i32(num_slots) || !r.read_i32(next_key)) {
            return SavegameContainerResult::truncated;
        }
        if (num_slots < 0 || next_key < 0) return SavegameContainerResult::negative_count;
        // `parties` is indexed by party key, so every key below `next_party_key` costs a
        // `WseParty` (as a tombstone once removed); `kMaxPartyKeys` bounds that allocation.
        if (num_slots > kMaxPartyKeys || next_key > kMaxPartyKeys) {
            return SavegameContainerResult::count_limit_exceeded;
        }
        // Every slot holds at least its int32 present flag; checking this before the
        // allocation stops a few-byte file from allocating ~160 MB of `WseParty`.
        if (static_cast<std::size_t>(num_slots) > r.remaining() / 4U) {
            return SavegameContainerResult::truncated;
        }
        world.parties.assign(static_cast<std::size_t>(num_slots), WseParty{});
        for (auto& slot : world.parties) slot.active = false;
        world.next_party_key = std::max(num_slots, next_key);

        for (std::int32_t i = 0; i < num_slots; ++i) {
            std::int32_t valid = 0;
            if (!r.read_i32(valid)) return SavegameContainerResult::truncated;
            if (valid != 0 && valid != 1) return SavegameContainerResult::corrupt_record;
            if (valid == 1) {
                std::int32_t raw_idx = 0;
                std::int32_t key = 0;
                if (!r.read_i32(raw_idx) || !r.read_i32(key)) {
                    return SavegameContainerResult::truncated;
                }
                // Slot index and key differ once retail has recycled hash-vector slots
                // (`m_nextKey > m_numSlots`), so a key may lie past `num_slots`: grow the
                // key-indexed vector to it instead of dropping the party. Every key handed out
                // is below `m_nextKey`, so a larger one marks a corrupt record.
                if (key >= world.next_party_key) return SavegameContainerResult::corrupt_record;
                if (key >= 0 && static_cast<std::size_t>(key) >= world.parties.size()) {
                    const std::size_t old_size = world.parties.size();
                    world.parties.resize(static_cast<std::size_t>(key) + 1U);
                    for (std::size_t k = old_size; k < world.parties.size(); ++k) {
                        world.parties[k].active = false;
                    }
                }
                WseParty* dst = (key >= 0) ? &world.parties[static_cast<std::size_t>(key)]
                                           : nullptr;
                if (dst == nullptr) out_count_exceeded = true;  // record consumed, dropped
                if (const auto p_rc = read_party_record(r, savegame_version, dst);
                    p_rc != SavegameContainerResult::ok) {
                    return p_rc;
                }
            }
        }
        if (world.is_valid_party(world.main_party_no)) {
            WseParty& mp = world.parties[static_cast<std::size_t>(world.main_party_no)];
            for (std::size_t s = 0; s < mp.stacks.size(); ++s) {
                WsePartyStack& st = mp.stacks[s];
                const bool hero = world.is_valid_troop(st.troop_no) &&
                                  world.troops[static_cast<std::size_t>(st.troop_no)].is_hero();
                if (!hero) {
                    if (!r.read_f32(st.xp) || !r.read_i32(st.num_upgradeable)) {
                        return SavegameContainerResult::truncated;
                    }
                    if (s < 32U) {
                        for (std::size_t k = 0; k < 32U; ++k) {
                            if (!r.read_i32(world.main_party_dnas[s * 32U + k])) {
                                return SavegameContainerResult::truncated;
                            }
                        }
                    }
                }
            }
        }
    }

    // events
    {
        std::int32_t count = 0;
        std::int32_t unused = 0;
        if (!r.read_i32(count) || !r.read_i32(unused)) {
            return SavegameContainerResult::truncated;
        }
        (void)unused;
        if (count < 0) return SavegameContainerResult::negative_count;
        if (static_cast<std::size_t>(count) > kMaxSavegameArrayCount) {
            return SavegameContainerResult::count_limit_exceeded;
        }
        if (static_cast<std::size_t>(count) > r.remaining() / 4U) {
            return SavegameContainerResult::truncated;
        }
        world.events.assign(static_cast<std::size_t>(count), WseSavegameEvent{});
        for (std::int32_t i = 0; i < count; ++i) {
            std::int32_t valid = 0;
            if (!r.read_i32(valid)) return SavegameContainerResult::truncated;
            if (valid != 0 && valid != 1) return SavegameContainerResult::corrupt_record;
            if (valid == 1) {
                std::int32_t dummy = 0;
                float df0 = 0.0f;
                float df1 = 0.0f;
                WseSavegameEvent& ev = world.events[static_cast<std::size_t>(i)];
                if (!r.read_i32(dummy) || !r.read_string(ev.extra) || !r.read_i32(ev.active) ||
                    !r.read_f32(ev.position.x) || !r.read_f32(ev.position.y) ||
                    !r.read_f32(ev.position.z) || !r.read_f32(ev.position.w) || !r.read_f32(df0) ||
                    !r.read_f32(df1) || !r.read_i32(ev.party_1_no) || !r.read_i32(ev.party_2_no) ||
                    !r.read_i64(ev.timer_value) || !r.read_f32(ev.duration)) {
                    return SavegameContainerResult::truncated;
                }
            }
        }
    }

    // troops + skills max levels
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 4U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.troops.size()) out_count_exceeded = true;
        for (std::int32_t i = 0; i < count; ++i) {
            WseTroop tmp{};
            if (const auto s_rc = read_slots(r, &tmp.slots);
                s_rc != SavegameContainerResult::ok) {
                return s_rc;
            }
            for (std::int32_t& a : tmp.attributes) {
                if (!r.read_i32(a)) return SavegameContainerResult::truncated;
            }
            for (float& pr : tmp.proficiencies) {
                if (!r.read_f32(pr)) return SavegameContainerResult::truncated;
            }
            for (std::uint32_t& sw : tmp.skills) {
                if (!r.read_u32(sw)) return SavegameContainerResult::truncated;
            }
            if (read_notes(r, &tmp.notes, &tmp.note_tableau_material_no, &tmp.note_available) !=
                SavegameContainerResult::ok) {
                return SavegameContainerResult::truncated;
            }
            std::uint32_t site_entry = 0;
            if (!r.read_u64(tmp.flags) || !r.read_u32(site_entry) ||
                !r.read_i32(tmp.skill_points) || !r.read_i32(tmp.attribute_points) ||
                !r.read_i32(tmp.proficiency_points) || !r.read_i32(tmp.level)) {
                return SavegameContainerResult::truncated;
            }
            tmp.home_site_no = static_cast<std::int16_t>(site_entry & 0xFFFFU);
            tmp.home_entry_no = static_cast<std::int16_t>((site_entry >> 16U) & 0xFFFFU);
            tmp.skin_no = static_cast<std::int32_t>(tmp.flags & 0xFU);

            const bool has_inventory_block =
                tmp.is_hero() || !ctx.dont_load_regular_troop_inventories;
            if (has_inventory_block) {
                if (!r.read_i32(tmp.gold) || !r.read_i32(tmp.xp) || !r.read_f32(tmp.hit_points) ||
                    !r.read_i32(tmp.faction_no)) {
                    return SavegameContainerResult::truncated;
                }
                for (std::size_t k = static_cast<std::size_t>(kNumTroopEquipmentSlots);
                     k < static_cast<std::size_t>(kTotalTroopInventoryCapacity); ++k) {
                    std::int32_t item_no = -1;
                    std::uint32_t packed = 0;
                    if (!r.read_i32(item_no) || !r.read_u32(packed)) {
                        return SavegameContainerResult::truncated;
                    }
                    unpack_inventory_flags(item_no, packed, tmp.inventory[k]);
                }
                for (std::size_t k = 0; k < static_cast<std::size_t>(kNumTroopEquipmentSlots);
                     ++k) {
                    std::int32_t item_no = -1;
                    std::uint32_t packed = 0;
                    if (!r.read_i32(item_no) || !r.read_u32(packed)) {
                        return SavegameContainerResult::truncated;
                    }
                    unpack_inventory_flags(item_no, packed, tmp.inventory[k]);
                }
                for (std::uint64_t& fk : tmp.face_keys[0]) {
                    if (!r.read_u64(fk)) return SavegameContainerResult::truncated;
                }
                if (!r.read_bool(tmp.renamed)) return SavegameContainerResult::truncated;
                if (tmp.renamed) {
                    if (!r.read_string(tmp.name) || !r.read_string(tmp.plural_name)) {
                        return SavegameContainerResult::truncated;
                    }
                }
                if (!r.read_i32(tmp.class_no)) return SavegameContainerResult::truncated;
            }

            // Merge only what the file carried. Replacing the whole troop reset every
            // regular troop's gold / xp / hit points / faction / inventory / face / class
            // to `WseTroop{}` defaults whenever `bDontLoadRegularTroopInventories` skipped
            // that block, and dropped module-only fields such as upgrade paths.
            if (static_cast<std::size_t>(i) < world.troops.size()) {
                WseTroop& dst = world.troops[static_cast<std::size_t>(i)];
                dst.slots = std::move(tmp.slots);
                dst.attributes = tmp.attributes;
                dst.proficiencies = tmp.proficiencies;
                dst.skills = tmp.skills;
                dst.notes = std::move(tmp.notes);
                dst.note_tableau_material_no = tmp.note_tableau_material_no;
                dst.note_available = tmp.note_available;
                dst.flags = tmp.flags;
                dst.skin_no = tmp.skin_no;
                dst.home_site_no = tmp.home_site_no;
                dst.home_entry_no = tmp.home_entry_no;
                dst.skill_points = tmp.skill_points;
                dst.attribute_points = tmp.attribute_points;
                dst.proficiency_points = tmp.proficiency_points;
                dst.level = tmp.level;
                if (has_inventory_block) {
                    dst.gold = tmp.gold;
                    dst.xp = tmp.xp;
                    dst.hit_points = tmp.hit_points;
                    dst.faction_no = tmp.faction_no;
                    dst.inventory = tmp.inventory;
                    dst.face_keys[0] = tmp.face_keys[0];
                    dst.renamed = tmp.renamed;
                    if (tmp.renamed) {
                        dst.name = std::move(tmp.name);
                        dst.plural_name = std::move(tmp.plural_name);
                    }
                    dst.class_no = tmp.class_no;
                }
            }
        }
        for (std::int32_t s = 0; s < kMaxSkills; ++s) {
            if (!r.read_i32(world.skills[static_cast<std::size_t>(s)].max_level)) {
                return SavegameContainerResult::truncated;
            }
        }
    }

    // item-kind slots
    {
        std::int32_t count = 0;
        const auto rc = read_checked_count(r, count, 4U);
        if (rc != SavegameContainerResult::ok) return rc;
        if (static_cast<std::size_t>(count) > world.items.size()) out_count_exceeded = true;
        for (std::int32_t i = 0; i < count; ++i) {
            wse::vm::Slots* dst = static_cast<std::size_t>(i) < world.items.size()
                                      ? &world.items[static_cast<std::size_t>(i)].slots
                                      : nullptr;
            if (const auto src_rc = read_slots(r, dst); src_rc != SavegameContainerResult::ok) {
                return src_rc;
            }
        }
    }

    // player tail
    std::uint64_t p_fk0 = 0;
    std::uint64_t p_fk1 = 0;
    if (!r.read_u64(p_fk0) || !r.read_u64(p_fk1) || !r.read_i32(world.player_kill_count) ||
        !r.read_i32(world.player_teamkill_count) || !r.read_i32(world.player_wound_count) ||
        !r.read_i32(world.player_teamwound_count)) {
        return SavegameContainerResult::truncated;
    }
    if (world.is_valid_troop(world.player_troop_no)) {
        auto& fk = world.troops[static_cast<std::size_t>(world.player_troop_no)].face_keys[0];
        fk[0] = p_fk0;
        fk[1] = p_fk1;
    }
    return SavegameContainerResult::ok;
}

}  // namespace savegame_detail

// Serializes the full savegame container (`WRDR` framing + `mbSavegame` header +
// all `mbGame` sections) into `writer` with byte-sum checksum tracking.
[[nodiscard]] inline SavegameWriteResult write_savegame_container(
    io::BinaryChecksumWriter& writer,
    const SavegameConstContext& ctx,
    std::string_view override_name = {},
    std::int32_t savegame_version = kSavegameFormatVersion) {
    SavegameWriteResult res{};
    if (ctx.world == nullptr) {
        return res;
    }
    if (savegame_version < kSavegameMinVersion || savegame_version > kSavegameFormatVersion) {
        res.status = SavegameContainerResult::unsupported_version;
        return res;
    }
    writer.clear();
    SavegameHeader header =
        fill_savegame_header(*ctx.world, ctx.timers_total, override_name);
    header.savegame_version = savegame_version;
    writer.write_via([&](io::BinaryWriter& raw) {
        write_savegame_header(raw, header);
        savegame_detail::write_world_body_sections(raw, ctx, savegame_version);
    });
    // The in-memory buffer cannot disagree with its own running sum; the meaningful
    // verification is the read-back in `save_world_to_vfs`, as in `mbGame::save`.
    res.checksum = writer.checksum();
    res.bytes_written = writer.size();
    res.ok = true;
    res.status = SavegameContainerResult::ok;
    return res;
}

// Parses the container header + world sections from `bytes`. Optionally verifies
// `expected_checksum` (if provided) and `compatible_module_version`. Note:
// `ctx.world->troops` must already hold the module's troop definitions (via
// `mbGame::readSettingsData` before `mbGame::readStream`, `0x1401129f0..0x140112ad8`) so
// `loadParties` (`0x1400c9420`) distinguishes hero vs regular stacks (`tf_hero` `0x10` is
// never toggled at runtime by `troop_set_type` `0x1401d0370`) and un-renamed troops retain
// their `troops.txt` names in `loadTroops` (`0x1400c97b0`).
// The body is parsed into staged copies of every target and committed only on
// success; on any failure the world, globals, and trigger managers are unchanged.
[[nodiscard]] inline SavegameLoadReport read_savegame_container(
    std::span<const std::byte> bytes,
    SavegameRuntimeContext& ctx,
    const std::uint64_t* expected_checksum = nullptr) {
    SavegameLoadReport report{};
    if (ctx.world == nullptr) {
        report.status = SavegameContainerResult::io_error;
        return report;
    }
    report.computed_checksum = io::compute_savegame_checksum(bytes);
    if (expected_checksum != nullptr && report.computed_checksum != *expected_checksum) {
        report.status = SavegameContainerResult::checksum_mismatch;
        return report;
    }

    io::BinaryReader r(bytes);
    const SavegameContainerResult h_rc = read_savegame_header(
        r, report.header, ctx.world->compatible_savegame_module_version);
    if (h_rc != SavegameContainerResult::ok) {
        report.status = h_rc;
        return report;
    }

    WseWorldState staged_world = *ctx.world;
    wse::vm::Slots staged_globals;
    TriggerManager staged_triggers;
    SimpleTriggerManager staged_simple;
    SavegameRuntimeContext staged = ctx;
    staged.world = &staged_world;
    if (ctx.global_variables != nullptr) {
        staged_globals = *ctx.global_variables;
        staged.global_variables = &staged_globals;
    }
    if (ctx.triggers != nullptr) {
        staged_triggers = *ctx.triggers;
        staged.triggers = &staged_triggers;
    }
    if (ctx.simple_triggers != nullptr) {
        staged_simple = *ctx.simple_triggers;
        staged.simple_triggers = &staged_simple;
    }

    bool count_exceeded = false;
    const SavegameContainerResult b_rc = savegame_detail::read_world_body_sections(
        r, report.header.savegame_version, staged, count_exceeded);
    report.bytes_consumed = r.cursor();
    report.status = b_rc;
    if (b_rc != SavegameContainerResult::ok) {
        return report;
    }
    report.count_exceeded = count_exceeded;
    *ctx.world = std::move(staged_world);
    if (ctx.global_variables != nullptr) *ctx.global_variables = std::move(staged_globals);
    if (ctx.triggers != nullptr) *ctx.triggers = std::move(staged_triggers);
    if (ctx.simple_triggers != nullptr) *ctx.simple_triggers = std::move(staged_simple);
    return report;
}

// Writes a savegame through `vfs` via staging (`new_game.sav`) + byte-sum
// checksum read-back verification + optional backup rename matching mbGame::save
// (`0x1400e1820`). Returns the finalized byte count & checksum.
[[nodiscard]] inline SavegameWriteResult save_world_to_vfs(
    platform::Vfs& vfs,
    std::string_view dir_path,
    std::string_view file_name,
    const SavegameConstContext& ctx,
    std::string_view override_name = {}) {
    SavegameWriteResult res{};
    if (ctx.world == nullptr || file_name.empty()) {
        return res;
    }

    const auto join = [&](std::string_view name) {
        if (dir_path.empty()) return std::string(name);
        std::string out(dir_path);
        if (out.back() != '/' && out.back() != '\\') out.push_back('/');
        out.append(name);
        return out;
    };

    const std::string final_path = join(file_name);
    const std::string tmp_path = join("new_game.sav");
    const std::string backup_path = join("last_savegame_backup.sav");

    if (!platform::make_parent_dirs(vfs, final_path)) {
        res.status = SavegameContainerResult::io_error;
        return res;
    }

    io::BinaryChecksumWriter writer;
    res = write_savegame_container(writer, ctx, override_name);
    if (!res.ok) {
        return res;
    }

    const std::span<const std::byte> raw_bytes = writer.bytes();
    const std::span<const std::uint8_t> payload(
        reinterpret_cast<const std::uint8_t*>(raw_bytes.data()), raw_bytes.size());
    if (!vfs.write_file(tmp_path, payload)) {
        res.ok = false;
        res.status = SavegameContainerResult::io_error;
        return res;
    }

    std::vector<std::uint8_t> readback;
    if (!vfs.read_file(tmp_path, readback)) {
        res.ok = false;
        res.status = SavegameContainerResult::io_error;
        return res;
    }
    const std::span<const std::byte> rb_bytes(
        reinterpret_cast<const std::byte*>(readback.data()), readback.size());
    // A byte sum alone misses lost zero bytes (a save truncated inside trailing zero
    // fields sums identically), so also require the same length and contents.
    if (readback.size() != payload.size() ||
        !std::equal(readback.begin(), readback.end(), payload.begin()) ||
        io::compute_savegame_checksum(rb_bytes) != res.checksum) {
        res.ok = false;
        res.status = SavegameContainerResult::checksum_mismatch;
        return res;
    }

    (void)vfs.remove_file(backup_path);
    if (vfs.is_file(final_path)) {
        (void)vfs.rename_file(final_path, backup_path);
    }
    if (!vfs.rename_file(tmp_path, final_path)) {
        if (!vfs.write_file(final_path, payload)) {
            res.ok = false;
            res.status = SavegameContainerResult::io_error;
            return res;
        }
        (void)vfs.remove_file(tmp_path);
    }
    return res;
}

[[nodiscard]] inline SavegameContainerResult inspect_savegame_header_vfs(
    platform::Vfs& vfs,
    std::string_view file_path,
    SavegameHeader& out_header,
    std::int32_t compatible_module_version = std::numeric_limits<std::int32_t>::min()) {
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(file_path, raw)) {
        return SavegameContainerResult::io_error;
    }
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    io::BinaryReader r(bytes);
    return read_savegame_header(r, out_header, compatible_module_version);
}

[[nodiscard]] inline SavegameLoadReport load_world_from_vfs(
    platform::Vfs& vfs,
    std::string_view file_path,
    SavegameRuntimeContext& ctx,
    const std::uint64_t* expected_checksum = nullptr) {
    SavegameLoadReport report{};
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(file_path, raw)) {
        report.status = SavegameContainerResult::io_error;
        return report;
    }
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    return read_savegame_container(bytes, ctx, expected_checksum);
}

}  // namespace wse::game
