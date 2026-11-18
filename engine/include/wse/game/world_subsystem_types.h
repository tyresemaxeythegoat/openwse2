#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/world_m5_types.h"

namespace wse::game {

inline constexpr std::int32_t kMaxRawKeys = 256;               // 0x100 (0x1401c6981)
inline constexpr std::int32_t kMaxGameKeys = 95;               // 0x5f (0x1401c68ec)
inline constexpr std::int32_t kNumFaceMorphKeys = 42;          // 0x2a (0x1401e158e)
inline constexpr std::int32_t kMaxSpawnEffectorSlots = 20;     // 0x14 (0x1401cba2c)
inline constexpr std::int32_t kMaxTalkInfoLines = 5;           // 5 (0x1401dbef5)
inline constexpr std::size_t kMaxContextMenuItems = 100;       // 100 (0x1401db9d7)
inline constexpr std::size_t kMaxPointLights = 256;
inline constexpr std::size_t kMaxParticleEvents = 1024;
inline constexpr std::size_t kMaxPartyParticleSystems = 64;
inline constexpr std::size_t kMaxTableauItems = 256;
inline constexpr std::size_t kMaxTableauOverrideItems = 128;
inline constexpr std::size_t kMaxBans = 512;
inline constexpr std::size_t kMaxUrlRequests = 128;
inline constexpr std::size_t kMaxSpawnedMissionObjects = 8192;
inline constexpr std::size_t kMaxSpawnedAgents = 8192;
inline constexpr std::size_t kMaxRayObstacles = 1024;
inline constexpr std::size_t kMaxReinforcementEvents = 512;
inline constexpr std::size_t kMaxAiMeshFaces = 4096;

// Shared perspective scales for `750 position_get_screen_projection` and `751
// mouse_get_world_projection`, so projecting then unprojecting round-trips: vertical
// field-of-view `tan(fov/2)` (fov clamped to [1, 179] degrees; safety deviation: a
// non-finite fov falls back to 60) and width/height aspect (4:3 when the viewport size
// is unknown).
struct CameraProjectionScales {
    float tan_half_fov = 0.0f;
    float aspect = 4.0f / 3.0f;
};

[[nodiscard]] inline CameraProjectionScales camera_projection_scales(
    float fov_deg, float viewport_width, float viewport_height) noexcept {
    CameraProjectionScales s{};
    const float fov = std::isfinite(fov_deg) ? std::clamp(fov_deg, 1.0f, 179.0f) : 60.0f;
    s.tan_half_fov = std::tan(fov * (3.14159265358979323846f / 360.0f));
    if (std::isfinite(viewport_width) && std::isfinite(viewport_height) && viewport_width > 0.0f &&
        viewport_height > 0.0f) {
        s.aspect = viewport_width / viewport_height;
    }
    return s;
}

// Safety deviation: float results convert with saturation (NaN -> 0) instead of UB.
[[nodiscard]] inline std::int64_t native_f2i_saturate(float value) noexcept {
    if (std::isnan(value)) {
        return 0;
    }
    constexpr float kLimit = 9.2233715e18F;  // largest float below 2^63
    if (value >= kLimit) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= -kLimit) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] constexpr std::int64_t saturating_add_i64(std::int64_t a,
                                                        std::int64_t b) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return a + b;
}

template <typename T>
inline void push_bounded(std::vector<T>& vec, T value, std::size_t cap) {
    if (vec.size() >= cap) {
        return;
    }
    vec.push_back(std::move(value));
}

// 256-bit Warband face key bitfield (mbFaceKeys::toString / fromString, 0x1400c5e70 / 0x1400c5f20).
struct WseFaceKeys {
    std::array<std::uint64_t, 4> keys{};

    [[nodiscard]] std::string to_string() const {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (std::size_t w = 0; w < 4; ++w) {
            std::uint64_t v = keys[w];
            for (std::size_t d = 0; d < 16; ++d) {
                out[w * 16 + (15 - d)] = kHex[v & 0xfULL];
                v >>= 4U;
            }
        }
        return out;
    }

    // Mirrors 0x1400c5f20 gated on exact 64-character strings.
    [[nodiscard]] bool from_string(std::string_view hex) noexcept {
        if (hex.size() != 64U) {
            keys.fill(0ULL);
            return false;
        }
        auto nibble = [](char ch) noexcept -> std::uint64_t {
            if (ch >= '0' && ch <= '9') {
                return static_cast<std::uint64_t>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f') {
                return static_cast<std::uint64_t>(10 + (ch - 'a'));
            }
            if (ch >= 'A' && ch <= 'F') {
                return static_cast<std::uint64_t>(10 + (ch - 'A'));
            }
            return 0ULL;
        };
        for (std::size_t w = 0; w < 4; ++w) {
            std::uint64_t acc = 0ULL;
            for (std::size_t d = 0; d < 16; ++d) {
                acc = (acc << 4U) | nibble(hex[w * 16 + d]);
            }
            keys[w] = acc;
        }
        return true;
    }

    [[nodiscard]] constexpr std::int32_t get_bits6(std::uint32_t shift) const noexcept {
        return static_cast<std::int32_t>((keys[0] >> shift) & 0x3fULL);
    }

    constexpr void set_bits6(std::uint32_t shift, std::int32_t value) noexcept {
        const std::uint64_t mask = 0x3fULL << shift;
        keys[0] = (keys[0] & ~mask) |
                  (static_cast<std::uint64_t>(static_cast<std::uint32_t>(value) & 0x3fU) << shift);
    }

    [[nodiscard]] constexpr std::int32_t get_morph_key(std::int32_t key_no) const noexcept {
        if (key_no < 0 || key_no >= kNumFaceMorphKeys) {
            return 0;
        }
        const auto word = static_cast<std::size_t>(key_no / 21 + 1);
        const auto shift = static_cast<std::uint32_t>((key_no % 21) * 3);
        return static_cast<std::int32_t>((keys[word] >> shift) & 0x7ULL);
    }

    constexpr void set_morph_key(std::int32_t key_no, std::int32_t value) noexcept {
        if (key_no < 0 || key_no >= kNumFaceMorphKeys) {
            return;
        }
        const auto word = static_cast<std::size_t>(key_no / 21 + 1);
        const auto shift = static_cast<std::uint32_t>((key_no % 21) * 3);
        const std::uint64_t mask = 0x7ULL << shift;
        keys[word] =
            (keys[word] & ~mask) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(value) & 0x7U) << shift);
    }
};

struct WseNoteEntry {
    std::u16string text{};
    std::int32_t date = -1;
    bool available = false;
};

struct WseInfoPage {
    std::string id{};
    std::string title{};
    std::string text{};
    bool note_available = true;
    std::int32_t note_tableau_material_no = -1;
    std::array<WseNoteEntry, 16> notes{};
};

struct WseInputState {
    std::array<bool, kMaxRawKeys> key_down{};
    std::array<bool, kMaxRawKeys> key_clicked{};
    std::array<bool, kMaxRawKeys> omitted_keys{};
    std::array<bool, kMaxGameKeys> game_key_down{};
    std::array<bool, kMaxGameKeys> game_key_clicked{};
    std::array<std::string, kMaxGameKeys> game_key_names{};
    std::array<std::string, kMaxGameKeys> game_key_alt_names{};
    // Port bookkeeping: `WseGameKeyManager::revision()` last copied into the name /
    // mapping tables (`0` = never), so unchanged bindings are not re-formatted per frame.
    std::uint64_t bindings_revision = 0U;
    // Slice P8: two-finger campaign map pinch zoom impulse (>0 zoom in, <0 zoom out).
    float map_zoom_delta = 0.0f;
    // mbMapWindow constructor (`0x140116ed0`) initializes +0x2d0 (`m_cameraDistance`) to 5.0f;
    // rglConfig::Campaign bounds `fMapCameraMinZoom` (`0x1406183c0`) = 3.5f and
    // `fMapCameraMaxZoom` (`0x1406183c4`) = 175.0f; `WorldInputBridge` scales by
    // `exp(-map_zoom_delta)` and clamps to `[kMinMapCameraZoom, kMaxMapCameraZoom]`.
    static constexpr float kDefaultMapCameraDistance = 5.0f;
    static constexpr float kMinMapCameraZoom = 3.5f;
    static constexpr float kMaxMapCameraZoom = 175.0f;
    float map_camera_distance = kDefaultMapCameraDistance;
    // `WseInputManager::frame_counter()` of the frame whose zoom was last applied, so
    // calling `WorldInputBridge::sync_to_world` twice for one frame zooms only once.
    std::uint64_t map_zoom_applied_frame = 0U;
};

struct WseMusicState {
    std::int32_t current_track_no = -1;
    std::int32_t track_options = 0;
    std::int32_t cue_track_no = -1;
    std::uint32_t situation_flags = 0;
    std::uint32_t culture_flags = 0;
    bool all_sounds_stopped = false;
    std::int32_t last_sound_channel = -1;
    std::vector<bool> stopped_channels{};
};

struct WsePointLightEntry {
    std::int32_t caller = 0;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float flicker_magnitude = 0.0f;
    float flicker_interval = 0.0f;
    float specular_coefficient = 0.0f;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    bool to_entity = false;
};

struct WseParticleSystemEntry {
    std::int32_t kind_no = -1;
    std::int32_t attach_type = 2;
    std::int32_t caller = 0;
    wse::core::WseTransform transform = wse::core::WseTransform::identity();
    std::int32_t burst_strength = 0;
    std::int32_t emit_amount = 0;
    bool synced = true;
};

struct WseTableauRenderItem {
    enum class Kind : std::uint8_t { mesh, tableau_mesh, map_icon, troop, horse };
    Kind kind = Kind::mesh;
    std::int32_t id = -1;
    std::int32_t param = 0;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    wse::core::WseVec4 scale{1.0f, 1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    std::uint32_t vertex_color = 0xffffffffu;
    std::int32_t override_horse_item_no = -1;
};

struct WseTableauLight {
    bool is_sun = false;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct WseTableauState {
    bool active = false;
    std::int32_t current_material_no = -1;
    bool render_as_alpha_mask = false;
    std::uint32_t background_color = 0u;
    float ambient_r = 0.0f;
    float ambient_g = 0.0f;
    float ambient_b = 0.0f;
    wse::core::WseTransform camera_position = wse::core::WseTransform::identity();
    bool camera_orthogonal = false;
    float camera_fov_or_width = 0.0f;
    float camera_aspect = 0.0f;
    float camera_near = 0.0f;
    float camera_far = 0.0f;
    std::uint32_t override_flags = 0;
    std::vector<std::int32_t> override_items{};
    std::vector<WseTableauLight> lights{};
    std::vector<WseTableauRenderItem> items{};
};

struct WseAiMeshFace {
    std::int32_t group_id = 0;
    bool visible = true;
};

struct WseMissileSpawnEntry {
    std::int32_t shooter_agent_no = -1;
    wse::core::WseTransform launch_pos = wse::core::WseTransform::identity();
    std::int32_t item_no = -1;
    std::int32_t item_modifier = 0;
    std::int32_t missile_item_no = -1;
    std::int32_t missile_modifier = 0;
    float speed = 0.0f;
};

struct WseReinforcementEvent {
    std::int32_t entry_group_no = 0;
    std::int32_t num_troops = 0;
};

struct WseProfileState {
    std::int32_t banner_no = -1;
    WseFaceKeys face_keys{};
};

struct WseBanInfo {
    bool valid = false;
    std::int32_t player_no = -1;
    std::int32_t unique_id = 0;
    std::string name{};
    bool permanent = false;
};

struct WseBanRecord {
    std::int32_t player_no = -1;
    std::int32_t unique_id = 0;
    std::string name{};
    bool permanent = false;
    std::int32_t banner_player_no = -1;
};

struct WseContextMenuEntry {
    std::string text{};
    std::int32_t value = 0;
};

struct WseRayObstacle {
    bool is_prop = true;
    std::int32_t object_no = -1;
    std::uint32_t mask = 0xffffffffu;
    wse::core::WseVec4 min_bound{};
    wse::core::WseVec4 max_bound{};
};

struct WseLightingState {
    std::int32_t skybox_no = -1;
    std::int32_t hdr_skybox_no = -1;
    float cloud_amount = 0.0f;
    float haze_amount = 0.0f;
    bool has_scene = true;
    bool has_ambient_light = true;
    wse::core::WseVec4 sun_color{1.0f, 1.0f, 1.0f, 1.0f};
    wse::core::WseVec4 ambient_color{0.5f, 0.5f, 0.5f, 1.0f};
    wse::core::WseVec4 ground_ambient_color{0.3f, 0.3f, 0.3f, 1.0f};
    bool shadow_map_needs_restore = false;
    bool river_shader_mud = false;
    std::int32_t active_postfx_no = -1;
};

struct WseItemTooltipRequest {
    bool visible = false;
    std::int32_t item_no = -1;
    std::int32_t modifier = 0;
    wse::core::WseVec2 screen_pos{};
    std::int32_t show_value_mode = 0;
};

struct WseTroopTooltipRequest {
    bool visible = false;
    std::int32_t troop_no = -1;
    wse::core::WseVec2 screen_pos{};
    std::int32_t mode = 0;
};

struct WseUrlRequestRecord {
    std::string url{};
    bool raw = false;
};

struct WseDialogState {
    bool map_conversation_active = false;
    std::int32_t map_conversation_troop_no = -1;
    std::int32_t map_conversation_troop_dna = 5;
    std::int32_t speaker_troop_no = -1;
    std::int32_t speaker_agent_no = -1;
    std::string speaker_text{};
    std::int32_t pending_mission_agent_no = -1;
    bool show_talk_info = false;
    std::int32_t talk_info_relation_value = 0;
    std::array<std::string, kMaxTalkInfoLines> talk_info_lines{};
};

struct WseUiPanelState {
    bool show_object_details_overlay = true;
    std::int32_t current_menu_no = -1;
    bool current_menu_option_disabled = false;
    std::string background_mesh_name{};
    std::int32_t menu_tableau_material_no = -1;
    float menu_tableau_param = 0.0f;
    wse::core::WseVec4 menu_tableau_bounds{};
    std::string tooltip_text{};
    WseItemTooltipRequest item_tooltip{};
    WseTroopTooltipRequest troop_tooltip{};
    std::vector<WseContextMenuEntry> context_menu_items{};
    std::int32_t exchange_party_no = -1;
    bool exchange_window_open = false;
};

struct WseSceneFxState {
    wse::core::WseVec4 trigger_current_color{1.0f, 1.0f, 1.0f, 1.0f};
    wse::core::WseVec4 trigger_position_delta{};
    wse::core::WseTransform trigger_transform = wse::core::WseTransform::identity();
    std::int32_t trigger_agent_no = -1;
    std::int32_t trigger_bone_no = -1;
    std::int32_t cur_item_tableau_material_no = -1;
    std::int32_t cur_item_tableau_param = 0;
    std::int32_t cur_scene_prop_tableau_material_no = -1;
    std::int32_t cur_scene_prop_tableau_param = 0;
    std::int32_t cur_map_icon_tableau_material_no = -1;
    std::int32_t cur_map_icon_tableau_param = 0;
    std::array<std::int32_t, 8> spawn_effector_scene_prop_kinds{
        -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<std::int32_t, kMaxSpawnEffectorSlots> spawn_effector_mission_object_nos{
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<WsePointLightEntry> point_lights{};
    std::vector<WseParticleSystemEntry> particle_events{};
    std::vector<WseAiMeshFace> ai_mesh_faces{};
    std::vector<WseMissileSpawnEntry> missiles{};
    std::vector<WseReinforcementEvent> reinforcement_events{};
    std::vector<WseRayObstacle> ray_obstacles{};
};

struct WseMpSessionState {
    std::string server_name{};
    std::string server_password{};
    std::string welcome_message{};
    WseBanInfo saved_ban{};
    std::vector<WseBanRecord> banned_players{};
    std::vector<WseUrlRequestRecord> url_requests{};
    std::vector<WseProfileState> profiles{};
    std::int32_t current_profile_no = -1;
    bool mission_starting = false;
    bool started_manually = false;
    std::int32_t next_site_no = -1;
    std::int32_t next_mission_template_no = -1;
    std::uint32_t mission_seed = 0;
};

struct WseTeamDivisionSubOrders {
    std::array<std::int32_t, 9> hold_fire{};
    std::array<std::int32_t, 9> riding{};
    std::array<std::int32_t, 9> weapon_usage{};
    std::array<std::int32_t, 9> gap_distance{};
};

struct WseQuestStrings {
    std::string text{};
    std::string giver_name{};
};

struct WseMiscRuntimeState {
    std::int32_t num_music_tracks = 0;
    std::int32_t num_map_icons = 0;
    std::int32_t num_tableau_materials = 0;
    std::int32_t num_scene_props = 0;
    std::int32_t num_actions = 0;
    std::int32_t num_menus = 0;
    std::int32_t num_post_effects = 0;
    std::int32_t num_sound_channels = 64;
    std::int32_t mercenary_source_party_no = -1;
    std::int32_t passage_menu_no = -1;
    std::int32_t save_mode_flags = 0;
    bool meta_mission_end_committed = true;
    bool meta_mission_event_ended = false;
    bool camera_first_person = false;
    wse::core::WseTransform camera_transform = wse::core::WseTransform::identity();
    float camera_fov_deg = 60.0f;
    float viewport_width = 1024.0f;
    float viewport_height = 768.0f;
    float physics_delta_time = 0.025f;
    bool ironman_allowed = false;
    float merchandise_modifier_quality = 0.0f;  // percent / 100
    bool encounter_left = false;                // leave_encounter (1301)
    std::int32_t rest_hours = 0;
    std::int32_t rest_time_speed = 0;
    bool rest_interactive = false;
    std::array<std::int64_t, 16> trigger_params{};
    std::int64_t repeat_object = 0;
    std::int32_t auto_save_count = 0;
    float difficulty_time_accum = 0.0f;
    float difficulty_last_timer = 0.0f;
    float average_game_difficulty = 0.0f;
    float battle_simulation_time = 0.0f;
    std::int32_t camp_bandits_party_no = -1;
    std::unordered_map<std::int32_t, WseQuestStrings> quest_strings{};
};

// mbQueuedWindow (8 B) entry types popped by `mbMapWindow::frameMove` (`0x1401180f0`, pop at
// `0x140118ae7`): 0x12 opens `mbMenuWindow` (`0x12`, whose `open` `0x1401203a0` clears the rest of
// the queue, and which halts odd simple-trigger passes at `0x140210352..0x140210377`); 0x0f opens
// presentation window `0x16` (`start_presentation` `0x1401cc082`); 0x10 opens notes window `0x15`
// (`change_screen_notes` `0x1401dc288`). Other queued types stay unpopped on map frames.
inline constexpr std::int32_t kQueuedWindowPresentationType = 0x0f;
inline constexpr std::int32_t kQueuedWindowNotesType = 0x10;
inline constexpr std::int32_t kQueuedWindowMenuType = 0x12;
// Safety deviation (no retail limit): `WseWorldState::enqueue_window` drops the oldest entry
// beyond this many pending transitions.
inline constexpr std::size_t kMaxQueuedWindows = 64;

// Window IDs allocated by `mbGameScreen::createWindows` (`0x1400ef870`) / checked by UI opcodes.
inline constexpr std::int32_t kScreenWindowLoading = 3;
inline constexpr std::int32_t kScreenWindowMap = 6;
inline constexpr std::int32_t kScreenWindowTactical = 10;
inline constexpr std::int32_t kScreenWindowConversation = 12;
inline constexpr std::int32_t kScreenWindowMenu = 0x12;
inline constexpr std::int32_t kScreenWindowNotes = 0x15;
inline constexpr std::int32_t kScreenWindowPresentation = 0x16;

// mbTrack (0x5c B) savegame fields serialized by saveTracks (0x1400ca580) / loadTracks (0x1400c9240).
struct WseSavegameTrack {
    wse::core::WseVec4 position{};
    float rotation_z = 0.0f;
    float age = 0.0f;
    std::uint32_t flags = 0;
};

// mbEvent (152 B) fields serialized by mbEvent::writeStream (0x1400bb0f0) / readStream (0x1400bafd0).
struct WseSavegameEvent {
    std::string extra{};
    std::int32_t active = 0;
    wse::core::WseVec4 position{};
    std::int32_t party_1_no = -1;
    std::int32_t party_2_no = -1;
    std::int64_t timer_value = 0;
    float duration = 0.0f;
};

}  // namespace wse::game
