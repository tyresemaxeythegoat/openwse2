// Host tests for the portable binary stream and trigger savegames.
//
// Build/run: powershell -File engine/build_host.ps1

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "wse/game/trigger.h"
#include "wse/io/binary_stream.h"

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
