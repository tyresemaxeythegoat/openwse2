// Host tests for the reconstructed script-VM operand and statement formats.
//
// Build/run: powershell -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "wse/vm/operand.h"
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

struct TestSlots {
    std::vector<std::int64_t> values;

    [[nodiscard]] std::int64_t get(std::int32_t index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
            return 0;
        }
        return values[static_cast<std::size_t>(index)];
    }

    bool set(std::int32_t index, std::int64_t value) {
        if (index < 0) {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index);
        if (slot >= values.size()) {
            values.resize(slot + 1, 0);
        }
        values[slot] = value;
        return true;
    }
};

class MemorySource final : public wse::vm::ByteSource {
public:
    MemorySource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool read(void* destination, std::size_t bytes) override {
        if (bytes > size_ - offset_) {
            return false;
        }
        std::memcpy(destination, data_ + offset_, bytes);
        offset_ += bytes;
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    std::uint8_t raw[sizeof(value)];
    std::memcpy(raw, &value, sizeof(value));
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

void append_i32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    std::uint8_t raw[sizeof(value)];
    std::memcpy(raw, &value, sizeof(value));
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

void append_i64(std::vector<std::uint8_t>& bytes, std::int64_t value) {
    std::uint8_t raw[sizeof(value)];
    std::memcpy(raw, &value, sizeof(value));
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
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
    TestSlots slots;
    slots.values = {100, 200};

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
    TestSlots slots;

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

void test_statement() {
    using namespace wse::vm;

    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 0xCE5);
    append_i32(bytes, 2);
    append_i64(bytes, 7);
    append_i64(bytes, -3);

    MemorySource source(bytes.data(), bytes.size());
    Statement statement{};
    const ReadError error = read_statement(source, statement);
    expect(error == ReadError::none, "statement reads");
    expect(statement.opcode == 0xCE5, "opcode is preserved");
    expect(statement.numOperands == 2, "operand count is preserved");
    expect(statement.operands[0] == 7 && statement.operands[1] == -3, "operands are preserved");
    expect(statement.endStatement == -1, "endStatement starts at -1");

    std::vector<std::uint8_t> too_many;
    append_u32(too_many, 1);
    append_i32(too_many, 33);
    MemorySource wide(too_many.data(), too_many.size());
    Statement rejected{};
    expect(read_statement(wide, rejected) == ReadError::operand_count_out_of_range,
           "operand count above 32 is rejected");

    std::vector<std::uint8_t> negative;
    append_u32(negative, 1);
    append_i32(negative, -1);
    MemorySource neg(negative.data(), negative.size());
    expect(read_statement(neg, rejected) == ReadError::negative_operand_count,
           "negative operand count is rejected");

    std::vector<std::uint8_t> truncated;
    append_u32(truncated, 1);
    append_i32(truncated, 2);
    append_i64(truncated, 7);
    MemorySource short_source(truncated.data(), truncated.size());
    expect(read_statement(short_source, rejected) == ReadError::io,
           "truncated operands are rejected");

    std::vector<std::uint8_t> empty;
    append_u32(empty, 0);
    append_i32(empty, 0);
    MemorySource empty_source(empty.data(), empty.size());
    expect(read_statement(empty_source, rejected) == ReadError::none && rejected.endStatement == -1,
           "zero-operand statement reads");
}

}  // namespace

int main() {
    test_tags();
    test_decode();
    test_store();
    test_statement();

    if (failures != 0) {
        std::printf("operand tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("operand tests: PASS\n");
    return 0;
}
