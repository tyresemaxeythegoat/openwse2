#include "lua_bitop.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include <lauxlib.h>
#include <lua.h>

namespace wse::lua::detail {
namespace {

using UBit = std::uint32_t;
using SBit = std::int32_t;

// Converts a Lua number into a 32-bit unsigned bit pattern without signed
// overflow or float-to-unsigned conversion UB on negative numbers.
UBit check_ubit32(lua_State* L, int idx) {
    const lua_Number n = luaL_checknumber(L, idx);
    if (!std::isfinite(n)) {
        luaL_argerror(L, idx, "number has no integer representation");
        return 0;
    }
    const double t = std::trunc(n);
    if (t >= 0.0 && t < 4294967296.0) {
        return static_cast<UBit>(t);
    }
    if (t >= -2147483648.0 && t < 0.0) {
        return static_cast<UBit>(static_cast<std::int64_t>(t));
    }
    const double wrapped = std::fmod(t, 4294967296.0);
    const std::int64_t i = static_cast<std::int64_t>(wrapped);
    return static_cast<UBit>(i);
}

int push_sbit32(lua_State* L, UBit u) {
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<SBit>(u)));
    return 1;
}

int bit_tobit(lua_State* L) {
    return push_sbit32(L, check_ubit32(L, 1));
}

int bit_bnot(lua_State* L) {
    return push_sbit32(L, ~check_ubit32(L, 1));
}

enum class FoldOp { And, Or, Xor };

int bit_fold(lua_State* L, FoldOp op, UBit init) {
    const int n = lua_gettop(L);
    UBit acc = init;
    for (int i = 1; i <= n; ++i) {
        const UBit v = check_ubit32(L, i);
        switch (op) {
        case FoldOp::And:
            acc &= v;
            break;
        case FoldOp::Or:
            acc |= v;
            break;
        case FoldOp::Xor:
            acc ^= v;
            break;
        }
    }
    return push_sbit32(L, acc);
}

int bit_band(lua_State* L) { return bit_fold(L, FoldOp::And, 0xFFFFFFFFU); }
int bit_bor(lua_State* L) { return bit_fold(L, FoldOp::Or, 0U); }
int bit_bxor(lua_State* L) { return bit_fold(L, FoldOp::Xor, 0U); }

UBit check_shift(lua_State* L, int idx) {
    return check_ubit32(L, idx) & 31U;
}

int bit_lshift(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    const UBit s = check_shift(L, 2);
    return push_sbit32(L, v << s);
}

int bit_rshift(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    const UBit s = check_shift(L, 2);
    return push_sbit32(L, v >> s);
}

int bit_arshift(lua_State* L) {
    const SBit v = static_cast<SBit>(check_ubit32(L, 1));
    const UBit s = check_shift(L, 2);
    if (s == 0U) {
        return push_sbit32(L, static_cast<UBit>(v));
    }
    if (v >= 0) {
        return push_sbit32(L, static_cast<UBit>(v) >> s);
    }
    const UBit mask = ~0U << (32U - s);
    return push_sbit32(L, (static_cast<UBit>(v) >> s) | mask);
}

int bit_rol(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    const UBit s = check_shift(L, 2);
    return push_sbit32(L, s == 0U ? v : ((v << s) | (v >> (32U - s))));
}

int bit_ror(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    const UBit s = check_shift(L, 2);
    return push_sbit32(L, s == 0U ? v : ((v >> s) | (v << (32U - s))));
}

int bit_bswap(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    const UBit swapped = ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U) |
                         ((v & 0x00FF0000U) >> 8U) | ((v & 0xFF000000U) >> 24U);
    return push_sbit32(L, swapped);
}

int bit_tohex(lua_State* L) {
    const UBit v = check_ubit32(L, 1);
    SBit n = lua_isnoneornil(L, 2) ? 8 : static_cast<SBit>(luaL_checkinteger(L, 2));
    const bool upper = n < 0;
    if (upper) {
        n = (n == std::numeric_limits<SBit>::min()) ? 8 : -n;
    }
    if (n < 1) {
        n = 1;
    } else if (n > 8) {
        n = 8;
    }
    static constexpr std::string_view kLower = "0123456789abcdef";
    static constexpr std::string_view kUpper = "0123456789ABCDEF";
    const std::string_view digits = upper ? kUpper : kLower;
    char buf[8]{};
    UBit cur = v;
    for (int i = n - 1; i >= 0; --i) {
        buf[i] = digits[cur & 0xFU];
        cur >>= 4U;
    }
    lua_pushlstring(L, buf, static_cast<size_t>(n));
    return 1;
}

const luaL_Reg kBitFuncs[] = {
    {"tobit", bit_tobit},     {"bnot", bit_bnot},   {"band", bit_band},
    {"bor", bit_bor},         {"bxor", bit_bxor},   {"lshift", bit_lshift},
    {"rshift", bit_rshift},   {"arshift", bit_arshift}, {"rol", bit_rol},
    {"ror", bit_ror},         {"bswap", bit_bswap}, {"tohex", bit_tohex},
    {nullptr, nullptr},
};

int luaopen_bit_module(lua_State* L) {
    luaL_register(L, "bit", kBitFuncs);
    return 1;
}

} // namespace

void register_bit_library(lua_State* L) {
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "preload");
        if (lua_istable(L, -1)) {
            lua_pushcfunction(L, luaopen_bit_module);
            lua_setfield(L, -2, "bit");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_pushcfunction(L, luaopen_bit_module);
    lua_call(L, 0, 1);
    lua_pop(L, 1);
}

} // namespace wse::lua::detail
