// Host tests for the portable binary stream, RglString, trigger, and container savegames.
//
// Build/run: powershell -File engine/build_host.ps1

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "wse/core/rgl_string.h"
#include "wse/game/savegame.h"
#include "wse/game/trigger.h"
#include "wse/io/binary_stream.h"
#include "wse/platform/vfs.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

int byte_at(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<int>(bytes[index]);
}

void test_binary_writer() {
    using namespace wse;

    io::BinaryWriter writer;
    writer.write_i32(-2);
    writer.write_u32(0x01020304U);
    writer.write_i64(0x0102030405060708LL);
    writer.write_bool(true);
    writer.write_byte(0xABU);
    writer.write_f32(1.0F);

    const std::span<const std::byte> bytes = writer.bytes();
    expect(writer.size() == 22, "binary writer size");
    expect(byte_at(bytes, 0) == 0xFE && byte_at(bytes, 3) == 0xFF, "int32 is little-endian");
    expect(byte_at(bytes, 4) == 0x04 && byte_at(bytes, 7) == 0x01, "uint32 is little-endian");
    expect(byte_at(bytes, 8) == 0x08 && byte_at(bytes, 15) == 0x01,
           "int64 is little-endian");
    expect(byte_at(bytes, 16) == 0x01, "bool writes one byte");
    expect(byte_at(bytes, 17) == 0xAB, "byte writes the raw value");
    expect(byte_at(bytes, 18) == 0x00 && byte_at(bytes, 19) == 0x00 &&
               byte_at(bytes, 20) == 0x80 && byte_at(bytes, 21) == 0x3F,
           "float uses its four raw bytes");

    io::BinaryReader reader(bytes);
    std::int32_t i32 = 0;
    std::uint32_t u32 = 0;
    std::int64_t i64 = 0;
    bool boolean = false;
    std::uint8_t byte = 0;
    float f32 = 0.0F;
    expect(reader.read_i32(i32) && i32 == -2, "int32 round-trip");
    expect(reader.read_u32(u32) && u32 == 0x01020304U, "uint32 round-trip");
    expect(reader.read_i64(i64) && i64 == 0x0102030405060708LL, "int64 round-trip");
    expect(reader.read_bool(boolean) && boolean, "bool round-trip");
    expect(reader.read_byte(byte) && byte == 0xABU, "byte round-trip");
    expect(reader.read_f32(f32) && f32 == 1.0F, "float round-trip");
    expect(reader.at_end(), "binary reader consumes the buffer");
}

void test_binary_reader_bounds() {
    using namespace wse;

    const std::byte shortBuffer[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    io::BinaryReader reader(shortBuffer);

    std::int32_t value = 42;
    expect(!reader.read_i32(value), "truncated int32 is rejected");
    expect(value == 42, "failed read leaves the output untouched");
    expect(reader.remaining() == 3, "failed read does not advance the cursor");

    std::int64_t wide = 7;
    expect(!reader.read_i64(wide), "truncated int64 is rejected");
    expect(wide == 7, "failed int64 read leaves the output untouched");
}

void test_trigger_savegame_roundtrip() {
    using namespace wse::game;
    using namespace wse;

    TriggerManager source;
    source.triggers.resize(2);
    source.triggers[0].status = 2;
    source.triggers[0].checkTimer.value = -100000;
    source.triggers[0].delayTimer.value = 200000;
    source.triggers[0].rearmTimer.value = 300000;
    source.triggers[1].status = 1;
    source.triggers[1].checkTimer.value = 0x0102030405060708LL;
    source.triggers[1].delayTimer.value = 11;
    source.triggers[1].rearmTimer.value = 12;

    io::BinaryWriter writer;
    write_trigger_savegame(source, writer);

    const std::span<const std::byte> bytes = writer.bytes();
    expect(writer.size() == 4 + 2 * (4 + 3 * 8), "trigger savegame size");
    expect(byte_at(bytes, 0) == 2 && byte_at(bytes, 1) == 0 && byte_at(bytes, 2) == 0 &&
               byte_at(bytes, 3) == 0,
           "trigger savegame starts with the int32 count");
    expect(byte_at(bytes, 4) == 2, "first trigger status follows the count");
    expect(byte_at(bytes, 8) == 0x60 && byte_at(bytes, 9) == 0x79 &&
               byte_at(bytes, 10) == 0xFE && byte_at(bytes, 11) == 0xFF,
           "check timer is a little-endian int64");

    TriggerManager target;
    target.triggers.resize(2);
    target.triggers[0].checkTimer.timerNo = 3;
    target.triggers[1].rearmTimer.timerNo = 4;
    io::BinaryReader reader(bytes);
    expect(read_trigger_savegame(target, reader) == SavegameResult::ok,
           "trigger savegame reads");
    expect(reader.at_end(), "trigger savegame consumes every byte");

    for (std::size_t index = 0; index < source.triggers.size(); ++index) {
        expect(target.triggers[index].status == source.triggers[index].status,
               "saved status round-trips");
        expect(target.triggers[index].checkTimer.value ==
                   source.triggers[index].checkTimer.value,
               "saved check timer round-trips");
        expect(target.triggers[index].delayTimer.value ==
                   source.triggers[index].delayTimer.value,
               "saved delay timer round-trips");
        expect(target.triggers[index].rearmTimer.value ==
                   source.triggers[index].rearmTimer.value,
               "saved rearm timer round-trips");
    }
    expect(target.triggers[0].checkTimer.timerNo == 3 &&
               target.triggers[1].rearmTimer.timerNo == 4,
           "timer numbers are not part of the savegame");
}

void test_trigger_savegame_extra_entries() {
    using namespace wse::game;
    using namespace wse;

    TriggerManager source;
    source.triggers.resize(3);
    source.triggers[0].status = 1;
    source.triggers[0].checkTimer.value = 10;
    source.triggers[1].status = 2;
    source.triggers[2].status = 2;

    io::BinaryWriter writer;
    write_trigger_savegame(source, writer);

    TriggerManager target;
    target.triggers.resize(1);
    target.triggers[0].status = 0;
    io::BinaryReader reader(writer.bytes());
    expect(read_trigger_savegame(target, reader) == SavegameResult::count_exceeded,
           "surplus file triggers report the retail count error");
    expect(target.triggers[0].status == 1 && target.triggers[0].checkTimer.value == 10,
           "the loaded trigger still receives its values");
    expect(reader.at_end(), "surplus file triggers are consumed and discarded");
}

void test_trigger_savegame_short_and_negative() {
    using namespace wse::game;
    using namespace wse;

    {
        TriggerManager source;
        source.triggers.resize(1);
        source.triggers[0].status = 2;
        source.triggers[0].checkTimer.value = 55;
        io::BinaryWriter writer;
        write_trigger_savegame(source, writer);

        TriggerManager target;
        target.triggers.resize(3);
        target.triggers[1].status = 7;
        io::BinaryReader reader(writer.bytes());
        expect(read_trigger_savegame(target, reader) == SavegameResult::ok,
               "a file with fewer triggers is accepted");
        expect(target.triggers[0].checkTimer.value == 55, "present entries are applied");
        expect(target.triggers[1].status == 7, "absent entries are left unchanged");
        expect(reader.at_end(), "short file is fully consumed");
    }

    {
        const std::byte negativeCount[4] = {
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        TriggerManager target;
        target.triggers.resize(1);
        target.triggers[0].status = 4;
        io::BinaryReader reader(negativeCount);
        expect(read_trigger_savegame(target, reader) == SavegameResult::negative_count,
               "negative savegame count is rejected");
        expect(target.triggers[0].status == 4, "negative count leaves the manager unchanged");
    }
}

void test_trigger_savegame_truncated() {
    using namespace wse::game;
    using namespace wse;

    TriggerManager source;
    source.triggers.resize(2);
    source.triggers[0].status = 1;
    source.triggers[0].checkTimer.value = 99;
    source.triggers[1].status = 2;
    source.triggers[1].checkTimer.value = 100;

    io::BinaryWriter writer;
    write_trigger_savegame(source, writer);
    const std::span<const std::byte> partial(writer.bytes().data(), writer.size() - 1);

    TriggerManager target;
    target.triggers.resize(2);
    target.triggers[1].status = 5;
    io::BinaryReader reader(partial);
    expect(read_trigger_savegame(target, reader) == SavegameResult::truncated,
           "truncated savegame is rejected");
    expect(target.triggers[0].status == 1 && target.triggers[0].checkTimer.value == 99,
           "complete records before the truncation are applied");
    expect(target.triggers[1].status == 5, "an incomplete record is not applied");
    expect(reader.remaining() == 7, "truncation leaves the incomplete record unread");
}

void test_simple_trigger_savegame() {
    using namespace wse::game;
    using namespace wse;

    io::BinaryWriter writer;
    writer.write_i32(3);
    writer.write_i64(11);
    writer.write_i64(22);
    writer.write_i64(33);

    SimpleTriggerManager target;
    target.triggers.resize(2);
    target.triggers[1].timer.timerNo = 6;
    io::BinaryReader reader(writer.bytes());
    expect(read_simple_trigger_savegame(target, reader) == SavegameResult::count_exceeded,
           "surplus simple triggers report the count error");
    expect(target.triggers[0].timer.value == 11 && target.triggers[1].timer.value == 22,
           "simple trigger timers are applied in order");
    expect(target.triggers[1].timer.timerNo == 6,
           "simple trigger timer numbers are not part of the savegame");
    expect(reader.at_end(), "surplus simple trigger entries are consumed");

    const std::byte shortBuffer[8] = {
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x2A}, std::byte{0}, std::byte{0}, std::byte{0}};
    SimpleTriggerManager shortTarget;
    shortTarget.triggers.resize(1);
    io::BinaryReader shortReader(shortBuffer);
    expect(read_simple_trigger_savegame(shortTarget, shortReader) == SavegameResult::truncated,
           "truncated simple trigger savegame is rejected");
    expect(shortTarget.triggers[0].timer.value == 0,
           "an incomplete simple trigger record is not applied");
}

void test_savegame_descriptions() {
    using namespace wse::game;
    expect(describe(SavegameResult::ok) == "ok", "ok description");
    expect(!describe(SavegameResult::count_exceeded).empty(), "count error description");
    expect(!describe(SavegameResult::negative_count).empty(), "negative count description");
    expect(!describe(SavegameResult::truncated).empty(), "truncation description");
    expect(describe(SavegameContainerResult::ok) == "ok", "container ok description");
    expect(!describe(SavegameContainerResult::checksum_mismatch).empty(),
           "checksum mismatch description");
    expect(!describe(SavegameContainerResult::invalid_magic).empty(),
           "invalid magic description");
    expect(!describe(SavegameContainerResult::unsupported_version).empty(),
           "unsupported version description");
    expect(!describe(SavegameContainerResult::incompatible_module_version).empty(),
           "incompatible module version description");
}

void test_rgl_string() {
    using wse::core::RglString;

    RglString s;
    expect(s.empty() && s.size() == 0 && s.using_inline_storage() && s.capacity() == 48U,
           "default RglString uses 48-byte inline SSO buffer");

    // 47 payload chars + NUL fits in 48-byte SSO buffer without heap spill.
    const std::string fits47(47U, 'a');
    s.assign(fits47);
    expect(s.size() == 47U && s.using_inline_storage() && s.view() == fits47,
           "47-byte string remains inside 48-byte SSO capacity");

    // 48 payload chars needs 49 bytes -> spills to 8-byte aligned heap storage.
    const std::string spill48(48U, 'b');
    s.assign(spill48);
    expect(s.size() == 48U && !s.using_inline_storage() && (s.capacity() % 8U) == 0U &&
               s.capacity() >= 49U && s.view() == spill48,
           "48-byte string grows to aligned heap buffer");

    s.assign("hello world\t  tabbed\r\nend");
    expect(s.next_token() == "hello", "next_token extracts first token");
    expect(s.next_token() == "world", "next_token skips space");
    expect(s.next_token() == "tabbed", "next_token skips tab and multiple spaces");
    expect(s.next_token() == "end", "next_token skips CRLF");
    expect(s.next_token().empty(), "next_token returns empty at end");
    s.set_cursor(-10);
    expect(s.cursor() == 0, "set_cursor clamps negative to 0");
    s.set_cursor(9999);
    expect(static_cast<std::size_t>(s.cursor()) == s.size(), "set_cursor clamps to size");
    s.reset_cursor();
    expect(s.next_token() == "hello", "reset_cursor restarts tokenization");

    s.assign("a b c");
    s.swap_spaces_with_underscores();
    expect(s.view() == "a_b_c", "swap_spaces_with_underscores converts spaces");
    s.swap_underscores_with_spaces();
    expect(s.view() == "a b c", "swap_underscores_with_spaces restores spaces");

    std::int64_t parsed_i = 0;
    std::uint64_t parsed_u = 0;
    s.assign("-9876543210");
    expect(s.parse_i64(parsed_i) && parsed_i == -9876543210LL, "parse_i64 signed");
    s.assign("1844674407370955161");
    expect(s.parse_u64(parsed_u) && parsed_u == 1844674407370955161ULL, "parse_u64 unsigned");
    s.assign("123x");
    expect(!s.parse_i64(parsed_i) && !s.parse_u64(parsed_u),
           "numeric parsers reject trailing garbage");

    expect(RglString::encode_savegame_string("") == "-1", "empty encodes to -1 sentinel");
    expect(RglString::decode_savegame_string("-1").empty(), "-1 sentinel decodes to empty");
    expect(RglString::encode_savegame_string("Harlaus held Praven") == "Harlaus_held_Praven",
           "savegame encoder turns spaces into underscores");
    expect(RglString::decode_savegame_string("Harlaus_held_Praven") == "Harlaus held Praven",
           "savegame decoder turns underscores into spaces");

    s.clear();
    expect(s.empty() && s.cursor() == 0 && s.c_str()[0] == '\0', "clear resets string");
}

void test_checksum_writer() {
    using namespace wse;
    io::BinaryChecksumWriter cw;
    cw.write_i32(-5);
    cw.write_u64(0x1122334455667788ULL);
    cw.write_bool(true);
    cw.write_string("Warband");
    cw.write_via([](io::BinaryWriter& w) { w.write_f32(2.5f); });

    const std::uint64_t expected = io::compute_savegame_checksum(cw.bytes());
    expect(cw.verify_checksum() && cw.checksum() == expected && cw.size() > 0,
           "BinaryChecksumWriter matches compute_savegame_checksum byte sum");

    cw.clear();
    expect(cw.size() == 0 && cw.checksum() == 0U && cw.verify_checksum(),
           "BinaryChecksumWriter clear resets bytes and checksum");
}

wse::game::WseWorldState make_test_savegame_world_fixture() {
    using namespace wse::game;
    WseWorldState w;
    w.has_game = true;
    w.module_version = 104;
    w.compatible_savegame_module_version = 100;
    w.savegame_name = "Ironman Day 12";
    w.campaign_time = 987654321LL;
    w.seed = 424242;
    w.encounter_flags = 0x5A01U;
    w.combat_damage_to_self = 2;
    w.combat_damage_to_friends = 1;
    w.reduce_battle_ai = 0;
    w.reduce_campaign_ai = 1;
    w.combat_speed = 3;
    w.date_timer_no = 1;
    w.date_timer_value = 500000;
    w.current_hours = 19;
    w.game_day = 12;
    w.game_week = 2;
    w.game_month = 3;
    w.game_year = 1257;
    w.cloud_amount = 0.25f;
    w.haze_amount = 0.125f;
    w.misc.average_game_difficulty = 0.75f;
    w.misc.difficulty_time_accum = 12.5f;
    w.tutorial_flags = 0x13U;
    w.in_meta_mission = false;
    w.encountered_party_no = 1;
    w.encountered_party_2_no = -1;
    w.party_creation_max_random = 7;
    w.jump_menu_no = 4;
    w.screen_troop_no = 0;
    w.current_site_no = 3;
    w.misc.rest_interactive = true;
    w.misc.rest_time_speed = 6;
    w.rest_period = 8.0f;
    w.rest_no_attack = 1;
    w.game_log = "Captured Suno from Swadia";
    w.class_names[0] = "Infantry";
    w.class_names[2] = "Cavalry";
    w.main_party_no = 0;
    w.player_troop_no = 0;
    w.player_kill_count = 33;
    w.player_teamkill_count = 1;
    w.player_wound_count = 9;
    w.player_teamwound_count = 0;

    // 2 troops: player hero + regular soldier
    w.troops.resize(2);
    w.troops[0].flags = 0x11ULL;  // hero bit (0x10) + skin 1
    w.troops[0].name = "Sir Calrad";
    w.troops[0].plural_name = "Sir Calrads";
    w.troops[0].renamed = true;
    w.troops[0].level = 18;
    w.troops[0].gold = 4500;
    w.troops[0].xp = 12000;
    w.troops[0].hit_points = 62.5f;
    w.troops[0].faction_no = 0;
    w.troops[0].class_no = 2;
    w.troops[0].home_site_no = 5;
    w.troops[0].home_entry_no = 2;
    w.troops[0].attributes = {15, 14, 12, 11};
    w.troops[0].proficiencies = {120.0f, 95.0f, 80.0f, 40.0f, 35.0f, 20.0f, 10.0f};
    w.troops[0].skills[0] = 0x12345678U;
    w.troops[0].skill_points = 3;
    w.troops[0].attribute_points = 2;
    w.troops[0].proficiency_points = 10;
    w.troops[0].face_keys[0] = {0xAAAABBBBULL, 0xCCCCDDDDULL, 0x11112222ULL, 0x33334444ULL};
    w.troops[0].inventory[0] = {10, 4, 0, 0};   // equipped sword + modifier 4
    w.troops[0].inventory[10] = {12, 2, 24, 0};  // bag arrows with ammo 24
    w.troops[0].slots.set(0, 777);
    w.troops[0].notes[0].text = u"Loyal champion";
    w.troops[0].notes[0].date = 10;
    w.troops[0].note_tableau_material_no = 7;

    w.troops[1].flags = 0x0ULL;  // regular
    w.troops[1].name = "Swadian Recruit";
    w.troops[1].plural_name = "Swadian Recruits";
    w.troops[1].renamed = false;
    w.troops[1].level = 4;
    w.troops[1].gold = 55;
    w.troops[1].inventory[1] = {3, 1, 0, 0};

    w.skills[0].max_level = 12;

    w.items.resize(1);
    w.items[0].slots.set(3, 99);

    w.quests.resize(1);
    w.quests[0].progression_flags = 3;
    w.quests[0].giver_troop_no = 0;
    w.quests[0].number = 25;
    w.quests[0].start_date = 1.5f;
    w.quests[0].title = "Deliver Letter";
    w.quests[0].text = "Deliver sealed orders";
    w.quests[0].giver_name = "King Harlaus";
    w.quests[0].slots.set(1, 55);
    w.quests[0].notes[0].text = u"Quest started";

    w.info_pages.resize(1);
    w.info_pages[0].notes[0].text = u"Calradia lore";
    w.info_pages[0].notes[0].date = 1;

    w.sites.resize(1);
    w.sites[0].slots.set(0, 123);

    w.factions.resize(2);
    w.factions[0].name = "Player Kingdom";
    w.factions[0].renamed = true;
    w.factions[0].color = 0xFF334455U;
    w.factions[0].relations = {1.0f, -0.5f};
    w.factions[0].slots.set(0, 11);
    w.factions[1].name = "Swadia";
    w.factions[1].relations = {-0.5f, 1.0f};

    w.tracks.push_back({{10.0f, 20.0f, 0.5f, 1.0f}, 1.25f, 3.5f, 0x09U});

    w.party_templates.resize(1);
    w.party_templates[0].num_created = 5;
    w.party_templates[0].num_destroyed = 2;
    w.party_templates[0].num_destroyed_by_player = 1;
    w.party_templates[0].slots.set(2, 88);

    // 2 hash-vector slots: [0] main_party active, [1] empty slot
    w.parties.resize(2);
    w.parties[0].active = true;
    w.parties[0].id = "main_party";
    w.parties[0].name = "Player Company";
    w.parties[0].renamed = true;
    w.parties[0].flags = 0x20U;
    w.parties[0].menu_no = 2;
    w.parties[0].position.o = {12.5f, -4.0f, 0.25f, 1.0f};
    w.parties[0].bearing = 1.75f;
    w.parties[0].behavior_position = {14.0f, -2.0f};
    w.parties[0].extra_text = "banner_0";
    w.parties[0].morale = 85.0f;
    w.parties[0].bandit_attraction = 65;
    w.parties[0].attached_party_nos = {1};
    w.parties[0].particle_system_nos = {3, 4};
    w.parties[0].slots.set(4, 654);
    w.parties[0].stacks.push_back({0, 1, 0, 0});   // hero stack
    w.parties[0].stacks.push_back({1, 15, 2, 0});  // regular stack with xp & DNA
    w.parties[0].stacks[1].xp = 45.5f;
    w.parties[0].stacks[1].num_upgradeable = 3;
    w.main_party_dnas[1U * 32U + 5U] = 999;

    w.parties[1].active = false;  // inactive hole in rglHashVector

    w.events.resize(2);
    w.events[0].active = 1;
    w.events[0].extra = "siege";
    w.events[0].position = {3.0f, 4.0f, 0.0f, 1.0f};
    w.events[0].party_1_no = 0;
    w.events[0].party_2_no = -1;
    w.events[0].timer_value = 500;
    w.events[0].duration = 12.0f;
    w.events[1].active = 0;

    return w;
}

void test_savegame_container_roundtrip() {
    using namespace wse;
    using namespace wse::game;

    const WseWorldState src = make_test_savegame_world_fixture();
    wse::vm::Slots src_globals;
    src_globals.set(7, 1337);

    TriggerManager src_triggers;
    src_triggers.triggers.resize(1);
    src_triggers.triggers[0].status = 1;
    src_triggers.triggers[0].checkTimer.value = 55;

    SimpleTriggerManager src_simple;
    src_simple.triggers.resize(1);
    src_simple.triggers[0].timer.value = 88;

    const std::array<std::int64_t, 3> timers_total{0, 1200000, 0};
    SavegameConstContext write_ctx{};
    write_ctx.world = &src;
    write_ctx.global_variables = &src_globals;
    write_ctx.triggers = &src_triggers;
    write_ctx.simple_triggers = &src_simple;
    write_ctx.timers_total = timers_total;

    io::BinaryChecksumWriter cw;
    const SavegameWriteResult wres = write_savegame_container(cw, write_ctx);
    expect(wres.ok && wres.status == SavegameContainerResult::ok &&
               wres.bytes_written == cw.size() && cw.size() > 64U,
           "write_savegame_container produces valid framed byte stream");

    // Prepare destination world pre-populated with module object counts (like real game load).
    WseWorldState dst;
    dst.has_game = true;
    dst.compatible_savegame_module_version = 100;
    dst.troops.resize(2);
    dst.troops[0].flags = 0x11ULL;
    dst.troops[1].flags = 0x0ULL;
    dst.troops[1].name = "Swadian Recruit";
    dst.troops[1].plural_name = "Swadian Recruits";
    dst.items.resize(1);
    dst.quests.resize(1);
    dst.info_pages.resize(1);
    dst.sites.resize(1);
    dst.factions.resize(2);
    dst.party_templates.resize(1);

    wse::vm::Slots dst_globals;
    TriggerManager dst_triggers;
    dst_triggers.triggers.resize(1);
    SimpleTriggerManager dst_simple;
    dst_simple.triggers.resize(1);

    SavegameRuntimeContext read_ctx{};
    read_ctx.world = &dst;
    read_ctx.global_variables = &dst_globals;
    read_ctx.triggers = &dst_triggers;
    read_ctx.simple_triggers = &dst_simple;

    const SavegameLoadReport report =
        read_savegame_container(cw.bytes(), read_ctx, &wres.checksum);
    expect(report.succeeded() && !report.count_exceeded &&
               report.bytes_consumed == cw.size() &&
               report.computed_checksum == wres.checksum,
           "read_savegame_container consumes all bytes and verifies checksum");

    expect(report.header.magic == kSavegameMagic &&
               report.header.savegame_version == kSavegameFormatVersion &&
               report.header.module_version == 104 &&
               report.header.name == "Ironman Day 12" &&
               report.header.player_name == "Sir Calrad" &&
               report.header.player_level == 18 &&
               std::fabs(report.header.game_date_hours - 7.0f) < 1e-4f,
           "savegame header round-trips with exact game_date_hours");

    expect(dst.campaign_time == 987654321LL && dst.seed == 424242 &&
               dst.encounter_flags == 0x5A01U && dst.combat_damage_to_self == 2 &&
               dst.combat_damage_to_friends == 1 && dst.reduce_campaign_ai == 1 &&
               dst.combat_speed == 3 && dst.game_year == 1257 &&
               dst.game_log == "Captured Suno from Swadia" &&
               dst.class_names[0] == "Infantry" && dst.class_names[2] == "Cavalry",
           "world scalar and game log fields round-trip");

    expect(dst_globals.get(7) == 1337 && dst_triggers.triggers[0].status == 1 &&
               dst_triggers.triggers[0].checkTimer.value == 55 &&
               dst_simple.triggers[0].timer.value == 88,
           "global variables, triggers, and simple triggers round-trip");

    expect(dst.quests[0].title == "Deliver Letter" &&
               dst.quests[0].giver_name == "King Harlaus" &&
               dst.quests[0].notes[0].text == u"Quest started" &&
               dst.quests[0].slots.get(1) == 55 &&
               dst.info_pages[0].notes[0].text == u"Calradia lore" &&
               dst.sites[0].slots.get(0) == 123,
           "quests, info pages, and sites round-trip");

    expect(dst.factions[0].renamed && dst.factions[0].name == "Player Kingdom" &&
               dst.factions[0].color == 0xFF334455U &&
               std::fabs(dst.factions[0].relations[1] - (-0.5f)) < 1e-5f &&
               dst.factions[0].slots.get(0) == 11,
           "factions, relations matrix, and renamed flag round-trip");

    expect(dst.tracks.size() == 1U && dst.tracks[0].flags == 0x09U &&
               dst.party_templates[0].num_created == 5 &&
               dst.party_templates[0].num_destroyed_by_player == 1 &&
               dst.party_templates[0].slots.get(2) == 88,
           "tracks and party templates round-trip");

    expect(dst.parties.size() == 2U && dst.parties[0].active && !dst.parties[1].active &&
               dst.parties[0].id == "main_party" && dst.parties[0].renamed &&
               dst.parties[0].menu_no == 2 && dst.parties[0].bandit_attraction == 65 &&
               std::fabs(dst.parties[0].bearing - 1.75f) < 1e-5f &&
               dst.parties[0].attached_party_nos == std::vector<std::int32_t>{1} &&
               dst.parties[0].particle_system_nos == std::vector<std::int32_t>{3, 4} &&
               dst.parties[0].stacks.size() == 2U &&
               std::fabs(dst.parties[0].stacks[1].xp - 45.5f) < 1e-5f &&
               dst.parties[0].stacks[1].num_upgradeable == 3 &&
               dst.main_party_dnas[1U * 32U + 5U] == 999,
           "hash vector parties, sparse slots, main party stack XP, and DNAs round-trip");

    expect(dst.events.size() == 2U && dst.events[0].active == 1 &&
               dst.events[0].extra == "siege" && dst.events[1].active == 0 &&
               dst.troops[0].renamed && dst.troops[0].name == "Sir Calrad" &&
               dst.troops[0].gold == 4500 && dst.troops[0].home_site_no == 5 &&
               dst.troops[0].home_entry_no == 2 &&
               dst.troops[0].inventory[0].item_no == 10 &&
               dst.troops[0].inventory[0].modifier == 4 &&
               dst.troops[0].inventory[10].item_no == 12 &&
               dst.troops[0].inventory[10].ammo == 24 &&
               dst.troops[0].face_keys[0][3] == 0x33334444ULL &&
               !dst.troops[1].renamed && dst.troops[1].name == "Swadian Recruit" &&
               dst.skills[0].max_level == 12 && dst.items[0].slots.get(3) == 99 &&
               dst.player_kill_count == 33 && dst.player_teamkill_count == 1 &&
               dst.player_wound_count == 9,
           "events, hero+regular troops, packed inventory, skills, items, and tail round-trip");
}

void test_savegame_container_validation_and_vfs() {
    using namespace wse;
    using namespace wse::game;

    const WseWorldState src = make_test_savegame_world_fixture();
    const std::array<std::int64_t, 3> timers_total{0, 600000, 0};
    SavegameConstContext wctx{};
    wctx.world = &src;
    wctx.timers_total = timers_total;

    // 1. VFS staging + backup rename + inspect + load.
    wse::platform::MemoryVfs vfs;
    const SavegameWriteResult first =
        save_world_to_vfs(vfs, "Savegames/Native", "sg00.sav", wctx, "Manual Slot 1");
    expect(first.ok && vfs.is_file("Savegames/Native/sg00.sav") &&
               !vfs.is_file("Savegames/Native/new_game.sav") &&
               !vfs.is_file("Savegames/Native/last_savegame_backup.sav"),
           "first save_world_to_vfs creates target and clears temp file");

    const SavegameWriteResult second =
        save_world_to_vfs(vfs, "Savegames/Native", "sg00.sav", wctx, "Manual Slot 2");
    expect(second.ok && vfs.is_file("Savegames/Native/sg00.sav") &&
               vfs.is_file("Savegames/Native/last_savegame_backup.sav") &&
               !vfs.is_file("Savegames/Native/new_game.sav"),
           "second save_world_to_vfs rotates previous file into last_savegame_backup.sav");

    SavegameHeader inspected{};
    expect(inspect_savegame_header_vfs(vfs, "Savegames/Native/sg00.sav", inspected, 100) ==
               SavegameContainerResult::ok &&
               inspected.name == "Manual Slot 2",
           "inspect_savegame_header_vfs reads header override cleanly");
    expect(inspect_savegame_header_vfs(vfs, "Savegames/Native/sg00.sav", inspected, 200) ==
               SavegameContainerResult::incompatible_module_version,
           "inspect_savegame_header_vfs rejects old module_version");

    // 2. Module with empty/smaller section arrays tolerates surplus records without UB (e.g.
    // info_pages empty), while retaining hero troop[0] flags so loadParties distinguishes hero stacks.
    WseWorldState empty_dst;
    empty_dst.has_game = true;
    empty_dst.compatible_savegame_module_version = 100;
    empty_dst.troops.resize(1);
    empty_dst.troops[0].flags = 0x10ULL;
    SavegameRuntimeContext empty_ctx{};
    empty_ctx.world = &empty_dst;
    const SavegameLoadReport surplus_rep =
        load_world_from_vfs(vfs, "Savegames/Native/sg00.sav", empty_ctx, &second.checksum);
    expect(surplus_rep.succeeded() && surplus_rep.count_exceeded,
           "load_world_from_vfs consumes surplus records safely when world vectors are smaller/empty");

    // 3. Checksum mismatch and header / payload corruption checks.
    const std::uint64_t bad_checksum = second.checksum + 1ULL;
    const SavegameLoadReport bad_chk_rep =
        load_world_from_vfs(vfs, "Savegames/Native/sg00.sav", empty_ctx, &bad_checksum);
    expect(bad_chk_rep.status == SavegameContainerResult::checksum_mismatch,
           "read_savegame_container rejects mismatched checksum");

    io::BinaryChecksumWriter raw_cw;
    (void)write_savegame_container(raw_cw, wctx);
    std::vector<std::byte> mutated(raw_cw.bytes().begin(), raw_cw.bytes().end());
    mutated[0] = std::byte{'X'};
    expect(read_savegame_container(mutated, empty_ctx).status ==
               SavegameContainerResult::invalid_magic,
           "bad WRDR magic rejected");

    std::vector<std::byte> truncated(raw_cw.bytes().begin(), raw_cw.bytes().end() - 1);
    expect(read_savegame_container(truncated, empty_ctx).status ==
               SavegameContainerResult::truncated,
           "truncated savegame container rejected");
    std::vector<std::byte> half_truncated(raw_cw.bytes().begin(),
                                          raw_cw.bytes().begin() + raw_cw.size() / 2U);
    expect(read_savegame_container(half_truncated, empty_ctx).status ==
               SavegameContainerResult::truncated,
           "mid-stream truncated savegame container rejected");

    // Version bounds [802..1174].
    SavegameHeader vh = fill_savegame_header(src, timers_total);
    vh.savegame_version = 801;
    expect(validate_savegame_header(vh, 100) == SavegameContainerResult::unsupported_version,
           "version below 802 rejected");
    vh.savegame_version = 1175;
    expect(validate_savegame_header(vh, 100) == SavegameContainerResult::unsupported_version,
           "version above 1174 rejected");
    vh.savegame_version = 802;
    expect(validate_savegame_header(vh, 100) == SavegameContainerResult::ok,
           "minimum supported version 802 accepted");
}

}  // namespace

namespace wse::selftest {

int run_savegame_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_binary_writer();
    test_binary_reader_bounds();
    test_trigger_savegame_roundtrip();
    test_trigger_savegame_extra_entries();
    test_trigger_savegame_short_and_negative();
    test_trigger_savegame_truncated();
    test_simple_trigger_savegame();
    test_savegame_descriptions();
    test_rgl_string();
    test_checksum_writer();
    test_savegame_container_roundtrip();
    test_savegame_container_validation_and_vfs();

    if (failures != 0) {
        std::printf("savegame tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("savegame tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_savegame_suite(nullptr);
}
#endif
