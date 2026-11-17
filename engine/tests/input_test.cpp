// Host and Android unit tests for Slice P7 Input, Warband key/game-key mapping,
// context-sensitive touch overlay, layout editor, per-module profiles, free
// camera viewer control, and VM/world input integration.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/core/math.h"
#include "wse/game/world_input.h"
#include "wse/platform/input.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_brf.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

[[nodiscard]] bool close(float a, float b, float eps = 1e-4f) noexcept {
    return std::fabs(a - b) <= eps;
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) noexcept {
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56) |
           (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(1, static_cast<std::uint64_t>(idx)));
}

wse::vm::Statement make_stmt(std::uint32_t opcode,
                             std::initializer_list<std::int64_t> args) {
    wse::vm::Statement stmt{};
    stmt.opcode = opcode;
    stmt.numOperands = static_cast<std::int32_t>(args.size());
    std::size_t idx = 0;
    for (std::int64_t arg : args) {
        if (idx < wse::vm::kMaxOperands) {
            stmt.operands[idx++] = arg;
        }
    }
    return stmt;
}

bool run_stmt(wse::vm::VmContext& ctx,
              std::uint32_t opcode,
              std::initializer_list<std::int64_t> args) {
    std::vector<std::int64_t> locals(32, 0);
    const auto stmt = make_stmt(opcode, args);
    return wse::vm::execute_statement(stmt, 0, ctx, locals);
}

std::vector<std::byte> build_bc1_dds(std::uint16_t color0_565,
                                     std::uint16_t color1_565,
                                     std::uint32_t indices_u32) {
    std::vector<std::byte> buf(136U, std::byte{0});
    const auto w16 = [&](std::size_t off, std::uint16_t v) {
        buf[off + 0U] = static_cast<std::byte>(v & 0xFFU);
        buf[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
    };
    const auto w32 = [&](std::size_t off, std::uint32_t v) {
        buf[off + 0U] = static_cast<std::byte>(v & 0xFFU);
        buf[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
        buf[off + 2U] = static_cast<std::byte>((v >> 16U) & 0xFFU);
        buf[off + 3U] = static_cast<std::byte>((v >> 24U) & 0xFFU);
    };
    buf[0] = std::byte{'D'};
    buf[1] = std::byte{'D'};
    buf[2] = std::byte{'S'};
    buf[3] = std::byte{' '};
    w32(4U, 124U);
    w32(8U, 0x1007U);
    w32(12U, 4U);
    w32(16U, 4U);
    w32(20U, 8U);
    w32(76U, 32U);
    w32(80U, 0x4U);
    buf[84] = std::byte{'D'};
    buf[85] = std::byte{'X'};
    buf[86] = std::byte{'T'};
    buf[87] = std::byte{'1'};
    w32(108U, 0x1000U);
    w16(128U, color0_565);
    w16(130U, color1_565);
    w32(132U, indices_u32);
    return buf;
}

wse::asset::BrfMesh build_test_quad_mesh() {
    wse::asset::BrfMesh mesh{};
    mesh.name = "p7_viewer_quad";
    mesh.positions = {
        {-0.6f, -0.6f, 0.0f},
        {0.6f, -0.6f, 0.0f},
        {0.6f, 0.6f, 0.0f},
        {-0.6f, 0.6f, 0.0f},
    };
    const std::array<wse::asset::Vec2f, 4> uvs{
        wse::asset::Vec2f{0.0f, 1.0f},
        wse::asset::Vec2f{1.0f, 1.0f},
        wse::asset::Vec2f{1.0f, 0.0f},
        wse::asset::Vec2f{0.0f, 0.0f},
    };
    for (std::size_t i = 0; i < 4U; ++i) {
        wse::asset::BrfFaceCorner c{};
        c.vertex_index = static_cast<std::int32_t>(i);
        c.color = 0xFFFFFFFFU;
        c.normal = {0.0f, 0.0f, 1.0f};
        c.uv = uvs[i];
        c.uv2 = uvs[i];
        mesh.face_corners.push_back(c);
    }
    mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 1, 2}});
    mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 2, 3}});
    return mesh;
}

void test_raw_keys_and_game_key_manager() {
    using namespace wse::platform;

    int named_count = 0;
    for (std::int32_t k = 0; k < kMaxRawInputKeys; ++k) {
        if (!get_raw_key_name(k).empty()) {
            ++named_count;
        }
    }
    expect(named_count == 125 && get_raw_key_name(0x00).empty() &&
               get_raw_key_name(0x46).empty() &&
               get_raw_key_name(RawKey::kEscape) == "Esc" &&
               get_raw_key_name(RawKey::kW) == "W" &&
               get_raw_key_name(RawKey::kLeftCtrl) == "Left Ctrl" &&
               get_raw_key_name(RawKey::kLeftMouse) == "Left Mouse Btn" &&
               get_raw_key_name(RawKey::kScrollUp) == "Scroll Up" &&
               get_raw_key_name(RawKey::kGamepadA) == "Gamepad A" &&
               get_raw_key_name(RawKey::kRightTrigger) == "Right Trigger" &&
               get_raw_key_name(RawKey::kLeftStick) == "Left Stick",
           "get_raw_key_name matches all 125 v1.1.5.5 keys (`0x1402ad360`)");

    WseGameKeyManager km;
    expect(km.find_by_config_name("MoveForward") == 0 &&
               km.find_by_config_name("TakeScreenshot") == 74 &&
               km.find_by_config_name("Custom1") == 75 &&
               km.find_by_config_name("Custom20") == 94,
           "default game keys cover MoveForward..TakeScreenshot and Custom1..Custom20");

    expect(km.get_key_name(0, 0) == "W" && km.get_key_name(0, 1) == "/" &&
               km.get_key_name(54, 0) == "Left Ctrl+J" &&
               km.get_key_name(6, 0) == "Left Mouse Btn" &&
               km.get_key_name(6, 1) == "Right Trigger" &&
               km.get_key_name(53, 0) == "No key assigned",
           "get_key_name formats single, modified, gamepad alt, and unassigned slots");

    std::array<bool, kMaxRawInputKeys> prev{};
    std::array<bool, kMaxRawInputKeys> down{};
    // Bare J without Left Ctrl must not match gk_rear_horse (54).
    down[static_cast<std::size_t>(RawKey::kJ)] = true;
    expect(!km.key_down(54, down) && !km.key_clicked(54, down, prev),
           "modifier chord requires modifier key held");

    // Hold Left Ctrl + press J -> clicked + down; next frame stay held -> down, !clicked.
    down[static_cast<std::size_t>(RawKey::kLeftCtrl)] = true;
    expect(km.key_down(54, down) && km.key_clicked(54, down, prev) &&
               !km.key_raised(54, down, prev),
           "Left Ctrl + J triggers RearHorse down and clicked");
    prev = down;
    down[static_cast<std::size_t>(RawKey::kJ)] = false;
    expect(!km.key_down(54, down) && !km.key_clicked(54, down, prev) &&
               km.key_raised(54, down, prev),
           "releasing J while holding Left Ctrl fires RearHorse keyRaised");

    // storeKeys / setKey / restoreKeys
    km.store_keys();
    expect(km.set_key(75, 0, KeyChord::make(RawKey::kF5, RawKey::kRightCtrl)) &&
               km.get_key_name(75, 0) == "Right Ctrl+F5",
           "set_key binds Custom1 with Right Ctrl modifier");
    expect(km.restore_keys() && km.get_key_name(75, 0) == "No key assigned",
           "restore_keys reverts temporary binding changes");
}

void test_contexts_gamepad_and_touch_visibility() {
    using namespace wse::platform;

    WseInputManager mgr;
    expect(mgr.is_touch_overlay_visible() && mgr.is_widget_visible(TouchWidgetId::AttackZone) &&
               mgr.is_widget_visible(TouchWidgetId::MoveStick),
           "touch overlay starts visible with combat zones in Mission context");

    mgr.set_context(InputContext::CampaignMap);
    expect(mgr.is_touch_overlay_visible() && mgr.is_widget_visible(TouchWidgetId::MoveStick) &&
               mgr.is_widget_visible(TouchWidgetId::LookArea) &&
               !mgr.is_widget_visible(TouchWidgetId::AttackZone) &&
               !mgr.is_widget_visible(TouchWidgetId::BlockZone) &&
               !mgr.is_widget_visible(TouchWidgetId::CommandRadial),
           "CampaignMap shows movement stick/look while hiding mission combat zones");

    for (const auto ctx : {InputContext::Menu, InputContext::Inventory,
                           InputContext::Presentation, InputContext::Dialog}) {
        mgr.set_context(ctx);
        expect(!mgr.is_touch_overlay_visible() &&
                   !mgr.is_widget_visible(TouchWidgetId::MoveStick),
               "non-gameplay contexts hide the touch overlay");
    }

    // Active touch held while entering Menu gets cleared and touches in Menu do nothing.
    mgr.set_context(InputContext::Mission);
    const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
    mgr.process_frame(std::array<RawInputEvent, 2>{{
        RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 1, stick.cx, stick.cy - 0.10f},
        RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 1, stick.cx, stick.cy - 0.11f},
    }});
    expect(mgr.frame_state().move_y > 0.5f && mgr.game_key_down()[0],
           "touch stick drives forward movement in Mission");

    mgr.set_context(InputContext::Menu);
    mgr.process_frame({});
    expect(!mgr.frame_state().touch_overlay_visible && close(mgr.frame_state().move_y, 0.0f) &&
               !mgr.game_key_down()[0],
           "switching to Menu hides overlay and clears held virtual stick");

    // Gamepad connect hides overlay; disconnect brings it back.
    mgr.set_context(InputContext::Mission);
    expect(mgr.is_touch_overlay_visible(), "Mission restores touch overlay visibility");
    mgr.process_frame(std::array<RawInputEvent, 2>{{
        RawInputEvent{RawInputEvent::Type::GamepadConnected, -1, 77},
        RawInputEvent{RawInputEvent::Type::GamepadAxis, -1, 77, 0, 0, 0, 0, 0, 0.0f, -0.9f, 0.0f,
                      0.0f, 0.0f, 0.8f},
    }});
    expect(!mgr.frame_state().touch_overlay_visible && mgr.frame_state().gamepad_connected &&
               mgr.frame_state().move_y > 0.7f && mgr.game_key_down()[0] &&
               mgr.game_key_down()[6],
           "gamepad attach hides touch overlay and maps left stick + right trigger");

    mgr.process_frame(std::array<RawInputEvent, 1>{{
        RawInputEvent{RawInputEvent::Type::GamepadDisconnected, -1, 77},
    }});
    expect(!mgr.frame_state().gamepad_connected && mgr.frame_state().touch_overlay_visible,
           "gamepad detach restores touch overlay visibility");
}

void test_touch_overlay_controls() {
    using namespace wse::platform;

    WseInputManager mgr;
    const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
    const auto look = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::LookArea)];
    const auto atk = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::AttackZone)];
    const auto blk = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::BlockZone)];
    const auto rad = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::CommandRadial)];

    // Deadzone hold on MoveStick -> zero motion
    mgr.process_frame(std::array<RawInputEvent, 1>{{
        RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 10, stick.cx + 0.005f, stick.cy},
    }});
    expect(close(mgr.frame_state().move_x, 0.0f) && !mgr.game_key_down()[3],
           "stick inner deadzone suppresses small jitter");

    // Multi-touch: Finger 10 drags stick NE; Finger 11 drags LookArea right+up; Finger 12 swipes AttackZone Left.
    mgr.process_frame(std::array<RawInputEvent, 5>{{
        RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 10, stick.cx + 0.08f, stick.cy - 0.10f},
        RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 11, look.cx - 0.05f, look.cy},
        RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 11, look.cx + 0.05f, look.cy - 0.04f},
        RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 12, atk.cx, atk.cy},
        RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 12, atk.cx - 0.05f, atk.cy},
    }});
    expect(mgr.frame_state().move_x > 0.5f && mgr.frame_state().move_y > 0.5f &&
               mgr.game_key_down()[0] && mgr.game_key_down()[3] &&
               mgr.frame_state().look_yaw_delta > 0.0f &&
               mgr.frame_state().look_pitch_delta > 0.0f &&
               mgr.frame_state().attack_dir == TouchCombatDirection::Left &&
               mgr.game_key_down()[6],
           "multi-touch simultaneously drives stick, look drag, and left attack swipe");

    // Check remaining 3 attack directions & 4 block directions
    const std::array<std::pair<float, float>, 4> dirs{{
        {0.0f, -0.05f},  // Up
        {0.0f, +0.05f},  // Down
        {-0.05f, 0.0f},  // Left
        {+0.05f, 0.0f},  // Right
    }};
    const std::array<TouchCombatDirection, 4> expected_dirs{{
        TouchCombatDirection::Up,
        TouchCombatDirection::Down,
        TouchCombatDirection::Left,
        TouchCombatDirection::Right,
    }};
    for (std::size_t i = 0; i < 4U; ++i) {
        mgr.process_frame(std::array<RawInputEvent, 4>{{
            RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 20, blk.cx, blk.cy},
            RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 20, blk.cx + dirs[i].first,
                          blk.cy + dirs[i].second},
            RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 12, atk.cx + dirs[i].first,
                          atk.cy + dirs[i].second},
            RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 20},
        }});
        expect(mgr.frame_state().block_dir == TouchCombatDirection::None &&
                   mgr.game_key_clicked()[7] && mgr.frame_state().block_active,
               "block swipe inside one frame still registers one Defend press");
        // Verify active block dir before TouchUp
        mgr.process_frame(std::array<RawInputEvent, 2>{{
            RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 21, blk.cx, blk.cy},
            RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 21, blk.cx + dirs[i].first,
                          blk.cy + dirs[i].second},
        }});
        expect(mgr.frame_state().block_dir == expected_dirs[i] &&
                   mgr.frame_state().attack_dir == expected_dirs[i] &&
                   mgr.game_key_down()[7],
               "all 4 attack and block swipe directions classify accurately");
        mgr.process_frame(std::array<RawInputEvent, 1>{{
            RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 21},
        }});
    }

    // Release remaining touch fingers cleanly
    mgr.process_frame(std::array<RawInputEvent, 3>{{
        RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 10},
        RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 11},
        RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 12},
    }});

    // Command radial: press opens ViewOrders (22), drag East (sector 2 = Order3 = gk 25), release pulses Order3
    mgr.process_frame(std::array<RawInputEvent, 2>{{
        RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 30, rad.cx, rad.cy},
        RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 30, rad.cx + 0.05f, rad.cy},
    }});
    expect(mgr.frame_state().radial_open && mgr.frame_state().radial_sector == 2 &&
               mgr.game_key_clicked()[22],
           "CommandRadial opens ViewOrders and highlights sector 2 (East)");

    mgr.process_frame(std::array<RawInputEvent, 1>{{
        RawInputEvent{RawInputEvent::Type::TouchUp, -1, 0, 30},
    }});
    expect(!mgr.frame_state().radial_open && mgr.game_key_clicked()[25],
           "releasing CommandRadial sector 2 clicks Order3 (gk 25)");
}

void test_layout_editor_and_module_profiles() {
    using namespace wse::platform;

    WseInputManager mgr;
    auto& ed = mgr.layout_editor();
    const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
    expect(ed.hit_test(mgr.profile(), stick.cx, stick.cy) == TouchWidgetId::MoveStick,
           "layout editor hit_test locates MoveStick");

    ed.begin_edit();
    ed.select(TouchWidgetId::AttackZone);
    expect(ed.move_selected(mgr.profile(), 0.50f, -0.20f) &&
               mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::AttackZone)].cx <= 0.96f &&
               ed.set_widget_scale(mgr.profile(), TouchWidgetId::MoveStick, 3.5f) &&
               close(mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)].scale,
                     2.0f),
           "layout editor clamps edited widget position and scale bounds");
    ed.end_edit();

    (void)mgr.game_keys().set_key(75, 0, KeyChord::make(RawKey::kF6, RawKey::kRightAlt));
    mgr.profile().look_sensitivity = 4.2f;

    MemoryVfs vfs;
    std::string err;
    expect(save_input_profile(vfs, "Phantasy Calradia", mgr.profile(), &mgr.game_keys(), &err),
           "save per-module JSON profile over Vfs");
    expect(!save_input_profile(vfs, "../escape", mgr.profile(), nullptr, &err) &&
               input_profile_vfs_path("..").empty(),
           "profile path sanitizer blocks directory traversal");

    TouchOverlayProfile loaded_profile{};
    WseGameKeyManager loaded_keys;
    expect(load_input_profile(vfs, "Phantasy Calradia", loaded_profile, &loaded_keys, &err) &&
               loaded_profile == mgr.profile() &&
               loaded_keys.get_key_name(75, 0) == "Right Alt+F6",
           "load_input_profile round-trips widget layout, sensitivity, and bindings");

    // Corrupt JSON fails without mutating caller's existing profile
    const auto before = loaded_profile;
    expect(vfs.write_file("input_profiles/broken.json",
                          std::vector<std::uint8_t>{'{', 'b', 'a', 'd'}) &&
               !load_input_profile(vfs, "broken", loaded_profile, &loaded_keys, &err) &&
               loaded_profile == before,
           "corrupt input profile JSON is rejected without partial mutation");

    // Reset layout back to defaults
    ed.reset_defaults(loaded_profile);
    expect(loaded_profile.widgets == default_touch_widgets(),
           "layout editor reset_defaults restores canonical widget placement");
}

void test_touch_overlay_drives_free_camera_in_p4_viewer() {
    using namespace wse::platform;

    const auto dds_bytes = build_bc1_dds(0xF800U, 0x07E0U, 0x00000000U);
    wse::asset::DdsImageView dds{};
    std::string err;
    expect(wse::asset::parse_dds(dds_bytes, dds, &err), "parse DDS for P7 viewer camera test");
    const auto mesh = build_test_quad_mesh();

    SoftwareRhiDevice rhi;
    BrfMeshRenderer viewer;
    expect(viewer.upload(rhi, mesh, dds, &err), "upload test mesh to P4 BrfMeshRenderer");

    FreeCameraController cam{};
    float mvp[16]{};
    std::vector<std::uint8_t> frame0;
    cam.build_mvp_matrix(1.0f, 60.0f, mvp);
    expect(viewer.render(64U, 64U, mvp, frame0, &err), "render initial P4 viewer frame");
    const std::uint64_t hash0 = hash_rgba8_frame(frame0);

    // Match check against P4 make_orbit_mvp_matrix at (0, 0, 1.8), yaw=0, pitch=0
    float orbit_mvp[16]{};
    make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, orbit_mvp);
    std::vector<std::uint8_t> orbit_frame;
    expect(viewer.render(64U, 64U, orbit_mvp, orbit_frame, &err) &&
               hash_rgba8_frame(orbit_frame) == hash0,
           "FreeCameraController initial pose matches P4 make_orbit_mvp_matrix");

    WseInputManager mgr;
    const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
    const auto look = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::LookArea)];

    // Touch move stick forward+right & drag look area across 5 ticks -> camera moves & rotates
    mgr.process_frame(std::array<RawInputEvent, 2>{{
                          RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 1, stick.cx + 0.06f,
                                        stick.cy - 0.09f},
                          RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 2, look.cx, look.cy},
                      }},
                      0.05f);
    cam.apply_input(mgr.frame_state(), 0.05f);
    for (int step = 1; step <= 5; ++step) {
        mgr.process_frame(std::array<RawInputEvent, 1>{{
                              RawInputEvent{RawInputEvent::Type::TouchMove, -1, 0, 2,
                                            look.cx + 0.015f * static_cast<float>(step),
                                            look.cy - 0.006f * static_cast<float>(step)},
                          }},
                          0.05f);
        cam.apply_input(mgr.frame_state(), 0.05f);
    }

    std::vector<std::uint8_t> frame1;
    cam.build_mvp_matrix(1.0f, 60.0f, mvp);
    expect(viewer.render(64U, 64U, mvp, frame1, &err), "render moved free-camera frame");
    const std::uint64_t hash1 = hash_rgba8_frame(frame1);
    expect(cam.pos_z < 1.8f && cam.yaw_rad > 0.0f && cam.pitch_rad > 0.0f && hash1 != hash0,
           "touch overlay stick + look drag steers free camera and alters P4 viewer output");

    // In Menu context, further touch drags must hide overlay and leave viewer frame unchanged.
    mgr.set_context(InputContext::Menu);
    mgr.process_frame(std::array<RawInputEvent, 2>{{
                          RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 5, stick.cx,
                                        stick.cy - 0.1f},
                          RawInputEvent{RawInputEvent::Type::TouchDown, -1, 0, 6, look.cx, look.cy},
                      }},
                      0.05f);
    cam.apply_input(mgr.frame_state(), 0.05f);
    std::vector<std::uint8_t> frame2;
    cam.build_mvp_matrix(1.0f, 60.0f, mvp);
    expect(viewer.render(64U, 64U, mvp, frame2, &err) && hash_rgba8_frame(frame2) == hash1 &&
               !mgr.frame_state().touch_overlay_visible,
           "Menu context hides touch overlay and stops touch events moving camera");

    // Controller attach in Mission also hides overlay while gamepad axes drive camera.
    mgr.set_context(InputContext::Mission);
    mgr.process_frame(std::array<RawInputEvent, 2>{{
                          RawInputEvent{RawInputEvent::Type::GamepadConnected, -1, 1},
                          RawInputEvent{RawInputEvent::Type::GamepadAxis, -1, 1, 0, 0, 0, 0, 0,
                                        0.0f, 1.0f, -0.8f, 0.0f, 0.0f, 0.0f},
                      }},
                      0.2f);
    cam.apply_input(mgr.frame_state(), 0.2f);
    std::vector<std::uint8_t> frame3;
    cam.build_mvp_matrix(1.0f, 60.0f, mvp);
    expect(!mgr.frame_state().touch_overlay_visible && viewer.render(64U, 64U, mvp, frame3, &err) &&
               hash_rgba8_frame(frame3) != hash1,
           "controller attach hides overlay and drives free camera cleanly");

    const wse::core::WseTransform ct = cam.to_transform();
    expect(close(ct.s.length(), 1.0f) && close(ct.f.length(), 1.0f) &&
               close(ct.u.length(), 1.0f) && close(ct.s.dot(ct.f), 0.0f) &&
               close(ct.f.dot(ct.u), 0.0f) && close(ct.s.dot(ct.u), 0.0f),
           "FreeCameraController::to_transform produces orthonormal Warband camera frame");
}

void test_vm_and_world_input_bridge() {
    using namespace wse::game;
    using namespace wse::platform;

    WseInputManager mgr;
    SyntheticInputBackend backend;
    wse::vm::VmContext ctx{};
    ctx.world.has_game = true;

    // Frame 1: Left Ctrl + J down, mouse moved to (0.25, 0.75)
    backend.push_batch(std::array<RawInputEvent, 3>{{
        RawInputEvent{RawInputEvent::Type::KeyDown, RawKey::kLeftCtrl},
        RawInputEvent{RawInputEvent::Type::KeyDown, RawKey::kJ},
        RawInputEvent{RawInputEvent::Type::MouseMotion, -1, 0, 0, 0.25f, 0.75f, 0.0f, 0.0f},
    }});
    WorldInputBridge::step_and_sync(mgr, backend, ctx.world);

    // 65: game_key_get_mapped_key_name
    expect(run_stmt(ctx, 65U, {0, 0, 0}) && ctx.stringRegisters[0] == u"W" &&
               run_stmt(ctx, 65U, {1, 54, 0}) && ctx.stringRegisters[1] == u"Left Ctrl+J" &&
               run_stmt(ctx, 65U, {2, 0, 1}) && ctx.stringRegisters[2] == u"/",
           "65 game_key_get_mapped_key_name reflects live WseGameKeyManager bindings");

    // 70 key_is_down, 71 key_clicked, 72 game_key_is_down, 73 game_key_clicked
    expect(run_stmt(ctx, 70U, {RawKey::kJ}) && run_stmt(ctx, 71U, {RawKey::kJ}) &&
               run_stmt(ctx, 72U, {54}) && run_stmt(ctx, 73U, {54}),
           "70..73 key/game_key down and clicked evaluate true on press frame");

    // 75 mouse_get_position: Warband UI space (x 0..1, y 0..0.75 growing upwards), so a
    // cursor 3/4 down the screen reads y = 0.25 * 0.75.
    expect(run_stmt(ctx, 75U, {0}) && close(ctx.positions[0].o.x, 0.25f) &&
               close(ctx.positions[0].o.y, 0.1875f),
           "75 mouse_get_position returns the cursor in Warband UI space (y up, 0..0.75)");

    // 3100 game_key_get_key
    expect(run_stmt(ctx, 3100U, {reg(0), 54, 0, 0}) && ctx.registers[0] == RawKey::kJ &&
               run_stmt(ctx, 3100U, {reg(1), 54, 0, 1}) &&
               ctx.registers[1] == RawKey::kLeftCtrl &&
               run_stmt(ctx, 3100U, {reg(2), 54, 1, 0}) && ctx.registers[2] == -1,
           "3100 game_key_get_key returns bound key, modifier, and alt slots");

    // 77 omit_key_once & 78 clear_omitted_keys (omitted_keys state; raw 70 unaffected for scripts)
    expect(run_stmt(ctx, 77U, {RawKey::kJ}) && ctx.world.input.omitted_keys[RawKey::kJ] &&
               run_stmt(ctx, 70U, {RawKey::kJ}) &&
               run_stmt(ctx, 78U, {}) && !ctx.world.input.omitted_keys[RawKey::kJ] &&
               run_stmt(ctx, 70U, {RawKey::kJ}),
           "77 omit_key_once toggles omitted_keys until 78 without suppressing script 70");

    // Frame 2: release J -> 3101 key_released and 3102 game_key_released fire
    backend.push(RawInputEvent{RawInputEvent::Type::KeyUp, RawKey::kJ});
    WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
    expect(!run_stmt(ctx, 70U, {RawKey::kJ}) && run_stmt(ctx, 3101U, {RawKey::kJ}) &&
               run_stmt(ctx, 3102U, {54}),
           "3101 key_released and 3102 game_key_released fire on key release");
}

// Regressions from the review of the first P7 commit (`4793c11`).
void test_review_regressions() {
    using namespace wse::platform;
    using T = RawInputEvent::Type;
    const auto key = [](T type, std::int32_t raw) {
        RawInputEvent e{};
        e.type = type;
        e.raw_key = raw;
        return e;
    };
    const auto touch = [](T type, std::int64_t finger, float x, float y) {
        RawInputEvent e{};
        e.type = type;
        e.finger_id = finger;
        e.x = x;
        e.y = y;
        return e;
    };
    constexpr auto gk = [](GameKeyId id) { return static_cast<std::size_t>(id); };

    // 1. A press + release inside one frame is a click (raw and game key) and a release.
    {
        WseInputManager mgr;
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {key(T::KeyDown, RawKey::kF), key(T::KeyUp, RawKey::kF)}});
        expect(mgr.raw_clicked()[RawKey::kF] && mgr.raw_released()[RawKey::kF] &&
                   !mgr.raw_down()[RawKey::kF] && mgr.game_key_clicked()[gk(GameKeyId::Use)] &&
                   mgr.game_key_released()[gk(GameKeyId::Use)],
               "key tapped inside one frame still clicks and releases (raw + game key)");
    }

    // 2. Every wheel notch on consecutive frames is a click.
    {
        WseInputManager mgr;
        int clicks = 0;
        for (int i = 0; i < 6; ++i) {
            RawInputEvent w{};
            w.type = T::MouseWheel;
            w.dy = 1.0f;
            mgr.process_frame(std::array<RawInputEvent, 1>{{w}});
            clicks += mgr.game_key_clicked()[gk(GameKeyId::EquipPrimaryWeapon)] ? 1 : 0;
        }
        expect(clicks == 6, "six consecutive wheel notches give six EquipPrimaryWeapon clicks");
    }

    // 3. Analog movement stays proportional past the key threshold; diagonals are unit length.
    {
        WseInputManager mgr;
        const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
        const float r = stick.circle_radius(1.0f);
        float prev = 0.0f;
        bool monotonic = true;
        bool below_full = true;
        for (const float d : {0.3f, 0.4f, 0.5f, 0.6f, 0.7f}) {
            mgr.reset();
            mgr.process_frame(std::array<RawInputEvent, 1>{
                {touch(T::TouchDown, 1, stick.cx, stick.cy - d * r)}});
            const float my = mgr.frame_state().move_y;
            monotonic = monotonic && my > prev;
            below_full = below_full && my < 0.8f;
            prev = my;
        }
        expect(monotonic && below_full && mgr.game_key_down()[gk(GameKeyId::MoveForward)],
               "touch stick speed is proportional above stick_key_threshold (no jump to 1.0)");

        WseInputManager kb;
        kb.process_frame(std::array<RawInputEvent, 2>{
            {key(T::KeyDown, RawKey::kW), key(T::KeyDown, RawKey::kD)}});
        const float mag = std::hypot(kb.frame_state().move_x, kb.frame_state().move_y);
        expect(close(mag, 1.0f), "W+D diagonal movement has unit length");

        WseInputManager pad;
        RawInputEvent ax{};
        ax.type = T::GamepadAxis;
        ax.device_id = 3;
        ax.left_y = -0.6f;
        pad.process_frame(std::array<RawInputEvent, 1>{{ax}});
        expect(pad.frame_state().move_y > 0.4f && pad.frame_state().move_y < 0.7f,
               "gamepad left stick at 60% moves at about 50% speed, not full speed");
    }

    // 4. The gamepad left stick also drives the campaign-map move keys.
    {
        WseInputManager mgr;
        mgr.set_context(InputContext::CampaignMap);
        RawInputEvent ax{};
        ax.type = T::GamepadAxis;
        ax.device_id = 1;
        ax.left_y = -1.0f;
        mgr.process_frame(std::array<RawInputEvent, 1>{{ax}});
        expect(mgr.game_key_down()[gk(GameKeyId::MapMoveForward)],
               "gamepad stick forward presses MapMoveForward on the campaign map");
    }

    // 5. Unplugging a pad releases what it held; another pad keeps its own buttons.
    {
        WseInputManager mgr;
        RawInputEvent a = key(T::GamepadButtonDown, RawKey::kGamepadA);
        a.device_id = 1;
        RawInputEvent b = key(T::GamepadButtonDown, RawKey::kGamepadB);
        b.device_id = 2;
        mgr.process_frame(std::array<RawInputEvent, 2>{{a, b}});
        RawInputEvent gone{};
        gone.type = T::GamepadDisconnected;
        gone.device_id = 1;
        mgr.process_frame(std::array<RawInputEvent, 1>{{gone}});
        expect(!mgr.game_key_down()[gk(GameKeyId::Jump)] &&
                   mgr.game_key_released()[gk(GameKeyId::Jump)] &&
                   mgr.game_key_down()[gk(GameKeyId::DropPrimaryWeapon)],
               "removed gamepad's held button is released; other pad's button stays down");

        // An idle second pad cannot zero the stick of the pad in use.
        RawInputEvent move{};
        move.type = T::GamepadAxis;
        move.device_id = 2;
        move.left_y = -1.0f;
        RawInputEvent idle{};
        idle.type = T::GamepadAxis;
        idle.device_id = 5;
        mgr.process_frame(std::array<RawInputEvent, 2>{{move, idle}});
        expect(mgr.frame_state().move_y > 0.9f, "stick of the pad in use wins over an idle pad");
    }

    // 6. Focus loss releases keys, buttons, and touches; cancelled touches commit nothing.
    {
        WseInputManager mgr;
        const auto stick = mgr.profile().widgets[0];
        const auto rad = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::CommandRadial)];
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {key(T::KeyDown, RawKey::kW), touch(T::TouchDown, 1, stick.cx, stick.cy - 0.1f)}});
        RawInputEvent lost{};
        lost.type = T::FocusLost;
        mgr.process_frame(std::array<RawInputEvent, 1>{{lost}});
        for (int i = 0; i < 3; ++i) {
            mgr.process_frame({});
        }
        expect(!mgr.game_key_down()[gk(GameKeyId::MoveForward)] &&
                   close(mgr.frame_state().move_y, 0.0f) && !mgr.raw_down()[RawKey::kW],
               "FocusLost releases held keys and the touch stick");

        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 7, rad.cx, rad.cy), touch(T::TouchMove, 7, rad.cx + 0.05f, rad.cy)}});
        mgr.process_frame(std::array<RawInputEvent, 1>{{touch(T::TouchCancel, 7, 0.0f, 0.0f)}});
        expect(!mgr.frame_state().radial_open && !mgr.game_key_clicked()[gk(GameKeyId::Order3)],
               "TouchCancel closes the command radial without issuing the highlighted order");

        WseInputManager tap;
        const auto atk = tap.profile().widgets[static_cast<std::size_t>(TouchWidgetId::AttackZone)];
        tap.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 3, atk.cx, atk.cy), touch(T::TouchUp, 3, atk.cx, atk.cy)}});
        expect(tap.game_key_clicked()[gk(GameKeyId::Attack)],
               "attack-zone tap inside one frame still attacks");
    }

    // 7. Physical clicks follow keyClicked (0x1400eee90): only the main key's edge counts.
    {
        WseInputManager mgr;
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyDown, RawKey::kJ)}});
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyDown, RawKey::kLeftCtrl)}});
        expect(mgr.game_key_down()[gk(GameKeyId::RearHorse)] &&
                   !mgr.game_key_clicked()[gk(GameKeyId::RearHorse)],
               "holding J then pressing Ctrl does not click RearHorse (keyClicked semantics)");
    }

    // 8. omit_key_once swallows exactly one press of the key for game-key mapping.
    {
        WseInputManager mgr;
        mgr.omit_key_once(RawKey::kF);
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyDown, RawKey::kF)}});
        const bool swallowed = !mgr.game_key_clicked()[gk(GameKeyId::Use)] &&
                               !mgr.game_key_down()[gk(GameKeyId::Use)] &&
                               mgr.raw_down()[RawKey::kF] && !mgr.omitted_keys()[RawKey::kF];
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyUp, RawKey::kF)}});
        const bool no_release = !mgr.game_key_released()[gk(GameKeyId::Use)];
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyDown, RawKey::kF)}});
        expect(swallowed && no_release && mgr.game_key_clicked()[gk(GameKeyId::Use)],
               "omit_key_once suppresses the next press only, then is consumed");

        // Same-frame Up+Down while suppressed must clear suppression on KeyUp and allow the new press.
        mgr.omit_key_once(RawKey::kF);
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyUp, RawKey::kF)}});
        mgr.process_frame(std::array<RawInputEvent, 1>{{key(T::KeyDown, RawKey::kF)}});
        const bool held_suppressed = !mgr.game_key_down()[gk(GameKeyId::Use)];
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {key(T::KeyUp, RawKey::kF), key(T::KeyDown, RawKey::kF)}});
        expect(held_suppressed && mgr.game_key_clicked()[gk(GameKeyId::Use)] &&
                   mgr.game_key_down()[gk(GameKeyId::Use)],
               "same-frame KeyUp+KeyDown clears omit suppression before accepting new press");
    }

    // 9. Aspect ratio: circular stick and swipe classification in physical units.
    {
        WseInputManager mgr;
        mgr.set_viewport_size(2400, 1080);
        const float a = mgr.viewport_aspect();
        const auto stick = mgr.profile().widgets[0];
        const float r = stick.circle_radius(a);
        // Same physical distance right and up must give the same deflection.
        mgr.process_frame(std::array<RawInputEvent, 1>{
            {touch(T::TouchDown, 1, stick.cx + 0.8f * r / a, stick.cy)}});
        const float right = mgr.frame_state().move_x;
        mgr.reset();
        mgr.set_viewport_size(2400, 1080);
        mgr.process_frame(std::array<RawInputEvent, 1>{
            {touch(T::TouchDown, 1, stick.cx, stick.cy - 0.8f * r)}});
        const float up = mgr.frame_state().move_y;
        expect(close(right, up, 1e-4f) && right > 0.5f,
               "touch stick is a circle on screen: equal pixel distances give equal deflection");

        // 60 px right, 35 px up on 2400x1080 is a mostly horizontal swipe.
        const auto atk = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::AttackZone)];
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 4, atk.cx, atk.cy),
             touch(T::TouchMove, 4, atk.cx + 60.0f / 2400.0f, atk.cy - 35.0f / 1080.0f)}});
        expect(mgr.frame_state().attack_dir == TouchCombatDirection::Right,
               "swipe direction is classified in pixels, not per-axis normalized units");
    }

    // 10. Touch brings the overlay back while a pad is connected; using the pad hides it.
    {
        WseInputManager mgr;
        RawInputEvent c{};
        c.type = T::GamepadConnected;
        c.device_id = 9;
        mgr.process_frame(std::array<RawInputEvent, 1>{{c}});
        const bool hidden = !mgr.is_touch_overlay_visible();
        const auto stick = mgr.profile().widgets[0];
        mgr.process_frame(std::array<RawInputEvent, 1>{
            {touch(T::TouchDown, 1, stick.cx, stick.cy - 0.1f)}});
        const bool shown = mgr.is_touch_overlay_visible() &&
                           mgr.game_key_down()[gk(GameKeyId::MoveForward)];
        RawInputEvent btn = key(T::GamepadButtonDown, RawKey::kGamepadA);
        btn.device_id = 9;
        mgr.process_frame(std::array<RawInputEvent, 1>{{btn}});
        expect(hidden && shown && !mgr.is_touch_overlay_visible() &&
                   !mgr.game_key_down()[gk(GameKeyId::MoveForward)],
               "touch re-shows the overlay with a pad connected; pad input hides it again");
    }

    // 11. Mouse motion does not turn the camera in menus or when mouse look is off.
    {
        WseInputManager mgr;
        RawInputEvent mm{};
        mm.type = T::MouseMotion;
        mm.x = 0.5f;
        mm.y = 0.5f;
        mm.dx = 0.1f;
        mm.dy = 0.1f;
        mgr.set_context(InputContext::Menu);
        mgr.process_frame(std::array<RawInputEvent, 1>{{mm}});
        const bool menu_still = close(mgr.frame_state().look_yaw_delta, 0.0f);
        mgr.set_context(InputContext::Mission);
        mgr.set_mouse_look_enabled(false);
        mgr.process_frame(std::array<RawInputEvent, 1>{{mm}});
        const bool off_still = close(mgr.frame_state().look_yaw_delta, 0.0f);
        mgr.set_mouse_look_enabled(true);
        mgr.process_frame(std::array<RawInputEvent, 1>{{mm}});
        expect(menu_still && off_still && mgr.frame_state().look_yaw_delta > 0.0f,
               "mouse look only in gameplay contexts with mouse look enabled");
    }

    // 12. Module profile names: real names get a file, distinct names stay distinct.
    {
        const std::string a = input_profile_vfs_path("A World of Ice & Fire");
        const std::string b = input_profile_vfs_path("L'Aigle");
        const std::string c = input_profile_vfs_path("\xe4\xb8\xad\xe6\x96\x87");
        const std::string d = input_profile_vfs_path("Foo Bar");
        const std::string e = input_profile_vfs_path("Foo_Bar");
        const bool single_segment =
            a.find('/', 15U) == std::string::npos && c.find('/', 15U) == std::string::npos;
        expect(!a.empty() && !b.empty() && !c.empty() && d != e && single_segment &&
                   input_profile_vfs_path("native") == input_profile_vfs_path("Native") &&
                   input_profile_vfs_path("a/b").empty() && input_profile_vfs_path(".").size() > 0U &&
                   input_profile_vfs_path("..").empty(),
               "module profile ids encode punctuation and UTF-8 instead of rejecting them");

        MemoryVfs vfs;
        TouchOverlayProfile p{};
        p.look_sensitivity = 3.3f;
        TouchOverlayProfile back{};
        std::string err;
        expect(save_input_profile(vfs, "A World of Ice & Fire", p, nullptr, &err) &&
                   load_input_profile(vfs, "A World of Ice & Fire", back, nullptr, &err) &&
                   back == p,
               "profile with punctuation in the module name round-trips");
    }

    // 13. Bridge: same-frame tap reaches 71 / 3101, omit state round-trips, names only
    //     re-sync when bindings change.
    {
        using namespace wse::game;
        WseInputManager mgr;
        SyntheticInputBackend backend;
        wse::vm::VmContext ctx{};
        ctx.world.has_game = true;
        backend.push_batch(std::array<RawInputEvent, 2>{
            {key(T::KeyDown, RawKey::kK), key(T::KeyUp, RawKey::kK)}});
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        expect(run_stmt(ctx, 71U, {RawKey::kK}) && run_stmt(ctx, 3101U, {RawKey::kK}) &&
                   !run_stmt(ctx, 70U, {RawKey::kK}),
               "71 key_clicked and 3101 key_released both see a same-frame tap");

        expect(run_stmt(ctx, 77U, {RawKey::kF}), "77 omit_key_once from script");
        backend.push(key(T::KeyDown, RawKey::kF));
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        // Retail 70/71 (raw key) are not suppressed by omit_key_once when scripts run; 72/73 (game key) are.
        const bool swallowed = !run_stmt(ctx, 72U, {static_cast<std::int64_t>(GameKeyId::Use)}) &&
                               !run_stmt(ctx, 73U, {static_cast<std::int64_t>(GameKeyId::Use)}) &&
                               run_stmt(ctx, 70U, {RawKey::kF}) &&
                               run_stmt(ctx, 71U, {RawKey::kF});
        backend.push(key(T::KeyUp, RawKey::kF));
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        backend.push(key(T::KeyDown, RawKey::kF));
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        expect(swallowed && !ctx.world.input.omitted_keys[RawKey::kF] &&
                   run_stmt(ctx, 70U, {RawKey::kF}) &&
                   run_stmt(ctx, 73U, {static_cast<std::int64_t>(GameKeyId::Use)}),
               "omit_key_once from a script suppresses game-key 72/73 only, leaving raw 70/71 intact");

        const std::uint64_t rev = ctx.world.input.bindings_revision;
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        const bool unchanged = ctx.world.input.bindings_revision == rev;
        (void)mgr.game_keys().set_key(0, 0, KeyChord::make(RawKey::kUpArrow));
        WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
        expect(unchanged && ctx.world.input.bindings_revision != rev &&
                   ctx.world.input.game_key_names[0] == "Up Arrow",
               "binding names re-sync only after a binding changes");
    }

    // 14. FreeCameraController keeps yaw wrapped.
    {
        FreeCameraController cam{};
        InputFrameState in{};
        in.look_yaw_delta = 4.0f;
        for (int i = 0; i < 1000; ++i) {
            cam.apply_input(in, 0.0f);
        }
        expect(cam.yaw_rad > -3.1416f && cam.yaw_rad <= 3.1416f, "free camera yaw stays wrapped");
    }
}

void test_p8_sensors_and_pinch_zoom() {
    using namespace wse::platform;
    using T = RawInputEvent::Type;
    const auto gyro = [](float yaw_rate, float pitch_rate) {
        RawInputEvent ev{};
        ev.type = T::GyroSample;
        ev.dx = yaw_rate;
        ev.dy = pitch_rate;
        return ev;
    };
    const auto touch = [](T t, std::int64_t id, float x, float y) {
        RawInputEvent ev{};
        ev.type = t;
        ev.finger_id = id;
        ev.x = x;
        ev.y = y;
        return ev;
    };

    // 1. Gyro aiming is opt-in and Mission-only, smoothed with radial deadzone and FocusLost reset.
    {
        WseInputManager mgr;
        expect(!mgr.profile().gyro_aim_enabled, "gyro aim defaults to opt-in disabled");
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(1.5f, -0.5f)}}, 1.0f / 60.0f);
        expect(close(mgr.frame_state().look_yaw_delta, 0.0f) &&
                   close(mgr.frame_state().look_pitch_delta, 0.0f),
               "disabled gyro produces zero look delta");

        mgr.profile().gyro_aim_enabled = true;
        mgr.profile().gyro_smoothing = 0.5f;
        mgr.profile().gyro_deadzone = 0.02f;
        mgr.profile().gyro_sensitivity = 1.0f;

        mgr.set_context(InputContext::CampaignMap);
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(1.5f, 0.5f)}}, 1.0f / 60.0f);
        expect(close(mgr.frame_state().look_yaw_delta, 0.0f) &&
                   close(mgr.frame_state().look_pitch_delta, 0.0f),
               "gyro does not trigger on CampaignMap");

        mgr.set_context(InputContext::Mission);
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(0.015f, 0.01f)}}, 1.0f / 60.0f);
        expect(close(mgr.frame_state().look_yaw_delta, 0.0f) &&
                   close(mgr.frame_state().look_pitch_delta, 0.0f),
               "gyro drift inside deadzone produces zero output");

        // Reset filter cleanly, test step response (0.5 smoothing -> 0.50 -> 0.75 of 1.0 rad/s).
        RawInputEvent reset_fl{};
        reset_fl.type = T::FocusLost;
        mgr.process_frame(std::array<RawInputEvent, 1>{{reset_fl}}, 1.0f / 60.0f);
        mgr.profile().gyro_deadzone = 0.0f;
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(1.0f, 0.0f)}}, 1.0f / 60.0f);
        const float y1 = mgr.frame_state().look_yaw_delta;
        mgr.process_frame(std::array<RawInputEvent, 2>{{gyro(0.8f, 0.4f), gyro(1.2f, 0.4f)}},
                          1.0f / 60.0f);
        const float y2 = mgr.frame_state().look_yaw_delta;
        const float p2 = mgr.frame_state().look_pitch_delta;
        expect(close(y1, 0.5f / 60.0f, 1e-5f) && close(y2, 0.75f / 60.0f, 1e-5f) &&
                   p2 > 0.0f,
               "gyro low-pass filter averages per-frame samples and smooths step response");

        // FocusLost clears filter history; invert_gyro_y reverses vertical axis.
        RawInputEvent fl{};
        fl.type = T::FocusLost;
        mgr.process_frame(std::array<RawInputEvent, 1>{{fl}}, 1.0f / 60.0f);
        mgr.process_frame({}, 1.0f / 60.0f);
        const bool reset_clean = close(mgr.frame_state().look_yaw_delta, 0.0f);
        mgr.profile().invert_gyro_y = true;
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(0.0f, 1.0f)}}, 1.0f / 60.0f);
        expect(reset_clean && mgr.frame_state().look_pitch_delta < 0.0f,
               "FocusLost clears gyro filter state and invert_gyro_y flips pitch");
    }

    // 2. Two-finger pinch zooms CampaignMap without contaminating look, respecting aspect and toggle.
    {
        WseInputManager mgr;
        mgr.set_viewport_size(2400, 1080);
        mgr.set_context(InputContext::CampaignMap);
        mgr.profile().pinch_zoom_enabled = true;
        mgr.profile().pinch_zoom_sensitivity = 1.0f;

        // Place one finger outside LookArea (non-stick) and one inside LookArea.
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 10, 0.30f, 0.50f), touch(T::TouchDown, 11, 0.60f, 0.50f)}});
        expect(mgr.frame_state().pinch_active && close(mgr.frame_state().map_zoom_delta, 0.0f),
               "two non-stick touches activate CampaignMap pinch state");

        // Spread fingers outward -> positive map_zoom_delta and zero camera look rotation.
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchMove, 10, 0.25f, 0.50f), touch(T::TouchMove, 11, 0.65f, 0.50f)}});
        const float zoom_in = mgr.frame_state().map_zoom_delta;
        const bool no_look_on_spread =
            close(mgr.frame_state().look_yaw_delta, 0.0f) &&
            close(mgr.frame_state().look_pitch_delta, 0.0f);

        // Pinch inward -> negative map_zoom_delta, propagated through WorldInputBridge and applied
        // to map_camera_distance via exp(-map_zoom_delta).
        wse::game::WseWorldState world{};
        const float initial_dist = world.input.map_camera_distance;
        const bool initial_ok =
            close(initial_dist, wse::game::WseInputState::kDefaultMapCameraDistance);
        wse::platform::SyntheticInputBackend backend;
        backend.push_batch(std::array<RawInputEvent, 2>{
            {touch(T::TouchMove, 10, 0.35f, 0.50f), touch(T::TouchMove, 11, 0.55f, 0.50f)}});
        wse::game::WorldInputBridge::step_and_sync(mgr, backend, world);
        const float zoom_out = mgr.frame_state().map_zoom_delta;
        const float zoomed_dist = world.input.map_camera_distance;
        // Mirroring the same frame again must not apply the zoom impulse twice.
        wse::game::WorldInputBridge::sync_to_world(mgr, world);
        bool bridged = initial_ok &&
                       close(world.input.map_zoom_delta, zoom_out) &&
                       zoomed_dist > initial_dist &&
                       world.input.map_camera_distance == zoomed_dist;

        // Verify retail `rglConfig::Campaign` zoom clamp `[3.5f, 175.0f]`.
        wse::game::WseWorldState clamp_world{};
        WseInputManager clamp_mgr;
        clamp_mgr.set_viewport_size(2400, 1080);
        clamp_mgr.set_context(InputContext::CampaignMap);
        clamp_mgr.profile().pinch_zoom_enabled = true;
        clamp_mgr.profile().pinch_zoom_sensitivity = 1.0f;
        clamp_world.input.map_camera_distance = 3.6f;
        clamp_mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 20, 0.45f, 0.50f), touch(T::TouchDown, 21, 0.55f, 0.50f)}});
        clamp_mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchMove, 20, 0.10f, 0.50f), touch(T::TouchMove, 21, 0.90f, 0.50f)}});
        wse::game::WorldInputBridge::sync_to_world(clamp_mgr, clamp_world);
        bridged = bridged && close(clamp_world.input.map_camera_distance, 3.5f);

        clamp_world.input.map_camera_distance = 174.0f;
        clamp_mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchMove, 20, 0.49f, 0.50f), touch(T::TouchMove, 21, 0.51f, 0.50f)}});
        wse::game::WorldInputBridge::sync_to_world(clamp_mgr, clamp_world);
        bridged = bridged && close(clamp_world.input.map_camera_distance, 175.0f);

        // Lifting one finger exits pinch without jumping look; re-check context/toggle gating.
        mgr.process_frame(std::array<RawInputEvent, 1>{{touch(T::TouchUp, 10, 0.35f, 0.50f)}});
        const bool lifted = !mgr.frame_state().pinch_active;

        // MoveStick + LookArea in CampaignMap must NOT count as a pinch.
        mgr.process_frame(std::array<RawInputEvent, 1>{{touch(T::TouchUp, 11, 0.55f, 0.50f)}});
        const auto stick = mgr.profile().widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 20, stick.cx, stick.cy),
             touch(T::TouchDown, 21, 0.70f, 0.50f)}});
        const bool stick_not_pinch = !mgr.frame_state().pinch_active;

        // Disabling pinch_zoom_enabled prevents pinch activation.
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchUp, 20, stick.cx, stick.cy), touch(T::TouchUp, 21, 0.70f, 0.50f)}});
        mgr.profile().pinch_zoom_enabled = false;
        mgr.process_frame(std::array<RawInputEvent, 2>{
            {touch(T::TouchDown, 30, 0.50f, 0.50f), touch(T::TouchDown, 31, 0.70f, 0.50f)}});
        expect(zoom_in > 0.0f && no_look_on_spread && zoom_out < 0.0f && bridged && lifted &&
                   stick_not_pinch && !mgr.frame_state().pinch_active,
               "pinch zooms in/out without look contamination, bridges to world, and honors toggles");
    }

    // 3. Profile JSON round-trip preserves P8 fields while staying backward-compatible with v1 JSON.
    {
        TouchOverlayProfile legacy{};
        std::string err;
        expect(deserialize_input_profile_json("{\"version\":\"1\",\"look_sensitivity\":\"3.0\"}",
                                              legacy, nullptr, &err) &&
                   !legacy.gyro_aim_enabled && legacy.pinch_zoom_enabled &&
                   close(legacy.gyro_sensitivity, 1.0f),
               "legacy v1 JSON without P8 keys loads with P8 defaults");

        MemoryVfs vfs;
        TouchOverlayProfile p{};
        p.gyro_aim_enabled = true;
        p.gyro_sensitivity = 2.1f;
        p.gyro_smoothing = 0.65f;
        p.gyro_deadzone = 0.04f;
        p.invert_gyro_y = true;
        p.pinch_zoom_enabled = false;
        p.pinch_zoom_sensitivity = 2.4f;
        TouchOverlayProfile back{};
        expect(save_input_profile(vfs, "Native", p, nullptr, &err) &&
                   load_input_profile(vfs, "Native", back, nullptr, &err) &&
                   back == p,
               "P8 sensor & pinch toggles round-trip through module JSON profile");
    }

    // 4. Review of 9411644: gyro axes follow the screen rotation.
    {
        const auto eq = [](GyroLookRates r, float yaw, float pitch) {
            return close(r.yaw_right, yaw) && close(r.pitch_up, pitch);
        };
        // Device +X / +Y angular rates of 0.3 / 0.7 rad/s.
        expect(eq(gyro_look_rates(ScreenRotation::Deg0, 0.3f, 0.7f), -0.7f, 0.3f) &&
                   eq(gyro_look_rates(ScreenRotation::Deg90, 0.3f, 0.7f), 0.3f, 0.7f) &&
                   eq(gyro_look_rates(ScreenRotation::Deg180, 0.3f, 0.7f), 0.7f, -0.3f) &&
                   eq(gyro_look_rates(ScreenRotation::Deg270, 0.3f, 0.7f), -0.3f, -0.7f) &&
                   screen_rotation_from_degrees(-90) == ScreenRotation::Deg270 &&
                   screen_rotation_from_degrees(450) == ScreenRotation::Deg90 &&
                   screen_rotation_from_degrees(180) == ScreenRotation::Deg180,
               "gyro_look_rates remaps natural-orientation axes for each screen rotation");
    }

    // 5. Gyro smoothing is frame-rate independent and holds the rate between samples.
    {
        const auto run = [&](float hz, float seconds, int sample_every) {
            WseInputManager mgr;
            mgr.set_context(InputContext::Mission);
            mgr.profile().gyro_aim_enabled = true;
            mgr.profile().gyro_deadzone = 0.0f;
            mgr.profile().gyro_smoothing = 0.6f;
            const int frames = static_cast<int>(hz * seconds + 0.5f);
            float total = 0.0f;
            for (int f = 0; f < frames; ++f) {
                if (f % sample_every == 0) {
                    mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(1.0f, 0.0f)}}, 1.0f / hz);
                } else {
                    mgr.process_frame({}, 1.0f / hz);
                }
                total += mgr.frame_state().look_yaw_delta;
            }
            return total;
        };
        const float at60 = run(60.0f, 0.5f, 1);
        const float at120 = run(120.0f, 0.5f, 1);
        // A 60 Hz sensor at a 120 Hz frame rate delivers a sample every other frame.
        const float at120_sparse = run(120.0f, 0.5f, 2);
        expect(std::fabs(at60 - at120) < 0.01f && std::fabs(at120 - at120_sparse) < 1e-4f,
               "gyro turn over 0.5 s matches at 60/120 Hz and with a slower sensor");

        WseInputManager mgr;
        mgr.set_context(InputContext::Mission);
        mgr.profile().gyro_aim_enabled = true;
        mgr.profile().gyro_deadzone = 0.0f;
        mgr.profile().gyro_smoothing = 0.0f;
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(1.0f, 0.0f)}}, 1.0f / 120.0f);
        for (int f = 0; f < 8; ++f) {  // ~67 ms without a sample: past the hold window
            mgr.process_frame({}, 1.0f / 120.0f);
        }
        expect(close(mgr.frame_state().look_yaw_delta, 0.0f),
               "a stopped gyro stream decays to zero after the hold window");

        // invert_look_y flips touch/stick look, not the gyro (invert_gyro_y does that).
        mgr.profile().invert_look_y = true;
        mgr.process_frame(std::array<RawInputEvent, 1>{{gyro(0.0f, 1.0f)}}, 1.0f / 60.0f);
        expect(mgr.frame_state().look_pitch_delta > 0.0f,
               "invert_look_y does not also invert gyro pitch");
    }

    // 6. Pinch zoom is the log of the spread ratio: the same relative gesture zooms the
    // same amount whether the fingers start close or far apart.
    {
        const auto pinch = [&](float half_start, float half_end) {
            WseInputManager mgr;
            mgr.set_viewport_size(1080, 1080);
            mgr.set_context(InputContext::CampaignMap);
            mgr.profile().pinch_zoom_enabled = true;
            mgr.profile().pinch_zoom_sensitivity = 1.0f;
            mgr.process_frame(std::array<RawInputEvent, 2>{
                {touch(T::TouchDown, 1, 0.5f - half_start, 0.5f),
                 touch(T::TouchDown, 2, 0.5f + half_start, 0.5f)}});
            mgr.process_frame(std::array<RawInputEvent, 2>{
                {touch(T::TouchMove, 1, 0.5f - half_end, 0.5f),
                 touch(T::TouchMove, 2, 0.5f + half_end, 0.5f)}});
            return mgr.frame_state().map_zoom_delta;
        };
        const float small = pinch(0.05f, 0.10f);
        const float wide = pinch(0.15f, 0.30f);
        expect(close(small, std::log(2.0f), 1e-3f) && close(wide, std::log(2.0f), 1e-3f) &&
                   close(pinch(0.2f, 0.1f), -std::log(2.0f), 1e-3f),
               "pinch zoom delta is ln(spread ratio), independent of starting spread");
    }
}

}  // namespace

namespace wse::selftest {

int run_input_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_raw_keys_and_game_key_manager();
    test_contexts_gamepad_and_touch_visibility();
    test_touch_overlay_controls();
    test_layout_editor_and_module_profiles();
    test_touch_overlay_drives_free_camera_in_p4_viewer();
    test_vm_and_world_input_bridge();
    test_review_regressions();
    test_p8_sensors_and_pinch_zoom();

    if (g_failures == 0) {
        std::printf("input tests: PASS\n");
        return 0;
    }
    std::printf("input tests: FAIL (%d)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_input_suite(nullptr);
}
#endif
