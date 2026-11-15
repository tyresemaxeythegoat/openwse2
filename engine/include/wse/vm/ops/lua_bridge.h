// Milestone 6 Lua 5.1 stack, call, and trigger callback bridge opcode handlers
// (15 WSE opcodes: 5100..5114).

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline bool wse_m6_valid_lua_dot_path(
    std::string_view name) noexcept {
    if (name.empty() || name.front() == '.' || name.back() == '.') {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] == '.' && name[i - 1] == '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline WseDispatch execute_wse_lua_bridge(
    std::uint32_t opcode,
    const WseOperands& operands,
    WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    auto* world = operands.state.world;
    auto* rt = (operands.state.lua_runtime != nullptr && operands.state.lua_runtime->is_ready())
                   ? operands.state.lua_runtime
                   : nullptr;
    auto strings = operands.state.strings;
    const auto positions = operands.state.positions;
    const auto report_lua_err = [&](std::string msg) {
        if (world != nullptr) {
            world->m6.lua.add_error(std::move(msg));
        }
    };

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::lua_get_top: {
        // 5100 (0x1401f409c)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            effect.value = rt->lua_get_top();
            effect.store_value = true;
            return WseDispatch::handled;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.lua.get_top();
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::lua_set_top: {
        // 5101 (0x1401f40ae)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            if (!rt->lua_set_top(narrow[0])) {
                report_lua_err("lua_set_top: stack overflow");
                return WseDispatch::ignored;
            }
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        // Safety deviation: indices above kMaxLuaStackSize are rejected instead
        // of resizing the stack to a script-controlled size.
        if (!world->m6.lua.set_top(narrow[0])) {
            world->m6.lua.add_error("lua_set_top: stack overflow");
            return WseDispatch::ignored;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_insert: {
        // 5102 (0x1401f40c3)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            if (rt->lua_value_type(narrow[0]) == -1) {
                return WseDispatch::ignored;
            }
            rt->lua_insert(narrow[0]);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[0]) < 0) {
            return WseDispatch::ignored;
        }
        world->m6.lua.insert(narrow[0]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_remove: {
        // 5103 (0x1401f40e7)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            if (rt->lua_value_type(narrow[0]) == -1) {
                return WseDispatch::ignored;
            }
            rt->lua_remove(narrow[0]);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[0]) < 0) {
            return WseDispatch::ignored;
        }
        world->m6.lua.remove(narrow[0]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_pop: {
        // 5104 (0x1401f410b)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            rt->lua_pop(narrow[0]);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.lua.pop(narrow[0]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_to_int: {
        // 5105 (0x1401f4124)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            if (rt->lua_value_type(narrow[1]) == -1) {
                return WseDispatch::ignored;
            }
            effect.value = rt->lua_to_int(narrow[1]);
            effect.store_value = true;
            return WseDispatch::handled;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[1]) < 0) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.lua.to_integer(narrow[1]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::lua_to_str: {
        // 5106 (0x1401f414a)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        if (rt != nullptr) {
            if (rt->lua_value_type(narrow[1]) == -1) {
                return WseDispatch::ignored;
            }
            std::string s;
            if (!rt->lua_to_string(narrow[1], s)) {
                report_lua_err("value at index " + std::to_string(narrow[1]) +
                               " is not a valid string");
                return WseDispatch::handled_no_store;
            }
            return wse_store_string(strings, narrow[0], wse_utf8_to_utf16(s))
                       ? WseDispatch::handled_no_store
                       : WseDispatch::ignored;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[1]) < 0) {
            return WseDispatch::ignored;
        }
        const std::string* s = world->m6.lua.to_string(narrow[1]);
        if (s == nullptr) {
            world->m6.lua.add_error(
                "value at index " + std::to_string(narrow[1]) +
                " is not a valid string");
            return WseDispatch::handled_no_store;
        }
        return wse_store_string(strings, narrow[0], wse_utf8_to_utf16(*s))
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::lua_to_pos: {
        // 5107 (0x1401f41b7)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        if (rt != nullptr) {
            if (rt->lua_value_type(narrow[1]) == -1) {
                return WseDispatch::ignored;
            }
            WseTransform pos = WseTransform::identity();
            if (!rt->lua_to_pos(narrow[1], pos)) {
                report_lua_err("value at index " + std::to_string(narrow[1]) +
                               " is not a valid position");
                return WseDispatch::handled_no_store;
            }
            positions[static_cast<std::size_t>(narrow[0])] = pos;
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[1]) < 0) {
            return WseDispatch::ignored;
        }
        const WseTransform* pos = world->m6.lua.to_pos(narrow[1]);
        if (pos == nullptr) {
            world->m6.lua.add_error(
                "value at index " + std::to_string(narrow[1]) +
                " is not a valid position");
            return WseDispatch::handled_no_store;
        }
        positions[static_cast<std::size_t>(narrow[0])] = *pos;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_push_int: {
        // 5108 (0x1401f42da)
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            rt->lua_push_int(wide[0]);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.lua.push_int(wide[0]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_push_str: {
        // 5109 (0x1401f42ef)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string parsed;
        if (!wse_resolve_parsed_utf8_operand(0, operands, parsed)) {
            return WseDispatch::ignored;
        }
        if (rt != nullptr) {
            rt->lua_push_str(parsed);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.lua.push_str(std::move(parsed));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_push_pos: {
        // 5110 (0x1401f4357)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        if (rt != nullptr) {
            rt->lua_push_pos(positions[static_cast<std::size_t>(narrow[0])]);
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.lua.push_pos(
            positions[static_cast<std::size_t>(narrow[0])]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_get_type: {
        // 5111 (0x1401f4389)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            const std::int32_t t = rt->lua_value_type(narrow[1]);
            if (t == -1) {
                return WseDispatch::ignored;
            }
            effect.value = t;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[1]) < 0) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.lua.value_type(narrow[1]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::lua_call: {
        // 5112 (0x1401f43af)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string func_name;
        if (!wse_resolve_parsed_utf8_operand(0, operands, func_name)) {
            return WseDispatch::ignored;
        }
        const std::int32_t num_args = narrow[1];
        if (num_args < 0) {
            return WseDispatch::ignored;
        }
        std::replace(func_name.begin(), func_name.end(), ' ', '_');
        if (rt != nullptr) {
            if (!wse_m6_valid_lua_dot_path(func_name)) {
                report_lua_err("bad func_name: '" + func_name + "'");
                return WseDispatch::handled_no_store;
            }
            if (rt->lua_get_top() < num_args) {
                report_lua_err("not enough arguments on stack");
                return WseDispatch::handled_no_store;
            }
            const bool ok =
                rt->lua_call(func_name, num_args, operands.state.caller);
            if (!ok) {
                report_lua_err("lua_call failed: " + func_name);
                return WseDispatch::condition_false;
            }
            return WseDispatch::handled_no_store;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        auto& lua = world->m6.lua;
        if (lua.get_top() < num_args) {
            lua.add_error("not enough arguments on stack");
            return WseDispatch::handled_no_store;
        }
        lua.lua_context = operands.state.caller;
        if (!wse_m6_valid_lua_dot_path(func_name)) {
            lua.add_error("bad func_name: '" + func_name + "'");
            return WseDispatch::handled_no_store;
        }
        const auto it = lua.functions.find(func_name);
        if (it == lua.functions.end() || !it->second) {
            lua.add_error("'_G." + func_name + "' is nil");
            return WseDispatch::handled_no_store;
        }
        const int rc = it->second(lua, num_args);
        if (rc < 0) {
            lua.add_error("lua_call failed: " + func_name);
            return WseDispatch::condition_false;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::lua_triggerCallback: {
        // 5113 (0x1401f4686)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t cb_ctx =
            (narrow.size() >= 3) ? narrow[2] : operands.state.caller;
        if (rt != nullptr) {
            return rt->lua_trigger_callback(narrow[0], narrow[1], cb_ctx)
                       ? WseDispatch::handled_no_store
                       : WseDispatch::condition_false;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        auto& lua = world->m6.lua;
        lua.lua_context = (narrow.size() >= 3) ? narrow[2] : 0;
        const auto it = lua.callbacks.find(narrow[0]);
        if (it == lua.callbacks.end() || !it->second) {
            lua.add_error("missing lua callback " +
                                 std::to_string(narrow[0]));
            return WseDispatch::condition_false;
        }
        const std::size_t before = lua.stack.size();
        const int rc = it->second(lua, narrow[1]);
        if (rc < 0) {
            lua.stack.resize(before);
            lua.add_error("lua callback error");
            return WseDispatch::condition_false;
        }
        bool cond_ok = true;
        if (narrow[1] == 0 && lua.stack.size() > before &&
            lua.stack.back().type == wse::game::WseLuaValue::Type::boolean) {
            cond_ok = lua.stack.back().bool_val;
        }
        lua.stack.resize(before);
        return cond_ok ? WseDispatch::handled_no_store
                       : WseDispatch::condition_false;
    }
    case WseOpcode::lua_test: {
        // 5114 (0x1401f4757)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (rt != nullptr) {
            return rt->lua_test(narrow[0]) ? WseDispatch::handled_no_store
                                           : WseDispatch::condition_false;
        }
        if (world == nullptr || world->m6.lua.resolve_index(narrow[0]) < 0) {
            return WseDispatch::condition_false;
        }
        return world->m6.lua.to_boolean(narrow[0])
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
