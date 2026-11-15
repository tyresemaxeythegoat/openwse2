// Host tests for the reconstructed script-VM operand and statement formats.
//
// Build/run: powershell -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "wse/io/text_stream.h"
#include "wse/vm/operand.h"
#include "wse/vm/slots.h"
#include "wse/vm/statement.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) {
    return (static_cast<std::uint64_t>(tag) << 56) | (payload & wse::vm::kPayloadMask);
}

void test_tags() {
    using namespace wse::vm;

    expect(tag_of(0) == 0, "zero is an immediate");
    expect(tag_of(~std::uint64_t{0}) == 0, "negative values are immediates");
    expect(tag_of(42) == 0, "positive untagged value is an immediate");

    const auto local = tagged(0x11, 7);
    expect(tag_of(local) == 0x11, "tag 0x11 is decoded");
    expect(payload_of(local) == 7, "tag 0x11 payload is masked");

    const auto negative = static_cast<std::uint64_t>(-123456789);
    expect(payload_of(negative) == negative, "immediate payload keeps all 64 bits");

    const auto unknown = tagged(0x05, 1);
    expect(tag_of(unknown) == 0x05, "unknown tags survive tagging");
}

void test_decode() {
    using namespace wse::vm;

    std::int64_t globals[4] = {10, 20, 30, 40};
    std::int64_t locals[8] = {0, 0, 9, 0};
    Slots slots;
    expect(slots.set(0, 100) && slots.set(1, 200), "slots initialize");

    bool ok = false;
    expect(decode(123, globals, slots, locals, ok) == 123 && ok, "immediate decodes");
    expect(decode(static_cast<std::uint64_t>(-5), globals, slots, locals, ok) == -5 && ok,
           "negative immediate decodes");

    expect(decode(tagged(0x01, 2), globals, slots, locals, ok) == 30 && ok, "global decodes");
    expect(decode(tagged(0x01, 4), globals, slots, locals, ok) == 0 && !ok,
           "out-of-range global is rejected");

    expect(decode(tagged(0x02, 1), globals, slots, locals, ok) == 200 && ok, "slot decodes");
    expect(decode(tagged(0x02, 9), globals, slots, locals, ok) == 0 && ok,
           "missing slot reads as zero");

    expect(decode(tagged(0x11, 2), globals, slots, locals, ok) == 9 && ok, "local decodes");
    expect(decode(tagged(0x11, 8), globals, slots, locals, ok) == 0 && !ok,
           "out-of-range local is rejected");

    expect(decode(tagged(0x05, 1), globals, slots, locals, ok) == 0 && !ok,
           "unknown tag is rejected");
}

void test_store() {
    using namespace wse::vm;

    std::int64_t globals[3] = {0, 0, 0};
    std::int64_t locals[3] = {0, 0, 0};
    Slots slots;

    expect(store(tagged(0x01, 1), 55, globals, slots, locals), "global store succeeds");
    expect(globals[1] == 55, "global store writes the value");

    expect(store(tagged(0x02, 3), 66, globals, slots, locals), "slot store succeeds");
    expect(slots.get(3) == 66, "slot store uses mbSlots::set semantics");

    expect(store(tagged(0x11, 2), 77, globals, slots, locals), "local store succeeds");
    expect(locals[2] == 77, "local store writes the value");

    expect(!store(7, 1, globals, slots, locals), "immediate is not an lvalue");
    expect(!store(tagged(0x01, 9), 1, globals, slots, locals), "global store rejects OOB");
    expect(!store(tagged(0x11, 9), 1, globals, slots, locals), "local store rejects OOB");
    expect(!store(tagged(0x05, 1), 1, globals, slots, locals), "unknown tag is not an lvalue");
}

void test_slots() {
    using namespace wse::vm;

    Slots slots;
    expect(slots.get(0) == 0, "empty slots read as zero");
    expect(slots.set(3, 42), "slot write grows the vector");
    expect(slots.size() == 4, "slot write sizes to index + 1");
    expect(slots.get(3) == 42, "slot write stores the value");
    expect(slots.get(2) == 0, "grown slots are zero-filled");
    expect(!slots.set(-1, 1), "negative slot index is rejected");
    slots.clear();
    expect(slots.empty(), "slots clear");
}

void test_text_stream() {
    using namespace wse::io;

    {
        TokenReader reader("  \r\n\t42 -7 1.5 name");
        std::int32_t signedValue = 0;
        std::uint32_t unsignedValue = 0;
        float real = 0.0F;
        std::string name;
        expect(reader.read_i32(signedValue) && signedValue == 42, "i32 token parses");
        expect(reader.read_i32(signedValue) && signedValue == -7, "negative i32 token parses");
        expect(reader.read_f32(real) && real == 1.5F, "float token parses");
        expect(reader.read_string(name) && name == "name", "string token parses");
        expect(reader.at_end(), "reader reaches end");
        expect(!reader.read_u32(unsignedValue), "read past end fails");
    }

    {
        TokenReader reader("triggersfile version 1 2");
        expect(reader.skip_tokens(3), "three header tokens are skipped");
        std::int32_t count = 0;
        expect(reader.read_i32(count) && count == 2, "body follows the header");
    }

    {
        TokenReader reader("12x 7");
        std::int32_t value = 0;
        expect(!reader.read_i32(value), "trailing garbage is rejected");
        expect(reader.read_i32(value) && value == 7, "cursor advances past the bad token");
    }

    {
        TokenReader reader("true FALSE 1 0 2");
        bool value = false;
        expect(reader.read_bool(value) && value, "true parses");
        expect(reader.read_bool(value) && !value, "FALSE parses case-insensitively");
        expect(reader.read_bool(value) && value, "1 parses as true");
        expect(reader.read_bool(value) && !value, "0 parses as false");
        expect(reader.read_bool(value) && value, "nonzero integer parses as true");
    }

    {
        TokenReader reader("yes");
        bool value = true;
        expect(!reader.read_bool(value), "non-boolean token is rejected");
    }

    {
        TokenReader reader("   ");
        expect(reader.at_end(), "whitespace-only input is empty");
        expect(!reader.skip_tokens(1), "skipping a token past EOF fails");
    }
}

void test_statement() {
    using namespace wse::vm;
    using namespace wse::io;

    {
        TokenReader reader("2767 2 7 -3");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::none, "statement reads");
        expect(statement.opcode == 2767, "opcode is preserved");
        expect(statement.numOperands == 2, "operand count is preserved");
        expect(statement.operands[0] == 7 && statement.operands[1] == -3,
               "operands are preserved");
        expect(statement.endStatement == -1, "endStatement starts at -1");
    }

    {
        TokenReader reader("2147483648 0");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::none &&
                   statement.opcode == 0x80000000U,
               "flagged opcode keeps the high bit");
    }

    {
        TokenReader reader("1 33");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::operand_count_out_of_range,
               "operand count above 32 is rejected");
    }

    {
        TokenReader reader("1 -1");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::negative_count,
               "negative operand count is rejected");
    }

    {
        TokenReader reader("1 2 7");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::malformed_number,
               "truncated operands are rejected");
    }

    {
        TokenReader reader("opcode 1");
        Statement statement{};
        expect(read_statement(reader, statement) == ReadError::malformed_number,
               "non-numeric opcode is rejected");
    }

    {
        TokenReader reader("2 2767 2 7 -3 3 0");
        StatementBlock block;
        expect(read_statement_block(reader, block) == ReadError::none, "block reads");
        expect(block.statements.size() == 2, "statement count is preserved");
        expect(block.statements[0].opcode == 2767, "first statement opcode");
        expect(block.statements[1].opcode == 3 && block.statements[1].numOperands == 0,
               "zero-operand statement reads");
        expect(!block.analyzed, "readStream leaves analyzed false");
        expect(block.numLocalVariablesUsed == 0x400, "readStream seeds local count to 0x400");
        expect(block.luaHookRef == kNoLuaHook, "block keeps the no-hook sentinel");
        expect(block.statements[1].endStatement == -1, "every endStatement starts at -1");
    }

    {
        TokenReader reader("-1");
        StatementBlock block;
        expect(read_statement_block(reader, block) == ReadError::negative_count,
               "negative statement count is rejected");
    }

    {
        TokenReader reader("1048577");
        StatementBlock block;
        expect(read_statement_block(reader, block) == ReadError::statement_count_out_of_range,
               "statement count above the cap is rejected");
    }

    {
        // A count inside the cap but larger than the remaining input could
        // encode fails on the first missing record without reserving storage
        // for the claimed million statements.
        TokenReader reader("1000000 30 0");
        StatementBlock block;
        expect(read_statement_block(reader, block) == ReadError::malformed_number &&
                   block.statements.size() == 2 && block.statements.capacity() < 1000,
               "statement count beyond the unread input does not pre-allocate");
    }

    {
        TokenReader reader("1 1 1 5");
        StatementBlock block;
        expect(read_statement_block(reader, block) == ReadError::none &&
                   block.statements.size() == 1,
               "one-statement block reads");
    }
}

}  // namespace

namespace wse::selftest {

int run_operand_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_tags();
    test_decode();
    test_store();
    test_slots();
    test_text_stream();
    test_statement();

    if (failures != 0) {
        std::printf("operand tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("operand tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_operand_suite(nullptr);
}
#endif
