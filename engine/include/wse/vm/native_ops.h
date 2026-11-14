// Extended native Warband opcode handlers (v1.1.5.5 mbStatement::execute).
//
// Grounded in official WSE2 v1.1.5.5 PDB/binary decompilation:
//   entity_slots (500..510, 520..530, 540..550, 560..570)  0x1401c9799..0x1401ca66d
//   positions_and_math (700..746)                          0x1401ca915..0x1401cbc3c
//   display_message (1106)                                 0x1401cdb54
//   troop_is_hero (1507), troop_set_health (1560)          0x1401d027e, 0x1401d132e
//   agent_is_human (1704)                                  0x1401d37d3
//   shuffle_range..store_troop_gold (2134..2149)           0x1401dd37c..0x1401dd5ea
//   store_troop_faction (2173), store_faction_of_party     0x1401dd9d8, 0x1401ddd7f
//   strings (2318..2355)                                   0x1401de882..0x1401df231

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "wse/vm/block_vm.h"

namespace wse::vm {

namespace core_op {
inline constexpr std::uint32_t kTroopSetSlot = 500U;
inline constexpr std::uint32_t kPartySetSlot = 501U;
inline constexpr std::uint32_t kFactionSetSlot = 502U;
inline constexpr std::uint32_t kSceneSetSlot = 503U;
inline constexpr std::uint32_t kPartyTemplateSetSlot = 504U;
inline constexpr std::uint32_t kAgentSetSlot = 505U;
inline constexpr std::uint32_t kQuestSetSlot = 506U;
inline constexpr std::uint32_t kItemSetSlot = 507U;
inline constexpr std::uint32_t kPlayerSetSlot = 508U;
inline constexpr std::uint32_t kTeamSetSlot = 509U;
inline constexpr std::uint32_t kScenePropSetSlot = 510U;

inline constexpr std::uint32_t kTroopGetSlot = 520U;
inline constexpr std::uint32_t kPartyGetSlot = 521U;
inline constexpr std::uint32_t kFactionGetSlot = 522U;
inline constexpr std::uint32_t kSceneGetSlot = 523U;
inline constexpr std::uint32_t kPartyTemplateGetSlot = 524U;
inline constexpr std::uint32_t kAgentGetSlot = 525U;
inline constexpr std::uint32_t kQuestGetSlot = 526U;
inline constexpr std::uint32_t kItemGetSlot = 527U;
inline constexpr std::uint32_t kPlayerGetSlot = 528U;
inline constexpr std::uint32_t kTeamGetSlot = 529U;
inline constexpr std::uint32_t kScenePropGetSlot = 530U;

inline constexpr std::uint32_t kTroopSlotEq = 540U;
inline constexpr std::uint32_t kPartySlotEq = 541U;
inline constexpr std::uint32_t kFactionSlotEq = 542U;
inline constexpr std::uint32_t kSceneSlotEq = 543U;
inline constexpr std::uint32_t kPartyTemplateSlotEq = 544U;
inline constexpr std::uint32_t kAgentSlotEq = 545U;
inline constexpr std::uint32_t kQuestSlotEq = 546U;
inline constexpr std::uint32_t kItemSlotEq = 547U;
inline constexpr std::uint32_t kPlayerSlotEq = 548U;
inline constexpr std::uint32_t kTeamSlotEq = 549U;
inline constexpr std::uint32_t kScenePropSlotEq = 550U;

inline constexpr std::uint32_t kTroopSlotGe = 560U;
inline constexpr std::uint32_t kPartySlotGe = 561U;
inline constexpr std::uint32_t kFactionSlotGe = 562U;
inline constexpr std::uint32_t kSceneSlotGe = 563U;
inline constexpr std::uint32_t kPartyTemplateSlotGe = 564U;
inline constexpr std::uint32_t kAgentSlotGe = 565U;
inline constexpr std::uint32_t kQuestSlotGe = 566U;
inline constexpr std::uint32_t kItemSlotGe = 567U;
inline constexpr std::uint32_t kPlayerSlotGe = 568U;
inline constexpr std::uint32_t kTeamSlotGe = 569U;
inline constexpr std::uint32_t kScenePropSlotGe = 570U;

inline constexpr std::uint32_t kCopyPosition = 700U;
inline constexpr std::uint32_t kInitPosition = 701U;
inline constexpr std::uint32_t kGetAngleBetweenPositions = 705U;
inline constexpr std::uint32_t kGetDistanceBetweenPositions = 710U;
inline constexpr std::uint32_t kGetDistanceBetweenPositionsInMeters = 711U;
inline constexpr std::uint32_t kPositionIsBehindPosition = 714U;
inline constexpr std::uint32_t kPositionTransformPositionToParent = 716U;
inline constexpr std::uint32_t kPositionTransformPositionToLocal = 717U;
inline constexpr std::uint32_t kPositionCopyRotation = 718U;
inline constexpr std::uint32_t kPositionCopyOrigin = 719U;
inline constexpr std::uint32_t kPositionMoveX = 720U;
inline constexpr std::uint32_t kPositionMoveY = 721U;
inline constexpr std::uint32_t kPositionMoveZ = 722U;
inline constexpr std::uint32_t kPositionRotateX = 723U;
inline constexpr std::uint32_t kPositionRotateY = 724U;
inline constexpr std::uint32_t kPositionRotateZ = 725U;
inline constexpr std::uint32_t kPositionGetX = 726U;
inline constexpr std::uint32_t kPositionGetY = 727U;
inline constexpr std::uint32_t kPositionGetZ = 728U;
inline constexpr std::uint32_t kPositionSetX = 729U;
inline constexpr std::uint32_t kPositionSetY = 730U;
inline constexpr std::uint32_t kPositionSetZ = 731U;
inline constexpr std::uint32_t kPositionRotateZFloating = 734U;
inline constexpr std::uint32_t kPositionGetScaleX = 735U;
inline constexpr std::uint32_t kPositionGetScaleY = 736U;
inline constexpr std::uint32_t kPositionGetScaleZ = 737U;
inline constexpr std::uint32_t kPositionRotateXFloating = 738U;
inline constexpr std::uint32_t kPositionRotateYFloating = 739U;
inline constexpr std::uint32_t kPositionGetRotationAroundZ = 740U;
inline constexpr std::uint32_t kPositionNormalizeOrigin = 741U;
inline constexpr std::uint32_t kPositionGetRotationAroundX = 742U;
inline constexpr std::uint32_t kPositionGetRotationAroundY = 743U;
inline constexpr std::uint32_t kPositionSetScaleX = 744U;
inline constexpr std::uint32_t kPositionSetScaleY = 745U;
inline constexpr std::uint32_t kPositionSetScaleZ = 746U;

inline constexpr std::uint32_t kDisplayMessage = 1106U;
inline constexpr std::uint32_t kTroopIsHero = 1507U;
inline constexpr std::uint32_t kTroopSetHealth = 1560U;
inline constexpr std::uint32_t kAgentIsHuman = 1704U;

inline constexpr std::uint32_t kShuffleRange = 2134U;
inline constexpr std::uint32_t kStoreRandom = 2135U;
inline constexpr std::uint32_t kStoreRandomInRange = 2136U;
inline constexpr std::uint32_t kStoreAsin = 2140U;
inline constexpr std::uint32_t kStoreAcos = 2141U;
inline constexpr std::uint32_t kStoreAtan = 2142U;
inline constexpr std::uint32_t kStoreAtan2 = 2143U;
inline constexpr std::uint32_t kStoreTroopGold = 2149U;
inline constexpr std::uint32_t kStoreTroopFaction = 2173U;
inline constexpr std::uint32_t kStoreFactionOfParty = 2204U;

inline constexpr std::uint32_t kStrIsEmpty = 2318U;
inline constexpr std::uint32_t kStrClear = 2319U;
inline constexpr std::uint32_t kStrStoreString = 2320U;
inline constexpr std::uint32_t kStrStoreStringReg = 2321U;
inline constexpr std::uint32_t kStrStoreTroopName = 2322U;
inline constexpr std::uint32_t kStrStoreTroopNamePlural = 2323U;
inline constexpr std::uint32_t kStrStoreTroopNameByCount = 2324U;
inline constexpr std::uint32_t kStrStoreItemName = 2325U;
inline constexpr std::uint32_t kStrStoreItemNamePlural = 2326U;
inline constexpr std::uint32_t kStrStoreItemNameByCount = 2327U;
inline constexpr std::uint32_t kStrStorePartyName = 2330U;
inline constexpr std::uint32_t kStrStoreFactionName = 2335U;
inline constexpr std::uint32_t kStrStoreQuestName = 2336U;
inline constexpr std::uint32_t kStrStoreTroopNameLink = 2341U;
inline constexpr std::uint32_t kStrStorePartyNameLink = 2342U;
inline constexpr std::uint32_t kStrStoreFactionNameLink = 2343U;
inline constexpr std::uint32_t kStrStoreQuestNameLink = 2344U;
inline constexpr std::uint32_t kStrStorePlayerUsername = 2350U;
inline constexpr std::uint32_t kStrEncodeUrl = 2355U;
}  // namespace core_op

enum class NativeAxis : std::uint8_t {
    x = 0,
    y = 1,
    z = 2,
};

[[nodiscard]] inline Slots* native_entity_slots(wse::game::WseWorldState& world,
                                                std::uint32_t kind,
                                                std::int32_t id) noexcept {
    switch (kind) {
    case 0:
        return world.is_valid_troop(id) ? &world.troops[static_cast<std::size_t>(id)].slots
                                        : nullptr;
    case 1:
        return world.is_valid_party(id) ? &world.parties[static_cast<std::size_t>(id)].slots
                                        : nullptr;
    case 2:
        return world.is_valid_faction(id) ? &world.factions[static_cast<std::size_t>(id)].slots
                                          : nullptr;
    case 3:
        return world.is_valid_site(id) ? &world.sites[static_cast<std::size_t>(id)].slots
                                       : nullptr;
    case 4:
        return world.is_valid_party_template(id)
                   ? &world.party_templates[static_cast<std::size_t>(id)].slots
                   : nullptr;
    case 5:
        return world.is_valid_agent(id) ? &world.agents[static_cast<std::size_t>(id)].slots
                                        : nullptr;
    case 6:
        return world.is_valid_quest(id) ? &world.quests[static_cast<std::size_t>(id)].slots
                                        : nullptr;
    case 7:
        return world.is_valid_item(id) ? &world.items[static_cast<std::size_t>(id)].slots
                                       : nullptr;
    case 8:
        return world.is_valid_player(id) ? &world.players[static_cast<std::size_t>(id)].slots
                                         : nullptr;
    case 9:
        return world.is_valid_team(id) ? &world.teams[static_cast<std::size_t>(id)].slots
                                       : nullptr;
    case 10:
        return world.is_valid_mission_object(id)
                   ? &world.mission_objects[static_cast<std::size_t>(id)].slots
                   : nullptr;
    default:
        return nullptr;
    }
}

[[nodiscard]] constexpr bool native_slot_in_range(std::int32_t slot) noexcept {
    return slot >= 0 && slot <= wse::game::kMaxSlotIndex;
}

// Mirrors rglRand (0x14032ffb0): MSVC-compatible LCG using g_randSeed.
[[nodiscard]] inline std::int32_t native_rgl_rand(VmContext& ctx,
                                                  std::int32_t max_val) noexcept {
    if (max_val <= 1) {
        return 0;
    }
    ctx.randSeed = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(ctx.randSeed) * 0x343fdU + 0x269ec3U);
    return static_cast<std::int32_t>(
        ((static_cast<std::uint32_t>(ctx.randSeed) >> 16U) & 0x7fffU) %
        static_cast<std::uint32_t>(max_val));
}

inline void native_store_trig_deg(std::uint64_t dst,
                                  float angle_rad,
                                  VmContext& ctx,
                                  std::span<std::int64_t> locals) {
    constexpr float kRadToDeg = 57.29578F;  // mupRadToDeg (0x14006f9a0)
    const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
    const float deg = angle_rad * kRadToDeg;
    store_lvalue(dst, wse_trunc64(static_cast<double>(deg * fpScale)), ctx, locals);
}

inline void native_move_position(std::int32_t reg_idx,
                                 std::int64_t dist_cm,
                                 std::int64_t global_flag,
                                 NativeAxis axis,
                                 VmContext& ctx) noexcept {
    if (!wse_register_ok(reg_idx)) {
        return;
    }
    auto& pos = ctx.positions[static_cast<std::size_t>(reg_idx)];
    const float delta = static_cast<float>(dist_cm) / 100.0F;
    if (global_flag == 0) {
        const WseVec4& basis =
            (axis == NativeAxis::x) ? pos.s : ((axis == NativeAxis::y) ? pos.f : pos.u);
        pos.o.x += basis.x * delta;
        pos.o.y += basis.y * delta;
        pos.o.z += basis.z * delta;
    } else if (axis == NativeAxis::x) {
        pos.o.x += delta;
    } else if (axis == NativeAxis::y) {
        pos.o.y += delta;
    } else {
        pos.o.z += delta;
    }
}

inline void native_rotate_position(std::int32_t reg_idx,
                                   float angle_rad,
                                   std::int64_t global_flag,
                                   NativeAxis axis,
                                   VmContext& ctx) noexcept {
    if (!wse_register_ok(reg_idx)) {
        return;
    }
    auto& pos = ctx.positions[static_cast<std::size_t>(reg_idx)];
    if (global_flag == 0) {
        if (axis == NativeAxis::x) {
            pos.rotate_x(angle_rad);
        } else if (axis == NativeAxis::y) {
            pos.rotate_y(angle_rad);
        } else {
            pos.rotate_z(angle_rad);
        }
    } else {
        if (axis == NativeAxis::x) {
            pos.rotate_global_x(angle_rad);
        } else if (axis == NativeAxis::y) {
            pos.rotate_global_y(angle_rad);
        } else {
            pos.rotate_global_z(angle_rad);
        }
    }
}

[[nodiscard]] inline WseStringBanks native_string_banks(VmContext& ctx) noexcept {
    return WseStringBanks{ctx.stringRegisters, ctx.globalStrings, ctx.localStrings};
}

inline void native_store_count_name(std::int32_t reg_idx,
                                    std::string_view singular,
                                    std::string_view plural,
                                    std::int32_t count,
                                    VmContext& ctx) {
    const std::string_view selected = (count < 2) ? singular : plural;
    ctx.stringRegisters[static_cast<std::size_t>(reg_idx)] = wse_utf8_to_utf16(selected);
}

inline void native_store_name_link(std::int32_t reg_idx,
                                   std::string_view prefix,
                                   std::int32_t id,
                                   std::string_view text,
                                   VmContext& ctx) {
    std::string formatted;
    formatted.reserve(prefix.size() + text.size() + 20U);
    formatted.push_back('<');
    formatted.append(prefix);
    formatted.push_back('=');
    formatted.append(std::to_string(id));
    formatted.push_back('>');
    formatted.append(text);
    formatted.append("</>");
    ctx.stringRegisters[static_cast<std::size_t>(reg_idx)] = wse_utf8_to_utf16(formatted);
}

[[nodiscard]] inline NativeDispatch dispatch_native_entity_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline NativeDispatch dispatch_native_statement_ext(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    constexpr float kDegToRad = 0.01745329238474369F;  // mupDegToRad (0x14006f990)
    constexpr float kRadToDeg = 57.29578F;              // mupRadToDeg (0x14006f9a0)

    switch (opcode) {
    case core_op::kTroopSetSlot:
    case core_op::kPartySetSlot:
    case core_op::kFactionSetSlot:
    case core_op::kSceneSetSlot:
    case core_op::kPartyTemplateSetSlot:
    case core_op::kAgentSetSlot:
    case core_op::kQuestSetSlot:
    case core_op::kItemSetSlot:
    case core_op::kPlayerSetSlot:
    case core_op::kTeamSetSlot:
    case core_op::kScenePropSetSlot: {
        Slots* const slots =
            native_entity_slots(ctx.world, opcode - core_op::kTroopSetSlot, narrow[0]);
        if (slots != nullptr && native_slot_in_range(narrow[1])) {
            slots->set(narrow[1], wide[2]);
        }
        return NativeDispatch::handled;
    }

    case core_op::kTroopGetSlot:
    case core_op::kPartyGetSlot:
    case core_op::kFactionGetSlot:
    case core_op::kSceneGetSlot:
    case core_op::kPartyTemplateGetSlot:
    case core_op::kAgentGetSlot:
    case core_op::kQuestGetSlot:
    case core_op::kItemGetSlot:
    case core_op::kPlayerGetSlot:
    case core_op::kTeamGetSlot:
    case core_op::kScenePropGetSlot: {
        const Slots* const slots =
            native_entity_slots(ctx.world, opcode - core_op::kTroopGetSlot, narrow[1]);
        if (slots != nullptr && native_slot_in_range(narrow[2])) {
            store_lvalue(dst, slots->get(narrow[2]), ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;
    }

    case core_op::kTroopSlotEq:
    case core_op::kPartySlotEq:
    case core_op::kFactionSlotEq:
    case core_op::kSceneSlotEq:
    case core_op::kPartyTemplateSlotEq:
    case core_op::kAgentSlotEq:
    case core_op::kQuestSlotEq:
    case core_op::kItemSlotEq:
    case core_op::kPlayerSlotEq:
    case core_op::kTeamSlotEq:
    case core_op::kScenePropSlotEq: {
        const Slots* const slots =
            native_entity_slots(ctx.world, opcode - core_op::kTroopSlotEq, narrow[0]);
        return (slots != nullptr && native_slot_in_range(narrow[1]) &&
                slots->get(narrow[1]) == wide[2])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }

    case core_op::kTroopSlotGe:
    case core_op::kPartySlotGe:
    case core_op::kFactionSlotGe:
    case core_op::kSceneSlotGe:
    case core_op::kPartyTemplateSlotGe:
    case core_op::kAgentSlotGe:
    case core_op::kQuestSlotGe:
    case core_op::kItemSlotGe:
    case core_op::kPlayerSlotGe:
    case core_op::kTeamSlotGe:
    case core_op::kScenePropSlotGe: {
        const Slots* const slots =
            native_entity_slots(ctx.world, opcode - core_op::kTroopSlotGe, narrow[0]);
        return (slots != nullptr && native_slot_in_range(narrow[1]) &&
                slots->get(narrow[1]) >= wide[2])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }

    case core_op::kCopyPosition:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                ctx.positions[static_cast<std::size_t>(narrow[1])];
        }
        return NativeDispatch::handled;

    case core_op::kInitPosition:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] = WseTransform::identity();
        }
        return NativeDispatch::handled;

    case core_op::kGetAngleBetweenPositions:
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            WseTransform local =
                ctx.positions[static_cast<std::size_t>(narrow[1])].transform_to_local(
                    ctx.positions[static_cast<std::size_t>(narrow[2])]);
            local.orthonormalize();
            const float angle = wse::core::WseVec2{local.f.x, local.f.y}.get_angle();
            const float deg =
                (angle * static_cast<float>(ctx.fixedPointMultiplier)) * kRadToDeg;
            store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::int32_t>(wse_trunc64(deg))),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kGetDistanceBetweenPositions:
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const float dist = (ctx.positions[static_cast<std::size_t>(narrow[1])].o -
                                ctx.positions[static_cast<std::size_t>(narrow[2])].o)
                                   .length();
            store_lvalue(dst, wse_trunc64(static_cast<double>(dist * 100.0F)), ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kGetDistanceBetweenPositionsInMeters:
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const float dist = (ctx.positions[static_cast<std::size_t>(narrow[1])].o -
                                ctx.positions[static_cast<std::size_t>(narrow[2])].o)
                                   .length();
            store_lvalue(dst, wse_trunc64(static_cast<double>(dist)), ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionIsBehindPosition:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            const WseVec4 diff = ctx.positions[static_cast<std::size_t>(narrow[0])].o -
                                 ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            return ctx.positions[static_cast<std::size_t>(narrow[1])].f.dot(diff) < 0.0F
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;

    case core_op::kPositionTransformPositionToParent:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1]) &&
            wse_register_ok(narrow[2])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                ctx.positions[static_cast<std::size_t>(narrow[1])].transform_to_parent(
                    ctx.positions[static_cast<std::size_t>(narrow[2])]);
        }
        return NativeDispatch::handled;

    case core_op::kPositionTransformPositionToLocal:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1]) &&
            wse_register_ok(narrow[2])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                ctx.positions[static_cast<std::size_t>(narrow[1])].transform_to_local(
                    ctx.positions[static_cast<std::size_t>(narrow[2])]);
        }
        return NativeDispatch::handled;

    case core_op::kPositionCopyRotation:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].set_rotation(
                ctx.positions[static_cast<std::size_t>(narrow[1])]);
        }
        return NativeDispatch::handled;

    case core_op::kPositionCopyOrigin:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o;
        }
        return NativeDispatch::handled;

    case core_op::kPositionMoveX:
        native_move_position(narrow[0], wide[1], wide[2], NativeAxis::x, ctx);
        return NativeDispatch::handled;
    case core_op::kPositionMoveY:
        native_move_position(narrow[0], wide[1], wide[2], NativeAxis::y, ctx);
        return NativeDispatch::handled;
    case core_op::kPositionMoveZ:
        native_move_position(narrow[0], wide[1], wide[2], NativeAxis::z, ctx);
        return NativeDispatch::handled;

    case core_op::kPositionRotateX:
        native_rotate_position(narrow[0], static_cast<float>(wide[1]) * kDegToRad, wide[2],
                               NativeAxis::x, ctx);
        return NativeDispatch::handled;
    case core_op::kPositionRotateY:
        native_rotate_position(narrow[0], static_cast<float>(wide[1]) * kDegToRad, wide[2],
                               NativeAxis::y, ctx);
        return NativeDispatch::handled;
    case core_op::kPositionRotateZ:
        native_rotate_position(narrow[0], static_cast<float>(wide[1]) * kDegToRad, wide[2],
                               NativeAxis::z, ctx);
        return NativeDispatch::handled;

    case core_op::kPositionGetX:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].o.x),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case core_op::kPositionGetY:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].o.y),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case core_op::kPositionGetZ:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].o.z),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionSetX:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.x =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;
    case core_op::kPositionSetY:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.y =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;
    case core_op::kPositionSetZ:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.z =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;

    case core_op::kPositionRotateZFloating:
        native_rotate_position(
            narrow[0],
            (static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)) *
                kDegToRad,
            wide[2], NativeAxis::z, ctx);
        return NativeDispatch::handled;

    case core_op::kPositionGetScaleX:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].s.x),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case core_op::kPositionGetScaleY:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].f.y),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case core_op::kPositionGetScaleZ:
        if (wse_register_ok(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(static_cast<float>(ctx.fixedPointMultiplier) *
                                   ctx.positions[static_cast<std::size_t>(narrow[1])].u.z),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionRotateXFloating:
        native_rotate_position(
            narrow[0],
            (static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)) *
                kDegToRad,
            wide[2], NativeAxis::x, ctx);
        return NativeDispatch::handled;
    case core_op::kPositionRotateYFloating:
        native_rotate_position(
            narrow[0],
            (static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)) *
                kDegToRad,
            wide[2], NativeAxis::y, ctx);
        return NativeDispatch::handled;

    case core_op::kPositionGetRotationAroundZ:
        if (wse_register_ok(narrow[1])) {
            const auto& pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            const float angle = wse::core::WseVec2{pos.f.x, pos.f.y}.get_angle();
            const std::int64_t rounded = f2i_round(angle * kRadToDeg);
            store_lvalue(dst, (rounded + 360) % 360, ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionNormalizeOrigin:
        if (wse_register_ok(narrow[1])) {
            const float len =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o.normalize_with_length();
            store_lvalue(dst,
                         f2i_round(len * static_cast<float>(ctx.fixedPointMultiplier)),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionGetRotationAroundX:
        if (wse_register_ok(narrow[1])) {
            const auto& pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            const float len_xy = wse::core::WseVec2{pos.f.x, pos.f.y}.length();
            float angle = wse::core::WseVec2{len_xy, pos.f.z}.get_angle() + 1.5707964F;
            if (pos.u.z <= 0.0F) {
                angle = 3.1415927F - angle;
            }
            const std::int64_t rounded = f2i_round(angle * kRadToDeg);
            store_lvalue(dst, (rounded + 360) % 360, ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionGetRotationAroundY:
        if (wse_register_ok(narrow[1])) {
            const auto& pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            const float len_xy = wse::core::WseVec2{pos.s.x, pos.s.y}.length();
            float angle = wse::core::WseVec2{-len_xy, pos.s.z}.get_angle() - 1.5707964F;
            if (pos.u.z <= 0.0F) {
                angle = 3.1415927F - angle;
            }
            const std::int64_t rounded = f2i_round(angle * kRadToDeg);
            store_lvalue(dst, (rounded + 360) % 360, ctx, locals);
        }
        return NativeDispatch::handled;

    case core_op::kPositionSetScaleX:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].s.x =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;
    case core_op::kPositionSetScaleY:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].f.y =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;
    case core_op::kPositionSetScaleZ:
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].u.z =
                static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier);
        }
        return NativeDispatch::handled;

    case core_op::kDisplayMessage: {
        const auto banks = native_string_banks(ctx);
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            std::u16string parsed =
                wse_parse_string(wse_resolve_string(narrow[0], tag0, banks), ctx.registers, banks);
            const std::uint32_t color =
                (narrow[1] != 0) ? static_cast<std::uint32_t>(narrow[1]) : 0xffffffffU;
            ctx.world.displayed_messages.push_back({std::move(parsed), color});
        }
        return NativeDispatch::handled;
    }

    case core_op::kTroopIsHero:
        return (ctx.world.is_valid_troop(narrow[0]) &&
                ctx.world.troops[static_cast<std::size_t>(narrow[0])].is_hero())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    case core_op::kTroopSetHealth:
        if (ctx.world.is_valid_troop(narrow[0])) {
            auto& troop = ctx.world.troops[static_cast<std::size_t>(narrow[0])];
            const std::int32_t max_hp = std::max<std::int32_t>(
                0,
                troop.max_hit_points(
                    ctx.world.skills[static_cast<std::size_t>(wse::game::kSkillIronflesh)]
                        .max_level));
            if (wide[2] == 0) {
                const auto scaled =
                    static_cast<std::int32_t>(static_cast<float>(max_hp * narrow[1]) / 100.0F);
                troop.hit_points = static_cast<float>(std::clamp(scaled, 0, max_hp));
            } else {
                troop.hit_points = static_cast<float>(std::clamp(narrow[1], 0, max_hp));
            }
        }
        return NativeDispatch::handled;

    case core_op::kAgentIsHuman:
        return (ctx.world.is_valid_agent(narrow[0]) &&
                ctx.world.agents[static_cast<std::size_t>(narrow[0])].is_human())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    case core_op::kShuffleRange:
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1]) && narrow[0] < narrow[1]) {
            const std::int32_t span = narrow[1] - narrow[0];
            for (std::int32_t i = narrow[0]; i < narrow[1]; ++i) {
                const std::int32_t r = native_rgl_rand(ctx, span);
                std::swap(ctx.registers[static_cast<std::size_t>(i)],
                          ctx.registers[static_cast<std::size_t>(r + narrow[0])]);
            }
        }
        return NativeDispatch::handled;

    case core_op::kStoreRandom:
        store_lvalue(dst, static_cast<std::int64_t>(native_rgl_rand(ctx, narrow[1])), ctx, locals);
        return NativeDispatch::handled;

    case core_op::kStoreRandomInRange: {
        const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(narrow[2]) -
                                                    static_cast<std::uint32_t>(narrow[1]));
        store_lvalue(dst, static_cast<std::int64_t>(native_rgl_rand(ctx, span)) + wide[1], ctx,
                     locals);
        return NativeDispatch::handled;
    }

    case core_op::kStoreAsin:
        native_store_trig_deg(
            dst,
            std::asin(static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)),
            ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreAcos:
        native_store_trig_deg(
            dst,
            std::acos(static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)),
            ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreAtan:
        native_store_trig_deg(
            dst,
            std::atan(static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier)),
            ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreAtan2:
        native_store_trig_deg(
            dst,
            std::atan2(static_cast<float>(wide[1]) / static_cast<float>(ctx.fixedPointMultiplier),
                       static_cast<float>(wide[2]) / static_cast<float>(ctx.fixedPointMultiplier)),
            ctx, locals);
        return NativeDispatch::handled;

    case core_op::kStoreTroopGold:
        if (ctx.world.is_valid_troop(narrow[1])) {
            store_lvalue(dst, ctx.world.troops[static_cast<std::size_t>(narrow[1])].gold, ctx,
                         locals);
        }
        return NativeDispatch::handled;

    case core_op::kStoreTroopFaction:
        if (ctx.world.is_valid_troop(narrow[1])) {
            store_lvalue(dst, ctx.world.troops[static_cast<std::size_t>(narrow[1])].faction_no, ctx,
                         locals);
        }
        return NativeDispatch::handled;

    case core_op::kStoreFactionOfParty:
        if (ctx.world.is_valid_party(narrow[1])) {
            store_lvalue(dst, ctx.world.parties[static_cast<std::size_t>(narrow[1])].faction_no, ctx,
                         locals);
        }
        return NativeDispatch::handled;

    case core_op::kStrIsEmpty:
        return (wse_register_ok(narrow[0]) &&
                ctx.stringRegisters[static_cast<std::size_t>(narrow[0])].empty())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    case core_op::kStrClear:
        if (wse_register_ok(narrow[0])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])].clear();
        }
        return NativeDispatch::handled;

    case core_op::kStrStoreString: {
        const auto banks = native_string_banks(ctx);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (wse_register_ok(narrow[0]) && wse_string_register_ok(narrow[1], tag1, banks)) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_parse_string(wse_resolve_string(narrow[1], tag1, banks), ctx.registers, banks);
        }
        return NativeDispatch::handled;
    }

    case core_op::kStrStoreStringReg: {
        const auto banks = native_string_banks(ctx);
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] = wse_parse_string(
                ctx.stringRegisters[static_cast<std::size_t>(narrow[1])], ctx.registers, banks);
        }
        return NativeDispatch::handled;
    }

    case core_op::kStrStoreTroopName:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_troop(narrow[1])) {
            native_store_count_name(narrow[0],
                                    ctx.world.troops[static_cast<std::size_t>(narrow[1])].name,
                                    ctx.world.troops[static_cast<std::size_t>(narrow[1])].plural_name,
                                    1, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreTroopNamePlural:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_troop(narrow[1])) {
            native_store_count_name(narrow[0],
                                    ctx.world.troops[static_cast<std::size_t>(narrow[1])].name,
                                    ctx.world.troops[static_cast<std::size_t>(narrow[1])].plural_name,
                                    2, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreTroopNameByCount:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_troop(narrow[1])) {
            const auto& troop = ctx.world.troops[static_cast<std::size_t>(narrow[1])];
            native_store_count_name(narrow[0], troop.name, troop.plural_name, narrow[2], ctx);
        }
        return NativeDispatch::handled;

    case core_op::kStrStoreItemName:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_item(narrow[1])) {
            native_store_count_name(narrow[0],
                                    ctx.world.items[static_cast<std::size_t>(narrow[1])].name,
                                    ctx.world.items[static_cast<std::size_t>(narrow[1])].plural_name,
                                    1, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreItemNamePlural:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_item(narrow[1])) {
            native_store_count_name(narrow[0],
                                    ctx.world.items[static_cast<std::size_t>(narrow[1])].name,
                                    ctx.world.items[static_cast<std::size_t>(narrow[1])].plural_name,
                                    2, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreItemNameByCount:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_item(narrow[1])) {
            const auto& item = ctx.world.items[static_cast<std::size_t>(narrow[1])];
            native_store_count_name(narrow[0], item.name, item.plural_name, narrow[2], ctx);
        }
        return NativeDispatch::handled;

    case core_op::kStrStorePartyName:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_party(narrow[1])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(ctx.world.parties[static_cast<std::size_t>(narrow[1])].name);
        }
        return NativeDispatch::handled;

    case core_op::kStrStoreFactionName:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_faction(narrow[1])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(ctx.world.factions[static_cast<std::size_t>(narrow[1])].name);
        }
        return NativeDispatch::handled;

    case core_op::kStrStoreQuestName:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_quest(narrow[1])) {
            const auto banks = native_string_banks(ctx);
            const std::u16string raw =
                wse_utf8_to_utf16(ctx.world.quests[static_cast<std::size_t>(narrow[1])].title);
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_parse_string(raw, ctx.registers, banks);
        }
        return NativeDispatch::handled;

    case core_op::kStrStoreTroopNameLink:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_troop(narrow[1])) {
            native_store_name_link(narrow[0], "trp", narrow[1],
                                   ctx.world.troops[static_cast<std::size_t>(narrow[1])].name, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStorePartyNameLink:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_party(narrow[1])) {
            native_store_name_link(narrow[0], "p", narrow[1],
                                   ctx.world.parties[static_cast<std::size_t>(narrow[1])].name, ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreFactionNameLink:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_faction(narrow[1])) {
            native_store_name_link(narrow[0], "fac", narrow[1],
                                   ctx.world.factions[static_cast<std::size_t>(narrow[1])].name,
                                   ctx);
        }
        return NativeDispatch::handled;
    case core_op::kStrStoreQuestNameLink:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_quest(narrow[1])) {
            native_store_name_link(narrow[0], "qst", narrow[1],
                                   ctx.world.quests[static_cast<std::size_t>(narrow[1])].title, ctx);
        }
        return NativeDispatch::handled;

    case core_op::kStrStorePlayerUsername:
        if (wse_register_ok(narrow[0]) && ctx.world.is_valid_player(narrow[1])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(ctx.world.players[static_cast<std::size_t>(narrow[1])].name);
        }
        return NativeDispatch::handled;

    case core_op::kStrEncodeUrl:
        if (wse_register_ok(narrow[0])) {
            auto& sreg = ctx.stringRegisters[static_cast<std::size_t>(narrow[0])];
            sreg = wse_utf8_to_utf16(wse_url_escape(wse_utf16_to_utf8(sreg)));
        }
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_entity_statement(
        opcode, wide, narrow, tags, statement, ctx, locals);
}

[[nodiscard]] inline NativeDispatch dispatch_native_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    if (ctx.nativeHook != nullptr) {
        bool handled = false;
        const bool hookResult =
            ctx.nativeHook(ctx.nativeHookUser, opcode, wide, narrow, tags, statement, ctx, locals, handled);
        if (handled) {
            return hookResult ? NativeDispatch::handled : NativeDispatch::condition_false;
        }
    }

    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    constexpr float kDegToRad = 0.01745329238474369F;  // DAT_1405761e8

    switch (opcode) {
    case core_op::kGe:
        return wide[0] >= wide[1] ? NativeDispatch::handled : NativeDispatch::condition_false;
    case core_op::kEq:
        return wide[0] == wide[1] ? NativeDispatch::handled : NativeDispatch::condition_false;
    case core_op::kGt:
        return wide[0] > wide[1] ? NativeDispatch::handled : NativeDispatch::condition_false;
    case core_op::kIsBetween:
        return (wide[0] >= wide[1] && wide[0] < wide[2]) ? NativeDispatch::handled
                                                         : NativeDispatch::condition_false;
    case core_op::kValLshift: {
        const auto shifted = static_cast<std::uint64_t>(wide[0])
                             << (static_cast<unsigned>(wide[1]) & 63U);
        store_lvalue(dst, static_cast<std::int64_t>(shifted), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kValRshift:
        store_lvalue(dst, wide[0] >> (static_cast<unsigned>(wide[1]) & 63U), ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValAdd:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[0]) +
                                                    static_cast<std::uint64_t>(wide[1])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValSub:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[0]) -
                                                    static_cast<std::uint64_t>(wide[1])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValMul:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[0]) *
                                                    static_cast<std::uint64_t>(wide[1])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValDiv:
        store_lvalue(dst, checked_signed_div(wide[0], wide[1], ctx), ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValMod: {
        bool ok = false;
        const std::int64_t rem = checked_signed_mod(wide[0], wide[1], ctx, ok);
        if (ok) {
            store_lvalue(dst, rem, ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case core_op::kValMin:
        store_lvalue(dst, static_cast<std::int64_t>(std::min(narrow[0], narrow[1])), ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValMax:
        store_lvalue(dst, static_cast<std::int64_t>(std::max(narrow[0], narrow[1])), ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValClamp: {
        std::int32_t clamped = narrow[0];
        if (narrow[0] < narrow[1]) {
            clamped = narrow[1];
        } else if (narrow[0] >= narrow[2]) {
            clamped = static_cast<std::int32_t>(static_cast<std::uint32_t>(narrow[2]) - 1U);
        }
        store_lvalue(dst, static_cast<std::int64_t>(clamped), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kValAbs: {
        const auto absVal =
            wide[0] < 0 ? static_cast<std::int64_t>(-static_cast<std::uint64_t>(wide[0])) : wide[0];
        store_lvalue(dst, absVal, ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kValOr:
        store_lvalue(dst, wide[0] | wide[1], ctx, locals);
        return NativeDispatch::handled;
    case core_op::kValAnd:
        store_lvalue(dst, wide[0] & wide[1], ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreOr:
        store_lvalue(dst, wide[1] | wide[2], ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreAnd:
        store_lvalue(dst, wide[1] & wide[2], ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreMod: {
        bool ok = false;
        const std::int64_t rem = checked_signed_mod(wide[1], wide[2], ctx, ok);
        if (ok) {
            store_lvalue(dst, rem, ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case core_op::kStoreAdd:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[1]) +
                                                    static_cast<std::uint64_t>(wide[2])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreSub:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[1]) -
                                                    static_cast<std::uint64_t>(wide[2])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreMul:
        store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::uint64_t>(wide[1]) *
                                                    static_cast<std::uint64_t>(wide[2])),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kStoreDiv:
        store_lvalue(dst, checked_signed_div(wide[1], wide[2], ctx), ctx, locals);
        return NativeDispatch::handled;
    case core_op::kSetFixedPointMultiplier:
        if (narrow[0] > 0) {
            ctx.fixedPointMultiplier = narrow[0];
        }
        return NativeDispatch::handled;
    case core_op::kStoreSqrt:
        if (wide[1] > 0) {
            const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
            const float value = std::sqrt(static_cast<float>(wide[1]) / fpScale) * fpScale;
            store_lvalue(dst, f2i_round(value), ctx, locals);
        }
        return NativeDispatch::handled;
    case core_op::kStorePow: {
        const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
        const float value =
            std::pow(static_cast<float>(wide[1]) / fpScale, static_cast<float>(wide[2]) / fpScale) *
            fpScale;
        store_lvalue(dst, f2i_round(value), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kStoreSin: {
        const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
        const float value = std::sin((static_cast<float>(wide[1]) * kDegToRad) / fpScale) * fpScale;
        store_lvalue(dst, f2i_round(value), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kStoreCos: {
        const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
        const float value = std::cos((static_cast<float>(wide[1]) * kDegToRad) / fpScale) * fpScale;
        store_lvalue(dst, f2i_round(value), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kStoreTan: {
        const auto fpScale = static_cast<float>(ctx.fixedPointMultiplier);
        const float value = std::tan((static_cast<float>(wide[1]) * kDegToRad) / fpScale) * fpScale;
        store_lvalue(dst, f2i_round(value), ctx, locals);
        return NativeDispatch::handled;
    }
    case core_op::kConvertToFixedPoint:
        // Wraps like the retail imul instead of signed-overflow UB.
        store_lvalue(dst,
                     static_cast<std::int64_t>(
                         static_cast<std::uint64_t>(wide[0]) *
                         static_cast<std::uint64_t>(
                             static_cast<std::int64_t>(ctx.fixedPointMultiplier))),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kConvertFromFixedPoint:
        // set_fixed_point_multiplier only accepts positive values; the checked
        // divide also covers a host that zeroes the field directly.
        store_lvalue(dst,
                     checked_signed_div(
                         wide[0], static_cast<std::int64_t>(ctx.fixedPointMultiplier), ctx),
                     ctx, locals);
        return NativeDispatch::handled;
    case core_op::kAssign:
        store_lvalue(dst, wide[1], ctx, locals);
        return NativeDispatch::handled;
    default:
        return dispatch_native_statement_ext(opcode, wide, narrow, tags, statement, ctx, locals);
    }
}

[[nodiscard]] inline std::int32_t find_next_valid_party(const VmContext& ctx,
                                                        std::int32_t start) noexcept {
    const auto limit = static_cast<std::int32_t>(ctx.world.parties.size());
    for (std::int32_t idx = std::max<std::int32_t>(0, start); idx < limit; ++idx) {
        if (ctx.world.is_valid_party(idx)) {
            return idx;
        }
    }
    return limit;
}

[[nodiscard]] inline std::int32_t find_next_valid_agent(const Statement& stmt,
                                                        VmContext& ctx,
                                                        std::span<const std::int64_t> locals,
                                                        std::int32_t start,
                                                        bool reportPosError) {
    bool useDistance = false;
    std::int32_t posIdx = -1;
    float maxDist = 0.0f;
    if (stmt.numOperands > 1) {
        std::uint8_t unusedTag = 0;
        const auto reg = static_cast<std::int32_t>(parse_operand(
            static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag));
        if (static_cast<std::uint32_t>(reg) < 128U) {
            posIdx = reg;
            const std::int64_t rawRad =
                stmt.numOperands > 2
                    ? parse_operand(static_cast<std::uint64_t>(stmt.operands[2]), ctx, locals, unusedTag)
                    : 0;
            const float fpMul = ctx.fixedPointMultiplier != 0
                                    ? static_cast<float>(ctx.fixedPointMultiplier)
                                    : 100.0f;
            maxDist = static_cast<float>(rawRad) / fpMul;
            useDistance = true;
        } else if (reportPosError) {
            ctx.report_error("Invalid Register ID in try_for_agents");
        }
    }

    const auto limit = static_cast<std::int32_t>(ctx.world.agents.size());
    for (std::int32_t idx = std::max<std::int32_t>(0, start); idx < limit; ++idx) {
        if (!ctx.world.is_valid_agent(idx)) {
            continue;
        }
        if (useDistance) {
            const auto& origin = ctx.positions[static_cast<std::size_t>(posIdx)].o;
            const auto& apos = ctx.world.agents[static_cast<std::size_t>(idx)].position.o;
            const float dx = origin.x - apos.x;
            const float dy = origin.y - apos.y;
            const float dz = origin.z - apos.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxDist) {
                continue;
            }
        }
        return idx;
    }
    return limit;
}

[[nodiscard]] inline std::int32_t find_next_valid_prop(const Statement& stmt,
                                                       VmContext& ctx,
                                                       std::span<const std::int64_t> locals,
                                                       std::int32_t start) {
    std::uint8_t unusedTag = 0;
    const std::int32_t filterProp =
        stmt.numOperands > 1
            ? static_cast<std::int32_t>(parse_operand(
                  static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag))
            : 0;
    const std::int32_t filterMetaPlusOne =
        stmt.numOperands > 2
            ? static_cast<std::int32_t>(parse_operand(
                  static_cast<std::uint64_t>(stmt.operands[2]), ctx, locals, unusedTag))
            : 0;

    const auto limit = static_cast<std::int32_t>(ctx.world.mission_objects.size());
    for (std::int32_t idx = std::max<std::int32_t>(0, start); idx < limit; ++idx) {
        if (!ctx.world.is_valid_mission_object(idx)) {
            continue;
        }
        const auto& obj = ctx.world.mission_objects[static_cast<std::size_t>(idx)];
        if (filterProp > 0 && obj.scene_prop_no != filterProp) {
            continue;
        }
        if (filterMetaPlusOne > 0 && obj.meta_mesh_no != filterMetaPlusOne - 1) {
            continue;
        }
        return idx;
    }
    return limit;
}

[[nodiscard]] inline std::int32_t find_next_valid_player(const VmContext& ctx,
                                                         std::int32_t start) noexcept {
    const std::int32_t limit =
        std::min<std::int32_t>(ctx.world.max_players,
                               static_cast<std::int32_t>(ctx.world.players.size()));
    for (std::int32_t idx = std::max<std::int32_t>(0, start); idx < limit; ++idx) {
        if (ctx.world.is_valid_player(idx)) {
            return idx;
        }
    }
    return ctx.world.max_players;
}

}  // namespace wse::vm

#include "wse/vm/native_entity_ops.h"
