// Host tests for the reconstructed mbScript/mbScriptManager readers.
//
// Build/run: powershell -File engine/build_host.ps1
//
// The fixture mirrors the retail header ("scriptsfile version 1"), a script
// count, and four records. Run directly as `script_test.exe <scripts.txt>`.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "wse/game/script.h"
#include "wse/io/text_stream.h"
#include "wse/vm/statement.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

std::string read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    for (const std::string& value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

void collect_missing(void* context, std::string_view name) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(name);
}

void test_fixture(const std::string& contents) {
    using namespace wse::game;
    using namespace wse;

    io::TokenReader reader(contents);
    ScriptManager manager;
    expect(manager.mappedScripts[0] == -1 && manager.mappedScripts[63] == -1,
           "mapped scripts default to -1");

    std::vector<std::string> missing;
    ScriptMapCallbacks callbacks;
    callbacks.reportMissing = collect_missing;
    callbacks.context = &missing;

    expect(read_script_manager(reader, manager, callbacks) == vm::ReadError::none,
           "scripts.txt fixture reads");
    expect(reader.at_end(), "scripts.txt fixture consumes every token");
    expect(manager.scripts.size() == 4, "fixture script count");

    expect(manager.scripts[0].id == "game_start", "first script id");
    expect(manager.scripts[0].interval == -1.0F, "first script interval");
    expect(manager.scripts[0].block.id == "Script [0] game_start", "first block id");
    expect(manager.scripts[0].block.statements.size() == 2, "first script statement count");
    expect(manager.scripts[0].block.statements[0].opcode == 2133, "first statement opcode");
    expect(manager.scripts[0].block.statements[0].numOperands == 2,
           "first statement operand count");
    expect(manager.scripts[0].block.statements[0].operands[0] == 1224979098644774912LL,
           "first statement wide operand");
    expect(manager.scripts[0].block.statements[0].operands[1] == 5,
           "first statement second operand");
    expect(manager.scripts[0].block.statements[1].opcode == 2105,
           "second statement opcode");

    expect(manager.scripts[1].id == "unmapped_custom_script", "unmapped script id");
    expect(manager.scripts[1].interval == 0.5F, "unmapped script interval");
    expect(manager.scripts[1].block.id == "Script [1] unmapped_custom_script",
           "unmapped script block id");
    expect(manager.scripts[1].block.statements.size() == 1, "unmapped script statement count");
    expect(manager.scripts[1].block.statements[0].operands[0] == 7, "unmapped script operand");

    expect(manager.scripts[2].id == "game_start", "duplicate id is preserved");
    expect(manager.scripts[2].interval == 2.25F, "duplicate script interval");

    expect(manager.scripts[3].block.statements.empty(), "empty statement block");
    expect(manager.scripts[3].block.id == "Script [3] last_script", "last block id");

    expect(manager.mappedScripts[0] == 0, "game_start maps to the first duplicate");
    bool onlyFirstMapped = manager.mappedScripts[0] == 0;
    for (std::size_t slot = 1; slot < kMappedScriptCount; ++slot) {
        onlyFirstMapped = onlyFirstMapped && manager.mappedScripts[slot] == -1;
    }
    expect(onlyFirstMapped, "every other mapped slot stays -1");

    expect(missing.size() == kMappedScriptCount - 1, "every absent mapped name is reported");
    expect(!contains(missing, "game_start"), "mapped name is not reported missing");
    expect(contains(missing, "game_quick_start"), "second mapped name is reported missing");
    expect(contains(missing, "wse_initial_window_start"),
           "initial-window mapped name is reported missing");
    expect(contains(missing, "wse_camp_button_clicked"),
           "camp-button mapped name is reported missing");
}

void test_read_script_index() {
    using namespace wse::game;
    using namespace wse;

    io::TokenReader reader("probe 2.5\n1\n9 0\n");
    Script script;
    expect(read_script(reader, script, 7) == vm::ReadError::none, "single script reads");
    expect(script.id == "probe", "single script id");
    expect(script.interval == 2.5F, "single script interval");
    expect(script.block.id == "Script [7] probe", "block id uses the file index");
    expect(script.block.statements.size() == 1, "single script statement count");
    expect(script.block.statements[0].opcode == 9, "single script opcode");
    expect(script.block.statements[0].numOperands == 0, "zero-operand statement");
}

wse::vm::ReadError read_manager(const char* text, wse::game::ScriptManager& manager) {
    wse::io::TokenReader reader(text);
    return wse::game::read_script_manager(reader, manager);
}

void test_header_errors() {
    using namespace wse::game;
    using namespace wse;

    {
        ScriptManager manager;
        expect(read_manager("", manager) == vm::ReadError::malformed_string,
               "missing first header string is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile", manager) == vm::ReadError::malformed_string,
               "missing second header string is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version", manager) == vm::ReadError::malformed_number,
               "missing header version is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version 1", manager) == vm::ReadError::malformed_number,
               "missing script count is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version 1 -4", manager) == vm::ReadError::negative_count,
               "negative script count is rejected");
    }
    {
        ScriptManager manager;
        const std::string text =
            "scriptsfile version 1 " + std::to_string(kMaxScripts + 1);
        expect(read_manager(text.c_str(), manager) ==
                   vm::ReadError::script_count_out_of_range,
               "oversized script count is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version 1 1", manager) ==
                   vm::ReadError::malformed_string,
               "missing script id is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version 1 1\nprobe", manager) ==
                   vm::ReadError::malformed_number,
               "missing script interval is rejected");
    }
    {
        ScriptManager manager;
        expect(read_manager("scriptsfile version 1 1\nprobe 1.0\n1\n7 2 3", manager) ==
                   vm::ReadError::malformed_number,
               "missing statement operand is rejected");
    }
}

void test_empty_manager() {
    using namespace wse::game;
    using namespace wse;

    ScriptManager manager;
    manager.scripts.resize(2);
    manager.scripts[0].id = "old_script";
    manager.mappedScripts.fill(5);

    io::TokenReader reader("scriptsfile version 1 0");
    expect(read_script_manager(reader, manager) == vm::ReadError::none,
           "empty script list reads");
    expect(manager.scripts.empty(), "empty script list replaces the old scripts");
    bool allUnmapped = true;
    for (std::int32_t mapped : manager.mappedScripts) {
        allUnmapped = allUnmapped && mapped == -1;
    }
    expect(allUnmapped, "empty script list leaves every mapped slot at -1");
    expect(reader.at_end(), "empty script list consumes the header");
}

void test_error_descriptions() {
    using namespace wse::vm;
    expect(!describe(ReadError::malformed_string).empty(), "string error description");
    expect(!describe(ReadError::script_count_out_of_range).empty(),
           "script count error description");
}

}  // namespace

namespace wse::selftest {

int run_script_suite(const char* fixture_path) {
    failures = 0;
    const char* fixture = fixture_path ? fixture_path : "engine/fixtures/scripts.txt";
    const std::string contents = read_file(fixture);
    if (contents.empty()) {
        std::printf("script tests: FAIL (cannot read fixture %s)\n", fixture);
        return 1;
    }

    test_fixture(contents);
    test_read_script_index();
    test_header_errors();
    test_empty_manager();
    test_error_descriptions();

    if (failures != 0) {
        std::printf("script tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("script tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main(int argc, char** argv) {
    return wse::selftest::run_script_suite(argc > 1 ? argv[1] : nullptr);
}
#endif
