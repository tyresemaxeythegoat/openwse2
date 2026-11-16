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
        expect(mgr.frame_state().block_dir == expected_dirs[i] ||
                   !mgr.frame_state().block_active,
               "block swipe releases cleanly after direction");
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

    // 75 mouse_get_position
    expect(run_stmt(ctx, 75U, {0}) && close(ctx.positions[0].o.x, 0.25f) &&
               close(ctx.positions[0].o.y, 0.75f),
           "75 mouse_get_position returns synchronized normalized cursor position");

    // 3100 game_key_get_key
    expect(run_stmt(ctx, 3100U, {reg(0), 54, 0, 0}) && ctx.registers[0] == RawKey::kJ &&
               run_stmt(ctx, 3100U, {reg(1), 54, 0, 1}) &&
               ctx.registers[1] == RawKey::kLeftCtrl &&
               run_stmt(ctx, 3100U, {reg(2), 54, 1, 0}) && ctx.registers[2] == -1,
           "3100 game_key_get_key returns bound key, modifier, and alt slots");

    // 77 omit_key_once & 78 clear_omitted_keys
    expect(run_stmt(ctx, 77U, {RawKey::kJ}) && !run_stmt(ctx, 70U, {RawKey::kJ}) &&
               run_stmt(ctx, 78U, {}) && run_stmt(ctx, 70U, {RawKey::kJ}),
           "77 omit_key_once suppresses key_is_down until 78 clear_omitted_keys");

    // Frame 2: release J -> 3101 key_released and 3102 game_key_released fire
    backend.push(RawInputEvent{RawInputEvent::Type::KeyUp, RawKey::kJ});
    WorldInputBridge::step_and_sync(mgr, backend, ctx.world);
    expect(!run_stmt(ctx, 70U, {RawKey::kJ}) && run_stmt(ctx, 3101U, {RawKey::kJ}) &&
               run_stmt(ctx, 3102U, {54}),
           "3101 key_released and 3102 game_key_released fire on key release");
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
