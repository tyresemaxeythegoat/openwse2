// Milestone 5 state extensions for agents, multiplayer/players, scenes/props,
// mission camera, shader params, and UI/presentations. Kept separate from
// world_state.h to preserve modular headers under the 1,300-line ceiling.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wse/core/math.h"

namespace wse::game {

struct WseAgentActionChannel {
    std::int32_t action_no = -1;
    float progress = 0.0f;
};

struct WseAgentOffer {
    std::int32_t agent_no = -1;
    float timeout = 0.0f;
};

struct WseAgentBoneEntry {
    bool valid = false;
    wse::core::WseTransform transform = wse::core::WseTransform::identity();
};

struct WseAgentM5Ext {
    wse::core::WseVec2 speed{};
    std::array<std::int32_t, 10> item_hit_points{};
    std::int32_t scripted_mode = 0;
    std::int32_t look_target_agent_no = -1;
    std::int32_t look_target_flags = 0;
    wse::core::WseVec4 look_target_pos{};
    bool invulnerable_shield = false;
    bool no_death_knockdown_only = false;
    float horse_speed_factor = 1.0f;
    std::int32_t combat_state = 0;
    std::int32_t walk_action_no = -1;
    std::array<WseAgentActionChannel, 2> action_channels{};
    std::int32_t attack_action = 0;
    std::int32_t attack_override_type = -1;
    std::int32_t attack_override_hold = 0;
    std::int32_t defend_action = 0;
    std::int32_t defend_override_type = -1;
    float defend_override_duration = 0.0f;
    float behavior_update_time = 0.0f;
    std::int32_t exit_status = 2;
    std::int32_t running_away_state = 0;
    bool running_away = false;
    std::int64_t exit_timer_start = 0;
    std::int64_t death_timer_start = 0;
    bool death_timer_active = false;
    std::int32_t behavior = 0;
    float aggressiveness = 0.5f;
    bool can_kick = true;
    bool no_dynamics = false;
    std::array<std::int32_t, 4> attached_bone_nos{};
    std::array<bool, 4> attached_use_bone_rot{};
    std::array<wse::core::WseVec4, 4> attached_offsets{};
    std::uint32_t ai_behavior_flags = 0;
    std::int32_t sound_channel = -1;
    bool is_crouching = false;
    bool ai_interact_with_player = true;
    std::int32_t ai_move_target_agent_no = -1;
    std::int32_t ai_behavior_target_agent_no = -1;
    bool ai_can_crouch = false;
    std::vector<std::int32_t> cached_enemies{};
    std::vector<WseAgentBoneEntry> bones{};
    std::int32_t last_inflicted_damage = 0;
    std::int32_t standing_mission_object_no = -1;
    std::vector<WseAgentOffer> offers{};

    void add_offer(std::int32_t offerer_agent_no,
                   float duration_s,
                   const std::array<std::int64_t, 3>& totals) {
        const float now = static_cast<float>(totals[2]) / 100000.0f;
        const float timeout = (duration_s == 0.0f) ? 100000.0f : (now + duration_s);
        for (auto& existing : offers) {
            if (existing.agent_no == offerer_agent_no) {
                existing.timeout = timeout;
                return;
            }
        }
        offers.push_back(WseAgentOffer{offerer_agent_no, timeout});
    }

    [[nodiscard]] bool has_offer(std::int32_t offerer_agent_no,
                                 const std::array<std::int64_t, 3>& totals) const noexcept {
        const float now = static_cast<float>(totals[2]) / 100000.0f;
        for (const auto& entry : offers) {
            if (entry.agent_no == offerer_agent_no) {
                return now <= entry.timeout;
            }
        }
        return false;
    }

    [[nodiscard]] float death_elapsed_seconds(
        const std::array<std::int64_t, 3>& totals) const noexcept {
        if (!death_timer_active) {
            return 0.0f;
        }
        return static_cast<float>(totals[2] - death_timer_start) / 100000.0f;
    }
};

struct WsePlayerM5Ext {
    bool is_busy_with_menus = false;
    bool spawn_items_saved = false;
    std::int32_t saved_spawn_items_value = 0;
    bool ready_to_spawn = false;
    std::int32_t spawn_entry_no = 0;
    bool is_muted_globally = false;
};

struct WseMissionObjectM5Ext {
    bool is_spawned_item = false;
    float prune_time = 0.0f;
    // Maps to `mbMissionObject +0x1d5` (zeroed in `initialize` `0x1401530f5`, set when `prune_time
    // > 0.0f` in `spawnMissionObject` `0x14013a018` / `setPruneTime` `0x140153f08`, and removed on
    // `clearScene` `0x14014801f` as well as frame prune expiration).
    bool enable_pruning = false;
    bool collision_enabled = true;
    bool fade_out = false;
    float fade_duration = 0.0f;
    float animate_rot_duration = 0.0f;
    bool is_rotating = false;
    float rotation = 0.0f;
    float total_rotation = 0.0f;
    float rotation_angle = 0.0f;
    float total_rotation_angle = 0.0f;
    float friction = 0.0f;
    float mass = 0.0f;
    wse::core::WseVec4 inertia{};
    wse::core::WseVec4 linear_velocity{};
    wse::core::WseVec4 angular_velocity{};
    wse::core::WseVec4 center_of_mass{};
    wse::core::WseVec4 last_impulse{};
    wse::core::WseVec4 bound_half_extents{0.5f, 0.5f, 0.5f, 0.0f};
    std::int32_t active_sound_no = -1;
    std::int32_t active_sound_flags = 0;
    std::int32_t sound_channel = -1;
    bool sound_playing = false;
    std::vector<std::int32_t> particle_system_nos{};
    float deform_time = 0.0f;
    std::int32_t deform_start_frame = 0;
    std::int32_t deform_end_frame = 0;
    float deform_duration = 0.0f;
    bool deform_loop = false;
    float deform_progress = 0.0f;
    std::string material_name{};
    std::int32_t material_submesh = -1;
};

// `WseOverlayM5Ext::anim_active` bits (one per `overlay_animate_to_*` opcode 932..937).
inline constexpr std::uint8_t kOverlayAnimColor = 0x01U;
inline constexpr std::uint8_t kOverlayAnimAlpha = 0x02U;
inline constexpr std::uint8_t kOverlayAnimHighlightColor = 0x04U;
inline constexpr std::uint8_t kOverlayAnimHighlightAlpha = 0x08U;
inline constexpr std::uint8_t kOverlayAnimSize = 0x10U;
inline constexpr std::uint8_t kOverlayAnimPosition = 0x20U;

struct WseOverlayM5Ext {
    std::int32_t mesh_no = -1;
    std::int32_t pressed_mesh_no = -1;
    std::int32_t tableau_material_no = -1;
    std::int32_t item_no = -1;
    std::int32_t container_overlay_no = -1;
    bool is_checkbox = false;
    // Text button overlays (`create_button_overlay` 912, `create_game_button_overlay` 940,
    // `create_in_game_button_overlay` 941) share `type == 1` with `create_text_overlay` 910;
    // this tells them apart for the UI layer (port bookkeeping, not a retail field).
    bool is_button = false;
    // Create opcode of a text button (912, 940, or 941): the three build different retail
    // widgets with different hit boxes (`game::UiSession::sync_overlays_from_world`).
    std::int32_t button_opcode = 0;
    // Create opcode of any overlay (910..948): the port collapses several retail widget classes
    // into one `type` (sliders and number boxes are 2, combo buttons and combo labels 5), so the
    // UI layer reads this to pick the retail widget's input behaviour (`rglWidget::getType`
    // values; port bookkeeping, not a retail field). 0 for overlays built outside the opcodes.
    std::int32_t create_opcode = 0;
    // `overlay_set_size` (925) was applied: game buttons (940/941) then take the size vector as
    // their box instead of their creation default (port bookkeeping, not a retail field).
    bool size_set = false;
    // `overlay_set_size` scale in force when the text was last set (creation or
    // `overlay_set_text` 920): `rglWidget::setText` (`0x14031f0f0`) re-measures a
    // `create_button_overlay` box at the text size current at that moment (0.02 x scale), so
    // the box follows the order of `overlay_set_size` and `overlay_set_text` (port bookkeeping).
    wse::core::WseVec2 text_measure_scale{1.0f, 1.0f};
    std::uint32_t widget_flags = 0;
    double min_value = 0.0;
    double max_value = 0.0;
    std::uint32_t highlight_color = 0xffffffffu;
    float highlight_alpha = 1.0f;
    wse::core::WseVec2 boundaries_min{};
    wse::core::WseVec2 boundaries_max{};
    wse::core::WseVec2 area_size{};
    wse::core::WseTransform mesh_transform = wse::core::WseTransform::identity();
    std::vector<std::u16string> items{};
    std::u16string tooltip{};
    bool focused = false;
    // A text box lost the focus to `overlay_obtain_focus` (949): retail `obtainFocus`
    // (`0x140318010`) calls the previous widget's `loseFocus` (`0x140317fe0`), which raises
    // `0x10000`; `UiSession::handle_input` reports it as `-62` (port bookkeeping).
    bool commit_pending = false;
    float additional_render_height = 0.0f;
    std::string material_name{};
    std::uint32_t anim_color = 0xffffffffu;
    float anim_color_duration = 0.0f;
    float anim_alpha = 1.0f;
    float anim_alpha_duration = 0.0f;
    std::uint32_t anim_highlight_color = 0xffffffffu;
    float anim_highlight_color_duration = 0.0f;
    float anim_highlight_alpha = 1.0f;
    float anim_highlight_alpha_duration = 0.0f;
    wse::core::WseVec2 anim_size{1.0f, 1.0f};
    float anim_size_duration = 0.0f;
    wse::core::WseVec2 anim_position{};
    float anim_position_duration = 0.0f;
    // Running `overlay_animate_to_*` state (`rglWidget::animateTo*` `0x14031d980`..
    // `0x14031dac0` capture the current value as the start and the time; `rglWidget::frameMove`
    // `0x14031ced0` interpolates). Bits of `anim_active`: `kOverlayAnim*`.
    std::uint8_t anim_active = 0;
    std::uint32_t anim_color_start = 0;
    float anim_alpha_start = 1.0f;
    std::uint32_t anim_highlight_color_start = 0;
    float anim_highlight_alpha_start = 1.0f;
    wse::core::WseVec2 anim_size_start{1.0f, 1.0f};
    wse::core::WseVec2 anim_position_start{};
    // Seconds since each animation started: colour, alpha, highlight colour, highlight
    // alpha, size, position (the `kOverlayAnim*` bit order).
    std::array<float, 6> anim_elapsed{};
};

struct WseMissionCameraState {
    std::int32_t mode = 0;
    std::int32_t target_agent_no = -1;
    bool target_agent_use_rotation = false;
    std::int32_t animation_no = -1;
    bool non_linear = false;
    std::uint32_t screen_color = 0x00000000u;
    std::uint32_t screen_color_target = 0x00000000u;
    float screen_color_duration = 0.0f;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    wse::core::WseTransform target_position = wse::core::WseTransform::identity();
    float position_duration = 0.0f;
    float aperture = 60.0f;
    float target_aperture = 60.0f;
    float aperture_duration = 0.0f;
};

struct WseShaderParamValue {
    enum class Kind : std::uint8_t { integer = 0, scalar = 1, vec4 = 2, mat4 = 3 };
    Kind kind = Kind::integer;
    std::int32_t int_val = 0;
    float float_val = 0.0f;
    std::array<float, 16> matrix_val{};
};

struct WseNetClientEvent {
    std::int32_t event_type = 0;
    std::int32_t num_ints = 0;
    std::array<std::int64_t, 4> ints{};
    std::u16string text{};
};

struct WseAttachedMissile {
    std::int32_t mission_object_no = -1;
    std::int32_t item_no = -1;
};

struct WseM5WorldExt {
    // Weather & fog
    std::int32_t rain_type = 0;
    float rain_strength = 0.0f;
    std::int32_t rain_strength_percent = 0;
    std::int32_t precipitation_type = 0;
    float fog_distance = 0.0f;
    std::uint32_t fog_color = 0u;
    std::array<std::uint32_t, 32> skybox_fog_colors{};

    // Mission camera, attached missiles & shader params
    WseMissionCameraState mission_camera{};
    std::vector<WseAttachedMissile> attached_missiles{};
    std::vector<std::pair<std::string, WseShaderParamValue>> shader_params{};
    bool scene_editor_open = false;

    // UI cursor, overlays container, dialog/message box & screen queue
    wse::core::WseVec2 cursor_pos{};
    std::int32_t current_container_overlay_no = -1;
    std::int32_t focused_overlay_no = -1;
    bool yes_no_window_open = false;
    bool yes_no_is_question = false;
    std::u16string dialog_title{};
    std::u16string dialog_text{};
    std::u16string dialog_yes_text{};
    std::u16string dialog_no_text{};
    std::u16string message_box_text{};
    std::uint32_t message_box_color = 0xffffffffu;
    std::uint32_t message_box_flags = 0u;
    bool message_box_center_justify = false;
    std::string message_box_bg_mesh{};
    std::vector<std::pair<std::int32_t, std::int32_t>> enqueued_windows{};
    std::int32_t active_window = 0;
    std::int32_t screen_entry_point_no = -1;
    std::int32_t screen_note_kind = 0;
    std::int32_t screen_note_object = -1;
    bool quit_requested = false;
    std::int32_t loading_mode = 0;
    std::int32_t loading_savegame_no = -1;

    // Multiplayer server settings & client->server events
    std::vector<WseNetClientEvent> client_events{};
    bool allow_renaming_server = true;
    bool allow_changing_game_type = true;
    std::int32_t server_combat_speed = 2;
    std::int32_t server_friendly_fire = 0;
    std::int32_t server_control_block_dir = 0;
    std::int32_t server_add_to_servers_list = 1;
    std::int32_t server_ghost_mode = 0;
    std::int32_t server_melee_friendly_fire = 0;
    std::int32_t server_ff_self_ratio = 0;
    std::int32_t server_ff_friend_ratio = 100;
    std::int32_t server_anti_cheat = 0;
    std::int32_t max_private_players = 250;
    wse::core::WseVec2 message_box_position{};
    wse::core::WseVec2 message_box_size{};
    std::int32_t num_sounds = 0;
};

}  // namespace wse::game
