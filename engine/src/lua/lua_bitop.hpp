// Clean-room 32-bit bitwise extension (`bit`) matching LuaBitOp semantics for
// Lua 5.1 inside the WSE2 runtime.
#pragma once

struct lua_State;

namespace wse::lua::detail {

// Registers the `bit` global table and preloads `package.preload["bit"]`.
void register_bit_library(lua_State* L);

} // namespace wse::lua::detail
