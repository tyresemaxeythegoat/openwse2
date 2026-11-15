// Host tests for the portable system, register, slot, scalar config, skill, savegame, and position executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include "wse_test_common.h"

namespace {

using namespace wse::test;

void test_register_group() {
    std::array<std::int64_t, wse::vm::kFpRegisterCount> bank{};
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    bank[5] = 77;
    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 5}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled && effect.store_value &&
               effect.value == 77,
           "register_get reads the int64 register bank");

    dispatch = run(opcode(WseOpcode::register_set), {0, 99}, {3}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled_no_store && !effect.store_value &&
               bank[3] == 99,
           "register_set writes operand 1 into the named register");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 3}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled && effect.value == 99,
           "register_set and register_get share the same bank");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 0}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 5,
           "store_wse_version 0 reports WSE version 5");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 1}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0x483,
           "store_wse_version 1 reports Warband build 1155");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 2}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_wse_version defaults an unknown argument to 0");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 128}, effect, {}, bank);
    expect(dispatch == WseDispatch::ignored,
           "register_get rejects register 128 (mbCheckRegister accepts 0..127)");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, -1}, effect, {}, bank);
    expect(dispatch == WseDispatch::ignored,
           "register_get rejects a negative register index");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 5}, effect);
    expect(dispatch == WseDispatch::ignored,
           "register_get without a register bank is a no-op");

    dispatch = run(opcode(WseOpcode::register_get), {0}, {0}, effect, {}, bank);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "register_get rejects a missing register index");

    dispatch = run(opcode(WseOpcode::register_set), {0}, {}, effect, {}, bank);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "register_set rejects a missing destination register");

    dispatch = run(opcode(WseOpcode::store_wse_version), {}, {0}, effect);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "store_wse_version rejects a missing selector");
}

void test_script_context_group() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;
    WseVmState state;

    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_current_trigger is 0 outside a trigger");

    state.caller = -3;
    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == -3,
           "store_current_trigger returns the negative trigger context");

    state.caller = 7;
    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_current_trigger ignores a positive context");

    state = {};
    state.num_return_values = 3;
    dispatch = run(opcode(WseOpcode::store_num_return_values), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 3,
           "store_num_return_values reads the count");

    std::array<std::int64_t, 16> returns{11, 22, 33};
    state.return_values = returns;
    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 2}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == 22,
           "store_return_value reads the 1-based slot");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 4}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects an unset slot");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 0}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects index 0");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 17}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects index 17");

    dispatch = run(opcode(WseOpcode::store_return_value), {0}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "store_return_value rejects a missing index");

    state = {};
    state.app_time_seconds = 1.5;
    dispatch = run(opcode(WseOpcode::store_application_time), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 1500,
           "store_application_time converts seconds to milliseconds");

    state.app_time_seconds = 2.25;
    dispatch = run(opcode(WseOpcode::store_application_time), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 2250,
           "store_application_time rounds to the nearest millisecond");

    state = {};
    dispatch = run(opcode(WseOpcode::store_cur_mission_template_no), {}, {}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "store_cur_mission_template_no is -1 without a mission");

    std::int32_t template_no = 42;
    state.current_mission_template_no = &template_no;
    dispatch = run(opcode(WseOpcode::store_cur_mission_template_no), {}, {}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == 42,
           "store_cur_mission_template_no reads the active template");
}

void test_options_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options;
    WseVmState state;
    state.options = &options;

    expect(run(opcode(WseOpcode::options_get_verbose_casualties), {}, {},
               effect) == WseDispatch::handled &&
               effect.value == 0,
           "options_get_verbose_casualties defaults false with null options");
    expect(run(opcode(WseOpcode::options_set_verbose_casualties), {}, {1},
               effect) == WseDispatch::ignored,
           "options_set_verbose_casualties ignores null options state");

    const struct {
        WseOpcode get;
        WseOpcode set;
        const char* label;
    } kCases[] = {
        {WseOpcode::options_get_verbose_casualties,
         WseOpcode::options_set_verbose_casualties, "verbose_casualties"},
        {WseOpcode::options_get_cheat_mode, WseOpcode::options_set_cheat_mode,
         "cheat_mode"},
        {WseOpcode::options_get_realistic_headshots,
         WseOpcode::options_set_realistic_headshots, "realistic_headshots"},
    };
    for (const auto& entry : kCases) {
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 0,
               entry.label);
        expect(run(opcode(entry.set), {}, {7}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
                   !effect.store_value,
               entry.label);
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 1,
               entry.label);
        expect(run(opcode(entry.set), {}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store,
               entry.label);
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 0,
               entry.label);
        expect(run(opcode(entry.set), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::operand_count_out_of_range,
               entry.label);
    }
}

void test_script_state_and_position_group() {
    std::u16string str_regs[128] = {};
    const std::u16string_view global_strs[] = {
        u"marker_global",  // 0
        u"bad_pos_val",    // 1
    };
    const std::u16string_view local_strs[] = {
        u"marker_local",   // 0
    };
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<std::int64_t, 16> return_vals{};
    std::int32_t num_returns = 0;
    std::array<wse::vm::WseTransform, 128> positions{};
    std::array<wse::vm::WseAppTimer, 128> timers{};
    std::array<std::int64_t, 3> timers_total{1000000LL, 2000000LL, 5000000LL};
    wse::vm::WseMersenneTwister rng{};
    std::int32_t rand_seed = 0;
    bool profiler_recording = false;
    std::vector<std::string> profiler_markers{};

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.strings.global = std::span<const std::u16string_view>(global_strs);
    state.strings.local = std::span<const std::u16string_view>(local_strs);
    state.return_values = return_vals;
    state.return_values_out = return_vals;
    state.num_return_values = 0;
    state.num_return_values_out = &num_returns;
    state.positions = positions;
    state.timers = timers;
    state.timers_total = timers_total;
    state.rng = &rng;
    state.rand_seed = &rand_seed;
    state.profiler_recording = &profiler_recording;
    state.profiler_markers = &profiler_markers;

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
        operands.state.return_values = return_vals;
        operands.state.num_return_values = num_returns;
        effect = WseEffect{0x1234, true};
        return execute_wse(opcode(op), operands, effect);
    };

    // 3015 return_values -> 3016 store_num_return_values / 3017 store_return_value.
    expect(call(WseOpcode::return_values, {11, 22, 33}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 3 && return_vals[0] == 11 &&
               return_vals[2] == 33,
           "return_values writes up to 16 int64 slots");
    expect(call(WseOpcode::store_num_return_values, {0}) ==
                   WseDispatch::handled &&
               effect.value == 3,
           "store_num_return_values reads updated count");
    expect(call(WseOpcode::store_return_value, {0, 2}) ==
                   WseDispatch::handled &&
               effect.value == 22,
           "store_return_value reads 1-based slot after return_values");
    expect(call(WseOpcode::return_values,
                {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 16 && return_vals[15] == 16,
           "return_values clamps operand list to 16 slots");
    expect(call(WseOpcode::store_return_value, {0, 17}) ==
                   WseDispatch::ignored &&
               effect.value == 0x1234,
           "store_return_value rejects slot 17 beyond 16");
    expect(call(WseOpcode::return_values, {}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 0,
           "return_values accepts zero operands and clears count");

    // 3020 mtsrand & 3021 mtrand.
    expect(call(WseOpcode::mtsrand, {}) == WseDispatch::operand_count_out_of_range,
           "mtsrand requires 1 operand");
    expect(call(WseOpcode::mtrand, {0, 10}) ==
               WseDispatch::operand_count_out_of_range,
           "mtrand requires 3 operands");
    expect(call(WseOpcode::mtsrand, {1337}) == WseDispatch::handled_no_store,
           "mtsrand seeds MT19937");
    std::vector<std::int64_t> seq1;
    for (int i = 0; i < 6; ++i) {
        expect(call(WseOpcode::mtrand, {0, -5, 15}) == WseDispatch::handled &&
                   effect.value >= -5 && effect.value < 15,
               "mtrand produces values in [min, max)");
        seq1.push_back(effect.value);
    }
    expect(call(WseOpcode::mtsrand, {1337}) == WseDispatch::handled_no_store,
           "mtsrand re-seeds reproducibly");
    // Edge case: min == max - 1 (span == 0) returns min without advancing RNG.
    expect(call(WseOpcode::mtrand, {0, 42, 43}) == WseDispatch::handled &&
               effect.value == 42,
           "mtrand handles min == max - 1 without drawing");
    for (int i = 0; i < 6; ++i) {
        expect(call(WseOpcode::mtrand, {0, -5, 15}) == WseDispatch::handled &&
                   effect.value == seq1[static_cast<std::size_t>(i)],
               "mtrand min == max - 1 preserves exact PRNG stream");
    }
    // Edge case: min == max wraps unsigned span to 0xffffffffU and draws once cleanly.
    expect(call(WseOpcode::mtrand, {0, 5, 5}) == WseDispatch::handled,
           "mtrand min == max completes without hanging");
    {
        WseVmState no_rng = state;
        no_rng.rng = nullptr;
        std::array<std::int32_t, 3> args{0, 1, 10};
        WseOperands ops;
        ops.narrow = args;
        ops.state = no_rng;
        expect(execute_wse(opcode(WseOpcode::mtrand), ops, effect) ==
                   WseDispatch::ignored,
               "mtrand ignores missing RNG pointer");
    }

    // 3022 get_time (UTC & local).
    expect(call(WseOpcode::get_time, {}) ==
               WseDispatch::operand_count_out_of_range,
           "get_time requires 1 operand");
    const std::time_t sys_before = std::time(nullptr);
    expect(call(WseOpcode::get_time, {0}) == WseDispatch::handled,
           "get_time UTC succeeds");
    const std::int64_t utc_val = effect.value;
    const std::time_t sys_after = std::time(nullptr);
    expect(utc_val >= static_cast<std::int32_t>(sys_before) - 2 &&
               utc_val <= static_cast<std::int32_t>(sys_after) + 2,
           "get_time returns current UTC unix epoch seconds");
    expect(call(WseOpcode::get_time, {0, 1}) == WseDispatch::handled,
           "get_time local succeeds");
    const std::int64_t local_val = effect.value;
    const std::time_t mid_t = static_cast<std::time_t>(utc_val);
    std::tm lt{};
    bool has_lt = false;
#ifdef _WIN32
    has_lt = localtime_s(&lt, &mid_t) == 0;
#else
    has_lt = localtime_r(&mid_t, &lt) != nullptr;
#endif
    if (has_lt) {
        const std::int64_t expected_local =
            wse::vm::wse_days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1,
                                         lt.tm_mday) *
                86400LL +
            static_cast<std::int64_t>(lt.tm_hour) * 3600LL +
            static_cast<std::int64_t>(lt.tm_min) * 60LL +
            static_cast<std::int64_t>(lt.tm_sec);
        expect(local_val >= expected_local - 2 &&
                   local_val <= expected_local + 2,
               "get_time(local=1) applies local timezone offset");
    }

    // With a WallClock installed the result is exact and host-independent.
    {
        wse::platform::ManualWallClock wall;
        wall.set(1'700'000'000, 19800);
        state.wall_clock = &wall;
        expect(call(WseOpcode::get_time, {0}) == WseDispatch::handled &&
                   effect.value == 1'700'000'000,
               "get_time reads UTC seconds from the installed WallClock");
        expect(call(WseOpcode::get_time, {0, 1}) == WseDispatch::handled &&
                   effect.value == 1'700'019'800,
               "get_time(local=1) adds the WallClock UTC offset");
        wall.set(0x1'0000'0005LL, 0);
        expect(call(WseOpcode::get_time, {0}) == WseDispatch::handled &&
                   effect.value == 5,
               "get_time truncates to 32 bits");
        state.wall_clock = nullptr;
    }

    // 3027 timer_reset & 3028 timer_get_elapsed_time (100kHz ticks).
    expect(call(WseOpcode::timer_reset, {}) ==
               WseDispatch::operand_count_out_of_range,
           "timer_reset requires 1 operand");
    expect(call(WseOpcode::timer_get_elapsed_time, {0}) ==
               WseDispatch::operand_count_out_of_range,
           "timer_get_elapsed_time requires 2 operands");
    expect(call(WseOpcode::timer_reset, {0, 1}) ==
                   WseDispatch::handled_no_store &&
               timers[0].kind == 0 && timers[0].start_ticks == 1000000LL,
           "timer_reset nonzero op1 selects app clock (kind 0)");
    expect(call(WseOpcode::timer_reset, {1}) ==
                   WseDispatch::handled_no_store &&
               timers[1].kind == 2 && timers[1].start_ticks == 5000000LL,
           "timer_reset default/0 op1 selects game clock (kind 2)");
    timers_total[0] += 150000LL;  // 1.500s -> rglRound64(1.5f) == 1
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 1,
           "timer_get_elapsed_time rounds exact 1.5s down matching rglRound64");
    timers_total[0] += 1000LL;    // 1.510s -> 2
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 2,
           "timer_get_elapsed_time uses 100kHz scale");
    timers_total[2] += 325000LL;  // 3.25s -> 3
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 1}) ==
                   WseDispatch::handled &&
               effect.value == 3,
           "timer_get_elapsed_time tracks kind 2 clock independently");
    expect(call(WseOpcode::timer_reset, {128, 1}) == WseDispatch::ignored &&
               call(WseOpcode::timer_get_elapsed_time, {0, -1}) ==
                   WseDispatch::ignored,
           "timer ops ignore out-of-range timer slot");

    // 3034 set_random_seed.
    expect(call(WseOpcode::set_random_seed, {}) ==
               WseDispatch::operand_count_out_of_range,
           "set_random_seed requires 1 operand");
    expect(call(WseOpcode::set_random_seed, {98765}) ==
                   WseDispatch::handled_no_store &&
               rand_seed == 98765,
           "set_random_seed updates state.rand_seed");

    // 3040..3043 profiler_start/stop/is_recording/mark.
    expect(call(WseOpcode::profiler_is_recording, {}) ==
               WseDispatch::condition_false,
           "profiler_is_recording initially false");
    str_regs[0] = u"ignored_marker";
    expect(call(WseOpcode::profiler_mark, {0}) ==
                   WseDispatch::handled_no_store &&
               profiler_markers.empty(),
           "profiler_mark ignores markers when not recording");
    expect(call(WseOpcode::profiler_start, {}) ==
                   WseDispatch::handled_no_store &&
               profiler_recording,
           "profiler_start enables recording");
    expect(call(WseOpcode::profiler_is_recording, {}) ==
               WseDispatch::handled_no_store,
           "profiler_is_recording returns true while active");
    str_regs[0] = u"frame_0";
    expect(call(WseOpcode::profiler_mark, {0}, {0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::profiler_mark, {0}, {3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::profiler_mark, {0}, {0x16}) ==
                   WseDispatch::handled_no_store,
           "profiler_mark records across tag banks");
    expect(profiler_markers.size() == 3U && profiler_markers[0] == "frame_0" &&
               profiler_markers[1] == "marker_global" &&
               profiler_markers[2] == "marker_local",
           "profiler_mark appends UTF-8 markers");
    expect(call(WseOpcode::profiler_stop, {}) ==
                   WseDispatch::handled_no_store &&
               !profiler_recording &&
               call(WseOpcode::profiler_is_recording, {}) ==
                   WseDispatch::condition_false,
           "profiler_stop disables recording");
    expect(call(WseOpcode::profiler_mark, {0}) ==
                   WseDispatch::handled_no_store &&
               profiler_markers.size() == 3U,
           "profiler_mark stops recording markers after profiler_stop");
    expect(call(WseOpcode::profiler_mark, {}) ==
               WseDispatch::operand_count_out_of_range,
           "profiler_mark requires 1 operand");

    // 4100 position_get_vector_to_position, 4102..4104, 4402..4404 fld_pos_*.
    positions[1].o = {1.0f, 2.0f, 3.0f, 1.0f};
    positions[2].o = {1.0f, 6.0f, 6.0f, 1.0f};
    expect(call(WseOpcode::position_get_vector_to_position, {0, 0, 1, 2}) ==
                   WseDispatch::handled &&
               effect.value == 500,
           "position_get_vector_to_position returns scaled distance");
    expect(close(positions[0].o.x, 1.0f) && close(positions[0].o.y, 2.0f) &&
               close(positions[0].o.z, 3.0f) &&
               close(positions[0].f.x, 0.0f) &&
               close(positions[0].f.y, 0.8f) &&
               close(positions[0].f.z, 0.6f),
           "position_get_vector_to_position sets origin and aimed forward basis");
    expect(close(positions[0].s.length(), 1.0f) &&
               close(positions[0].u.length(), 1.0f) &&
               close(positions[0].s.dot(positions[0].f), 0.0f) &&
               close(positions[0].s.dot(positions[0].u), 0.0f) &&
               close(positions[0].f.dot(positions[0].u), 0.0f),
           "position_get_vector_to_position yields orthonormal basis");
    // Aliasing test: dst == pos1.
    positions[3].o = {3.0f, 4.0f, 12.0f, 1.0f};
    positions[4].o = {0.0f, 0.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::position_get_vector_to_position, {0, 3, 3, 4}) ==
                   WseDispatch::handled &&
               effect.value == 1300 && close(positions[3].o.x, 3.0f) &&
               close(positions[3].f.x, -3.0f / 13.0f) &&
               close(positions[3].f.y, -4.0f / 13.0f) &&
               close(positions[3].f.z, -12.0f / 13.0f),
           "position_get_vector_to_position handles dst == pos1 aliasing");
    // 4102 position_get_length.
    positions[5].o = {3.0f, 4.0f, 12.0f, 1.0f};
    expect(call(WseOpcode::position_get_length, {0, 5}) ==
                   WseDispatch::handled &&
               effect.value == 1300,
           "position_get_length returns fixed-point vector length");
    // 4103 get_dot_product_of_positions.
    positions[6].o = {1.0f, 2.0f, 3.0f, 1.0f};
    positions[7].o = {4.0f, -5.0f, 6.0f, 1.0f};
    expect(call(WseOpcode::get_dot_product_of_positions, {0, 6, 7}) ==
                   WseDispatch::handled &&
               effect.value == 1200,
           "get_dot_product_of_positions computes fixed-point dot product");
    // 4104 get_cross_product_of_positions (including aliasing dst == pos1).
    positions[8].o = {1.0f, 0.0f, 0.0f, 1.0f};
    positions[9].o = {0.0f, 2.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::get_cross_product_of_positions, {8, 8, 9}) ==
                   WseDispatch::handled_no_store &&
               close(positions[8].o.x, 0.0f) &&
               close(positions[8].o.y, 0.0f) &&
               close(positions[8].o.z, 2.0f),
           "get_cross_product_of_positions handles cross product with aliasing");
    // 4402..4404 fld_pos_{x,y,z}.
    positions[10].o = {-1.25f, 3.5f, 9.75f, 1.0f};
    expect(call(WseOpcode::fld_pos_x, {4, 10}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::fld_pos_y, {5, 10}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::fld_pos_z, {6, 10}) ==
                   WseDispatch::handled_no_store &&
               close(fp[4], -1.25f) && close(fp[5], 3.5f) &&
               close(fp[6], 9.75f),
           "fld_pos_x/y/z copy origin coordinates to fp registers");
    expect(call(WseOpcode::fld_pos_x, {128, 10}) == WseDispatch::ignored &&
               call(WseOpcode::fld_pos_y, {4, 128}) == WseDispatch::ignored &&
               call(WseOpcode::position_get_length, {0, -1}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::get_dot_product_of_positions, {0, 1, 128}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::get_cross_product_of_positions, {128, 1, 2}) ==
                   WseDispatch::ignored,
           "position/fld_pos ops ignore invalid registers");
    expect(call(WseOpcode::fld_pos_z, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::position_get_vector_to_position, {0, 1, 2}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::position_get_length, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::get_dot_product_of_positions, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::get_cross_product_of_positions, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range,
           "position/fld_pos ops validate operand counts");
}

void test_descriptions() {
    using wse::vm::describe;
    expect(!describe(WseDispatch::handled).empty(), "handled description");
    expect(!describe(WseDispatch::handled_no_store).empty(),
           "no-store description");
    expect(!describe(WseDispatch::condition_false).empty(),
           "condition description");
    expect(!describe(WseDispatch::ignored).empty(), "ignored description");
    expect(!describe(WseDispatch::unimplemented).empty(), "unimplemented description");
    expect(!describe(WseDispatch::operand_count_out_of_range).empty(),
           "operand count description");
}

void test_slot_gt_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.items.push_back(wse::game::WseItemKind{});
    world.items[0].slots.set(4, 15);
    world.party_templates.push_back(wse::game::WsePartyTemplate{});
    world.party_templates[0].slots.set(1, 20);
    world.troops.push_back(wse::game::WseTroop{});
    world.troops[0].slots.set(7, 50);
    world.factions.push_back(wse::game::WseFaction{});
    world.factions[0].slots.set(2, -3);
    world.quests.push_back(wse::game::WseQuest{});
    world.quests[0].slots.set(0, 9);
    world.sites.push_back(wse::game::WseSite{});
    world.sites[0].slots.set(3, 42);
    world.parties.resize(2);
    world.parties[0].active = true;
    world.parties[0].slots.set(5, 100);
    world.parties[1].active = false;
    world.parties[1].slots.set(5, 999);
    world.players.resize(2);
    world.players[0].status = 2;
    world.players[0].slots.set(6, 77);
    world.players[1].status = 1;
    world.players[1].slots.set(6, 999);
    world.teams[7].slots.set(8, 88);
    world.agents.resize(2);
    world.agents[0].active = true;
    world.agents[0].slots.set(9, 64);
    world.agents[1].active = false;
    world.agents[1].slots.set(9, 999);
    world.mission_objects.resize(2);
    world.mission_objects[0].active = true;
    world.mission_objects[0].slots.set(10, 33);
    world.mission_objects[1].active = false;
    world.mission_objects[1].slots.set(10, 999);

    WseVmState state{};
    state.world = &world;

    auto check_slot = [&](WseOpcode op, std::int64_t id, std::int64_t slot,
                          std::int64_t rhs) {
        return run(opcode(op), {id, slot, rhs},
                   {static_cast<std::int32_t>(id),
                    static_cast<std::int32_t>(slot),
                    static_cast<std::int32_t>(rhs)},
                   effect, {}, {}, state);
    };

    expect(check_slot(WseOpcode::item_slot_gt, 0, 4, 14) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::item_slot_gt, 0, 4, 15) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_template_slot_gt, 0, 1, 19) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::party_template_slot_gt, 0, 1, 20) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::troop_slot_gt, 0, 7, 49) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::troop_slot_gt, 0, 7, 50) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -4) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -3) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 8) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 9) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 41) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 42) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 99) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 100) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::player_slot_gt, 0, 6, 76) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::player_slot_gt, 0, 6, 77) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::team_slot_gt, 7, 8, 87) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::team_slot_gt, 7, 8, 88) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::agent_slot_gt, 0, 9, 63) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::agent_slot_gt, 0, 9, 64) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_prop_slot_gt, 0, 10, 32) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::scene_prop_slot_gt, 0, 10, 33) ==
                   WseDispatch::condition_false,
           "all 11 *_slot_gt opcodes evaluate true and false boundaries");

    // Unset valid slots read 0; out-of-range slot indices and inactive entities fail.
    expect(check_slot(WseOpcode::item_slot_gt, 0, 0xfffff, -5) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::item_slot_gt, 0, 0xfffff, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::item_slot_gt, 0, -1, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::item_slot_gt, 0, 0x100000, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 1, 5, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::player_slot_gt, 1, 6, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::agent_slot_gt, 1, 9, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_prop_slot_gt, 1, 10, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::team_slot_gt, 8, 0, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::troop_slot_gt, -1, 0, -5) ==
                   WseDispatch::condition_false &&
               run(opcode(WseOpcode::troop_slot_gt), {0, 0}, {0, 0}, effect, {},
                   {}, state) == WseDispatch::operand_count_out_of_range,
           "*_slot_gt enforces slot range 0..0xfffff, entity activity, and operand count");

    world.has_game = false;
    expect(check_slot(WseOpcode::troop_slot_gt, 0, 7, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -10) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 0) ==
                   WseDispatch::condition_false,
           "game-scoped *_slot_gt handlers fail cleanly when !has_game");
}

void test_scalar_config_skill_savegame_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options{};
    std::array<std::u16string, 128> registers{};
    wse::game::WseWorldState world{};
    world.parties.resize(3);
    world.parties[0].active = true;
    world.parties[1].active = false;
    world.parties[2].active = true;
    world.skills[4].id = "skl_trade";
    world.skills[4].name = "Trade";
    world.skills[4].description = "Base trade description.";
    world.skills[4].flags = wse::game::kSkillPartyFlagBit;
    world.skills[5].id = "skl_ironflesh";
    world.skills[5].name = "Ironflesh";
    world.skills[5].description = "Base ironflesh description.";
    world.skills[5].flags = 0u;
    world.translations["skl_trade_desc"] = "Translated trade description.";
    world.module_settings["net_config"]["max_players"] = "64";
    world.game_variables["hero_bonus"] = "125";

    const auto save_dir =
        std::filesystem::temp_directory_path() / "wse_test_savegame_md5_dir";
    std::error_code ec;
    std::filesystem::remove_all(save_dir, ec);
    std::filesystem::create_directories(save_dir, ec);
    {
        std::ofstream empty_sav(save_dir / "sg00.sav", std::ios::binary);
    }
    {
        std::ofstream abc_sav(save_dir / "sg09.sav", std::ios::binary);
        abc_sav.write("abc", 3);
    }
    wse::platform::HostVfs save_vfs(save_dir);
    world.savegames_vfs = &save_vfs;

    WseVmState state{};
    state.options = &options;
    state.world = &world;
    state.strings = {registers, {}, {}};

    // sleep_ms (3026).
    expect(run(opcode(WseOpcode::sleep_ms), {-1}, {-1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::sleep_ms), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::sleep_ms), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store,
           "sleep_ms skips negative durations and handles 0/1 ms");

    // set_main_party / get_main_party (3030 / 3031).
    expect(run(opcode(WseOpcode::set_main_party), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::set_main_party), {2}, {2}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::get_main_party), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == 2 && effect.store_value,
           "set_main_party validates active parties and get_main_party reads back");

    // is_party_skill (3036).
    expect(run(opcode(WseOpcode::is_party_skill), {4}, {4}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::is_party_skill), {5}, {5}, effect, {}, {}, state) ==
                   WseDispatch::condition_false &&
               run(opcode(WseOpcode::is_party_skill), {42}, {42}, effect, {}, {}, state) ==
                   WseDispatch::ignored,
           "is_party_skill checks bit 4 across 0..41");

    // campaign_time (3037 / 3038) & show_hud (3045).
    expect(run(opcode(WseOpcode::set_campaign_time), {0x123456789abcLL}, {0},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::get_campaign_time), {0}, {0}, effect, {}, {},
                   state) == WseDispatch::handled &&
               effect.value == 0x123456789abcLL &&
               run(opcode(WseOpcode::show_hud), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.hide_hud &&
               run(opcode(WseOpcode::show_hud), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               !world.hide_hud,
           "campaign_time keeps 64-bit values and show_hud inverts into hide_hud");

    // set_show_use_tooltip (3601), stop_time (3608), get_water_level (3610).
    world.water_level = 2.506f;
    state.fixed_point_multiplier = 100;
    expect(run(opcode(WseOpcode::set_show_use_tooltip), {3, 1}, {3, 1}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               world.use_objects_tooltip_enabled[3] &&
               run(opcode(WseOpcode::set_show_use_tooltip), {3}, {3}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               !world.use_objects_tooltip_enabled[3] &&
               run(opcode(WseOpcode::set_show_use_tooltip), {11, 1}, {11, 1},
                   effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::stop_time), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               !world.paused,
           "set_show_use_tooltip validates 0..10 and stop_time requires cheat_mode");

    options.cheat_mode = true;
    expect(run(opcode(WseOpcode::stop_time), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.paused &&
               run(opcode(WseOpcode::stop_time), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               !world.paused &&
               run(opcode(WseOpcode::get_water_level), {0}, {0}, effect, {}, {},
                   state) == WseDispatch::handled &&
               effect.value == 251,
           "stop_time toggles m_paused in cheat_mode and get_water_level rounds via rglRound64");

    // 3619 / 3622 options flags.
    expect(!options.horse_friendly_fire && options.show_crosshair &&
               run(opcode(WseOpcode::set_horse_friendly_fire), {1}, {1}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               options.horse_friendly_fire &&
               run(opcode(WseOpcode::set_show_crosshair), {0}, {0}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               !options.show_crosshair,
           "set_horse_friendly_fire and set_show_crosshair match retail defaults and toggles");

    // 4226 / 4231 module setting & game variable with {sN} parse + spacesToUnderscores.
    registers[1] = u"max {s3}";
    registers[2] = u"net config";
    registers[3] = u"players";
    registers[4] = u"Max Players";
    registers[5] = u"hero bonus";
    expect(run(opcode(WseOpcode::str_store_module_setting), {0, 1, 2},
               {0, 1, 2}, effect, {}, {}, state, {0, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0] == u"64" &&
               run(opcode(WseOpcode::str_store_module_setting), {0, 4, 2},
                   {0, 4, 2}, effect, {}, {}, state, {0, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0].empty() &&
               run(opcode(WseOpcode::str_store_game_variable), {0, 5}, {0, 5},
                   effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0] == u"125",
           "str_store_module_setting and str_store_game_variable normalize spaces case-sensitively");

    // 4232 / 4245 str_store_skill_name / str_store_skill_desc.
    expect(run(opcode(WseOpcode::str_store_skill_name), {0, 4}, {0, 4}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Trade" &&
               run(opcode(WseOpcode::str_store_skill_desc), {0, 4}, {0, 4}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Translated trade description." &&
               run(opcode(WseOpcode::str_store_skill_desc), {0, 5}, {0, 5}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Base ironflesh description." &&
               run(opcode(WseOpcode::str_store_skill_name), {0, 42}, {0, 42},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "str_store_skill_{name,desc} support _desc translations and fallback text");

    // 4246 str_store_savegame_md5.
    registers[0] = u"stale";
    expect(run(opcode(WseOpcode::str_store_savegame_md5), {0, 12}, {0, 12},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0].empty(),
           "str_store_savegame_md5 clears destination when savegame is missing");
    registers[0] = u"stale";
    expect(run(opcode(WseOpcode::str_store_savegame_md5), {0, 0}, {0, 0},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0].empty() &&
               run(opcode(WseOpcode::str_store_savegame_md5), {0, 9}, {0, 9},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"900150983cd24fb0d6963f7d28e17f72",
           "str_store_savegame_md5 skips empty files and formats sg%d%d.sav MD5");

    wse::platform::MemoryVfs mem_save_vfs;
    expect(mem_save_vfs.write_text("sg12.sav", "abc"),
           "MemoryVfs::write_text seeds sg12.sav");
    world.savegames_vfs = &mem_save_vfs;
    registers[0] = u"stale";
    expect(run(opcode(WseOpcode::str_store_savegame_md5), {0, 12}, {0, 12},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"900150983cd24fb0d6963f7d28e17f72",
           "str_store_savegame_md5 reads sg12.sav through MemoryVfs");
    world.savegames_vfs = nullptr;

    std::filesystem::remove_all(save_dir, ec);
}

void test_sleep_and_dispatch_b8() {
    WseOperands b8_ops{};
    WseEffect b8_eff{};
    static_assert(!noexcept(execute_wse(0u, b8_ops, b8_eff)),
                  "execute_wse must not be noexcept");
    static_assert(!noexcept(execute_wse_system(0u, b8_ops, b8_eff)),
                  "execute_wse_system must not be noexcept");

    struct SleepLog {
        int calls = 0;
        std::int32_t last_ms = -1;
    } log;

    WseVmState state;
    state.sleep_ms_user = &log;
    state.sleep_ms_fn = [](void* user, std::int32_t ms) {
        auto* out = static_cast<SleepLog*>(user);
        ++out->calls;
        out->last_ms = ms;
    };

    WseEffect effect;
    expect(run(opcode(WseOpcode::sleep_ms), {25}, {25}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               log.calls == 1 && log.last_ms == 25,
           "sleep_ms delegates positive duration to platform sleep_ms_fn hook");
    expect(run(opcode(WseOpcode::sleep_ms), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               log.calls == 2 && log.last_ms == 0,
           "sleep_ms invokes sleep_ms_fn hook for 0ms");
    expect(run(opcode(WseOpcode::sleep_ms), {-5}, {-5}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               log.calls == 2,
           "sleep_ms skips hook on negative duration");
    expect(run(opcode(WseOpcode::sleep_ms), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::operand_count_out_of_range,
           "sleep_ms rejects missing operand");

    state.sleep_ms_fn = nullptr;
    expect(run(opcode(WseOpcode::sleep_ms), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store,
           "sleep_ms fallback succeeds when sleep_ms_fn is null");
}

}  // namespace

namespace wse::selftest {

int run_wse_system_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_register_group();
    test_script_context_group();
    test_options_group();
    test_script_state_and_position_group();
    test_descriptions();
    test_slot_gt_group();
    test_scalar_config_skill_savegame_group();
    test_sleep_and_dispatch_b8();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("wse_system tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_system_suite(nullptr);
}
#endif
