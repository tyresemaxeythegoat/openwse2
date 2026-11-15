#include "wse_lua_internal.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wse::lua::detail {
namespace {

[[nodiscard]] std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] bool is_ident_char(char ch) noexcept {
    const unsigned char u = static_cast<unsigned char>(ch);
    return std::isalnum(u) != 0 || ch == '_';
}

[[nodiscard]] bool is_ident(std::string_view s) noexcept {
    if (s.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(s.front());
    if (std::isalpha(first) == 0 && s.front() != '_') return false;
    for (char ch : s) {
        if (!is_ident_char(ch)) return false;
    }
    return true;
}

[[nodiscard]] bool iequals_prefix(std::string_view text,
                                  std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string strip_py_const_table_name(std::string_view filename) {
    std::string base(filename);
    if (iequals_prefix(base, "header_")) {
        base.erase(0, 7);
    } else if (iequals_prefix(base, "id_")) {
        base.erase(0, 3);
    }
    if (base.size() >= 3 && iequals_prefix(std::string_view(base).substr(base.size() - 3), ".py")) {
        base.resize(base.size() - 3);
    }
    return base;
}

// Strips comments (`#`) outside single/double/triple Python strings on one line,
// tracking multiline triple-quoted docstring state across lines.
std::string strip_python_comment(std::string_view line, char& triple_quote) {
    std::string out;
    out.reserve(line.size());
    char quote = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        if (triple_quote != 0) {
            if (i + 2 < line.size() && line[i] == triple_quote &&
                line[i + 1] == triple_quote && line[i + 2] == triple_quote) {
                triple_quote = 0;
                i += 3;
            } else {
                ++i;
            }
            continue;
        }
        const char ch = line[i];
        if (quote != 0) {
            out.push_back(ch);
            if (ch == '\\' && i + 1 < line.size()) {
                out.push_back(line[i + 1]);
                i += 2;
                continue;
            }
            if (ch == quote) quote = 0;
            ++i;
            continue;
        }
        if ((ch == '"' || ch == '\'') && i + 2 < line.size() &&
            line[i + 1] == ch && line[i + 2] == ch) {
            triple_quote = ch;
            i += 3;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            out.push_back(ch);
            ++i;
            continue;
        }
        if (ch == '#') break;
        out.push_back(ch);
        ++i;
    }
    return out;
}

[[nodiscard]] bool parse_number_token(std::string_view tok, double& out) noexcept {
    tok = trim_sv(tok);
    if (tok.empty()) return false;
    bool neg = false;
    if (tok.front() == '-') {
        neg = true;
        tok.remove_prefix(1);
    } else if (tok.front() == '+') {
        tok.remove_prefix(1);
    }
    if (tok.empty()) return false;
    if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        std::uint64_t val = 0;
        const auto* begin = tok.data() + 2;
        const auto* end = tok.data() + tok.size();
        auto res = std::from_chars(begin, end, val, 16);
        if (res.ec != std::errc{} || res.ptr != end) return false;
        out = neg ? -static_cast<double>(val) : static_cast<double>(val);
        return true;
    }
    // Check integer first to preserve 53-bit integer exactness.
    std::int64_t ival = 0;
    const auto* begin = tok.data();
    const auto* end = tok.data() + tok.size();
    auto ires = std::from_chars(begin, end, ival, 10);
    if (ires.ec == std::errc{} && ires.ptr == end) {
        out = neg ? -static_cast<double>(ival) : static_cast<double>(ival);
        return true;
    }
    std::string copy(tok);
    char* parse_end = nullptr;
    const double dval = std::strtod(copy.c_str(), &parse_end);
    if (parse_end == copy.c_str() || *parse_end != '\0') return false;
    out = neg ? -dval : dval;
    return true;
}

// Truncates a double to its 64-bit pattern without float-to-integer undefined
// behaviour: values outside [-2^63, 2^64) (and inf/NaN) are rejected.
[[nodiscard]] bool double_to_u64_bits(double d, std::uint64_t& out) noexcept {
    std::int64_t bits = 0;
    if (!number_to_i64_noexcept(d, bits)) return false;
    out = static_cast<std::uint64_t>(bits);
    return true;
}

[[nodiscard]] bool eval_const_expr(
    std::string_view rhs,
    const std::unordered_map<std::string, double>& local_syms,
    double& out) {
    rhs = trim_sv(rhs);
    if (rhs.empty()) return false;

    // Strip matching outer parentheses when they enclose the whole expression.
    while (rhs.size() >= 2 && rhs.front() == '(' && rhs.back() == ')') {
        int depth = 0;
        bool wraps_all = true;
        for (std::size_t i = 0; i + 1 < rhs.size(); ++i) {
            if (rhs[i] == '(') ++depth;
            else if (rhs[i] == ')') --depth;
            if (depth == 0) {
                wraps_all = false;
                break;
            }
        }
        if (!wraps_all) break;
        rhs = trim_sv(rhs.substr(1, rhs.size() - 2));
    }

    auto split_op = [&](char op, std::string_view& lhs_out, std::string_view& rhs_out) {
        int depth = 0;
        for (std::size_t i = rhs.size(); i > 0; --i) {
            const char ch = rhs[i - 1];
            if (ch == ')') ++depth;
            else if (ch == '(') --depth;
            else if (depth == 0 && ch == op && i > 1) {
                // Skip unary +/- immediately after another operator.
                const std::string_view before = trim_sv(rhs.substr(0, i - 1));
                if (!before.empty() &&
                    (op == '|' || (before.back() != '+' && before.back() != '-' &&
                                   before.back() != '|' && before.back() != '<'))) {
                    lhs_out = before;
                    rhs_out = trim_sv(rhs.substr(i));
                    return true;
                }
            }
        }
        return false;
    };

    std::string_view lhs_part;
    std::string_view rhs_part;
    if (split_op('|', lhs_part, rhs_part)) {
        double a = 0.0, b = 0.0;
        if (!eval_const_expr(lhs_part, local_syms, a) ||
            !eval_const_expr(rhs_part, local_syms, b)) {
            return false;
        }
        std::uint64_t ua = 0;
        std::uint64_t ub = 0;
        if (!double_to_u64_bits(a, ua) || !double_to_u64_bits(b, ub)) return false;
        out = static_cast<double>(ua | ub);
        return true;
    }
    if (split_op('+', lhs_part, rhs_part)) {
        double a = 0.0, b = 0.0;
        if (!eval_const_expr(lhs_part, local_syms, a) ||
            !eval_const_expr(rhs_part, local_syms, b)) {
            return false;
        }
        out = a + b;
        return true;
    }
    // Check `a << b` shifts used in header_items/triggers bitfields.
    const std::size_t shl = rhs.find("<<");
    if (shl != std::string_view::npos) {
        double a = 0.0, b = 0.0;
        if (eval_const_expr(rhs.substr(0, shl), local_syms, a) &&
            eval_const_expr(rhs.substr(shl + 2), local_syms, b)) {
            std::uint64_t ua = 0;
            if (b >= 0.0 && b < 64.0 && double_to_u64_bits(a, ua)) {
                out = static_cast<double>(ua << static_cast<unsigned>(b));
                return true;
            }
        }
        return false;
    }

    if (parse_number_token(rhs, out)) return true;
    if (is_ident(rhs)) {
        const auto it = local_syms.find(std::string(rhs));
        if (it != local_syms.end()) {
            out = it->second;
            return true;
        }
    }
    return false;
}

void split_csv_idents(std::string_view body, std::vector<std::string>& out) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t comma = body.find(',', pos);
        const std::string_view raw =
            comma == std::string_view::npos ? body.substr(pos) : body.substr(pos, comma - pos);
        const std::string_view tok = trim_sv(raw);
        if (is_ident(tok)) {
            out.emplace_back(tok);
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
}

void split_rel_segments(std::string_view path, std::vector<std::string_view>& segs) {
    segs.clear();
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
        if (i >= path.size()) break;
        const std::size_t start = i;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') ++i;
        segs.push_back(path.substr(start, i - start));
    }
}

[[nodiscard]] bool join_sanitized_no_upward(std::string_view base_dir,
                                            std::string_view rel,
                                            std::string& out) {
    if (!rel.empty() && (rel.front() == '/' || rel.front() == '\\' ||
                         rel.find(':') != std::string_view::npos)) {
        return false;
    }
    std::vector<std::string_view> base_segs;
    split_rel_segments(base_dir, base_segs);
    std::vector<std::string_view> rel_segs;
    split_rel_segments(rel, rel_segs);
    for (std::string_view s : rel_segs) {
        if (s == "." || s.empty()) continue;
        if (s == "..") return false;
        base_segs.push_back(s);
    }
    out.clear();
    for (std::size_t i = 0; i < base_segs.size(); ++i) {
        if (i > 0) out.push_back('/');
        out.append(base_segs[i]);
    }
    return true;
}

void ensure_parent_dirs(platform::Vfs* vfs, std::string_view file_path) {
    if (vfs == nullptr) return;
    std::string cur;
    std::vector<std::string_view> segs;
    split_rel_segments(file_path, segs);
    if (segs.size() <= 1) return;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        if (!cur.empty()) cur.push_back('/');
        cur.append(segs[i]);
        if (!vfs->is_dir(cur)) {
            (void)vfs->make_dir(cur);
        }
    }
}

// ---- Sandboxed VFS file userdata for Lua `io` ----
struct LuaVfsFile {
    WseLuaManagerImpl* mgr = nullptr;
    platform::Vfs* vfs = nullptr;
    std::string path{};
    std::string data{};
    std::size_t pos = 0;
    bool readable = false;
    bool writable = false;
    bool dirty = false;
    bool closed = false;
};

constexpr const char* kFileMeta = "wse.VfsFile";

LuaVfsFile* check_vfs_file(lua_State* L, int idx) {
    return static_cast<LuaVfsFile*>(luaL_checkudata(L, idx, kFileMeta));
}

bool flush_vfs_file(LuaVfsFile* f) {
    if (f == nullptr || f->closed || !f->writable || f->vfs == nullptr) {
        return true;
    }
    if (!f->dirty) return true;
    ensure_parent_dirs(f->vfs, f->path);
    if (!f->vfs->write_text(f->path, f->data)) return false;
    f->dirty = false;
    return true;
}

int file_close(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (f->closed) {
        luaL_error(L, "attempt to use a closed file");
        return 0;
    }
    const bool ok = flush_vfs_file(f);
    f->closed = true;
    if (!ok) {
        lua_pushnil(L);
        lua_pushliteral(L, "failed to flush file to VFS");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int file_gc(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (!f->closed) {
        // When the state is torn down by the runtime's destructor the Vfs may
        // already have been destroyed; only an explicit shutdown() may flush.
        if (f->mgr != nullptr && f->mgr->L_ != nullptr &&
            (f->mgr->ready_ || f->mgr->gc_flush_allowed_)) {
            (void)flush_vfs_file(f);
        }
        f->closed = true;
    }
    f->~LuaVfsFile();
    return 0;
}

int file_flush(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (f->closed) return luaL_error(L, "attempt to use a closed file");
    if (!flush_vfs_file(f)) {
        lua_pushnil(L);
        lua_pushliteral(L, "write failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

bool read_one_item(LuaVfsFile* f, lua_State* L, int arg_idx) {
    if (f->pos >= f->data.size()) {
        // Stock Lua 5.1: "*a" at end of file yields "", every other format nil.
        if (!lua_isnumber(L, arg_idx)) {
            std::string_view fmt = to_sv(L, arg_idx);
            if (!fmt.empty() && fmt.front() == '*') fmt.remove_prefix(1);
            if (fmt.starts_with("a")) {
                lua_pushliteral(L, "");
                return true;
            }
        }
        lua_pushnil(L);
        return false;
    }
    if (lua_isnumber(L, arg_idx)) {
        const lua_Integer count = lua_tointeger(L, arg_idx);
        if (count <= 0) {
            lua_pushliteral(L, "");
            return true;
        }
        const std::size_t avail = f->data.size() - f->pos;
        const std::size_t take = std::min<std::size_t>(avail, static_cast<std::size_t>(count));
        lua_pushlstring(L, f->data.data() + f->pos, take);
        f->pos += take;
        return true;
    }
    std::string_view fmt = to_sv(L, arg_idx);
    if (!fmt.empty() && fmt.front() == '*') fmt.remove_prefix(1);
    if (fmt.starts_with("a")) {
        lua_pushlstring(L, f->data.data() + f->pos, f->data.size() - f->pos);
        f->pos = f->data.size();
        return true;
    }
    if (fmt.starts_with("l")) {
        const std::size_t start = f->pos;
        std::size_t end = start;
        while (end < f->data.size() && f->data[end] != '\n') ++end;
        std::size_t line_end = end;
        if (line_end > start && f->data[line_end - 1] == '\r') --line_end;
        lua_pushlstring(L, f->data.data() + start, line_end - start);
        f->pos = (end < f->data.size()) ? end + 1 : end;
        return true;
    }
    if (fmt.starts_with("n")) {
        std::size_t p = f->pos;
        while (p < f->data.size() &&
               std::isspace(static_cast<unsigned char>(f->data[p])) != 0) {
            ++p;
        }
        if (p >= f->data.size()) {
            f->pos = f->data.size();
            lua_pushnil(L);
            return false;
        }
        char* end_ptr = nullptr;
        const double val = std::strtod(f->data.c_str() + p, &end_ptr);
        if (end_ptr == f->data.c_str() + p) {
            lua_pushnil(L);
            return false;
        }
        f->pos = static_cast<std::size_t>(end_ptr - f->data.c_str());
        lua_pushnumber(L, val);
        return true;
    }
    return luaL_argerror(L, arg_idx, "invalid format") != 0;
}

int file_read(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (f->closed || !f->readable) {
        return luaL_error(L, "attempt to read closed or write-only file");
    }
    const int top = lua_gettop(L);
    if (top == 1) {
        lua_pushliteral(L, "*l");
        (void)read_one_item(f, L, 2);
        return 1;
    }
    int nres = 0;
    for (int i = 2; i <= top; ++i) {
        const bool ok = read_one_item(f, L, i);
        ++nres;
        if (!ok) break;
    }
    return nres;
}

int file_write(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (f->closed || !f->writable) {
        return luaL_error(L, "attempt to write to closed or read-only file");
    }
    const int top = lua_gettop(L);
    for (int i = 2; i <= top; ++i) {
        const std::string_view s = to_sv(L, i);
        if (s.data() == nullptr) {
            return luaL_argerror(L, i, "string or number expected");
        }
        if (f->pos >= f->data.size()) {
            f->data.append(s);
        } else {
            f->data.replace(f->pos, std::min(s.size(), f->data.size() - f->pos), s);
        }
        f->pos += s.size();
        f->dirty = true;
    }
    lua_pushboolean(L, 1); // Lua 5.1 returns true, not the file (that is 5.2+)
    return 1;
}

int file_seek(lua_State* L) {
    LuaVfsFile* f = check_vfs_file(L, 1);
    if (f->closed) return luaL_error(L, "attempt to use a closed file");
    const char* whence = luaL_optstring(L, 2, "cur");
    const long offset = static_cast<long>(luaL_optinteger(L, 3, 0));
    std::int64_t base = 0;
    if (std::strcmp(whence, "set") == 0) base = 0;
    else if (std::strcmp(whence, "cur") == 0) base = static_cast<std::int64_t>(f->pos);
    else if (std::strcmp(whence, "end") == 0) base = static_cast<std::int64_t>(f->data.size());
    else return luaL_argerror(L, 2, "invalid option");
    const std::int64_t next = base + offset;
    if (next < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "invalid seek position");
        return 2;
    }
    if (static_cast<std::size_t>(next) > f->data.size() && f->writable) {
        f->data.resize(static_cast<std::size_t>(next), '\0');
    }
    f->pos = std::min<std::size_t>(static_cast<std::size_t>(next), f->data.size());
    lua_pushinteger(L, static_cast<lua_Integer>(f->pos));
    return 1;
}

int file_lines_iter(lua_State* L) {
    LuaVfsFile* f = static_cast<LuaVfsFile*>(lua_touserdata(L, lua_upvalueindex(1)));
    const bool close_at_eof = lua_toboolean(L, lua_upvalueindex(2)) != 0;
    if (f == nullptr || f->closed) return 0;
    if (f->pos >= f->data.size()) {
        if (close_at_eof) f->closed = true;
        return 0;
    }
    const std::size_t start = f->pos;
    std::size_t end = start;
    while (end < f->data.size() && f->data[end] != '\n') ++end;
    std::size_t line_end = end;
    if (line_end > start && f->data[line_end - 1] == '\r') --line_end;
    lua_pushlstring(L, f->data.data() + start, line_end - start);
    f->pos = (end < f->data.size()) ? end + 1 : end;
    return 1;
}

int file_lines(lua_State* L) {
    check_vfs_file(L, 1);
    lua_pushvalue(L, 1);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, guarded<file_lines_iter>, 2);
    return 1;
}

int file_setvbuf(lua_State* L) {
    check_vfs_file(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int push_opened_vfs_file(lua_State* L, WseLuaManagerImpl& mgr,
                         std::string_view raw_path, std::string_view mode,
                         int& nresults) {
    const bool write_mode =
        mode.find('w') != std::string_view::npos ||
        mode.find('a') != std::string_view::npos ||
        mode.find('+') != std::string_view::npos;
    std::string vfs_path;
    if (!mgr.sandbox_path(raw_path, write_mode, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushnil(L);
        push_sv(L, "access denied by lua sandbox");
        nresults = 2;
        return 0;
    }

    std::string initial;
    const bool plus = mode.find('+') != std::string_view::npos;
    bool readable = mode.starts_with('r') || plus;
    bool writable = mode.starts_with('w') || mode.starts_with('a') || plus;
    std::size_t init_pos = 0;

    if (mode.starts_with('r')) {
        if (!mgr.cfg_.vfs->is_file(vfs_path) || !mgr.cfg_.vfs->read_text(vfs_path, initial)) {
            lua_pushnil(L);
            lua_pushliteral(L, "cannot open file");
            nresults = 2;
            return 0;
        }
    } else if (mode.starts_with('a')) {
        if (mgr.cfg_.vfs->is_file(vfs_path)) {
            (void)mgr.cfg_.vfs->read_text(vfs_path, initial);
        }
        init_pos = initial.size();
    } else if (mode.starts_with('w')) {
        // Truncate/create immediately on VFS so empty opens create the file.
        ensure_parent_dirs(mgr.cfg_.vfs, vfs_path);
        if (!mgr.cfg_.vfs->write_text(vfs_path, "")) {
            lua_pushnil(L);
            lua_pushliteral(L, "cannot create file");
            nresults = 2;
            return 0;
        }
    } else {
        lua_pushnil(L);
        lua_pushliteral(L, "invalid mode");
        nresults = 2;
        return 0;
    }

    void* mem = lua_newuserdata(L, sizeof(LuaVfsFile));
    auto* f = new (mem) LuaVfsFile{};
    f->mgr = &mgr;
    f->vfs = mgr.cfg_.vfs;
    f->path = std::move(vfs_path);
    f->data = std::move(initial);
    f->pos = init_pos;
    f->readable = readable;
    f->writable = writable;
    luaL_getmetatable(L, kFileMeta);
    lua_setmetatable(L, -2);
    nresults = 1;
    return 1;
}

int lua_io_open(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view filename = luaL_checkstring(L, 1);
    const std::string_view mode = luaL_optstring(L, 2, "r");
    int nres = 0;
    (void)push_opened_vfs_file(L, mgr, filename, mode, nres);
    return nres;
}

int lua_io_lines(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, "wse_io_input");
        if (!lua_isuserdata(L, -1)) {
            return luaL_error(L, "no default input file");
        }
        lua_pushboolean(L, 0);
        lua_pushcclosure(L, guarded<file_lines_iter>, 2);
        return 1;
    }
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view filename = luaL_checkstring(L, 1);
    int nres = 0;
    if (push_opened_vfs_file(L, mgr, filename, "r", nres) == 0) {
        return luaL_error(L, "cannot open file '%s'", std::string(filename).c_str());
    }
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, guarded<file_lines_iter>, 2);
    return 1;
}

int lua_io_default_stream(lua_State* L, const char* reg_key, const char* mode) {
    if (lua_isnoneornil(L, 1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, reg_key);
        return 1;
    }
    if (lua_isstring(L, 1)) {
        auto& mgr = WseLuaManagerImpl::from_lua(L);
        int nres = 0;
        if (push_opened_vfs_file(L, mgr, lua_tostring(L, 1), mode, nres) == 0) {
            return luaL_error(L, "cannot open file '%s'", lua_tostring(L, 1));
        }
    } else {
        (void)check_vfs_file(L, 1);
        lua_pushvalue(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, reg_key);
    return 1;
}

int lua_io_input(lua_State* L) {
    return lua_io_default_stream(L, "wse_io_input", "r");
}

int lua_io_output(lua_State* L) {
    return lua_io_default_stream(L, "wse_io_output", "w");
}

int lua_io_type(lua_State* L) {
    void* p = lua_touserdata(L, 1);
    if (p != nullptr && lua_getmetatable(L, 1)) {
        luaL_getmetatable(L, kFileMeta);
        const bool match = lua_rawequal(L, -1, -2) != 0;
        c_lua_pop(L, 2);
        if (match) {
            auto* f = static_cast<LuaVfsFile*>(p);
            lua_pushstring(L, f->closed ? "closed file" : "file");
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int lua_io_close(lua_State* L) {
    if (lua_isnone(L, 1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, "wse_io_output");
        if (lua_isnil(L, -1)) return 0;
        // The default output is now the only value, at index 1, which is
        // where file_close expects it (lua_replace(L, 1) here would pop it).
    }
    return file_close(L);
}

int lua_io_flush(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wse_io_output");
    if (lua_isnil(L, -1)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_insert(L, 1);
    return file_flush(L);
}

int lua_io_read(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wse_io_input");
    if (!lua_isuserdata(L, -1)) return luaL_error(L, "no default input file");
    lua_insert(L, 1);
    return file_read(L);
}

int lua_io_write(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wse_io_output");
    if (!lua_isuserdata(L, -1)) return luaL_error(L, "no default output file");
    lua_insert(L, 1);
    return file_write(L);
}

int lua_os_remove(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, true, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushnil(L);
        lua_pushliteral(L, "access denied by lua sandbox");
        return 2;
    }
    if (!mgr.cfg_.vfs->remove_file(vfs_path)) {
        lua_pushnil(L);
        lua_pushliteral(L, "cannot remove file");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_os_rename(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view old_raw = luaL_checkstring(L, 1);
    const std::string_view new_raw = luaL_checkstring(L, 2);
    std::string old_vfs, new_vfs;
    if (!mgr.sandbox_path(old_raw, true, old_vfs) ||
        !mgr.sandbox_path(new_raw, true, new_vfs) || mgr.cfg_.vfs == nullptr) {
        lua_pushnil(L);
        lua_pushliteral(L, "access denied by lua sandbox");
        return 2;
    }
    std::string content;
    if (!mgr.cfg_.vfs->read_text(old_vfs, content)) {
        lua_pushnil(L);
        lua_pushliteral(L, "source file not found");
        return 2;
    }
    // Rename is copy-then-remove; on the same file (the Vfs is ASCII
    // case-insensitive) the remove would delete the only copy.
    if (old_vfs.size() == new_vfs.size() && iequals_prefix(old_vfs, new_vfs)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    ensure_parent_dirs(mgr.cfg_.vfs, new_vfs);
    if (!mgr.cfg_.vfs->write_text(new_vfs, content) ||
        !mgr.cfg_.vfs->remove_file(old_vfs)) {
        lua_pushnil(L);
        lua_pushliteral(L, "rename failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Rejects compiled Lua bytecode (`\x1b...`) everywhere user chunks load.
int load_text_chunk(lua_State* L, std::string_view code,
                    std::string_view chunk_name, bool error_throws) {
    if (!code.empty() && static_cast<unsigned char>(code[0]) == 0x1BU) {
        if (error_throws) {
            return luaL_error(L, "attempt to load bytecode");
        }
        lua_pushnil(L);
        lua_pushliteral(L, "attempt to load bytecode");
        return 2;
    }
    const std::string cname(chunk_name);
    const int rc = luaL_loadbuffer(L, code.data(), code.size(), cname.c_str());
    if (rc != 0) {
        if (error_throws) return lua_error(L);
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

int lua_sandboxed_loadstring(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const char* chunk = luaL_optstring(L, 2, "=(loadstring)");
    return load_text_chunk(L, std::string_view(s, len), chunk, false);
}

int lua_sandboxed_load(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const char* chunk = luaL_optstring(L, 2, "=(load)");
    std::string buf;
    constexpr std::size_t kMaxChunkBytes = 16U << 20;
    for (;;) {
        lua_pushvalue(L, 1);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            c_lua_pop(L, 1);
            break;
        }
        if (!lua_isstring(L, -1)) {
            c_lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushliteral(L, "reader function must return a string");
            return 2;
        }
        size_t len = 0;
        const char* p = lua_tolstring(L, -1, &len);
        if (len == 0) {
            c_lua_pop(L, 1);
            break;
        }
        if (buf.size() + len > kMaxChunkBytes) {
            c_lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushliteral(L, "chunk exceeds maximum size");
            return 2;
        }
        buf.append(p, len);
        c_lua_pop(L, 1);
    }
    return load_text_chunk(L, buf, chunk, false);
}

int lua_sandboxed_loadfile(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, false, vfs_path) || mgr.cfg_.vfs == nullptr) {
        lua_pushnil(L);
        lua_pushliteral(L, "access denied by lua sandbox");
        return 2;
    }
    std::string code;
    if (!mgr.cfg_.vfs->is_file(vfs_path) || !mgr.cfg_.vfs->read_text(vfs_path, code)) {
        lua_pushnil(L);
        push_sv(L, "cannot open " + vfs_path);
        return 2;
    }
    return load_text_chunk(L, code, "@" + vfs_path, false);
}

int lua_sandboxed_dofile(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    const std::string_view raw = luaL_checkstring(L, 1);
    std::string vfs_path;
    if (!mgr.sandbox_path(raw, false, vfs_path) || mgr.cfg_.vfs == nullptr) {
        return luaL_error(L, "access denied by lua sandbox");
    }
    std::string code;
    if (!mgr.cfg_.vfs->is_file(vfs_path) || !mgr.cfg_.vfs->read_text(vfs_path, code)) {
        return luaL_error(L, "cannot open %s", vfs_path.c_str());
    }
    const int fn_res = load_text_chunk(L, code, "@" + vfs_path, true);
    if (fn_res != 1) return fn_res;
    const int nbefore = lua_gettop(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - (nbefore - 1);
}

int lua_wse_require_loader(lua_State* L) {
    auto& mgr = WseLuaManagerImpl::from_lua(L);
    if (mgr.cfg_.vfs == nullptr) {
        lua_pushliteral(L, "\n\tno VFS attached");
        return 1;
    }
    std::string mod(luaL_checkstring(L, 1));
    std::replace(mod.begin(), mod.end(), '.', '/');

    std::string paths_spec = "./?.lua;./?/init.lua";
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "path");
        if (lua_isstring(L, -1)) {
            paths_spec = lua_tostring(L, -1);
        }
        c_lua_pop(L, 1);
    }
    c_lua_pop(L, 1);

    std::string errors;
    std::size_t pos = 0;
    while (pos <= paths_spec.size()) {
        const std::size_t semi = paths_spec.find(';', pos);
        std::string tmpl = semi == std::string::npos
                               ? paths_spec.substr(pos)
                               : paths_spec.substr(pos, semi - pos);
        const std::size_t q = tmpl.find('?');
        if (q != std::string::npos) {
            tmpl.replace(q, 1, mod);
        }
        std::string vfs_path;
        if (!tmpl.empty() && mgr.sandbox_path(tmpl, false, vfs_path) &&
            mgr.cfg_.vfs->is_file(vfs_path)) {
            std::string code;
            if (mgr.cfg_.vfs->read_text(vfs_path, code)) {
                if (!code.empty() && static_cast<unsigned char>(code[0]) == 0x1BU) {
                    return luaL_error(L, "attempt to load bytecode in module '%s'",
                                      mod.c_str());
                }
                const std::string chunk = "@" + vfs_path;
                if (luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()) != 0) {
                    return lua_error(L);
                }
                return 1;
            }
        } else {
            errors.append("\n\tno file '");
            errors.append(tmpl);
            errors.push_back('\'');
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    push_sv(L, errors);
    return 1;
}

} // namespace

bool WseLuaManagerImpl::sandbox_path(std::string_view raw, bool for_write,
                                     std::string& out_vfs_path) const {
    raw = trim_sv(raw);
    if (raw.empty()) return false;
    if (raw.front() == '/' || raw.front() == '\\' ||
        raw.find(':') != std::string_view::npos) {
        return false;
    }

    auto match_special = [&](std::string_view tag,
                             std::string_view base_dir) -> int {
        if (!iequals_prefix(raw, tag)) return 0;
        std::string_view rest = raw.substr(tag.size());
        if (!rest.empty()) {
            if (rest.front() != '/' && rest.front() != '\\') return 0;
            while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
                rest.remove_prefix(1);
            }
        }
        if (!join_sanitized_no_upward(base_dir, rest, out_vfs_path)) return -1;
        return 1;
    };

    if (const int st = match_special("%storage%", resolved_storage_dir_); st != 0) {
        return st > 0;
    }
    if (const int sv = match_special("%savegames%", resolved_savegames_dir_); sv != 0) {
        return sv > 0;
    }

    if (for_write) {
        return join_sanitized_no_upward(resolved_lua_dir_, raw, out_vfs_path);
    }

    // Read path: start from resolved_lua_dir_ and allow '..' up to Warband VFS root.
    std::vector<std::string_view> stack;
    split_rel_segments(resolved_lua_dir_, stack);
    std::vector<std::string_view> rel_segs;
    split_rel_segments(raw, rel_segs);
    for (std::string_view s : rel_segs) {
        if (s == "." || s.empty()) continue;
        if (s == "..") {
            if (stack.empty()) return false; // escapes Warband VFS root
            stack.pop_back();
        } else {
            stack.push_back(s);
        }
    }
    out_vfs_path.clear();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i > 0) out_vfs_path.push_back('/');
        out_vfs_path.append(stack[i]);
    }
    return true;
}

void WseLuaManagerImpl::install_sandbox() {
    LuaStackGuard guard(L_);

    // Register VfsFile metatable.
    luaL_newmetatable(L_, kFileMeta);
    lua_pushvalue(L_, -1);
    lua_setfield(L_, -2, "__index");
    static const luaL_Reg kFileMethods[] = {
        {"close", guarded<file_close>},   {"flush", guarded<file_flush>}, {"lines", guarded<file_lines>},
        {"read", guarded<file_read>},     {"seek", guarded<file_seek>},   {"setvbuf", guarded<file_setvbuf>},
        {"write", guarded<file_write>},   {"__gc", guarded<file_gc>},     {nullptr, nullptr},
    };
    luaL_register(L_, nullptr, kFileMethods);
    c_lua_pop(L_, 1);

    // Replace io.* completely with sandboxed VFS-backed equivalents.
    lua_newtable(L_);
    static const luaL_Reg kIoFuncs[] = {
        {"open", guarded<lua_io_open>},   {"lines", guarded<lua_io_lines>}, {"input", guarded<lua_io_input>},
        {"output", guarded<lua_io_output>}, {"type", guarded<lua_io_type>}, {"close", guarded<lua_io_close>},
        {"flush", guarded<lua_io_flush>}, {"read", guarded<lua_io_read>},   {"write", guarded<lua_io_write>},
        {nullptr, nullptr},
    };
    luaL_register(L_, nullptr, kIoFuncs);
    lua_setglobal(L_, "io");

    // Lock down os.*: keep clock/date/difftime/time, sandbox remove/rename, nil unsafe ones.
    lua_getglobal(L_, "os");
    if (lua_istable(L_, -1)) {
        for (const char* dead : {"execute", "getenv", "exit", "tmpname", "setlocale"}) {
            lua_pushnil(L_);
            lua_setfield(L_, -2, dead);
        }
        lua_pushcfunction(L_, guarded<lua_os_remove>);
        lua_setfield(L_, -2, "remove");
        lua_pushcfunction(L_, guarded<lua_os_rename>);
        lua_setfield(L_, -2, "rename");
    }
    c_lua_pop(L_, 1);

    // Sandboxed chunk loaders rejecting bytecode (`\x1b`).
    lua_pushcfunction(L_, guarded<lua_sandboxed_loadstring>);
    lua_setglobal(L_, "loadstring");
    lua_pushcfunction(L_, guarded<lua_sandboxed_load>);
    lua_setglobal(L_, "load");
    lua_pushcfunction(L_, guarded<lua_sandboxed_loadfile>);
    lua_setglobal(L_, "loadfile");
    lua_pushcfunction(L_, guarded<lua_sandboxed_dofile>);
    lua_setglobal(L_, "dofile");

    // Lock down package.*: no native DLL loading, only preload + sandboxed VFS loader.
    lua_getglobal(L_, "package");
    if (lua_istable(L_, -1)) {
        lua_pushnil(L_);
        lua_setfield(L_, -2, "loadlib");
        lua_pushliteral(L_, "");
        lua_setfield(L_, -2, "cpath");
        lua_pushliteral(L_, "./?.lua;./?/init.lua");
        lua_setfield(L_, -2, "path");

        lua_newtable(L_);
        lua_getfield(L_, -2, "loaders");
        if (lua_istable(L_, -1)) {
            lua_rawgeti(L_, -1, 1); // preload loader
            lua_rawseti(L_, -3, 1);
        }
        c_lua_pop(L_, 1);
        lua_pushcfunction(L_, guarded<lua_wse_require_loader>);
        lua_rawseti(L_, -2, 2);
        lua_setfield(L_, -2, "loaders");
    }
    c_lua_pop(L_, 1);
}

void WseLuaManagerImpl::load_operations() {
    ops_by_name_.clear();
    op_flags_by_code_.clear();
    if (cfg_.vfs == nullptr) return;

    std::unordered_map<std::string, double> expr_syms;
    std::vector<std::string> lhs_ops;
    std::vector<std::string> cf_ops;

    auto parse_ops_file = [&](std::string_view rel_name) {
        const std::string path =
            resolved_msfiles_dir_.empty()
                ? std::string(rel_name)
                : (resolved_msfiles_dir_ + "/" + std::string(rel_name));
        std::string content;
        if (!cfg_.vfs->is_file(path) || !cfg_.vfs->read_text(path, content)) return;

        enum class ActiveList { None, Lhs, Cf };
        ActiveList active_list = ActiveList::None;
        char triple_quote = 0;

        std::size_t pos = 0;
        while (pos <= content.size()) {
            const std::size_t nl = content.find('\n', pos);
            std::string_view raw_line = nl == std::string_view::npos
                                            ? std::string_view(content).substr(pos)
                                            : std::string_view(content).substr(pos, nl - pos);
            std::string clean_str = strip_python_comment(raw_line, triple_quote);
            std::string_view line = trim_sv(clean_str);

            if (active_list != ActiveList::None) {
                const std::size_t close = line.find(']');
                const std::string_view chunk =
                    close == std::string_view::npos ? line : line.substr(0, close);
                split_csv_idents(chunk, active_list == ActiveList::Lhs ? lhs_ops : cf_ops);
                if (close != std::string_view::npos) active_list = ActiveList::None;
            } else if (!line.empty()) {
                const std::size_t eq = line.find('=');
                if (eq != std::string_view::npos) {
                    std::string_view lhs_part = trim_sv(line.substr(0, eq));
                    if (!lhs_part.empty() && lhs_part.back() == '+') {
                        lhs_part = trim_sv(lhs_part.substr(0, lhs_part.size() - 1));
                    }
                    const std::string_view rhs_part = trim_sv(line.substr(eq + 1));
                    if (is_ident(lhs_part)) {
                        if (!rhs_part.empty() && rhs_part.front() == '[') {
                            ActiveList kind = ActiveList::None;
                            if (lhs_part == "lhs_operations" ||
                                lhs_part == "global_lhs_operations") {
                                kind = ActiveList::Lhs;
                            } else if (lhs_part == "can_fail_operations") {
                                kind = ActiveList::Cf;
                            }
                            if (kind != ActiveList::None) {
                                const std::string_view after = rhs_part.substr(1);
                                const std::size_t close = after.find(']');
                                const std::string_view chunk =
                                    close == std::string_view::npos
                                        ? after
                                        : after.substr(0, close);
                                split_csv_idents(chunk,
                                                 kind == ActiveList::Lhs ? lhs_ops : cf_ops);
                                if (close == std::string_view::npos) active_list = kind;
                            }
                        } else {
                            double val = 0.0;
                            if (eval_const_expr(rhs_part, expr_syms, val)) {
                                const std::string key(lhs_part);
                                expr_syms[key] = val;
                                // Range-check as a double first: flag constants such
                                // as `neg = 0x80000000` do not fit an int32.
                                if (val >= 0.0 &&
                                    val < static_cast<double>(vm::kMaxLuaOperationSlots)) {
                                    const auto opcode = static_cast<std::int32_t>(val);
                                    ops_by_name_[key] = GameOpDef{key, opcode, 0};
                                    op_flags_by_code_.try_emplace(opcode, std::uint16_t{0});
                                }
                            }
                        }
                    }
                }
            }

            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
    };

    parse_ops_file("header_operations.py");
    parse_ops_file("header_operations_addon.py");

    auto apply_flag = [&](const std::vector<std::string>& names, std::uint16_t flag) {
        for (const auto& name : names) {
            auto it = ops_by_name_.find(name);
            if (it != ops_by_name_.end()) {
                it->second.flags |= flag;
                op_flags_by_code_[it->second.opcode] |= flag;
            }
        }
    };
    apply_flag(lhs_ops, vm::kLuaOpFlagLhs);
    apply_flag(cf_ops, vm::kLuaOpFlagCf);

    // Expose `game._opcodes[name] = opcode` for LuaGlobals `game.mt`.
    LuaStackGuard guard(L_);
    lua_getglobal(L_, "game");
    if (lua_istable(L_, -1)) {
        lua_newtable(L_);
        for (const auto& [name, def] : ops_by_name_) {
            push_sv(L_, name);
            lua_pushinteger(L_, def.opcode);
            lua_rawset(L_, -3);
        }
        lua_setfield(L_, -2, "_opcodes");
    }
    c_lua_pop(L_, 1);
}

void WseLuaManagerImpl::load_constants_and_variables() {
    gvar_map_.clear();
    if (cfg_.vfs == nullptr) return;

    // 1. Load <module>/variables.txt -> 0-based gvar index map.
    if (!resolved_module_dir_.empty()) {
        const std::string vars_path = resolved_module_dir_ + "/variables.txt";
        std::string vars_content;
        if (cfg_.vfs->is_file(vars_path) && cfg_.vfs->read_text(vars_path, vars_content)) {
            std::int32_t idx = 0;
            std::size_t pos = 0;
            while (pos <= vars_content.size()) {
                const std::size_t nl = vars_content.find('\n', pos);
                std::string_view raw =
                    nl == std::string_view::npos
                        ? std::string_view(vars_content).substr(pos)
                        : std::string_view(vars_content).substr(pos, nl - pos);
                const std::string_view name = trim_sv(raw);
                if (!name.empty()) {
                    gvar_map_[std::string(name)] = idx++;
                }
                if (nl == std::string_view::npos) break;
                pos = nl + 1;
            }
        }
    }

    // 2. Scan msfiles for header_*.py and ID_*.py (excluding header_operations*).
    std::vector<platform::VfsEntry> entries;
    if (!cfg_.vfs->list_dir(resolved_msfiles_dir_, entries)) return;
    std::sort(entries.begin(), entries.end(),
              [](const platform::VfsEntry& a, const platform::VfsEntry& b) {
                  return a.name < b.name;
              });

    LuaStackGuard guard(L_);
    lua_getglobal(L_, "game");
    if (!lua_istable(L_, -1)) return;
    lua_getfield(L_, -1, "const");
    if (!lua_istable(L_, -1)) {
        c_lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushvalue(L_, -1);
        lua_setfield(L_, -3, "const");
    }

    for (const auto& ent : entries) {
        if (ent.is_directory) continue;
        const std::string& entry = ent.name;
        if (!iequals_prefix(entry, "header_") && !iequals_prefix(entry, "id_")) continue;
        if (iequals_prefix(entry, "header_operations")) continue;
        if (entry.size() < 4 ||
            !iequals_prefix(std::string_view(entry).substr(entry.size() - 3), ".py")) {
            continue;
        }
        const std::string full_path =
            resolved_msfiles_dir_.empty() ? entry : (resolved_msfiles_dir_ + "/" + entry);
        std::string content;
        if (!cfg_.vfs->is_file(full_path) || !cfg_.vfs->read_text(full_path, content)) {
            continue;
        }

        const std::string table_name = strip_py_const_table_name(entry);
        std::unordered_map<std::string, double> syms;
        char triple_quote = 0;

        lua_getfield(L_, -1, table_name.c_str());
        if (!lua_istable(L_, -1)) {
            c_lua_pop(L_, 1);
            lua_newtable(L_);
        }

        std::size_t pos = 0;
        while (pos <= content.size()) {
            const std::size_t nl = content.find('\n', pos);
            std::string_view raw_line =
                nl == std::string_view::npos
                    ? std::string_view(content).substr(pos)
                    : std::string_view(content).substr(pos, nl - pos);
            std::string clean_str = strip_python_comment(raw_line, triple_quote);
            const std::string_view line = trim_sv(clean_str);
            const std::size_t eq = line.find('=');
            if (eq != std::string_view::npos && line.find('[') == std::string_view::npos) {
                const std::string_view lhs = trim_sv(line.substr(0, eq));
                const std::string_view rhs = trim_sv(line.substr(eq + 1));
                double val = 0.0;
                if (is_ident(lhs) && eval_const_expr(rhs, syms, val)) {
                    const std::string key(lhs);
                    syms[key] = val;
                    push_sv(L_, key);
                    lua_pushnumber(L_, val);
                    lua_rawset(L_, -3);
                }
            }
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }

        lua_setfield(L_, -2, table_name.c_str());
    }
}

} // namespace wse::lua::detail
