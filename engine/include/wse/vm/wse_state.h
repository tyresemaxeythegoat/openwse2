#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/core/json.h"
#include "wse/core/math.h"
#include "wse/game/world_state.h"
#include "wse/platform/clock.h"
#include "wse/platform/vfs.h"
#include "wse/vm/lua_runtime.h"
#include "wse/vm/wse_opcodes.h"

namespace wse::vm {

using wse::core::WseMersenneTwister;
using wse::core::WseTransform;
using wse::core::WseVec4;
using wse::core::wse_append_json_string;
using wse::core::wse_append_le32;
using wse::core::wse_append_le_f32;
using wse::core::wse_append_utf8_codepoint;
using wse::core::wse_check_path;
using wse::core::wse_days_from_civil;
using wse::core::wse_fold_case;
using wse::core::wse_fold_string;
using wse::core::wse_format_float;
using wse::core::wse_hex_nibble;
using wse::core::wse_is_ascii_alpha;
using wse::core::wse_is_ascii_digit;
using wse::core::wse_is_ascii_space;
using wse::core::wse_is_space;
using wse::core::wse_md5_hex;
using wse::core::wse_parse_hex4;
using wse::core::wse_parse_json_object;
using wse::core::wse_parse_json_scalar;
using wse::core::wse_parse_json_string;
using wse::core::wse_read_le32;
using wse::core::wse_read_le_f32;
using wse::core::wse_replace_all;
using wse::core::wse_round64;
using wse::core::wse_sanitize_string;
using wse::core::wse_serialize_json_object;
using wse::core::wse_skip_json_ws;
using wse::core::wse_spaces_to_underscores;
using wse::core::wse_string_compare;
using wse::core::wse_string_contains;
using wse::core::wse_string_ends_with;
using wse::core::wse_string_equals;
using wse::core::wse_string_starts_with;
using wse::core::wse_transforms_equal;
using wse::core::wse_trim;
using wse::core::wse_trim_ascii;
using wse::core::wse_trim_left;
using wse::core::wse_trim_right;
using wse::core::wse_trunc64;
using wse::core::wse_upper_case;
using wse::core::wse_url_escape;
using wse::core::wse_url_unescape;
using wse::core::wse_utf16_to_utf8;
using wse::core::wse_utf8_to_utf16;
using wse::core::wse_wtof;
using wse::core::wse_zero_transform;

inline constexpr std::uint32_t kWseOpcodeMask = 0x0FFFFFFFu;

// mbCheckRegister accepts unsigned indices 0..0x7f.
inline constexpr std::size_t kFpRegisterCount = 128;

// DAT_140663be8, fixed-point values per unit (set to 100 at 0x14007115a).
inline constexpr float kWseFixedPointScale = 100.0f;

// String banks addressed by the str_* handlers. The spans hold views into the
// runtime's string registers; a missing entry resolves to the empty string,
// matching the static empty rgl::string returned by 0x140219df0. Tag 0 is
// writable because str_store_* and str_split/str_sort assign into it.
struct WseStringBanks {
    // Tag 0: the 128-entry string register table at 0x140666060 (stride 0x48).
    // Script literals live here too; str_store_* overwrite entries.
    std::span<std::u16string> registers{};
    // Tag 3 (and unknown tags): [0x1406227a0], count [0x1406227b0].
    std::span<const std::u16string_view> global{};
    // Tag 0x16: [0x1406227a8], count [0x1406227b4].
    std::span<const std::u16string_view> local{};
};

// Runtime option values queried and mutated by native opcodes 260..271
// and WSE opcodes 4300..4305, 3619, 3622, and 4606.
struct WseOptionsState {
    bool verbose_casualties = false;   // 0x1406183b7
    bool cheat_mode = false;           // 0x14061ff51
    bool realistic_headshots = false;  // 0x14061ff3a
    bool show_crosshair = true;        // 0x14061881a
    bool horse_friendly_fire = false;  // 0x14061ff1b
    bool edit_mode_enabled = false;    // 0x14061ffb9
    float damage_to_player = 1.0f;     // rglConfig::Battle::fDamageToSelf
    float damage_to_friends = 1.0f;    // rglConfig::Battle::fDamageToFriends
    std::int32_t combat_ai = 1;        // rglConfig::BattleAi::iReduceBattleAi
    std::int32_t campaign_ai = 1;      // rglConfig::CampaignAi::iReduceCampaignAi
    std::int32_t combat_speed = 2;     // rglConfig::Battle::iCombatSpeed
    float battle_size = 1.0f;          // rglConfig::Battle::fBattleSize
};

// 16-byte timer slot (0x140668b48 / 0x140668b50, stride 0x10).
// Ticks run at 100,000 Hz (0x1401e5faf divides delta by 100000.0f -> seconds).
struct WseAppTimer {
    std::int32_t kind = 0;
    std::int64_t start_ticks = 0;
};

// Ordered UTF-8 string map mirroring rglSectionlessIni.
struct WseDictionary {
    std::map<std::string, std::string> entries{};

    [[nodiscard]] bool empty() const noexcept {
        return entries.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries.size();
    }

    [[nodiscard]] bool has_key(const std::string& key) const noexcept {
        return entries.find(key) != entries.end();
    }

    void clear() noexcept {
        entries.clear();
    }

    void set(const std::string& key, std::string value) {
        entries[key] = std::move(value);
    }

    void merge(const std::map<std::string, std::string>& source,
               bool overwrite) {
        for (const auto& [key, value] : source) {
            if (overwrite || entries.find(key) == entries.end()) {
                entries[key] = value;
            }
        }
    }
};

enum class WseArrayType : std::int32_t {
    integer = 0,
    string = 1,
    position = 2,
};

inline constexpr std::int32_t kWseArrayMaxDims = 14;
// Safety deviation: retail hands any dimension product to the allocator. The
// port refuses arrays above this many leaf elements so a script (or a
// .wsearray header) cannot exhaust memory with one statement.
inline constexpr std::uint64_t kWseArrayMaxElements = 1ULL << 24;
inline constexpr std::uint32_t kWseArrayBinaryMagic = 0x57534541U;
inline constexpr std::int32_t kWseArrayBinaryVersion = 1;

// Recursive slice storage backing 1..14D typed dynamic arrays
// (WSEBasicDynMultiArray / WSEDynMultiArray<T>).
struct WseArraySlice {
    std::vector<std::unique_ptr<WseArraySlice>> children{};
    std::vector<std::int32_t> ints{};
    std::vector<std::string> strings{};
    std::vector<WseTransform> positions{};

    [[nodiscard]] std::unique_ptr<WseArraySlice> clone(
        WseArrayType type, std::size_t depth, std::size_t dim_count) const {
        auto copy = std::make_unique<WseArraySlice>();
        if (depth + 1 < dim_count) {
            copy->children.reserve(children.size());
            for (const auto& child : children) {
                copy->children.push_back(
                    child ? child->clone(type, depth + 1, dim_count)
                          : nullptr);
            }
            return copy;
        }
        switch (type) {
        case WseArrayType::integer:
            copy->ints = ints;
            break;
        case WseArrayType::string:
            copy->strings = strings;
            break;
        case WseArrayType::position:
            copy->positions = positions;
            break;
        }
        return copy;
    }

    void fill_all(WseArrayType type,
                  std::size_t depth,
                  std::size_t dim_count,
                  std::int32_t int_val,
                  const std::string& str_val,
                  const WseTransform& pos_val) {
        if (depth + 1 < dim_count) {
            for (auto& child : children) {
                if (child) {
                    child->fill_all(type, depth + 1, dim_count, int_val,
                                    str_val, pos_val);
                }
            }
            return;
        }
        switch (type) {
        case WseArrayType::integer:
            std::fill(ints.begin(), ints.end(), int_val);
            break;
        case WseArrayType::string:
            std::fill(strings.begin(), strings.end(), str_val);
            break;
        case WseArrayType::position:
            std::fill(positions.begin(), positions.end(), pos_val);
            break;
        }
    }
};

struct WseDynArray {
    WseArrayType type = WseArrayType::integer;
    std::vector<std::int32_t> dim_sizes{};
    std::unique_ptr<WseArraySlice> root{};

    [[nodiscard]] static bool dims_valid(
        std::span<const std::int32_t> dims) noexcept {
        if (dims.empty() ||
            static_cast<std::int32_t>(dims.size()) > kWseArrayMaxDims) {
            return false;
        }
        if (dims[0] < 0) {
            return false;
        }
        for (std::size_t i = 1; i < dims.size(); ++i) {
            if (dims[i] < 1) {
                return false;
            }
        }
        return element_count_ok(dims);
    }

    // Product of all dimensions stays within kWseArrayMaxElements.
    [[nodiscard]] static bool element_count_ok(
        std::span<const std::int32_t> dims) noexcept {
        std::uint64_t total = 1;
        for (const std::int32_t dim : dims) {
            if (dim <= 0) {
                return dim == 0;
            }
            total *= static_cast<std::uint64_t>(dim);
            if (total > kWseArrayMaxElements) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::unique_ptr<WseArraySlice> make_slice(
        WseArrayType type, std::span<const std::int32_t> dims) {
        if (dims.empty()) {
            return nullptr;
        }
        auto slice = std::make_unique<WseArraySlice>();
        const auto count = static_cast<std::size_t>(dims[0]);
        if (dims.size() == 1) {
            switch (type) {
            case WseArrayType::integer:
                slice->ints.resize(count, 0);
                break;
            case WseArrayType::string:
                slice->strings.resize(count);
                break;
            case WseArrayType::position:
                slice->positions.resize(count, wse_zero_transform());
                break;
            }
            return slice;
        }
        slice->children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            slice->children.push_back(make_slice(type, dims.subspan(1)));
        }
        return slice;
    }

    [[nodiscard]] static std::unique_ptr<WseDynArray> create(
        WseArrayType type, std::span<const std::int32_t> dims) {
        if (!dims_valid(dims)) {
            return nullptr;
        }
        auto arr = std::make_unique<WseDynArray>();
        arr->type = type;
        arr->dim_sizes.assign(dims.begin(), dims.end());
        arr->root = make_slice(type, arr->dim_sizes);
        return arr;
    }

    [[nodiscard]] std::unique_ptr<WseDynArray> clone() const {
        auto copy = std::make_unique<WseDynArray>();
        copy->type = type;
        copy->dim_sizes = dim_sizes;
        copy->root =
            root ? root->clone(type, 0, dim_sizes.size()) : nullptr;
        return copy;
    }

    [[nodiscard]] std::int32_t type_id() const noexcept {
        return static_cast<std::int32_t>(type);
    }

    [[nodiscard]] std::int32_t dim_count() const noexcept {
        return static_cast<std::int32_t>(dim_sizes.size());
    }

    [[nodiscard]] std::int32_t dim_size(
        std::int32_t dim_index) const noexcept {
        if (dim_index < 0 ||
            static_cast<std::size_t>(dim_index) >= dim_sizes.size()) {
            return -1;
        }
        return dim_sizes[static_cast<std::size_t>(dim_index)];
    }

    [[nodiscard]] const WseArraySlice* leaf_for(
        std::span<const std::int32_t> indices,
        std::size_t& last_index) const noexcept {
        if (!root || indices.size() != dim_sizes.size()) {
            return nullptr;
        }
        for (std::size_t d = 0; d < dim_sizes.size(); ++d) {
            if (indices[d] < 0 || indices[d] >= dim_sizes[d]) {
                return nullptr;
            }
        }
        const WseArraySlice* cur = root.get();
        for (std::size_t d = 0; d + 1 < dim_sizes.size(); ++d) {
            const auto idx = static_cast<std::size_t>(indices[d]);
            if (idx >= cur->children.size() || !cur->children[idx]) {
                return nullptr;
            }
            cur = cur->children[idx].get();
        }
        last_index = static_cast<std::size_t>(indices.back());
        return cur;
    }

    [[nodiscard]] WseArraySlice* leaf_for(
        std::span<const std::int32_t> indices,
        std::size_t& last_index) noexcept {
        return const_cast<WseArraySlice*>(
            std::as_const(*this).leaf_for(indices, last_index));
    }

    [[nodiscard]] WseArraySlice* slice_along_dim0(
        std::size_t row,
        std::span<const std::int32_t> tail_indices,
        std::size_t& last_index) noexcept {
        if (!root || tail_indices.size() + 1 != dim_sizes.size() ||
            row >= static_cast<std::size_t>(dim_sizes[0])) {
            return nullptr;
        }
        if (dim_sizes.size() == 1) {
            last_index = row;
            return root.get();
        }
        for (std::size_t k = 0; k < tail_indices.size(); ++k) {
            if (tail_indices[k] < 0 || tail_indices[k] >= dim_sizes[k + 1]) {
                return nullptr;
            }
        }
        if (row >= root->children.size() || !root->children[row]) {
            return nullptr;
        }
        WseArraySlice* cur = root->children[row].get();
        for (std::size_t k = 0; k + 1 < tail_indices.size(); ++k) {
            const auto idx = static_cast<std::size_t>(tail_indices[k]);
            if (idx >= cur->children.size() || !cur->children[idx]) {
                return nullptr;
            }
            cur = cur->children[idx].get();
        }
        last_index = static_cast<std::size_t>(tail_indices.back());
        return cur;
    }

    [[nodiscard]] const WseArraySlice* slice_along_dim0(
        std::size_t row,
        std::span<const std::int32_t> tail_indices,
        std::size_t& last_index) const noexcept {
        return const_cast<WseDynArray*>(this)->slice_along_dim0(
            row, tail_indices, last_index);
    }

    [[nodiscard]] bool resize_dim(std::int32_t dim_index,
                                  std::int32_t new_size) {
        if (!root || dim_index < 0 ||
            static_cast<std::size_t>(dim_index) >= dim_sizes.size() ||
            new_size < 1) {
            return false;
        }
        const auto target_dim = static_cast<std::size_t>(dim_index);
        std::vector<std::int32_t> resized = dim_sizes;
        resized[target_dim] = new_size;
        if (!element_count_ok(resized)) {
            return false;
        }
        dim_sizes[target_dim] = new_size;
        resize_recursive(*root, 0, target_dim, static_cast<std::size_t>(new_size));
        return true;
    }

    [[nodiscard]] bool push_subarray(const WseDynArray& src) {
        if (!root || dim_sizes.size() <= 1 || src.type != type ||
            src.dim_sizes.size() + 1 != dim_sizes.size() || !src.root) {
            return false;
        }
        for (std::size_t k = 0; k < src.dim_sizes.size(); ++k) {
            if (src.dim_sizes[k] != dim_sizes[k + 1]) {
                return false;
            }
        }
        root->children.push_back(
            src.root->clone(type, 1, dim_sizes.size()));
        dim_sizes[0] = static_cast<std::int32_t>(root->children.size());
        return true;
    }

    [[nodiscard]] std::unique_ptr<WseDynArray> pop_subarray() {
        if (!root || dim_sizes.size() <= 1 || root->children.empty()) {
            return nullptr;
        }
        auto out = std::make_unique<WseDynArray>();
        out->type = type;
        out->dim_sizes.assign(dim_sizes.begin() + 1, dim_sizes.end());
        out->root = std::move(root->children.back());
        root->children.pop_back();
        dim_sizes[0] = static_cast<std::int32_t>(root->children.size());
        return out;
    }

private:
    void resize_recursive(WseArraySlice& slice,
                          std::size_t depth,
                          std::size_t target_dim,
                          std::size_t new_size) {
        if (depth + 1 == dim_sizes.size()) {
            switch (type) {
            case WseArrayType::integer:
                slice.ints.resize(new_size, 0);
                break;
            case WseArrayType::string:
                slice.strings.resize(new_size);
                break;
            case WseArrayType::position:
                slice.positions.resize(new_size, wse_zero_transform());
                break;
            }
            return;
        }
        if (depth == target_dim) {
            const std::size_t old_size = slice.children.size();
            slice.children.resize(new_size);
            const auto child_dims =
                std::span<const std::int32_t>(dim_sizes).subspan(depth + 1);
            for (std::size_t i = old_size; i < new_size; ++i) {
                slice.children[i] = make_slice(type, child_dims);
            }
            return;
        }
        for (auto& child : slice.children) {
            if (child) {
                resize_recursive(*child, depth + 1, target_dim, new_size);
            }
        }
    }
};

enum class WseObjectKind : std::uint8_t {
    empty = 0,
    dictionary,
    array,
};

struct WseObjectSlot {
    WseObjectKind kind = WseObjectKind::empty;
    std::unique_ptr<WseDictionary> dictionary{};
    std::unique_ptr<WseDynArray> array{};
};

// Unified handle allocator mirroring WSEObjectManager
// (0x140699ac0, 0x14034f100/0x14034f170, 0x140073900/0x140073970) using LIFO free-ID reuse
// shared by dictionaries (3200..3222) and dynamic arrays (5000..5022).
struct WseObjectManager {
    std::vector<WseObjectSlot> slots{};
    std::vector<std::int32_t> free_ids{};
    // Storage root for the .wsedict/.ini/.json/.wsearray file opcodes; they
    // are no-ops while this is null. Not owned: the embedder keeps the Vfs
    // alive for as long as this manager is used.
    wse::platform::Vfs* vfs = nullptr;

    [[nodiscard]] std::int32_t create() {
        WseObjectSlot slot{};
        slot.kind = WseObjectKind::dictionary;
        slot.dictionary = std::make_unique<WseDictionary>();
        return alloc_slot(std::move(slot));
    }

    [[nodiscard]] std::int32_t create_array(std::unique_ptr<WseDynArray> arr) {
        if (!arr) {
            return -1;
        }
        WseObjectSlot slot{};
        slot.kind = WseObjectKind::array;
        slot.array = std::move(arr);
        return alloc_slot(std::move(slot));
    }

    void destroy(std::int32_t id) noexcept {
        release_slot(id, WseObjectKind::dictionary);
    }

    void destroy_array(std::int32_t id) noexcept {
        release_slot(id, WseObjectKind::array);
    }

    [[nodiscard]] WseDictionary* get(std::int32_t id) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::dictionary ? slot.dictionary.get()
                                                      : nullptr;
    }

    [[nodiscard]] const WseDictionary* get(std::int32_t id) const noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::dictionary ? slot.dictionary.get()
                                                      : nullptr;
    }

    [[nodiscard]] WseDynArray* get_array(std::int32_t id) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::array ? slot.array.get() : nullptr;
    }

    [[nodiscard]] const WseDynArray* get_array(std::int32_t id) const noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[static_cast<std::size_t>(id)];
        return slot.kind == WseObjectKind::array ? slot.array.get() : nullptr;
    }

private:
    [[nodiscard]] std::int32_t alloc_slot(WseObjectSlot slot) {
        if (!free_ids.empty()) {
            const std::int32_t id = free_ids.back();
            free_ids.pop_back();
            slots[static_cast<std::size_t>(id)] = std::move(slot);
            return id;
        }
        const auto id = static_cast<std::int32_t>(slots.size());
        slots.push_back(std::move(slot));
        return id;
    }

    void release_slot(std::int32_t id, WseObjectKind expected_kind) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= slots.size()) {
            return;
        }
        auto& slot = slots[static_cast<std::size_t>(id)];
        if (slot.kind == expected_kind) {
            slot.dictionary.reset();
            slot.array.reset();
            slot.kind = WseObjectKind::empty;
            free_ids.push_back(id);
        }
    }
};

using WseDictionaryStore = WseObjectManager;

using WseExecuteScriptFn = bool (*)(void* user,
                                    std::int32_t script_index,
                                    std::span<const std::int64_t> params);

using WseSleepMsFn = void (*)(void* user, std::int32_t milliseconds);

// Runtime state read by the script-context query handlers. The defaults model
// a statement executed outside any trigger, with no return values, zero
// application time, and no active mission.
struct WseVmState {
    // param_6 of executeWSE: the statement block's context flags. WSE stores a
    // negative value while inside a trigger (0 outside), so store_current_trigger
    // returns it only when it is <= 0.
    std::int32_t caller = 0;
    // 0x140699b20: NUM_RETURN_VALUES (16) int64 slots written by return_values.
    std::span<const std::int64_t> return_values{};
    // 0x140699b18: number of slots currently set by return_values.
    std::int32_t num_return_values = 0;
    // DXUtil_Timer(TIMER_GETAPPTIME) in seconds; 3035 scales it by 1000.
    double app_time_seconds = 0.0;
    // [0x140620078] + 0x9ae8: cur_mission->cur_mission_template_no. A null
    // pointer means there is no current mission, which yields -1.
    const std::int32_t* current_mission_template_no = nullptr;
    // String registers used by the string group (4200-4224, 4233-4244).
    WseStringBanks strings{};
    // Runtime option flags used by options_{get,set}_* (4300-4305).
    WseOptionsState* options = nullptr;
    // Writable return-value buffer/count for 3015 return_values.
    std::span<std::int64_t> return_values_out{};
    std::int32_t* num_return_values_out = nullptr;
    // DAT_140663be8: script fixed-point scale (default 100).
    std::int32_t fixed_point_multiplier = 100;
    // rglTransform[128] position registers (0x140664060, stride 0x40).
    std::span<WseTransform> positions{};
    // App timers (0x140668b48, 128 slots) and global tick clocks (0x140620030).
    std::span<WseAppTimer> timers{};
    std::span<const std::int64_t> timers_total{};
    // PRNG and rand_seed state for 3020/3021/3034.
    WseMersenneTwister* rng = nullptr;
    std::int32_t* rand_seed = nullptr;
    // Profiler recording flag and marker log for 3040..3043.
    bool* profiler_recording = nullptr;
    std::vector<std::string>* profiler_markers = nullptr;
    // Unified dictionary/array handle store (WSEObjectManager) for 3200..3222
    // and 5000..5022.
    WseDictionaryStore* dictionaries = nullptr;
    // Script invocation callback and loaded script count for 5016
    // array_sort_custom.
    std::size_t num_scripts = 0;
    WseExecuteScriptFn execute_script_fn = nullptr;
    void* execute_script_user = nullptr;
    // Optional sleep_ms (3026) platform callback; falls back to blocking
    // wse::platform::sleep_ms when null.
    WseSleepMsFn sleep_ms_fn = nullptr;
    void* sleep_ms_user = nullptr;
    // Optional calendar-time source for get_time (3022); the system clock is
    // read when null. Not owned: it must outlive this state.
    wse::platform::WallClock* wall_clock = nullptr;
    // Active WSELuaManager runtime backend for opcodes 5100..5114; falls back
    // to world->m6.lua when null or uninitialized.
    WseLuaRuntime* lua_runtime = nullptr;
    // Game-world, mission, config, skill, and edit-mode state.
    wse::game::WseWorldState* world = nullptr;
};

// Decoded operands, matching the arrays built by mbStatement::execute.
struct WseOperands {
    std::span<const std::int64_t> wide{};     // param_3, full int64 values
    std::span<const std::int32_t> narrow{};   // param_4, int32 truncations
    std::span<const std::uint8_t> tags{};     // param_5, parseOperand tags
    std::span<float> fp{};                    // DAT_140699748, 128 floats
    // g_basicGame+0x270 (0x140663c60): the 128 int64 registers shared with
    // operand tag 1 and used by register_get/register_set (3000/3001).
    std::span<std::int64_t> registers{};
    // Script-context state for the global query handlers (3014-3600).
    WseVmState state{};
};

// Result of a successful handler. Value-writing handlers (mbStatement's
// common LAB_1401e0f8e tail) write effect.value through operand 0; the
// floating-point and comparison handlers write their own destinations.
struct WseEffect {
    std::int64_t value = 0;
    bool store_value = true;
};

enum class WseDispatch : std::uint8_t {
    handled = 0,                 // success; write effect.value through operand 0
    handled_no_store,            // success; handler wrote its own destination
    condition_false,             // executeWSE returned false (failed condition)
    ignored,                     // success; retail returned true without effect
    unimplemented,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(WseDispatch result) noexcept {
    switch (result) {
    case WseDispatch::handled:
        return "handled";
    case WseDispatch::handled_no_store:
        return "handled without operand-0 store";
    case WseDispatch::condition_false:
        return "false condition";
    case WseDispatch::ignored:
        return "ignored (invalid index)";
    case WseDispatch::unimplemented:
        return "unimplemented WSE opcode";
    case WseDispatch::operand_count_out_of_range:
        return "WSE opcode operand count out of range";
    }
    return "unknown WSE dispatch result";
}

// The shift amount is `(byte)operand & 0x3F`, exactly as the retail handlers
// use it (x86-64 masks a 64-bit shift by 63).
[[nodiscard]] constexpr unsigned wse_shift(std::int64_t value) noexcept {
    return static_cast<unsigned>(value & 0x3F);
}

// mbCheckRegister: unsigned comparison against 0x7f.
[[nodiscard]] constexpr bool wse_register_ok(std::int32_t index) noexcept {
    return static_cast<std::uint32_t>(index) <= 0x7Fu;
}

// mbCheckStringRegister (0x1401c5370): tags 3 and 0x16 are bounded by the
// runtime register counts, tag 0 by the fixed 128-entry literal table, and
// unknown tags are accepted.
[[nodiscard]] inline bool wse_string_register_ok(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    switch (tag) {
    case 0:
        return static_cast<std::uint32_t>(index) <= 0x7Fu;
    case 3:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.global.size();
    case 0x16:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.local.size();
    default:
        return true;
    }
}

// 0x140219df0: tag 0 reads the string register table, tag 0x16 the local
// bank, and tag 3 or an unknown tag the global bank. Out-of-range reads
// resolve to the static empty rgl::string (0x140622848 / 0x1406227b8).
[[nodiscard]] inline std::u16string_view wse_resolve_string(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    if (tag == 0) {
        if (static_cast<std::uint32_t>(index) <= 0x7Fu &&
            static_cast<std::size_t>(index) < banks.registers.size()) {
            return banks.registers[static_cast<std::size_t>(index)];
        }
        return {};
    }
    const auto bank = tag == 0x16 ? banks.local : banks.global;
    if (index >= 0 && static_cast<std::size_t>(index) < bank.size()) {
        return bank[static_cast<std::size_t>(index)];
    }
    return {};
}

// 0x140310b20 assigns a tag-0 string register. A missing table entry cannot
// be written, so the handler reports the retail invalid-register no-op.
[[nodiscard]] inline bool wse_store_string(WseStringBanks& banks,
                                           std::int32_t index,
                                           std::u16string_view value) {
    if (index < 0 || static_cast<std::uint32_t>(index) > 0x7Fu ||
        static_cast<std::size_t>(index) >= banks.registers.size()) {
        return false;
    }
    banks.registers[static_cast<std::size_t>(index)].assign(value);
    return true;
}

// Missing tags and operands read as zero: mbStatement::execute zeroes the
// first five tag and operand slots before parsing (0x1401c6399-0x1401c63ec).
[[nodiscard]] constexpr std::uint8_t wse_tag_at(
    std::span<const std::uint8_t> tags, std::size_t index) noexcept {
    return index < tags.size() ? tags[index] : std::uint8_t{0};
}

[[nodiscard]] inline bool* wse_option_flag(WseOptionsState* options,
                                           std::uint32_t opcode) noexcept {
    if (!options) {
        return nullptr;
    }
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::options_get_verbose_casualties:
    case WseOpcode::options_set_verbose_casualties:
        return &options->verbose_casualties;
    case WseOpcode::options_get_cheat_mode:
    case WseOpcode::options_set_cheat_mode:
        return &options->cheat_mode;
    case WseOpcode::options_get_realistic_headshots:
    case WseOpcode::options_set_realistic_headshots:
        return &options->realistic_headshots;
    case WseOpcode::set_horse_friendly_fire:
        return &options->horse_friendly_fire;
    case WseOpcode::set_show_crosshair:
        return &options->show_crosshair;
    case WseOpcode::edit_mode_set_enabled:
        return &options->edit_mode_enabled;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline bool wse_resolve_utf8_operand(
    std::size_t idx, const WseOperands& operands, std::string& out) {
    if (idx >= operands.narrow.size()) {
        return false;
    }
    const std::uint8_t tag = wse_tag_at(operands.tags, idx);
    if (!wse_string_register_ok(operands.narrow[idx], tag,
                                operands.state.strings)) {
        return false;
    }
    out = wse_utf16_to_utf8(wse_resolve_string(
        operands.narrow[idx], tag, operands.state.strings));
    return true;
}

[[nodiscard]] inline bool wse_parse_unsigned_index(
    std::u16string_view sv,
    std::size_t& pos,
    std::size_t& out_index) noexcept {
    if (pos >= sv.size() || sv[pos] < u'0' || sv[pos] > u'9') {
        return false;
    }
    std::size_t val = 0;
    while (pos < sv.size() && sv[pos] >= u'0' && sv[pos] <= u'9') {
        const auto digit = static_cast<std::size_t>(sv[pos] - u'0');
        if (val <= 1000000U) {
            val = val * 10U + digit;
        }
        ++pos;
    }
    out_index = val;
    return true;
}

[[nodiscard]] inline std::u16string wse_parse_string(
    std::u16string_view input,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks,
    bool url_encode_tokens = false);

// Mirrors mbBasicGame::parseToken / parseTokenReg / parseTokenStr
// (0x140072500..0x140072cd3): strips "!" / "***" comments, expands "reg<N>"
// or "reg<N>?<then>:<else>" (backward ':' scan >= '?'), and returns raw s[N].
[[nodiscard]] inline std::u16string wse_parse_token(
    std::u16string_view token,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks) {
    if (token.empty() || token.starts_with(u"!") ||
        token.starts_with(u"***")) {
        return {};
    }
    if (token.starts_with(u"s")) {
        std::size_t pos = 1;
        std::size_t index = 0;
        if (wse_parse_unsigned_index(token, pos, index) &&
            pos == token.size() && index < banks.registers.size()) {
            return banks.registers[index];
        }
    }
    if (token.starts_with(u"reg")) {
        std::size_t pos = 3;
        std::size_t index = 0;
        if (wse_parse_unsigned_index(token, pos, index) &&
            index < registers.size()) {
            if (pos == token.size()) {
                const std::string dec = std::to_string(registers[index]);
                return wse_utf8_to_utf16(dec);
            }
            if (token[pos] == u'?') {
                std::size_t colon = std::u16string_view::npos;
                std::size_t brace_depth = 0;
                for (std::size_t k = token.size(); k > pos + 1; --k) {
                    const char16_t ch = token[k - 1];
                    if (ch == u'}') {
                        ++brace_depth;
                    } else if (ch == u'{') {
                        if (brace_depth > 0) {
                            --brace_depth;
                        }
                    } else if (ch == u':' && brace_depth == 0) {
                        colon = k - 1;
                        break;
                    }
                }
                if (colon != std::u16string_view::npos) {
                    const std::u16string_view branch =
                        registers[index] != 0
                            ? token.substr(pos + 1, colon - (pos + 1))
                            : token.substr(colon + 1);
                    return wse_parse_string(branch, registers, banks);
                }
            }
        }
    }
    std::u16string out = u"UNRECOGNIZED TOKEN <";
    out.append(token);
    out.push_back(u'>');
    return out;
}

// Mirrors mbBasicGame::parseString (0x140072210): turns '^' into '\n' and
// evaluates balanced {...} tokens; expanded s[N] text is NOT re-parsed.
[[nodiscard]] inline std::u16string wse_parse_string(
    std::u16string_view input,
    std::span<const std::int64_t> registers,
    const WseStringBanks& banks,
    bool url_encode_tokens) {
    std::u16string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        const char16_t ch = input[i];
        if (ch == u'^') {
            out.push_back(u'\n');
            ++i;
            continue;
        }
        if (ch == u'{') {
            std::size_t depth = 1;
            std::size_t j = i + 1;
            while (j < input.size() && depth > 0) {
                if (input[j] == u'{') {
                    ++depth;
                } else if (input[j] == u'}') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
                ++j;
            }
            if (depth != 0 || j >= input.size()) {
                break;
            }
            if (j > i + 1) {
                const std::u16string_view token =
                    input.substr(i + 1, j - (i + 1));
                std::u16string expanded =
                    wse_parse_token(token, registers, banks);
                if (url_encode_tokens) {
                    expanded = wse_utf8_to_utf16(
                        wse_url_escape(wse_utf16_to_utf8(expanded)));
                }
                out.append(expanded);
            }
            i = j + 1;
            continue;
        }
        out.push_back(ch);
        ++i;
    }
    return out;
}

[[nodiscard]] inline bool wse_resolve_parsed_utf8_operand(
    std::size_t idx, const WseOperands& operands, std::string& out) {
    if (idx >= operands.narrow.size()) {
        return false;
    }
    const std::uint8_t tag = wse_tag_at(operands.tags, idx);
    if (!wse_string_register_ok(operands.narrow[idx], tag,
                                operands.state.strings)) {
        return false;
    }
    const std::u16string_view raw = wse_resolve_string(
        operands.narrow[idx], tag, operands.state.strings);
    const std::u16string expanded =
        wse_parse_string(raw, operands.registers, operands.state.strings);
    out = wse_utf16_to_utf8(expanded);
    return true;
}

[[nodiscard]] inline bool wse_dict_read_file_bytes(
    wse::platform::Vfs* vfs, std::string_view rel_path, std::string& out) {
    if (vfs == nullptr || !vfs->read_text(rel_path, out)) {
        out.clear();
        return false;
    }
    return true;
}

[[nodiscard]] inline bool wse_dict_write_file_bytes(
    wse::platform::Vfs* vfs, std::string_view rel_path, std::string_view data) {
    if (vfs == nullptr) {
        return false;
    }
    const std::span<const std::uint8_t> raw(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return vfs->write_file(rel_path, raw);
}

[[nodiscard]] inline bool wse_dict_target_path(
    const WseDictionaryStore* store,
    std::string name,
    std::string_view ext,
    std::string& out_path) {
    if (!store || store->vfs == nullptr || !wse_check_path(name)) {
        return false;
    }
    name.append(ext);
    out_path = std::move(name);
    return true;
}

[[nodiscard]] inline WseDispatch wse_store_dict_string(
    WseStringBanks& strings,
    std::int32_t sreg,
    std::string_view utf8_value) {
    return wse_store_string(strings, sreg, wse_utf8_to_utf16(utf8_value))
               ? WseDispatch::handled_no_store
               : WseDispatch::ignored;
}

}  // namespace wse::vm
