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
#include <string>
#include <string_view>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/font.h"
#include "wse/game/module_mission_loaders.h"
#include "wse/game/ui_runtime.h"
#include "wse/io/xml.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_ui.h"
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

    wse::asset::FontData bad;
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
    menu.text = "Choose an action.";
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

    // Presentation triggers -60/-61/-62/-63 + Escape / duration expiry close 0x16
    wse::game::LoadedPresentation prsnt{};
    prsnt.id = "prsnt_test";
    prsnt.flags = 1U;  // read-only 10.0s default duration
    auto add_trig = [&](float interval, std::initializer_list<wse::vm::Statement> stmts) {
        wse::game::SimpleTrigger st{};
        st.interval = interval;
        st.block.statements.assign(stmts.begin(), stmts.end());
        prsnt.triggers.triggers.push_back(std::move(st));
    };
    add_trig(-60.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(10), 1})});
    add_trig(-61.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(11), reg(0)})});
    add_trig(-62.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(12), reg(0)})});
    add_trig(-63.0f, {make_stmt(wse::vm::core_op::kAssign, {reg(13), reg(1)})});

    wse::game::WsePresentationOverlay ov{};
    ov.type = 3;  // button overlay
    ov.text = u"Press";
    ov.position = {0.2f, 0.2f};
    ov.size = {0.2f, 0.05f};
    ov.visible = true;
    ctx.world.overlays.clear();
    ctx.world.overlays.push_back(ov);

    std::array<wse::game::LoadedPresentation, 1> prsnts{prsnt};
    expect(session.start_presentation(ctx.world, ctx, 0, prsnts) && ctx.registers[10] == 1,
           "start_presentation fires -60 trigger");
    expect(session.step_presentation(ctx.world, ctx, 0.5f, prsnts) && ctx.registers[11] == 500,
           "step_presentation passes elapsed ms and fires -61");

    const auto prsnt_evs = session.handle_pointer_and_keys(
        ctx.world, 0.30f, 0.22f, false, true, false, &ctx, prsnts);
    expect(prsnt_evs.size() == 1U && prsnt_evs[0].kind == wse::game::UiActionKind::OverlayClicked &&
               ctx.registers[12] == 0 && ctx.registers[13] == 0,
           "overlay hover enters (-63 reg1=0) and click fires (-62 reg0=0)");

    (void)session.handle_pointer_and_keys(ctx.world, 0.9f, 0.7f, false, false, false, &ctx, prsnts);
    expect(ctx.registers[13] == 1, "moving pointer off overlay fires -63 leave (reg1=1)");

    expect(!session.step_presentation(ctx.world, ctx, 10.0f, prsnts) &&
               ctx.world.active_presentation_no == -1 &&
               ctx.world.m5.active_window == wse::game::kScreenWindowMap,
           "presentation auto-closes 0x16 when 10s read-only timer expires");
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

}  // namespace

namespace wse::selftest {

int run_ui_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_xml_parser_errors_and_roundtrip();
    test_font_data_and_text_layout();
    test_language_and_game_variables();
    test_ui_session_windows_and_regressions();
    test_synthetic_ui_rhi_render_and_uv_orientation();
    test_retail_warband_native_main_menu();

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
