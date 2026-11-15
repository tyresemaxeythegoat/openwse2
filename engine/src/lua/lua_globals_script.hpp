// Clean-room Lua startup script mirroring WSE2 PE resource `LuaGlobals`
// (math/print, table.*, string.*, game.op / game.mt / game.reg / game.sreg /
// game.preg / game.gvar / game.const.mt / vector3 / game.rotation / game.pos).
#pragma once

#include <string_view>

namespace wse::lua::detail {

inline constexpr std::string_view kLuaGlobalsScript = R"WSE_LUA_GLOBALS(
local bit = require("bit")

function round(num, idp)
    local mult = 10 ^ (idp or 0)
    if num >= 0 then
        return math.floor(num * mult + 0.5) / mult
    end
    return math.ceil(num * mult - 0.5) / mult
end
math.round = round

function _G.print(...)
    local n = select("#", ...)
    if n == 0 then
        _print("")
        return
    end
    local parts = {}
    for i = 1, n do
        parts[i] = tostring(select(i, ...))
    end
    _print(table.concat(parts, "\t"))
end

function _G.printf(fmt, ...)
    print(string.format(fmt, ...))
end

------------ table helpers ------------
function table.contain(t, val)
    for _, v in pairs(t) do
        if v == val then return true end
    end
    return false
end
table.contains = table.contain

function table.find(t, val)
    for k, v in pairs(t) do
        if v == val then return k end
    end
    return nil
end

function table.length(t)
    local c = 0
    for _ in pairs(t) do
        c = c + 1
    end
    return c
end
table.count = table.length
table.size = table.length

function table.make(t, ...)
    local cur = t
    local n = select("#", ...)
    for i = 1, n do
        local k = select(i, ...)
        if cur[k] == nil then
            cur[k] = {}
        end
        cur = cur[k]
    end
    return cur
end

function table.copy(t, deep, seen)
    if type(t) ~= "table" then return t end
    seen = seen or {}
    if seen[t] then return seen[t] end
    local res = {}
    seen[t] = res
    for k, v in pairs(t) do
        local nk = deep and table.copy(k, true, seen) or k
        local nv = deep and table.copy(v, true, seen) or v
        res[nk] = nv
    end
    return setmetatable(res, getmetatable(t))
end

function table.clear(t)
    for k in pairs(t) do
        t[k] = nil
    end
end

function table.swap(t, i, j)
    t[i], t[j] = t[j], t[i]
end

function table.shuffle(t)
    for i = #t, 2, -1 do
        local j = math.random(i)
        t[i], t[j] = t[j], t[i]
    end
    return t
end

function table.shuffle_ipairs(t)
    local keys = {}
    for i = 1, #t do keys[i] = i end
    table.shuffle(keys)
    local idx = 0
    return function()
        idx = idx + 1
        local k = keys[idx]
        if k ~= nil then return k, t[k] end
    end
end

function table.reverse(t)
    local i, j = 1, #t
    while i < j do
        t[i], t[j] = t[j], t[i]
        i = i + 1
        j = j - 1
    end
    return t
end

function table.randVal(t)
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = k end
    if #keys == 0 then return nil end
    local k = keys[math.random(#keys)]
    return t[k], k
end

function table.dump(t, indent, seen)
    if type(t) ~= "table" then return tostring(t) end
    seen = seen or {}
    if seen[t] then return "<cycle>" end
    seen[t] = true
    indent = indent or 0
    local pad = string.rep("  ", indent)
    local out = {"{\n"}
    for k, v in pairs(t) do
        out[#out + 1] = pad .. "  [" .. tostring(k) .. "] = " .. table.dump(v, indent + 1, seen) .. ",\n"
    end
    out[#out + 1] = pad .. "}"
    return table.concat(out)
end

------------ string helpers ------------
function string.starts_with(s, prefix)
    return string.sub(s, 1, #prefix) == prefix
end
string.startswith = string.starts_with

function string.ends_with(s, suffix)
    return suffix == "" or string.sub(s, -#suffix) == suffix
end
string.endswith = string.ends_with

function string.trim(s)
    return string.match(s, "^%s*(.-)%s*$")
end

function string.split(s, sep)
    sep = sep or "%s+"
    local res = {}
    if sep == "" then
        for i = 1, #s do res[i] = string.sub(s, i, i) end
        return res
    end
    local pos = 1
    while true do
        local st, en = string.find(s, sep, pos, true)
        if not st then
            res[#res + 1] = string.sub(s, pos)
            break
        end
        res[#res + 1] = string.sub(s, pos, st - 1)
        pos = en + 1
    end
    return res
end

function string.get_matches(s, pat)
    local res = {}
    for m in string.gmatch(s, pat) do
        res[#res + 1] = m
    end
    return res
end

------------ vector3 / game.rotation / game.pos ------------
vector3 = {}
vector3.mt = {
    __add = function(a, b)
        return vector3.new((a.x or 0) + (b.x or 0), (a.y or 0) + (b.y or 0), (a.z or 0) + (b.z or 0))
    end,
    __sub = function(a, b)
        return vector3.new((a.x or 0) - (b.x or 0), (a.y or 0) - (b.y or 0), (a.z or 0) - (b.z or 0))
    end,
    __mul = function(a, b)
        if type(a) == "number" then
            return vector3.new(a * b.x, a * b.y, a * b.z)
        elseif type(b) == "number" then
            return vector3.new(a.x * b, a.y * b, a.z * b)
        end
        return (a.x * b.x) + (a.y * b.y) + (a.z * b.z)
    end,
    __div = function(a, s)
        return vector3.new(a.x / s, a.y / s, a.z / s)
    end,
    __unm = function(a)
        return vector3.new(-a.x, -a.y, -a.z)
    end,
    __eq = function(a, b)
        return a.x == b.x and a.y == b.y and a.z == b.z
    end,
    __tostring = function(v)
        return string.format("vector3(%.4f, %.4f, %.4f)", v.x, v.y, v.z)
    end,
    __index = function(_, k)
        return vector3[k]
    end,
}

function vector3.new(x, y, z)
    local v
    if type(x) == "table" then
        v = {x = tonumber(x.x) or 0, y = tonumber(x.y) or 0, z = tonumber(x.z) or 0}
    else
        v = {x = tonumber(x) or 0, y = tonumber(y) or 0, z = tonumber(z) or 0}
    end
    return setmetatable(v, vector3.mt)
end

setmetatable(vector3, {
    __call = function(_, x, y, z) return vector3.new(x, y, z) end
})

function vector3.lengthSq(v) return v.x * v.x + v.y * v.y + v.z * v.z end
function vector3.length(v) return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z) end
function vector3.dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
function vector3.cross(a, b)
    return vector3.new(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    )
end
function vector3.distance(a, b)
    local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end
function vector3.normalize(v)
    local len = vector3.length(v)
    if len > 0 then
        v.x = v.x / len
        v.y = v.y / len
        v.z = v.z / len
    end
    return v
end
function vector3.normalized(v)
    return vector3.new(v):normalize()
end
function vector3.copy(v) return vector3.new(v) end

game.rotation = {}
game.rotation.mt = {
    __index = function(_, k) return game.rotation[k] end,
    __eq = function(a, b)
        return a.s == b.s and a.f == b.f and a.u == b.u
    end,
}

function game.rotation.new(t)
    local s = t and t.s and vector3.new(t.s) or vector3.new(1, 0, 0)
    local f = t and t.f and vector3.new(t.f) or vector3.new(0, 1, 0)
    local u = t and t.u and vector3.new(t.u) or vector3.new(0, 0, 1)
    return setmetatable({s = s, f = f, u = u}, game.rotation.mt)
end

function game.rotation.copy(r)
    return game.rotation.new(r)
end

function game.rotation.orthonormalize(r)
    r.f:normalize()
    r.s = r.f:cross(r.u):normalize()
    r.u = r.s:cross(r.f)
    return r
end

game.pos = {}
game.pos.mt = {
    __index = function(_, k) return game.pos[k] end,
    __eq = function(a, b) return a.o == b.o and a.rot == b.rot end,
    __tostring = function(p)
        return string.format("pos(o=%s)", tostring(p.o))
    end,
}

function game.pos.new(arg)
    if type(arg) == "number" then
        return game.getReg(2, arg)
    end
    local o = arg and arg.o and vector3.new(arg.o) or vector3.new(0, 0, 0)
    local rot = arg and arg.rot and game.rotation.new(arg.rot) or game.rotation.new()
    return setmetatable({o = o, rot = rot}, game.pos.mt)
end

function game.pos.copy(p)
    return game.pos.new(p)
end

function game.pos.distance(a, b)
    return a.o:distance(b.o)
end

------------ game op wrappers / registers / consts ------------
game.op = game.op or {}

function game.op.make_default(opcode, flags)
    local is_lhs = bit.band(flags, 1) ~= 0
    local is_cf = bit.band(flags, 2) ~= 0
    if not is_lhs and not is_cf then
        return function(...)
            local err = game.execOperation(opcode, ...)
            if err ~= 0 then error("execOperation failed (" .. tostring(opcode) .. "): err=" .. tostring(err)) end
        end
    elseif is_cf and not is_lhs then
        return function(...)
            local cf, err = game.execOperation(opcode, ...)
            if err ~= 0 then error("execOperation failed (" .. tostring(opcode) .. "): err=" .. tostring(err)) end
            return cf
        end
    elseif not is_cf and is_lhs then
        return function(...)
            local lhs, err = game.execOperation(opcode, ...)
            if err ~= 0 then error("execOperation failed (" .. tostring(opcode) .. "): err=" .. tostring(err)) end
            return lhs
        end
    else
        return function(...)
            local cf, lhs, err = game.execOperation(opcode, ...)
            if err ~= 0 then error("execOperation failed (" .. tostring(opcode) .. "): err=" .. tostring(err)) end
            return cf, lhs
        end
    end
end

game.regMt = {
    __index = function(t, k) return game.getReg(t.typeId, k) end,
    __newindex = function(t, k, v) game.setReg(t.typeId, k, v) end,
}
game.reg = setmetatable({typeId = 0}, game.regMt)
game.sreg = setmetatable({typeId = 1}, game.regMt)
game.preg = setmetatable({typeId = 2}, game.regMt)

game.gvarMt = {
    __index = function(_, k) return game.getGvar(k) end,
    __newindex = function(_, k, v) game.setGvar(k, v) end,
}
game.gvar = setmetatable({}, game.gvarMt)

game.const = game.const or {}
game.const.mt = {
    __index = function(t, k)
        for _, v in pairs(t) do
            if type(v) == "table" and v[k] ~= nil then
                return v[k]
            end
        end
        if not game._disable_const_warnings then
            log_rgl("WARNING: game.const missing key: " .. tostring(k))
        end
        return nil
    end,
}
setmetatable(game.const, game.const.mt)

game.script = setmetatable({}, {
    __index = function(_, k) return game.getScriptNo(k) end,
})
game.string = setmetatable({}, {
    __index = function(_, k) return game.getStringId(k, false) end,
})
game.qstring = setmetatable({}, {
    __index = function(_, k) return game.getStringId(k, true) end,
})

game.mt = {
    __index = function(t, k)
        local flags = game.getOperationFlags(k)
        if flags ~= nil then
            local opcode = game._opcodes and game._opcodes[k]
            if opcode ~= nil then
                local fn = game.op.make_default(opcode, flags)
                rawset(t, k, fn)
                return fn
            end
        end
        local s = game.getScriptNo(k)
        if s ~= nil then
            local fn = function(...)
                return game.call_script(s, ...)
            end
            rawset(t, k, fn)
            return fn
        end
        return nil
    end,
}
setmetatable(game, game.mt)
)WSE_LUA_GLOBALS";

} // namespace wse::lua::detail
