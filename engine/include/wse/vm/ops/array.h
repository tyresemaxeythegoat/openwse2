#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/vm/wse_state.h"

namespace wse::vm {

inline void wse_array_serialize_slice(const WseDynArray& arr,
                                      const WseArraySlice& slice,
                                      std::size_t depth,
                                      std::string& out) {
    if (depth + 1 < arr.dim_sizes.size()) {
        for (const auto& child : slice.children) {
            if (child) {
                wse_array_serialize_slice(arr, *child, depth + 1, out);
            }
        }
        return;
    }
    switch (arr.type) {
    case WseArrayType::integer:
        for (const std::int32_t v : slice.ints) {
            wse_append_le32(out, 4U);
            wse_append_le32(out, static_cast<std::uint32_t>(v));
        }
        break;
    case WseArrayType::string:
        for (const auto& s : slice.strings) {
            wse_append_le32(out, static_cast<std::uint32_t>(s.size()));
            out.append(s);
        }
        break;
    case WseArrayType::position:
        // 0x1401c5910 writes o.xyz, f.xyz, u.xyz (9 floats = 36 bytes).
        for (const auto& t : slice.positions) {
            wse_append_le32(out, 36U);
            wse_append_le_f32(out, t.o.x);
            wse_append_le_f32(out, t.o.y);
            wse_append_le_f32(out, t.o.z);
            wse_append_le_f32(out, t.f.x);
            wse_append_le_f32(out, t.f.y);
            wse_append_le_f32(out, t.f.z);
            wse_append_le_f32(out, t.u.x);
            wse_append_le_f32(out, t.u.y);
            wse_append_le_f32(out, t.u.z);
        }
        break;
    }
}

[[nodiscard]] inline bool wse_array_save_binary(
    wse::platform::Vfs* vfs, std::string_view rel_path, const WseDynArray& arr) {
    if (!arr.root || arr.dim_sizes.empty()) {
        return false;
    }
    std::string out;
    wse_append_le32(out, kWseArrayBinaryMagic);
    wse_append_le32(out, static_cast<std::uint32_t>(kWseArrayBinaryVersion));
    wse_append_le32(out, static_cast<std::uint32_t>(arr.type_id()));
    wse_append_le32(out, static_cast<std::uint32_t>(arr.dim_count()));
    for (const std::int32_t dim : arr.dim_sizes) {
        wse_append_le32(out, static_cast<std::uint32_t>(dim));
    }
    wse_array_serialize_slice(arr, *arr.root, 0, out);
    return wse_dict_write_file_bytes(vfs, rel_path, out);
}

[[nodiscard]] inline bool wse_array_deserialize_slice(
    WseDynArray& arr,
    WseArraySlice& slice,
    std::size_t depth,
    const std::string& bytes,
    std::size_t& offset) {
    if (depth + 1 < arr.dim_sizes.size()) {
        for (auto& child : slice.children) {
            if (!child ||
                !wse_array_deserialize_slice(arr, *child, depth + 1, bytes,
                                             offset)) {
                return false;
            }
        }
        return true;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    switch (arr.type) {
    case WseArrayType::integer:
        for (auto& dst : slice.ints) {
            if (bytes.size() - offset < 8U) {
                return false;
            }
            const std::uint32_t len = wse_read_le32(raw + offset);
            if (len != 4U) {
                return false;
            }
            dst = static_cast<std::int32_t>(wse_read_le32(raw + offset + 4U));
            offset += 8U;
        }
        return true;
    case WseArrayType::string:
        for (auto& dst : slice.strings) {
            if (bytes.size() - offset < 4U) {
                return false;
            }
            const auto slen =
                static_cast<std::int32_t>(wse_read_le32(raw + offset));
            offset += 4U;
            if (slen < 0 ||
                static_cast<std::size_t>(slen) > bytes.size() - offset) {
                return false;
            }
            const auto uslen = static_cast<std::size_t>(slen);
            dst.assign(bytes.data() + offset, uslen);
            offset += uslen;
        }
        return true;
    case WseArrayType::position:
        // 0x1401c59f0 reads o, f, u (36 bytes) and orthonormalizes.
        for (auto& dst : slice.positions) {
            if (bytes.size() - offset < 40U) {
                return false;
            }
            const std::uint32_t len = wse_read_le32(raw + offset);
            if (len != 36U) {
                return false;
            }
            offset += 4U;
            dst = wse_zero_transform();
            dst.o = {wse_read_le_f32(raw + offset + 0U),
                     wse_read_le_f32(raw + offset + 4U),
                     wse_read_le_f32(raw + offset + 8U), 1.0f};
            dst.f = {wse_read_le_f32(raw + offset + 12U),
                     wse_read_le_f32(raw + offset + 16U),
                     wse_read_le_f32(raw + offset + 20U), 0.0f};
            dst.u = {wse_read_le_f32(raw + offset + 24U),
                     wse_read_le_f32(raw + offset + 28U),
                     wse_read_le_f32(raw + offset + 32U), 0.0f};
            dst.orthonormalize();
            offset += 36U;
        }
        return true;
    }
    return false;
}

[[nodiscard]] inline bool wse_array_load_binary(
    wse::platform::Vfs* vfs,
    std::string_view rel_path,
    std::unique_ptr<WseDynArray>& out,
    bool& version_too_new,
    bool& bad_type_id) {
    version_too_new = false;
    bad_type_id = false;
    std::string bytes;
    if (!wse_dict_read_file_bytes(vfs, rel_path, bytes) || bytes.size() < 16U) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::uint32_t magic = wse_read_le32(raw);
    const auto version = static_cast<std::int32_t>(wse_read_le32(raw + 4U));
    const auto type_id = static_cast<std::int32_t>(wse_read_le32(raw + 8U));
    const auto dim_count = static_cast<std::int32_t>(wse_read_le32(raw + 12U));
    if (magic != kWseArrayBinaryMagic || version < 0) {
        return false;
    }
    if (version > kWseArrayBinaryVersion) {
        version_too_new = true;
        return false;
    }
    if (type_id < 0 || type_id > 2) {
        bad_type_id = true;
        return false;
    }
    if (dim_count < 1 || dim_count > kWseArrayMaxDims) {
        return false;
    }
    std::size_t offset = 16U;
    const std::size_t dims_bytes = static_cast<std::size_t>(dim_count) * 4U;
    if (bytes.size() - offset < dims_bytes) {
        return false;
    }
    std::vector<std::int32_t> dims(static_cast<std::size_t>(dim_count));
    for (std::int32_t d = 0; d < dim_count; ++d) {
        dims[static_cast<std::size_t>(d)] =
            static_cast<std::int32_t>(wse_read_le32(raw + offset));
        offset += 4U;
    }
    if (!WseDynArray::dims_valid(dims)) {
        return false;
    }
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t min_elem_bytes =
        type_id == 0 ? 8U : (type_id == 2 ? 40U : 4U);
    std::size_t total_elems = 1;
    for (const std::int32_t dim : dims) {
        if (dim == 0) {
            total_elems = 0;
            break;
        }
        const auto udim = static_cast<std::size_t>(dim);
        if (udim > (remaining / min_elem_bytes) / total_elems) {
            return false;
        }
        total_elems *= udim;
    }
    auto loaded =
        WseDynArray::create(static_cast<WseArrayType>(type_id), dims);
    if (!loaded || !loaded->root ||
        !wse_array_deserialize_slice(*loaded, *loaded->root, 0, bytes,
                                     offset)) {
        return false;
    }
    out = std::move(loaded);
    return true;
}

[[nodiscard]] inline int wse_array_compare_bytes_cs(
    std::string_view lhs, std::string_view rhs) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    if (common > 0) {
        const int cmp = std::memcmp(lhs.data(), rhs.data(), common);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    return lhs.size() > rhs.size() ? 1 : 0;
}

[[nodiscard]] inline int wse_array_compare_bytes_ci(
    std::string_view lhs, std::string_view rhs) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < common; ++i) {
        const int a = std::tolower(static_cast<unsigned char>(lhs[i]));
        const int b = std::tolower(static_cast<unsigned char>(rhs[i]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    return lhs.size() > rhs.size() ? 1 : 0;
}

// Mirrors 0x1401fde40 / 0x1401fddb2: stable natural merge sort over dim 0,
// aborting without mutating `arr` if not sorted within `n` double-passes.
template <typename BeforeOrEqual>
[[nodiscard]] inline bool wse_array_natural_merge_sort(
    WseDynArray& arr, BeforeOrEqual before_or_equal) {
    if (!arr.root || arr.dim_sizes.empty() || arr.dim_sizes[0] <= 1) {
        return false;
    }
    const auto n = static_cast<std::size_t>(arr.dim_sizes[0]);
    std::vector<std::size_t> buf_a(n);
    for (std::size_t i = 0; i < n; ++i) {
        buf_a[i] = i;
    }
    std::vector<std::size_t> buf_b(n);

    auto run_pass = [&](const std::vector<std::size_t>& src,
                        std::vector<std::size_t>& dst) -> bool {
        struct Run {
            std::size_t begin;
            std::size_t end;
        };
        std::vector<Run> runs;
        std::size_t start = 0;
        while (start < n) {
            std::size_t end = start + 1;
            while (end < n && before_or_equal(src[end - 1], src[end])) {
                ++end;
            }
            runs.push_back({start, end});
            start = end;
        }
        if (runs.size() <= 1U) {
            return true;
        }
        std::size_t out_idx = 0;
        for (std::size_t r = 0; r + 1 < runs.size(); r += 2) {
            std::size_t i = runs[r].begin;
            std::size_t j = runs[r + 1].begin;
            const std::size_t i_end = runs[r].end;
            const std::size_t j_end = runs[r + 1].end;
            while (i < i_end && j < j_end) {
                if (before_or_equal(src[i], src[j])) {
                    dst[out_idx++] = src[i++];
                } else {
                    dst[out_idx++] = src[j++];
                }
            }
            while (i < i_end) {
                dst[out_idx++] = src[i++];
            }
            while (j < j_end) {
                dst[out_idx++] = src[j++];
            }
        }
        if ((runs.size() & 1U) != 0U) {
            const Run last = runs.back();
            for (std::size_t i = last.begin; i < last.end; ++i) {
                dst[out_idx++] = src[i];
            }
        }
        return false;
    };

    const std::vector<std::size_t>* sorted = nullptr;
    if (run_pass(buf_a, buf_b)) {
        sorted = &buf_a;
    } else {
        for (std::size_t double_passes = 0;;) {
            if (run_pass(buf_b, buf_a)) {
                sorted = &buf_b;
                break;
            }
            if (++double_passes >= n) {
                return false;
            }
            if (run_pass(buf_a, buf_b)) {
                sorted = &buf_a;
                break;
            }
        }
    }

    const auto& order = *sorted;
    if (arr.dim_sizes.size() > 1U) {
        std::vector<std::unique_ptr<WseArraySlice>> reordered(n);
        for (std::size_t i = 0; i < n; ++i) {
            reordered[i] = std::move(arr.root->children[order[i]]);
        }
        arr.root->children = std::move(reordered);
    } else {
        switch (arr.type) {
        case WseArrayType::integer: {
            std::vector<std::int32_t> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = arr.root->ints[order[i]];
            }
            arr.root->ints = std::move(reordered);
            break;
        }
        case WseArrayType::string: {
            std::vector<std::string> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = std::move(arr.root->strings[order[i]]);
            }
            arr.root->strings = std::move(reordered);
            break;
        }
        case WseArrayType::position: {
            std::vector<WseTransform> reordered(n);
            for (std::size_t i = 0; i < n; ++i) {
                reordered[i] = arr.root->positions[order[i]];
            }
            arr.root->positions = std::move(reordered);
            break;
        }
        }
    }
    return true;
}

[[nodiscard]] inline bool wse_array_validate_sort_subdims(
    const WseDynArray& arr,
    std::span<const std::int32_t> subdims) noexcept {
    if (arr.dim_sizes.empty() || arr.dim_sizes[0] <= 1 ||
        subdims.size() + 1 != arr.dim_sizes.size()) {
        return false;
    }
    for (std::size_t k = 0; k < subdims.size(); ++k) {
        if (subdims[k] < 0 || subdims[k] >= arr.dim_sizes[k + 1]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool wse_array_sort_builtin(
    WseDynArray& arr,
    std::int32_t mode,
    std::span<const std::int32_t> subdims) {
    if (!wse_array_validate_sort_subdims(arr, subdims)) {
        return false;
    }
    if (arr.type == WseArrayType::integer) {
        if (mode != 0 && mode != 1) {
            return false;
        }
        const bool desc = mode == 1;
        return wse_array_natural_merge_sort(
            arr, [&](std::size_t row_a, std::size_t row_b) noexcept {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = arr.slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = arr.slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return true;
                }
                const std::int32_t va = sa->ints[leaf_a];
                const std::int32_t vb = sb->ints[leaf_b];
                return desc ? (va >= vb) : (va <= vb);
            });
    }
    if (arr.type == WseArrayType::string) {
        if (mode < 0 || mode > 3) {
            return false;
        }
        const bool desc = (mode & 1) != 0;
        const bool ci = (mode & 2) != 0;
        return wse_array_natural_merge_sort(
            arr, [&](std::size_t row_a, std::size_t row_b) noexcept {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = arr.slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = arr.slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return true;
                }
                const int cmp =
                    ci ? wse_array_compare_bytes_ci(sa->strings[leaf_a],
                                                    sb->strings[leaf_b])
                       : wse_array_compare_bytes_cs(sa->strings[leaf_a],
                                                    sb->strings[leaf_b]);
                return desc ? (cmp >= 0) : (cmp <= 0);
            });
    }
    return false;
}

[[nodiscard]] inline WseDispatch wse_array_compare_op(
    const WseOperands& operands, std::int32_t cmp_mode) {
    if (operands.narrow.size() < 3) {
        return WseDispatch::operand_count_out_of_range;
    }
    const WseDynArray* arr =
        operands.state.dictionaries != nullptr
            ? operands.state.dictionaries->get_array(operands.narrow[0])
            : nullptr;
    if (!arr) {
        return WseDispatch::condition_false;
    }
    std::size_t leaf_idx = 0;
    const WseArraySlice* leaf =
        arr->leaf_for(operands.narrow.subspan(2), leaf_idx);
    if (!leaf) {
        return WseDispatch::condition_false;
    }
    auto eval_ordered = [cmp_mode](int rel) noexcept -> bool {
        switch (cmp_mode) {
        case 0:
            return rel == 0;
        case 1:
            return rel != 0;
        case 2:
            return rel > 0;
        case 3:
            return rel >= 0;
        case 4:
            return rel < 0;
        case 5:
            return rel <= 0;
        default:
            return false;
        }
    };
    switch (arr->type) {
    case WseArrayType::integer: {
        const std::int32_t lhs = leaf->ints[leaf_idx];
        const std::int32_t rhs = operands.narrow[1];
        const int rel = (lhs < rhs) ? -1 : (lhs > rhs ? 1 : 0);
        return eval_ordered(rel) ? WseDispatch::handled_no_store
                                 : WseDispatch::condition_false;
    }
    case WseArrayType::string: {
        std::string rhs;
        if (!wse_resolve_parsed_utf8_operand(1, operands, rhs)) {
            return WseDispatch::condition_false;
        }
        const int rel = wse_array_compare_bytes_cs(leaf->strings[leaf_idx], rhs);
        return eval_ordered(rel) ? WseDispatch::handled_no_store
                                 : WseDispatch::condition_false;
    }
    case WseArrayType::position: {
        if ((cmp_mode != 0 && cmp_mode != 1) ||
            !wse_register_ok(operands.narrow[1]) ||
            static_cast<std::size_t>(operands.narrow[1]) >=
                operands.state.positions.size()) {
            return WseDispatch::condition_false;
        }
        const bool eq = wse_transforms_equal(
            leaf->positions[leaf_idx],
            operands.state.positions[static_cast<std::size_t>(
                operands.narrow[1])]);
        const bool ok = cmp_mode == 0 ? eq : !eq;
        return ok ? WseDispatch::handled_no_store
                  : WseDispatch::condition_false;
    }
    }
    return WseDispatch::condition_false;
}

// Mirrors 0x14030f270 (rglString::spacesToUnderscores): in-place ' ' -> '_'.

[[nodiscard]] inline WseDispatch execute_wse_array(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto narrow = operands.narrow;
    const auto registers = operands.registers;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    // Dynamic array group (5000..5022 / 0x1388..0x139e).
    case WseOpcode::array_create: {
        // 5000 (0x1388): (dst, type_id, dim0, [dim1..dim13]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        const std::int32_t type_id = narrow[1];
        const auto dims = narrow.subspan(2);
        if (type_id < 0 || type_id > 2 || !WseDynArray::dims_valid(dims)) {
            effect.value = 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        auto arr =
            WseDynArray::create(static_cast<WseArrayType>(type_id), dims);
        effect.value = arr ? static_cast<std::int64_t>(
                                 operands.state.dictionaries->create_array(
                                     std::move(arr)))
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_free: {
        // 5001 (0x1389): (array_id).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries != nullptr) {
            operands.state.dictionaries->destroy_array(narrow[0]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_copy: {
        // 5002 (0x138a): (dst, src_array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* src =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!src) {
            return WseDispatch::handled_no_store;
        }
        auto copy = src->clone();
        effect.value = copy ? static_cast<std::int64_t>(
                                  operands.state.dictionaries->create_array(
                                      std::move(copy)))
                            : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_save_file: {
        // 5003 (0x138b): (array_id, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        std::string target;
        if (arr && wse_dict_target_path(operands.state.dictionaries, file_name,
                                        ".wsearray", target)) {
            (void)wse_array_save_binary(operands.state.dictionaries->vfs,
                                        target, *arr);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_load_file: {
        // 5004 (0x138c): (dst, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::string target;
        if (!wse_dict_target_path(operands.state.dictionaries, file_name,
                                  ".wsearray", target)) {
            return WseDispatch::handled_no_store;
        }
        std::unique_ptr<WseDynArray> loaded;
        bool version_too_new = false;
        bool bad_type_id = false;
        if (!wse_array_load_binary(operands.state.dictionaries->vfs, target,
                                   loaded, version_too_new, bad_type_id)) {
            if (bad_type_id) {
                return WseDispatch::handled_no_store;
            }
            effect.value = 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        effect.value = static_cast<std::int64_t>(
            operands.state.dictionaries->create_array(std::move(loaded)));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_delete_file: {
        // 5005 (0x138d): (file_str).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_parsed_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::string target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 ".wsearray", target)) {
            (void)operands.state.dictionaries->vfs->remove_file(target);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_set_val: {
        // 5006 (0x138e): (array_id, value, index0..13).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        std::string str_val;
        if (arr->type == WseArrayType::string) {
            if (!wse_resolve_parsed_utf8_operand(1, operands, str_val)) {
                return WseDispatch::ignored;
            }
        } else if (arr->type == WseArrayType::position) {
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
        }
        std::size_t leaf_idx = 0;
        WseArraySlice* leaf = arr->leaf_for(narrow.subspan(2), leaf_idx);
        if (!leaf) {
            return WseDispatch::handled_no_store;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            leaf->ints[leaf_idx] = narrow[1];
            break;
        case WseArrayType::string:
            leaf->strings[leaf_idx] = std::move(str_val);
            break;
        case WseArrayType::position:
            leaf->positions[leaf_idx] =
                operands.state.positions[static_cast<std::size_t>(narrow[1])];
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_set_val_all: {
        // 5007 (0x138f): (array_id, value).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!arr || !arr->root) {
            return WseDispatch::handled_no_store;
        }
        std::int32_t int_val = 0;
        std::string str_val;
        WseTransform pos_val{};
        switch (arr->type) {
        case WseArrayType::integer:
            int_val = narrow[1];
            break;
        case WseArrayType::string:
            if (!wse_resolve_parsed_utf8_operand(1, operands, str_val)) {
                return WseDispatch::ignored;
            }
            break;
        case WseArrayType::position:
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            pos_val =
                operands.state.positions[static_cast<std::size_t>(narrow[1])];
            break;
        }
        arr->root->fill_all(arr->type, 0, arr->dim_sizes.size(), int_val,
                            str_val, pos_val);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_get_val: {
        // 5008 (0x1390): (dst, array_id, index0..13).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        if ((arr->type == WseArrayType::string ||
             arr->type == WseArrayType::position) &&
            !wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::size_t leaf_idx = 0;
        const WseArraySlice* leaf = arr->leaf_for(narrow.subspan(2), leaf_idx);
        if (!leaf) {
            return WseDispatch::handled_no_store;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            effect.value = static_cast<std::int64_t>(leaf->ints[leaf_idx]);
            effect.store_value = true;
            return WseDispatch::handled;
        case WseArrayType::string:
            return wse_store_dict_string(strings, narrow[0],
                                         leaf->strings[leaf_idx]);
        case WseArrayType::position:
            if (static_cast<std::size_t>(narrow[0]) >=
                operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            operands.state.positions[static_cast<std::size_t>(narrow[0])] =
                leaf->positions[leaf_idx];
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_push: {
        // 5009 (0x1391): (array_id, src_val_or_subarray).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* dst =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (!dst || !dst->root) {
            return WseDispatch::handled_no_store;
        }
        if (dst->dim_sizes.size() > 1U) {
            const WseDynArray* src =
                operands.state.dictionaries->get_array(narrow[1]);
            if (src) {
                (void)dst->push_subarray(*src);
            }
            return WseDispatch::handled_no_store;
        }
        switch (dst->type) {
        case WseArrayType::integer:
            dst->root->ints.push_back(narrow[1]);
            dst->dim_sizes[0] = static_cast<std::int32_t>(dst->root->ints.size());
            break;
        case WseArrayType::string: {
            std::string val;
            if (!wse_resolve_parsed_utf8_operand(1, operands, val)) {
                return WseDispatch::ignored;
            }
            dst->root->strings.push_back(std::move(val));
            dst->dim_sizes[0] =
                static_cast<std::int32_t>(dst->root->strings.size());
            break;
        }
        case WseArrayType::position:
            if (!wse_register_ok(narrow[1]) ||
                static_cast<std::size_t>(narrow[1]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            dst->root->positions.push_back(
                operands.state.positions[static_cast<std::size_t>(narrow[1])]);
            dst->dim_sizes[0] =
                static_cast<std::int32_t>(dst->root->positions.size());
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_pop: {
        // 5010 (0x1392): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr || !arr->root) {
            return WseDispatch::handled_no_store;
        }
        if (arr->dim_sizes.size() > 1U) {
            auto sub = arr->pop_subarray();
            if (!sub) {
                return WseDispatch::handled_no_store;
            }
            effect.value = static_cast<std::int64_t>(
                operands.state.dictionaries->create_array(std::move(sub)));
            effect.store_value = true;
            return WseDispatch::handled;
        }
        switch (arr->type) {
        case WseArrayType::integer:
            if (arr->root->ints.empty()) {
                return WseDispatch::handled_no_store;
            }
            effect.value = static_cast<std::int64_t>(arr->root->ints.back());
            arr->root->ints.pop_back();
            arr->dim_sizes[0] = static_cast<std::int32_t>(arr->root->ints.size());
            effect.store_value = true;
            return WseDispatch::handled;
        case WseArrayType::string: {
            if (!wse_register_ok(narrow[0])) {
                return WseDispatch::ignored;
            }
            if (arr->root->strings.empty()) {
                return WseDispatch::handled_no_store;
            }
            std::string popped = std::move(arr->root->strings.back());
            if (!wse_store_string(strings, narrow[0],
                                  wse_utf8_to_utf16(popped))) {
                arr->root->strings.back() = std::move(popped);
                return WseDispatch::ignored;
            }
            arr->root->strings.pop_back();
            arr->dim_sizes[0] =
                static_cast<std::int32_t>(arr->root->strings.size());
            return WseDispatch::handled_no_store;
        }
        case WseArrayType::position:
            if (!wse_register_ok(narrow[0]) ||
                static_cast<std::size_t>(narrow[0]) >=
                    operands.state.positions.size()) {
                return WseDispatch::ignored;
            }
            if (arr->root->positions.empty()) {
                return WseDispatch::handled_no_store;
            }
            operands.state.positions[static_cast<std::size_t>(narrow[0])] =
                arr->root->positions.back();
            arr->root->positions.pop_back();
            arr->dim_sizes[0] =
                static_cast<std::int32_t>(arr->root->positions.size());
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_resize_dim: {
        // 5011 (0x1393): (array_id, dim_index, size >= 1).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (arr) {
            (void)arr->resize_dim(narrow[1], narrow[2]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_get_dim_size: {
        // 5012 (0x1394): (dst, array_id, dim_index).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->dim_size(narrow[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_get_dim_count: {
        // 5013 (0x1395): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->dim_count());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_get_type_id: {
        // 5014 (0x1396): (dst, array_id).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[1])
                : nullptr;
        if (!arr) {
            return WseDispatch::handled_no_store;
        }
        effect.value = static_cast<std::int64_t>(arr->type_id());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::array_sort: {
        // 5015 (0x1397): (array_id, mode, [idx1..idx13]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        if (arr) {
            (void)wse_array_sort_builtin(*arr, narrow[1], narrow.subspan(2));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_sort_custom: {
        // 5016 (0x1398): (array_id, cmp_script, [idx1..idx13]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDynArray* arr =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get_array(narrow[0])
                : nullptr;
        const std::int32_t cmp_script = narrow[1];
        const auto subdims = narrow.subspan(2);
        if (!arr || !wse_array_validate_sort_subdims(*arr, subdims) ||
            cmp_script < 0 ||
            static_cast<std::size_t>(cmp_script) >=
                operands.state.num_scripts ||
            operands.state.execute_script_fn == nullptr ||
            operands.state.num_return_values_out == nullptr ||
            operands.state.return_values_out.empty()) {
            return WseDispatch::handled_no_store;
        }
        const bool banks_ok =
            (arr->type == WseArrayType::integer && registers.size() >= 2U) ||
            (arr->type == WseArrayType::string &&
             strings.registers.size() >= 2U) ||
            (arr->type == WseArrayType::position &&
             operands.state.positions.size() >= 2U);
        if (!banks_ok) {
            return WseDispatch::handled_no_store;
        }
        // Safety deviation: the comparator is an arbitrary module script and
        // may free, resize, or pop the array being sorted. Sort a detached
        // copy so no pointer into the live object is held across a callback,
        // then install the result only if the id still names an array of the
        // same type and shape. Row indices passed to the script are positions
        // in the unsorted array, which stays in place until the sort finishes.
        const std::int32_t array_id = narrow[0];
        const std::unique_ptr<WseDynArray> work = arr->clone();
        arr = nullptr;
        if (!work || !work->root) {
            return WseDispatch::handled_no_store;
        }
        const bool sorted = wse_array_natural_merge_sort(
            *work, [&](std::size_t row_a, std::size_t row_b) -> bool {
                std::size_t leaf_a = 0;
                std::size_t leaf_b = 0;
                const auto* sa = work->slice_along_dim0(row_a, subdims, leaf_a);
                const auto* sb = work->slice_along_dim0(row_b, subdims, leaf_b);
                if (!sa || !sb) {
                    return false;
                }
                switch (work->type) {
                case WseArrayType::integer:
                    registers[0] =
                        static_cast<std::int64_t>(sa->ints[leaf_a]);
                    registers[1] =
                        static_cast<std::int64_t>(sb->ints[leaf_b]);
                    break;
                case WseArrayType::string:
                    strings.registers[0] =
                        wse_utf8_to_utf16(sa->strings[leaf_a]);
                    strings.registers[1] =
                        wse_utf8_to_utf16(sb->strings[leaf_b]);
                    break;
                case WseArrayType::position:
                    operands.state.positions[0] = sa->positions[leaf_a];
                    operands.state.positions[1] = sb->positions[leaf_b];
                    break;
                }
                *operands.state.num_return_values_out = 0;
                const std::array<std::int64_t, 2> params = {
                    static_cast<std::int64_t>(row_a),
                    static_cast<std::int64_t>(row_b)};
                (void)operands.state.execute_script_fn(
                    operands.state.execute_script_user, cmp_script, params);
                return *operands.state.num_return_values_out == 1 &&
                       operands.state.return_values_out[0] != 0;
            });
        if (sorted) {
            WseDynArray* live =
                operands.state.dictionaries->get_array(array_id);
            if (live && live->root && live->type == work->type &&
                live->dim_sizes == work->dim_sizes) {
                live->root = std::move(work->root);
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::array_eq:
        return wse_array_compare_op(operands, 0);
    case WseOpcode::array_neq:
        return wse_array_compare_op(operands, 1);
    case WseOpcode::array_gt:
        return wse_array_compare_op(operands, 2);
    case WseOpcode::array_ge:
        return wse_array_compare_op(operands, 3);
    case WseOpcode::array_lt:
        return wse_array_compare_op(operands, 4);
    case WseOpcode::array_le:
        return wse_array_compare_op(operands, 5);

    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
