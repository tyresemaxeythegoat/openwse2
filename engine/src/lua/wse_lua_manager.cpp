#include "lua_bitop.hpp"
#include "lua_globals_script.hpp"
#include "wse_lua_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace wse::lua {
namespace detail {
namespace {

std::string normalize_slash_dir(std::string_view p) {
    std::string out(p);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.back() == '/') out.pop_back();
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

std::string join_vfs_dir(std::string_view base, std::string_view sub) {
    if (base.empty()) return std::string(sub);
    if (sub.empty()) return std::string(base);
    std::string out(base);
    out.push_back('/');
    out.append(sub);
    return out;
}

// push_pos builds a position table three levels deep and looks up metatables.
constexpr int kPushPosStackSlots = 8;

int lua_traceback_handler(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        return 1;
    }
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    if (!lua_istable(L, -1)) {
        c_lua_pop(L, 1);
        return 1;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        c_lua_pop(L, 2);
        return 1;
    }
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

bool push_dotted_callable(lua_State* L, std::string_view dotted_name) {
    if (dotted_name.empty()) return false;
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    std::size_t pos = 0;
    while (pos <= dotted_name.size()) {
        const std::size_t dot = dotted_name.find('.', pos);
        const std::string_view seg =
            dot == std::string_view::npos ? dotted_name.substr(pos)
                                          : dotted_name.substr(pos, dot - pos);
        if (seg.empty() || !lua_istable(L, -1)) {
            c_lua_pop(L, 1);
            return false;
        }
        push_sv(L, seg);
        lua_gettable(L, -2);
        lua_remove(L, -2);
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (!lua_isfunction(L, -1)) {
        c_lua_pop(L, 1);
        return false;
    }
    return true;
}

template <class PushArgsFn>
bool call_game_event(WseLuaManagerImpl& mgr, const char* name, int nargs,
                     int nresults, PushArgsFn&& push_args) {
    if (!mgr.is_ready() || mgr.L_ == nullptr) return false;
    lua_State* L = mgr.L_;
    if (!mgr.reserve_stack(nargs + 4)) return false;
    lua_getglobal(L, "game");
    if (!lua_istable(L, -1)) {
        c_lua_pop(L, 1);
        return false;
    }
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        c_lua_pop(L, 1);
        return false;
    }
    push_args(L);
    return mgr.pcall_with_traceback(nargs, nresults, name);
}

} // namespace

WseLuaManagerImpl::WseLuaManagerImpl() {
    op_hook_refs_.fill(LUA_NOREF);
}

WseLuaManagerImpl::~WseLuaManagerImpl() {
    close_lua_state_only();
}

WseLuaManagerImpl& WseLuaManagerImpl::from_lua(lua_State* L) {
    lua_pushlstring(L, kMgrRegistryKey.data(), kMgrRegistryKey.size());
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* ptr = static_cast<WseLuaManagerImpl*>(lua_touserdata(L, -1));
    c_lua_pop(L, 1);
    return *ptr;
}

void raise_cpp_exception(lua_State* L, const char* what) {
    luaL_error(L, "C++ exception in Lua callback: %s", what);
    std::abort(); // luaL_error never returns
}

bool WseLuaManagerImpl::reserve_stack(int slots) {
    // The VM-facing entry points run at the base of the Lua stack, where only
    // LUA_MINSTACK slots are guaranteed; pushing past them corrupts the heap.
    if (lua_checkstack(L_, slots) != 0) return true;
    record_error("Lua stack overflow");
    return false;
}

void WseLuaManagerImpl::record_print(std::string msg) {
    print_history_.push_back(msg);
    log_rgl_direct(msg);
}

void WseLuaManagerImpl::log_rgl_direct(std::string_view msg) {
    if (cfg_.log == nullptr) return;
    const bool prev = in_rgl_log_;
    in_rgl_log_ = true;
    cfg_.log->info(msg);
    in_rgl_log_ = prev;
}

void WseLuaManagerImpl::record_error(std::string_view msg, bool also_log) {
    std::string formatted = "LUA Error: ";
    formatted.append(msg);
    error_history_.push_back(formatted);
    if (ctx_ != nullptr) {
        ctx_->report_error(formatted);
    }
    if (also_log && cfg_.log != nullptr) {
        const bool prev = in_rgl_log_;
        in_rgl_log_ = true;
        cfg_.log->error(formatted);
        in_rgl_log_ = prev;
    }
}

bool WseLuaManagerImpl::load_buffer_checked(std::string_view code,
                                            std::string_view chunk_name,
                                            std::string& err_out) {
    if (!code.empty() && static_cast<unsigned char>(code[0]) == 0x1BU) {
        err_out = "attempt to load bytecode";
        return false;
    }
    const std::string cname(chunk_name);
    if (luaL_loadbuffer(L_, code.data(), code.size(), cname.c_str()) != 0) {
        err_out = std::string(to_sv(L_, -1));
        c_lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool WseLuaManagerImpl::pcall_with_traceback(int nargs, int nresults,
                                             std::string_view context) {
    if (L_ == nullptr) return false;
    const int err_idx = ::lua_gettop(L_) - nargs;
    if (!reserve_stack(1)) {
        ::lua_settop(L_, err_idx - 1);
        return false;
    }
    lua_pushcfunction(L_, lua_traceback_handler);
    ::lua_insert(L_, err_idx);
    const int rc = ::lua_pcall(L_, nargs, nresults, err_idx);
    ::lua_remove(L_, err_idx);
    if (rc != 0) {
        std::string err = std::string(to_sv(L_, -1));
        c_lua_pop(L_, 1);
        if (!context.empty()) {
            record_error(std::string(context) + ": " + err);
        } else {
            record_error(err);
        }
        return false;
    }
    return true;
}

bool WseLuaManagerImpl::initialize(vm::VmContext& ctx,
                                   const vm::WseLuaInitConfig& cfg) {
    shutdown();

    ctx_ = &ctx;
    cfg_ = cfg;
    ready_ = false;
    has_main_script_ = false;
    print_history_.clear();
    error_history_.clear();
    script_fail_stack_.clear();

    resolved_module_dir_ = normalize_slash_dir(cfg.module_dir);
    resolved_lua_dir_ = cfg.lua_dir.empty()
                            ? join_vfs_dir(resolved_module_dir_, "lua")
                            : normalize_slash_dir(cfg.lua_dir);
    resolved_msfiles_dir_ = join_vfs_dir(resolved_lua_dir_, "msfiles");
    resolved_storage_dir_ = normalize_slash_dir(cfg.storage_dir);
    resolved_savegames_dir_ = normalize_slash_dir(cfg.savegames_dir);

    L_ = luaL_newstate();
    if (L_ == nullptr) {
        record_error("unable to allocate Lua state");
        return false;
    }

    push_sv(L_, kMgrRegistryKey);
    lua_pushlightuserdata(L_, this);
    lua_rawset(L_, LUA_REGISTRYINDEX);

    luaL_openlibs(L_);
    register_bit_library(L_);
    install_sandbox();
    register_callbacks();
    load_operations();

    std::string err;
    if (!load_buffer_checked(kLuaGlobalsScript, "=(LuaGlobals)", err) ||
        !pcall_with_traceback(0, 0, "LuaGlobals")) {
        if (!err.empty()) record_error(err);
        shutdown();
        return false;
    }

    load_constants_and_variables();
    ready_ = true;

    if (cfg_.vfs != nullptr) {
        const std::string main_path = join_vfs_dir(resolved_lua_dir_, "main.lua");
        if (cfg_.vfs->is_file(main_path)) {
            has_main_script_ = true;
            std::string code;
            if (!cfg_.vfs->read_text(main_path, code)) {
                record_error("unable to read main.lua");
            } else {
                std::string main_err;
                if (!load_buffer_checked(code, "@" + main_path, main_err)) {
                    record_error(main_err);
                } else {
                    (void)pcall_with_traceback(0, 0, "main.lua");
                }
            }
        }
    }

    return true;
}

void WseLuaManagerImpl::close_lua_state_only() noexcept {
    // Cleared first so file __gc handlers running inside lua_close can tell a
    // teardown from normal operation.
    ready_ = false;
    if (L_ != nullptr) {
        op_hook_refs_.fill(LUA_NOREF);
        lua_close(L_);
        L_ = nullptr;
    }
    ops_by_name_.clear();
    op_flags_by_code_.clear();
    gvar_map_.clear();
    script_fail_stack_.clear();
    clear_deferred_state();
    ctx_ = nullptr;
    cfg_ = {};
    ready_ = false;
    has_main_script_ = false;
    in_rgl_log_ = false;
    gc_flush_allowed_ = false;
}

void WseLuaManagerImpl::apply_deferred_edits() {
    if (deferred_edits_.empty()) return;
    // Edits run in the order Lua issued them, so each one sees the container
    // exactly as the index it was given assumed.
    auto edits = std::move(deferred_edits_);
    clear_deferred_state();
    for (auto& edit : edits) edit();
}

void WseLuaManagerImpl::shutdown() {
    if (L_ != nullptr) {
        if (ctx_ != nullptr && ctx_->scripts != nullptr) {
            for (auto& s : ctx_->scripts->scripts) {
                if (s.block.luaHookRef != vm::kNoLuaHook) {
                    luaL_unref(L_, LUA_REGISTRYINDEX, s.block.luaHookRef);
                    s.block.luaHookRef = vm::kNoLuaHook;
                }
            }
        }
        if (cfg_.bundle != nullptr) {
            for (auto& t : cfg_.bundle->game_triggers.triggers) {
                unref_all_block_callbacks(t.conditions);
                unref_all_block_callbacks(t.consequences);
            }
            for (auto& st : cfg_.bundle->simple_triggers.triggers) {
                unref_all_block_callbacks(st.block);
            }
            for (auto& mt : cfg_.bundle->mission_templates) {
                for (auto& t : mt.triggers.triggers) {
                    unref_all_block_callbacks(t.conditions);
                    unref_all_block_callbacks(t.consequences);
                }
            }
            for (auto& it : cfg_.bundle->item_triggers) {
                for (auto& st : it.triggers.triggers) {
                    unref_all_block_callbacks(st.block);
                }
            }
            for (auto& sp : cfg_.bundle->scene_props) {
                for (auto& st : sp.triggers.triggers) {
                    unref_all_block_callbacks(st.block);
                }
            }
            for (auto& pr : cfg_.bundle->presentations) {
                for (auto& st : pr.triggers.triggers) {
                    unref_all_block_callbacks(st.block);
                }
            }
        }
    }
    // An explicit shutdown happens while the caller's Vfs is alive, so files
    // Lua left open may flush from __gc. The destructor path must not.
    gc_flush_allowed_ = true;
    close_lua_state_only();
}

std::optional<std::uint16_t> WseLuaManagerImpl::operation_flags(
    std::string_view name) const noexcept {
    const auto it = ops_by_name_.find(std::string(name));
    if (it == ops_by_name_.end()) return std::nullopt;
    return it->second.flags;
}

std::optional<std::int32_t> WseLuaManagerImpl::gvar_index(
    std::string_view name) const noexcept {
    const auto it = gvar_map_.find(std::string(name));
    if (it == gvar_map_.end()) return std::nullopt;
    return it->second;
}

bool WseLuaManagerImpl::do_string(std::string_view code, std::string_view chunk_name) {
    if (L_ == nullptr) return false;
    std::string err;
    if (!load_buffer_checked(code, chunk_name, err)) {
        record_error(err);
        return false;
    }
    return pcall_with_traceback(0, 0, chunk_name);
}

bool WseLuaManagerImpl::do_file(std::string_view sandbox_rel_path) {
    if (L_ == nullptr || cfg_.vfs == nullptr) return false;
    std::string vfs_path;
    if (!sandbox_path(sandbox_rel_path, false, vfs_path)) {
        record_error("access denied by lua sandbox");
        return false;
    }
    std::string code;
    if (!cfg_.vfs->is_file(vfs_path) || !cfg_.vfs->read_text(vfs_path, code)) {
        record_error("unable to open file: " + vfs_path);
        return false;
    }
    std::string err;
    if (!load_buffer_checked(code, "@" + vfs_path, err)) {
        record_error(err);
        return false;
    }
    return pcall_with_traceback(0, 0, vfs_path);
}

// ---- WSE opcodes 5100..5114 ----
std::int32_t WseLuaManagerImpl::lua_get_top() {
    if (L_ == nullptr) return 0;
    return static_cast<std::int32_t>(::lua_gettop(L_));
}

bool WseLuaManagerImpl::lua_set_top(std::int32_t idx) {
    if (L_ == nullptr) return false;
    const int top = ::lua_gettop(L_);
    if (idx < -(top + 1)) return false;
    // lua_settop does not grow the stack itself; an unchecked large index
    // writes nils past the end of it.
    if (idx > top && lua_checkstack(L_, static_cast<int>(idx) - top) == 0) return false;
    ::lua_settop(L_, static_cast<int>(idx));
    return true;
}

void WseLuaManagerImpl::lua_insert(std::int32_t idx) {
    if (L_ == nullptr) return;
    const int top = ::lua_gettop(L_);
    if (idx == 0 || std::abs(idx) > top) return;
    ::lua_insert(L_, static_cast<int>(idx));
}

void WseLuaManagerImpl::lua_remove(std::int32_t idx) {
    if (L_ == nullptr) return;
    const int top = ::lua_gettop(L_);
    if (idx == 0 || std::abs(idx) > top) return;
    ::lua_remove(L_, static_cast<int>(idx));
}

void WseLuaManagerImpl::lua_pop(std::int32_t n) {
    if (L_ == nullptr || n <= 0) return;
    const int top = ::lua_gettop(L_);
    c_lua_pop(L_, std::min(top, static_cast<int>(n)));
}

std::int32_t WseLuaManagerImpl::lua_value_type(std::int32_t idx) {
    if (L_ == nullptr) {
        return static_cast<std::int32_t>(game::WseLuaValue::Type::none);
    }
    const int top = ::lua_gettop(L_);
    const int i = static_cast<int>(idx);
    if (i == 0 || i > top || (i < 0 && i > LUA_REGISTRYINDEX && -i > top)) {
        return static_cast<std::int32_t>(game::WseLuaValue::Type::none);
    }
    switch (::lua_type(L_, i)) {
    case LUA_TNIL:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::nil);
    case LUA_TBOOLEAN:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::boolean);
    case LUA_TLIGHTUSERDATA:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::lightuserdata);
    case LUA_TNUMBER:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::number);
    case LUA_TSTRING:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::string);
    case LUA_TTABLE:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::table);
    case LUA_TFUNCTION:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::function);
    case LUA_TUSERDATA:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::userdata);
    case LUA_TTHREAD:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::thread);
    default:
        return static_cast<std::int32_t>(game::WseLuaValue::Type::none);
    }
}

std::int64_t WseLuaManagerImpl::lua_to_int(std::int32_t idx) {
    if (L_ == nullptr) return 0;
    const int i = static_cast<int>(idx);
    if (lua_isboolean(L_, i)) return ::lua_toboolean(L_, i) ? 1 : 0;
    // Runs from a VM opcode, outside any protected call: must not raise.
    std::int64_t value = 0;
    if (::lua_isnumber(L_, i) && try_i64(L_, i, value)) return value;
    return 0;
}

bool WseLuaManagerImpl::lua_to_boolean(std::int32_t idx) {
    if (L_ == nullptr) return false;
    return ::lua_toboolean(L_, static_cast<int>(idx)) != 0;
}

bool WseLuaManagerImpl::lua_to_string(std::int32_t idx, std::string& out) {
    out.clear();
    if (L_ == nullptr) return false;
    const int i = static_cast<int>(idx);
    if (!::lua_isstring(L_, i)) return false;
    out = std::string(to_sv(L_, i));
    return true;
}

bool WseLuaManagerImpl::lua_to_pos(std::int32_t idx, core::WseTransform& out) {
    if (L_ == nullptr) return false;
    return read_pos(L_, static_cast<int>(idx), out);
}

void WseLuaManagerImpl::lua_push_int(std::int64_t val) {
    if (L_ == nullptr || !reserve_stack(1)) return;
    ::lua_pushnumber(L_, static_cast<lua_Number>(val));
}

void WseLuaManagerImpl::lua_push_str(std::string_view val) {
    if (L_ == nullptr || !reserve_stack(1)) return;
    push_sv(L_, val);
}

void WseLuaManagerImpl::lua_push_pos(const core::WseTransform& pos) {
    if (L_ == nullptr || !reserve_stack(kPushPosStackSlots)) return;
    push_pos(L_, pos);
}

bool WseLuaManagerImpl::lua_call(std::string_view func_name,
                                 std::int32_t num_args,
                                 std::int32_t /*caller*/) {
    if (L_ == nullptr || !ready_) return false;
    const int top = ::lua_gettop(L_);
    const int nargs = std::max(0, static_cast<int>(num_args));
    if (nargs > top) {
        record_error("lua_call: not enough arguments on stack");
        return false;
    }
    if (!reserve_stack(3) || !push_dotted_callable(L_, func_name)) {
        record_error("lua_call: undefined function '" + std::string(func_name) + "'");
        return false;
    }
    ::lua_insert(L_, ::lua_gettop(L_) - nargs);
    push_script_fail(false);
    const bool ok = pcall_with_traceback(nargs, LUA_MULTRET, func_name);
    const bool failed = current_script_failed();
    pop_script_fail();
    return ok && !failed;
}

bool WseLuaManagerImpl::lua_trigger_callback(std::int32_t ref,
                                             std::int32_t num_args,
                                             std::int32_t /*caller*/) {
    if (L_ == nullptr || !ready_ || ref == LUA_NOREF || ref == LUA_REFNIL) {
        return false;
    }
    const int top = ::lua_gettop(L_);
    const int nargs = std::clamp(static_cast<int>(num_args), 0, top);
    // The arguments are consumed by the call, so the stack must come back to
    // the height below them, whatever happens.
    const int base = top - nargs;
    if (!reserve_stack(2)) {
        ::lua_settop(L_, base);
        return false;
    }
    ::lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L_, -1)) {
        ::lua_settop(L_, base);
        return false;
    }
    if (nargs > 0) {
        ::lua_insert(L_, ::lua_gettop(L_) - nargs);
    }
    push_script_fail(false);
    const bool ok = pcall_with_traceback(nargs, 1, "lua_triggerCallback");
    const bool failed = current_script_failed();
    pop_script_fail();
    bool result = false;
    if (ok && !failed) {
        result = !lua_isboolean(L_, -1) || ::lua_toboolean(L_, -1) != 0;
    }
    ::lua_settop(L_, base);
    return result;
}

bool WseLuaManagerImpl::lua_test(std::int32_t idx) {
    if (L_ == nullptr) return false;
    const int i = static_cast<int>(idx);
    if (lua_isnoneornil(L_, i)) return false;
    if (lua_isboolean(L_, i)) return ::lua_toboolean(L_, i) != 0;
    if (::lua_isnumber(L_, i)) return ::lua_tonumber(L_, i) != 0.0;
    return true;
}

// ---- Engine Event Hooks ----
void WseLuaManagerImpl::on_world_trigger(float campaign_hours) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnWorldTrigger", 1, 0, [&](lua_State* L) {
        lua_pushnumber(L, static_cast<lua_Number>(campaign_hours));
    });
}

void WseLuaManagerImpl::on_rgl_log_write(std::string_view message) {
    if (!ready_ || L_ == nullptr || in_rgl_log_) return;
    in_rgl_log_ = true;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnRglLogWrite", 1, 0, [&](lua_State* L) {
        push_sv(L, message);
    });
    in_rgl_log_ = false;
}

void WseLuaManagerImpl::on_chat_message_received(std::int32_t player_no,
                                                  bool team_chat,
                                                  std::string& message_in_out,
                                                  bool& suppress_out) {
    suppress_out = false;
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    const int base_top = ::lua_gettop(L_);
    if (!call_game_event(*this, "OnChatMessageReceived", 3, LUA_MULTRET, [&](lua_State* L) {
            lua_pushinteger(L, player_no);
            lua_pushboolean(L, team_chat ? 1 : 0);
            push_sv(L, message_in_out);
        })) {
        return;
    }
    const int nret = ::lua_gettop(L_) - base_top;
    if (nret >= 1) {
        if (lua_isboolean(L_, base_top + 1)) {
            if (!::lua_toboolean(L_, base_top + 1)) suppress_out = true;
            if (nret >= 2 && ::lua_isstring(L_, base_top + 2)) {
                message_in_out = std::string(to_sv(L_, base_top + 2));
            }
        } else if (::lua_isstring(L_, base_top + 1)) {
            message_in_out = std::string(to_sv(L_, base_top + 1));
            if (nret >= 2 && lua_isboolean(L_, base_top + 2) &&
                !::lua_toboolean(L_, base_top + 2)) {
                suppress_out = true;
            }
        }
    }
}

void WseLuaManagerImpl::on_game_load() {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnGameLoad", 0, 0, [](lua_State*) {});
}

void WseLuaManagerImpl::on_save(std::int32_t slot_no) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnSave", 1, 0, [&](lua_State* L) {
        lua_pushinteger(L, slot_no);
    });
}

void WseLuaManagerImpl::on_load_save(std::int32_t slot_no) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnLoadSave", 1, 0, [&](lua_State* L) {
        lua_pushinteger(L, slot_no);
    });
}

void WseLuaManagerImpl::on_conversation_start(std::int32_t troop_no,
                                              std::int32_t agent_no,
                                              std::int32_t party_no) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnConversationStart", 3, 0, [&](lua_State* L) {
        lua_pushinteger(L, troop_no);
        lua_pushinteger(L, agent_no);
        lua_pushinteger(L, party_no);
    });
}

void WseLuaManagerImpl::on_conversation_line(std::int32_t troop_no,
                                             std::int32_t agent_no,
                                             bool is_player,
                                             std::string_view text,
                                             std::string_view sentence_id,
                                             bool is_auto_proceed) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnConversationLine", 6, 0, [&](lua_State* L) {
        lua_pushinteger(L, troop_no);
        lua_pushinteger(L, agent_no);
        lua_pushboolean(L, is_player ? 1 : 0);
        push_sv(L, text);
        push_sv(L, sentence_id);
        lua_pushboolean(L, is_auto_proceed ? 1 : 0);
    });
}

void WseLuaManagerImpl::on_conversation_end(std::int32_t troop_no,
                                            std::int32_t agent_no,
                                            std::int32_t party_no) {
    if (!ready_ || L_ == nullptr) return;
    LuaStackGuard guard(L_);
    (void)call_game_event(*this, "OnConversationEnd", 3, 0, [&](lua_State* L) {
        lua_pushinteger(L, troop_no);
        lua_pushinteger(L, agent_no);
        lua_pushinteger(L, party_no);
    });
}

} // namespace wse::lua::detail

std::shared_ptr<vm::WseLuaRuntime> create_lua_runtime() {
    return std::make_shared<detail::WseLuaManagerImpl>();
}

} // namespace wse::lua
