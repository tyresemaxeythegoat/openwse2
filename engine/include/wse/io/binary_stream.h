// Little-endian binary stream used by savegames.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   ev_24eecfa4fc91587dd7b5b29ca767a78c8747a538e953f37b295746c67f18c0b6
//     rglBinaryStream writer export (9 functions).
//   ev_130d8023206760c8a441e940113e7a231d384634f4a73d4813a3ae6b412c62e6
//     rglBinaryStream reader export; Ghidra emitted 0x1402aebe0 as
//     FUN_1402a1d00 because it did not split the adjacent 20-byte entries.
//   rglBinaryStream vftable 0x14055c780, 30 slots.
//   PDB public symbols at identical addresses prove the slot aliases:
//     +0x20 writeInt64 / writeUInt64   0x1402ae9d0
//     +0x30 writeInt32 / writeUInt32   0x1402ae980  (also writeFloat)
//     +0x38 writeBool / writeByte      0x1402aea30
//     +0x40 writeCString               0x1402aea80
//     +0x48 writeString                0x1402aeb10
//     +0x58 readFloat / readInt32      0x1402aebe0  (also +0x78/+0x80)
//     +0x60 readBool / readByte        0x1402aec00  (also +0x88)
//     +0x68 readUInt64 / readInt64     0x1402aec20  (also +0x70)
//     +0x90 readString                 0x1402aec40
//   writeFloat (0x1402ae980) appends the raw 4 bytes of its argument,
//   writeInt64 (0x1402ae9d0) the raw 8 bytes, and writeBool (0x1402aea30) the
//   raw byte, into the buffered rglString at stream+0x58. writeString
//   (0x1402aeb10) and writeCString (0x1402aea80) first write an int32 byte
//   count through slot +0x30, then the bytes with no terminator. readString
//   (0x1402aec40) reads the same int32 count through slot +0x80.
//   readInt32 (0x1402aebe0) loads 4 raw bytes from data+(+0x60) at the int32
//   cursor (+0x58) and advances the cursor by 4; readByte (0x1402aec00) and
//   readInt64 (0x1402aec20) are the same pattern with widths 1 and 8. The
//   retail readers have no bounds check. There is no header, version, length,
//   or checksum in this stream class.
//
// The retail binary is x86-64, so the raw copies are little-endian. The
// portable stream fixes that byte order explicitly, keeping Android arm64
// savegames byte-identical to the retail format. It also bounds-checks every
// read; retail would read past the buffer on a truncated savegame.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wse::io {

class BinaryWriter {
public:
    void write_i32(std::int32_t value) {
        append_unsigned(static_cast<std::uint32_t>(value), 4);
    }

    void write_u32(std::uint32_t value) { append_unsigned(value, 4); }

    void write_i64(std::int64_t value) {
        append_unsigned(static_cast<std::uint64_t>(value), 8);
    }

    void write_u64(std::uint64_t value) { append_unsigned(value, 8); }

    // Same slot as writeInt32: the four raw bytes of the float.
    void write_f32(float value) { write_u32(std::bit_cast<std::uint32_t>(value)); }

    // Same slot as writeByte: one raw byte, no canonicalization.
    void write_bool(bool value) { bytes_.push_back(static_cast<std::byte>(value ? 1 : 0)); }

    void write_byte(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    void clear() noexcept { bytes_.clear(); }

private:
    void append_unsigned(std::uint64_t value, unsigned width) {
        for (unsigned index = 0; index < width; ++index) {
            bytes_.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xFF));
        }
    }

    std::vector<std::byte> bytes_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool read_i32(std::int32_t& out) noexcept {
        std::uint32_t value = 0;
        if (!read_u32(value)) {
            return false;
        }
        out = static_cast<std::int32_t>(value);
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& out) noexcept { return read_unsigned(out, 4); }

    [[nodiscard]] bool read_i64(std::int64_t& out) noexcept {
        std::uint64_t value = 0;
        if (!read_u64(value)) {
            return false;
        }
        out = static_cast<std::int64_t>(value);
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& out) noexcept { return read_unsigned(out, 8); }

    // Same slot as readInt32: four raw bytes interpreted as an IEEE float.
    [[nodiscard]] bool read_f32(float& out) noexcept {
        std::uint32_t bits = 0;
        if (!read_u32(bits)) {
            return false;
        }
        out = std::bit_cast<float>(bits);
        return true;
    }

    // Same slot as readByte: any nonzero byte is true.
    [[nodiscard]] bool read_bool(bool& out) noexcept {
        std::uint8_t value = 0;
        if (!read_byte(value)) {
            return false;
        }
        out = value != 0;
        return true;
    }

    [[nodiscard]] bool read_byte(std::uint8_t& out) noexcept {
        if (cursor_ >= bytes_.size()) {
            return false;
        }
        out = std::to_integer<std::uint8_t>(bytes_[cursor_]);
        ++cursor_;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return cursor_ == bytes_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - cursor_; }

private:
    template <class Integer>
    [[nodiscard]] bool read_unsigned(Integer& out, unsigned width) noexcept {
        if (remaining() < width) {
            return false;
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < width; ++index) {
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(bytes_[cursor_ + index]))
                     << (8 * index);
        }
        cursor_ += width;
        out = static_cast<Integer>(value);
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0;
};

}  // namespace wse::io
