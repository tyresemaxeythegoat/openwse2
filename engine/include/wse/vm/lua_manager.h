// Factory header for instantiating the compiled stock Lua 5.1.5 backend.
// Separate from `lua_runtime.h` so header-only targets without `wse_lua` linkage
// incur no external symbol dependency.
#pragma once

#include <memory>

#include "wse/vm/lua_runtime.h"

namespace wse::lua {

[[nodiscard]] std::shared_ptr<vm::WseLuaRuntime> create_lua_runtime();

} // namespace wse::lua
