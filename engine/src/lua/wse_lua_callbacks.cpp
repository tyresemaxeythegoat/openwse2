#include "wse_lua_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wse::lua::detail {
namespace {

[[nodiscard]] bool read_table_vec3(lua_State* L, int tbl_idx, const char* field,
                                   core::WseVec4& out, float def_x, float def_y,
                                   float def_z, float w = 0.0f) {
    lua_getfield(L, tbl_idx, field);
    if (!lua_istable(L, -1)) {
        c_lua_pop(L, 1);
        out = core::WseVec4{def_x, def_y, def_z, w};
        return false;
    }
    auto read_comp = [&](const char* k, float def) {
        lua_getfield(L, -1, k);
        const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
        c_lua_pop(L, 1);
        return v;
    };
    out = core::WseVec4{read_comp("x", def_x), read_comp("y", def_y),
                        read_comp("z", def_z), w};
    c_lua_pop(L, 1);
    return true;
}

void push_vec3_table(lua_State* L, const core::WseVec4& v) {
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(v.x));
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, static_cast<lua_Number>(v.y));
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, static_cast<lua_Number>(v.z));
    lua_setfield(L, -2, "z");

    lua_getglobal(L, "vector3");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "mt");
        if (lua_istable(L, -1)) {
            lua_setmetatable(L, -3);
        } else {
            c_lua_pop(L, 1);
        }
    }
    c_lua_pop(L, 1);
}

[[nodiscard]] bool extract_trigger_block(lua_State* L, int fn_idx,
                                         vm::StatementBlock& out_block) {
    if (!lua_isfunction(L, fn_idx)) return false;
    lua_pushvalue(L, fn_idx);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    vm::Statement stmt{};
    stmt.opcode = static_cast<std::uint32_t>(vm::WseOpcode::lua_triggerCallback);
    stmt.numOperands = 2;
    stmt.operands[0] = static_cast<std::int64_t>(ref);
    stmt.operands[1] = 0;
    out_block.statements.clear();
    out_block.statements.push_back(stmt);
    return true;
}

[[nodiscard]] std::uint32_t read_flags_arg(lua_State* L, int idx) {
    if (lua_isnumber(L, idx)) {
        return static_cast<std::uint32_t>(check_i64(L, idx));
    }
    if (lua_istable(L, idx)) {
        std::uint64_t acc = 0;
        const size_t n = lua_objlen(L, idx);
        for (size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, static_cast<int>(i));
            if (lua_isnumber(L, -1)) {
                acc |= static_cast<std::uint64_t>(check_i64(L, -1));
            }
            c_lua_pop(L, 1);
        }
        return static_cast<std::uint32_t>(acc);
    }
    return 0U;
}

[[nodiscard]] game::TriggerManager* resolve_template_triggers(
    WseLuaManagerImpl& mgr, std::int32_t mst_no) {
    if (mgr.cfg_.bundle == nullptr) return nullptr;
    if (mst_no == -1) return &mgr.cfg_.bundle->game_triggers;
    if (mst_no < 0 ||
        static_cast<std::size_t>(mst_no) >= mgr.cfg_.bundle->mission_templates.size()) {
        return nullptr;
    }
    return &mgr.cfg_.bundle->mission_templates[static_cast<std::size_t>(mst_no)].triggers;
}

template <class Vec, class UnrefFn>
bool remove_trigger_at(Vec& vec, std::int32_t idx, UnrefFn&& unref_fn) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= vec.size()) return false;
    unref_fn(vec[static_cast<std::size_t>(idx)]);
    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(idx));
    return true;
}

// Removes `vec[idx]` now, or queues the removal when a trigger is executing
// (erasing would invalidate the trigger the VM is in the middle of running).
// The bounds check uses the size the container will have once queued edits
// are applied, so the result matches in-place removal.
template <class Vec, class UnrefFn>
bool remove_at_or_defer(WseLuaManagerImpl& mgr, Vec& vec, std::int32_t idx,
                        UnrefFn unref_fn) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= mgr.virtual_size(vec)) return false;
    Vec* target = &vec;
    mgr.run_or_defer_edit(target, -1, [target, idx, unref_fn] {
        (void)remove_trigger_at(*target, idx, unref_fn);
    });
    return true;
}

// Appends `value` now or queues the append; returns the index it will occupy.
template <class Vec, class T>
std::size_t append_or_defer(WseLuaManagerImpl& mgr, Vec& vec, T&& value) {
    const std::size_t index = mgr.virtual_size(vec);
    Vec* target = &vec;
    auto pending = std::make_shared<std::decay_t<T>>(std::forward<T>(value));
    mgr.run_or_defer_edit(target, 1, [target, pending] {
        target->push_back(std::move(*pending));
    });
    return index;
}

[[nodiscard]] std::int32_t resolve_script_index(WseLuaManagerImpl& mgr,
                                                lua_State* L, int idx) {
    if (mgr.ctx_ == nullptr || mgr.ctx_->scripts == nullptr) return -1;
    const auto& scripts = mgr.ctx_->scripts->scripts;
    if (lua_isnumber(L, idx)) {
        const auto sidx = static_cast<std::int32_t>(check_i64(L, idx));
        if (sidx >= 0 && static_cast<std::size_t>(sidx) < scripts.size()) return sidx;
        return -1;
    }
    if (lua_isstring(L, idx)) {
        const std::string_view id = to_sv(L, idx);
        for (std::size_t i = 0; i < scripts.size(); ++i) {
            if (scripts[i].id == id) return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] std::int32_t resolve_opcode_arg(WseLuaManagerImpl& mgr,
                                              lua_State* L, int idx) {
    if (lua_isnumber(L, idx)) return static_cast<std::int32_t>(check_i64(L, idx));
    if (lua_isstring(L, idx)) {
        const auto it = mgr.ops_by_name_.find(std::string(to_sv(L, idx)));
        if (it != mgr.ops_by_name_.end()) return it->second.opcode;
    }
    return -1;
}

// ---- game.* C callbacks (36 total) ----

int lc_execOperation(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    vm::VmContext& ctx = *mgr.ctx_;
    const auto opcode = static_cast<std::uint32_t>(check_i64(L, 1));
    const int top = lua_gettop(L);

    std::size_t cur_str_reg = ctx.stringRegisters.size();
    std::size_t cur_pos_reg = ctx.positions.size();
    auto pack_arg = [&](int lua_idx, std::int64_t& out) -> bool {
        if (lua_isboolean(L, lua_idx)) {
            out = lua_toboolean(L, lua_idx) ? 1 : 0;
            return true;
        }
        if (lua_isnumber(L, lua_idx)) {
            out = check_i64(L, lua_idx);
            return true;
        }
        if (lua_isstring(L, lua_idx)) {
            if (cur_str_reg == 0) return false;
            --cur_str_reg;
            ctx.stringRegisters[cur_str_reg] = core::wse_utf8_to_utf16(to_sv(L, lua_idx));
            out = static_cast<std::int64_t>(cur_str_reg);
            return true;
        }
        if (is_pos(L, lua_idx)) {
            if (cur_pos_reg == 0) return false;
            --cur_pos_reg;
            if (!read_pos(L, lua_idx, ctx.positions[cur_pos_reg])) return false;
            out = static_cast<std::int64_t>(cur_pos_reg);
            return true;
        }
        return false;
    };

    // call_script (1) calls vm::execute_script directly.
    if (opcode == vm::core_op::kCallScript) {
        if (top < 2 || ctx.scripts == nullptr) {
            lua_pushboolean(L, 0);
            lua_pushinteger(L, -1);
            return 2;
        }
        const auto script_no = static_cast<std::int32_t>(check_i64(L, 2));
        std::vector<std::int64_t> params;
        params.reserve(static_cast<std::size_t>(std::max(0, top - 2)));
        for (int i = 3; i <= top; ++i) {
            std::int64_t v = 0;
            if (!pack_arg(i, v)) {
                return luaL_argerror(L, i, "invalid operand type for call_script");
            }
            params.push_back(v);
        }
        const bool ok = vm::execute_script(*ctx.scripts, script_no, ctx, params);
        lua_pushboolean(L, ok ? 1 : 0);
        lua_pushinteger(L, 0);
        return 2;
    }

    std::uint16_t flags = 0;
    if (const auto it = mgr.op_flags_by_code_.find(static_cast<std::int32_t>(opcode));
        it != mgr.op_flags_by_code_.end()) {
        flags = it->second;
    }
    const bool is_lhs = (flags & vm::kLuaOpFlagLhs) != 0;
    const bool is_cf = (flags & vm::kLuaOpFlagCf) != 0;

    vm::Statement stmt{};
    stmt.opcode = opcode;
    std::array<std::int64_t, vm::kDefaultLocalVariables> local_vals{};
    int arg_idx = 2;

    if (is_lhs) {
        local_vals[0] = (arg_idx <= top && !lua_isnil(L, arg_idx))
                            ? check_i64(L, arg_idx)
                            : 0;
        if (arg_idx <= top) ++arg_idx;
        stmt.operands[0] =
            static_cast<std::int64_t>((static_cast<std::uint64_t>(vm::OperandTag::local) << 56));
        stmt.numOperands = 1;
    }

    for (int i = arg_idx; i <= top; ++i) {
        if (static_cast<std::size_t>(stmt.numOperands) >= vm::kMaxOperands) {
            return luaL_error(L, "too many operands in execOperation");
        }
        std::int64_t val = 0;
        if (!pack_arg(i, val)) {
            return luaL_argerror(L, i, "unsupported operand type");
        }
        stmt.operands[static_cast<std::size_t>(stmt.numOperands++)] = val;
    }

    const bool cf = vm::execute_statement(
        stmt, vm::kScriptCaller, ctx,
        std::span<std::int64_t>(local_vals.data(), local_vals.size()),
        /*allowLuaHook=*/false);

    int nret = 0;
    if (is_cf) {
        lua_pushboolean(L, cf ? 1 : 0);
        ++nret;
    }
    if (is_lhs) {
        lua_pushnumber(L, static_cast<lua_Number>(local_vals[0]));
        ++nret;
    }
    lua_pushinteger(L, 0);
    ++nret;
    return nret;
}

int lc_getOperationFlags(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view name = to_sv(L, 1);
    const auto flags = mgr.operation_flags(name);
    if (!flags) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(*flags));
    return 1;
}

int lc_getReg(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    int type_id = 0;
    int reg_idx = 0;
    if (lua_gettop(L) >= 2) {
        type_id = static_cast<int>(check_i64(L, 1));
        reg_idx = static_cast<int>(check_i64(L, 2));
    } else {
        reg_idx = static_cast<int>(check_i64(L, 1));
    }
    if (reg_idx < 0 || reg_idx >= 128) return luaL_error(L, "register index out of range");

    const auto idx = static_cast<std::size_t>(reg_idx);
    if (type_id == 0) {
        lua_pushnumber(L, static_cast<lua_Number>(mgr.ctx_->registers[idx]));
        return 1;
    }
    if (type_id == 1) {
        push_sv(L, core::wse_utf16_to_utf8(mgr.ctx_->stringRegisters[idx]));
        return 1;
    }
    if (type_id == 2) {
        push_pos(L, mgr.ctx_->positions[idx]);
        return 1;
    }
    return luaL_error(L, "invalid register typeId %d", type_id);
}

int lc_setReg(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    int type_id = 0;
    int reg_idx = 0;
    int val_arg = 2;
    if (lua_gettop(L) >= 3) {
        type_id = static_cast<int>(check_i64(L, 1));
        reg_idx = static_cast<int>(check_i64(L, 2));
        val_arg = 3;
    } else {
        reg_idx = static_cast<int>(check_i64(L, 1));
    }
    if (reg_idx < 0 || reg_idx >= 128) return luaL_error(L, "register index out of range");

    const auto idx = static_cast<std::size_t>(reg_idx);
    if (type_id == 0) {
        mgr.ctx_->registers[idx] = check_i64(L, val_arg);
        return 0;
    }
    if (type_id == 1) {
        mgr.ctx_->stringRegisters[idx] = core::wse_utf8_to_utf16(to_sv(L, val_arg));
        return 0;
    }
    if (type_id == 2) {
        if (!read_pos(L, val_arg, mgr.ctx_->positions[idx])) {
            return luaL_argerror(L, val_arg, "expected game.pos table");
        }
        return 0;
    }
    return luaL_error(L, "invalid register typeId %d", type_id);
}

std::int32_t resolve_gvar_slot(WseLuaManagerImpl& mgr, lua_State* L, int idx) {
    if (lua_isnumber(L, idx)) return static_cast<std::int32_t>(check_i64(L, idx));
    if (lua_isstring(L, idx)) {
        const auto slot = mgr.gvar_index(to_sv(L, idx));
        if (slot) return *slot;
    }
    return -1;
}

int lc_getGvar(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const std::int32_t slot = resolve_gvar_slot(mgr, L, 1);
    if (slot < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(mgr.ctx_->globalVariables.get(slot)));
    return 1;
}

int lc_setGvar(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    const std::int32_t slot = resolve_gvar_slot(mgr, L, 1);
    if (slot < 0) return 0;
    (void)mgr.ctx_->globalVariables.set(slot, check_i64(L, 2));
    return 0;
}

int lc_getScriptNo(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::int32_t idx = resolve_script_index(mgr, L, 1);
    if (idx < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, idx);
    return 1;
}

int lc_getString(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const auto idx = static_cast<std::int32_t>(check_i64(L, 1));
    const bool quick = lua_toboolean(L, 2) != 0;
    const auto& vec =
        quick ? mgr.cfg_.bundle->quick_string_entries : mgr.cfg_.bundle->string_entries;
    if (idx < 0 || static_cast<std::size_t>(idx) >= vec.size()) {
        lua_pushnil(L);
        return 1;
    }
    push_sv(L, vec[static_cast<std::size_t>(idx)].text);
    return 1;
}

int lc_getStringId(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const std::string_view target = to_sv(L, 1);
    const bool quick = lua_toboolean(L, 2) != 0;
    const auto& vec =
        quick ? mgr.cfg_.bundle->quick_string_entries : mgr.cfg_.bundle->string_entries;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (vec[i].id == target) {
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int lc_getCurTemplateNo(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    lua_pushinteger(L, mgr.ctx_->world.current_mission_template_no);
    return 1;
}

int lc_getCurTemplateId(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const std::int32_t idx = mgr.ctx_->world.current_mission_template_no;
    if (idx >= 0 && static_cast<std::size_t>(idx) < mgr.ctx_->world.mission_templates.size()) {
        push_sv(L, mgr.ctx_->world.mission_templates[static_cast<std::size_t>(idx)].id);
        return 1;
    }
    if (mgr.cfg_.bundle != nullptr && idx >= 0 &&
        static_cast<std::size_t>(idx) < mgr.cfg_.bundle->mission_templates.size()) {
        push_sv(L, mgr.cfg_.bundle->mission_templates[static_cast<std::size_t>(idx)].id);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int lc_getNumTemplates(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    std::size_t count = 0;
    if (mgr.cfg_.bundle != nullptr && !mgr.cfg_.bundle->mission_templates.empty()) {
        count = mgr.cfg_.bundle->mission_templates.size();
    } else if (mgr.ctx_ != nullptr) {
        count = mgr.ctx_->world.mission_templates.size();
    }
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

int lc_getTemplateId(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const auto idx = static_cast<std::int32_t>(check_i64(L, 1));
    if (mgr.cfg_.bundle != nullptr && idx >= 0 &&
        static_cast<std::size_t>(idx) < mgr.cfg_.bundle->mission_templates.size()) {
        push_sv(L, mgr.cfg_.bundle->mission_templates[static_cast<std::size_t>(idx)].id);
        return 1;
    }
    if (mgr.ctx_ != nullptr && idx >= 0 &&
        static_cast<std::size_t>(idx) < mgr.ctx_->world.mission_templates.size()) {
        push_sv(L, mgr.ctx_->world.mission_templates[static_cast<std::size_t>(idx)].id);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int lc_addTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const auto mst_no = static_cast<std::int32_t>(check_i64(L, 1));
    game::TriggerManager* tm = resolve_template_triggers(mgr, mst_no);
    if (tm == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    game::Trigger trig{};
    trig.checkInterval = static_cast<float>(luaL_checknumber(L, 2));
    trig.delayInterval = static_cast<float>(luaL_checknumber(L, 3));
    trig.rearmInterval = static_cast<float>(luaL_checknumber(L, 4));
    if (!extract_trigger_block(L, 5, trig.conditions)) {
        return luaL_argerror(L, 5, "expected condition/callback function");
    }
    if (lua_gettop(L) >= 6 && !lua_isnil(L, 6)) {
        if (!extract_trigger_block(L, 6, trig.consequences)) {
            mgr.unref_all_block_callbacks(trig.conditions);
            return luaL_argerror(L, 6, "expected consequence function");
        }
    }
    lua_pushinteger(L, static_cast<lua_Integer>(
                           append_or_defer(mgr, tm->triggers, std::move(trig))));
    return 1;
}

int lc_removeTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    game::TriggerManager* tm = resolve_template_triggers(mgr, static_cast<std::int32_t>(check_i64(L, 1)));
    const auto trig_no = static_cast<std::int32_t>(check_i64(L, 2));
    WseLuaManagerImpl* pmgr = &mgr;
    const bool removed =
        tm != nullptr &&
        remove_at_or_defer(mgr, tm->triggers, trig_no, [pmgr](game::Trigger& t) {
            pmgr->unref_all_block_callbacks(t.conditions);
            pmgr->unref_all_block_callbacks(t.consequences);
        });
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

int lc_getNumTriggers(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    game::TriggerManager* tm = resolve_template_triggers(mgr, static_cast<std::int32_t>(check_i64(L, 1)));
    lua_pushinteger(L, tm ? static_cast<lua_Integer>(mgr.virtual_size(tm->triggers)) : 0);
    return 1;
}

template <class Container>
int add_simple_trigger_impl(WseLuaManagerImpl& mgr, lua_State* L, Container& vec,
                            std::int32_t target_idx) {
    if (target_idx < 0 || static_cast<std::size_t>(target_idx) >= vec.size()) {
        lua_pushinteger(L, -1);
        return 1;
    }
    game::SimpleTrigger st{};
    st.interval = static_cast<float>(luaL_checknumber(L, 2));
    if (!extract_trigger_block(L, 3, st.block)) {
        return luaL_argerror(L, 3, "expected trigger function");
    }
    auto& trigs = vec[static_cast<std::size_t>(target_idx)].triggers.triggers;
    lua_pushinteger(L, static_cast<lua_Integer>(append_or_defer(mgr, trigs, std::move(st))));
    return 1;
}

template <class Container>
int remove_simple_trigger_impl(WseLuaManagerImpl& mgr, lua_State* L, Container& vec,
                               std::int32_t target_idx, std::int32_t trig_idx) {
    bool removed = false;
    if (target_idx >= 0 && static_cast<std::size_t>(target_idx) < vec.size()) {
        auto& trigs = vec[static_cast<std::size_t>(target_idx)].triggers.triggers;
        WseLuaManagerImpl* pmgr = &mgr;
        removed = remove_at_or_defer(mgr, trigs, trig_idx, [pmgr](game::SimpleTrigger& t) {
            pmgr->unref_all_block_callbacks(t.block);
        });
    }
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

int lc_addItemTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    return add_simple_trigger_impl(mgr, L, mgr.cfg_.bundle->item_triggers,
                                   static_cast<std::int32_t>(check_i64(L, 1)));
}

int lc_removeItemTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    return remove_simple_trigger_impl(mgr, L, mgr.cfg_.bundle->item_triggers,
                                      static_cast<std::int32_t>(check_i64(L, 1)),
                                      static_cast<std::int32_t>(check_i64(L, 2)));
}

int lc_getNumItemTriggers(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const auto idx = static_cast<std::int32_t>(check_i64(L, 1));
    std::size_t n = 0;
    if (mgr.cfg_.bundle != nullptr && idx >= 0 &&
        static_cast<std::size_t>(idx) < mgr.cfg_.bundle->item_triggers.size()) {
        n = mgr.virtual_size(
            mgr.cfg_.bundle->item_triggers[static_cast<std::size_t>(idx)].triggers.triggers);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    return 1;
}

int lc_addScenePropTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    return add_simple_trigger_impl(mgr, L, mgr.cfg_.bundle->scene_props,
                                   static_cast<std::int32_t>(check_i64(L, 1)));
}

int lc_removeScenePropTrigger(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    return remove_simple_trigger_impl(mgr, L, mgr.cfg_.bundle->scene_props,
                                      static_cast<std::int32_t>(check_i64(L, 1)),
                                      static_cast<std::int32_t>(check_i64(L, 2)));
}

int lc_getNumScenePropTriggers(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const auto idx = static_cast<std::int32_t>(check_i64(L, 1));
    std::size_t n = 0;
    if (mgr.cfg_.bundle != nullptr && idx >= 0 &&
        static_cast<std::size_t>(idx) < mgr.cfg_.bundle->scene_props.size()) {
        n = mgr.virtual_size(
            mgr.cfg_.bundle->scene_props[static_cast<std::size_t>(idx)].triggers.triggers);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    return 1;
}

int lc_addPrsnt(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    game::LoadedPresentation prsnt{};

    lua_getfield(L, 1, "id");
    if (lua_isstring(L, -1)) prsnt.id = to_sv(L, -1);
    c_lua_pop(L, 1);

    lua_getfield(L, 1, "flags");
    prsnt.flags = read_flags_arg(L, -1);
    c_lua_pop(L, 1);

    lua_getfield(L, 1, "mesh");
    if (lua_isnumber(L, -1)) {
        prsnt.mesh_no = static_cast<std::int32_t>(check_i64(L, -1));
    } else if (lua_isstring(L, -1)) {
        const std::string_view mname = to_sv(L, -1);
        for (std::size_t i = 0; i < mgr.cfg_.bundle->meshes.size(); ++i) {
            if (mgr.cfg_.bundle->meshes[i].id == mname) {
                prsnt.mesh_no = static_cast<std::int32_t>(i);
                break;
            }
        }
    }
    c_lua_pop(L, 1);

    lua_getfield(L, 1, "triggers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isnumber(L, -2) && lua_isfunction(L, -1)) {
                game::SimpleTrigger st{};
                st.interval = static_cast<float>(lua_tonumber(L, -2));
                (void)extract_trigger_block(L, -1, st.block);
                prsnt.triggers.triggers.push_back(std::move(st));
            }
            c_lua_pop(L, 1);
        }
    }
    c_lua_pop(L, 1);

    if (mgr.edits_deferred()) {
        mgr.pending_presentation_ids().push_back(prsnt.id);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(append_or_defer(
                           mgr, mgr.cfg_.bundle->presentations, std::move(prsnt))));
    return 1;
}

int lc_removePrsnt(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto& vec = mgr.cfg_.bundle->presentations;
    WseLuaManagerImpl* pmgr = &mgr;
    const auto unref = [pmgr](game::LoadedPresentation& p) {
        for (auto& st : p.triggers.triggers) pmgr->unref_all_block_callbacks(st.block);
    };
    const auto find_id = [](const std::vector<game::LoadedPresentation>& v,
                            std::string_view id) -> std::int32_t {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].id == id) return static_cast<std::int32_t>(i);
        }
        return -1;
    };
    // While edits are queued, lookups go through the id list as it will be
    // once they are applied, so a presentation added earlier in the same
    // trigger can be removed and one already removed is not found again.
    const bool deferred = mgr.edits_deferred();
    bool removed = false;
    if (lua_isnumber(L, 1)) {
        const auto idx = static_cast<std::int32_t>(check_i64(L, 1));
        removed = remove_at_or_defer(mgr, vec, idx, unref);
        if (removed && deferred) {
            auto& ids = mgr.pending_presentation_ids();
            ids.erase(ids.begin() + idx);
        }
    } else if (lua_isstring(L, 1)) {
        // An id is resolved when the edit is applied, so removals queued ahead
        // of it cannot shift it onto a different presentation.
        std::string id(to_sv(L, 1));
        if (deferred) {
            auto& ids = mgr.pending_presentation_ids();
            const auto it = std::find(ids.begin(), ids.end(), id);
            removed = it != ids.end();
            if (removed) ids.erase(it);
        } else {
            removed = find_id(vec, id) >= 0;
        }
        if (removed) {
            auto* target = &vec;
            mgr.run_or_defer_edit(target, -1, [target, id = std::move(id), unref, find_id] {
                (void)remove_trigger_at(*target, find_id(*target, id), unref);
            });
        }
    }
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

int lc_addPsys(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushinteger(L, -1);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    game::LoadedParticleSystem psys{};

    lua_getfield(L, 1, "id");
    if (lua_isstring(L, -1)) psys.id = to_sv(L, -1);
    c_lua_pop(L, 1);

    lua_getfield(L, 1, "flags");
    psys.flags = read_flags_arg(L, -1);
    c_lua_pop(L, 1);

    lua_getfield(L, 1, "mesh_name");
    if (lua_isstring(L, -1)) psys.mesh_name = to_sv(L, -1);
    c_lua_pop(L, 1);

    if (psys.mesh_name.empty()) {
        lua_getfield(L, 1, "mesh");
        if (lua_isstring(L, -1)) psys.mesh_name = to_sv(L, -1);
        c_lua_pop(L, 1);
    }

    lua_getfield(L, 1, "params");
    if (lua_istable(L, -1)) {
        const size_t n = std::min<size_t>(lua_objlen(L, -1), psys.params.size());
        for (size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, static_cast<int>(i));
            if (lua_isnumber(L, -1)) {
                psys.params[i - 1] = static_cast<float>(lua_tonumber(L, -1));
            }
            c_lua_pop(L, 1);
        }
    }
    c_lua_pop(L, 1);

    const auto idx = static_cast<lua_Integer>(mgr.cfg_.bundle->particle_systems.size());
    mgr.cfg_.bundle->particle_systems.push_back(std::move(psys));
    if (mgr.ctx_ != nullptr) {
        mgr.ctx_->world.num_particle_systems =
            static_cast<std::int32_t>(mgr.cfg_.bundle->particle_systems.size());
    }
    lua_pushinteger(L, idx);
    return 1;
}

int lc_removePsys(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.bundle == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto& vec = mgr.cfg_.bundle->particle_systems;
    std::int32_t idx = -1;
    if (lua_isnumber(L, 1)) {
        idx = static_cast<std::int32_t>(check_i64(L, 1));
    } else if (lua_isstring(L, 1)) {
        const std::string_view id = to_sv(L, 1);
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].id == id) {
                idx = static_cast<std::int32_t>(i);
                break;
            }
        }
    }
    bool removed = false;
    if (idx >= 0 && static_cast<std::size_t>(idx) < vec.size()) {
        vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(idx));
        removed = true;
        if (mgr.ctx_ != nullptr) {
            mgr.ctx_->world.num_particle_systems = static_cast<std::int32_t>(vec.size());
        }
    }
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

int lc_getMeshId(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view name = to_sv(L, 1);
    if (mgr.cfg_.bundle != nullptr) {
        for (std::size_t i = 0; i < mgr.cfg_.bundle->meshes.size(); ++i) {
            if (mgr.cfg_.bundle->meshes[i].id == name) {
                lua_pushinteger(L, static_cast<lua_Integer>(i));
                return 1;
            }
        }
    }
    if (mgr.ctx_ != nullptr) {
        for (std::size_t i = 0; i < mgr.ctx_->world.mesh_names.size(); ++i) {
            if (mgr.ctx_->world.mesh_names[i] == name) {
                lua_pushinteger(L, static_cast<lua_Integer>(i));
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

int lc_getNumMeshes(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    std::size_t n = 0;
    if (mgr.cfg_.bundle != nullptr && !mgr.cfg_.bundle->meshes.empty()) {
        n = mgr.cfg_.bundle->meshes.size();
    } else if (mgr.ctx_ != nullptr) {
        n = mgr.ctx_->world.mesh_names.size();
    }
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    return 1;
}

// ---- Iterators ----
int parties_next(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    auto idx = static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const auto& world = mgr.ctx_->world;
    while (idx < world.parties.size()) {
        const auto cur = static_cast<std::int32_t>(idx++);
        if (world.is_valid_party(cur)) {
            lua_pushinteger(L, static_cast<lua_Integer>(idx));
            lua_replace(L, lua_upvalueindex(1));
            lua_pushinteger(L, cur);
            return 1;
        }
    }
    return 0;
}

int lc_partiesIt(lua_State* L) {
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, guarded<parties_next>, 1);
    return 1;
}

int agents_next(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    auto idx = static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const bool has_sphere = lua_toboolean(L, lua_upvalueindex(2)) != 0;
    const float cx = static_cast<float>(lua_tonumber(L, lua_upvalueindex(3)));
    const float cy = static_cast<float>(lua_tonumber(L, lua_upvalueindex(4)));
    const float cz = static_cast<float>(lua_tonumber(L, lua_upvalueindex(5)));
    const float radius_sq = static_cast<float>(lua_tonumber(L, lua_upvalueindex(6)));

    const auto& world = mgr.ctx_->world;
    while (idx < world.agents.size()) {
        const auto cur = static_cast<std::int32_t>(idx++);
        if (!world.is_valid_agent(cur)) continue;
        if (has_sphere) {
            const auto& p = world.agents[static_cast<std::size_t>(cur)].position.o;
            const float dx = p.x - cx;
            const float dy = p.y - cy;
            const float dz = p.z - cz;
            if (dx * dx + dy * dy + dz * dz > radius_sq) continue;
        }
        lua_pushinteger(L, static_cast<lua_Integer>(idx));
        lua_replace(L, lua_upvalueindex(1));
        lua_pushinteger(L, cur);
        return 1;
    }
    return 0;
}

int lc_agentsIt(lua_State* L) {
    bool has_sphere = false;
    double cx = 0.0, cy = 0.0, cz = 0.0, radius_sq = 0.0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 1) && !lua_isnil(L, 2)) {
        core::WseTransform center{};
        if (!read_pos(L, 1, center)) {
            return luaL_argerror(L, 1, "expected game.pos table");
        }
        const double radius = luaL_checknumber(L, 2);
        has_sphere = true;
        cx = center.o.x;
        cy = center.o.y;
        cz = center.o.z;
        radius_sq = radius >= 0.0 ? radius * radius : -1.0;
    }
    lua_pushinteger(L, 0);
    lua_pushboolean(L, has_sphere ? 1 : 0);
    lua_pushnumber(L, cx);
    lua_pushnumber(L, cy);
    lua_pushnumber(L, cz);
    lua_pushnumber(L, radius_sq);
    lua_pushcclosure(L, guarded<agents_next>, 6);
    return 1;
}

int prop_inst_next(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    auto idx = static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const auto sub_kind = static_cast<std::int32_t>(lua_tointeger(L, lua_upvalueindex(2)));
    const auto meta_type = static_cast<std::int32_t>(lua_tointeger(L, lua_upvalueindex(3)));

    const auto& world = mgr.ctx_->world;
    while (idx < world.mission_objects.size()) {
        const auto cur = static_cast<std::int32_t>(idx++);
        if (!world.is_valid_mission_object(cur)) continue;
        const auto& mo = world.mission_objects[static_cast<std::size_t>(cur)];
        if (meta_type == 0 && mo.is_item) continue;
        if (meta_type > 0 && !mo.is_item) continue;
        if (sub_kind >= 0 && mo.scene_prop_no != sub_kind) continue;

        lua_pushinteger(L, static_cast<lua_Integer>(idx));
        lua_replace(L, lua_upvalueindex(1));
        lua_pushinteger(L, cur);
        return 1;
    }
    return 0;
}

int lc_propInstIt(lua_State* L) {
    const std::int32_t sub_kind =
        (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) ? static_cast<std::int32_t>(check_i64(L, 1)) : -1;
    const std::int32_t meta_type =
        (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) ? static_cast<std::int32_t>(check_i64(L, 2)) : -1;
    lua_pushinteger(L, 0);
    lua_pushinteger(L, sub_kind);
    lua_pushinteger(L, meta_type);
    lua_pushcclosure(L, guarded<prop_inst_next>, 3);
    return 1;
}

int players_next(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.ctx_ == nullptr) return 0;
    auto idx = static_cast<std::size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const bool skip_server = lua_toboolean(L, lua_upvalueindex(2)) != 0;

    const auto& world = mgr.ctx_->world;
    while (idx < world.players.size()) {
        const auto cur = static_cast<std::int32_t>(idx++);
        if (skip_server && cur == 0) continue;
        if (!world.is_valid_player(cur)) continue;
        lua_pushinteger(L, static_cast<lua_Integer>(idx));
        lua_replace(L, lua_upvalueindex(1));
        lua_pushinteger(L, cur);
        return 1;
    }
    return 0;
}

int lc_playersIt(lua_State* L) {
    const bool skip_server = lua_toboolean(L, 1) != 0;
    lua_pushinteger(L, 0);
    lua_pushboolean(L, skip_server ? 1 : 0);
    lua_pushcclosure(L, guarded<players_next>, 2);
    return 1;
}

// ---- Hooks ----
int lc_hookOperation(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::int32_t opcode = resolve_opcode_arg(mgr, L, 1);
    if (opcode < 0 || static_cast<std::size_t>(opcode) >= vm::kMaxLuaOperationSlots) {
        return luaL_argerror(L, 1, "unknown or out-of-range operation");
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int& slot = mgr.op_hook_refs_[static_cast<std::size_t>(opcode)];
    if (slot != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot);
        slot = LUA_NOREF;
    }
    lua_pushvalue(L, 2);
    slot = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

int lc_unhookOperation(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::int32_t opcode = resolve_opcode_arg(mgr, L, 1);
    if (opcode < 0 || static_cast<std::size_t>(opcode) >= vm::kMaxLuaOperationSlots) {
        return 0;
    }
    int& slot = mgr.op_hook_refs_[static_cast<std::size_t>(opcode)];
    if (slot != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot);
        slot = LUA_NOREF;
    }
    return 0;
}

int lc_hookScript(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::int32_t script_no = resolve_script_index(mgr, L, 1);
    if (script_no < 0 || mgr.ctx_ == nullptr || mgr.ctx_->scripts == nullptr) {
        return luaL_argerror(L, 1, "unknown script");
    }
    auto& block = mgr.ctx_->scripts->scripts[static_cast<std::size_t>(script_no)].block;
    if (block.luaHookRef != vm::kNoLuaHook) {
        luaL_unref(L, LUA_REGISTRYINDEX, block.luaHookRef);
        block.luaHookRef = vm::kNoLuaHook;
    }
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        block.luaHookRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

int lc_fail(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    mgr.set_script_fail();
    return 0;
}

// ---- _G callbacks (7 total) ----
int lc_lsdir(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, false, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushnil(L);
        lua_pushliteral(L, "access denied by lua sandbox");
        return 2;
    }
    std::vector<platform::VfsEntry> entries;
    if (!mgr.cfg_.vfs->list_dir(vfs_path, entries)) {
        lua_pushnil(L);
        lua_pushliteral(L, "not a directory");
        return 2;
    }
    lua_newtable(L);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        push_sv(L, entries[i].name);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int lc_mkdir(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, true, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, mgr.cfg_.vfs->make_dir(vfs_path) ? 1 : 0);
    return 1;
}

int lc_rmdir(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, true, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, mgr.cfg_.vfs->remove_dir(vfs_path) ? 1 : 0);
    return 1;
}

int lc_printStack(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    lua_getglobal(L, "debug");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "traceback");
        if (lua_isfunction(L, -1)) {
            lua_pushliteral(L, "Lua stack:");
            lua_pushinteger(L, 2);
            if (lua_pcall(L, 2, 1, 0) == 0 && lua_isstring(L, -1)) {
                mgr.record_print(std::string(to_sv(L, -1)));
            }
            c_lua_pop(L, 2);
            return 0;
        }
        c_lua_pop(L, 1);
    }
    c_lua_pop(L, 1);
    return 0;
}

int lc_print(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const int n = lua_gettop(L);
    std::string msg;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) msg.push_back('\t');
        const std::string_view part = to_sv(L, i);
        if (part.data() != nullptr) {
            msg.append(part);
        } else {
            msg.append(luaL_typename(L, i));
        }
    }
    mgr.record_print(std::move(msg));
    return 0;
}

int lc_log_rgl(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view msg = to_sv(L, 1);
    mgr.log_rgl_direct(msg);
    return 0;
}

int lc_getTime(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const double secs = mgr.cfg_.clock ? mgr.cfg_.clock->seconds() : 0.0;
    lua_pushnumber(L, static_cast<lua_Number>(secs));
    return 1;
}

} // namespace

bool number_to_i64_noexcept(lua_Number d, std::int64_t& out) noexcept {
    if (!std::isfinite(d)) return false;
    const double t = std::trunc(d);
    if (t >= -9223372036854775808.0 && t < 9223372036854775808.0) {
        out = static_cast<std::int64_t>(t);
        return true;
    }
    if (t >= 9223372036854775808.0 && t < 18446744073709551616.0) {
        out = static_cast<std::int64_t>(static_cast<std::uint64_t>(t));
        return true;
    }
    return false;
}

std::int64_t lua_number_to_i64(lua_Number d, lua_State* L, int arg) {
    std::int64_t out = 0;
    if (number_to_i64_noexcept(d, out)) return out;
    return luaL_argerror(L, arg, std::isfinite(d) ? "number out of int64 range"
                                                  : "number is non-finite");
}

bool try_i64(lua_State* L, int idx, std::int64_t& out) {
    if (lua_isboolean(L, idx)) {
        out = lua_toboolean(L, idx) ? 1 : 0;
        return true;
    }
    // lua_isnumber also accepts numeric strings; lua_tonumber does not raise.
    if (!lua_isnumber(L, idx)) return false;
    return number_to_i64_noexcept(lua_tonumber(L, idx), out);
}

std::int64_t check_i64(lua_State* L, int idx) {
    if (lua_isboolean(L, idx)) return lua_toboolean(L, idx) ? 1 : 0;
    return lua_number_to_i64(luaL_checknumber(L, idx), L, idx);
}

std::int64_t opt_i64(lua_State* L, int idx, std::int64_t def) {
    if (lua_isnoneornil(L, idx)) return def;
    return check_i64(L, idx);
}

bool is_pos(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return false;
    LuaStackGuard guard(L);
    if (!lua_getmetatable(L, idx)) return false;
    lua_getglobal(L, "game");
    if (!lua_istable(L, -1)) return false;
    lua_getfield(L, -1, "pos");
    if (!lua_istable(L, -1)) return false;
    lua_getfield(L, -1, "mt");
    return lua_rawequal(L, -4, -1) != 0;
}

void push_pos(lua_State* L, const core::WseTransform& pos) {
    lua_newtable(L);
    push_vec3_table(L, pos.o);
    lua_setfield(L, -2, "o");

    lua_newtable(L);
    push_vec3_table(L, pos.s);
    lua_setfield(L, -2, "s");
    push_vec3_table(L, pos.f);
    lua_setfield(L, -2, "f");
    push_vec3_table(L, pos.u);
    lua_setfield(L, -2, "u");
    lua_getglobal(L, "game");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "rotation");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "mt");
            if (lua_istable(L, -1)) {
                lua_setmetatable(L, -4);
            } else {
                c_lua_pop(L, 1);
            }
        }
        c_lua_pop(L, 1);
    }
    c_lua_pop(L, 1);
    lua_setfield(L, -2, "rot");

    lua_getglobal(L, "game");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "pos");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "mt");
            if (lua_istable(L, -1)) {
                lua_setmetatable(L, -4);
            } else {
                c_lua_pop(L, 1);
            }
        }
        c_lua_pop(L, 1);
    }
    c_lua_pop(L, 1);
}

bool read_pos(lua_State* L, int idx, core::WseTransform& out) {
    if (!lua_istable(L, idx)) return false;
    LuaStackGuard guard(L);
    const int abs_idx = idx < 0 ? (lua_gettop(L) + idx + 1) : idx;
    out = core::WseTransform::identity();
    (void)read_table_vec3(L, abs_idx, "o", out.o, 0.0f, 0.0f, 0.0f, 1.0f);
    lua_getfield(L, abs_idx, "rot");
    if (lua_istable(L, -1)) {
        const int rot_idx = lua_gettop(L);
        (void)read_table_vec3(L, rot_idx, "s", out.s, 1.0f, 0.0f, 0.0f);
        (void)read_table_vec3(L, rot_idx, "f", out.f, 0.0f, 1.0f, 0.0f);
        (void)read_table_vec3(L, rot_idx, "u", out.u, 0.0f, 0.0f, 1.0f);
    }
    c_lua_pop(L, 1);
    return true;
}

void WseLuaManagerImpl::unref_all_block_callbacks(vm::StatementBlock& block) {
    if (L_ == nullptr) return;
    for (auto& st : block.statements) {
        if (st.opcode == static_cast<std::uint32_t>(vm::WseOpcode::lua_triggerCallback) &&
            st.numOperands >= 1) {
            const int ref = static_cast<int>(st.operands[0]);
            if (ref != LUA_NOREF && ref != LUA_REFNIL) {
                luaL_unref(L_, LUA_REGISTRYINDEX, ref);
                st.operands[0] = LUA_NOREF;
            }
        }
    }
}

void WseLuaManagerImpl::register_callbacks() {
    LuaStackGuard guard(L_);

    static const luaL_Reg kGameCallbacks[] = {
        {"execOperation", guarded<lc_execOperation>},
        {"getOperationFlags", guarded<lc_getOperationFlags>},
        {"getReg", guarded<lc_getReg>},
        {"setReg", guarded<lc_setReg>},
        {"getGvar", guarded<lc_getGvar>},
        {"setGvar", guarded<lc_setGvar>},
        {"getScriptNo", guarded<lc_getScriptNo>},
        {"getString", guarded<lc_getString>},
        {"getStringId", guarded<lc_getStringId>},
        {"getCurTemplateNo", guarded<lc_getCurTemplateNo>},
        {"getCurTemplateId", guarded<lc_getCurTemplateId>},
        {"getNumTemplates", guarded<lc_getNumTemplates>},
        {"getTemplateId", guarded<lc_getTemplateId>},
        {"addTrigger", guarded<lc_addTrigger>},
        {"removeTrigger", guarded<lc_removeTrigger>},
        {"getNumTriggers", guarded<lc_getNumTriggers>},
        {"addItemTrigger", guarded<lc_addItemTrigger>},
        {"removeItemTrigger", guarded<lc_removeItemTrigger>},
        {"getNumItemTriggers", guarded<lc_getNumItemTriggers>},
        {"addScenePropTrigger", guarded<lc_addScenePropTrigger>},
        {"removeScenePropTrigger", guarded<lc_removeScenePropTrigger>},
        {"getNumScenePropTriggers", guarded<lc_getNumScenePropTriggers>},
        {"addPrsnt", guarded<lc_addPrsnt>},
        {"removePrsnt", guarded<lc_removePrsnt>},
        {"addPsys", guarded<lc_addPsys>},
        {"removePsys", guarded<lc_removePsys>},
        {"getMeshId", guarded<lc_getMeshId>},
        {"getNumMeshes", guarded<lc_getNumMeshes>},
        {"partiesIt", guarded<lc_partiesIt>},
        {"agentsIt", guarded<lc_agentsIt>},
        {"propInstIt", guarded<lc_propInstIt>},
        {"playersIt", guarded<lc_playersIt>},
        {"hookOperation", guarded<lc_hookOperation>},
        {"unhookOperation", guarded<lc_unhookOperation>},
        {"hookScript", guarded<lc_hookScript>},
        {"fail", guarded<lc_fail>},
        {nullptr, nullptr},
    };
    luaL_register(L_, "game", kGameCallbacks);
    if (cfg_.disable_game_const_warnings) {
        lua_pushboolean(L_, 1);
        lua_setfield(L_, -2, "_disable_const_warnings");
    }
    c_lua_pop(L_, 1);

    static const luaL_Reg kGlobalCallbacks[] = {
        {"lsdir", guarded<lc_lsdir>},   {"mkdir", guarded<lc_mkdir>},   {"rmdir", guarded<lc_rmdir>},
        {"printStack", guarded<lc_printStack>}, {"_print", guarded<lc_print>},
        {"log_rgl", guarded<lc_log_rgl>}, {"getTime", guarded<lc_getTime>}, {nullptr, nullptr},
    };
    lua_pushvalue(L_, LUA_GLOBALSINDEX);
    luaL_register(L_, nullptr, kGlobalCallbacks);
    c_lua_pop(L_, 1);
}

bool WseLuaManagerImpl::has_operation_hook(std::uint32_t opcode) const noexcept {
    if (!ready_ || L_ == nullptr || opcode >= vm::kMaxLuaOperationSlots) return false;
    return op_hook_refs_[opcode] != LUA_NOREF;
}

vm::WseLuaOpHookResult WseLuaManagerImpl::on_operation_execute(
    std::uint32_t opcode,
    std::span<const std::int64_t> parsed_operands,
    std::span<const std::uint8_t> operand_tags,
    vm::VmContext& ctx) {
    vm::WseLuaOpHookResult out{};
    if (!has_operation_hook(opcode)) return out;

    const int ref = op_hook_refs_[opcode];
    LuaStackGuard guard(L_);
    const int base_top = lua_gettop(L_);
    if (!reserve_stack(static_cast<int>(parsed_operands.size()) + 4)) return out;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L_, -1)) return out;

    const vm::WseStringBanks banks{ctx.stringRegisters, ctx.globalStrings, ctx.localStrings};
    for (std::size_t i = 0; i < parsed_operands.size(); ++i) {
        const std::uint8_t tag = i < operand_tags.size() ? operand_tags[i] : 0U;
        if (tag == 3U || tag == 0x16U) {
            const auto raw = vm::wse_resolve_string(
                static_cast<std::int32_t>(parsed_operands[i]), tag, banks);
            const std::u16string expanded = vm::wse_parse_string(raw, ctx.registers, banks);
            push_sv(L_, core::wse_utf16_to_utf8(expanded));
        } else {
            lua_pushnumber(L_, static_cast<lua_Number>(parsed_operands[i]));
        }
    }

    if (!pcall_with_traceback(static_cast<int>(parsed_operands.size()), LUA_MULTRET,
                              "hookOperation")) {
        return out;
    }

    const int nret = lua_gettop(L_) - base_top;
    if (nret <= 0) return out;

    out.execute = lua_toboolean(L_, base_top + 1) != 0;
    std::uint16_t flags = 0;
    if (const auto it = op_flags_by_code_.find(static_cast<std::int32_t>(opcode));
        it != op_flags_by_code_.end()) {
        flags = it->second;
    }
    const bool is_lhs = (flags & vm::kLuaOpFlagLhs) != 0;
    const bool is_cf = (flags & vm::kLuaOpFlagCf) != 0;

    // The pcall has returned, so nothing here may raise a Lua error: a hook
    // that returns a non-number (or inf/NaN) where the lhs value belongs is
    // reported and the lhs store is skipped.
    const auto read_lhs = [&](int idx) {
        if (try_i64(L_, idx, out.lhs_value)) {
            out.has_lhs = true;
        } else {
            record_error("hookOperation: lhs return value is not an integer-convertible number");
        }
    };
    if (nret == 2) {
        if (is_cf && !is_lhs) {
            out.has_cf = true;
            out.cf_result = lua_toboolean(L_, base_top + 2) != 0;
        } else if (is_lhs && !is_cf) {
            read_lhs(base_top + 2);
        } else if (lua_isboolean(L_, base_top + 2)) {
            out.has_cf = true;
            out.cf_result = lua_toboolean(L_, base_top + 2) != 0;
        } else if (lua_isnumber(L_, base_top + 2)) {
            read_lhs(base_top + 2);
        }
    } else if (nret >= 3) {
        out.has_cf = true;
        out.cf_result = lua_toboolean(L_, base_top + 2) != 0;
        read_lhs(base_top + 3);
    }
    return out;
}

vm::WseLuaScriptHookResult WseLuaManagerImpl::on_script_execute(
    std::int32_t hook_ref,
    std::span<const std::int64_t> params,
    vm::VmContext& /*ctx*/) {
    vm::WseLuaScriptHookResult out{};
    if (!ready_ || L_ == nullptr || hook_ref == vm::kNoLuaHook) return out;

    LuaStackGuard guard(L_);
    const int base_top = lua_gettop(L_);
    if (!reserve_stack(static_cast<int>(params.size()) + 4)) return out;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, hook_ref);
    if (!lua_isfunction(L_, -1)) return out;

    for (std::int64_t p : params) {
        lua_pushnumber(L_, static_cast<lua_Number>(p));
    }

    push_script_fail(false);
    const bool ok = pcall_with_traceback(static_cast<int>(params.size()), LUA_MULTRET,
                                         "hookScript");
    const bool failed = current_script_failed();
    pop_script_fail();

    if (failed) {
        out.execute = false;
        out.cf_result = false;
        return out;
    }
    if (!ok) return out;

    const int nret = lua_gettop(L_) - base_top;
    if (nret >= 1 && lua_isboolean(L_, base_top + 1)) {
        out.execute = lua_toboolean(L_, base_top + 1) != 0;
        if (!out.execute) {
            if (nret >= 2 && lua_isboolean(L_, base_top + 2)) {
                out.cf_result = lua_toboolean(L_, base_top + 2) != 0;
            } else {
                out.cf_result = true;
            }
            return out;
        }
        if (nret >= 2) {
            out.replace_params = true;
            for (int i = 2; i <= nret; ++i) {
                std::int64_t value = 0;
                if (try_i64(L_, base_top + i, value)) {
                    out.replacement_params.push_back(value);
                }
            }
        }
    }
    return out;
}

} // namespace wse::lua::detail
