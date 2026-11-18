// Little-endian binary stream used by savegames.
//
// Evidence (v1.1.5.5 REA / PDB):
//   ev_8cc48986006b9c7e64b7e7b94e4b37bb06c9950ecba367a50b0b601381dab2e7
//     rglBinaryStream writer/stream export (9 functions).
//   readInt32 (0x1402aee50), readByte (0x1402aee70), and readInt64 (0x1402aee90)
//   sit at 0x20-byte strides; slot aliases and readers verified in PDB + disasm.
//   rglBinaryStream vftable 0x14055d7b0, 30 slots.
//   PDB public symbols at identical addresses prove the slot aliases:
//     +0x20 writeInt64 / writeUInt64   0x1402aec40
//     +0x30 writeInt32 / writeUInt32   0x1402aebf0  (also writeFloat)
//     +0x38 writeBool / writeByte      0x1402aeca0
//     +0x40 writeCString               0x1402aecf0
//     +0x48 writeString                0x1402aed80
//     +0x58 readFloat / readInt32      0x1402aee50  (also +0x78/+0x80)
//     +0x60 readBool / readByte        0x1402aee70  (also +0x88)
//     +0x68 readUInt64 / readInt64     0x1402aee90  (also +0x70)
//     +0x90 readString                 0x1402aeeb0
//   rglOutputBinaryFileStreamWithChecksum vftable 0x140566990:
//     writeInt32/writeUInt32/writeFloat (0x1402c8c70), writeByte/writeBool
//     (0x1402c8cd0), writeInt64/writeUInt64 (0x1402c8d00), writeCString
//     (0x1402c8db0), writeString (0x1402c8e70) accumulate every written
//     unsigned byte modulo 2^64 into a 64-bit checksum verified after closing
//     by mbGame::save (0x1400e1bef..0x1400e1c50).
//   writeFloat (0x1402aebf0) appends the raw 4 bytes of its argument,
//   writeInt64 (0x1402aec40) the raw 8 bytes, and writeBool (0x1402aeca0) the
//   raw byte, into the buffered rglString at stream+0x58. writeString
//   (0x1402aed80) and writeCString (0x1402aecf0) first write an int32 byte
//   count through slot +0x30, then the bytes with no terminator. readString
//   (0x1402aeeb0) reads the same int32 count through slot +0x80.
//   readInt32 (0x1402aee50) loads 4 raw bytes from data+(+0x60) at the int32
//   cursor (+0x58) and advances the cursor by 4; readByte (0x1402aee70) and
//   readInt64 (0x1402aee90) are the same pattern with widths 1 and 8. The
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
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
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

    void write_bytes(std::span<const std::byte> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    // Mirrors rglBinaryStream::writeString (0x1402aed80): int32 byte count + raw bytes.
    void write_string(std::string_view text) {
        const std::size_t max_len =
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
        const std::size_t len = text.size() > max_len ? max_len : text.size();
        write_i32(static_cast<std::int32_t>(len));
        for (std::size_t i = 0; i < len; ++i) {
            bytes_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(text[i])));
        }
    }

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

    [[nodiscard]] bool read_bytes(std::span<std::byte> out) noexcept {
        if (remaining() < out.size()) {
            return false;
        }
        if (!out.empty()) {
            std::memcpy(out.data(), bytes_.data() + cursor_, out.size());
            cursor_ += out.size();
        }
        return true;
    }

    [[nodiscard]] bool skip(std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        cursor_ += count;
        return true;
    }

    // Mirrors rglBinaryStream::readString (0x1402aeeb0) with bounds checking.
    [[nodiscard]] bool read_string(std::string& out,
                                   std::size_t max_len = (1U << 20U)) {
        std::int32_t len = 0;
        if (!read_i32(len) || len < 0) {
            return false;
        }
        const auto byte_count = static_cast<std::size_t>(len);
        if (byte_count > max_len || byte_count > remaining()) {
            return false;
        }
        out.resize(byte_count);
        if (byte_count > 0U) {
            std::memcpy(out.data(), bytes_.data() + cursor_, byte_count);
            cursor_ += byte_count;
        }
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return cursor_ == bytes_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
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

// Mirrors mbGame::save (0x1400e1bef..0x1400e1c4e) byte-sum savegame checksum.
[[nodiscard]] inline std::uint64_t compute_savegame_checksum(
    std::span<const std::byte> data) noexcept {
    std::uint64_t sum = 0U;
    for (const std::byte b : data) {
        sum += static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
    }
    return sum;
}

// Mirrors rglOutputBinaryFileStreamWithChecksum (0x140566990): forwards writes
// to BinaryWriter while accumulating the running uint64 byte sum.
class BinaryChecksumWriter {
public:
    void write_i32(std::int32_t value) {
        record_delta([&] { writer_.write_i32(value); });
    }
    void write_u32(std::uint32_t value) {
        record_delta([&] { writer_.write_u32(value); });
    }
    void write_i64(std::int64_t value) {
        record_delta([&] { writer_.write_i64(value); });
    }
    void write_u64(std::uint64_t value) {
        record_delta([&] { writer_.write_u64(value); });
    }
    void write_f32(float value) {
        record_delta([&] { writer_.write_f32(value); });
    }
    void write_bool(bool value) {
        record_delta([&] { writer_.write_bool(value); });
    }
    void write_byte(std::uint8_t value) {
        record_delta([&] { writer_.write_byte(value); });
    }
    void write_bytes(std::span<const std::byte> data) {
        record_delta([&] { writer_.write_bytes(data); });
    }
    void write_string(std::string_view text) {
        record_delta([&] { writer_.write_string(text); });
    }

    // Allows helpers written against BinaryWriter& to participate in checksum tracking.
    template <typename Fn>
    void write_via(Fn&& fn) {
        record_delta([&] { fn(writer_); });
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return writer_.bytes(); }
    [[nodiscard]] std::size_t size() const noexcept { return writer_.size(); }
    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }
    [[nodiscard]] bool verify_checksum() const noexcept {
        return compute_savegame_checksum(writer_.bytes()) == checksum_;
    }

    void clear() noexcept {
        writer_.clear();
        checksum_ = 0U;
    }

private:
    template <typename Fn>
    void record_delta(Fn&& fn) {
        const std::size_t before = writer_.size();
        fn();
        const std::span<const std::byte> written = writer_.bytes();
        for (std::size_t i = before; i < written.size(); ++i) {
            checksum_ += static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(written[i]));
        }
    }

    BinaryWriter writer_{};
    std::uint64_t checksum_ = 0U;
};

}  // namespace wse::io
