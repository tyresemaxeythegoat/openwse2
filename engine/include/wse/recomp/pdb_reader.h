// Minimal MSF 7.00 / PDB reader for the recompiler: PDB info GUID/age and S_PUB32 publics.
//
// Reads a user-supplied PDB from memory (never bundled). Implemented from the public MSF 7.00
// and CodeView symbol-record formats: superblock, block map, stream directory, stream 1 (PDB
// info), stream 3 (DBI header -> SymRecordStream), and S_PUB32 (0x110E) records. Every block
// index, size, and count is bounds-checked; any inconsistency fails with a message (never
// reads out of range, never throws).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/recomp/pe_image.h"

namespace wse::recomp {

struct PdbGuidAge {
    std::array<std::uint8_t, 16> guid{};
    std::uint32_t age = 0;
};

struct PdbPublic {
    std::string name;
    std::uint16_t section = 0;
    std::uint32_t offset = 0;
    std::uint32_t flags = 0;
};  // S_PUB32 fields; `section` is 1-based

struct PdbFile {
    std::uint32_t block_size = 0;
    std::vector<std::vector<std::uint8_t>> streams;  // only the streams that were requested
};

namespace pdb_detail {

inline constexpr std::uint16_t kSymPub32 = 0x110EU;
inline constexpr std::size_t kMaxCodeViewRecords = 50'000'000U;
inline constexpr std::uint32_t kEmptyStreamSize = 0xFFFFFFFFU;

inline bool set_error(std::string* error, const char* msg) {
    if (error != nullptr) {
        *error = msg;
    }
    return false;
}

inline bool rd16(const std::uint8_t* p, std::size_t n, std::size_t off,
                 std::uint16_t& out) noexcept {
    if (off > n || n - off < 2U) {
        return false;
    }
    out = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[off]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[off + 1U]) << 8U));
    return true;
}

inline bool rd32(const std::uint8_t* p, std::size_t n, std::size_t off,
                 std::uint32_t& out) noexcept {
    if (off > n || n - off < 4U) {
        return false;
    }
    out = static_cast<std::uint32_t>(p[off]) |
          (static_cast<std::uint32_t>(p[off + 1U]) << 8U) |
          (static_cast<std::uint32_t>(p[off + 2U]) << 16U) |
          (static_cast<std::uint32_t>(p[off + 3U]) << 24U);
    return true;
}

struct StreamSpan {
    std::uint32_t byte_size = 0;
    std::uint32_t dir_block_list_off = 0;
    std::uint32_t block_count = 0;
};

struct MsfDirectory {
    std::uint32_t block_size = 0;
    std::uint32_t num_blocks = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<StreamSpan> streams;
};

inline bool parse_msf_directory(const std::uint8_t* data, std::size_t size, MsfDirectory& out,
                                std::string* error) {
    out = MsfDirectory{};
    if (data == nullptr || size < 56U) {
        return set_error(error, "truncated MSF 7.00 superblock");
    }
    static constexpr std::uint8_t kMsf7Magic[32] = {
        'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ', 'C', '/', 'C', '+', '+', ' ',
        'M', 'S', 'F', ' ', '7', '.', '0', '0', '\r', '\n', 0x1A, 'D', 'S', 0x00, 0x00, 0x00,
    };
    if (std::memcmp(data, kMsf7Magic, sizeof(kMsf7Magic)) != 0) {
        return set_error(error, "invalid MSF 7.00 magic");
    }

    std::uint32_t block_size = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t num_dir_bytes = 0;
    std::uint32_t block_map_addr = 0;
    (void)rd32(data, size, 32U, block_size);
    (void)rd32(data, size, 40U, num_blocks);
    (void)rd32(data, size, 44U, num_dir_bytes);
    (void)rd32(data, size, 52U, block_map_addr);

    if (block_size != 512U && block_size != 1024U && block_size != 2048U &&
        block_size != 4096U) {
        return set_error(error, "unsupported MSF block size");
    }
    if (num_blocks == 0U) {
        return set_error(error, "MSF block count is zero");
    }
    const std::uint64_t total_msf_bytes =
        static_cast<std::uint64_t>(num_blocks) * static_cast<std::uint64_t>(block_size);
    if (total_msf_bytes > static_cast<std::uint64_t>(size)) {
        return set_error(error, "MSF block count exceeds file size");
    }
    if (block_map_addr >= num_blocks) {
        return set_error(error, "MSF block map address out of range");
    }
    if (num_dir_bytes < 4U) {
        return set_error(error, "MSF stream directory too small");
    }

    const std::uint64_t num_dir_blocks =
        (static_cast<std::uint64_t>(num_dir_bytes) + static_cast<std::uint64_t>(block_size) - 1ULL) /
        static_cast<std::uint64_t>(block_size);
    if (num_dir_blocks > static_cast<std::uint64_t>(num_blocks)) {
        return set_error(error, "MSF stream directory exceeds block count");
    }

    const std::uint64_t map_byte_off =
        static_cast<std::uint64_t>(block_map_addr) * static_cast<std::uint64_t>(block_size);
    const std::uint64_t map_byte_len = num_dir_blocks * 4ULL;
    if (map_byte_len > total_msf_bytes - map_byte_off) {
        return set_error(error, "MSF block map extends past end of file");
    }

    out.block_size = block_size;
    out.num_blocks = num_blocks;
    out.bytes.resize(num_dir_bytes);

    std::size_t dir_copied = 0;
    for (std::uint64_t i = 0; i < num_dir_blocks; ++i) {
        std::uint32_t blk = 0;
        (void)rd32(data, size, static_cast<std::size_t>(map_byte_off + i * 4ULL), blk);
        if (blk >= num_blocks) {
            return set_error(error, "MSF directory block index out of range");
        }
        const std::size_t blk_off =
            static_cast<std::size_t>(static_cast<std::uint64_t>(blk) * block_size);
        const std::size_t rem = num_dir_bytes - dir_copied;
        const std::size_t chunk =
            rem < block_size ? rem : static_cast<std::size_t>(block_size);
        std::memcpy(out.bytes.data() + dir_copied, data + blk_off, chunk);
        dir_copied += chunk;
    }

    std::uint32_t num_streams = 0;
    if (!rd32(out.bytes.data(), out.bytes.size(), 0U, num_streams)) {
        return set_error(error, "MSF stream directory truncated");
    }
    const std::uint64_t sizes_end = 4ULL + static_cast<std::uint64_t>(num_streams) * 4ULL;
    if (sizes_end > out.bytes.size()) {
        return set_error(error, "MSF stream directory truncated in stream sizes");
    }

    out.streams.reserve(num_streams);
    std::size_t block_list_pos = static_cast<std::size_t>(sizes_end);
    for (std::uint32_t s = 0; s < num_streams; ++s) {
        std::uint32_t raw_size = 0;
        (void)rd32(out.bytes.data(), out.bytes.size(),
                   4U + static_cast<std::size_t>(s) * 4U, raw_size);
        StreamSpan span;
        span.dir_block_list_off = static_cast<std::uint32_t>(block_list_pos);
        if (raw_size != kEmptyStreamSize) {
            if (static_cast<std::uint64_t>(raw_size) > total_msf_bytes) {
                return set_error(error, "MSF stream size exceeds file size");
            }
            span.byte_size = raw_size;
            span.block_count = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(raw_size) + block_size - 1ULL) / block_size);
        }

        const std::uint64_t needed_bytes = static_cast<std::uint64_t>(span.block_count) * 4ULL;
        if (needed_bytes > out.bytes.size() - block_list_pos) {
            return set_error(error, "MSF stream size exceeds its block list");
        }
        for (std::uint32_t b = 0; b < span.block_count; ++b) {
            std::uint32_t blk = 0;
            (void)rd32(out.bytes.data(), out.bytes.size(),
                       block_list_pos + static_cast<std::size_t>(b) * 4U, blk);
            if (blk >= num_blocks) {
                return set_error(error, "MSF stream block index out of range");
            }
        }
        out.streams.push_back(span);
        block_list_pos += static_cast<std::size_t>(needed_bytes);
    }
    return true;
}

inline bool read_msf_stream(const std::uint8_t* data, std::size_t size, const MsfDirectory& dir,
                            std::uint32_t stream_idx, std::vector<std::uint8_t>& out,
                            std::string* error) {
    out.clear();
    if (stream_idx >= dir.streams.size()) {
        return set_error(error, "MSF stream index out of range");
    }
    const StreamSpan& span = dir.streams[stream_idx];
    if (span.byte_size == 0U) {
        return true;
    }
    if (data == nullptr ||
        static_cast<std::uint64_t>(dir.num_blocks) * dir.block_size > size) {
        return set_error(error, "MSF file buffer too small for stream");
    }
    out.resize(span.byte_size);
    std::size_t copied = 0;
    for (std::uint32_t b = 0; b < span.block_count; ++b) {
        std::uint32_t blk = 0;
        (void)rd32(dir.bytes.data(), dir.bytes.size(),
                   static_cast<std::size_t>(span.dir_block_list_off) +
                       static_cast<std::size_t>(b) * 4U,
                   blk);
        const std::size_t blk_off =
            static_cast<std::size_t>(static_cast<std::uint64_t>(blk) * dir.block_size);
        const std::size_t rem = span.byte_size - copied;
        const std::size_t chunk =
            rem < dir.block_size ? rem : static_cast<std::size_t>(dir.block_size);
        std::memcpy(out.data() + copied, data + blk_off, chunk);
        copied += chunk;
    }
    return true;
}

inline bool parse_pdb_info(const std::vector<std::uint8_t>& stream1, PdbGuidAge& out,
                           std::string* error) {
    out = PdbGuidAge{};
    if (stream1.size() < 28U) {
        return set_error(error, "PDB info stream (stream 1) truncated");
    }
    (void)rd32(stream1.data(), stream1.size(), 8U, out.age);
    std::memcpy(out.guid.data(), stream1.data() + 12U, 16U);
    return true;
}

inline bool parse_dbi_sym_stream(const std::vector<std::uint8_t>& stream3,
                                 std::size_t num_streams, std::uint16_t& sym_stream_out,
                                 std::string* error) {
    sym_stream_out = 0;
    if (stream3.size() < 64U) {
        return set_error(error, "DBI stream (stream 3) header truncated");
    }
    std::uint32_t version_signature = 0;
    (void)rd32(stream3.data(), stream3.size(), 0U, version_signature);
    if (version_signature != 0xFFFFFFFFU) {
        return set_error(error, "DBI stream has invalid VersionSignature");
    }
    (void)rd16(stream3.data(), stream3.size(), 20U, sym_stream_out);
    if (static_cast<std::size_t>(sym_stream_out) >= num_streams) {
        return set_error(error, "DBI SymRecordStream index out of range");
    }
    return true;
}

inline bool collect_publics(const std::vector<std::uint8_t>& sym,
                            std::vector<PdbPublic>& publics, std::string* error) {
    publics.clear();
    std::size_t pos = 0;
    std::size_t records_seen = 0;
    while (pos < sym.size()) {
        if (records_seen >= kMaxCodeViewRecords) {
            return set_error(error, "CodeView symbol record limit exceeded");
        }
        ++records_seen;
        std::uint16_t rec_len = 0;
        std::uint16_t rec_kind = 0;
        if (!rd16(sym.data(), sym.size(), pos, rec_len) ||
            !rd16(sym.data(), sym.size(), pos + 2U, rec_kind)) {
            return set_error(error, "truncated CodeView symbol record");
        }
        if (rec_len < 2U) {
            return set_error(error, "invalid CodeView symbol record length");
        }
        const std::size_t payload_bytes = static_cast<std::size_t>(rec_len) - 2U;
        if (payload_bytes > sym.size() - (pos + 4U)) {
            return set_error(error, "CodeView symbol record extends past stream end");
        }
        const std::size_t body_off = pos + 4U;
        if (rec_kind == kSymPub32) {
            if (payload_bytes < 10U) {
                return set_error(error, "truncated S_PUB32 record");
            }
            PdbPublic pub;
            (void)rd32(sym.data(), sym.size(), body_off + 0U, pub.flags);
            (void)rd32(sym.data(), sym.size(), body_off + 4U, pub.offset);
            (void)rd16(sym.data(), sym.size(), body_off + 8U, pub.section);
            const std::uint8_t* name_ptr = sym.data() + body_off + 10U;
            const std::size_t max_name_bytes = payload_bytes - 10U;
            const void* nul = std::memchr(name_ptr, '\0', max_name_bytes);
            if (nul == nullptr) {
                return set_error(error, "unterminated S_PUB32 symbol name");
            }
            const std::size_t name_len =
                static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - name_ptr);
            pub.name.assign(reinterpret_cast<const char*>(name_ptr), name_len);
            publics.push_back(std::move(pub));
        }
        pos += 2U + static_cast<std::size_t>(rec_len);
    }
    return true;
}

}  // namespace pdb_detail

// MSF 7.00 ("Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"): superblock, block map, stream
// directory, then the streams. Every block index, size and count is bounds-checked; any
// inconsistency fails with a message (never reads out of range, never throws).
inline bool read_pdb_publics(const std::uint8_t* data, std::size_t size,
                             std::vector<PdbPublic>& publics, PdbGuidAge* info,
                             std::string* error) {
    publics.clear();
    if (info != nullptr) {
        *info = PdbGuidAge{};
    }

    pdb_detail::MsfDirectory dir;
    if (!pdb_detail::parse_msf_directory(data, size, dir, error)) {
        return false;
    }
    if (dir.streams.size() <= 3U) {
        return pdb_detail::set_error(error, "MSF missing PDB info or DBI stream");
    }

    PdbFile pdb;
    pdb.block_size = dir.block_size;
    pdb.streams.resize(3U);

    if (!pdb_detail::read_msf_stream(data, size, dir, 1U, pdb.streams[0], error)) {
        return false;
    }
    PdbGuidAge parsed_info;
    if (!pdb_detail::parse_pdb_info(pdb.streams[0], parsed_info, error)) {
        return false;
    }

    if (!pdb_detail::read_msf_stream(data, size, dir, 3U, pdb.streams[1], error)) {
        return false;
    }
    std::uint16_t sym_stream = 0;
    if (!pdb_detail::parse_dbi_sym_stream(pdb.streams[1], dir.streams.size(), sym_stream, error)) {
        return false;
    }

    if (!pdb_detail::read_msf_stream(data, size, dir, sym_stream, pdb.streams[2], error)) {
        return false;
    }
    std::vector<PdbPublic> collected;
    if (!pdb_detail::collect_publics(pdb.streams[2], collected, error)) {
        return false;
    }

    publics = std::move(collected);
    if (info != nullptr) {
        *info = parsed_info;
    }
    return true;
}

// Convenience: VA of a public by exact (decorated) name, using 1-based section numbers into
// `sections` (PeImage::sections from pe_image.h) and `image_base`; nullopt when absent.
inline std::optional<std::uint64_t> pdb_public_va(const std::vector<PdbPublic>& publics,
                                                  std::string_view name,
                                                  const std::vector<PeSection>& sections,
                                                  std::uint64_t image_base) {
    for (const PdbPublic& pub : publics) {
        if (pub.name != name) {
            continue;
        }
        if (pub.section == 0U || static_cast<std::size_t>(pub.section) > sections.size()) {
            return std::nullopt;
        }
        const PeSection& sec = sections[static_cast<std::size_t>(pub.section) - 1U];
        const std::uint64_t rva =
            static_cast<std::uint64_t>(sec.virtual_address) + static_cast<std::uint64_t>(pub.offset);
        if (image_base > std::numeric_limits<std::uint64_t>::max() - rva) {
            return std::nullopt;
        }
        return image_base + rva;
    }
    return std::nullopt;
}

}  // namespace wse::recomp
