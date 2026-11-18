// Tagged script-VM operands.
//
// Evidence:
//   mbStatement::parseOperand  (0x1401c61f0)  Ghidra 12.1.4 pseudocode
//   mbStatement::returnValue   (0x1401c6280)
//   mbStatementBlock::execute  (0x1401f6b50)
//   mbStatementBlock::analyze  (0x1401f68c0)
//
// Encoding (proven by the pseudocode above):
//   bits  0..55  payload
//   bits 56..62  tag, (value >> 56) & 0x7f
//   bit  63      unused; any signed value <= 0 is an untagged immediate
//
// Safety deviation: the original reads the global array (tag 1) and the local
// array (tag 0x11) without bounds checks, and reads the slot vector (tag 2)
// with a check that yields 0. This port bounds-checks all three and reports a
// failure instead of performing an out-of-bounds access.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace wse::vm {

inline constexpr std::size_t kMaxOperands = 32;
inline constexpr std::uint64_t kPayloadMask = 0x00FFFFFFFFFFFFFFull;
inline constexpr std::uint8_t kTagMask = 0x7f;

enum class OperandTag : std::uint8_t {
    immediate = 0x00,
    global = 0x01,
    slot = 0x02,
    local = 0x11,
};

// Tag of a raw operand word. Values <= 0 are immediates, so the tag path only
// sees positive values.
[[nodiscard]] constexpr std::uint8_t tag_of(std::uint64_t raw) noexcept {
    if (static_cast<std::int64_t>(raw) <= 0) {
        return static_cast<std::uint8_t>(OperandTag::immediate);
    }
    return static_cast<std::uint8_t>((raw >> 56) & kTagMask);
}

// Payload bits of a tagged operand; immediates keep the full 64-bit value.
[[nodiscard]] constexpr std::uint64_t payload_of(std::uint64_t raw) noexcept {
    return tag_of(raw) == static_cast<std::uint8_t>(OperandTag::immediate)
               ? raw
               : (raw & kPayloadMask);
}

// Resolves one operand. `ok` is false when a tag or index is invalid; the
// return value is then 0. Tag 2 (mbSlots) never fails: a missing slot reads as
// 0, matching mbSlots::get.
template <class Slots>
[[nodiscard]] std::int64_t decode(std::uint64_t raw,
                                  std::span<const std::int64_t> globals,
                                  const Slots& slots,
                                  std::span<const std::int64_t> locals,
                                  bool& ok) noexcept {
    const auto tag = tag_of(raw);
    const auto payload = payload_of(raw);
    const auto index = static_cast<std::int32_t>(payload);

    ok = true;
    switch (static_cast<OperandTag>(tag)) {
    case OperandTag::immediate:
        return static_cast<std::int64_t>(raw);
    case OperandTag::global:
        if (index < 0 || static_cast<std::size_t>(index) >= globals.size()) {
            ok = false;
            return 0;
        }
        return globals[static_cast<std::size_t>(index)];
    case OperandTag::slot:
        return slots.get(index);
    case OperandTag::local:
        if (index < 0 || static_cast<std::size_t>(index) >= locals.size()) {
            ok = false;
            return 0;
        }
        return locals[static_cast<std::size_t>(index)];
    }
    ok = false;
    return 0;
}

// Writes `value` through operand 0, which the VM treats as the destination
// lvalue (mbStatement::returnValue). Returns false for an illegal lvalue or an
// out-of-range global/local index.
template <class Slots>
[[nodiscard]] bool store(std::uint64_t raw,
                         std::int64_t value,
                         std::span<std::int64_t> globals,
                         Slots& slots,
                         std::span<std::int64_t> locals) noexcept {
    const auto tag = tag_of(raw);
    const auto index = static_cast<std::int32_t>(payload_of(raw));

    switch (static_cast<OperandTag>(tag)) {
    case OperandTag::global:
        if (index < 0 || static_cast<std::size_t>(index) >= globals.size()) {
            return false;
        }
        globals[static_cast<std::size_t>(index)] = value;
        return true;
    case OperandTag::slot:
        return slots.set(index, value);
    case OperandTag::local:
        if (index < 0 || static_cast<std::size_t>(index) >= locals.size()) {
            return false;
        }
        locals[static_cast<std::size_t>(index)] = value;
        return true;
    case OperandTag::immediate:
        break;
    }
    return false;
}

}  // namespace wse::vm
