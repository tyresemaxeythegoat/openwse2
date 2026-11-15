// Internal C++20 declarations shared across engine/src/lua translation units.
// Strictly private to engine/src/lua/ so <lua.h> never leaks into headers.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/module_bundle.h"
#include "wse/game/world_m6_types.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/lua_manager.h"
#include "wse/vm/lua_runtime.h"
#include "wse/vm/ops/lua_bridge.h"
#include "wse/vm/wse_opcodes.h"

// Lua 5.1.5 is compiled as C++ (see CMakeLists.txt / build_host.ps1) so that
// Lua errors unwind as C++ exceptions and run destructors instead of
// longjmp-ing over live C++ objects. Its headers therefore have C++ linkage
// and must not be wrapped in `extern "C"`.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

namespace wse::lua::detail {

// Raises `what` as an ordinary Lua error (never returns).
[[noreturn]] void raise_cpp_exception(lua_State* L, const char* what);

// Every C function handed to Lua goes through this trampoline. Lua compiled as
// C++ protects calls with `catch (...)` (luaconf.h LUAI_TRY), so a C++
// exception escaping a callback (std::bad_alloc, a throwing Vfs, ...) would
// surface as status -1 with no error message. Converting it here gives Lua a
// normal, catchable error string. Lua's own errors are thrown as
// `lua_longjmp*`, which this handler does not match, so they pass through.
template <lua_CFunction Fn>
int guarded(lua_State* L) {
    char what[256];
    try {
        return Fn(L);
    } catch (const std::exception& e) {
        std::snprintf(what, sizeof(what), "%s", e.what());
    }
    // Raised outside the handler so the caught exception is destroyed first.
    raise_cpp_exception(L, what);
}

} // namespace wse::lua::detail

// Wrap Lua 5.1's `lua_pop(L, n)` macro and undefine it so WseLuaRuntime::lua_pop
// never collides with C preprocessor expansion inside translation units.
namespace wse::lua::detail {
inline void c_lua_pop(lua_State* L, int n) { lua_pop(L, n); }
} // namespace wse::lua::detail
#ifdef lua_pop
#undef lua_pop
#endif

namespace wse::lua::detail {

struct GameOpDef {
    std::string name{};
    std::int32_t opcode = 0;
    std::uint16_t flags = 0; // vm::kLuaOpFlagLhs / vm::kLuaOpFlagCf
};

inline void push_sv(lua_State* L, std::string_view sv) {
    lua_pushlstring(L, sv.data(), sv.size());
}

inline std::string_view to_sv(lua_State* L, int idx) {
    size_t len = 0;
    const char* p = lua_tolstring(L, idx, &len);
    return p ? std::string_view(p, len) : std::string_view{};
}

// Converts Lua number (double with exact 53-bit integer range) or boolean to
// int64 without float-to-int undefined behavior for uint32/uint64 bitmasks.
[[nodiscard]] std::int64_t lua_number_to_i64(lua_Number d, lua_State* L, int arg);
[[nodiscard]] std::int64_t check_i64(lua_State* L, int idx);
[[nodiscard]] std::int64_t opt_i64(lua_State* L, int idx, std::int64_t def);
// Non-raising variants for code that runs outside a protected call (after a
// pcall has returned, or from a VM opcode): a Lua error there is unprotected
// and terminates the process.
[[nodiscard]] bool number_to_i64_noexcept(lua_Number d, std::int64_t& out) noexcept;
[[nodiscard]] bool try_i64(lua_State* L, int idx, std::int64_t& out);

[[nodiscard]] bool is_pos(lua_State* L, int idx);
void push_pos(lua_State* L, const core::WseTransform& pos);
bool read_pos(lua_State* L, int idx, core::WseTransform& out);

class LuaStackGuard {
public:
    explicit LuaStackGuard(lua_State* L) noexcept : L_(L), top_(lua_gettop(L)) {}
    ~LuaStackGuard() {
        if (L_ != nullptr) {
            lua_settop(L_, top_);
        }
    }
    LuaStackGuard(const LuaStackGuard&) = delete;
    LuaStackGuard& operator=(const LuaStackGuard&) = delete;
    void release() noexcept { L_ = nullptr; }

private:
    lua_State* L_;
    int top_;
};

inline constexpr std::string_view kMgrRegistryKey = "wse_lua_manager_impl";

class WseLuaManagerImpl final : public vm::WseLuaRuntime {
public:
    WseLuaManagerImpl();
    ~WseLuaManagerImpl() override;

    bool initialize(vm::VmContext& ctx, const vm::WseLuaInitConfig& cfg) override;
    void shutdown() override;
    void apply_deferred_edits() override;
    void close_lua_state_only() noexcept;

    [[nodiscard]] bool is_ready() const noexcept override { return ready_; }
    [[nodiscard]] bool has_main_script() const noexcept override {
        return has_main_script_;
    }
    [[nodiscard]] std::size_t operation_count() const noexcept override {
        return ops_by_name_.size();
    }
    [[nodiscard]] std::optional<std::uint16_t> operation_flags(
        std::string_view name) const noexcept override;
    [[nodiscard]] std::optional<std::int32_t> gvar_index(
        std::string_view name) const noexcept override;
    [[nodiscard]] const std::vector<std::string>& print_history() const noexcept override {
        return print_history_;
    }
    [[nodiscard]] const std::vector<std::string>& error_history() const noexcept override {
        return error_history_;
    }

    bool do_string(std::string_view code, std::string_view chunk_name) override;
    bool do_file(std::string_view sandbox_rel_path) override;

    [[nodiscard]] bool has_operation_hook(std::uint32_t opcode) const noexcept override;
    vm::WseLuaOpHookResult on_operation_execute(
        std::uint32_t opcode,
        std::span<const std::int64_t> parsed_operands,
        std::span<const std::uint8_t> operand_tags,
        vm::VmContext& ctx) override;
    vm::WseLuaScriptHookResult on_script_execute(
        std::int32_t hook_ref,
        std::span<const std::int64_t> params,
        vm::VmContext& ctx) override;

    [[nodiscard]] std::int32_t lua_get_top() override;
    bool lua_set_top(std::int32_t idx) override;
    void lua_insert(std::int32_t idx) override;
    void lua_remove(std::int32_t idx) override;
    void lua_pop(std::int32_t n) override;
    [[nodiscard]] std::int32_t lua_value_type(std::int32_t idx) override;
    [[nodiscard]] std::int64_t lua_to_int(std::int32_t idx) override;
    [[nodiscard]] bool lua_to_boolean(std::int32_t idx) override;
    bool lua_to_string(std::int32_t idx, std::string& out) override;
    bool lua_to_pos(std::int32_t idx, core::WseTransform& out) override;
    void lua_push_int(std::int64_t val) override;
    void lua_push_str(std::string_view val) override;
    void lua_push_pos(const core::WseTransform& pos) override;
    bool lua_call(std::string_view func_name, std::int32_t num_args,
                  std::int32_t caller) override;
    bool lua_trigger_callback(std::int32_t ref, std::int32_t num_args,
                              std::int32_t caller) override;
    [[nodiscard]] bool lua_test(std::int32_t idx) override;

    void on_world_trigger(float campaign_hours) override;
    void on_rgl_log_write(std::string_view message) override;
    void on_chat_message_received(std::int32_t player_no, bool team_chat,
                                  std::string& message_in_out,
                                  bool& suppress_out) override;
    void on_game_load() override;
    void on_save(std::int32_t slot_no) override;
    void on_load_save(std::int32_t slot_no) override;
    void on_conversation_start(std::int32_t troop_no, std::int32_t agent_no,
                               std::int32_t party_no) override;
    void on_conversation_line(std::int32_t troop_no, std::int32_t agent_no,
                              bool is_player, std::string_view text,
                              std::string_view sentence_id,
                              bool is_auto_proceed) override;
    void on_conversation_end(std::int32_t troop_no, std::int32_t agent_no,
                             std::int32_t party_no) override;

    // Helpers callable from lua_ops / lua_callbacks translation units.
    [[nodiscard]] bool sandbox_path(std::string_view raw, bool for_write,
                                    std::string& out_vfs_path) const;
    [[nodiscard]] bool reserve_stack(int slots);
    void record_print(std::string msg);
    void record_error(std::string_view msg, bool also_log = true);
    void log_rgl_direct(std::string_view msg);

    bool pcall_with_traceback(int nargs, int nresults, std::string_view context);
    [[nodiscard]] bool load_buffer_checked(std::string_view code,
                                           std::string_view chunk_name,
                                           std::string& err_out);

    // Implemented across wse_lua_ops.cpp and wse_lua_callbacks.cpp.
    void install_sandbox();
    void load_operations();
    void load_constants_and_variables();
    void register_callbacks();

    void unref_all_block_callbacks(vm::StatementBlock& block);
    void push_script_fail(bool init = false) { script_fail_stack_.push_back(init); }
    void pop_script_fail() {
        if (!script_fail_stack_.empty()) script_fail_stack_.pop_back();
    }
    void set_script_fail() {
        if (!script_fail_stack_.empty()) script_fail_stack_.back() = true;
    }
    [[nodiscard]] bool current_script_failed() const noexcept {
        return !script_fail_stack_.empty() && script_fail_stack_.back();
    }

    // Structural trigger/presentation edits. While a trigger is executing the
    // edit is queued (in call order) and `pending_size_delta_` tracks how each
    // container's size will change, so indices and counts returned to Lua match
    // what in-place editing would have produced.
    [[nodiscard]] bool edits_deferred() const noexcept {
        return ctx_ != nullptr && ctx_->triggerIterationDepth > 0;
    }
    template <class Vec>
    [[nodiscard]] std::size_t virtual_size(const Vec& vec) const {
        const auto it = pending_size_delta_.find(&vec);
        const std::ptrdiff_t delta = it == pending_size_delta_.end() ? 0 : it->second;
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(vec.size()) + delta);
    }
    void run_or_defer_edit(const void* container, std::ptrdiff_t size_delta,
                           std::function<void()> edit) {
        if (edits_deferred()) {
            pending_size_delta_[container] += size_delta;
            deferred_edits_.push_back(std::move(edit));
        } else {
            edit();
        }
    }

    // Presentation ids in the order the container will hold them once the
    // queued edits are applied; only meaningful while edits_deferred().
    [[nodiscard]] std::vector<std::string>& pending_presentation_ids() {
        if (!pending_presentation_ids_valid_) {
            pending_presentation_ids_.clear();
            if (cfg_.bundle != nullptr) {
                for (const auto& p : cfg_.bundle->presentations) {
                    pending_presentation_ids_.push_back(p.id);
                }
            }
            pending_presentation_ids_valid_ = true;
        }
        return pending_presentation_ids_;
    }
    void clear_deferred_state() noexcept {
        deferred_edits_.clear();
        pending_size_delta_.clear();
        pending_presentation_ids_.clear();
        pending_presentation_ids_valid_ = false;
    }

    static WseLuaManagerImpl& from_lua(lua_State* L);

    lua_State* L_ = nullptr;
    vm::VmContext* ctx_ = nullptr;
    vm::WseLuaInitConfig cfg_{};
    bool ready_ = false;
    bool has_main_script_ = false;
    bool in_rgl_log_ = false;
    // True only while shutdown() closes the state: file __gc may then flush
    // through cfg_.vfs. False on the destructor path, where the Vfs may be gone.
    bool gc_flush_allowed_ = false;
    std::vector<std::function<void()>> deferred_edits_{};
    std::unordered_map<const void*, std::ptrdiff_t> pending_size_delta_{};
    std::vector<std::string> pending_presentation_ids_{};
    bool pending_presentation_ids_valid_ = false;

    std::string resolved_module_dir_{};
    std::string resolved_lua_dir_{};
    std::string resolved_msfiles_dir_{};
    std::string resolved_storage_dir_{};
    std::string resolved_savegames_dir_{};

    std::unordered_map<std::string, GameOpDef> ops_by_name_{};
    std::unordered_map<std::int32_t, std::uint16_t> op_flags_by_code_{};
    std::array<int, vm::kMaxLuaOperationSlots> op_hook_refs_{};
    std::unordered_map<std::string, std::int32_t> gvar_map_{};
    std::vector<bool> script_fail_stack_{};
    std::vector<std::string> print_history_{};
    std::vector<std::string> error_history_{};
};

} // namespace wse::lua::detail
