// Host tests for the portable dictionary executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include "wse_test_common.h"

namespace {

using namespace wse::test;

void test_dictionary_group() {
    namespace fs = std::filesystem;
    const fs::path temp_dir =
        fs::temp_directory_path() / "wse2_dict_unit_test_tmp";
    const fs::path outside_escape =
        fs::temp_directory_path() / "wse2_dict_escape_forbidden.wsedict";
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
        expect(!ec, "created temporary directory for dictionary file tests");
    }

    std::u16string str_regs[128] = {};
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};
    wse::platform::HostVfs host_vfs(temp_dir);
    wse::vm::WseDictionaryStore store;
    store.vfs = &host_vfs;

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.positions = positions;
    state.dictionaries = &store;

    WseEffect effect{};
    const auto call = [&](WseOpcode op, std::initializer_list<std::int64_t> wide_args,
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
        return execute_wse(opcode(op), operands, effect);
    };

    // 3200 dict_create & 3201 dict_free (LIFO free-ID reuse).
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "dict_create assigns id 0");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create assigns id 1");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 2,
           "dict_create assigns id 2");
    expect(call(WseOpcode::dict_free, {1}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {-1}) == WseDispatch::handled_no_store,
           "dict_free ignores invalid or already-freed IDs");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "dict_create reuses most recently freed id (LIFO)");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create reuses earlier freed id next");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 3,
           "dict_create allocates fresh slot once free list is empty");

    // 3206/3207/3208/3210..3214/3221/3205 basic CRUD & iteration.
    expect(call(WseOpcode::dict_is_empty, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_is_empty, {99}) ==
                   WseDispatch::handled_no_store,
           "dict_is_empty returns true for empty and invalid IDs");
    str_regs[0] = u"beta";
    str_regs[1] = u"hello";
    str_regs[2] = u"alpha";
    str_regs[3] = u"-42";
    str_regs[4] = u"gamma";
    str_regs[5] = u"world\u00e9";
    str_regs[6] = u"missing";
    str_regs[7] = u"fallback_str";
    expect(call(WseOpcode::dict_set_str, {0, 0, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_int, {0, 2, -42}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {0, 4, 5}) ==
                   WseDispatch::handled_no_store,
           "dict_set_str and dict_set_int store entries");
    expect(call(WseOpcode::dict_is_empty, {0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_size, {0, 0}) == WseDispatch::handled &&
               effect.value == 3,
           "dict_is_empty and dict_get_size reflect populated entries");
    expect(call(WseOpcode::dict_has_key, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {0, 6}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_has_key, {99, 0}) ==
                   WseDispatch::condition_false,
           "dict_has_key distinguishes present, missing, and invalid dicts");
    expect(call(WseOpcode::dict_get_str, {10, 0, 4}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"world\u00e9" &&
               call(WseOpcode::dict_get_str, {11, 0, 6, 7}) ==
                   WseDispatch::handled_no_store &&
               str_regs[11] == u"fallback_str" &&
               call(WseOpcode::dict_get_str, {12, 0, 6}) ==
                   WseDispatch::handled_no_store &&
               str_regs[12].empty(),
           "dict_get_str reads UTF-8 values and applies default/empty fallback");
    expect(call(WseOpcode::dict_get_int, {0, 0, 2}) == WseDispatch::handled &&
               effect.value == -42 &&
               call(WseOpcode::dict_get_int, {0, 0, 6, 77}) ==
                   WseDispatch::handled &&
               effect.value == 77 &&
               call(WseOpcode::dict_get_int, {0, 0, 0, 77}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "dict_get_int parses ints, uses default on missing, 0 on non-int");
    expect(call(WseOpcode::dict_get_key_by_iterator, {20, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[20] == u"alpha" &&
               call(WseOpcode::dict_get_key_by_iterator, {21, 0, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[21] == u"beta" &&
               call(WseOpcode::dict_get_key_by_iterator, {22, 0, 2}) ==
                   WseDispatch::handled_no_store &&
               str_regs[22] == u"gamma" &&
               call(WseOpcode::dict_get_key_by_iterator, {23, 0, 3}) ==
                   WseDispatch::handled_no_store &&
               str_regs[23].empty(),
           "dict_get_key_by_iterator visits keys in sorted order");
    expect(call(WseOpcode::dict_erase, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {0, 0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_size, {0, 0}) == WseDispatch::handled &&
               effect.value == 2,
           "dict_erase removes target key only");

    // 3215 dict_get_pos & 3216 dict_set_pos (9-float fallback & orthonormalization).
    positions[0] = wse::vm::WseTransform::identity();
    positions[0].o = {10.5f, -20.25f, 30.75f, 1.0f};
    positions[0].rotate_z(0.6f);
    positions[0].rotate_x(0.3f);
    str_regs[30] = u"pose";
    expect(call(WseOpcode::dict_set_pos, {0, 30, 0}) ==
               WseDispatch::handled_no_store,
           "dict_set_pos serializes 9 floats");
    expect(call(WseOpcode::dict_get_pos, {1, 0, 30}) ==
                   WseDispatch::handled_no_store &&
               close(positions[1].o.x, 10.5f) &&
               close(positions[1].o.y, -20.25f) &&
               close(positions[1].o.z, 30.75f) &&
               close(positions[1].s.x, positions[0].s.x) &&
               close(positions[1].f.y, positions[0].f.y) &&
               close(positions[1].u.z, positions[0].u.z),
           "dict_get_pos round-trips position transform");
    // Non-orthonormal input gets orthonormalized:
    str_regs[31] = u"skewed";
    str_regs[32] = u"1,2,3,0,2,0,0,1,3";
    expect(call(WseOpcode::dict_set_str, {0, 31, 32}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_pos, {2, 0, 31}) ==
                   WseDispatch::handled_no_store &&
               close(positions[2].o.x, 1.0f) &&
               close(positions[2].o.y, 2.0f) &&
               close(positions[2].o.z, 3.0f) &&
               close(positions[2].s.length(), 1.0f) &&
               close(positions[2].f.length(), 1.0f) &&
               close(positions[2].u.length(), 1.0f) &&
               close(positions[2].s.dot(positions[2].f), 0.0f) &&
               close(positions[2].f.dot(positions[2].u), 0.0f),
           "dict_get_pos orthonormalizes parsed f/u vectors");
    // Fallback when token count != 9 or key missing:
    positions[3].o = {7.0f, 8.0f, 9.0f, 1.0f};
    str_regs[33] = u"short_pos";
    str_regs[34] = u"1,2,3";
    expect(call(WseOpcode::dict_set_str, {0, 33, 34}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_pos, {4, 0, 33, 3}) ==
                   WseDispatch::handled_no_store &&
               close(positions[4].o.x, 7.0f) &&
               close(positions[4].o.y, 8.0f) &&
               close(positions[4].o.z, 9.0f) &&
               call(WseOpcode::dict_get_pos, {4, 0, 33}) ==
                   WseDispatch::handled_no_store &&
               close(positions[4].o.x, 0.0f) && close(positions[4].s.x, 1.0f),
           "dict_get_pos uses fallback/identity when tokens != 9");

    // 3203 dict_load_dict mode semantics (0 is no-op, 1 overwrites, 2 inserts missing).
    str_regs[40] = u"k_keep";
    str_regs[41] = u"v_keep";
    str_regs[42] = u"k_shared";
    str_regs[43] = u"v_old";
    str_regs[44] = u"v_new";
    str_regs[45] = u"k_added";
    str_regs[46] = u"v_added";
    expect(call(WseOpcode::dict_set_str, {1, 40, 41}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {1, 42, 43}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {2, 42, 44}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {2, 45, 46}) ==
                   WseDispatch::handled_no_store,
           "populated source and dest dicts for dict_load_dict");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {1, 45}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_old",
           "dict_load_dict mode 0 is a no-op");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {50, 1, 45}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_added" &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_old",
           "dict_load_dict mode 2 inserts only missing keys");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_new" &&
               call(WseOpcode::dict_has_key, {1, 40}) ==
                   WseDispatch::handled_no_store,
           "dict_load_dict mode 1 overwrites collisions while preserving existing keys");

    // 3204 dict_save, 3202 dict_load_file, 3209 dict_delete_file (.wsedict and .ini).
    str_regs[60] = u"bin_dict";
    str_regs[61] = u"ini_dict";
    expect(call(WseOpcode::dict_save, {1, 60}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "bin_dict.wsedict"),
           "dict_save writes binary .wsedict by default");
    expect(call(WseOpcode::dict_save, {1, 61, 1}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "ini_dict.ini"),
           "dict_save(format=1) writes .ini file");
    expect(call(WseOpcode::dict_clear, {3}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {3, 40, 46}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file, {3, 60, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 40}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_keep" &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_new",
           "dict_load_file(mode=0) clears and loads binary .wsedict");
    // Verify mode 2 vs mode 1 on INI and binary:
    expect(call(WseOpcode::dict_set_str, {3, 42, 43}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_erase, {3, 45}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file, {3, 61, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 45}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_added" &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_old",
           "dict_load_file(mode=2, format=1) inserts missing keys from .ini");
    expect(call(WseOpcode::dict_load_file, {3, 61, 1, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_new",
           "dict_load_file(mode=1, format=1) overwrites keys from .ini");
    // Corrupt/truncated .wsedict does not crash.
    {
        std::ofstream corrupt(temp_dir / "corrupt.wsedict", std::ios::binary);
        const char bytes[] = {'\x7f', '\x00', '\x00', '\x00', '\x01', '\x00'};
        corrupt.write(bytes, sizeof(bytes));
    }
    str_regs[62] = u"corrupt";
    expect(call(WseOpcode::dict_load_file, {3, 62, 1}) ==
               WseDispatch::handled_no_store,
           "dict_load_file rejects truncated/malformed binary safely");
    expect(call(WseOpcode::dict_delete_file, {60}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "bin_dict.wsedict") &&
               call(WseOpcode::dict_delete_file, {61, 1}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "ini_dict.ini"),
           "dict_delete_file deletes .wsedict and .ini targets");

    // 3217/3218/3222 JSON save/load/delete including \uXXXX and surrogate pairs.
    {
        std::ofstream js(temp_dir / "unicode.json", std::ios::binary);
        js << "{\"uml\":\"\\u00e9\",\"smile\":\"\\uD83D\\uDE00\",\"esc\":\"a\\\\b\\nc\"}";
    }
    str_regs[63] = u"unicode";
    str_regs[64] = u"uml";
    str_regs[65] = u"smile";
    str_regs[66] = u"esc";
    expect(call(WseOpcode::dict_load_file_json, {3, 63, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {52, 3, 64}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\u00e9" &&
               call(WseOpcode::dict_get_str, {52, 3, 65}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\U0001F600" &&
               call(WseOpcode::dict_get_str, {52, 3, 66}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"a\\b\nc",
           "dict_load_file_json decodes \\uXXXX, UTF-16 surrogates, and escapes");
    str_regs[67] = u"roundtrip_json";
    expect(call(WseOpcode::dict_save_json, {3, 67}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "roundtrip_json.json") &&
               call(WseOpcode::dict_clear, {3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file_json, {3, 67, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {52, 3, 65}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\U0001F600" &&
               call(WseOpcode::dict_delete_file_json, {67}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "roundtrip_json.json"),
           "dict_save_json / load / delete round-trip cleanly");

    // 3219 dict_from_url_encoded_json & 3220 dict_to_url_encoded_json.
    expect(call(WseOpcode::dict_clear, {3}) == WseDispatch::handled_no_store,
           "cleared dict 3");
    str_regs[70] = u"a+b";
    str_regs[71] = u"hello world!";
    expect(call(WseOpcode::dict_set_str, {3, 70, 71}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_to_url_encoded_json, {80, 3}) ==
                   WseDispatch::handled_no_store &&
               str_regs[80] == u"%7B%22a%2Bb%22%3A%22hello%20world%21%22%7D",
           "dict_to_url_encoded_json percent-encodes reserved bytes and + as %2B");
    // Now verify dict_from_url_encoded_json turns '+' into space while keeping %2B as '+'.
    str_regs[81] = u"%7B%22a%2Bb%22%3A+%22hello+world%21%22%7D";
    expect(call(WseOpcode::dict_set_str, {2, 40, 41}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_from_url_encoded_json, {2, 81, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_size, {0, 2}) == WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::dict_get_str, {53, 2, 70}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"hello world!",
           "dict_from_url_encoded_json converts + to space before unescape");
    // Mode 2 keeps existing entries when loading URL-encoded JSON.
    str_regs[82] = u"%7B%22a%2Bb%22%3A%22changed%22%2C%22extra%22%3A%22ok%22%7D";
    str_regs[83] = u"extra";
    expect(call(WseOpcode::dict_from_url_encoded_json, {2, 82, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {53, 2, 70}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"hello world!" &&
               call(WseOpcode::dict_get_str, {53, 2, 83}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"ok",
           "dict_from_url_encoded_json mode 2 inserts only missing keys");

    // mbCheckPath sandboxing rejects traversal, separators, wildcards, empty names.
    const std::u16string_view bad_paths[] = {
        u"../wse2_dict_escape_forbidden",
        u"sub/file",
        u"sub\\file",
        u"C:evil",
        u"bad?name",
        u"...",
        u"",
    };
    for (const auto bad : bad_paths) {
        str_regs[90] = std::u16string(bad);
        expect(call(WseOpcode::dict_save, {2, 90}) ==
                       WseDispatch::handled_no_store &&
                   call(WseOpcode::dict_save_json, {2, 90}) ==
                       WseDispatch::handled_no_store &&
                   !fs::exists(outside_escape),
               "mbCheckPath blocks unsafe/traversal file names");
    }

    // Invalid registers & operand count checks across dictionary ops.
    expect(call(WseOpcode::dict_get_str, {128, 2, 70}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_key_by_iterator, {-1, 2, 0}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_to_url_encoded_json, {128, 2}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_pos, {128, 0, 30}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_pos, {1, 0, 30, 128}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_set_pos, {0, 30, 128}) ==
                   WseDispatch::ignored,
           "dictionary ops return ignored for invalid register indices");
    expect(call(WseOpcode::dict_create, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_free, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_dict, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_save, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_clear, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_is_empty, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_has_key, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_size, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_delete_file, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_str, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_int, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_str, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_int, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_key_by_iterator, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_pos, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_pos, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_file_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_save_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_from_url_encoded_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_to_url_encoded_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_erase, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_delete_file_json, {}) ==
                   WseDispatch::operand_count_out_of_range,
           "all 23 dictionary opcodes validate minimum operand count");
}

void test_dictionary_memory_vfs() {
    std::u16string str_regs[128] = {};
    str_regs[0] = u"mem_save";
    str_regs[1] = u"greeting";
    str_regs[2] = u"hello";
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};

    wse::platform::MemoryVfs mem_vfs;
    wse::vm::WseDictionaryStore store;
    store.vfs = &mem_vfs;

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.positions = positions;
    state.dictionaries = &store;

    WseEffect effect{};
    expect(run(opcode(WseOpcode::dict_create), {0}, {0}, effect, fp, int_regs,
               state) == WseDispatch::handled,
           "MemoryVfs: dict_create returns handle");
    const std::int64_t d1 = effect.value;
    expect(run(opcode(WseOpcode::dict_set_str), {d1, 1, 2},
               {static_cast<std::int32_t>(d1), 1, 2}, effect, fp, int_regs,
               state) == WseDispatch::handled_no_store,
           "MemoryVfs: dict_set_str populates dictionary");
    expect(run(opcode(WseOpcode::dict_save), {d1, 0},
               {static_cast<std::int32_t>(d1), 0}, effect, fp, int_regs, state) ==
                   WseDispatch::handled_no_store &&
               mem_vfs.is_file("mem_save.wsedict"),
           "MemoryVfs: dict_save writes .wsedict in memory");
    expect(run(opcode(WseOpcode::dict_save_json), {d1, 0},
               {static_cast<std::int32_t>(d1), 0}, effect, fp, int_regs, state) ==
                   WseDispatch::handled_no_store &&
               mem_vfs.is_file("mem_save.json"),
           "MemoryVfs: dict_save_json writes .json in memory");

    expect(run(opcode(WseOpcode::dict_create), {0}, {0}, effect, fp, int_regs,
               state) == WseDispatch::handled,
           "MemoryVfs: dict_create second handle");
    const std::int64_t d2 = effect.value;
    str_regs[3].clear();
    expect(run(opcode(WseOpcode::dict_load_file_json), {d2, 0},
               {static_cast<std::int32_t>(d2), 0}, effect, fp, int_regs, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::dict_get_str), {3, d2, 1},
                   {3, static_cast<std::int32_t>(d2), 1}, effect, fp, int_regs,
                   state) == WseDispatch::handled_no_store &&
               str_regs[3] == u"hello",
           "MemoryVfs: dict_load_file_json reloads saved JSON");
    expect(run(opcode(WseOpcode::dict_delete_file), {0}, {0}, effect, fp,
               int_regs, state) == WseDispatch::handled_no_store &&
               !mem_vfs.is_file("mem_save.wsedict") &&
               run(opcode(WseOpcode::dict_delete_file_json), {0}, {0}, effect,
                   fp, int_regs, state) == WseDispatch::handled_no_store &&
               !mem_vfs.is_file("mem_save.json"),
           "MemoryVfs: dict_delete_file{,_json} remove in-memory files");
}

// Storage edge cases of the Vfs routing: binary and .ini round trips, a store
// with no Vfs, a host root that does not exist yet, and names that differ only
// in case.
void test_dictionary_storage_edges() {
    namespace fs = std::filesystem;
    std::u16string str_regs[128] = {};
    str_regs[0] = u"edge";
    str_regs[1] = u"greeting";
    str_regs[2] = u"hello";
    str_regs[4] = u"EDGE";
    str_regs[5] = u"bye";
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};

    wse::platform::MemoryVfs mem_vfs;
    wse::vm::WseDictionaryStore store;
    store.vfs = &mem_vfs;

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.positions = positions;
    state.dictionaries = &store;

    WseEffect effect{};
    const auto op = [&](WseOpcode code, std::initializer_list<std::int64_t> args) {
        const std::vector<std::int64_t> wide(args);
        std::vector<std::int32_t> narrow;
        for (const std::int64_t arg : wide) {
            narrow.push_back(static_cast<std::int32_t>(arg));
        }
        wse::vm::WseOperands operands;
        operands.wide = wide;
        operands.narrow = narrow;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        effect = WseEffect{0x1234, true};
        return wse::vm::execute_wse(opcode(code), operands, effect);
    };
    const auto create = [&] {
        expect(op(WseOpcode::dict_create, {0}) == WseDispatch::handled,
               "storage edges: dict_create");
        return effect.value;
    };
    const auto reads_back = [&](std::int64_t dict, const char16_t* want) {
        str_regs[3] = u"stale";
        return op(WseOpcode::dict_get_str, {3, dict, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[3] == want;
    };

    const std::int64_t src = create();
    expect(op(WseOpcode::dict_set_str, {src, 1, 2}) ==
               WseDispatch::handled_no_store,
           "storage edges: source dictionary populated");

    // Binary (.wsedict) and .ini round trips through MemoryVfs.
    for (const std::int64_t format : {std::int64_t{0}, std::int64_t{1}}) {
        const char* file = format == 1 ? "edge.ini" : "edge.wsedict";
        const std::int64_t dst = create();
        expect(op(WseOpcode::dict_save, {src, 0, format}) ==
                   WseDispatch::handled_no_store &&
                   mem_vfs.is_file(file),
               "MemoryVfs: dict_save writes the binary and .ini formats");
        expect(op(WseOpcode::dict_load_file, {dst, 0, 0, format}) ==
                   WseDispatch::handled_no_store &&
                   reads_back(dst, u"hello"),
               "MemoryVfs: dict_load_file reloads the binary and .ini formats");
    }

    // Names that differ only in case address one file, as on Windows.
    {
        const std::int64_t other = create();
        const std::int64_t dst = create();
        str_regs[2] = u"bye";
        expect(op(WseOpcode::dict_set_str, {other, 1, 2}) ==
                   WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_save, {other, 4}) ==
                       WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_load_file, {dst, 0, 0}) ==
                       WseDispatch::handled_no_store &&
                   reads_back(dst, u"bye"),
               "dict_save to a differently-cased name overwrites the same file");
        std::vector<wse::platform::VfsEntry> entries;
        std::size_t wsedict_files = 0;
        if (mem_vfs.list_dir("", entries)) {
            for (const auto& entry : entries) {
                if (entry.name.ends_with(".wsedict")) ++wsedict_files;
            }
        }
        expect(wsedict_files == 1, "case-colliding saves leave one .wsedict");
        str_regs[2] = u"hello";
    }

    // Without a Vfs every file opcode is a harmless no-op.
    {
        store.vfs = nullptr;
        const std::int64_t dst = create();
        expect(op(WseOpcode::dict_set_str, {dst, 1, 5}) ==
                   WseDispatch::handled_no_store,
               "storage edges: no-Vfs dictionary populated");
        expect(op(WseOpcode::dict_save, {dst, 0}) ==
                   WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_save, {dst, 0, 1}) ==
                       WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_save_json, {dst, 0}) ==
                       WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_delete_file, {0}) ==
                       WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_delete_file_json, {0}) ==
                       WseDispatch::handled_no_store,
               "dict save/delete opcodes are no-ops without a Vfs");
        expect(op(WseOpcode::dict_load_file, {dst, 0, 0}) ==
                   WseDispatch::handled_no_store &&
                   op(WseOpcode::dict_load_file_json, {dst, 0, 0}) ==
                       WseDispatch::handled_no_store &&
                   reads_back(dst, u"bye"),
               "dict load opcodes leave the dictionary alone without a Vfs");
        expect(mem_vfs.is_file("edge.wsedict") && mem_vfs.is_file("edge.ini"),
               "no-Vfs opcodes do not touch the detached store");
        std::string stale = "stale";
        expect(!wse::vm::wse_dict_read_file_bytes(nullptr, "edge.wsedict", stale) &&
                   stale.empty(),
               "wse_dict_read_file_bytes clears its output without a Vfs");
        stale = "stale";
        expect(!wse::vm::wse_dict_read_file_bytes(&mem_vfs, "absent.wsedict", stale) &&
                   stale.empty(),
               "wse_dict_read_file_bytes clears its output on a missing file");
    }

    // A host save directory that does not exist yet is created by the first
    // save instead of the save being dropped.
    {
        std::error_code ec;
        const fs::path fresh =
            fs::temp_directory_path() / "wse2_dict_unit_test_fresh" / "dicts";
        fs::remove_all(fresh.parent_path(), ec);
        wse::platform::HostVfs fresh_vfs(fresh);
        store.vfs = &fresh_vfs;
        const std::int64_t dst = create();
        expect(op(WseOpcode::dict_load_file, {dst, 0, 0}) ==
                   WseDispatch::handled_no_store &&
                   !fs::exists(fresh, ec),
               "dict_load_file does not create a missing host root");
        expect(op(WseOpcode::dict_save, {src, 0}) ==
                   WseDispatch::handled_no_store &&
                   fs::exists(fresh / "edge.wsedict", ec),
               "dict_save creates a missing host root");
        expect(op(WseOpcode::dict_load_file, {dst, 4, 0}) ==
                   WseDispatch::handled_no_store &&
                   reads_back(dst, u"hello"),
               "dict_load_file finds a host file case-insensitively");
        store.vfs = nullptr;
        fs::remove_all(fresh.parent_path(), ec);
    }
}

}  // namespace

namespace wse::selftest {

int run_wse_dict_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_dictionary_group();
    test_dictionary_memory_vfs();
    test_dictionary_storage_edges();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("wse_dict tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_dict_suite(nullptr);
}
#endif
