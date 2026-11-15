// Host tests for the portable dynamic array executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include "wse_test_common.h"

namespace {

using namespace wse::test;

void test_array_group() {
    namespace fs = std::filesystem;
    const fs::path temp_dir =
        fs::temp_directory_path() / "wse2_array_unit_test_tmp";
    const fs::path outside_escape =
        fs::temp_directory_path() / "wse2_array_escape_forbidden.wsearray";
    struct TempGuard {
        fs::path dir;
        fs::path escape;
        ~TempGuard() {
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::remove(escape, ec);
        }
    } guard{temp_dir, outside_escape};
    {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::remove(outside_escape, ec);
        fs::create_directories(temp_dir, ec);
        expect(!ec, "created temporary directory for array file tests");
    }

    std::array<std::u16string, 128> str_regs{};
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};
    for (auto& pos : positions) {
        pos = wse::vm::WseTransform::identity();
    }
    wse::platform::HostVfs host_vfs(temp_dir);
    wse::vm::WseObjectManager objects;
    objects.vfs = &host_vfs;

    WseVmState state;
    state.strings.registers = str_regs;
    state.positions = positions;
    state.dictionaries = &objects;

    WseEffect effect{};
    const auto call = [&](WseOpcode op,
                          std::initializer_list<std::int64_t> wide_args,
                          std::initializer_list<std::uint8_t> tag_args = {}) {
        std::vector<std::int64_t> wide(wide_args);
        std::vector<std::int32_t> narrow(wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i) {
            narrow[i] = static_cast<std::int32_t>(wide[i]);
        }
        std::vector<std::uint8_t> tags(tag_args);
        WseOperands operands;
        operands.wide = wide;
        operands.narrow = narrow;
        operands.tags = tags;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        effect = WseEffect{0x1234, true};
        return wse::vm::execute_wse(opcode(op), operands, effect);
    };

    // Unified WseObjectManager ID sharing and cross-type rejection.
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "unified pool allocates dict id 0");
    expect(call(WseOpcode::array_create, {0, 0, 3}) == WseDispatch::handled &&
               effect.value == 1,
           "unified pool allocates array id 1 next");
    expect(call(WseOpcode::dict_get_size, {0, 1}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_get_type_id, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_free, {0}) == WseDispatch::handled_no_store,
           "cross-type dictionary/array operations safely reject mismatched handles");
    expect(call(WseOpcode::array_free, {1}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create reuses freed array handle in LIFO order");
    expect(call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {1}) == WseDispatch::handled_no_store,
           "cleaned up dict handles");

    // 5000 array_create validation + 1..14D + default initialization.
    expect(call(WseOpcode::array_create, {0, 3, 2}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create, {0, 0, -1}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create, {0, 0, 2, 0}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create,
                    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "array_create stores 0 on invalid type_id, dims, or >14 dimensions");

    // Safety deviation: element counts above kWseArrayMaxElements are refused.
    expect(call(WseOpcode::array_create, {0, 0, 2000000000}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create, {0, 0, 65536, 65536}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "array_create stores 0 instead of allocating an oversized array");
    expect(call(WseOpcode::array_create, {0, 0, 4, 4}) == WseDispatch::handled,
           "array_create creates a small 2D array for the resize cap check");
    const std::int64_t capArr = effect.value;
    expect(call(WseOpcode::array_resize_dim, {capArr, 1, 2000000000}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, capArr, 1}) ==
                   WseDispatch::handled &&
               effect.value == 4 &&
               call(WseOpcode::array_free, {capArr}) == WseDispatch::handled_no_store,
           "array_resize_dim leaves the array unchanged when the new size exceeds the cap");

    expect(call(WseOpcode::array_create,
                {0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) ==
               WseDispatch::handled,
           "array_create creates 14D integer array");
    const std::int64_t arr14d = effect.value;
    expect(call(WseOpcode::array_get_dim_count, {0, arr14d}) == WseDispatch::handled &&
               effect.value == 14 &&
               call(WseOpcode::array_get_dim_size, {0, arr14d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 2 &&
               call(WseOpcode::array_get_dim_size, {0, arr14d, 14}) ==
                   WseDispatch::handled &&
               effect.value == -1 &&
               call(WseOpcode::array_get_val,
                    {0, arr14d, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_free, {arr14d}) == WseDispatch::handled_no_store,
           "14D array reports dim count/sizes, default 0 values, and -1 OOB dim");

    // 1D int array: set_val, set_val_all, resize_dim (size>=1), copy, eq..le.
    expect(call(WseOpcode::array_create, {0, 0, 3}) == WseDispatch::handled,
           "create 1D int array");
    const std::int64_t ints_id = effect.value;
    expect(call(WseOpcode::array_set_val_all, {ints_id, 42}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_set_val, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_resize_dim, {ints_id, 0, 5}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_resize_dim, {ints_id, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_get_val, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 42 &&
               call(WseOpcode::array_get_val, {0, ints_id, 1}) ==
                   WseDispatch::handled &&
               effect.value == 15 &&
               call(WseOpcode::array_get_val, {0, ints_id, 4}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "int array set_val_all, set_val, and resize_dim preserve overlap and zero-fill");

    expect(call(WseOpcode::array_eq, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_neq, {ints_id, 99, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {ints_id, 10, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_ge, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_lt, {ints_id, 20, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_le, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {ints_id, 15, 1}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::array_eq, {ints_id, 15, 99}) ==
                   WseDispatch::condition_false,
           "array_eq..array_le evaluate integer comparisons and OOB indices");

    // 5002 array_copy creates independent deep copy; bad handle does not store.
    expect(call(WseOpcode::array_copy, {0, ints_id}) == WseDispatch::handled,
           "array_copy duplicates int array");
    const std::int64_t copy_id = effect.value;
    expect(call(WseOpcode::array_set_val, {copy_id, 999, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {0, ints_id, 1}) ==
                   WseDispatch::handled &&
               effect.value == 15 &&
               call(WseOpcode::array_copy, {0, 9999}) ==
                   WseDispatch::handled_no_store &&
               effect.value == 0x1234,
           "array_copy is deep and ignores invalid source ID");
    (void)call(WseOpcode::array_free, {copy_id});

    // 5009 / 5010 push & pop on 1D int/string/pos + 2D multidim + empty pop guard.
    expect(call(WseOpcode::array_push, {ints_id, 77}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_pop, {0, ints_id}) == WseDispatch::handled &&
               effect.value == 77 &&
               call(WseOpcode::array_get_dim_size, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5,
           "1D int array_push/array_pop updates element and dimension size");

    expect(call(WseOpcode::array_create, {0, 1, 0}) == WseDispatch::handled,
           "create 1D empty string array");
    const std::int64_t strs_id = effect.value;
    int_regs[3] = 55;
    str_regs[0] = u"val^{reg3}";
    str_regs[1] = u"Alpha";
    str_regs[2] = u"alpha";
    str_regs[3] = u"beta";
    expect(call(WseOpcode::array_push, {strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {5, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[5] == u"val\n55" &&
               call(WseOpcode::array_pop, {128, strs_id}) == WseDispatch::ignored &&
               call(WseOpcode::array_get_dim_size, {0, strs_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 4 &&
               call(WseOpcode::array_lt, {strs_id, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_pop, {6, strs_id}) ==
                   WseDispatch::handled_no_store &&
               str_regs[6] == u"beta",
           "string array expands parseString, guards pop dst register, and compares bytes");

    // 5015 array_sort: CS/CI string sort + 2D stability check!
    expect(call(WseOpcode::array_sort, {strs_id, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {10, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {11, strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"Alpha" && str_regs[11] == u"alpha" &&
               call(WseOpcode::array_sort, {strs_id, 3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {10, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {11, strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"val\n55" && str_regs[11] == u"Alpha",
           "string array_sort supports CI ascending/descending while staying stable");

    // 2D int array stability, subdim sort, push_subarray, pop_subarray!
    expect(call(WseOpcode::array_create, {0, 0, 4, 2}) == WseDispatch::handled,
           "create 2D (4x2) int array");
    const std::int64_t arr2d = effect.value;
    // Rows: (key=20, id=0), (key=10, id=1), (key=20, id=2), (key=10, id=3).
    (void)call(WseOpcode::array_set_val, {arr2d, 20, 0, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 0, 0, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 10, 1, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 1, 1, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 20, 2, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 2, 2, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 10, 3, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 3, 3, 1});
    expect(call(WseOpcode::array_sort, {arr2d, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {0, arr2d, 0, 1}) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::array_get_val, {0, arr2d, 1, 1}) ==
                   WseDispatch::handled &&
               effect.value == 3 &&
               call(WseOpcode::array_get_val, {0, arr2d, 2, 1}) ==
                   WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_get_val, {0, arr2d, 3, 1}) ==
                   WseDispatch::handled &&
               effect.value == 2,
           "2D array_sort permutes dim 0 stably by key at subdims");

    // 1D subarray push + pop on 2D array + mismatched push rejection.
    expect(call(WseOpcode::array_create, {0, 0, 2}) == WseDispatch::handled,
           "create matching 1D slice");
    const std::int64_t row_id = effect.value;
    (void)call(WseOpcode::array_set_val, {row_id, 50, 0});
    (void)call(WseOpcode::array_set_val, {row_id, 99, 1});
    expect(call(WseOpcode::array_push, {arr2d, row_id}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, arr2d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_push, {arr2d, ints_id}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, arr2d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_pop, {0, arr2d}) == WseDispatch::handled,
           "multidim push/pop rejects mismatched dims and creates subarray on pop");
    const std::int64_t popped_row = effect.value;
    expect(call(WseOpcode::array_get_dim_count, {0, popped_row}) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::array_get_val, {0, popped_row, 1}) ==
                   WseDispatch::handled &&
               effect.value == 99,
           "multidim popped row contains pushed elements");

    // Position arrays: default zeroed transform, orthonormalize compare in array_eq/neq, file I/O!
    expect(call(WseOpcode::array_create, {0, 2, 2}) == WseDispatch::handled,
           "create 1D position array");
    const std::int64_t pos_arr = effect.value;
    expect(call(WseOpcode::array_get_val, {20, pos_arr, 0}) ==
                   WseDispatch::handled_no_store &&
               positions[20].s.x == 0.0f && positions[20].f.y == 0.0f &&
               positions[20].o.x == 0.0f,
           "position array default initializes to 0-position");
    positions[1] = wse::vm::WseTransform::identity();
    positions[1].o = {1.5f, -2.25f, 3.75f, 1.0f};
    positions[1].f = {0.0f, 5.0f, 0.0f, 0.0f};  // Unnormalized forward, orthonormalize makes identical!
    positions[2] = wse::vm::WseTransform::identity();
    positions[2].o = {1.5f, -2.25f, 3.75f, 1.0f};
    positions[3] = wse::vm::WseTransform::identity();
    positions[3].o = {9.0f, 0.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::array_set_val, {pos_arr, 1, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_eq, {pos_arr, 2, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_neq, {pos_arr, 3, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {pos_arr, 2, 0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::array_sort, {pos_arr, 0}) ==
                   WseDispatch::handled_no_store,
           "position array_eq/neq orthonormalizes copies and rejects ordered compare ops");

    // .wsearray binary save / load / delete + path traversal & version > 1 checks!
    str_regs[0] = u"unit_pos_{reg3}";
    str_regs[1] = u"../wse2_array_escape_forbidden";
    expect(call(WseOpcode::array_save_file, {pos_arr, 0}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "unit_pos_55.wsearray") &&
               call(WseOpcode::array_save_file, {pos_arr, 1}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(outside_escape) &&
               call(WseOpcode::array_load_file, {0, 0}) == WseDispatch::handled &&
               effect.value > 0,
           "array_save_file/load_file roundtrip .wsearray and reject traversal paths");
    const std::int64_t loaded_pos = effect.value;
    expect(call(WseOpcode::array_eq, {loaded_pos, 2, 0}) ==
                   WseDispatch::handled_no_store,
           "loaded .wsearray orthonormalizes transforms cleanly");

    // Write a version=2 .wsearray header and confirm load returns 0.
    {
        const fs::path bad_ver = temp_dir / "future_ver.wsearray";
        std::ofstream out(bad_ver, std::ios::binary | std::ios::trunc);
        const std::array<std::uint8_t, 16> header = {
            0x41, 0x45, 0x53, 0x57,  // LE 0x57534541
            0x02, 0x00, 0x00, 0x00,  // version 2 (> 1)
            0x00, 0x00, 0x00, 0x00,  // int
            0x01, 0x00, 0x00, 0x00   // dimCount = 1
        };
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
    }
    str_regs[4] = u"future_ver";
    expect(call(WseOpcode::array_load_file, {0, 4}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_delete_file, {0}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "unit_pos_55.wsearray"),
           "array_load_file rejects version>1 with 0 and delete_file removes .wsearray");

    // Binary .wsearray save/load roundtrip for 2D integer + string arrays.
    str_regs[7] = u"arr2d_bin";
    str_regs[8] = u"strs_bin";
    expect(call(WseOpcode::array_save_file, {arr2d, 7}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_save_file, {strs_id, 8}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_load_file, {0, 7}) == WseDispatch::handled &&
               effect.value > 0 &&
               call(WseOpcode::array_eq, {effect.value, 10, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_load_file, {0, 8}) == WseDispatch::handled &&
               effect.value > 0 &&
               call(WseOpcode::array_get_val, {12, effect.value, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[12] == u"val\n55",
           ".wsearray binary save/load roundtrips 2D int and UTF-8 string arrays");

    // Operand-count checks across array ops.
    expect(call(WseOpcode::array_create, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_free, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_copy, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_save_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_load_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_delete_file, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_set_val, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_set_val_all, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_val, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_push, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_pop, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_resize_dim, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_dim_size, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_dim_count, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_type_id, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_sort, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_sort_custom, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_eq, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range,
           "array ops validate operand counts");
}

void test_array_memory_vfs() {
    std::array<std::u16string, 128> str_regs{};
    str_regs[0] = u"mem_arr";
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};

    wse::platform::MemoryVfs mem_vfs;
    wse::vm::WseObjectManager objects;
    objects.vfs = &mem_vfs;

    WseVmState state;
    state.strings.registers = str_regs;
    state.positions = positions;
    state.dictionaries = &objects;

    WseEffect effect{};
    expect(run(opcode(WseOpcode::array_create), {0, 0, 3}, {0, 0, 3}, effect,
               fp, int_regs, state) == WseDispatch::handled,
           "MemoryVfs: array_create returns handle");
    const std::int64_t a1 = effect.value;
    expect(run(opcode(WseOpcode::array_set_val), {a1, 77, 1},
               {static_cast<std::int32_t>(a1), 77, 1}, effect, fp, int_regs,
               state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::array_save_file), {a1, 0},
                   {static_cast<std::int32_t>(a1), 0}, effect, fp, int_regs,
                   state) == WseDispatch::handled_no_store &&
               mem_vfs.is_file("mem_arr.wsearray"),
           "MemoryVfs: array_save_file persists .wsearray in memory");
    expect(run(opcode(WseOpcode::array_load_file), {0, 0}, {0, 0}, effect, fp,
               int_regs, state) == WseDispatch::handled &&
               effect.value > 0,
           "MemoryVfs: array_load_file loads .wsearray from memory");
    const std::int64_t a2 = effect.value;
    expect(run(opcode(WseOpcode::array_get_val), {0, a2, 1},
               {0, static_cast<std::int32_t>(a2), 1}, effect, fp, int_regs,
               state) == WseDispatch::handled &&
               effect.value == 77,
           "MemoryVfs: loaded array preserves stored element");
    expect(run(opcode(WseOpcode::array_delete_file), {0}, {0}, effect, fp,
               int_regs, state) == WseDispatch::handled_no_store &&
               !mem_vfs.is_file("mem_arr.wsearray"),
           "MemoryVfs: array_delete_file removes in-memory file");

    // Without a Vfs the array file opcodes are no-ops and load yields no array.
    objects.vfs = nullptr;
    expect(run(opcode(WseOpcode::array_save_file), {a1, 0},
               {static_cast<std::int32_t>(a1), 0}, effect, fp, int_regs,
               state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::array_load_file), {0, 0}, {0, 0}, effect,
                   fp, int_regs, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::array_delete_file), {0}, {0}, effect, fp,
                   int_regs, state) == WseDispatch::handled_no_store &&
               !mem_vfs.is_file("mem_arr.wsearray"),
           "array file opcodes are no-ops without a Vfs");

    // A host directory that does not exist yet is created by the first save.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path fresh =
        fs::temp_directory_path() / "wse2_array_unit_test_fresh" / "arrays";
    fs::remove_all(fresh.parent_path(), ec);
    {
        wse::platform::HostVfs fresh_vfs(fresh);
        objects.vfs = &fresh_vfs;
        expect(run(opcode(WseOpcode::array_save_file), {a1, 0},
                   {static_cast<std::int32_t>(a1), 0}, effect, fp, int_regs,
                   state) == WseDispatch::handled_no_store &&
                   fs::exists(fresh / "mem_arr.wsearray", ec),
               "array_save_file creates a missing host root");
        objects.vfs = nullptr;
    }
    fs::remove_all(fresh.parent_path(), ec);
}

}  // namespace

namespace wse::selftest {

int run_wse_array_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_array_group();
    test_array_memory_vfs();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("wse_array tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_array_suite(nullptr);
}
#endif
