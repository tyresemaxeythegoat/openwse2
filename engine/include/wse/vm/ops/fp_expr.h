#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/vm/wse_state.h"

namespace wse::vm {

enum class WseExprOp : std::uint8_t {
    literal = 0,
    fp_var,
    pos_var,
    unary_neg,
    unary_pos,
    assign,
    sequence,
    ternary,
    log_or,
    log_and,
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    add,
    sub,
    mul,
    div,
    pow,
    fn_sin,
    fn_cos,
    fn_tan,
    fn_asin,
    fn_acos,
    fn_atan,
    fn_sinh,
    fn_cosh,
    fn_tanh,
    fn_asinh,
    fn_acosh,
    fn_atanh,
    fn_log2,
    fn_log10,
    fn_ln,
    fn_exp,
    fn_sqrt,
    fn_sign,
    fn_rint,
    fn_abs,
    fn_deg2rad,
    fn_rad2deg,
    fn_floor,
    fn_ceil,
    fn_fmod,
    fn_atan2,
    fn_clamp,
    fn_sum,
    fn_avg,
    fn_min,
    fn_max,
};

struct WseExprNode {
    WseExprOp op = WseExprOp::literal;
    float literal_value = 0.0f;
    std::uint16_t reg_index = 0;
    std::uint8_t pos_comp = 0;
    std::size_t a = 0;
    std::size_t b = 0;
    std::size_t c = 0;
};

struct WseExprCompiler {
    std::string_view src{};
    std::size_t pos = 0;
    std::vector<WseExprNode> nodes{};
    std::vector<std::size_t> call_args{};
    std::size_t depth = 0;

    static constexpr std::size_t kMaxParseDepth = 256;

    void skip_ws() noexcept {
        while (pos < src.size() && wse::core::wse_is_ascii_space(src[pos])) {
            ++pos;
        }
    }

    [[nodiscard]] std::size_t add_node(WseExprNode node) {
        const std::size_t id = nodes.size();
        nodes.push_back(node);
        return id;
    }

    [[nodiscard]] static bool parse_uint_suffix(
        std::string_view sv, std::size_t& cur, std::uint16_t& out) noexcept {
        if (cur >= sv.size() || sv[cur] < '0' || sv[cur] > '9') {
            return false;
        }
        std::uint32_t v = 0;
        while (cur < sv.size() && sv[cur] >= '0' && sv[cur] <= '9') {
            v = v * 10U + static_cast<std::uint32_t>(sv[cur] - '0');
            if (v > 127U) {
                return false;
            }
            ++cur;
        }
        out = static_cast<std::uint16_t>(v);
        return true;
    }

    [[nodiscard]] static bool match_pos_var(
        std::string_view ident,
        std::uint16_t& reg_out,
        std::uint8_t& comp_out) noexcept {
        if (!ident.starts_with("pos")) {
            return false;
        }
        std::size_t cur = 3;
        if (!parse_uint_suffix(ident, cur, reg_out)) {
            return false;
        }
        const std::string_view comp = ident.substr(cur);
        constexpr std::array<std::string_view, 12> kComps = {
            "sx", "sy", "sz", "fx", "fy", "fz",
            "ux", "uy", "uz", "x",  "y",  "z"};
        for (std::size_t i = 0; i < kComps.size(); ++i) {
            if (comp == kComps[i]) {
                comp_out = static_cast<std::uint8_t>(i);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_number(std::size_t& node_out) {
        const std::size_t start = pos;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            std::size_t epos = pos + 1;
            if (epos < src.size() && (src[epos] == '+' || src[epos] == '-')) {
                ++epos;
            }
            if (epos < src.size() && src[epos] >= '0' && src[epos] <= '9') {
                pos = epos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                    ++pos;
                }
            }
        }
        const std::string token(src.substr(start, pos - start));
        char* endptr = nullptr;
        const float val = std::strtof(token.c_str(), &endptr);
        if (endptr != token.data() + token.size()) {
            return false;
        }
        WseExprNode node{};
        node.op = WseExprOp::literal;
        node.literal_value = val;
        node_out = add_node(node);
        return true;
    }

    [[nodiscard]] static bool lookup_builtin(
        std::string_view name,
        WseExprOp& op_out,
        std::size_t& min_args,
        std::size_t& max_args) noexcept {
        struct FuncEntry {
            std::string_view name;
            WseExprOp op;
            std::size_t min_a;
            std::size_t max_a;
        };
        constexpr std::size_t kMaxArgs = std::numeric_limits<std::size_t>::max();
        constexpr std::array<FuncEntry, 32> kTable = {{
            {"sin", WseExprOp::fn_sin, 1, 1},
            {"cos", WseExprOp::fn_cos, 1, 1},
            {"tan", WseExprOp::fn_tan, 1, 1},
            {"asin", WseExprOp::fn_asin, 1, 1},
            {"acos", WseExprOp::fn_acos, 1, 1},
            {"atan", WseExprOp::fn_atan, 1, 1},
            {"sinh", WseExprOp::fn_sinh, 1, 1},
            {"cosh", WseExprOp::fn_cosh, 1, 1},
            {"tanh", WseExprOp::fn_tanh, 1, 1},
            {"asinh", WseExprOp::fn_asinh, 1, 1},
            {"acosh", WseExprOp::fn_acosh, 1, 1},
            {"atanh", WseExprOp::fn_atanh, 1, 1},
            {"log2", WseExprOp::fn_log2, 1, 1},
            {"log10", WseExprOp::fn_log10, 1, 1},
            {"log", WseExprOp::fn_ln, 1, 1},
            {"ln", WseExprOp::fn_ln, 1, 1},
            {"exp", WseExprOp::fn_exp, 1, 1},
            {"sqrt", WseExprOp::fn_sqrt, 1, 1},
            {"sign", WseExprOp::fn_sign, 1, 1},
            {"rint", WseExprOp::fn_rint, 1, 1},
            {"abs", WseExprOp::fn_abs, 1, 1},
            {"deg2rad", WseExprOp::fn_deg2rad, 1, 1},
            {"rad2deg", WseExprOp::fn_rad2deg, 1, 1},
            {"floor", WseExprOp::fn_floor, 1, 1},
            {"ceil", WseExprOp::fn_ceil, 1, 1},
            {"fmod", WseExprOp::fn_fmod, 2, 2},
            {"atan2", WseExprOp::fn_atan2, 2, 2},
            {"clamp", WseExprOp::fn_clamp, 3, 3},
            {"sum", WseExprOp::fn_sum, 1, kMaxArgs},
            {"avg", WseExprOp::fn_avg, 1, kMaxArgs},
            {"min", WseExprOp::fn_min, 1, kMaxArgs},
            {"max", WseExprOp::fn_max, 1, kMaxArgs},
        }};
        for (const auto& entry : kTable) {
            if (name == entry.name) {
                op_out = entry.op;
                min_args = entry.min_a;
                max_args = entry.max_a;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_primary(std::size_t& node_out) {
        if (++depth > kMaxParseDepth) {
            return false;
        }
        struct DepthPop {
            std::size_t& d;
            ~DepthPop() noexcept { --d; }
        } pop{depth};

        skip_ws();
        if (pos >= src.size()) {
            return false;
        }
        const char ch = src[pos];
        if (ch == '-') {
            ++pos;
            std::size_t inner = 0;
            if (!parse_prec(9, inner)) {
                return false;
            }
            node_out = add_node({WseExprOp::unary_neg, 0.0f, 0, 0, inner, 0, 0});
            return true;
        }
        if (ch == '+') {
            ++pos;
            std::size_t inner = 0;
            if (!parse_prec(9, inner)) {
                return false;
            }
            node_out = add_node({WseExprOp::unary_pos, 0.0f, 0, 0, inner, 0, 0});
            return true;
        }
        if (ch == '(') {
            ++pos;
            if (!parse_prec(0, node_out)) {
                return false;
            }
            skip_ws();
            if (pos >= src.size() || src[pos] != ')') {
                return false;
            }
            ++pos;
            return true;
        }
        if ((ch >= '0' && ch <= '9') ||
            (ch == '.' && pos + 1 < src.size() && src[pos + 1] >= '0' &&
             src[pos + 1] <= '9')) {
            return parse_number(node_out);
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            ch == '_') {
            const std::size_t start = pos;
            while (pos < src.size()) {
                const char c = src[pos];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                    ++pos;
                } else {
                    break;
                }
            }
            const std::string_view ident = src.substr(start, pos - start);
            skip_ws();
            if (pos < src.size() && src[pos] == '(') {
                WseExprOp fn_op{};
                std::size_t min_a = 0;
                std::size_t max_a = 0;
                if (!lookup_builtin(ident, fn_op, min_a, max_a)) {
                    return false;
                }
                ++pos;
                std::vector<std::size_t> args;
                skip_ws();
                if (pos < src.size() && src[pos] != ')') {
                    for (;;) {
                        std::size_t arg_node = 0;
                        if (!parse_prec(1, arg_node)) {
                            return false;
                        }
                        args.push_back(arg_node);
                        skip_ws();
                        if (pos >= src.size()) {
                            return false;
                        }
                        if (src[pos] == ')') {
                            break;
                        }
                        if (src[pos] != ',') {
                            return false;
                        }
                        ++pos;
                    }
                }
                if (pos >= src.size() || src[pos] != ')') {
                    return false;
                }
                ++pos;
                if (args.size() < min_a || args.size() > max_a) {
                    return false;
                }
                const std::size_t offset = call_args.size();
                call_args.insert(call_args.end(), args.begin(), args.end());
                WseExprNode node{};
                node.op = fn_op;
                node.a = offset;
                node.b = args.size();
                node_out = add_node(node);
                return true;
            }
            if (ident == "_pi") {
                node_out =
                    add_node({WseExprOp::literal, 3.1415927f, 0, 0, 0, 0, 0});
                return true;
            }
            if (ident == "_e") {
                node_out =
                    add_node({WseExprOp::literal, 2.7182817f, 0, 0, 0, 0, 0});
                return true;
            }
            if (ident.starts_with("fp")) {
                std::size_t cur = 2;
                std::uint16_t idx = 0;
                if (parse_uint_suffix(ident, cur, idx) && cur == ident.size()) {
                    node_out =
                        add_node({WseExprOp::fp_var, 0.0f, idx, 0, 0, 0, 0});
                    return true;
                }
            }
            std::uint16_t pos_idx = 0;
            std::uint8_t pos_comp = 0;
            if (match_pos_var(ident, pos_idx, pos_comp)) {
                node_out = add_node(
                    {WseExprOp::pos_var, 0.0f, pos_idx, pos_comp, 0, 0, 0});
                return true;
            }
            return false;
        }
        return false;
    }

    struct InfixToken {
        int prec = 0;
        bool right_assoc = false;
        std::size_t len = 0;
        WseExprOp op = WseExprOp::add;
    };

    [[nodiscard]] bool peek_infix(InfixToken& tok) const noexcept {
        if (pos >= src.size()) {
            return false;
        }
        const std::string_view rest = src.substr(pos);
        if (rest.starts_with("<=")) {
            tok = {6, false, 2, WseExprOp::le};
            return true;
        }
        if (rest.starts_with(">=")) {
            tok = {6, false, 2, WseExprOp::ge};
            return true;
        }
        if (rest.starts_with("!=")) {
            tok = {5, false, 2, WseExprOp::ne};
            return true;
        }
        if (rest.starts_with("==")) {
            tok = {5, false, 2, WseExprOp::eq};
            return true;
        }
        if (rest.starts_with("&&")) {
            tok = {4, false, 2, WseExprOp::log_and};
            return true;
        }
        if (rest.starts_with("||")) {
            tok = {3, false, 2, WseExprOp::log_or};
            return true;
        }
        switch (rest[0]) {
        case ',':
            tok = {0, false, 1, WseExprOp::sequence};
            return true;
        case '=':
            tok = {1, true, 1, WseExprOp::assign};
            return true;
        case '?':
            tok = {2, true, 1, WseExprOp::ternary};
            return true;
        case '<':
            tok = {6, false, 1, WseExprOp::lt};
            return true;
        case '>':
            tok = {6, false, 1, WseExprOp::gt};
            return true;
        case '+':
            tok = {7, false, 1, WseExprOp::add};
            return true;
        case '-':
            tok = {7, false, 1, WseExprOp::sub};
            return true;
        case '*':
            tok = {8, false, 1, WseExprOp::mul};
            return true;
        case '/':
            tok = {8, false, 1, WseExprOp::div};
            return true;
        case '^':
            tok = {10, true, 1, WseExprOp::pow};
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool parse_prec(int min_prec, std::size_t& lhs_out) {
        if (++depth > kMaxParseDepth) {
            return false;
        }
        struct DepthPop {
            std::size_t& d;
            ~DepthPop() noexcept { --d; }
        } pop{depth};

        if (!parse_primary(lhs_out)) {
            return false;
        }
        for (;;) {
            skip_ws();
            InfixToken tok{};
            if (!peek_infix(tok) || tok.prec < min_prec) {
                break;
            }
            pos += tok.len;
            if (tok.op == WseExprOp::ternary) {
                std::size_t then_node = 0;
                if (!parse_prec(1, then_node)) {
                    return false;
                }
                skip_ws();
                if (pos >= src.size() || src[pos] != ':') {
                    return false;
                }
                ++pos;
                std::size_t else_node = 0;
                if (!parse_prec(tok.prec, else_node)) {
                    return false;
                }
                lhs_out = add_node(
                    {WseExprOp::ternary, 0.0f, 0, 0, lhs_out, then_node,
                     else_node});
                continue;
            }
            const int next_min = tok.right_assoc ? tok.prec : tok.prec + 1;
            std::size_t rhs = 0;
            if (!parse_prec(next_min, rhs)) {
                return false;
            }
            if (tok.op == WseExprOp::assign) {
                const WseExprOp lop = nodes[lhs_out].op;
                if (lop != WseExprOp::fp_var && lop != WseExprOp::pos_var) {
                    return false;
                }
            }
            lhs_out = add_node({tok.op, 0.0f, 0, 0, lhs_out, rhs, 0});
        }
        return true;
    }

    [[nodiscard]] static float* pos_comp_ptr(
        std::span<WseTransform> positions,
        std::size_t index,
        std::uint8_t comp) noexcept {
        if (index >= positions.size()) {
            return nullptr;
        }
        WseTransform& t = positions[index];
        switch (comp) {
        case 0:
            return &t.s.x;
        case 1:
            return &t.s.y;
        case 2:
            return &t.s.z;
        case 3:
            return &t.f.x;
        case 4:
            return &t.f.y;
        case 5:
            return &t.f.z;
        case 6:
            return &t.u.x;
        case 7:
            return &t.u.y;
        case 8:
            return &t.u.z;
        case 9:
            return &t.o.x;
        case 10:
            return &t.o.y;
        case 11:
            return &t.o.z;
        default:
            return nullptr;
        }
    }

    [[nodiscard]] bool eval_node(std::size_t node_id,
                                 std::span<float> fp,
                                 std::span<WseTransform> positions,
                                 float& out_val) const noexcept {
        const WseExprNode& n = nodes[node_id];
        auto arg = [&](std::size_t idx, float& dst) noexcept -> bool {
            return eval_node(call_args[n.a + idx], fp, positions, dst);
        };
        switch (n.op) {
        case WseExprOp::literal:
            out_val = n.literal_value;
            return true;
        case WseExprOp::fp_var:
            if (n.reg_index >= fp.size()) {
                return false;
            }
            out_val = fp[n.reg_index];
            return true;
        case WseExprOp::pos_var: {
            const float* p = pos_comp_ptr(positions, n.reg_index, n.pos_comp);
            if (!p) {
                return false;
            }
            out_val = *p;
            return true;
        }
        case WseExprOp::unary_neg: {
            float v = 0.0f;
            if (!eval_node(n.a, fp, positions, v)) {
                return false;
            }
            out_val = -v;
            return true;
        }
        case WseExprOp::unary_pos:
            return eval_node(n.a, fp, positions, out_val);
        case WseExprOp::assign: {
            float rhs = 0.0f;
            if (!eval_node(n.b, fp, positions, rhs)) {
                return false;
            }
            const WseExprNode& dst = nodes[n.a];
            if (dst.op == WseExprOp::fp_var) {
                if (dst.reg_index >= fp.size()) {
                    return false;
                }
                fp[dst.reg_index] = rhs;
                out_val = rhs;
                return true;
            }
            if (dst.op == WseExprOp::pos_var) {
                float* p = pos_comp_ptr(positions, dst.reg_index, dst.pos_comp);
                if (!p) {
                    return false;
                }
                *p = rhs;
                out_val = rhs;
                return true;
            }
            return false;
        }
        case WseExprOp::sequence: {
            float unused = 0.0f;
            if (!eval_node(n.a, fp, positions, unused)) {
                return false;
            }
            return eval_node(n.b, fp, positions, out_val);
        }
        case WseExprOp::ternary: {
            float cond = 0.0f;
            if (!eval_node(n.a, fp, positions, cond)) {
                return false;
            }
            return eval_node(cond != 0.0f ? n.b : n.c, fp, positions, out_val);
        }
        case WseExprOp::log_or: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            out_val = (l != 0.0f || r != 0.0f) ? 1.0f : 0.0f;
            return true;
        }
        case WseExprOp::log_and: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            out_val = (l != 0.0f && r != 0.0f) ? 1.0f : 0.0f;
            return true;
        }
        case WseExprOp::eq:
        case WseExprOp::ne:
        case WseExprOp::lt:
        case WseExprOp::le:
        case WseExprOp::gt:
        case WseExprOp::ge:
        case WseExprOp::add:
        case WseExprOp::sub:
        case WseExprOp::mul:
        case WseExprOp::div:
        case WseExprOp::pow: {
            float l = 0.0f;
            float r = 0.0f;
            if (!eval_node(n.a, fp, positions, l) ||
                !eval_node(n.b, fp, positions, r)) {
                return false;
            }
            switch (n.op) {
            case WseExprOp::eq:
                out_val = (l == r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::ne:
                out_val = (l != r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::lt:
                out_val = (l < r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::le:
                out_val = (l <= r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::gt:
                out_val = (l > r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::ge:
                out_val = (l >= r) ? 1.0f : 0.0f;
                return true;
            case WseExprOp::add:
                out_val = l + r;
                return true;
            case WseExprOp::sub:
                out_val = l - r;
                return true;
            case WseExprOp::mul:
                out_val = l * r;
                return true;
            case WseExprOp::div:
                out_val = l / r;
                return true;
            case WseExprOp::pow:
                out_val = std::pow(l, r);
                return true;
            default:
                return false;
            }
        }
        case WseExprOp::fn_sin:
        case WseExprOp::fn_cos:
        case WseExprOp::fn_tan:
        case WseExprOp::fn_asin:
        case WseExprOp::fn_acos:
        case WseExprOp::fn_atan:
        case WseExprOp::fn_sinh:
        case WseExprOp::fn_cosh:
        case WseExprOp::fn_tanh:
        case WseExprOp::fn_asinh:
        case WseExprOp::fn_acosh:
        case WseExprOp::fn_atanh:
        case WseExprOp::fn_log2:
        case WseExprOp::fn_log10:
        case WseExprOp::fn_ln:
        case WseExprOp::fn_exp:
        case WseExprOp::fn_sqrt:
        case WseExprOp::fn_sign:
        case WseExprOp::fn_rint:
        case WseExprOp::fn_abs:
        case WseExprOp::fn_deg2rad:
        case WseExprOp::fn_rad2deg:
        case WseExprOp::fn_floor:
        case WseExprOp::fn_ceil: {
            float x = 0.0f;
            if (!arg(0, x)) {
                return false;
            }
            constexpr float kPi = 3.1415927f;
            switch (n.op) {
            case WseExprOp::fn_sin:
                out_val = std::sin(x);
                return true;
            case WseExprOp::fn_cos:
                out_val = std::cos(x);
                return true;
            case WseExprOp::fn_tan:
                out_val = std::tan(x);
                return true;
            case WseExprOp::fn_asin:
                out_val = std::asin(x);
                return true;
            case WseExprOp::fn_acos:
                out_val = std::acos(x);
                return true;
            case WseExprOp::fn_atan:
                out_val = std::atan(x);
                return true;
            case WseExprOp::fn_sinh:
                out_val = std::sinh(x);
                return true;
            case WseExprOp::fn_cosh:
                out_val = std::cosh(x);
                return true;
            case WseExprOp::fn_tanh:
                out_val = std::tanh(x);
                return true;
            case WseExprOp::fn_asinh:
                out_val = std::asinh(x);
                return true;
            case WseExprOp::fn_acosh:
                out_val = std::acosh(x);
                return true;
            case WseExprOp::fn_atanh:
                out_val = std::atanh(x);
                return true;
            case WseExprOp::fn_log2:
                out_val = std::log2(x);
                return true;
            case WseExprOp::fn_log10:
                out_val = std::log10(x);
                return true;
            case WseExprOp::fn_ln:
                out_val = std::log(x);
                return true;
            case WseExprOp::fn_exp:
                out_val = std::exp(x);
                return true;
            case WseExprOp::fn_sqrt:
                out_val = std::sqrt(x);
                return true;
            case WseExprOp::fn_sign:
                out_val = (x < 0.0f) ? -1.0f : (x > 0.0f ? 1.0f : 0.0f);
                return true;
            case WseExprOp::fn_rint:
                out_val = std::floor(x + 0.5f);
                return true;
            case WseExprOp::fn_abs:
                out_val = std::fabs(x);
                return true;
            case WseExprOp::fn_deg2rad:
                out_val = x * (kPi / 180.0f);
                return true;
            case WseExprOp::fn_rad2deg:
                out_val = x * (180.0f / kPi);
                return true;
            case WseExprOp::fn_floor:
                out_val = std::floor(x);
                return true;
            case WseExprOp::fn_ceil:
                out_val = std::ceil(x);
                return true;
            default:
                return false;
            }
        }
        case WseExprOp::fn_fmod: {
            float x = 0.0f;
            float y = 0.0f;
            if (!arg(0, x) || !arg(1, y)) {
                return false;
            }
            out_val = std::fmod(x, y);
            return true;
        }
        case WseExprOp::fn_atan2: {
            float y = 0.0f;
            float x = 0.0f;
            if (!arg(0, y) || !arg(1, x)) {
                return false;
            }
            out_val = std::atan2(y, x);
            return true;
        }
        case WseExprOp::fn_clamp: {
            float v = 0.0f;
            float lo = 0.0f;
            float hi = 0.0f;
            if (!arg(0, v) || !arg(1, lo) || !arg(2, hi)) {
                return false;
            }
            if (lo > hi) {
                std::swap(lo, hi);
            }
            out_val = v < lo ? lo : (v > hi ? hi : v);
            return true;
        }
        case WseExprOp::fn_sum:
        case WseExprOp::fn_avg:
        case WseExprOp::fn_min:
        case WseExprOp::fn_max: {
            float acc = 0.0f;
            if (!arg(0, acc)) {
                return false;
            }
            for (std::size_t i = 1; i < n.b; ++i) {
                float v = 0.0f;
                if (!arg(i, v)) {
                    return false;
                }
                if (n.op == WseExprOp::fn_min) {
                    if (v < acc) {
                        acc = v;
                    }
                } else if (n.op == WseExprOp::fn_max) {
                    if (v > acc) {
                        acc = v;
                    }
                } else {
                    acc += v;
                }
            }
            out_val = (n.op == WseExprOp::fn_avg)
                          ? (acc / static_cast<float>(n.b))
                          : acc;
            return true;
        }
        }
        return false;
    }
};

// Mirrors mu::Parser expression evaluation for feval (4437 / 0x1155): compiles
// the entire expression into AST before executing so syntax errors leave fp and
// positions untouched.
[[nodiscard]] inline bool wse_eval_float_expr(
    std::string_view expr,
    std::span<float> fp,
    std::span<WseTransform> positions) {
    WseExprCompiler compiler{};
    compiler.src = expr;
    std::size_t root = 0;
    if (!compiler.parse_prec(0, root)) {
        return false;
    }
    compiler.skip_ws();
    if (compiler.pos != compiler.src.size()) {
        return false;
    }
    float unused_result = 0.0f;
    return compiler.eval_node(root, fp, positions, unused_result);
}


}  // namespace wse::vm
