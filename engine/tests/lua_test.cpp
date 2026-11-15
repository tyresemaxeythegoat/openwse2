// Comprehensive host tests for Phase 2F: stock MIT Lua 5.1.5 runtime,
// sandbox, ModSys parsers, hooks, opcodes 5100..5114, and headless runner.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/headless_runner.h"
#include "wse/game/module_bundle.h"
#include "wse/game/world_m6_types.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/lua_manager.h"
#include "wse/vm/lua_runtime.h"
#include "wse/vm/operand.h"
#include "wse/vm/ops/lua_bridge.h"
#include "wse/vm/wse_opcodes.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

constexpr std::int64_t op_tag(wse::vm::OperandTag tag, std::uint64_t idx) noexcept {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(tag) << 56) | idx);
}

wse::vm::Statement make_stmt(std::uint32_t opcode, std::initializer_list<std::int64_t> args) {
    wse::vm::Statement st{};
    st.opcode = opcode;
    st.numOperands = static_cast<std::int32_t>(args.size());
    std::size_t i = 0;
    for (std::int64_t v : args) st.operands[i++] = v;
    return st;
}

void seed_lua_fixture_files(wse::platform::Vfs& vfs) {
    (void)vfs.make_dir("Modules");
    (void)vfs.make_dir("Modules/TestMod");
    (void)vfs.make_dir("Modules/TestMod/lua");
    (void)vfs.make_dir("Modules/TestMod/lua/msfiles");
    (void)vfs.make_dir("Modules/TestMod/lua/lib");
    (void)vfs.make_dir("lua_storage");
    (void)vfs.make_dir("savegames");

    (void)vfs.write_text(
        "Modules/TestMod/variables.txt",
        "g_score\n"
        "g_flag\n"
        "g_target\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/header_operations.py",
        "# Subset of Warband/WSE2 operations for tests\n"
        "\"\"\"multiline docstring assign = 999\"\"\"\n"
        "call_script = 1\n"
        "eq = 31\n"
        "ge = 30\n"
        "val_add = 2105\n"
        "assign = 2133\n"
        "str_store_string = 2320\n"
        "copy_position = 700\n"
        "agent_get_troop_id = 1718\n"
        "lua_push_int = 5100\n"
        "lua_call = 5112\n"
        "lhs_operations = [\n"
        "    val_add,\n"
        "    assign,\n"
        "    agent_get_troop_id,\n"
        "]\n"
        "can_fail_operations = [\n"
        "    call_script,\n"
        "    eq,\n"
        "    ge,\n"
        "]\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/header_operations_addon.py",
        "dict_get_int = 3205\n"
        "lhs_operations += [dict_get_int]\n"
        "can_fail_operations += [dict_get_int]\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/header_triggers.py",
        "ti_before_mission_start = -50\n"
        "ti_on_weapon_attack = -51.0\n"
        "sf_mask = 0x10 | 0x04\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/ID_items.py",
        "itm_sword = 0\n"
        "itm_shield = 1\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/ID_scenes.py",
        "scn_arena = 3\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/ID_meshes.py",
        "mesh_banner = 0\n");

    (void)vfs.write_text(
        "Modules/TestMod/lua/lib/helper.lua",
        "local M = {}\n"
        "function M.add(a, b) return a + b end\n"
        "return M\n");
}

void test_stdlib_bit_table_string_vector_pos() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;

    wse::platform::MemoryVfs vfs;
    wse::platform::CaptureLog log;
    wse::platform::ManualClock clock;
    clock.advance_seconds(42.5);

    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;

    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    cfg.log = &log;
    cfg.clock = &clock;
    cfg.disable_game_const_warnings = true;

    expect(rt->initialize(*ctx, cfg), "initialize without main.lua succeeds");
    expect(rt->is_ready() && !rt->has_main_script(), "ready without main.lua");

    const bool ok = rt->do_string(
        "local b = require('bit')\n"
        "assert(b.band(0xff00, 0x0f0f) == 0x0f00)\n"
        "assert(b.bor(0x1, 0x4) == 5)\n"
        "assert(b.bxor(0xf, 0xa) == 5)\n"
        "assert(b.lshift(1, 3) == 8)\n"
        "assert(b.rshift(-1, 28) == 15)\n"
        "assert(b.arshift(-16, 2) == -4)\n"
        "assert(b.tohex(255, 4) == '00ff')\n"
        "assert(b.tohex(0xab, -2) == 'AB')\n"
        "assert(b.bswap(0x12345678) == 0x78563412)\n"
        "assert(round(3.14159, 2) == 3.14)\n"
        "assert(math.round(-2.6) == -3)\n"
        "local t = {10, 20, 30}\n"
        "assert(table.contain(t, 20) and table.find(t, 30) == 3)\n"
        "assert(table.length(t) == 3)\n"
        "table.reverse(t); assert(t[1] == 30 and t[3] == 10)\n"
        "local nested = {}; table.make(nested, 'a', 'b'); nested.a.b.x = 7\n"
        "local copy = table.copy(nested, true); nested.a.b.x = 9; assert(copy.a.b.x == 7)\n"
        "assert(string.starts_with('itm_sword', 'itm_') and string.ends_with('a.lua', '.lua'))\n"
        "assert(string.trim('  hi  ') == 'hi')\n"
        "local sp = string.split('a,b,c', ','); assert(#sp == 3 and sp[2] == 'b')\n"
        "local v1 = vector3(3, 4, 0)\n"
        "assert(v1:length() == 5)\n"
        "local v2 = vector3(1, 0, 0)\n"
        "assert((v1 + v2).x == 4 and v1:dot(v2) == 3)\n"
        "local p = game.pos.new({o = vector3(10, 20, 30)})\n"
        "game.preg[5] = p\n"
        "local p2 = game.pos.new(5)\n"
        "assert(p2.o.x == 10 and p2.o.y == 20 and p2.o.z == 30)\n"
        "print('hello', 123)\n"
        "printf('val=%d', 7)\n"
        "assert(math.abs(getTime() - 42.5) < 0.001)\n",
        "stdlib_test");
    expect(ok, "Lua stdlib, bit, table, string, vector3, pos, clock, and print");
    expect(rt->print_history().size() == 2 &&
               rt->print_history()[0] == "hello\t123" &&
               rt->print_history()[1] == "val=7",
           "print_history records print/printf output");

    rt->shutdown();
}

void test_ops_constants_variables_and_exec() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;

    // Seed two scripts: script 0 returns sum(param1, param2) into reg[0]
    wse::game::Script s0{};
    s0.id = "add_to_reg0";
    s0.block.statements.push_back(make_stmt(wse::vm::core_op::kValAdd, {
        op_tag(wse::vm::OperandTag::local, 0), 0
    }));
    // Replace cleanly using store_script_param (no, direct local + global add):
    // Use assign reg0 = script param1 + param2 via local/global statements:
    s0.block.statements.clear();
    // Script param op 23 (store_script_param): param1 -> reg0, param2 -> reg1, val_add reg0 reg1
    s0.block.statements.push_back(make_stmt(23U, {op_tag(wse::vm::OperandTag::global, 0), 1}));
    s0.block.statements.push_back(make_stmt(23U, {op_tag(wse::vm::OperandTag::global, 1), 2}));
    s0.block.statements.push_back(make_stmt(wse::vm::core_op::kValAdd, {
        op_tag(wse::vm::OperandTag::global, 0),
        op_tag(wse::vm::OperandTag::global, 1)
    }));
    bundle->scripts.scripts.push_back(s0);

    wse::platform::MemoryVfs vfs;
    seed_lua_fixture_files(vfs);
    wse::platform::CaptureLog log;
    wse::platform::ManualClock clock;

    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;

    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    cfg.log = &log;
    cfg.clock = &clock;

    expect(rt->initialize(*ctx, cfg), "initialize with msfiles & variables.txt");
    expect(rt->operation_count() == 11, "loaded 10 header_operations + 1 addon op");
    expect(rt->operation_flags("assign") == wse::vm::kLuaOpFlagLhs, "assign flags");
    expect(rt->operation_flags("eq") == wse::vm::kLuaOpFlagCf, "eq flags");
    expect(rt->operation_flags("dict_get_int") ==
               (wse::vm::kLuaOpFlagLhs | wse::vm::kLuaOpFlagCf),
           "addon cf+lhs flags");
    expect(rt->gvar_index("g_flag") == 1 && rt->gvar_index("g_target") == 2,
           "variables.txt parsed 0-based");

    const bool ok = rt->do_string(
        "assert(game.const.triggers.ti_before_mission_start == -50)\n"
        "assert(game.const.sf_mask == 0x14)\n"
        "assert(game.const.itm_sword == 0 and game.const.items.itm_shield == 1)\n"
        "assert(game.const.scenes.scn_arena == 3)\n"
        "assert(game.const.meshes.mesh_banner == 0)\n"
        "game.gvar.g_score = 42\n"
        "assert(game.gvar.g_score == 42 and game.gvar[0] == 42)\n"
        "game.reg[3] = 99\n"
        "assert(game.reg[3] == 99)\n"
        "game.sreg[4] = 'warband'\n"
        "assert(game.sreg[4] == 'warband')\n"
        "assert(game.eq(7, 7) == true and game.eq(7, 8) == false)\n"
        "local x = game.assign(0, 12)\n"
        "assert(x == 12)\n"
        "local y = game.val_add(x, 30)\n"
        "assert(y == 42)\n"
        "game.str_store_string(10, 'hello_lua')\n"
        "assert(game.sreg[10] == 'hello_lua')\n"
        "game.copy_position(11, game.pos.new({o = vector3(5, 6, 7)}))\n"
        "assert(game.preg[11].o.z == 7)\n"
        "assert(game.script.add_to_reg0 == 0)\n"
        "local cf = game.add_to_reg0(19, 23)\n"
        "assert(cf == true and game.reg[0] == 42)\n",
        "ops_and_consts");
    expect(ok, "execOperation LHS/CF, string/pos temp allocation, call_script, consts, gvar");
    expect(ctx->globalVariables.get(0) == 42 && ctx->registers[0] == 42,
           "C++ VmContext observed Lua writes");

    rt->shutdown();
}

void test_hooks_triggers_and_iterators() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;

    // World entities for iterators
    ctx->world.parties.resize(3);
    ctx->world.parties[1].active = false;
    ctx->world.agents.resize(3);
    ctx->world.agents[0].position.o = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->world.agents[1].position.o = {3.0f, 4.0f, 0.0f, 1.0f}; // dist = 5
    ctx->world.agents[2].position.o = {100.0f, 0.0f, 0.0f, 1.0f};
    ctx->world.mission_objects.resize(3);
    ctx->world.mission_objects[0].scene_prop_no = 7;
    ctx->world.mission_objects[1].scene_prop_no = 8;
    ctx->world.mission_objects[2].scene_prop_no = 7;
    ctx->world.mission_objects[2].is_item = true;
    ctx->world.players.resize(3);
    ctx->world.players[1].status = 0; // inactive

    // Bundle entries for triggers / meshes / presentations / psys
    bundle->meshes.push_back({"mesh_banner", 0, "res", {}});
    bundle->item_triggers.resize(1);
    bundle->scene_props.resize(1);
    bundle->mission_templates.resize(1);
    bundle->mission_templates[0].id = "mt_battle";
    ctx->world.current_mission_template_no = 0;
    bundle->string_entries.push_back({"str_hi", "Greetings"});
    bundle->quick_string_entries.push_back({"qstr_yo", "Quick"});

    // Script 0: doubles param1 into reg[2]
    wse::game::Script s0{};
    s0.id = "hookable_script";
    s0.block.statements.push_back(make_stmt(23U, {op_tag(wse::vm::OperandTag::global, 2), 1}));
    s0.block.statements.push_back(make_stmt(wse::vm::core_op::kValMul, {
        op_tag(wse::vm::OperandTag::global, 2), 2
    }));
    bundle->scripts.scripts.push_back(s0);

    wse::platform::MemoryVfs vfs;
    seed_lua_fixture_files(vfs);
    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;

    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    expect(rt->initialize(*ctx, cfg), "initialize hooks test");

    const bool lua_setup = rt->do_string(
        "assert(game.getString(0, false) == 'Greetings')\n"
        "assert(game.getString(0, true) == 'Quick')\n"
        "assert(game.getStringId('str_hi') == 0 and game.getStringId('qstr_yo', true) == 0)\n"
        "assert(game.getNumTemplates() == 1 and game.getCurTemplateNo() == 0)\n"
        "assert(game.getCurTemplateId() == 'mt_battle' and game.getTemplateId(0) == 'mt_battle')\n"
        "assert(game.getNumMeshes() == 1 and game.getMeshId('mesh_banner') == 0)\n"
        "local ps = {}; for i in game.partiesIt() do ps[#ps+1] = i end\n"
        "assert(#ps == 2 and ps[1] == 0 and ps[2] == 2)\n"
        "local ags = {}; for i in game.agentsIt(game.pos.new(), 6) do ags[#ags+1] = i end\n"
        "assert(#ags == 2 and ags[1] == 0 and ags[2] == 1)\n"
        "local props = {}; for i in game.propInstIt(7, 0) do props[#props+1] = i end\n"
        "assert(#props == 1 and props[1] == 0)\n"
        "local pls = {}; for i in game.playersIt(true) do pls[#pls+1] = i end\n"
        "assert(#pls == 1 and pls[1] == 2)\n"
        "_G.trig_hits = 0\n"
        "local ti = game.addTrigger(0, -50, 0, 0, function()\n"
        "    _G.trig_hits = _G.trig_hits + 1\n"
        "    return true\n"
        "end, function()\n"
        "    _G.trig_hits = _G.trig_hits + 10\n"
        "end)\n"
        "assert(ti == 0 and game.getNumTriggers(0) == 1)\n"
        "local ii = game.addItemTrigger(0, -51, function() _G.trig_hits = _G.trig_hits + 100 end)\n"
        "local si = game.addScenePropTrigger(0, -52, function() _G.trig_hits = _G.trig_hits + 1000 end)\n"
        "assert(ii == 0 and si == 0 and game.getNumItemTriggers(0) == 1 and game.getNumScenePropTriggers(0) == 1)\n"
        "local pr = game.addPrsnt({id='hud', flags={1, 4}, mesh='mesh_banner', triggers={[-60]=function() end}})\n"
        "local py = game.addPsys({id='spark', flags=2, mesh='mesh_banner', params={1.5, 2.5}})\n"
        "assert(pr == 0 and py == 0)\n"
        "-- Hook val_add (2105): when arg2 == 99, short-circuit to lhs=777, else pass through\n"
        "game.hookOperation('val_add', function(cur, rhs)\n"
        "    if rhs == 99 then return false, 777 end\n"
        "    -- Calling val_add from Lua must NOT recurse into this hook\n"
        "    _G.hook_saw_sum = game.val_add(cur, rhs)\n"
        "    return true\n"
        "end)\n"
        "-- Hook script 0: mode in gvar g_flag: 1=rewrite param, 2=cancel true, 3=fail\n"
        "game.hookScript('hookable_script', function(a)\n"
        "    local mode = game.gvar.g_flag\n"
        "    if mode == 1 then return true, a + 5 end\n"
        "    if mode == 2 then return false, true end\n"
        "    if mode == 3 then game.fail() end\n"
        "end)\n",
        "hooks_setup");
    expect(lua_setup, "iterators, triggers/prsnt/psys creation, hook setup");

    // Fire triggers through VM execution
    expect(wse::vm::execute_trigger_manager(bundle->mission_templates[0].triggers, -50,
                                            ctx->timersTotal, *ctx),
           "execute template trigger");
    expect(wse::vm::execute_simple_trigger_manager(bundle->item_triggers[0].triggers, -51,
                                                   ctx->timersTotal, *ctx),
           "execute item simple trigger");
    expect(wse::vm::execute_simple_trigger_manager(bundle->scene_props[0].triggers, -52,
                                                   ctx->timersTotal, *ctx),
           "execute scene prop simple trigger");
    expect(rt->do_string("assert(_G.trig_hits == 1111)", "verify_trig_hits"),
           "Lua condition+consequence, item trigger, and scene prop trigger fired");

    // Verify removals
    expect(rt->do_string(
               "assert(game.removeTrigger(0, 0) and game.getNumTriggers(0) == 0)\n"
               "assert(game.removeItemTrigger(0, 0) and game.getNumItemTriggers(0) == 0)\n"
               "assert(game.removeScenePropTrigger(0, 0) and game.getNumScenePropTriggers(0) == 0)\n"
               "assert(game.removePrsnt('hud') and game.removePsys('spark'))\n",
               "verify_removals"),
           "trigger, presentation, and psys removals");

    // Test operation hook via VM statement execution
    ctx->globalVariables.set(2, 10);
    std::array<std::int64_t, 16> locals{};
    const auto hooked_cancel =
        make_stmt(wse::vm::core_op::kValAdd, {op_tag(wse::vm::OperandTag::slot, 2), 99});
    expect(wse::vm::execute_statement(hooked_cancel, wse::vm::kScriptCaller, *ctx, locals),
           "hooked val_add returns true");
    expect(ctx->globalVariables.get(2) == 777, "hookOperation overrode LHS to 777");

    ctx->globalVariables.set(2, 10);
    const auto hooked_pass =
        make_stmt(wse::vm::core_op::kValAdd, {op_tag(wse::vm::OperandTag::slot, 2), 3});
    expect(wse::vm::execute_statement(hooked_pass, wse::vm::kScriptCaller, *ctx, locals),
           "pass-through val_add executes natively");
    expect(ctx->globalVariables.get(2) == 13, "pass-through val_add updated global");
    expect(rt->do_string("assert(_G.hook_saw_sum == 13)", "non_recursive_exec"),
           "execOperation inside hook did not recurse");

    expect(rt->do_string("game.unhookOperation('val_add')", "unhook_op"), "unhookOperation");
    ctx->globalVariables.set(2, 1);
    expect(wse::vm::execute_statement(hooked_cancel, wse::vm::kScriptCaller, *ctx, locals) &&
               ctx->globalVariables.get(2) == 100,
           "unhooked val_add runs original handler");

    // Test script hook modes
    const std::array<std::int64_t, 1> hook_params{10};
    ctx->globalVariables.set(1, 1); // rewrite param: 10 -> 15 * 2 = 30
    expect(wse::vm::execute_script(bundle->scripts, 0, *ctx, hook_params) &&
               ctx->registers[2] == 30,
           "hookScript parameter rewrite");
    ctx->globalVariables.set(1, 2); // cancel returning true
    ctx->registers[2] = 0;
    expect(wse::vm::execute_script(bundle->scripts, 0, *ctx, hook_params) &&
               ctx->registers[2] == 0,
           "hookScript cancel returning true");
    ctx->globalVariables.set(1, 3); // game.fail()
    expect(!wse::vm::execute_script(bundle->scripts, 0, *ctx, hook_params),
           "hookScript game.fail() makes execute_script return false");
    expect(rt->do_string("game.hookScript('hookable_script', nil)", "unhook_script"),
           "unhook script with nil");
    expect(wse::vm::execute_script(bundle->scripts, 0, *ctx, hook_params) &&
               ctx->registers[2] == 20,
           "unhooked script runs normally");

    rt->shutdown();
}

void test_opcodes_5100_to_5114_and_events() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;

    wse::platform::MemoryVfs vfs;
    seed_lua_fixture_files(vfs);
    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;

    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    expect(rt->initialize(*ctx, cfg), "initialize opcodes & events test");

    expect(rt->do_string(
               "ns = {}\n"
               "function ns.combine(a, tag, p)\n"
               "    return a + 10, tag .. '_ok', true, p\n"
               "end\n"
               "_G.events = {}\n"
               "function game.OnWorldTrigger(h) _G.events.world = h end\n"
               "function game.OnRglLogWrite(m) _G.events.rgl = m; log_rgl('reentrant') end\n"
               "function game.OnChatMessageReceived(pl, team, msg)\n"
               "    if msg == 'mute' then return false end\n"
               "    return msg .. '!', true\n"
               "end\n"
               "function game.OnGameLoad() _G.events.loaded = true end\n"
               "function game.OnSave(s) _G.events.save = s end\n"
               "function game.OnLoadSave(s) _G.events.loadsave = s end\n"
               "function game.OnConversationStart(t, a, p) _G.events.cs = t + a + p end\n"
               "function game.OnConversationLine(t, a, ip, txt, sid, ap) _G.events.cl = sid .. ':' .. txt end\n"
               "function game.OnConversationEnd(t, a, p) _G.events.ce = t * 10 + p end\n",
               "opcodes_and_events"),
           "define ns.combine and game event callbacks");

    // Drive VM statements using opcodes 5100..5114
    ctx->stringRegisters[0] = u"hello";
    ctx->positions[0] = wse::core::WseTransform::identity();
    ctx->positions[0].o = {11.0f, 22.0f, 33.0f, 1.0f};

    wse::vm::StatementBlock blk{};
    using wse::vm::WseOpcode;
    const auto op = [](WseOpcode c) { return static_cast<std::uint32_t>(c); };
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_push_int), {5}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_push_str), {0}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_push_pos), {0}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_call), {1, 3})); // s1 = "ns.combine"
    ctx->stringRegisters[1] = u"ns.combine";

    blk.statements.push_back(make_stmt(op(WseOpcode::lua_get_top), {op_tag(wse::vm::OperandTag::global, 0)}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_get_type), {op_tag(wse::vm::OperandTag::global, 1), 1}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_to_int), {op_tag(wse::vm::OperandTag::global, 2), 1}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_to_str), {2, 2}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_to_int), {op_tag(wse::vm::OperandTag::global, 3), 3}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_test), {3}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_to_pos), {1, 4}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_push_int), {99}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_insert), {1}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_remove), {1}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_pop), {2}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_get_top), {op_tag(wse::vm::OperandTag::global, 4)}));
    blk.statements.push_back(make_stmt(op(WseOpcode::lua_set_top), {0}));

    expect(wse::vm::execute_statement_block(blk, 0, 0, {}, *ctx),
           "VM block executing opcodes 5100..5114");
    expect(ctx->registers[0] == 4 &&
               ctx->registers[1] == static_cast<std::int64_t>(wse::game::WseLuaValue::Type::number) &&
               ctx->registers[2] == 15 &&
               ctx->stringRegisters[2] == u"hello_ok" &&
               ctx->registers[3] == 1 &&
               std::fabs(ctx->positions[1].o.z - 33.0f) < 1e-4f &&
               ctx->registers[4] == 2 &&
               rt->lua_get_top() == 0,
           "opcodes 5100..5114 decoded int, string, bool, pos, top, and pop accurately");

    // Fire game events
    rt->on_world_trigger(12.5f);
    rt->on_rgl_log_write("engine tick");
    std::string chat1 = "hi";
    bool sup1 = false;
    rt->on_chat_message_received(1, false, chat1, sup1);
    expect(chat1 == "hi!" && !sup1, "chat callback edited message");
    std::string chat2 = "mute";
    bool sup2 = false;
    rt->on_chat_message_received(1, true, chat2, sup2);
    expect(sup2, "chat callback suppressed muted message");
    rt->on_game_load();
    rt->on_save(3);
    rt->on_load_save(4);
    rt->on_conversation_start(10, 20, 30);
    rt->on_conversation_line(10, 20, true, "hail", "dlga_1", false);
    rt->on_conversation_end(10, 20, 30);

    expect(rt->do_string(
               "assert(_G.events.world == 12.5)\n"
               "assert(_G.events.rgl == 'engine tick')\n"
               "assert(_G.events.loaded == true)\n"
               "assert(_G.events.save == 3 and _G.events.loadsave == 4)\n"
               "assert(_G.events.cs == 60)\n"
               "assert(_G.events.cl == 'dlga_1:hail')\n"
               "assert(_G.events.ce == 130)\n",
               "verify_events"),
           "all 9 WSELuaManager event hooks delivered exact parameters");

    rt->shutdown();
}

void test_sandbox_require_and_headless_host_vfs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "wse_lua_host_test";
    fs::remove_all(root, ec);

    {
        wse::platform::HostVfs host(root);
        seed_lua_fixture_files(host);
        (void)host.write_text(
            "Modules/TestMod/scripts.txt",
            "scriptsfile version 1\n1\n"
            "game_start -1\n1\n"
            "2133 2 " + std::to_string(op_tag(wse::vm::OperandTag::slot, 0)) + " 1\n");
        (void)host.write_text(
            "Modules/TestMod/lua/main.lua",
            "local helper = require('lib.helper')\n"
            "assert(helper.add(20, 22) == 42)\n"
            "assert(os.execute == nil and os.getenv == nil and os.exit == nil)\n"
            "assert(package.loadlib == nil and io.popen == nil)\n"
            "-- Upward read to module root succeeds; above Warband root fails\n"
            "local vf = assert(io.open('../variables.txt', 'r'))\n"
            "assert(vf:read('*l') == 'g_score'); vf:close()\n"
            "local bad_read, _ = io.open('../../../../etc/passwd', 'r')\n"
            "assert(bad_read == nil)\n"
            "-- Upward write is forbidden; %storage% write/rename/remove works\n"
            "local bad_write, _ = io.open('../evil.txt', 'w')\n"
            "assert(bad_write == nil)\n"
            "mkdir('%storage%/sub')\n"
            "local sf = assert(io.open('%storage%/sub/tmp.txt', 'w'))\n"
            "sf:write('ok'); sf:close()\n"
            "assert(os.rename('%storage%/sub/tmp.txt', '%storage%/sub/done.txt'))\n"
            "local rf = assert(io.open('%storage%/sub/done.txt', 'r'))\n"
            "assert(rf:read('*a') == 'ok'); rf:close()\n"
            "-- Bytecode rejection\n"
            "local bc_fn, bc_err = loadstring('\\027LuaQ')\n"
            "assert(bc_fn == nil and string.find(bc_err, 'bytecode') ~= nil)\n"
            "function game.OnGameLoad() game.gvar.g_flag = 88 end\n"
            "function game.OnWorldTrigger(hours) _G.last_hours = hours end\n");

        wse::platform::CaptureLog log;
        wse::game::HeadlessRunOptions opts{};
        opts.module_dir = "Modules/TestMod";
        opts.load_options.require_core_files = false;
        opts.trigger_frames = 2;
        opts.frame_ticks = 100000;
        opts.log = &log;
        opts.enable_lua = true;

        wse::game::HeadlessSession session{};
        session.ctx->luaRuntime = wse::lua::create_lua_runtime();
        session.report =
            wse::game::run_headless_module(host, *session.ctx, *session.bundle, opts);
        expect(session.report.ok &&
                   session.ctx->luaRuntime->is_ready() &&
                   session.ctx->luaRuntime->has_main_script() &&
                   session.ctx->luaRuntime->error_history().empty(),
               "Headless session executed main.lua cleanly with zero Lua errors");
        expect(session.ctx->globalVariables.get(0) == 1 &&
                   session.ctx->globalVariables.get(1) == 88,
               "Headless OnGameLoad set g_flag via Lua and game_start set g_score");
        expect(fs::exists(root / "lua_storage" / "sub" / "done.txt", ec),
               "%storage% write persisted on HostVfs");
    }

    fs::remove_all(root, ec);
}

// Regression coverage for the Phase 2F review: each block below crashed,
// corrupted the Lua stack, or misbehaved before the fix it names.
void test_review_regressions() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;
    bundle->mission_templates.resize(1);
    bundle->item_triggers.resize(1);

    wse::platform::MemoryVfs vfs;
    seed_lua_fixture_files(vfs);
    // Constants that do not fit int64/int32 must be rejected, not cast (UB).
    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/header_operations.py",
        "neg = 0x80000000\n"
        "this_or_next = 0x40000000\n"
        "val_add = 2105\n"
        "assign = 2133\n"
        "lhs_operations = [val_add, assign]\n");
    (void)vfs.write_text(
        "Modules/TestMod/lua/msfiles/header_big.py",
        // 2^63 + 2^11: above int64, and still exactly representable as a double
        // so the assertion below can tell whether the OR happened.
        "top_bits = 0x8000000000000000 | 0x800\n"
        "bad_shift = 1 << -1\n"
        "wide_shift = 1 << 64\n"
        "ok_shift = 1 << 4\n");

    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;
    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    expect(rt->initialize(*ctx, cfg), "initialize regression fixture");
    // val_add, assign, plus dict_get_int from the fixture addon file.
    expect(rt->operation_count() == 3U && !rt->operation_flags("neg").has_value(),
           "flag constants above the opcode range are not operations");
    expect(rt->do_string(
               "assert(game.const.big.ok_shift == 16)\n"
               "assert(game.const.big.top_bits == 2^63 + 2048)\n"
               "assert(game.const.big.top_bits ~= 2^63)\n"
               "assert(rawget(game.const.big, 'bad_shift') == nil)\n"
               "assert(rawget(game.const.big, 'wide_shift') == nil)\n",
               "consts"),
           "out-of-range constant expressions are rejected without UB");

    // 1. Pushing past LUA_MINSTACK from VM opcodes grows the stack.
    for (int i = 0; i < 500; ++i) rt->lua_push_int(i);
    rt->lua_push_str("s");
    rt->lua_push_pos(wse::core::WseTransform::identity());
    expect(rt->lua_get_top() == 502, "push opcodes grow the Lua stack");
    expect(rt->lua_to_int(500) == 499, "values survive stack growth");

    // 2. lua_set_top grows the stack, and refuses an impossible size.
    expect(rt->lua_set_top(3000) && rt->lua_get_top() == 3000, "lua_set_top grows the stack");
    expect(!rt->lua_set_top(100000000), "lua_set_top rejects an unreachable height");
    expect(rt->lua_set_top(0) && rt->lua_get_top() == 0, "lua_set_top(0) clears the stack");

    // 3. lua_to_int on inf / NaN / huge values must not raise an unprotected error.
    expect(rt->do_string("function inf() return 1/0, 0/0, 1e300 end", "inf"), "define inf()");
    expect(rt->lua_call("inf", 0, 0) && rt->lua_get_top() == 3, "inf() leaves three results");
    expect(rt->lua_to_int(1) == 0 && rt->lua_to_int(2) == 0 && rt->lua_to_int(3) == 0,
           "lua_to_int yields 0 for non-representable numbers");
    (void)rt->lua_set_top(0);

    // 4. An operation hook returning a non-number lhs is reported, not fatal.
    expect(rt->do_string("game.hookOperation('assign', function() return false, {} end)", "hk"),
           "install bad-lhs hook");
    {
        const std::size_t errors_before = rt->error_history().size();
        std::array<std::int64_t, 16> locals{};
        locals[0] = 41;
        const auto st = make_stmt(2133, {op_tag(wse::vm::OperandTag::local, 0), 5});
        expect(wse::vm::execute_statement(st, 0, *ctx, locals), "hooked assign still succeeds");
        expect(locals[0] == 41, "bad lhs hook result does not store");
        expect(rt->error_history().size() == errors_before + 1U, "bad lhs hook result is reported");
        expect(rt->lua_get_top() == 0, "hook leaves the stack balanced");
    }
    expect(rt->do_string("game.unhookOperation('assign')", "unhk"), "remove hook");

    // 5. lua_triggerCallback consumes its arguments.
    expect(rt->do_string(
               "_G.cb_sum = 0\n"
               "game.addItemTrigger(0, -51, function(a, b, c) _G.cb_sum = a + b + c return true end)\n",
               "cb"),
           "register callback");
    {
        const auto ref = static_cast<std::int32_t>(
            bundle->item_triggers[0].triggers.triggers[0].block.statements[0].operands[0]);
        rt->lua_push_int(7); // stays below the arguments
        rt->lua_push_int(1);
        rt->lua_push_int(2);
        rt->lua_push_int(3);
        expect(rt->lua_trigger_callback(ref, 3, 0), "callback returns true");
        expect(rt->lua_get_top() == 1 && rt->lua_to_int(1) == 7,
               "callback arguments and result are removed from the stack");
        expect(!rt->lua_trigger_callback(-12345, 1, 0) && rt->lua_get_top() == 0,
               "invalid callback ref still consumes its arguments");
        expect(rt->do_string("assert(_G.cb_sum == 6)", "cbsum"), "callback saw its arguments");
    }

    // 6. Triggers that add/remove triggers while the manager is iterating.
    expect(rt->do_string(
               "_G.runs = 0\n"
               "game.addTrigger(0, -50, 0, 0, function()\n"
               "    _G.runs = _G.runs + 1\n"
               "    assert(game.removeTrigger(0, 0) == true)\n"       // itself
               "    for i = 1, 64 do\n"
               "        local idx = game.addTrigger(0, -50, 0, 0, function() _G.runs = _G.runs + 100 return false end)\n"
               "        assert(idx == i - 1, 'index ' .. tostring(idx))\n"
               "    end\n"
               "    assert(game.getNumTriggers(0) == 64)\n"
               "    assert(game.removeTrigger(0, 63) == true)\n"
               "    assert(game.removeTrigger(0, 63) == false)\n"
               "    assert(game.getNumTriggers(0) == 63)\n"
               "    return true\n"
               "end, function() _G.runs = _G.runs + 10 end)\n",
               "selfmod"),
           "register self-modifying trigger");
    auto& tm = bundle->mission_templates[0].triggers;
    expect(wse::vm::execute_trigger_manager(tm, -50, ctx->timersTotal, *ctx),
           "self-modifying trigger executes");
    expect(rt->error_history().size() == 1U, "self-modifying trigger raised no Lua error");
    expect(tm.triggers.size() == 63U, "queued edits applied after the manager finished");
    expect(ctx->triggerIterationDepth == 0, "trigger iteration depth unwinds");
    expect(rt->do_string("assert(_G.runs == 11, tostring(_G.runs))", "runs"),
           "removed trigger finished its run; queued triggers did not run early");
    expect(wse::vm::execute_trigger_manager(tm, -50, ctx->timersTotal, *ctx) &&
               rt->do_string("assert(_G.runs == 11 + 6300, tostring(_G.runs))", "runs2"),
           "queued triggers run on the next pass");
    expect(rt->do_string("assert(game.addTrigger(0, -1, 0, 0, function() end) == 63)", "imm"),
           "edits outside trigger execution apply immediately");
    expect(tm.triggers.size() == 64U, "immediate add");

    // 7. Lua errors unwind C++ frames (no leak under LeakSanitizer) and stay catchable.
    expect(rt->do_string(
               "local ok, err = pcall(dofile, 'a_missing_file_with_a_long_enough_name_to_heap_allocate.lua')\n"
               "assert(not ok and err:find('cannot open', 1, true))\n"
               "ok = pcall(game.execOperation, 1, 0, {}, {})\n"
               "assert(not ok)\n"
               "ok = pcall(game.addTrigger, 0, 0, 0, 0, function() end, 5)\n"
               "assert(not ok)\n",
               "unwind"),
           "Lua errors propagate through C++ callbacks");

    // 8. Sandboxed io / os semantics match stock Lua 5.1.
    expect(rt->do_string(
               "local f = assert(io.open('%storage%/e.txt', 'w'))\n"
               "assert(f:write('') == true)\n"
               "f:close()\n"
               "f = assert(io.open('%storage%/e.txt', 'r'))\n"
               "assert(f:read('*a') == '')\n"
               "assert(f:read('*l') == nil and f:read(1) == nil)\n"
               "f:close()\n"
               "io.output('%storage%/o.txt'); io.write('x'); assert(io.close() == true)\n"
               "f = assert(io.open('%storage%/o.txt', 'r')); assert(f:read('*a') == 'x'); f:close()\n"
               "assert(os.rename('%storage%/o.txt', '%storage%/O.TXT') == true)\n"
               "f = assert(io.open('%storage%/o.txt', 'r'), 'self-rename deleted the file')\n"
               "assert(f:read('*a') == 'x'); f:close()\n"
               "assert(os.rename('%storage%/o.txt', '%storage%/p.txt') == true)\n"
               "assert(io.open('%storage%/o.txt', 'r') == nil and io.open('%storage%/p.txt', 'r'))\n",
               "io"),
           "io.close(), read('*a') at EOF, file:write result, os.rename onto itself");

    // 8b. Presentation removal by id while a trigger executes sees the queued
    // edits: one added in the same trigger is found, one already removed is not.
    expect(rt->do_string(
               "assert(game.addPrsnt({id = 'keep'}) == 0)\n"
               "_G.prs = {}\n"
               "game.addTrigger(0, -52, 0, 0, function()\n"
               "    _G.prs.add = game.addPrsnt({id = 'fresh'})\n"
               "    _G.prs.rm_fresh = game.removePrsnt('fresh')\n"
               "    _G.prs.rm_keep = game.removePrsnt('keep')\n"
               "    _G.prs.rm_keep_again = game.removePrsnt('keep')\n"
               "    _G.prs.rm_index = game.removePrsnt(0)\n"
               "    _G.prs.add2 = game.addPrsnt({id = 'second'})\n"
               "    return true\n"
               "end)\n",
               "prsnt"),
           "register presentation-editing trigger");
    expect(wse::vm::execute_trigger_manager(tm, -52, ctx->timersTotal, *ctx) &&
               rt->do_string(
                   "assert(_G.prs.add == 1 and _G.prs.rm_fresh == true)\n"
                   "assert(_G.prs.rm_keep == true and _G.prs.rm_keep_again == false)\n"
                   "assert(_G.prs.rm_index == false and _G.prs.add2 == 0)\n",
                   "prsnt2"),
           "queued presentation removals by id match in-place removal");
    expect(bundle->presentations.size() == 1U && bundle->presentations[0].id == "second",
           "queued presentation edits applied in order");

    // 9. A file left open is flushed by an explicit shutdown() ...
    expect(rt->do_string("_G.keep = io.open('%storage%/late.txt', 'w'); _G.keep:write('late')", "late"),
           "leave a dirty file open");
    rt->shutdown();
    std::string late;
    expect(vfs.read_text("lua_storage/late.txt", late) && late == "late",
           "shutdown() flushes open files");
    expect(tm.triggers.size() == 65U && !rt->is_ready(), "shutdown leaves triggers in place, not ready");

    // ... and a runtime destroyed after its Vfs must not touch the dead Vfs.
    {
        auto ctx2 = std::make_unique<wse::vm::VmContext>();
        auto bundle2 = std::make_unique<wse::game::ModuleBundle>();
        ctx2->scripts = &bundle2->scripts;
        auto rt2 = wse::lua::create_lua_runtime();
        {
            wse::platform::MemoryVfs short_lived;
            seed_lua_fixture_files(short_lived);
            wse::vm::WseLuaInitConfig cfg2{};
            cfg2.module_dir = "Modules/TestMod";
            cfg2.vfs = &short_lived;
            cfg2.bundle = bundle2.get();
            expect(rt2->initialize(*ctx2, cfg2), "initialize short-lived Vfs runtime");
            expect(rt2->do_string("_G.keep = io.open('%storage%/x.txt', 'w'); _G.keep:write('x')", "x"),
                   "leave a dirty file open on a short-lived Vfs");
        }
        rt2.reset(); // destructor path: must not flush through the destroyed Vfs
    }
}

// A Vfs whose reads throw: stands in for any C++ exception (std::bad_alloc, a
// misbehaving platform backend) escaping a callback that Lua called.
class ThrowingVfs final : public wse::platform::Vfs {
public:
    explicit ThrowingVfs(wse::platform::Vfs& inner) : inner_(inner) {}
    bool armed = false;

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        maybe_throw();
        return inner_.read_file(path, out);
    }
    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        return inner_.write_file(path, data);
    }
    bool remove_file(std::string_view path) override { return inner_.remove_file(path); }
    bool list_dir(std::string_view path, std::vector<wse::platform::VfsEntry>& out) override {
        return inner_.list_dir(path, out);
    }
    bool is_file(std::string_view path) override {
        maybe_throw();
        return inner_.is_file(path);
    }
    bool is_dir(std::string_view path) override { return inner_.is_dir(path); }
    bool make_dir(std::string_view path) override { return inner_.make_dir(path); }
    bool remove_dir(std::string_view path) override { return inner_.remove_dir(path); }

private:
    void maybe_throw() const {
        if (armed) throw std::runtime_error("vfs backend exploded");
    }
    wse::platform::Vfs& inner_;
};

// Lua built as C++ guards calls with `catch (...)`; without the callback
// trampoline a C++ exception surfaced as an error with no message.
void test_cpp_exception_becomes_lua_error() {
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    auto ctx = std::make_unique<wse::vm::VmContext>();
    ctx->scripts = &bundle->scripts;
    wse::platform::MemoryVfs mem;
    seed_lua_fixture_files(mem);
    ThrowingVfs vfs(mem);

    auto rt = wse::lua::create_lua_runtime();
    ctx->luaRuntime = rt;
    wse::vm::WseLuaInitConfig cfg{};
    cfg.module_dir = "Modules/TestMod";
    cfg.vfs = &vfs;
    cfg.bundle = bundle.get();
    expect(rt->initialize(*ctx, cfg), "initialize throwing-Vfs runtime");

    vfs.armed = true;
    expect(rt->do_string(
               "local ok, err = pcall(dofile, 'anything.lua')\n"
               "assert(not ok, 'dofile should fail')\n"
               "assert(type(err) == 'string', 'error is ' .. type(err))\n"
               "assert(err:find('vfs backend exploded', 1, true), err)\n"
               "ok, err = pcall(io.open, '%storage%/x.txt', 'r')\n"
               "assert(not ok and type(err) == 'string' and err:find('exploded', 1, true))\n"
               "_G.survived = true\n",
               "throw"),
           "C++ exception in a callback is a catchable Lua error with its message");
    const std::size_t errors_before = rt->error_history().size();
    expect(!rt->do_string("dofile('anything.lua')", "throw2"),
           "uncaught C++ exception fails the chunk");
    expect(rt->error_history().size() == errors_before + 1U &&
               rt->error_history().back().find("vfs backend exploded") != std::string::npos,
           "uncaught C++ exception is reported with its message");
    vfs.armed = false;
    expect(rt->do_string("assert(_G.survived == true)", "after") && rt->lua_get_top() == 0,
           "runtime stays usable after a C++ exception");
    rt->shutdown();
}

} // namespace

namespace wse::selftest {

int run_lua_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_stdlib_bit_table_string_vector_pos();
    test_ops_constants_variables_and_exec();
    test_hooks_triggers_and_iterators();
    test_opcodes_5100_to_5114_and_events();
    test_sandbox_require_and_headless_host_vfs();
    test_review_regressions();
    test_cpp_exception_becomes_lua_error();

    if (failures == 0) {
        std::printf("lua tests: PASS\n");
        return 0;
    }
    std::printf("lua tests: FAIL (%d)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_lua_suite(nullptr);
}
#endif
