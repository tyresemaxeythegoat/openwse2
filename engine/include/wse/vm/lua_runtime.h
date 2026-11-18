// Pure C++20 interface for the WSE2 Lua 5.1 runtime (`WSELuaManager`),
// kept strictly header-clean (no `<lua.h>` include) so standalone header
// checks and Android/host targets compile without Lua C headers.
//
// Evidence (WSE2 v1.1.5.6 PE + PDB + Ghidra 12.1.4):
//   WSELuaManager::WSELuaManager    0x14033d190  size 21504B (type 0xadaf)
//   WSELuaManager::initialize       0x14033d840  Lua state + stdlibs + LuaGlobals + main.lua
//   WSELuaManager::OnOperationExecute    0x14033db90  statement-level Lua hook
//   WSELuaManager::OnOperationMgrExecute 0x14033de50  block/script-level Lua hook
//   WSELuaManager::doMainScript     0x140342cd0  executes <module>/lua/main.lua
//   g_luaManager                    0x14069b6f0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/core/math.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
#include "wse/platform/vfs.h"

namespace wse::game {
struct ModuleBundle;
} // namespace wse::game

namespace wse::vm {

struct VmContext;

inline constexpr std::size_t kMaxLuaOperationSlots = 5300;
inline constexpr std::uint16_t kLuaOpFlagLhs = 0x1U;
inline constexpr std::uint16_t kLuaOpFlagCf = 0x2U;

struct WseLuaOpHookResult {
    bool execute = true;
    bool has_cf = false;
    bool cf_result = true;
    bool has_lhs = false;
    std::int64_t lhs_value = 0;
};

struct WseLuaScriptHookResult {
    bool execute = true;
    bool cf_result = true;
    bool replace_params = false;
    std::vector<std::int64_t> replacement_params{};
};

struct WseLuaInitConfig {
    // VFS paths relative to Warband root VFS. Empty module_dir means no module
    // files are mounted on disk; lua_dir defaults to `<module_dir>/lua`.
    std::string module_dir{};
    std::string lua_dir{};
    // Relative VFS directories mapped to `%storage%` and `%savegames%`.
    std::string storage_dir = "lua_storage";
    std::string savegames_dir = "savegames";
    platform::Vfs* vfs = nullptr;
    game::ModuleBundle* bundle = nullptr;
    platform::LogSink* log = nullptr;
    platform::Clock* clock = nullptr;
    bool disable_game_const_warnings = false;
};

class WseLuaRuntime {
public:
    virtual ~WseLuaRuntime() = default;

    // Initializes a fresh Lua 5.1 state, loads header_operations.py, msfiles
    // constants, variables.txt, installs the sandbox, game.* + _G.* bindings,
    // LuaGlobals startup script, and executes <lua_dir>/main.lua if present.
    // A runtime that is already initialized is shut down first (see shutdown()),
    // which touches the *previous* `cfg.vfs`, `cfg.bundle` and VmContext: they
    // must still be alive, or the caller must have called shutdown() before
    // destroying them. `run_headless_module` relies on the same rule.
    virtual bool initialize(VmContext& ctx, const WseLuaInitConfig& cfg) = 0;
    // Unreferences every Lua callback held by the VM, flushes sandboxed files
    // that are still open, and closes the state. `cfg.vfs` must still be alive.
    // Destroying the runtime without calling this is safe but discards
    // unflushed file writes, because the Vfs may already be gone by then.
    virtual void shutdown() = 0;

    // Applies trigger/presentation edits that Lua requested while a trigger
    // was executing (VmContext::triggerIterationDepth > 0). Called by
    // TriggerIterationScope once the outermost trigger execution has unwound.
    virtual void apply_deferred_edits() = 0;

    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    [[nodiscard]] virtual bool has_main_script() const noexcept = 0;
    [[nodiscard]] virtual std::size_t operation_count() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint16_t> operation_flags(
        std::string_view name) const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int32_t> gvar_index(
        std::string_view name) const noexcept = 0;
    [[nodiscard]] virtual const std::vector<std::string>& print_history() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<std::string>& error_history() const noexcept = 0;

    // Executes an inline chunk or sandboxed file inside the initialized state.
    virtual bool do_string(std::string_view code, std::string_view chunk_name = "chunk") = 0;
    virtual bool do_file(std::string_view sandbox_rel_path) = 0;

    // Statement and script hook entry points (OnOperationExecute / OnOperationMgrExecute).
    [[nodiscard]] virtual bool has_operation_hook(std::uint32_t opcode) const noexcept = 0;
    [[nodiscard]] virtual WseLuaOpHookResult on_operation_execute(
        std::uint32_t opcode,
        std::span<const std::int64_t> parsed_operands,
        std::span<const std::uint8_t> operand_tags,
        VmContext& ctx) = 0;
    [[nodiscard]] virtual WseLuaScriptHookResult on_script_execute(
        std::int32_t hook_ref,
        std::span<const std::int64_t> params,
        VmContext& ctx) = 0;

    // WSE opcodes 5100..5114 backed by the real lua_State*.
    [[nodiscard]] virtual std::int32_t lua_get_top() = 0;
    virtual bool lua_set_top(std::int32_t idx) = 0;
    virtual void lua_insert(std::int32_t idx) = 0;
    virtual void lua_remove(std::int32_t idx) = 0;
    virtual void lua_pop(std::int32_t n) = 0;
    [[nodiscard]] virtual std::int32_t lua_value_type(std::int32_t idx) = 0;
    [[nodiscard]] virtual std::int64_t lua_to_int(std::int32_t idx) = 0;
    [[nodiscard]] virtual bool lua_to_boolean(std::int32_t idx) = 0;
    virtual bool lua_to_string(std::int32_t idx, std::string& out) = 0;
    virtual bool lua_to_pos(std::int32_t idx, core::WseTransform& out) = 0;
    virtual void lua_push_int(std::int64_t val) = 0;
    virtual void lua_push_str(std::string_view val) = 0;
    virtual void lua_push_pos(const core::WseTransform& pos) = 0;
    virtual bool lua_call(std::string_view func_name, std::int32_t num_args,
                          std::int32_t caller) = 0;
    virtual bool lua_trigger_callback(std::int32_t ref, std::int32_t num_args,
                                      std::int32_t caller) = 0;
    [[nodiscard]] virtual bool lua_test(std::int32_t idx) = 0;

    // Game event callbacks mirrored from WSELuaManager.
    virtual void on_world_trigger(float campaign_hours) = 0;
    virtual void on_rgl_log_write(std::string_view message) = 0;
    virtual void on_chat_message_received(std::int32_t player_no, bool team_chat,
                                          std::string& message_in_out,
                                          bool& suppress_out) = 0;
    virtual void on_game_load() = 0;
    virtual void on_save(std::int32_t slot_no) = 0;
    virtual void on_load_save(std::int32_t slot_no) = 0;
    virtual void on_conversation_start(std::int32_t troop_no, std::int32_t agent_no,
                                       std::int32_t party_no) = 0;
    virtual void on_conversation_line(std::int32_t troop_no, std::int32_t agent_no,
                                      bool is_player, std::string_view text,
                                      std::string_view sentence_id,
                                      bool is_auto_proceed) = 0;
    virtual void on_conversation_end(std::int32_t troop_no, std::int32_t agent_no,
                                     std::int32_t party_no) = 0;
};

} // namespace wse::vm
