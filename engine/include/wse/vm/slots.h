// Global script slots.
//
// Evidence:
//   mbSlots layout (PDB type 0x1b004, 24 bytes)
//     +0x0000 m_values rglVector<__int64>   (std::vector<int64_t>, 24 bytes)
//   mbStatement::parseOperand  (0x1401c61f0) reads tag 2 with a bounds check
//     against the vector size and yields 0 outside it.
//   mbStatement::returnValue   (0x1401c6280) writes tag 2 through mbSlots::set.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace wse::vm {

class Slots {
public:
    // Missing and negative indices read as 0, matching the bounds check in
    // mbStatement::parseOperand.
    [[nodiscard]] std::int64_t get(std::int32_t index) const noexcept {
        if (index < 0 || static_cast<std::size_t>(index) >= values_.size()) {
            return 0;
        }
        return values_[static_cast<std::size_t>(index)];
    }

    // Grows the vector to hold `index`. Negative indices are rejected; the
    // retail mbSlots::set is only reached with non-negative parsed payloads.
    bool set(std::int32_t index, std::int64_t value) {
        if (index < 0) {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index);
        if (slot >= values_.size()) {
            values_.resize(slot + 1, 0);
        }
        values_[slot] = value;
        return true;
    }

    [[nodiscard]] std::span<const std::int64_t> values() const noexcept { return values_; }
    void assign(std::span<const std::int64_t> src) { values_.assign(src.begin(), src.end()); }
    void assign(std::vector<std::int64_t>&& src) noexcept { values_ = std::move(src); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    void clear() noexcept { values_.clear(); }

private:
    std::vector<std::int64_t> values_;
};

}  // namespace wse::vm
