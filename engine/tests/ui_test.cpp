// Host and Android self-tests for Slice P9 Part 2: UI localization, layout
// variables, bitmap font XML parser + text layout, window/presentation/menu
// runtime, shared XML DOM parser, and 2D UI RHI rendering (including Native
// main menu rendering when `WSE_WARBAND_ROOT` is set).
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/font.h"
#include "wse/game/module_mission_loaders.h"
#include "wse/game/ui_runtime.h"
#include "wse/io/xml.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_geometry.h"
#include "wse/platform/rhi_ui.h"
#include "wse/platform/ui_frame.h"
#include "wse/platform/ui_game_shell.h"
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
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56U) |
           (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(1, static_cast<std::uint64_t>(idx)));
}

wse::vm::Statement make_stmt(std::uint32_t opcode, std::initializer_list<std::int64_t> args) {
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

std::string find_retail_warband_root() {
    if (const char* env = std::getenv("WSE_WARBAND_ROOT"); env != nullptr && env[0] != '\0') {
        std::error_code ec;
        if (std::filesystem::is_directory(env, ec)) {
            return std::string(env);
        }
    }
#ifdef _WIN32
    static constexpr const char* kDefaultRoot = "C:\\GOG Games\\Mount and Blade - Warband";
    std::error_code ec;
    if (std::filesystem::is_directory(kDefaultRoot, ec)) {
        return std::string(kDefaultRoot);
    }
#endif
    return {};
}

const wse::game::UiWidget* find_widget(const wse::game::UiWidget& node, std::string_view id) {
    if (node.id == id) {
        return &node;
    }
    for (const wse::game::UiWidget& child : node.children) {
        if (const auto* hit = find_widget(child, id)) {
            return hit;
        }
    }
    return nullptr;
}

std::uint64_t count_non_clear_pixels(const std::vector<std::uint8_t>& rgba,
                                     std::uint32_t w,
                                     std::uint32_t h,
                                     std::uint32_t x0,
                                     std::uint32_t y0,
                                     std::uint32_t x1,
                                     std::uint32_t y1,
                                     std::uint8_t clear_r,
                                     std::uint8_t clear_g,
                                     std::uint8_t clear_b) noexcept {
    std::uint64_t count = 0;
    const std::uint32_t cx0 = std::min(x0, w);
    const std::uint32_t cx1 = std::min(x1, w);
    const std::uint32_t cy0 = std::min(y0, h);
    const std::uint32_t cy1 = std::min(y1, h);
    for (std::uint32_t y = cy0; y < cy1; ++y) {
        for (std::uint32_t x = cx0; x < cx1; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4U;
            const int dr = std::abs(static_cast<int>(rgba[off + 0]) - static_cast<int>(clear_r));
            const int dg = std::abs(static_cast<int>(rgba[off + 1]) - static_cast<int>(clear_g));
            const int db = std::abs(static_cast<int>(rgba[off + 2]) - static_cast<int>(clear_b));
            if (dr + dg + db > 12) {
                ++count;
            }
        }
    }
    return count;
}

std::uint64_t diff_pixels_in_rect(const std::vector<std::uint8_t>& a,
                                  const std::vector<std::uint8_t>& b,
                                  std::uint32_t w,
                                  std::uint32_t h,
                                  std::uint32_t x0,
                                  std::uint32_t y0,
                                  std::uint32_t x1,
                                  std::uint32_t y1) noexcept {
    std::uint64_t diffs = 0;
    const std::uint32_t cx0 = std::min(x0, w);
    const std::uint32_t cx1 = std::min(x1, w);
    const std::uint32_t cy0 = std::min(y0, h);
    const std::uint32_t cy1 = std::min(y1, h);
    for (std::uint32_t y = cy0; y < cy1; ++y) {
        for (std::uint32_t x = cx0; x < cx1; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4U;
            if (a[off + 0] != b[off + 0] || a[off + 1] != b[off + 1] ||
                a[off + 2] != b[off + 2] || a[off + 3] != b[off + 3]) {
                ++diffs;
            }
        }
    }
    return diffs;
}

void test_xml_parser_errors_and_roundtrip() {
    wse::io::XmlElementNode root{};
    std::string err;
    const char* kXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- comment -->\n"
        "<Root count='2' label=\"A &amp; B\">\n"
        "  <Child id=\"1\" flag=\"true\"/>\n"
        "  <Child id='2'>hello</Child>\n"
        "</Root>\n";
    expect(wse::io::parse_xml_document(kXml, root, &err), "XML roundtrip parses valid doc");
    expect(root.tag == "Root", "XML root element tag decoded");
    std::string label_val;
    wse::io::read_xml_attr_str(root, "label", label_val);
    expect(label_val == "A & B", "XML attribute entity decoded");
    expect(root.children.size() == 2U, "XML root child count matches");

    bool flag_val = false;
    wse::io::read_xml_attr_flag_bit(root.children[0], "flag", flag_val);
    expect(flag_val, "XML flag bit sets on true");
    bool keep_default = true;
    wse::io::read_xml_attr_flag_bit(root.children[0], "no_such_flag", keep_default);
    expect(keep_default, "XML flag bit keeps the caller default when the attribute is absent");

    wse::io::XmlElementNode bad{};
    expect(!wse::io::parse_xml_document("<A><B></A>", bad, &err), "XML rejects mismatched tags");
    expect(!wse::io::parse_xml_document("<Unclosed attr='x", bad, &err),
           "XML rejects unterminated tag");
}

void test_font_data_and_text_layout() {
    const char* kFontXml =
        "<FontData width=\"256\" height=\"256\" padding=\"10\" font_size=\"64\" "
        "font_scale=\"100\" line_spacing=\"100\">\n"
        "  <FontDetails>\n"
        "    <character code=\"32\" postshift=\"16\"/>\n"
        "    <character code=\"63\" u=\"20\" v=\"20\" w=\"50\" h=\"60\" preshift=\"1\" yadjust=\"40\" postshift=\"26\"/>\n"
        "    <character code=\"65\" u=\"60\" v=\"20\" w=\"104\" h=\"76\" preshift=\"2\" yadjust=\"48\" postshift=\"40\"/>\n"
        "    <character code=\"66\" u=\"120\" v=\"20\" w=\"164\" h=\"76\" preshift=\"2\" yadjust=\"48\" postshift=\"40\"/>\n"
        "    <character code=\"20013\" u=\"180\" v=\"20\" w=\"224\" h=\"76\" preshift=\"1\" yadjust=\"48\" postshift=\"42\"/>\n"
        "  </FontDetails>\n"
        "</FontData>\n";

    wse::platform::MemoryVfs vfs;
    vfs.write_text("Data/font_data.xml", kFontXml);

    wse::asset::FontData font;
    std::string err;
    expect(wse::asset::load_font_data(vfs, "Data/font_data.xml", font, &err),
           "load_font_data parses synthetic font XML");
    expect(font.width == 256 && font.height == 256 && font.font_size == 64,
           "FontData header dimensions parsed");
    expect(font.get_glyph('A').has_quad() && !font.get_glyph(' ').has_quad(),
           "FontGlyph distinguishes printable glyph vs space");
    expect(font.get_glyph('A').uv_y > font.get_glyph('A').v_bottom,
           "FontGlyph preserves Warband bottom-up V (uv_y > v_bottom)");

    // UTF-8 multi-byte + <c=...> inline color tag + closing </c>
    const std::string colored = "A<c=0xFF0000>B</c>\xE4\xB8\xAD";  // U+4E2D
    const auto res = font.layout_text(colored, 0.0f, 1.0f, 0.5f, 0.04f, 0.04f, 0U, 0xFF112233U);
    expect(res.quads.size() == 3U, "layout_text skips color tags and decodes UTF-8 codepoint");
    expect(res.quads[0].color_argb == 0xFF112233U, "first glyph uses default color");
    expect(res.quads[1].color_argb == 0xFFFF0000U, "tagged glyph forces alpha 0xFF and RGB color");
    expect(res.quads[2].color_argb == 0xFF112233U, "</c> restores default color");
    expect(res.quads[0].y1 > res.quads[0].y0, "FontQuad y1 > y0 in Warband Y-up space");

    // Word-wrap vs single-line + alignment
    const auto wrapped = font.layout_text("AB AB AB", 0.1f, 0.22f, 0.5f, 0.04f, 0.04f,
                                          wse::asset::kTextFlagBoxWrap);
    expect(wrapped.height > 0.05f, "box wrap splits words across multiple lines");

    const auto single = font.layout_text("AB AB AB", 0.1f, 0.22f, 0.5f, 0.04f, 0.04f,
                                         wse::asset::kTextFlagSingleLine);
    expect(close(single.height, 0.04f * 0.9f, 1e-4f), "single line stays one line height");

    const auto right = font.layout_text("A", 0.1f, 0.6f, 0.5f, 0.04f, 0.04f,
                                        wse::asset::kTextFlagRightAlign);
    expect(right.quads.size() == 1U && right.quads[0].x1 <= 0.601f && right.quads[0].x0 > 0.5f,
           "right align places glyph at max_x edge");

    const auto vcenter_override = font.layout_text(
        "A", 0.1f, 0.9f, 0.5f, 0.04f, 0.04f,
        wse::asset::kTextFlagCenterAlign | wse::asset::kTextFlagVCenterOrLeft);
    expect(vcenter_override.quads.size() == 1U && vcenter_override.quads[0].x0 < 0.2f,
           "0x200 without 0x100 cancels horizontal centering per drawText");

    // measure_text reports the line width whatever the alignment.
    const auto left_m = font.measure_text("AB", 0.0f, 0.04f, 0.04f, 0U);
    const auto center_m = font.measure_text("AB", 0.0f, 0.04f, 0.04f, wse::asset::kTextFlagCenterAlign);
    const auto right_m = font.measure_text("AB", 0.0f, 0.04f, 0.04f, wse::asset::kTextFlagRightAlign);
    expect(left_m.first > 0.0f && close(center_m.first, left_m.first) &&
               close(right_m.first, left_m.first),
           "measure_text width does not depend on alignment");
    const auto two_lines = font.measure_text("AB\nAB", 0.0f, 0.04f, 0.04f, 0U);
    expect(close(two_lines.second, 2.0f * 0.04f * 0.9f) && close(two_lines.first, left_m.first),
           "measure_text without a box still breaks lines at newlines");

    wse::asset::FontData bad;
    expect(!bad.parse_xml("<FontData width=\"64\" height=\"64\" font_scale=\"0\">"
                          "<FontDetails/></FontData>", &err),
           "FontData rejects font_scale <= 0");
    expect(!bad.parse_xml("<FontData width=\"0\" height=\"0\"><FontDetails/></FontData>", &err),
           "FontData rejects non-positive dimensions");
    expect(!bad.parse_xml("<FontData width=\"64\" height=\"64\"/>", &err),
           "FontData rejects missing FontDetails");
}

void test_language_and_game_variables() {
    wse::platform::MemoryVfs vfs;
    vfs.write_text("languages/en/ui.csv",
                   "\xEF\xBB\xBF"
                   "ui_start_a_new_game|Start New\\nCampaign\n"
                   "ui_quit|Quit Game\n");
    vfs.write_text("Modules/TestMod/languages/en/ui.csv",
                   "ui_quit|Exit TestMod\n");
    vfs.write_text("Modules/TestMod/game_variables.txt",
                   "# comment\n"
                   "initial_start_new_game_button_position_x = 0.075\n"
                   "initial_start_new_game_button_text_color = 0xFF223344\n");

    wse::game::LanguageTable lang;
    expect(lang.load_from_vfs(vfs, "en", "Modules/TestMod"),
           "LanguageTable loads base + module CSVs");
    expect(lang.get_text("ui_start_a_new_game") == "Start New\nCampaign",
           "LanguageTable strips BOM and unescapes \\n");
    expect(lang.get_text("ui_quit") == "Exit TestMod",
           "module language CSV overrides base key");
    expect(lang.get_text("ui_custom_battle") == "custom battle",
           "missing ui_* key strips prefix and spaces as fallback");

    wse::game::GameVariablesTable vars;
    expect(vars.load_from_vfs(vfs, "Modules/TestMod"), "GameVariablesTable loads module overrides");
    wse::game::UiWidget btn{};
    btn.pos.x = 0.05f;
    btn.text_color_argb = 0xFF110000U;
    btn.apply_properties(vars, "initial_start_new_game_button");
    expect(close(btn.pos.x, 0.075f) && btn.text_color_argb == 0xFF223344U,
           "apply_properties updates widget floats and hex colors");
}

void test_ui_session_windows_and_regressions() {
    wse::game::WseWorldState world{};
    wse::game::UiSession session{};
    session.build_initial_window(world, "Native", /*has_saves=*/false, true, true, true);
    expect(session.current_window == wse::game::kScreenWindowInitial &&
               world.m5.active_window == wse::game::kScreenWindowInitial,
           "build_initial_window sets window 0");
    expect(find_widget(session.root, "backdrop") != nullptr &&
               find_widget(session.root, "menu_background") != nullptr &&
               find_widget(session.root, "game_logo") != nullptr,
           "initial window contains backdrop, sidebar, and logo widgets");
    expect(find_widget(session.root, "module_version_info") == nullptr,
           "initial window hides module label for Native");
    expect(find_widget(session.root, "resume_game_button") == nullptr,
           "initial window omits resume button when has_saves=false");

    const auto* start_btn = find_widget(session.root, "start_new_game_button");
    expect(start_btn != nullptr, "initial window includes start_new_game_button");
    const float hit_x = start_btn->pos.x + start_btn->size.x * 0.5f;
    const float hit_y = start_btn->pos.y + start_btn->size.y * 0.5f;
    const auto evs = session.handle_pointer_and_keys(world, hit_x, hit_y, false, true, false);
    expect(start_btn->highlighted, "pointer hover highlights start_new_game_button");
    expect(evs.size() == 1U && evs[0].kind == wse::game::UiActionKind::StartNewGame,
           "clicking start_new_game_button emits StartNewGame action");

    session.build_initial_window(world, "FlorisMod", true, true, true, true);
    expect(find_widget(session.root, "module_version_info") != nullptr &&
               find_widget(session.root, "resume_game_button") != nullptr,
           "non-Native initial window shows module label and resume button");

    // Menu filtering via condition block + option consequence execution
    wse::game::LoadedMenu menu{};
    menu.id = "mnu_test";
    menu.text = "Choose an action ({reg3} gold).";
    // Menu operations run on open, before the text is formatted.
    menu.operations.statements.push_back(make_stmt(wse::vm::core_op::kAssign, {reg(3), 7}));
    wse::game::LoadedMenuOption hidden_opt{};
    hidden_opt.id = "opt_hidden";
    hidden_opt.text = "Hidden";
    hidden_opt.condition.statements.push_back(make_stmt(wse::vm::core_op::kEq, {0, 1}));
    wse::game::LoadedMenuOption shown_opt{};
    shown_opt.id = "opt_shown";
    shown_opt.text = "Proceed";
    shown_opt.consequence.statements.push_back(make_stmt(wse::vm::core_op::kAssign, {reg(5), 77}));
    menu.options.push_back(std::move(hidden_opt));
    menu.options.push_back(std::move(shown_opt));

    wse::vm::VmContext ctx{};
    std::array<wse::game::LoadedMenu, 1> menus{menu};
    session.build_menu_window(ctx.world, 0, menus, &ctx);
    expect(ctx.world.m5.active_window == wse::game::kScreenWindowMenu &&
               find_widget(session.root, "menu_item_0") == nullptr &&
               find_widget(session.root, "menu_item_1") != nullptr,
           "build_menu_window filters failing condition options and keeps option index");
    const auto* menu_text = find_widget(session.root, "menu_text");
    expect(menu_text != nullptr && menu_text->text == "Choose an action (7 gold).",
           "build_menu_window runs menu operations and formats {reg3} in the menu text");
    expect(wse::game::UiSession::execute_menu_option(ctx, 0, 1, menus) && ctx.registers[5] == 77,
           "execute_menu_option executes selected option consequence");

    // Regression (`STATUS.md` §5 item #1): Notes window 0x15 Return/Escape closes cleanly
    ctx.world.enqueue_window(wse::game::kQueuedWindowNotesType, 0);
    session.open_notes_window(ctx.world, "Factions", "Kingdom of Nords", wse::game::kScreenWindowMap);
    expect(ctx.world.m5.active_window == wse::game::kScreenWindowNotes &&
               ctx.world.m6.ui.active_window_id == wse::game::kScreenWindowNotes,
           "open_notes_window activates window 0x15");
    const auto* ret_btn = find_widget(session.root, "notes_return_button");
    expect(ret_btn != nullptr, "notes window builds return button");
    const auto notes_evs = session.handle_pointer_and_keys(
        ctx.world, ret_btn->pos.x + ret_btn->size.x * 0.5f,
        ret_btn->pos.y + ret_btn->size.y * 0.5f, false, true, false);
    expect(notes_evs.size() == 1U && notes_evs[0].kind == wse::game::UiActionKind::NotesReturn,
           "clicking notes return emits NotesReturn");
    expect(session.current_window == wse::game::kScreenWindowMap &&
               ctx.world.m5.active_window == wse::game::kScreenWindowMap &&
               ctx.world.m6.ui.active_window_id == wse::game::kScreenWindowMap &&
               ctx.world.m5.enqueued_windows.empty(),
           "Notes window Return closes 0x15 cleanly back to map window");
    expect(find_widget(session.root, "notes_return_button") == nullptr,
           "closing notes drops the notes widget tree");
    ctx.world.enqueue_window(wse::game::kQueuedWindowPresentationType, 3);
    const auto stale_evs = session.handle_pointer_and_keys(
        ctx.world, 0.82f, 0.049f, false, true, false);
    expect(stale_evs.empty() && ctx.world.m5.active_window == wse::game::kScreenWindowMap &&
               ctx.world.m5.enqueued_windows.size() == 1U,
           "clicking where the closed notes button was does nothing (no -1 window, queue kept)");
    ctx.world.m5.enqueued_windows.clear();

    // Default prev_window returns to the window that was current (here the menu window).
    session.build_menu_window(ctx.world, 0, menus, &ctx);
    session.open_notes_window(ctx.world, "Factions", "Kingdom of Nords");
    (void)session.handle_pointer_and_keys(ctx.world, 0.0f, 0.0f, false, false, true);
    expect(session.current_window == wse::game::kScreenWindowMenu &&
               ctx.world.m5.active_window == wse::game::kScreenWindowMenu,
           "Escape closes notes opened from the menu back to the menu by default");

    // Presentation triggers -60/-61/-62/-63 receive their parameters through
    // `store_trigger_param_N` (2071/2072), not reg0/reg1.
    constexpr std::uint32_t kStoreTriggerParam1 = 2071U;
    constexpr std::uint32_t kStoreTriggerParam2 = 2072U;
    constexpr std::uint32_t kStartPresentation = 900U;
    constexpr std::uint32_t kPresentationSetDuration = 902U;
    constexpr std::int64_t kSentinel = 0x7777;
    wse::game::LoadedPresentation prsnt{};
    prsnt.id = "prsnt_test";
    prsnt.flags = 1U;  // read-only 10.0s default duration
    auto add_trig = [](wse::game::LoadedPresentation& p, float interval,
                       std::initializer_list<wse::vm::Statement> stmts) {
        wse::game::SimpleTrigger st{};
        st.interval = interval;
        st.block.statements.assign(stmts.begin(), stmts.end());
        p.triggers.triggers.push_back(std::move(st));
    };
    add_trig(prsnt, -60.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(10), 1})});
    add_trig(prsnt, -61.0f, {make_stmt(kStoreTriggerParam1, {reg(11)})});
    add_trig(prsnt, -62.0f, {make_stmt(kStoreTriggerParam1, {reg(12)}),
                             make_stmt(kStoreTriggerParam2, {reg(14)})});
    add_trig(prsnt, -63.0f, {make_stmt(kStoreTriggerParam1, {reg(15)}),
                             make_stmt(kStoreTriggerParam2, {reg(13)})});

    wse::game::WsePresentationOverlay stale_ov{};
    stale_ov.text = u"left over from an earlier presentation";
    ctx.world.overlays.assign(3U, stale_ov);

    std::array<wse::game::LoadedPresentation, 1> prsnts{prsnt};
    expect(session.start_presentation(ctx.world, ctx, 0, prsnts) && ctx.registers[10] == 1,
           "start_presentation fires -60 trigger");
    expect(ctx.world.overlays.empty(), "start_presentation clears the previous overlays");
    expect(ctx.world.presentation_duration == 1000,
           "read-only presentation arms a 10.0s (1000 cs) duration");

    wse::game::WsePresentationOverlay ov{};
    ov.type = 1;  // create_button_overlay / create_game_button_overlay
    ov.m5.is_button = true;
    ov.text = u"Press";
    ov.position = {0.2f, 0.2f};
    ov.val = 5;
    ctx.world.overlays.push_back(ov);

    ctx.registers[0] = 4242;
    ctx.registers[1] = 4343;
    for (const std::size_t r : {11U, 12U, 13U, 14U, 15U}) {
        ctx.registers[r] = kSentinel;
    }
    expect(session.step_presentation(ctx.world, ctx, 0.5f, prsnts) && ctx.registers[11] == 500,
           "step_presentation passes elapsed ms as trigger param 1 to -61");
    expect(ctx.world.presentation_duration == 950, "step_presentation counts the duration down");
    const auto* btn_w = find_widget(session.root, "overlay_0");
    expect(btn_w != nullptr && btn_w->kind == wse::game::UiWidgetKind::GameButton &&
               close(btn_w->size.x, wse::game::kUiOverlayButtonWidth) &&
               close(btn_w->size.y, wse::game::kUiOverlayButtonHeight),
           "text button overlay at scale 1 gets the default button hit box");

    const auto prsnt_evs = session.handle_pointer_and_keys(
        ctx.world, 0.30f, 0.22f, false, true, false, &ctx, prsnts);
    expect(prsnt_evs.size() == 1U && prsnt_evs[0].kind == wse::game::UiActionKind::OverlayClicked,
           "clicking the button overlay emits OverlayClicked");
    expect(ctx.registers[15] == 0 && ctx.registers[13] == 0,
           "hover fires -63 with param 1 = overlay 0, param 2 = 0 (enter)");
    expect(ctx.registers[12] == 0 && ctx.registers[14] == 5,
           "click fires -62 with param 1 = overlay 0, param 2 = overlay value");
    expect(ctx.registers[0] == 4242 && ctx.registers[1] == 4343,
           "presentation triggers leave reg0/reg1 untouched");

    (void)session.handle_pointer_and_keys(ctx.world, 0.9f, 0.7f, false, false, false, &ctx, prsnts);
    expect(ctx.registers[13] == 1, "moving pointer off overlay fires -63 leave (param 2 = 1)");

    // Overlay colours are 0xRRGGBB with the alpha from overlay_set_alpha; flags 0 = left.
    wse::game::WsePresentationOverlay txt{};
    txt.type = 1;
    txt.text = u"Label";
    txt.color = 0xFF0000U;  // overlay_set_color stores RGB only
    txt.alpha = 0.5f;
    ctx.world.overlays.push_back(txt);
    wse::game::WsePresentationOverlay empty_txt{};
    empty_txt.type = 1;  // create_text_overlay with an empty placeholder string
    ctx.world.overlays.push_back(empty_txt);
    wse::game::WsePresentationOverlay mesh_ov{};
    mesh_ov.type = 0;
    mesh_ov.m5.mesh_no = 0;
    mesh_ov.position = {0.1f, 0.1f};
    mesh_ov.size = {0.5f, 0.5f};
    ctx.world.overlays.push_back(mesh_ov);
    ctx.world.mesh_names = {"test_mesh"};
    session.mesh_bounds["test_mesh"] = wse::game::UiMeshBounds{0.0f, 0.0f, 1.0f, 0.75f};
    session.sync_overlays_from_world(ctx.world);
    const auto* txt_w = find_widget(session.root, "overlay_1");
    expect(txt_w != nullptr && txt_w->text_color_argb == 0x80FF0000U && txt_w->text_flags == 0U &&
               txt_w->kind == wse::game::UiWidgetKind::Label,
           "text overlay composes RGB + overlay alpha and keeps flags 0 (left aligned)");
    const auto* empty_w = find_widget(session.root, "overlay_2");
    expect(empty_w != nullptr && empty_w->kind == wse::game::UiWidgetKind::Label &&
               empty_w->size.x == 0.0f && empty_w->size.y == 0.0f,
           "empty text overlay is an empty label, not a white panel");
    const auto* mesh_w = find_widget(session.root, "overlay_3");
    expect(mesh_w != nullptr && mesh_w->kind == wse::game::UiWidgetKind::Image &&
               close(mesh_w->size.x, 0.5f) && close(mesh_w->size.y, 0.375f) && mesh_w->tint_mesh,
           "mesh overlay spans its native mesh extent times overlay_set_size");

    // Escape never closes a presentation (retail `mbPresentationWindow::frameMove` has no key
    // check; modules poll `key_clicked` in their own triggers), with or without
    // prsntf_manual_end_only (0x2).
    const auto esc_evs =
        session.handle_pointer_and_keys(ctx.world, 0.9f, 0.7f, false, false, true, &ctx, prsnts);
    expect(esc_evs.empty() && ctx.world.active_presentation_no == 0 &&
               session.current_window == wse::game::kScreenWindowPresentation,
           "Escape leaves the presentation open and emits no event");
    session.close_active_modal_window(ctx.world);
    expect(ctx.world.active_presentation_no == -1 &&
               session.current_window == wse::game::kScreenWindowMenu,
           "closing the presentation returns to the window it was opened from");

    prsnts[0].flags = wse::game::kPresentationFlagManualEndOnly;
    expect(session.start_presentation(ctx.world, ctx, 0, prsnts), "restart presentation 0");
    (void)session.handle_pointer_and_keys(ctx.world, 0.9f, 0.7f, false, false, true, &ctx, prsnts);
    expect(ctx.world.active_presentation_no == 0 &&
               session.current_window == wse::game::kScreenWindowPresentation,
           "Escape leaves a manual_end_only presentation open too");
    session.close_active_modal_window(ctx.world);

    // presentation_set_duration (902): a long duration outlives the default, 0 closes it.
    prsnts[0].flags = 0U;
    prsnts[0].triggers.triggers[0].block.statements.push_back(
        make_stmt(kPresentationSetDuration, {999999}));
    expect(session.start_presentation(ctx.world, ctx, 0, prsnts) &&
               session.step_presentation(ctx.world, ctx, 200.0f, prsnts),
           "presentation_set_duration 999999 keeps the presentation open past the default");
    prsnts[0].triggers.triggers[2].block.statements.push_back(
        make_stmt(kPresentationSetDuration, {0}));
    ctx.world.overlays.push_back(ov);
    session.sync_overlays_from_world(ctx.world);
    (void)session.handle_pointer_and_keys(ctx.world, 0.30f, 0.22f, false, true, false, &ctx, prsnts);
    expect(ctx.world.active_presentation_no == 0 &&
               session.current_window == wse::game::kScreenWindowPresentation,
           "trigger copy is taken at start: later edits to the loaded presentation do not apply");
    session.close_active_modal_window(ctx.world);
    expect(session.start_presentation(ctx.world, ctx, 0, prsnts), "restart with set_duration 0 on click");
    ctx.world.overlays.push_back(ov);
    session.sync_overlays_from_world(ctx.world);
    (void)session.handle_pointer_and_keys(ctx.world, 0.30f, 0.22f, false, true, false, &ctx, prsnts);
    expect(ctx.world.active_presentation_no == -1 &&
               session.current_window == wse::game::kScreenWindowMenu,
           "presentation_set_duration 0 from -62 closes window 0x16");

    // start_presentation (900) from a running presentation switches to the new one.
    wse::game::LoadedPresentation second{};
    second.id = "prsnt_second";
    add_trig(second, -60.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(16), 1})});
    wse::game::LoadedPresentation first = prsnt;
    first.flags = 0U;
    first.triggers.triggers[2].block.statements.push_back(make_stmt(kStartPresentation, {1}));
    std::array<wse::game::LoadedPresentation, 2> pair{first, second};
    expect(session.start_presentation(ctx.world, ctx, 0, pair), "start presentation 0 of 2");
    ctx.world.overlays.push_back(ov);
    session.sync_overlays_from_world(ctx.world);
    (void)session.handle_pointer_and_keys(ctx.world, 0.30f, 0.22f, false, true, false, &ctx, pair);
    expect(ctx.world.active_presentation_no == 1 && ctx.registers[16] == 1 &&
               ctx.world.overlays.empty() && ctx.world.m5.enqueued_windows.empty(),
           "start_presentation from -62 switches presentations, firing the new -60");
    expect(!session.step_presentation(ctx.world, ctx, 101.0f, pair) &&
               ctx.world.active_presentation_no == -1 &&
               ctx.world.m5.active_window == wse::game::kScreenWindowMenu,
           "presentation auto-closes 0x16 when its duration runs out");

    // Native refresh idiom (`prsnt_game_custom_battle_designer`, `prsnt_multiplayer_item_select`,
    // ...): -62 runs `presentation_set_duration 0` + `start_presentation <self>`, which restarts
    // the open presentation instead of closing it.
    wse::game::LoadedPresentation refresh{};
    refresh.id = "prsnt_refresh";
    add_trig(refresh, -60.0f, {make_stmt(wse::vm::core_op::kValAdd, {reg(17), 1})});
    add_trig(refresh, -62.0f, {make_stmt(kPresentationSetDuration, {0}),
                               make_stmt(kStartPresentation, {0})});
    std::array<wse::game::LoadedPresentation, 1> refresh_set{refresh};
    ctx.registers[17] = 0;
    expect(session.start_presentation(ctx.world, ctx, 0, refresh_set) && ctx.registers[17] == 1,
           "refresh presentation starts and fires -60 once");
    for (const bool in_scene : {false, true}) {
        ctx.world.lighting.has_scene = in_scene;
        const std::int64_t loads_before = ctx.registers[17];
        ctx.world.overlays.push_back(ov);
        session.sync_overlays_from_world(ctx.world);
        (void)session.handle_pointer_and_keys(ctx.world, 0.30f, 0.22f, false, true, false, &ctx,
                                              refresh_set);
        expect(ctx.registers[17] == loads_before + 1 &&
                   ctx.world.active_presentation_no == 0 &&
                   session.current_window == wse::game::kScreenWindowPresentation &&
                   ctx.world.overlays.empty() && ctx.world.presentation_duration == 10000 &&
                   ctx.world.m5.enqueued_windows.empty(),
               in_scene ? "self-restart inside a scene (no queued window) also restarts"
                        : "set_duration 0 + start_presentation <self> restarts instead of closing");
        expect(session.step_presentation(ctx.world, ctx, 0.5f, refresh_set) &&
                   ctx.registers[17] == loads_before + 1,
               "a handled self-restart is not replayed on the next frame");
    }
    ctx.world.lighting.has_scene = false;
    session.close_active_modal_window(ctx.world);
}

// --- Interactive presentation overlays (`UiSession::handle_input`) ---------------------------
// Presentation with one counting trigger per interactive event. Registers: -60 count 18;
// -62 params 12 / 14, count 20; -63 params 15 / 13, count 21; -64 params 16 / 17, count 22.
struct OverlayRig {
    std::unique_ptr<wse::vm::VmContext> ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    wse::game::UiSession session{};
    std::vector<wse::game::LoadedPresentation> prs{};
    static constexpr std::int64_t kSentinel = 0x7777;

    explicit OverlayRig(std::initializer_list<wse::vm::Statement> extra_62 = {}) {
        wse::game::LoadedPresentation p{};
        p.id = "prsnt_input";
        const auto add_trigger = [&p](float key, std::vector<wse::vm::Statement> stmts) {
            wse::game::SimpleTrigger st{};
            st.interval = key;
            st.block.statements = std::move(stmts);
            p.triggers.triggers.push_back(std::move(st));
        };
        add_trigger(-60.0f, {make_stmt(wse::vm::core_op::kValAdd, {reg(18), 1})});
        std::vector<wse::vm::Statement> t62{make_stmt(2071U, {reg(12)}), make_stmt(2072U, {reg(14)}),
                                            make_stmt(wse::vm::core_op::kValAdd, {reg(20), 1})};
        t62.insert(t62.end(), extra_62.begin(), extra_62.end());
        add_trigger(-62.0f, std::move(t62));
        add_trigger(-63.0f, {make_stmt(2071U, {reg(15)}), make_stmt(2072U, {reg(13)}),
                             make_stmt(wse::vm::core_op::kValAdd, {reg(21), 1})});
        add_trigger(-64.0f, {make_stmt(2071U, {reg(16)}), make_stmt(2072U, {reg(17)}),
                             make_stmt(wse::vm::core_op::kValAdd, {reg(22), 1})});
        prs.push_back(std::move(p));
        reset_regs();
        ctx.registers[18] = 0;
        (void)session.start_presentation(ctx.world, ctx, 0, prs);
    }

    void reset_regs() {
        for (const std::size_t r : {12U, 13U, 14U, 15U, 16U, 17U}) {
            ctx.registers[r] = kSentinel;
        }
        for (const std::size_t r : {20U, 21U, 22U}) {
            ctx.registers[r] = 0;
        }
    }
    std::int64_t n62() const { return ctx.registers[20]; }
    std::int64_t n63() const { return ctx.registers[21]; }
    std::int64_t n64() const { return ctx.registers[22]; }

    std::int32_t add(const wse::game::WsePresentationOverlay& o) {
        ctx.world.overlays.push_back(o);
        session.sync_overlays_from_world(ctx.world);
        return static_cast<std::int32_t>(ctx.world.overlays.size() - 1U);
    }
    wse::game::WsePresentationOverlay& ov(std::int32_t id) {
        return ctx.world.overlays[static_cast<std::size_t>(id)];
    }
    std::vector<wse::game::UiEvent> send(const wse::game::UiPointerInput& in) {
        return session.handle_input(ctx.world, in, &ctx, prs);
    }
    std::vector<wse::game::UiEvent> move(float x, float y, bool down = false, float t = 0.0f) {
        wse::game::UiPointerInput in{};
        in.x = x;
        in.y = y;
        in.left_down = down;
        in.time_s = t;
        return send(in);
    }
    std::vector<wse::game::UiEvent> press(float x, float y, float t = 0.0f) {
        wse::game::UiPointerInput in{};
        in.x = x;
        in.y = y;
        in.left_down = true;
        in.left_pressed = true;
        in.time_s = t;
        return send(in);
    }
    std::vector<wse::game::UiEvent> release(float x, float y, float t = 0.0f) {
        wse::game::UiPointerInput in{};
        in.x = x;
        in.y = y;
        in.left_released = true;
        in.time_s = t;
        return send(in);
    }
};

wse::game::WsePresentationOverlay boxed_text(float x, float y, float w, float h) {
    wse::game::WsePresentationOverlay o{};
    o.type = 1;
    o.position = {x, y};
    o.m5.area_size = {w, h};
    return o;
}

wse::game::WsePresentationOverlay box_button(float x, float y, float w, float h) {
    wse::game::WsePresentationOverlay o = boxed_text(x, y, w, h);
    o.text = u"Btn";
    o.m5.is_button = true;
    o.val = 5;
    return o;
}

void test_overlay_hover_and_press_states() {
    // 1. -63 enter / leave reaches overlays that are not buttons.
    {
        OverlayRig rig;
        const std::int32_t t = rig.add(boxed_text(0.4f, 0.4f, 0.2f, 0.1f));
        rig.move(0.9f, 0.9f);
        expect(rig.n63() == 0, "no -63 while the cursor is outside every overlay");
        rig.move(0.45f, 0.45f);
        expect(rig.n63() == 1 && rig.ctx.registers[15] == t && rig.ctx.registers[13] == 0,
               "text overlay with a box gets -63 enter (param 2 = 0)");
        rig.move(0.46f, 0.46f);
        expect(rig.n63() == 1, "moving inside the box fires no second -63");
        rig.move(0.9f, 0.9f);
        expect(rig.n63() == 2 && rig.ctx.registers[15] == t && rig.ctx.registers[13] == 1,
               "text overlay gets -63 leave (param 2 = 1)");
        expect(rig.n64() == 0 && rig.n62() == 0, "hover alone fires neither -64 nor -62");
    }
    {
        OverlayRig rig;
        rig.ctx.world.mesh_names = {"hover_mesh"};
        rig.session.mesh_bounds["hover_mesh"] = wse::game::UiMeshBounds{0.0f, 0.0f, 0.2f, 0.1f};
        wse::game::WsePresentationOverlay m{};
        m.type = 0;
        m.m5.mesh_no = 0;
        m.position = {0.1f, 0.1f};
        m.size = {1.0f, 1.0f};
        const std::int32_t id = rig.add(m);
        rig.move(0.15f, 0.15f);
        expect(rig.n63() == 1 && rig.ctx.registers[15] == id && rig.ctx.registers[13] == 0,
               "mesh overlay with mesh bounds gets -63 enter");
        rig.move(0.5f, 0.5f);
        expect(rig.n63() == 2 && rig.ctx.registers[13] == 1, "mesh overlay gets -63 leave");
    }

    // 2. -64 mouse press: param 2 = 0 / 1 / 2 for left / right / middle; buttons consume left.
    {
        OverlayRig rig;
        const std::int32_t under = rig.add(boxed_text(0.2f, 0.2f, 0.3f, 0.1f));
        const std::int32_t top = rig.add(box_button(0.25f, 0.22f, 0.1f, 0.05f));
        rig.move(0.27f, 0.23f);
        rig.reset_regs();
        rig.press(0.27f, 0.23f);
        expect(rig.n64() == 1 && rig.ctx.registers[16] == top && rig.ctx.registers[17] == 0,
               "left press: the button on top gets -64 (param 2 = 0) and consumes it");
        rig.release(0.27f, 0.23f);
        rig.reset_regs();
        wse::game::UiPointerInput in{};
        in.x = 0.27f;
        in.y = 0.23f;
        in.right_pressed = true;
        rig.send(in);
        expect(rig.n64() == 2 && rig.ctx.registers[16] == top && rig.ctx.registers[17] == 1,
               "right press is not consumed: both overlays get -64 (param 2 = 1)");
        rig.reset_regs();
        in.right_pressed = false;
        in.middle_pressed = true;
        rig.send(in);
        expect(rig.n64() == 2 && rig.ctx.registers[17] == 2,
               "middle press gives -64 with param 2 = 2 to both overlays");
        rig.reset_regs();
        rig.press(0.45f, 0.25f);
        expect(rig.n64() == 1 && rig.ctx.registers[16] == under && rig.ctx.registers[17] == 0,
               "a left press beside the button reaches the text overlay below");
    }

    // 3. Button: press + release fires -62 once and emits OverlayClicked; leaving disarms.
    {
        OverlayRig rig;
        const std::int32_t b = rig.add(box_button(0.5f, 0.5f, 0.2f, 0.05f));
        const auto pe = rig.press(0.55f, 0.52f);
        expect(pe.empty() && rig.n62() == 0, "button press alone fires no -62 and no event");
        const auto re = rig.release(0.55f, 0.52f);
        expect(rig.n62() == 1 && rig.ctx.registers[12] == b && rig.ctx.registers[14] == 5,
               "button release over it fires -62 once with the overlay value");
        expect(re.size() == 1U && re[0].kind == wse::game::UiActionKind::OverlayClicked &&
                   re[0].param == b,
               "button release emits one OverlayClicked event");
        rig.release(0.55f, 0.52f);
        expect(rig.n62() == 1, "a second release without a press fires nothing");
        rig.press(0.55f, 0.52f);
        rig.move(0.9f, 0.9f, true);
        rig.move(0.55f, 0.52f, true);
        const auto le = rig.release(0.55f, 0.52f);
        expect(rig.n62() == 1 && le.empty(),
               "press, leave, return and release does not fire (leave disarms)");
    }

    // 4. Check box toggles on every left press.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay c = boxed_text(0.3f, 0.3f, 0.05f, 0.05f);
        c.type = 4;
        c.m5.is_checkbox = true;
        c.m5.create_opcode = 918;
        const std::int32_t id = rig.add(c);
        rig.press(0.32f, 0.32f);
        expect(rig.ov(id).val == 1 && rig.n62() == 1 && rig.ctx.registers[14] == 1,
               "check box press sets 1 and -62 reports 1");
        rig.release(0.32f, 0.32f);
        expect(rig.n62() == 1, "check box release fires no extra -62");
        rig.press(0.32f, 0.32f);
        expect(rig.ov(id).val == 0 && rig.n62() == 2 && rig.ctx.registers[14] == 0,
               "second press clears it and -62 reports 0");
        rig.release(0.32f, 0.32f);
        rig.press(0.32f, 0.32f);
        expect(rig.ov(id).val == 1 && rig.ctx.registers[14] == 1 && rig.ctx.registers[12] == id,
               "third press sets 1 again");
    }
}

void test_overlay_value_widgets() {
    // 5. Slider.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay s{};
        s.type = 2;
        s.m5.create_opcode = 914;
        s.m5.min_value = 0.0;
        s.m5.max_value = 100.0;
        s.val = 0;
        s.position = {0.1f, 0.3f};
        const std::int32_t id = rig.add(s);
        const float span = wse::game::kUiSliderWidth - 2.0f * wse::game::kUiSliderMargin;
        const auto x_at = [&](float frac) {
            return 0.1f + wse::game::kUiSliderMargin + frac * span;
        };
        rig.press(x_at(0.5f), 0.315f);
        expect(std::abs(rig.ov(id).val - 50) <= 1 && rig.n62() == 1 &&
                   rig.ctx.registers[14] == rig.ov(id).val,
               "slider press away from the knob jumps to the cursor and fires -62");
        rig.move(x_at(0.75f), 0.315f, true);
        expect(std::abs(rig.ov(id).val - 75) <= 1 && rig.n62() == 2 &&
                   rig.ctx.registers[14] == rig.ov(id).val,
               "dragging fires -62 with the new truncated value");
        rig.move(x_at(0.75f), 0.315f, true);
        expect(rig.n62() == 2, "a drag frame without cursor movement fires nothing");
        rig.release(x_at(0.75f), 0.315f);
        const std::int32_t kept = rig.ov(id).val;
        const std::int64_t fired = rig.n62();
        rig.move(x_at(0.2f), 0.315f, true);
        rig.move(x_at(0.3f), 0.315f, true);
        expect(rig.ov(id).val == kept && rig.n62() == fired,
               "after release, moving the cursor no longer changes the slider");
        rig.move(0.9f, 0.9f);
        rig.press(0.349f, 0.315f);
        expect(rig.ov(id).val == 100, "slider value clamps at the maximum");
        rig.release(0.349f, 0.315f);
        rig.press(0.1005f, 0.315f);
        expect(rig.ov(id).val == 0 && rig.ctx.registers[14] == 0,
               "slider value clamps at the minimum");
    }

    // 6. Number box.
    {
        OverlayRig rig;
        (void)rig.add(boxed_text(0.2f, 0.4f, 0.4f, 0.3f));
        wse::game::WsePresentationOverlay n{};
        n.type = 2;
        n.m5.create_opcode = 942;
        n.m5.min_value = 0.0;
        n.m5.max_value = 10.0;
        n.val = 5;
        n.position = {0.3f, 0.5f};
        const std::int32_t id = rig.add(n);
        rig.move(0.36f, 0.52f);
        rig.reset_regs();
        rig.press(0.40f, 0.52f);
        expect(rig.ov(id).val == 6 && rig.n62() == 1 && rig.ctx.registers[14] == 6 &&
                   rig.n64() == 1,
               "number box right arrow adds 1 and consumes the press");
        rig.release(0.40f, 0.52f);
        rig.reset_regs();
        rig.press(0.31f, 0.52f);
        expect(rig.ov(id).val == 5 && rig.ctx.registers[14] == 5, "left arrow subtracts 1");
        rig.release(0.31f, 0.52f);
        rig.reset_regs();
        rig.press(0.36f, 0.52f);
        expect(rig.ov(id).val == 5 && rig.n62() == 0 && rig.n64() == 1 &&
                   rig.ctx.world.m5.focused_overlay_no == id && rig.ov(id).text == u"5",
               "a press in the middle focuses the numeric text box and is consumed");
        rig.release(0.36f, 0.52f);
        const auto type = [&](std::u16string_view chars) {
            wse::game::UiPointerInput in{};
            in.x = 0.36f;
            in.y = 0.52f;
            in.text_input = std::u16string(chars);
            return rig.send(in);
        };
        rig.reset_regs();
        type(u"3");
        expect(rig.ov(id).val == 9 && rig.ov(id).text == u"9" && rig.n62() == 0,
               "typing a digit makes value*10+digit, clamped below the excluded maximum");
        type(u"\bx7");
        expect(rig.ov(id).val == 7 && rig.ov(id).text == u"7",
               "backspace erases a character; non-digits are ignored");
        const auto* nw = find_widget(rig.session.root, "overlay_1");
        expect(nw != nullptr && nw->text == "7", "the focused number box shows the typed text");
        type(u"\r");
        expect(rig.ov(id).val == 7 && rig.n62() == 1 && rig.ctx.registers[14] == 7 &&
                   rig.ctx.world.m5.focused_overlay_no == -1 && !rig.ov(id).m5.focused,
               "Enter commits the typed value and fires -62 with it");
        rig.press(0.36f, 0.52f);
        rig.release(0.36f, 0.52f);
        type(u"\b");
        rig.reset_regs();
        rig.press(0.40f, 0.52f);
        expect(rig.ov(id).val == 0 && rig.n62() == 1 && rig.ctx.world.m5.focused_overlay_no == -1,
               "a press on an arrow while typing only commits (empty text reads as 0)");
        rig.release(0.40f, 0.52f);
        rig.ov(id).val = 10;
        rig.press(0.40f, 0.52f);
        expect(rig.ov(id).val == 9, "number box clamps below the excluded maximum (max - 1)");
        rig.release(0.40f, 0.52f);
        rig.ov(id).val = 0;
        rig.press(0.31f, 0.52f);
        expect(rig.ov(id).val == 0, "number box clamps at the minimum");
        rig.release(0.31f, 0.52f);

        // Auto-repeat while the arrow is held.
        rig.ov(id).val = 2;
        rig.press(0.40f, 0.52f, 0.0f);
        expect(rig.ov(id).val == 3, "held arrow: the press steps once");
        rig.move(0.40f, 0.52f, true, 0.3f);
        expect(rig.ov(id).val == 3, "no repeat before 0.5 s");
        rig.move(0.40f, 0.52f, true, 0.55f);
        expect(rig.ov(id).val == 4, "first repeat after 0.5 s");
        rig.move(0.40f, 0.52f, true, 0.62f);
        expect(rig.ov(id).val == 4, "no repeat before the 0.1 s interval has passed");
        rig.move(0.40f, 0.52f, true, 0.70f);
        expect(rig.ov(id).val == 5, "next repeat 0.1 s later");
        rig.release(0.40f, 0.52f, 0.71f);
        rig.move(0.40f, 0.52f, true, 2.0f);
        rig.move(0.40f, 0.52f, true, 2.5f);
        expect(rig.ov(id).val == 5, "releasing stops the auto-repeat");

        // Empty range (max <= min): setIntValue gives -1 and clears the text.
        rig.ov(id).m5.min_value = 5.0;
        rig.ov(id).m5.max_value = 5.0;
        rig.ov(id).val = 5;
        rig.press(0.36f, 0.52f);
        rig.release(0.36f, 0.52f);
        wse::game::UiPointerInput in{};
        in.x = 0.36f;
        in.y = 0.52f;
        in.text_input = u"3";
        rig.send(in);
        // Text "5" + "3" = 53, clamped to [0, max - 1] = [0, 4].
        const bool typed_ok = rig.ov(id).val == 4 && rig.ov(id).text == u"4";
        in.text_input = u"\r";
        rig.send(in);
        expect(typed_ok && rig.ov(id).val == -1 && rig.ov(id).text.empty(),
               "an empty range types against a minimum of 0, then commits to -1");
    }

    // 7. Combo label.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay c{};
        c.type = 5;
        c.m5.create_opcode = 948;
        c.m5.items = {u"a", u"b", u"c"};
        c.val = 0;
        c.position = {0.3f, 0.6f};
        const std::int32_t id = rig.add(c);
        const std::array<std::int32_t, 3> expect_right{1, 2, 0};
        bool right_ok = true;
        for (std::size_t k = 0; k < expect_right.size(); ++k) {
            rig.press(0.50f, 0.62f);
            rig.release(0.50f, 0.62f);
            right_ok = right_ok && rig.ov(id).val == expect_right[k] &&
                       rig.n62() == static_cast<std::int64_t>(k + 1U) &&
                       rig.ctx.registers[14] == expect_right[k];
        }
        expect(right_ok, "combo label right arrow steps 0->1->2->0 and fires -62 each time");
        rig.press(0.31f, 0.62f);
        expect(rig.ov(id).val == 2 && rig.n62() == 4 && rig.ctx.registers[14] == 2,
               "combo label left arrow wraps 0->2 and fires -62");
    }

    // 8. Combo button popup.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay c{};
        c.type = 5;
        c.m5.create_opcode = 916;
        c.m5.items = {u"a", u"b", u"c"};
        c.val = 0;
        c.position = {0.3f, 0.6f};
        const std::int32_t id = rig.add(c);
        expect(rig.session.combo_popup_overlay() == -1, "no combo popup before a press");
        rig.press(0.4f, 0.62f);
        expect(rig.session.combo_popup_overlay() == id &&
                   find_widget(rig.session.root, "combo_popup") != nullptr,
               "combo button press opens the popup and draws it");
        rig.release(0.4f, 0.62f);
        const float item2_y = 0.6f - 2.5f * wse::game::kUiInputBoxHeight;  // item = box height
        rig.press(0.4f, item2_y);
        expect(rig.ov(id).val == 2 && rig.n62() == 1 && rig.ctx.registers[14] == 2 &&
                   rig.session.combo_popup_overlay() == -1 &&
                   find_widget(rig.session.root, "combo_popup") == nullptr,
               "pressing item 2 selects it, fires -62 and closes the popup");
        rig.release(0.4f, item2_y);
        rig.press(0.4f, 0.62f);
        rig.release(0.4f, 0.62f);
        expect(rig.session.combo_popup_overlay() == id, "combo popup reopens");
        rig.press(0.9f, 0.9f);
        expect(rig.ov(id).val == 2 && rig.n62() == 1 && rig.session.combo_popup_overlay() == -1 &&
                   find_widget(rig.session.root, "combo_popup") == nullptr,
               "pressing elsewhere closes the popup without changing the value");
    }
}

void test_overlay_containers_and_dispatch_stop() {
    // 9. Scrollable container nesting, clipping and wheel.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay c = boxed_text(0.1f, 0.1f, 0.3f, 0.2f);
        c.val = 0x20000;
        const std::int32_t cid = rig.add(c);
        std::array<std::int32_t, 3> kids{};
        for (std::size_t k = 0; k < kids.size(); ++k) {
            wse::game::WsePresentationOverlay child =
                boxed_text(0.0f, 0.1f * static_cast<float>(k), 0.5f, 0.1f);
            child.m5.container_overlay_no = cid;
            kids[k] = rig.add(child);
        }
        const auto* cw = find_widget(rig.session.root, "overlay_0");
        expect(rig.session.root.children.size() == 1U && cw != nullptr &&
                   cw->children.size() == 3U && cw->clip_children &&
                   find_widget(*cw, "overlay_2") != nullptr,
               "container children are nested under the container widget, not at the top");
        expect(cw != nullptr && close(cw->child_offset.y, -0.1f),
               "content top starts at the container area top");

        rig.move(0.45f, 0.15f);
        expect(rig.n63() == 0, "a child outside the clip area gets no hover -63");
        rig.move(0.15f, 0.15f);
        expect(rig.n63() == 2 && rig.ctx.registers[15] == kids[1],
               "a child inside the clip area gets -63 (with its container)");
        rig.move(0.15f, 0.05f);
        expect(rig.n63() == 4 && rig.ctx.registers[13] == 1,
               "moving onto a clipped-away child fires leave only, no enter");

        const auto offset_y = [&]() {
            const auto* w = find_widget(rig.session.root, "overlay_0");
            return w != nullptr ? w->child_offset.y : 99.0f;
        };
        wse::game::UiPointerInput in{};
        in.x = 0.15f;
        in.y = 0.2f;
        in.wheel = -1.0f;
        rig.send(in);
        expect(close(offset_y(), -0.05f), "wheel down scrolls the content by one step");
        in.wheel = -10.0f;
        rig.send(in);
        expect(close(offset_y(), 0.0f), "wheel scrolling clamps at the end of the content");
        in.wheel = 1.0f;
        rig.send(in);
        expect(close(offset_y(), -0.05f), "wheel up scrolls back from the clamped end");
        in.wheel = 10.0f;
        rig.send(in);
        expect(close(offset_y(), -0.1f), "wheel scrolling clamps at the start of the content");
    }
    // Container cycle: nobody hangs and both overlays stay at the top.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay a = boxed_text(0.1f, 0.1f, 0.2f, 0.2f);
        a.m5.container_overlay_no = 1;
        wse::game::WsePresentationOverlay b = boxed_text(0.5f, 0.1f, 0.2f, 0.2f);
        b.m5.container_overlay_no = 0;
        rig.add(a);
        rig.add(b);
        expect(rig.session.root.children.size() == 2U &&
                   rig.session.root.children[0].children.empty() &&
                   rig.session.root.children[1].children.empty(),
               "a container cycle leaves both overlays at the top");
    }
    // A child of a hidden container is not built.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay c = boxed_text(0.1f, 0.1f, 0.3f, 0.2f);
        c.visible = false;
        rig.add(c);
        wse::game::WsePresentationOverlay child = boxed_text(0.0f, 0.0f, 0.1f, 0.1f);
        child.m5.container_overlay_no = 0;
        rig.add(child);
        expect(rig.session.root.children.empty() &&
                   find_widget(rig.session.root, "overlay_1") == nullptr,
               "a child of a hidden container is hidden too");
    }

    // 10. A -62 trigger that closes or restarts the presentation stops later dispatches.
    for (const bool restart : {false, true}) {
        OverlayRig rig = restart ? OverlayRig{make_stmt(902U, {0}), make_stmt(900U, {0})}
                                 : OverlayRig{make_stmt(902U, {0})};
        wse::game::WsePresentationOverlay c = boxed_text(0.3f, 0.3f, 0.1f, 0.1f);
        c.type = 4;
        c.m5.is_checkbox = true;
        c.m5.create_opcode = 918;
        rig.add(c);
        rig.add(boxed_text(0.3f, 0.3f, 0.1f, 0.1f));
        rig.move(0.32f, 0.32f);
        rig.reset_regs();
        rig.press(0.32f, 0.32f);
        expect(rig.n62() == 1 && rig.n64() == 0 && rig.n63() == 0,
               restart ? "a -62 that restarts the presentation stops the later dispatches"
                       : "a -62 that closes the presentation stops the later dispatches");
        if (restart) {
            expect(rig.ctx.registers[18] == 2 && rig.ctx.world.overlays.empty() &&
                       rig.session.current_window == wse::game::kScreenWindowPresentation,
                   "the restart re-ran -60 and cleared the overlays");
        } else {
            expect(rig.session.current_window != wse::game::kScreenWindowPresentation &&
                       rig.ctx.world.active_presentation_no == -1,
                   "the presentation is closed");
        }
    }
}

// `UiSession::drive_frame`: the per-frame glue that opens queued notes / presentation windows
// after `step_world_frame`, steps presentations, and drops modal windows when a scene starts.
void test_ui_session_drive_frame() {
    constexpr std::uint32_t kStartPresentation = 900U;
    constexpr std::uint32_t kPresentationSetDuration = 902U;
    constexpr std::uint32_t kChangeScreenNotes = 2053U;
    auto ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    wse::game::WseWorldState& world = ctx.world;
    world.has_game = true;
    world.lighting.has_scene = false;  // campaign map (the struct default is a mission scene)
    world.troops.resize(1U);
    world.troops[0].name = "Lord Test";
    world.troops[0].notes[0].text = u"Rules the test march.";
    world.m5.active_window = wse::game::kScreenWindowMap;
    world.m6.ui.active_window_id = wse::game::kScreenWindowMap;

    wse::game::LoadedPresentation prsnt{};
    prsnt.id = "prsnt_driven";
    wse::game::SimpleTrigger load{};
    load.interval = -60.0f;
    load.block.statements = {make_stmt(wse::vm::core_op::kValAdd, {reg(20), 1}),
                             make_stmt(kPresentationSetDuration, {50})};
    prsnt.triggers.triggers.push_back(load);
    wse::game::SimpleTrigger run{};
    run.interval = -61.0f;
    run.block.statements = {make_stmt(wse::vm::core_op::kValAdd, {reg(21), 1})};
    prsnt.triggers.triggers.push_back(run);
    const std::array<wse::game::LoadedPresentation, 1> prsnts{prsnt};

    wse::game::UiSession ui{};
    ui.current_window = wse::game::kScreenWindowMap;
    const std::array<std::int64_t, 3> timers{};
    auto frame = [&](float dt) {
        const std::int32_t before = world.m5.active_window;
        world.step_world_frame(timers, dt);
        return ui.drive_frame(world, ctx, dt, prsnts, before);
    };
    auto run_stmt = [&](const wse::vm::Statement& st) {
        wse::vm::StatementBlock blk{};
        blk.statements.push_back(st);
        return wse::vm::execute_statement_block(blk, ctx);
    };

    // start_presentation (900) on the map queues 0x0f; the next frame opens it.
    expect(run_stmt(make_stmt(kStartPresentation, {0})) && world.m5.enqueued_windows.size() == 1U,
           "start_presentation on the map queues window 0x0f");
    const auto opened = frame(0.25f);
    expect(opened.opened_presentation && opened.presentation_open && ctx.registers[20] == 1 &&
               ctx.registers[21] == 0 && world.m5.active_window == wse::game::kScreenWindowPresentation &&
               world.m5.enqueued_windows.empty() && world.presentation_duration == 50,
           "drive_frame opens the queued presentation and fires -60 (not -61) that frame");
    const auto stepped = frame(0.25f);
    expect(!stepped.opened_presentation && stepped.presentation_open && ctx.registers[21] == 1 &&
               world.presentation_duration == 25,
           "the next frame steps the presentation (-61, duration countdown)");
    const auto expired = frame(0.30f);
    expect(!expired.presentation_open && world.active_presentation_no == -1 &&
               world.m5.active_window == wse::game::kScreenWindowMap &&
               world.m6.ui.active_window_id == wse::game::kScreenWindowMap &&
               ui.current_window == wse::game::kScreenWindowMap,
           "an expired presentation closes window 0x16 back to the map");

    // change_screen_notes (2053) opens the notes window for the troop.
    expect(run_stmt(make_stmt(kChangeScreenNotes, {1, 0})), "change_screen_notes runs");
    const auto notes = frame(0.1f);
    const auto* title = find_widget(ui.root, "notes_title");
    const auto* body = find_widget(ui.root, "notes_data");
    expect(notes.opened_notes && world.m5.active_window == wse::game::kScreenWindowNotes &&
               title != nullptr && title->text == "Lord Test" && body != nullptr &&
               body->text == "Rules the test march.",
           "drive_frame opens the queued notes window with the troop's name and notes");

    // A presentation queued from the notes window returns to the map, not to the notes.
    (void)run_stmt(make_stmt(kStartPresentation, {0}));
    expect(frame(0.1f).opened_presentation && ui.source_window == wse::game::kScreenWindowMap,
           "a presentation opened over the notes window returns to the map");

    // A scene drops the open presentation without writing a campaign window back.
    world.lighting.has_scene = true;
    const auto mission = frame(0.1f);
    expect(mission.closed_for_mission && !mission.presentation_open &&
               world.m5.active_window == -1 && world.m6.ui.active_window_id == -1 &&
               world.active_presentation_no == -1,
           "a mission frame drops the presentation and leaves the window unknown");
    world.lighting.has_scene = false;

    // An unknown presentation number does not leave window 0x16 active.
    world.m5.active_window = wse::game::kScreenWindowMap;
    world.m6.ui.active_window_id = wse::game::kScreenWindowMap;
    ui.current_window = wse::game::kScreenWindowMap;
    (void)run_stmt(make_stmt(kStartPresentation, {7}));
    const auto bad = frame(0.1f);
    expect(!bad.opened_presentation && world.active_presentation_no == -1 &&
               world.m5.active_window == wse::game::kScreenWindowMap,
           "an unknown queued presentation falls back to the window it came from");
}

// Text button hit boxes per create opcode (`createButtonOverlay` 0x14020abe0,
// `createGameButtonOverlay` 0x14020c3e0, `createIngameButtonOverlay` 0x14020c7d0).
void test_overlay_button_boxes() {
    auto ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    wse::game::WseWorldState& world = ctx.world;
    world.active_presentation_no = 0;
    ctx.stringRegisters[0] = u"AB";
    ctx.stringRegisters[1] = u"Go";

    auto run_stmt = [&](const wse::vm::Statement& st) {
        wse::vm::StatementBlock blk{};
        blk.statements.push_back(st);
        return wse::vm::execute_statement_block(blk, ctx);
    };
    // A plain operand `N` names string register `sN`.
    constexpr std::int64_t s0 = 0;
    constexpr std::int64_t s1 = 1;
    expect(run_stmt(make_stmt(912U, {reg(0), s0, 0x1fffU})) &&
               run_stmt(make_stmt(940U, {reg(1), s1})) && run_stmt(make_stmt(941U, {reg(2), s1})) &&
               world.overlays.size() == 3U,
           "create_button / game_button / in_game_button overlays created");
    if (world.overlays.size() != 3U) {
        return;
    }
    expect(world.overlays[0].m5.button_opcode == 912 &&
               world.overlays[0].m5.widget_flags == ((0x1fffU & 0x181cU) | 0x200U) &&
               world.overlays[1].m5.button_opcode == 940 && world.overlays[1].m5.widget_flags == 0x10U &&
               world.overlays[2].m5.button_opcode == 941 && world.overlays[2].m5.widget_flags == 0x10U,
           "button overlays record their create opcode and retail text flags");

    wse::asset::FontData font;
    expect(font.parse_xml(
               "<FontData width=\"256\" height=\"256\" padding=\"10\" font_size=\"64\" "
               "font_scale=\"100\" line_spacing=\"100\"><FontDetails>"
               "<character code=\"65\" u=\"60\" v=\"20\" w=\"104\" h=\"76\" preshift=\"2\" "
               "yadjust=\"48\" postshift=\"40\"/>"
               "<character code=\"66\" u=\"120\" v=\"20\" w=\"164\" h=\"76\" preshift=\"2\" "
               "yadjust=\"48\" postshift=\"40\"/>"
               "</FontDetails></FontData>"),
           "synthetic button font parses");

    wse::game::UiSession ui{};
    ui.current_window = wse::game::kScreenWindowPresentation;
    ui.sync_overlays_from_world(world);
    const auto* plain = find_widget(ui.root, "overlay_0");
    expect(plain != nullptr && close(plain->size.x, wse::game::kUiOverlayButtonWidth),
           "without a font create_button_overlay keeps the fixed fallback box");

    ui.font = &font;
    ui.sync_overlays_from_world(world);
    const auto [tw, th] = font.measure_text("AB", 0.0f, wse::game::kUiOverlayTextScale,
                                            wse::game::kUiOverlayTextScale,
                                            world.overlays[0].m5.widget_flags);
    const auto* simple = find_widget(ui.root, "overlay_0");
    expect(simple != nullptr && tw > 0.0f && close(simple->size.x, tw) && close(simple->size.y, th),
           "create_button_overlay box is the text extent at the 0.02 creation size");
    const auto* game = find_widget(ui.root, "overlay_1");
    const auto* in_game = find_widget(ui.root, "overlay_2");
    expect(game != nullptr && close(game->size.x, wse::game::kUiGameButtonWidth) &&
               close(game->size.y, wse::game::kUiGameButtonHeight) &&
               game->mesh_name == "medium_button" && game->highlight_mesh_name == "medium_button_down" &&
               !game->tint_mesh,
           "create_game_button_overlay: 0.148 x 0.04 box with medium_button meshes");
    expect(in_game != nullptr && close(in_game->size.x, wse::game::kUiInGameButtonWidth) &&
               close(in_game->size.y, wse::game::kUiInGameButtonHeight) &&
               in_game->mesh_name == "mp_inventory_button",
           "create_in_game_button_overlay: 0.2123 x 0.04 box with mp_inventory_button");

    // overlay_set_size (925): text buttons keep the box and scale the text; game buttons take
    // the size vector as their box and keep the default text size.
    ctx.positions[1].o = {0.5f, 2.0f, 0.0f, 0.0f};
    expect(run_stmt(make_stmt(925U, {0, 1})) && run_stmt(make_stmt(925U, {1, 1})) &&
               world.overlays[1].m5.size_set,
           "overlay_set_size marks size_set");
    ui.sync_overlays_from_world(world);
    simple = find_widget(ui.root, "overlay_0");
    game = find_widget(ui.root, "overlay_1");
    expect(simple != nullptr && close(simple->size.x, tw) &&
               close(simple->text_scale.x, wse::game::kUiOverlayTextScale * 0.5f) &&
               close(simple->text_scale.y, wse::game::kUiOverlayTextScale * 2.0f),
           "overlay_set_size scales create_button_overlay text but not its box");
    expect(game != nullptr && close(game->size.x, 0.5f) && close(game->size.y, 2.0f) &&
               close(game->text_scale.x, wse::game::kUiOverlayTextScale),
           "overlay_set_size gives a game button its box and keeps its text size");
}

// Overlay animation (`overlay_animate_to_*` 932..937 via `animate_overlays`), text boxes
// (`rglTextBoxWidget`, create 917) and game-button mesh boxes (`sync_overlays_from_world`).
void test_overlay_animation_textbox_gamebutton() {
    // 1. Colour and alpha animation: 0.5 s of a 1 s channel, then the rest of it.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay o{};
        o.type = 1;
        o.position = {0.4f, 0.4f};
        o.m5.area_size = {0.2f, 0.1f};
        o.color = 0x000000u;
        o.alpha = 0.0f;
        const std::int32_t id = rig.add(o);
        wse::game::WseOverlayM5Ext& m = rig.ov(id).m5;
        m.anim_color = 0xFF8040u;
        m.anim_color_duration = 1.0f;
        m.anim_color_start = 0x000000u;
        m.anim_alpha = 1.0f;
        m.anim_alpha_duration = 1.0f;
        m.anim_alpha_start = 0.0f;
        m.anim_active = static_cast<std::uint8_t>(wse::game::kOverlayAnimColor | wse::game::kOverlayAnimAlpha);
        m.anim_elapsed = {};

        rig.session.step_presentation(rig.ctx.world, rig.ctx, 0.5f, rig.prs);
        expect(rig.ov(id).color == 0x7F4020u,
               "colour at 0.5 s of 1 s, black to 0xFF8040: round half up gives 0x7F4020");
        expect(close(rig.ov(id).alpha, 127.0f / 255.0f),
               "alpha at 0.5 s of 1 s, 0 to 1: 127/255 (fraction exactly 0.5 does not round up)");
        expect(rig.ov(id).m5.anim_active != 0U, "colour and alpha channels still running at 0.5 s");

        rig.session.step_presentation(rig.ctx.world, rig.ctx, 0.6f, rig.prs);
        expect(rig.ov(id).color == 0xFF8040u && close(rig.ov(id).alpha, 1.0f) &&
                   rig.ov(id).m5.anim_active == 0U,
               "past the duration the exact target is set and the anim_active bits are cleared");
    }

    // 1b. Size and position: 1 s of a 2 s channel.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay o{};
        o.type = 1;
        o.position = {0.1f, 0.1f};
        o.m5.area_size = {0.2f, 0.1f};
        const std::int32_t id = rig.add(o);
        wse::game::WseOverlayM5Ext& m = rig.ov(id).m5;
        m.anim_size_start = {1.0f, 1.0f};
        m.anim_size = {3.0f, 1.0f};
        m.anim_size_duration = 2.0f;
        m.anim_position_start = {0.0f, 0.0f};
        m.anim_position = {4.0f, 2.0f};
        m.anim_position_duration = 2.0f;
        m.anim_active = static_cast<std::uint8_t>(wse::game::kOverlayAnimSize | wse::game::kOverlayAnimPosition);
        m.size_set = false;

        rig.session.step_presentation(rig.ctx.world, rig.ctx, 1.0f, rig.prs);
        const wse::game::WsePresentationOverlay& after = rig.ov(id);
        expect(close(after.size.x, 2.0f) && close(after.size.y, 1.0f) && after.m5.size_set,
               "size at 1 s of 2 s, (1,1) to (3,1): (2,1) and size_set");
        expect(close(after.position.x, 2.0f) && close(after.position.y, 1.0f),
               "position at 1 s of 2 s, (0,0) to (4,2): (2,1)");
    }

    // 2. Text box: typing, backspace, commit by Enter / Escape / outside press.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay tb{};
        tb.type = 6;
        tb.position = {0.3f, 0.3f};
        tb.m5.area_size = {0.2f, 0.05f};
        tb.m5.create_opcode = 917;
        const std::int32_t t = rig.add(tb);
        const std::int32_t b = rig.add(box_button(0.6f, 0.6f, 0.2f, 0.05f));
        const auto typed = [&rig](std::u16string s) {
            wse::game::UiPointerInput in{};
            in.text_input = std::move(s);
            return rig.send(in);
        };

        rig.press(0.4f, 0.32f);
        expect(rig.ov(t).m5.focused && rig.ctx.world.m5.focused_overlay_no == t,
               "a left press inside a text box focuses it");
        rig.release(0.4f, 0.32f);
        rig.reset_regs();
        typed(u"ab<c");
        expect(rig.ov(t).text == u"abc" && rig.n62() == 0,
               "typed characters are added, '<' and '>' are rejected, and nothing commits");
        typed(u"\b");
        expect(rig.ov(t).text == u"ab", "backspace removes the last character");
        typed(u"\r");
        expect(rig.n62() == 1 && rig.ctx.registers[12] == t && rig.ctx.stringRegisters[0] == u"ab",
               "Enter commits once: -62 fires and string register 0 holds the text");
        expect(!rig.ov(t).m5.focused && rig.ctx.world.m5.focused_overlay_no == -1,
               "commit clears the text box focus");

        rig.press(0.4f, 0.32f);
        rig.release(0.4f, 0.32f);
        rig.reset_regs();
        typed(u"x\x1b");
        expect(rig.n62() == 1 && rig.ctx.stringRegisters[0] == u"abx" && !rig.ov(t).m5.focused,
               "Escape commits the text (typed 'x' before it is kept)");

        rig.press(0.4f, 0.32f);
        rig.release(0.4f, 0.32f);
        typed(u"z");
        rig.reset_regs();
        const auto pe = rig.press(0.65f, 0.61f);  // over the button, outside the text box
        expect(rig.n62() == 1 && rig.ctx.registers[12] == t && rig.ctx.stringRegisters[0] == u"abxz" &&
                   rig.ctx.world.m5.focused_overlay_no == -1,
               "a left press outside the box commits it once");
        expect(rig.n64() == 1 && rig.ctx.registers[16] == t && rig.ctx.registers[16] != b,
               "the outside press reaches only the text box with -64, not the button under it");
        rig.reset_regs();
        const auto re = rig.release(0.65f, 0.61f);
        expect(pe.empty() && re.empty() && rig.n62() == 0 && rig.n64() == 0,
               "the button is not armed by that press, so its release fires nothing");
    }

    // 3. Game button mesh rectangles (`sync_overlays_from_world`, opcodes 940 / 941).
    {
        OverlayRig rig;
        rig.session.mesh_bounds["medium_button"] = wse::game::UiMeshBounds{0.0f, 0.0f, 0.3f, 0.09f};
        wse::game::WsePresentationOverlay g{};
        g.type = 1;
        g.text = u"Go";
        g.m5.is_button = true;
        g.m5.button_opcode = 940;
        const std::int32_t gid = rig.add(g);
        const std::string gid_str = "overlay_" + std::to_string(gid);
        const auto* w = find_widget(rig.session.root, gid_str);
        expect(w != nullptr && w->has_mesh_rect && close(w->mesh_size.x, 0.3f * 5.0f / 9.0f) &&
                   close(w->mesh_size.y, 0.09f * 5.0f / 9.0f),
               "940 without size_set: mesh rect is the medium_button bounds times 5/9");
        expect(w != nullptr && close(w->size.x, wse::game::kUiGameButtonWidth) &&
                   close(w->size.y, wse::game::kUiGameButtonHeight),
               "940 without an area: box is kUiGameButtonWidth x kUiGameButtonHeight");

        rig.ov(gid).size = {0.296f, 0.08f};
        rig.ov(gid).m5.size_set = true;
        rig.session.sync_overlays_from_world(rig.ctx.world);
        w = find_widget(rig.session.root, gid_str);
        expect(w != nullptr && close(w->mesh_size.x, 2.0f * 0.3f * 5.0f / 9.0f) &&
                   close(w->mesh_size.y, 2.0f * 0.09f * 5.0f / 9.0f),
               "940 with size_set = (0.296, 0.08): mesh rect doubles with the 2x scale");
    }
    {
        OverlayRig rig;
        rig.session.mesh_bounds["mp_inventory_button"] = wse::game::UiMeshBounds{0.0f, 0.0f, 0.4f, 0.05f};
        wse::game::WsePresentationOverlay g{};
        g.type = 1;
        g.text = u"Inv";
        g.m5.is_button = true;
        g.m5.button_opcode = 941;
        const std::int32_t gid = rig.add(g);
        const auto* w = find_widget(rig.session.root, "overlay_" + std::to_string(gid));
        expect(w != nullptr && w->has_mesh_rect && close(w->mesh_size.x, 0.4f) &&
                   close(w->mesh_size.y, 0.05f),
               "941 without size_set: mesh rect is the mp_inventory_button bounds at scale 1");
    }
}

void test_mission_presentation_draw_and_input() {
    using wse::game::LoadedPresentation;
    using wse::game::kPresentationFlagManualEndOnly;
    constexpr std::int64_t s0 = 0;
    // Each presentation creates one button in -60; -62 counts in `n62_reg`, -63 in `n63_reg`.
    const auto make_prs = [](std::uint32_t flags, std::size_t n62_reg, std::size_t n63_reg,
                             std::vector<wse::vm::Statement> extra_62) {
        LoadedPresentation p{};
        p.id = "prsnt_mission_input";
        p.flags = flags;
        const auto add = [&p](float key, std::vector<wse::vm::Statement> stmts) {
            wse::game::SimpleTrigger t{};
            t.interval = key;
            t.block.statements = std::move(stmts);
            p.triggers.triggers.push_back(std::move(t));
        };
        add(-60.0f, {make_stmt(912U, {reg(0), s0})});
        std::vector<wse::vm::Statement> t62{
            make_stmt(wse::vm::core_op::kValAdd, {reg(static_cast<std::int32_t>(n62_reg)), 1})};
        t62.insert(t62.end(), extra_62.begin(), extra_62.end());
        add(-62.0f, std::move(t62));
        add(-63.0f, {make_stmt(wse::vm::core_op::kValAdd, {reg(static_cast<std::int32_t>(n63_reg)), 1})});
        return p;
    };
    auto ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    ctx.stringRegisters[0] = u"Go";
    ctx.world.lighting.has_scene = true;
    for (const std::size_t r : {40U, 41U, 42U, 43U}) {
        ctx.registers[r] = 0;
    }
    wse::game::UiSession session{};
    std::vector<LoadedPresentation> prs{};
    prs.push_back(make_prs(kPresentationFlagManualEndOnly, 40U, 41U, {}));  // B (bottom)
    // A (top) also calls start_presentation 900 from its -62.
    prs.push_back(make_prs(0U, 42U, 43U, {make_stmt(900U, {7})}));
    const auto step = [&]() { (void)session.drive_frame(ctx.world, ctx, 0.05f, prs, -1); };
    ctx.world.m6.ui.pending_conversation_presentations.push_back(0);
    step();
    ctx.world.m6.ui.pending_conversation_presentations.push_back(1);
    step();
    expect(ctx.world.m6.ui.active_presentations.size() == 2U && ctx.world.overlays.empty(),
           "B (manual_end_only) and A are both open; the campaign overlay list stays empty");
    expect(session.root.id == "mission" && session.root.children.size() == 2U &&
               find_widget(session.root.children[0], "overlay_0") != nullptr &&
               find_widget(session.root.children[1], "overlay_0") != nullptr,
           "root draws both mission containers, each with its own button");
    const wse::game::UiWidget* btn =
        session.root.children.size() == 2U ? find_widget(session.root.children[1], "overlay_0") : nullptr;
    if (btn == nullptr) {
        return;
    }
    const float x = btn->pos.x + btn->size.x * 0.5f;
    const float y = btn->pos.y + btn->size.y * 0.5f;
    wse::game::UiPointerInput in{};
    in.x = x;
    in.y = y;
    (void)session.handle_input(ctx.world, in, &ctx, prs);
    expect(ctx.registers[41] == 1 && ctx.registers[43] == 1,
           "hover enter (-63) reaches the button of every mission container");
    in.left_down = true;
    in.left_pressed = true;
    (void)session.handle_input(ctx.world, in, &ctx, prs);
    in.left_down = false;
    in.left_pressed = false;
    in.left_released = true;
    (void)session.handle_input(ctx.world, in, &ctx, prs);
    expect(ctx.registers[42] == 1 && ctx.registers[40] == 0,
           "a press the top container's button consumes fires its -62 only, not the one below");
    const auto& open = ctx.world.m6.ui.active_presentations;
    expect(open.size() == 2U && open[0] == 0 && open[1] == 1 &&
               ctx.world.active_presentation_no < 0 && session.root.id == "mission",
           "start_presentation in a container trigger renumbers no container and leaves no "
           "campaign presentation");
    step();
    expect(ctx.world.m6.ui.active_presentations.size() == 2U &&
               ctx.world.m6.ui.active_presentations[1] == 1,
           "the containers keep their numbers on the next frame");
    ctx.world.lighting.has_scene = false;
    step();
    expect(session.root.id != "mission" && session.root.children.empty(),
           "leaving the scene stops drawing the mission containers");
}

void test_synthetic_ui_rhi_render_and_uv_orientation() {
    // 16x16 font atlas: glyph 'A' covers x=[0..8), y=[0..16), top half red, bottom half blue
    // in top-down DDS/RGBA8 coordinates (row 0..7 red, row 8..15 blue).
    std::vector<std::uint8_t> font_tex(16U * 16U * 4U, 0);
    for (std::uint32_t y = 0; y < 16U; ++y) {
        for (std::uint32_t x = 0; x < 8U; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * 16U + x) * 4U;
            font_tex[off + 0] = (y < 8U) ? 255 : 0;
            font_tex[off + 1] = 0;
            font_tex[off + 2] = (y < 8U) ? 0 : 255;
            font_tex[off + 3] = 255;
        }
    }

    // 8x8 UI image texture: top half green, bottom half yellow in top-down RGBA8 rows.
    std::vector<std::uint8_t> panel_tex(8U * 8U * 4U, 255);
    for (std::uint32_t y = 0; y < 8U; ++y) {
        for (std::uint32_t x = 0; x < 8U; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * 8U + x) * 4U;
            panel_tex[off + 0] = (y < 4U) ? 0 : 255;
            panel_tex[off + 1] = 255;
            panel_tex[off + 2] = 0;
            panel_tex[off + 3] = 255;
        }
    }

    wse::asset::FontData font{};
    font.width = 16;
    font.height = 16;
    font.padding = 0;
    font.font_size = 16;
    font.base_size = 16;
    font.font_scale = 1.0f;
    font.font_width = 1.0f;
    font.font_height = 1.0f;
    font.line_spacing = 1.0f;
    wse::asset::FontGlyph ga{};
    ga.uv_x = 0.0f;
    ga.u_right = 0.5f;
    ga.uv_y = 1.0f;       // Warband bottom-up V for top edge (top row 0 -> 1 - 1.0 = 0.0)
    ga.v_bottom = 0.0f;   // Warband bottom-up V for bottom edge (row 15 -> 1 - 0.0 = 1.0)
    ga.preshift = 0.0f;
    ga.yadjust = 1.0f;
    ga.postshift = 0.5f;
    font.glyphs['A'] = ga;

    wse::asset::BrfMesh brf_quad{};
    brf_quad.name = "ui_test_quad";
    brf_quad.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    brf_quad.face_corners = {
        wse::asset::BrfFaceCorner{0, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {0.0f, 0.0f}, {}},
        wse::asset::BrfFaceCorner{1, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {1.0f, 0.0f}, {}},
        wse::asset::BrfFaceCorner{2, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {1.0f, 1.0f}, {}},
        wse::asset::BrfFaceCorner{3, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {0.0f, 1.0f}, {}},
    };
    brf_quad.faces = {{{{0, 1, 2}}}, {{{0, 2, 3}}}};

    wse::platform::SoftwareRhiDevice rhi{};
    wse::platform::UiRhiRenderer renderer(rhi);
    std::string err;
    expect(renderer.init(nullptr, &err), "UiRhiRenderer init succeeds");
    expect(renderer.upload_texture("font", 16, 16, font_tex), "upload synthetic font atlas");
    expect(renderer.upload_texture("panel_tex", 8, 8, panel_tex), "upload synthetic panel texture");
    expect(renderer.upload_brf_mesh(brf_quad, "panel_tex"), "upload synthetic BRF quad");

    wse::game::UiWidget root{};
    root.id = "root";
    root.kind = wse::game::UiWidgetKind::Container;
    root.size = {1.0f, 0.75f};

    // Place BRF image on right half [0.55..0.95] x [0.15..0.60]
    wse::game::UiWidget img{};
    img.id = "img";
    img.kind = wse::game::UiWidgetKind::Image;
    img.pos = {0.55f, 0.15f};
    img.size = {0.40f, 0.45f};
    img.mesh_name = "ui_test_quad";
    root.children.push_back(img);

    // Place button with text "A" on left half [0.10..0.40] x [0.20..0.55]
    wse::game::UiWidget btn{};
    btn.id = "btn";
    btn.kind = wse::game::UiWidgetKind::SimpleButton;
    btn.pos = {0.10f, 0.20f};
    btn.size = {0.30f, 0.35f};
    btn.text = "A";
    btn.text_scale = {0.25f, 0.25f};
    btn.text_flags = wse::asset::kTextFlagCenterAlign;
    btn.text_color_argb = 0xFFFFFFFFU;
    btn.highlight_color_argb = 0xFF00FF00U;
    root.children.push_back(btn);

    wse::platform::UiRenderConfig cfg{};
    cfg.viewport_width = 100;
    cfg.viewport_height = 75;
    std::vector<std::uint8_t> fb_normal;
    expect(renderer.render_tree(root, font, cfg, fb_normal), "synthetic render_tree succeeds");
    expect(fb_normal.size() == 100U * 75U * 4U, "render output size matches 100x75 RGBA8");

    // Verify BRF image orientation: top of image box (row ~22, col ~75) = green; bottom (row ~52, col ~75) = yellow.
    const std::size_t img_top = (22U * 100U + 75U) * 4U;
    const std::size_t img_bot = (52U * 100U + 75U) * 4U;
    expect(fb_normal[img_top + 0] < 40 && fb_normal[img_top + 1] > 200,
           "BRF UI quad top row maps to top of DDS texture (green)");
    expect(fb_normal[img_bot + 0] > 200 && fb_normal[img_bot + 1] > 200,
           "BRF UI quad bottom row maps to bottom of DDS texture (yellow)");

    // Verify upright glyph orientation: glyph 'A' spans rows ~20..44, col ~25;
    // top half (row 26) = red, bottom half (row 39) = blue.
    const std::size_t gly_top = (26U * 100U + 25U) * 4U;
    const std::size_t gly_bot = (39U * 100U + 25U) * 4U;
    expect(fb_normal[gly_top + 0] > 200 && fb_normal[gly_top + 2] < 40,
           "upright glyph 'A' top row samples red top half of font atlas");
    expect(fb_normal[gly_bot + 2] > 200 && fb_normal[gly_bot + 0] < 40,
           "upright glyph 'A' bottom row samples blue bottom half of font atlas");

    // Highlight button and verify color toggles framebuffer inside glyph box
    root.children[1].highlighted = true;
    std::vector<std::uint8_t> fb_hl;
    expect(renderer.render_tree(root, font, cfg, fb_hl), "highlighted render_tree succeeds");
    expect(diff_pixels_in_rect(fb_normal, fb_hl, 100, 75, 10, 20, 40, 60) > 10U,
           "highlighting button alters rendered glyph pixels");

    // Box-less (overlay) text is anchored at the widget position; an empty label draws nothing.
    wse::game::UiWidget root2{};
    root2.size = {1.0f, 0.75f};
    wse::game::UiWidget lbl{};
    lbl.kind = wse::game::UiWidgetKind::Label;
    lbl.pos = {0.5f, 0.3f};
    lbl.size = {0.0f, 0.0f};
    lbl.text = "A";
    lbl.text_scale = {0.25f, 0.25f};
    lbl.text_flags = 0U;
    lbl.text_color_argb = 0xFFFFFFFFU;
    root2.children.push_back(lbl);
    wse::game::UiWidget empty_lbl{};
    empty_lbl.kind = wse::game::UiWidgetKind::Label;
    empty_lbl.size = {0.0f, 0.0f};
    empty_lbl.color_argb = 0xFFFFFFFFU;
    root2.children.push_back(empty_lbl);
    std::vector<std::uint8_t> fb_lbl;
    expect(renderer.render_tree(root2, font, cfg, fb_lbl), "box-less label render succeeds");
    // Glyph spans x = [0.5, 0.625] -> px [50, 63), baseline 0.3 to top 0.55 -> rows [20, 45).
    expect(count_non_clear_pixels(fb_lbl, 100, 75, 50, 20, 63, 45, 12, 12, 14) > 200U &&
               count_non_clear_pixels(fb_lbl, 100, 75, 0, 0, 48, 75, 12, 12, 14) == 0U &&
               count_non_clear_pixels(fb_lbl, 100, 75, 66, 0, 100, 75, 12, 12, 14) == 0U,
           "box-less label text sits on its position and the empty label draws nothing");

    // `UiWidget::material_name` resolves through the material's diffuse texture.
    std::vector<std::uint8_t> red_tex(4U * 4U * 4U, 0);
    for (std::size_t px = 0; px < 16U; ++px) {
        red_tex[px * 4U + 0U] = 255;
        red_tex[px * 4U + 3U] = 255;
    }
    expect(renderer.upload_texture("red_tex", 4, 4, red_tex), "upload synthetic red texture");
    wse::platform::LoadedUiAssets mat_assets{};
    wse::asset::BrfMaterial red_mat{};
    red_mat.name = "mat_red";
    red_mat.texture_names[0] = "red_tex";
    mat_assets.materials["mat_red"] = red_mat;
    mat_assets.meshes["ui_test_quad"] = brf_quad;
    const auto bounds = wse::platform::collect_ui_mesh_bounds(mat_assets);
    const auto bq = bounds.find("ui_test_quad");
    expect(bq != bounds.end() && close(bq->second.min_x, 0.0f) && close(bq->second.max_x, 1.0f) &&
               close(bq->second.min_y, 0.0f) && close(bq->second.max_y, 1.0f),
           "collect_ui_mesh_bounds reports the UI-space extent of loaded meshes");
    mat_assets.meshes.clear();
    mat_assets.font_texture_key.clear();
    renderer.upload_assets(mat_assets);
    root.children[0].material_name = "MAT_RED";
    std::vector<std::uint8_t> fb_mat;
    expect(renderer.render_tree(root, font, cfg, fb_mat), "material override render succeeds");
    expect(fb_mat[img_top + 0] > 200 && fb_mat[img_top + 1] < 40 && fb_mat[img_bot + 0] > 200 &&
               fb_mat[img_bot + 1] < 40,
           "material_name draws the mesh with the material's texture");

    // Re-uploading the font atlas keeps rendering it (the new texture replaces the old one).
    expect(renderer.upload_texture("font", 16, 16, font_tex), "re-upload font atlas");
    std::vector<std::uint8_t> fb_refont;
    root.children[0].material_name.clear();
    root.children[1].highlighted = false;
    expect(renderer.render_tree(root, font, cfg, fb_refont) && fb_refont == fb_normal,
           "re-uploaded font atlas renders identically");

    // Mesh tint (`overlay_set_color` / `overlay_set_alpha` on mesh overlays): `color_argb`
    // multiplies the mesh only when `tint_mesh` is set.
    root.children[0].color_argb = 0xFF00FF00U;
    std::vector<std::uint8_t> fb_untinted;
    expect(renderer.render_tree(root, font, cfg, fb_untinted) && fb_untinted == fb_normal,
           "color_argb without tint_mesh leaves the mesh unchanged");
    root.children[0].tint_mesh = true;
    std::vector<std::uint8_t> fb_tint;
    expect(renderer.render_tree(root, font, cfg, fb_tint), "tinted render_tree succeeds");
    expect(fb_tint[img_bot + 0] < 40 && fb_tint[img_bot + 1] > 200 &&
               fb_normal[img_bot + 0] > 200,
           "a green tint removes the red channel from the mesh");
    root.children[0].color_argb = 0x00FFFFFFU;
    std::vector<std::uint8_t> fb_clear;
    expect(renderer.render_tree(root, font, cfg, fb_clear) &&
               fb_clear[img_bot + 0] == 12 && fb_clear[img_bot + 1] == 12 &&
               fb_clear[img_bot + 2] == 14,
           "a zero-alpha tint makes the mesh invisible (clear colour shows through)");
    root.children[0].tint_mesh = false;
    root.children[0].color_argb = 0xFFFFFFFFU;
}

// Campaign menu window 0x12 (`drive_frame` menu path), text-button re-measure at 912 when the
// overlay's measure scale changes, and `UiRhiRenderer` dynamic buffer reuse across frames.
void test_batch_menu_remeasure_and_ui_buffers() {
    // 1. Menu window 0x12 built from `jump_menu_no`, rebuilt only on a new queued 0x12.
    auto ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    ctx.world.lighting.has_scene = false;
    std::array<wse::game::LoadedMenu, 2> menus{};
    for (std::size_t i = 0; i < menus.size(); ++i) {
        menus[i].id = "menu_" + std::to_string(i);
        wse::game::LoadedMenuOption opt{};
        opt.id = "opt";
        opt.text = "Option " + std::to_string(i);
        menus[i].options.push_back(opt);
    }
    wse::game::UiSession session{};
    auto open_menu = [&](std::int32_t menu_no) {
        ctx.world.jump_menu_no = menu_no;
        ctx.world.enqueue_window(wse::game::kQueuedWindowMenuType);
        (void)ctx.world.consume_campaign_queued_window();
        return session.drive_frame(ctx.world, ctx, 0.016f, {}, wse::game::kScreenWindowMap, menus);
    };

    const auto first = open_menu(1);
    expect(first.opened_menu && session.current_window == wse::game::kScreenWindowMenu &&
               session.current_menu_index == 1 && ctx.world.ui.current_menu_no == 1,
           "queued 0x12 builds menu 1 from jump_menu_no");
    const auto idle = session.drive_frame(ctx.world, ctx, 0.016f, {}, wse::game::kScreenWindowMap,
                                          menus);
    expect(!idle.opened_menu && session.current_menu_index == 1,
           "drive_frame without a new queued 0x12 does not reopen the menu");
    const auto second = open_menu(0);
    expect(second.opened_menu && session.current_menu_index == 0 &&
               ctx.world.ui.current_menu_no == 0,
           "jumping to menu 0 rebuilds the menu with current_menu_index 0");
    const auto out_of_range = open_menu(5);
    expect(!out_of_range.opened_menu,
           "an out-of-range jump_menu_no builds nothing");

    // Shared synthetic bitmap font for checks 2 and 3 (same XML as the button-box test).
    wse::asset::FontData font;
    expect(font.parse_xml(
               "<FontData width=\"256\" height=\"256\" padding=\"10\" font_size=\"64\" "
               "font_scale=\"100\" line_spacing=\"100\"><FontDetails>"
               "<character code=\"65\" u=\"60\" v=\"20\" w=\"104\" h=\"76\" preshift=\"2\" "
               "yadjust=\"48\" postshift=\"40\"/>"
               "<character code=\"72\" u=\"120\" v=\"20\" w=\"164\" h=\"76\" preshift=\"2\" "
               "yadjust=\"48\" postshift=\"40\"/>"
               "</FontDetails></FontData>"),
           "synthetic re-measure font parses");

    // 2. Text button (912) box follows text_measure_scale after sync_overlays_from_world.
    {
        auto world_ptr = std::make_unique<wse::vm::VmContext>();
        wse::game::WseWorldState& world = world_ptr->world;
        world.active_presentation_no = 0;
        wse::game::WsePresentationOverlay ov{};
        ov.type = 1;
        ov.m5.is_button = true;
        ov.m5.button_opcode = 912;
        ov.m5.create_opcode = 912;
        ov.text = u"Hello";
        world.overlays.push_back(ov);

        wse::game::UiSession ui{};
        ui.current_window = wse::game::kScreenWindowPresentation;
        ui.font = &font;
        ui.sync_overlays_from_world(world);
        const auto* w1 = find_widget(ui.root, "overlay_0");
        const float width_1x = (w1 != nullptr) ? w1->size.x : 0.0f;

        world.overlays[0].m5.text_measure_scale = {2.0f, 2.0f};
        ui.sync_overlays_from_world(world);
        const auto* w2 = find_widget(ui.root, "overlay_0");
        const float width_2x = (w2 != nullptr) ? w2->size.x : 0.0f;
        expect(width_1x > 0.0f && std::fabs(width_2x - 2.0f * width_1x) <= 0.1f * 2.0f * width_1x,
               "912 button box width doubles (within 10%) when text_measure_scale is 2x");
    }

    // 3. UiRhiRenderer keeps the same dynamic VB/IB handles across identical frames.
    {
        wse::platform::SoftwareRhiDevice rhi{};
        wse::platform::UiRhiRenderer renderer(rhi);
        std::string err;
        expect(renderer.init(nullptr, &err), "dynamic-buffer renderer init succeeds");
        std::vector<std::uint8_t> font_tex(16U * 16U * 4U, 0xFFU);
        expect(renderer.upload_texture("font", 16, 16, font_tex), "upload font atlas for buffer test");

        wse::game::UiWidget root{};
        root.id = "root";
        root.kind = wse::game::UiWidgetKind::Container;
        root.size = {1.0f, 0.75f};
        wse::game::UiWidget btn{};
        btn.id = "btn";
        btn.kind = wse::game::UiWidgetKind::SimpleButton;
        btn.pos = {0.10f, 0.20f};
        btn.size = {0.30f, 0.35f};
        btn.text = "A";
        btn.text_scale = {0.25f, 0.25f};
        btn.text_flags = wse::asset::kTextFlagCenterAlign;
        btn.text_color_argb = 0xFFFFFFFFU;
        root.children.push_back(btn);

        wse::platform::UiRenderConfig cfg{};
        cfg.viewport_width = 100;
        cfg.viewport_height = 75;
        std::vector<std::uint8_t> fb_a, fb_b;
        expect(renderer.render_tree(root, font, cfg, fb_a), "first buffer-test frame renders");
        const auto vb_a = renderer.dynamic_vertex_buffer();
        const auto ib_a = renderer.dynamic_index_buffer();
        expect(renderer.render_tree(root, font, cfg, fb_b), "second buffer-test frame renders");
        const auto vb_b = renderer.dynamic_vertex_buffer();
        const auto ib_b = renderer.dynamic_index_buffer();
        expect(vb_a != 0U && ib_a != 0U && vb_a == vb_b && ib_a == ib_b && fb_a == fb_b,
               "UiRhiRenderer reuses its dynamic VB/IB across identical frames");
    }
}

void test_retail_warband_native_main_menu() {
    const std::string root = find_retail_warband_root();
    if (root.empty()) {
        std::printf("SKIP  retail UI test (WSE_WARBAND_ROOT not set)\n");
        return;
    }

    wse::platform::HostVfs vfs(root);
    wse::platform::LoadedUiAssets assets{};
    std::string err;
    expect(wse::platform::load_native_ui_assets(vfs, "", "Modules/Native", "en", assets, &err),
           "load_native_ui_assets loads retail Native UI assets");
    expect(assets.font.get_glyph('A').has_quad(), "retail font_data.xml loaded printable 'A'");
    expect(assets.lang.get_text("ui_start_a_new_game") == "Start a New Game",
           "retail English uimain/ui CSV provides 'Start a New Game'");
    expect(assets.vars.size() >= 100U, "retail game_variables.txt parsed 100+ layout keys");
    expect(assets.meshes.find("main_menu_nord") != assets.meshes.end() &&
               assets.meshes.find("ui_quick_battle_a") != assets.meshes.end() &&
               assets.meshes.find("warrider_logo") != assets.meshes.end() &&
               assets.meshes.find("dlg_button") != assets.meshes.end(),
           "retail BRF UI meshes loaded (main_menu_nord, ui_quick_battle_a, warrider_logo, dlg_button)");
    expect(assets.textures.find("font") != assets.textures.end() &&
               assets.textures.find("main_menu_nord") != assets.textures.end() &&
               assets.textures.find("ui_quick_battle") != assets.textures.end() &&
               assets.textures.find("warrider_logo") != assets.textures.end() &&
               assets.textures.find("interface") != assets.textures.end(),
           "retail UI DDS textures decoded cleanly");

    wse::game::UiSession session{};
    session.lang = assets.lang;
    session.vars = assets.vars;
    session.mesh_bounds = wse::platform::collect_ui_mesh_bounds(assets);
    wse::game::WseWorldState world{};
    session.build_initial_window(world, "Native", true, true, true, true);

    wse::platform::SoftwareRhiDevice rhi{};
    wse::platform::UiRhiRenderer renderer(rhi);
    expect(renderer.init(&assets, &err), "UiRhiRenderer uploads all retail UI assets");

    wse::platform::UiRenderConfig cfg{};
    cfg.viewport_width = 320;
    cfg.viewport_height = 240;
    std::vector<std::uint8_t> fb_idle;
    expect(renderer.render_tree(session.root, assets.font, cfg, fb_idle),
           "renders retail Native main menu to 320x240 RGBA8");
    expect(count_non_clear_pixels(fb_idle, 320, 240, 10, 20, 95, 220, 12, 12, 14) > 5000U,
           "retail parchment sidebar + buttons produce rich non-clear coverage");
    expect(count_non_clear_pixels(fb_idle, 320, 240, 140, 20, 310, 220, 12, 12, 14) > 5000U,
           "retail main_menu_nord backdrop covers right viewport");

    const auto* start_btn = find_widget(session.root, "start_new_game_button");
    expect(start_btn != nullptr, "retail initial window has start_new_game_button");
    const float hx = start_btn->pos.x + start_btn->size.x * 0.5f;
    const float hy = start_btn->pos.y + start_btn->size.y * 0.5f;
    (void)session.handle_pointer_and_keys(world, hx, hy, false, false, false);
    expect(start_btn->highlighted, "retail hover hits start_new_game_button");

    std::vector<std::uint8_t> fb_hover;
    expect(renderer.render_tree(session.root, assets.font, cfg, fb_hover),
           "re-renders Native main menu with hover highlight");

    // Convert Warband Y-up [hy] bounds to top-down pixel rect cleanly
    const auto y0_px = static_cast<std::uint32_t>(
        std::clamp((wse::game::kUiScreenHeight - (start_btn->pos.y + start_btn->size.y)) /
                       wse::game::kUiScreenHeight * 240.0f,
                   0.0f, 239.0f));
    const auto y1_px = static_cast<std::uint32_t>(
        std::clamp((wse::game::kUiScreenHeight - start_btn->pos.y) / wse::game::kUiScreenHeight * 240.0f + 1.0f,
                   0.0f, 240.0f));
    expect(diff_pixels_in_rect(fb_idle, fb_hover, 320, 240, 0, y0_px, 120, y1_px) > 15U,
           "hovering Start a New Game changes rendered text pixels inside button band");
    expect(diff_pixels_in_rect(fb_idle, fb_hover, 320, 240, 180, 0, 320, 240) == 0U,
           "hovering left sidebar leaves right backdrop unchanged");

    // Verify retail Native menus.txt + presentations.txt integrate cleanly into UiSession + UiRhiRenderer
    std::vector<wse::game::LoadedMenu> menus;
    const auto menu_err = wse::game::load_menus(vfs, "Modules/Native/menus.txt", world, &menus);
    expect(menu_err.ok && !menus.empty(), "retail Native menus.txt loads cleanly");
    std::vector<wse::game::LoadedPresentation> prsnts;
    const auto prsnt_err = wse::game::load_presentations(vfs, "Modules/Native/presentations.txt", prsnts);
    expect(prsnt_err.ok && !prsnts.empty(), "retail Native presentations.txt loads cleanly");

    session.build_menu_window(world, 0, menus);
    std::vector<std::uint8_t> fb_menu;
    expect(renderer.render_tree(session.root, assets.font, cfg, fb_menu) &&
               count_non_clear_pixels(fb_menu, 320, 240, 120, 40, 280, 200, 12, 12, 14) > 2000U,
           "renders retail Native game menu screen cleanly");
}

// --- Mission presentations, text box limits, 949 commit, slider knob, UI frame input -----------
const wse::game::UiWidget* find_mesh_widget(const wse::game::UiWidget& node, std::string_view mesh) {
    if (node.mesh_name == mesh) {
        return &node;
    }
    for (const wse::game::UiWidget& child : node.children) {
        if (const auto* hit = find_mesh_widget(child, mesh)) {
            return hit;
        }
    }
    return nullptr;
}

void test_mission_presentations_and_ui_frame() {
    using wse::game::LoadedPresentation;
    using wse::game::kPresentationFlagManualEndOnly;
    constexpr std::uint32_t kStoreParam1 = 2071U;
    constexpr std::uint32_t kSetDuration = 902U;
    constexpr std::size_t kCountA = 30U;
    constexpr std::size_t kCountB = 31U;
    constexpr std::size_t kCountC = 32U;
    constexpr std::size_t kElapsedA = 33U;
    constexpr std::size_t kElapsedB = 34U;
    constexpr std::size_t kElapsedC = 35U;
    const auto rg = [](std::size_t r) { return reg(static_cast<std::int32_t>(r)); };

    // Presentation with a -60 counter (trigger 0) and a -61 that stores the elapsed ms (trigger 1).
    const auto make_prs = [&](std::uint32_t flags, const char* id, std::size_t count_reg,
                              std::size_t elapsed_reg) {
        LoadedPresentation p{};
        p.id = id;
        p.flags = flags;
        wse::game::SimpleTrigger t60{};
        t60.interval = -60.0f;
        t60.block.statements.push_back(make_stmt(wse::vm::core_op::kValAdd, {rg(count_reg), 1}));
        wse::game::SimpleTrigger t61{};
        t61.interval = -61.0f;
        t61.block.statements.push_back(make_stmt(kStoreParam1, {rg(elapsed_reg)}));
        p.triggers.triggers.push_back(std::move(t60));
        p.triggers.triggers.push_back(std::move(t61));
        return p;
    };

    auto ctx_ptr = std::make_unique<wse::vm::VmContext>();
    wse::vm::VmContext& ctx = *ctx_ptr;
    wse::game::UiSession session{};
    ctx.world.lighting.has_scene = true;
    std::vector<LoadedPresentation> prs{};
    prs.push_back(make_prs(0U, "prsnt_mission_a", kCountA, kElapsedA));
    prs.push_back(make_prs(kPresentationFlagManualEndOnly, "prsnt_mission_b", kCountB, kElapsedB));
    prs.push_back(make_prs(0U, "prsnt_mission_c", kCountC, kElapsedC));
    const auto step = [&](float dt) { return session.drive_frame(ctx.world, ctx, dt, prs, -1); };
    const auto push = [&](std::int32_t no) {
        ctx.world.m6.ui.pending_conversation_presentations.push_back(no);
    };
    const auto is_open = [&](std::int32_t no) {
        const auto& open = ctx.world.m6.ui.active_presentations;
        return std::find(open.begin(), open.end(), no) != open.end();
    };
    const auto open_count = [&]() { return ctx.world.m6.ui.active_presentations.size(); };

    // 1. Mission stack: A; B ends A; C joins B (manual_end_only); B again restarts B and ends C.
    push(0);
    auto rep = step(0.1f);
    expect(ctx.registers[kCountA] == 1 && rep.mission_presentations_open == 1U && is_open(0) &&
               open_count() == 1U,
           "pushing A opens its container: -60 fired once, one container open");
    expect(ctx.registers[kElapsedA] >= 99 && ctx.registers[kElapsedA] <= 101,
           "A's -61 receives the elapsed milliseconds of its first frame (about 100)");

    push(1);
    rep = step(0.1f);
    expect(!is_open(0) && is_open(1) && open_count() == 1U && rep.mission_presentations_open == 1U,
           "pushing B ends A (no manual_end_only) and opens B");
    expect(ctx.registers[kCountA] == 1 && ctx.registers[kCountB] == 1,
           "ended A does not get another -60; B's -60 fired once");
    expect(ctx.registers[kElapsedB] >= 99 && ctx.registers[kElapsedB] <= 101,
           "B's -61 receives its own elapsed milliseconds (about 100)");

    push(2);
    rep = step(0.1f);
    expect(is_open(1) && is_open(2) && open_count() == 2U && rep.mission_presentations_open == 2U,
           "pushing C keeps B (manual_end_only) and opens C: two containers open");

    // The header ends every container without manual_end_only, so C (flags 0) also ends here.
    push(1);
    rep = step(0.1f);
    expect(ctx.registers[kCountB] == 2, "pushing B again restarts B (its -60 fires again)");
    expect(is_open(1) && !is_open(2) && open_count() == 1U && rep.mission_presentations_open == 1U,
           "pushing B again ends C too (C lacks manual_end_only); only the new B is open");

    // 2. A duration of 0 closes a container.
    // C's -61 (trigger 1) gets presentation_set_duration 0 (one operand: the presentation's own).
    prs[2].triggers.triggers[1].block.statements.push_back(make_stmt(kSetDuration, {0}));
    push(2);
    rep = step(0.1f);
    expect(!is_open(2) && is_open(1) && open_count() == 1U && rep.mission_presentations_open == 1U,
           "presentation_set_duration 0 in C's -61 closes C after that frame; B stays open");
    ctx.world.m6.ui.mission_duration_requests.emplace_back(1, 0);
    rep = step(0.1f);
    expect(!is_open(1) && open_count() == 0U && rep.mission_presentations_open == 0U,
           "a mission_duration_requests entry {B, 0} closes B on that frame");

    // 3. The campaign state (active presentation, duration, overlays) is restored.
    ctx.world.active_presentation_no = 77;
    ctx.world.presentation_duration = 555;
    wse::game::WsePresentationOverlay campaign_ov{};
    campaign_ov.text = u"campaign";
    ctx.world.overlays.assign(1U, campaign_ov);
    push(0);
    rep = step(0.1f);
    expect(is_open(0) && rep.mission_presentations_open == 1U,
           "A is open while the campaign state holds sentinels");
    expect(ctx.world.active_presentation_no == 77 && ctx.world.presentation_duration == 555 &&
               ctx.world.overlays.size() == 1U && ctx.world.overlays[0].text == u"campaign",
           "mission frame restores active_presentation_no, presentation_duration and overlays");

    // 4. Leaving the scene clears the stack and active_presentations; no further -61.
    const std::int64_t a_elapsed_before = ctx.registers[kElapsedA];
    ctx.world.lighting.has_scene = false;
    rep = step(0.1f);
    expect(open_count() == 0U && rep.mission_presentations_open == 0U && !is_open(0),
           "leaving the scene clears the mission stack and active_presentations");
    expect(ctx.registers[kElapsedA] == a_elapsed_before,
           "a container dropped by leaving the scene gets no further -61");

    // 5. A focused text box accepts at most kUiTextBoxMaxLength characters.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay tb = boxed_text(0.3f, 0.3f, 0.2f, 0.05f);
        tb.type = 6;
        tb.m5.create_opcode = 917;
        const std::int32_t t = rig.add(tb);
        rig.press(0.4f, 0.32f);
        rig.release(0.4f, 0.32f);
        wse::game::UiPointerInput in{};
        in.text_input = std::u16string(40U, u'a');
        rig.send(in);
        expect(rig.ov(t).m5.focused &&
                   rig.ov(t).text == std::u16string(wse::game::kUiTextBoxMaxLength, u'a'),
               "a focused text box keeps at most kUiTextBoxMaxLength (28) typed characters");
    }

    // 6. overlay_obtain_focus (949) on B commits text box A: -62 for A with its text.
    {
        OverlayRig rig;
        wse::game::WsePresentationOverlay a_ov = boxed_text(0.3f, 0.3f, 0.2f, 0.05f);
        a_ov.type = 6;
        a_ov.m5.create_opcode = 917;
        const std::int32_t a = rig.add(a_ov);
        wse::game::WsePresentationOverlay b_ov = boxed_text(0.6f, 0.6f, 0.2f, 0.05f);
        b_ov.type = 6;
        b_ov.m5.create_opcode = 917;
        const std::int32_t b = rig.add(b_ov);
        rig.press(0.4f, 0.32f);
        rig.release(0.4f, 0.32f);
        wse::game::UiPointerInput typed{};
        typed.text_input = u"hi";
        rig.send(typed);
        expect(rig.ov(a).text == u"hi" && rig.n62() == 0, "text box A holds typed text without committing");

        // The fields overlay_obtain_focus (949, native_ui_ops.h) sets for B.
        rig.ov(a).m5.commit_pending = true;
        rig.ov(a).m5.focused = false;
        rig.ov(b).m5.focused = true;
        rig.ctx.world.m5.focused_overlay_no = b;
        rig.reset_regs();
        rig.send(wse::game::UiPointerInput{});
        expect(rig.n62() == 1 && rig.ctx.registers[12] == a && rig.ctx.stringRegisters[0] == u"hi",
               "the next handle_input fires -62 once for A, with string register 0 = A's text");
        expect(!rig.ov(a).m5.commit_pending, "the commit clears commit_pending");
        rig.reset_regs();
        rig.send(wse::game::UiPointerInput{});
        expect(rig.n62() == 0, "the commit does not repeat on the following frame");
    }

    // 7. Slider: the slider_hor track with a handle_hor knob at the value's position.
    {
        OverlayRig rig;
        rig.session.mesh_bounds["handle_hor"] = wse::game::UiMeshBounds{0.0f, 0.0f, 0.02f, 0.03f};
        wse::game::WsePresentationOverlay s{};
        s.type = 2;
        s.m5.create_opcode = 914;
        s.m5.min_value = 0.0;
        s.m5.max_value = 100.0;
        s.val = 50;
        s.position = {0.1f, 0.3f};
        rig.add(s);
        const wse::game::UiWidget* track = find_mesh_widget(rig.session.root, "slider_hor");
        expect(track != nullptr && track->children.size() == 1U &&
                   track->children[0].mesh_name == "handle_hor",
               "slider builds a slider_hor widget with one handle_hor child");
        if (track != nullptr && track->children.size() == 1U) {
            const float expected =
                wse::game::kUiSliderMargin + 0.5f * (track->size.x - 2.0f * wse::game::kUiSliderMargin);
            expect(close(track->children[0].pos.x, expected, 1e-3f),
                   "handle_hor sits at kUiSliderMargin + 0.5 * (width - 2 * margin) at value 50 of 0..100");
        }
    }

    // 8. platform::UiInputAccumulator: edges, touch-as-click, cancel, keys, text, wheel.
    {
        using wse::platform::RawInputEvent;
        namespace RawKey = wse::platform::RawKey;
        wse::platform::UiInputAccumulator acc{};
        const auto make_event = [](RawInputEvent::Type type) {
            RawInputEvent e{};
            e.type = type;
            return e;
        };

        RawInputEvent down = make_event(RawInputEvent::Type::MouseButtonDown);
        down.raw_key = RawKey::kLeftMouse;
        down.x = 0.5f;
        down.y = 0.25f;
        acc.feed(down);
        wse::game::UiPointerInput in = acc.take(0.0f);
        expect(in.left_pressed && in.left_down && close(in.x, 0.5f) &&
                   close(in.y, (1.0f - 0.25f) * 0.75f),
               "left press: left_pressed and left_down, x = 0.5, y = (1 - 0.25) * 0.75");
        in = acc.take(0.1f);
        expect(!in.left_pressed && in.left_down, "the next take has no press edge but still holds");

        RawInputEvent up = make_event(RawInputEvent::Type::MouseButtonUp);
        up.raw_key = RawKey::kLeftMouse;
        up.x = 0.5f;
        up.y = 0.25f;
        acc.feed(up);
        in = acc.take(0.2f);
        expect(in.left_released && !in.left_down, "MouseButtonUp gives left_released");

        RawInputEvent touch_down = make_event(RawInputEvent::Type::TouchDown);
        touch_down.finger_id = 7;
        touch_down.x = 0.3f;
        touch_down.y = 0.6f;
        acc.feed(touch_down);
        in = acc.take(0.3f);
        expect(in.left_pressed && in.left_down, "first touch finger acts as a left press");
        RawInputEvent touch_up = make_event(RawInputEvent::Type::TouchUp);
        touch_up.finger_id = 7;
        touch_up.x = 0.3f;
        touch_up.y = 0.6f;
        acc.feed(touch_up);
        in = acc.take(0.4f);
        expect(in.left_released && !in.left_down, "touch up on finger 7 gives left_released");

        acc.feed(touch_down);
        (void)acc.take(0.5f);
        RawInputEvent cancel = make_event(RawInputEvent::Type::TouchCancel);
        cancel.finger_id = 7;
        acc.feed(cancel);
        in = acc.take(0.6f);
        expect(close(in.x, -1.0f) && close(in.y, -1.0f) && !in.left_released && !in.left_down,
               "TouchCancel moves the cursor to (-1, -1) with no release edge");

        RawInputEvent esc = make_event(RawInputEvent::Type::KeyDown);
        esc.raw_key = RawKey::kEscape;
        acc.feed(esc);
        in = acc.take(0.7f);
        expect(in.escape_pressed && in.text_input == u"\x1b",
               "Escape gives escape_pressed and U+001B in text_input");

        acc.feed_text("ab");
        in = acc.take(0.8f);
        expect(in.text_input == u"ab", "feed_text appends the UTF-8 text as UTF-16");

        RawInputEvent wheel = make_event(RawInputEvent::Type::MouseWheel);
        wheel.dy = 1.5f;
        acc.feed(wheel);
        wheel.dy = 0.5f;
        acc.feed(wheel);
        in = acc.take(0.9f);
        expect(close(in.wheel, 2.0f), "mouse wheel dy accumulates within a frame");
    }
}

// `GeometryCaptureRhiDevice` records the same picture `SoftwareRhiDevice` draws: the captured
// screen-space batches, replayed through the software rasterizer with an identity transform,
// match the direct render (texture orientation, tint, blend, painter's order).
// P10.7: an uncompressed A8R8G8B8 DDS with a full mip chain; every texel of mip `m` has blue
// byte `10 * m + 1`, so a test can tell which mip was decoded. `with_mips = false` writes
// only the top level (uniform blue 1).
std::vector<std::uint8_t> build_argb_mip_dds(std::uint32_t w, std::uint32_t h, bool with_mips) {
    std::vector<std::uint8_t> b(128U, 0U);
    const auto put = [&](std::size_t off, std::uint32_t v) {
        for (unsigned i = 0; i < 4U; ++i) {
            b[off + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU);
        }
    };
    std::uint32_t mips = 1U;
    if (with_mips) {
        for (std::uint32_t mw = w, mh = h; mw > 1U || mh > 1U; mw = std::max(mw / 2U, 1U),
                           mh = std::max(mh / 2U, 1U)) {
            ++mips;
        }
    }
    b[0] = 'D';
    b[1] = 'D';
    b[2] = 'S';
    b[3] = ' ';
    put(4, 124U);
    put(8, 0x1U | 0x2U | 0x4U | 0x1000U | (with_mips ? 0x20000U : 0U));
    put(12, h);
    put(16, w);
    put(20, w * 4U);
    put(28, mips);
    put(76, 32U);
    put(80, 0x41U);  // RGB | ALPHAPIXELS
    put(88, 32U);
    put(92, 0x00FF0000U);
    put(96, 0x0000FF00U);
    put(100, 0x000000FFU);
    put(104, 0xFF000000U);
    put(108, 0x1000U | (with_mips ? 0x400008U : 0U));
    std::uint32_t mw = w;
    std::uint32_t mh = h;
    for (std::uint32_t m = 0; m < mips; ++m) {
        for (std::uint32_t i = 0; i < mw * mh; ++i) {
            b.push_back(static_cast<std::uint8_t>(10U * m + 1U));  // B
            b.push_back(0x20U);                                     // G
            b.push_back(0x30U);                                     // R
            b.push_back(0xFFU);                                     // A
        }
        mw = std::max(mw / 2U, 1U);
        mh = std::max(mh / 2U, 1U);
    }
    return b;
}

// P10.7 texture budget: the largest mip that fits the cap is chosen and decoded; a texture
// without a small enough mip is box-filtered down; the UI loader applies the cap except to the
// font atlas, and the decoded size is counted.
void test_texture_budget() {
    std::string err;
    const auto chain = build_argb_mip_dds(64U, 32U, true);
    wse::asset::DdsImageView view{};
    expect(wse::asset::parse_dds(std::as_bytes(std::span<const std::uint8_t>(chain)), view, &err) &&
               view.mip_count == 7U,
           "P10.7 parse a 64x32 DDS with 7 mips");
    expect(wse::asset::select_budget_mip(view, 0U) == 0U &&
               wse::asset::select_budget_mip(view, 64U) == 0U &&
               wse::asset::select_budget_mip(view, 32U) == 1U &&
               wse::asset::select_budget_mip(view, 20U) == 2U &&
               wse::asset::select_budget_mip(view, 1U) == 6U,
           "P10.7 the largest mip whose sides fit the cap is chosen; 0 means no cap");

    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0U;
    std::uint32_t h = 0U;
    expect(wse::asset::decode_dds_rgba8_budgeted(view, 0U, 16U, rgba, w, h, &err) && w == 16U &&
               h == 8U && rgba.size() == 16U * 8U * 4U && rgba[2] == 21U,
           "P10.7 a 16 cap decodes mip 2 (16x8) of the chain");
    expect(wse::asset::decode_dds_rgba8_budgeted(view, 0U, 0U, rgba, w, h, &err) && w == 64U &&
               h == 32U && rgba[2] == 1U,
           "P10.7 no cap decodes the top level");

    const auto flat = build_argb_mip_dds(64U, 64U, false);
    wse::asset::DdsImageView flat_view{};
    expect(wse::asset::parse_dds(std::as_bytes(std::span<const std::uint8_t>(flat)), flat_view,
                                 &err) &&
               flat_view.mip_count == 1U,
           "P10.7 parse a 64x64 DDS without mips");
    expect(wse::asset::decode_dds_rgba8_budgeted(flat_view, 0U, 16U, rgba, w, h, &err) &&
               w == 16U && h == 16U && rgba.size() == 16U * 16U * 4U && rgba[0] == 0x30U &&
               rgba[1] == 0x20U && rgba[2] == 1U && rgba[3] == 0xFFU,
           "P10.7 a texture without mips is box-filtered down to the cap (uniform colour kept)");
    expect(!wse::asset::decode_dds_rgba8_budgeted(view, 1U, 16U, rgba, w, h, &err),
           "P10.7 an out-of-range face is an error");

    wse::platform::MemoryVfs vfs;
    expect(vfs.write_file("Textures/big.dds", chain) && vfs.write_file("Textures/font.dds", chain),
           "P10.7 seed textures");
    std::unordered_map<std::string, wse::platform::DecodedUiTexture> textures;
    expect(wse::platform::load_dds_texture_into_map(vfs, "Textures/big.dds", "big", textures, &err,
                                                    16U) &&
               textures["big"].width == 16U && textures["big"].height == 8U,
           "P10.7 the UI loader applies the texture cap");
    wse::platform::LoadedUiAssets assets{};
    assets.textures["big"] = textures["big"];
    assets.textures["font"].width = 2U;
    assets.textures["font"].height = 2U;
    assets.textures["font"].rgba8.assign(16U, 0U);
    expect(assets.texture_bytes() == 16U * 8U * 4U + 16U,
           "P10.7 texture_bytes counts every decoded texture");

    wse::platform::UiGameShell shell;
    expect(shell.texture_bytes() == 0U, "P10.7 a stopped shell holds no texture memory");
    wse::platform::UiGameShellOptions opts{};
    expect(opts.max_texture_size == 0U, "P10.7 the shell has no texture cap by default");
}

void test_geometry_capture_matches_software() {
    // 8x8 texture: top half green, bottom half yellow (top-down rows).
    std::vector<std::uint8_t> panel_tex(8U * 8U * 4U, 255);
    for (std::uint32_t y = 0; y < 8U; ++y) {
        for (std::uint32_t x = 0; x < 8U; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * 8U + x) * 4U;
            panel_tex[off + 0] = (y < 4U) ? 0 : 255;
            panel_tex[off + 1] = 255;
            panel_tex[off + 2] = static_cast<std::uint8_t>(x * 30U);
            panel_tex[off + 3] = 255;
        }
    }
    std::vector<std::uint8_t> font_tex(16U * 16U * 4U, 255);
    wse::asset::FontData font{};
    font.width = 16;
    font.height = 16;
    font.font_size = 16;
    font.base_size = 16;
    font.font_scale = 1.0f;
    font.font_width = 1.0f;
    font.font_height = 1.0f;
    font.line_spacing = 1.0f;
    wse::asset::FontGlyph ga{};
    ga.u_right = 0.5f;
    ga.uv_y = 1.0f;
    ga.yadjust = 1.0f;
    ga.postshift = 0.5f;
    font.glyphs['A'] = ga;

    wse::asset::BrfMesh quad{};
    quad.name = "geo_quad";
    quad.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    quad.face_corners = {
        wse::asset::BrfFaceCorner{0, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {0.0f, 0.0f}, {}},
        wse::asset::BrfFaceCorner{1, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {1.0f, 0.0f}, {}},
        wse::asset::BrfFaceCorner{2, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {1.0f, 1.0f}, {}},
        wse::asset::BrfFaceCorner{3, 0xFFFFFFFFU, {0, 0, 1}, {}, false, {0.0f, 1.0f}, {}},
    };
    quad.faces = {{{{0, 1, 2}}}, {{{0, 2, 3}}}};

    wse::game::UiWidget root{};
    root.size = {1.0f, 0.75f};
    wse::game::UiWidget img{};
    img.kind = wse::game::UiWidgetKind::Image;
    img.pos = {0.55f, 0.15f};
    img.size = {0.40f, 0.45f};
    img.mesh_name = "geo_quad";
    root.children.push_back(img);
    wse::game::UiWidget tinted = img;  // overlapping, half-transparent red tint on top
    tinted.pos = {0.45f, 0.30f};
    tinted.tint_mesh = true;
    tinted.color_argb = 0x80FF2020U;
    root.children.push_back(tinted);
    wse::game::UiWidget btn{};
    btn.kind = wse::game::UiWidgetKind::SimpleButton;
    btn.pos = {0.05f, 0.20f};
    btn.size = {0.30f, 0.35f};
    btn.text = "AA";
    btn.text_scale = {0.20f, 0.20f};
    btn.text_color_argb = 0xFF3060FFU;
    root.children.push_back(btn);

    wse::platform::UiRenderConfig cfg{};
    cfg.viewport_width = 160;
    cfg.viewport_height = 120;
    const auto setup = [&](wse::platform::UiRhiRenderer& r) {
        std::string e;
        return r.init(nullptr, &e) && r.upload_texture("font", 16, 16, font_tex) &&
               r.upload_texture("panel_tex", 8, 8, panel_tex) && r.upload_brf_mesh(quad, "panel_tex");
    };

    wse::platform::SoftwareRhiDevice sw{};
    wse::platform::UiRhiRenderer sw_renderer(sw);
    std::vector<std::uint8_t> direct;
    expect(setup(sw_renderer) && sw_renderer.render_tree(root, font, cfg, direct),
           "software render of the comparison tree");

    wse::platform::GeometryCaptureRhiDevice geo{};
    wse::platform::UiRhiRenderer geo_renderer(geo);
    std::vector<std::uint8_t> none{1, 2, 3};
    expect(setup(geo_renderer) && geo_renderer.render_tree(root, font, cfg, none) && none.empty(),
           "geometry capture of the same tree succeeds and reads nothing back");
    const wse::platform::GeometryFrame& f = geo.frame();
    expect(f.width == 160U && f.height == 120U && f.clear_r == cfg.clear_r && !f.batches.empty(),
           "captured frame keeps the pass size, clear colour and batches");
    bool indices_ok = true;
    for (const auto& b : f.batches) {
        for (std::uint32_t i = 0; i < b.index_count; ++i) {
            const std::int32_t ix = f.indices[b.first_index + i];
            indices_ok = indices_ok && ix >= 0 && static_cast<std::uint32_t>(ix) < b.vertex_count;
        }
    }
    expect(indices_ok, "batch indices stay inside their batch's vertex range");
    expect(geo.unsupported_draws() == 0U, "UI draws use only features the capture device supports");

    // Replay: pixel coordinates -> NDC with an identity transform, same textures and blends.
    wse::platform::SoftwareRhiDevice replay{};
    std::vector<wse::platform::MeshDrawCall> draws;
    std::vector<wse::platform::BufferHandle> keep;
    for (const auto& b : f.batches) {
        if (b.index_count == 0U) {
            continue;
        }
        std::vector<wse::platform::MeshVertex> mv(b.vertex_count);
        for (std::uint32_t i = 0; i < b.vertex_count; ++i) {
            const auto& gv = f.vertices[b.first_vertex + i];
            mv[i].px = gv.x / 160.0f * 2.0f - 1.0f;
            mv[i].py = gv.y / 120.0f * 2.0f - 1.0f;
            mv[i].u = gv.u;
            mv[i].v = gv.v;
            const auto c8 = [](float c) {
                return static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
            };
            mv[i].color_rgba = c8(gv.r) | (c8(gv.g) << 8U) | (c8(gv.b) << 16U) | (c8(gv.a) << 24U);
        }
        std::vector<std::uint16_t> ix(b.index_count);
        for (std::uint32_t i = 0; i < b.index_count; ++i) {
            ix[i] = static_cast<std::uint16_t>(f.indices[b.first_index + i]);
        }
        wse::platform::MeshDrawCall d{};
        d.vertex_buffer = replay.create_buffer(
            wse::platform::BufferUsage::vertex,
            {reinterpret_cast<const std::uint8_t*>(mv.data()), mv.size() * sizeof(mv[0])});
        d.index_buffer = replay.create_buffer(
            wse::platform::BufferUsage::index,
            {reinterpret_cast<const std::uint8_t*>(ix.data()), ix.size() * sizeof(ix[0])});
        if (const auto* t = geo.texture(b.texture); t != nullptr) {
            d.texture = replay.create_texture_rgba8(t->width, t->height, t->rgba8);
        }
        wse::platform::PipelineDesc pd{};
        pd.depth_test = false;
        pd.depth_write = false;
        pd.cull_mode = wse::platform::CullMode::none;
        pd.blend_mode = b.blend;
        d.pipeline = replay.create_pipeline(pd);
        d.index_count = b.index_count;
        draws.push_back(d);
    }
    wse::platform::RenderPassDesc pass{};
    pass.width = 160;
    pass.height = 120;
    pass.clear_r = f.clear_r;
    pass.clear_g = f.clear_g;
    pass.clear_b = f.clear_b;
    pass.clear_a = f.clear_a;
    std::vector<std::uint8_t> replayed;
    expect(replay.render_pass_and_readback(pass, draws, replayed) && replayed.size() == direct.size(),
           "captured batches replay through the software rasterizer");
    std::size_t differing = 0;
    std::size_t lit = 0;
    for (std::size_t p = 0; p + 3 < direct.size() && p + 3 < replayed.size(); p += 4) {
        int worst = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(static_cast<int>(direct[p + c]) - static_cast<int>(replayed[p + c])));
        }
        differing += worst > 8 ? 1U : 0U;
        lit += (direct[p] != cfg.clear_r || direct[p + 1] != cfg.clear_g) ? 1U : 0U;
    }
    expect(lit > 160U * 120U / 10U, "the comparison tree covers a real part of the frame");
    expect(differing * 200U <= direct.size() / 4U,
           "replayed capture matches the direct software render (<= 0.5% pixels differ)");
    if (differing * 200U > direct.size() / 4U) {
        std::printf("      geometry replay: %zu of %zu pixels differ\n", differing, direct.size() / 4U);
    }

    // Destroyed textures are reported once, for the host to free its copies.
    const wse::platform::TextureHandle t = geo.create_texture_rgba8(1, 1, std::vector<std::uint8_t>(4, 255));
    geo.destroy_texture(t);
    const auto gone = geo.take_destroyed_textures();
    expect(gone.size() == 1U && gone[0] == t && geo.take_destroyed_textures().empty() &&
               geo.texture(t) == nullptr,
           "destroyed textures are reported once");
}

}  // namespace

namespace wse::selftest {

int run_ui_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_xml_parser_errors_and_roundtrip();
    test_font_data_and_text_layout();
    test_language_and_game_variables();
    test_ui_session_windows_and_regressions();
    test_overlay_hover_and_press_states();
    test_overlay_value_widgets();
    test_overlay_containers_and_dispatch_stop();
    test_ui_session_drive_frame();
    test_overlay_button_boxes();
    test_overlay_animation_textbox_gamebutton();
    test_mission_presentations_and_ui_frame();
    test_mission_presentation_draw_and_input();
    test_synthetic_ui_rhi_render_and_uv_orientation();
    test_retail_warband_native_main_menu();
    test_batch_menu_remeasure_and_ui_buffers();
    test_geometry_capture_matches_software();
    test_texture_budget();

    if (g_failures == 0) {
        std::printf("ui tests: PASS\n");
        return 0;
    }
    std::printf("ui tests: FAIL (%d)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_ui_suite(nullptr);
}
#endif
