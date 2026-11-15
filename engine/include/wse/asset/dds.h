// Clean-room v1.1.5.5 .dds texture parser, software RGBA8 block decoder,
// and Warband Vfs texture path resolver.
//
// Reconstructed from v1.1.5.5 PE + PDB evidence:
// - rglTexture (0x1aadd, 288 B), rglTexture::initDeviceObjects (0x140317710)
// - rglResourceManager::tryGetTexture (0x1402f2f10)
// - Search templates: "Textures\\%s" (0x14056b3f8), "TexturesHD\\%s" (0x14056b408),
//   "%sTextures\\%s" (0x14056b418), "Textures\\languages\\%s\\%s" (0x14056b428),
//   "%sTextures\\languages\\%s\\%s.dds" (0x14056b448)
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/game/module_loader_common.h"
#include "wse/io/binary_stream.h"
#include "wse/platform/vfs.h"

namespace wse::asset {

enum class DdsFormat : std::uint8_t {
    Unknown = 0,
    BC1_UNORM,      // DXT1
    BC2_UNORM,      // DXT2 / DXT3
    BC3_UNORM,      // DXT4 / DXT5
    BC4_UNORM,      // ATI1 / BC4U
    BC5_UNORM,      // ATI2 / BC5U
    B8G8R8A8_UNORM, // A8R8G8B8
    R8G8B8A8_UNORM, // A8B8G8R8 / DXGI R8G8B8A8
    B8G8R8X8_UNORM, // X8R8G8B8
    R8G8B8X8_UNORM, // X8B8G8R8
    B8G8R8_UNORM,   // R8G8B8 (24-bit BGR byte order)
    B5G6R5_UNORM,   // R5G6B5
    B5G5R5A1_UNORM, // A1R5G5B5
    B4G4R4A4_UNORM, // A4R4G4B4
    A8_UNORM,       // A8
    L8_UNORM,       // L8 / R8
    L16_UNORM,      // L16 / R16 (D3DFMT_L16 / DXGI R16_UNORM)
    A8L8_UNORM,     // A8L8
};

struct DdsMipSlice {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::span<const std::byte> data{};
};

struct DdsImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 0;
    std::uint32_t face_count = 1;
    bool is_cubemap = false;
    bool has_dx10_header = false;
    bool has_alpha = false;
    DdsFormat format = DdsFormat::Unknown;
    std::vector<DdsMipSlice> slices{};  // indexed [face * mip_count + mip]
};

namespace dds_detail {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

inline bool set_error(std::string* error_out, std::string msg) {
    if (error_out != nullptr) *error_out = std::move(msg);
    return false;
}

inline DdsFormat map_dxgi_format(std::uint32_t dxgi) noexcept {
    switch (dxgi) {
        case 28: case 29: return DdsFormat::R8G8B8A8_UNORM;
        case 56: return DdsFormat::L16_UNORM;
        case 61: return DdsFormat::L8_UNORM;
        case 65: return DdsFormat::A8_UNORM;
        case 71: case 72: return DdsFormat::BC1_UNORM;
        case 74: case 75: return DdsFormat::BC2_UNORM;
        case 77: case 78: return DdsFormat::BC3_UNORM;
        case 80: return DdsFormat::BC4_UNORM;
        case 83: return DdsFormat::BC5_UNORM;
        case 85: return DdsFormat::B5G6R5_UNORM;
        case 86: return DdsFormat::B5G5R5A1_UNORM;
        case 87: case 91: return DdsFormat::B8G8R8A8_UNORM;
        case 88: case 93: return DdsFormat::B8G8R8X8_UNORM;
        case 115: return DdsFormat::B4G4R4A4_UNORM;
        default: return DdsFormat::Unknown;
    }
}

inline DdsFormat identify_pixel_format(std::uint32_t pf_flags,
                                       std::uint32_t pf_fourcc,
                                       std::uint32_t bpp,
                                       std::uint32_t r_mask,
                                       std::uint32_t g_mask,
                                       std::uint32_t b_mask,
                                       std::uint32_t a_mask,
                                       std::uint32_t dxgi_format,
                                       bool has_dx10,
                                       bool& has_alpha_out) noexcept {
    has_alpha_out = (pf_flags & 0x1U) != 0U || (pf_flags & 0x2U) != 0U;
    if (has_dx10) {
        const DdsFormat fmt = map_dxgi_format(dxgi_format);
        has_alpha_out = (fmt == DdsFormat::BC1_UNORM || fmt == DdsFormat::BC2_UNORM ||
                         fmt == DdsFormat::BC3_UNORM || fmt == DdsFormat::R8G8B8A8_UNORM ||
                         fmt == DdsFormat::B8G8R8A8_UNORM || fmt == DdsFormat::B5G5R5A1_UNORM ||
                         fmt == DdsFormat::B4G4R4A4_UNORM || fmt == DdsFormat::A8_UNORM);
        return fmt;
    }
    if ((pf_flags & 0x4U) != 0U) {  // DDPF_FOURCC
        switch (pf_fourcc) {
            case fourcc('D', 'X', 'T', '1'):
                has_alpha_out = true;
                return DdsFormat::BC1_UNORM;
            case fourcc('D', 'X', 'T', '2'):
            case fourcc('D', 'X', 'T', '3'):
                has_alpha_out = true;
                return DdsFormat::BC2_UNORM;
            case fourcc('D', 'X', 'T', '4'):
            case fourcc('D', 'X', 'T', '5'):
                has_alpha_out = true;
                return DdsFormat::BC3_UNORM;
            case fourcc('A', 'T', 'I', '1'):
            case fourcc('B', 'C', '4', 'U'):
                has_alpha_out = false;
                return DdsFormat::BC4_UNORM;
            case fourcc('A', 'T', 'I', '2'):
            case fourcc('B', 'C', '5', 'U'):
                has_alpha_out = false;
                return DdsFormat::BC5_UNORM;
            case 20U: has_alpha_out = false; return DdsFormat::B8G8R8_UNORM;
            case 21U: has_alpha_out = true; return DdsFormat::B8G8R8A8_UNORM;
            case 22U: has_alpha_out = false; return DdsFormat::B8G8R8X8_UNORM;
            case 23U: has_alpha_out = false; return DdsFormat::B5G6R5_UNORM;
            case 25U: has_alpha_out = true; return DdsFormat::B5G5R5A1_UNORM;
            case 26U: has_alpha_out = true; return DdsFormat::B4G4R4A4_UNORM;
            case 28U: has_alpha_out = true; return DdsFormat::A8_UNORM;
            case 32U: has_alpha_out = true; return DdsFormat::R8G8B8A8_UNORM;
            case 50U: has_alpha_out = false; return DdsFormat::L8_UNORM;
            case 51U: has_alpha_out = true; return DdsFormat::A8L8_UNORM;
            case 81U: has_alpha_out = false; return DdsFormat::L16_UNORM;
            default: return DdsFormat::Unknown;
        }
    }
    if ((pf_flags & 0x40U) != 0U) {  // DDPF_RGB
        if (bpp == 32U && r_mask == 0x00FF0000U && g_mask == 0x0000FF00U && b_mask == 0x000000FFU) {
            return (a_mask == 0xFF000000U) ? DdsFormat::B8G8R8A8_UNORM : DdsFormat::B8G8R8X8_UNORM;
        }
        if (bpp == 32U && r_mask == 0x000000FFU && g_mask == 0x0000FF00U && b_mask == 0x00FF0000U) {
            return (a_mask == 0xFF000000U) ? DdsFormat::R8G8B8A8_UNORM : DdsFormat::R8G8B8X8_UNORM;
        }
        if (bpp == 24U && r_mask == 0x00FF0000U && g_mask == 0x0000FF00U && b_mask == 0x000000FFU) {
            has_alpha_out = false;
            return DdsFormat::B8G8R8_UNORM;
        }
        if (bpp == 16U && r_mask == 0xF800U && g_mask == 0x07E0U && b_mask == 0x001FU) {
            has_alpha_out = false;
            return DdsFormat::B5G6R5_UNORM;
        }
        if (bpp == 16U && r_mask == 0x7C00U && g_mask == 0x03E0U && b_mask == 0x001FU && a_mask == 0x8000U) {
            has_alpha_out = true;
            return DdsFormat::B5G5R5A1_UNORM;
        }
        if (bpp == 16U && r_mask == 0x0F00U && g_mask == 0x00F0U && b_mask == 0x000FU && a_mask == 0xF000U) {
            has_alpha_out = true;
            return DdsFormat::B4G4R4A4_UNORM;
        }
    }
    if ((pf_flags & 0x20000U) != 0U) {  // DDPF_LUMINANCE
        if (bpp == 8U && r_mask == 0xFFU) {
            has_alpha_out = false;
            return DdsFormat::L8_UNORM;
        }
        if (bpp == 16U && r_mask == 0xFFFFU && g_mask == 0U && b_mask == 0U && a_mask == 0U) {
            has_alpha_out = false;
            return DdsFormat::L16_UNORM;
        }
        if (bpp == 16U && r_mask == 0x00FFU && a_mask == 0xFF00U) {
            has_alpha_out = true;
            return DdsFormat::A8L8_UNORM;
        }
    }
    if ((pf_flags & 0x2U) != 0U && bpp == 8U && a_mask == 0xFFU) {  // DDPF_ALPHA
        has_alpha_out = true;
        return DdsFormat::A8_UNORM;
    }
    return DdsFormat::Unknown;
}

inline bool compute_mip_layout(DdsFormat fmt,
                               std::uint32_t w,
                               std::uint32_t h,
                               std::uint32_t& row_pitch_out,
                               std::size_t& byte_size_out) noexcept {
    const std::uint64_t bw = (static_cast<std::uint64_t>(w) + 3ULL) / 4ULL;
    const std::uint64_t bh = (static_cast<std::uint64_t>(h) + 3ULL) / 4ULL;
    switch (fmt) {
        case DdsFormat::BC1_UNORM:
        case DdsFormat::BC4_UNORM: {
            const std::uint64_t blocks_w = (bw > 0) ? bw : 1;
            const std::uint64_t blocks_h = (bh > 0) ? bh : 1;
            const std::uint64_t rp = blocks_w * 8ULL;
            if (rp > 0xFFFFFFFFULL) return false;
            row_pitch_out = static_cast<std::uint32_t>(rp);
            byte_size_out = static_cast<std::size_t>(rp * blocks_h);
            return true;
        }
        case DdsFormat::BC2_UNORM:
        case DdsFormat::BC3_UNORM:
        case DdsFormat::BC5_UNORM: {
            const std::uint64_t blocks_w = (bw > 0) ? bw : 1;
            const std::uint64_t blocks_h = (bh > 0) ? bh : 1;
            const std::uint64_t rp = blocks_w * 16ULL;
            if (rp > 0xFFFFFFFFULL) return false;
            row_pitch_out = static_cast<std::uint32_t>(rp);
            byte_size_out = static_cast<std::size_t>(rp * blocks_h);
            return true;
        }
        case DdsFormat::B8G8R8A8_UNORM:
        case DdsFormat::R8G8B8A8_UNORM:
        case DdsFormat::B8G8R8X8_UNORM:
        case DdsFormat::R8G8B8X8_UNORM: {
            const std::uint64_t rp = static_cast<std::uint64_t>(w) * 4ULL;
            row_pitch_out = static_cast<std::uint32_t>(rp);
            byte_size_out = static_cast<std::size_t>(rp * h);
            return true;
        }
        case DdsFormat::B8G8R8_UNORM: {
            const std::uint64_t rp = static_cast<std::uint64_t>(w) * 3ULL;
            row_pitch_out = static_cast<std::uint32_t>(rp);
            byte_size_out = static_cast<std::size_t>(rp * h);
            return true;
        }
        case DdsFormat::B5G6R5_UNORM:
        case DdsFormat::B5G5R5A1_UNORM:
        case DdsFormat::B4G4R4A4_UNORM:
        case DdsFormat::L16_UNORM:
        case DdsFormat::A8L8_UNORM: {
            const std::uint64_t rp = static_cast<std::uint64_t>(w) * 2ULL;
            row_pitch_out = static_cast<std::uint32_t>(rp);
            byte_size_out = static_cast<std::size_t>(rp * h);
            return true;
        }
        case DdsFormat::A8_UNORM:
        case DdsFormat::L8_UNORM: {
            row_pitch_out = w;
            byte_size_out = static_cast<std::size_t>(static_cast<std::uint64_t>(w) * h);
            return true;
        }
        default:
            return false;
    }
}

inline std::uint16_t read_u16_le(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(p[0]) |
                                       (std::to_integer<std::uint16_t>(p[1]) << 8));
}

inline std::uint32_t read_u32_le(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(p[0]) |
                                       (std::to_integer<std::uint32_t>(p[1]) << 8) |
                                       (std::to_integer<std::uint32_t>(p[2]) << 16) |
                                       (std::to_integer<std::uint32_t>(p[3]) << 24));
}

inline void decode_rgb565(std::uint16_t c, std::array<std::uint8_t, 4>& rgba) noexcept {
    const std::uint8_t r5 = static_cast<std::uint8_t>((c >> 11) & 0x1FU);
    const std::uint8_t g6 = static_cast<std::uint8_t>((c >> 5) & 0x3FU);
    const std::uint8_t b5 = static_cast<std::uint8_t>(c & 0x1FU);
    rgba[0] = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
    rgba[1] = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
    rgba[2] = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
    rgba[3] = 255;
}

inline void decode_bc_color_palette(std::uint16_t c0,
                                    std::uint16_t c1,
                                    bool allow_1bit_alpha,
                                    std::array<std::array<std::uint8_t, 4>, 4>& out) noexcept {
    decode_rgb565(c0, out[0]);
    decode_rgb565(c1, out[1]);
    if (!allow_1bit_alpha || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            out[2][i] = static_cast<std::uint8_t>((2U * out[0][i] + out[1][i]) / 3U);
            out[3][i] = static_cast<std::uint8_t>((out[0][i] + 2U * out[1][i]) / 3U);
        }
        out[2][3] = 255;
        out[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            out[2][i] = static_cast<std::uint8_t>((out[0][i] + out[1][i]) / 2U);
            out[3][i] = 0;
        }
        out[2][3] = 255;
        out[3][3] = 0;
    }
}

inline std::array<std::uint8_t, 16> decode_bc_alpha_block(const std::byte* block) noexcept {
    const std::uint8_t a0 = std::to_integer<std::uint8_t>(block[0]);
    const std::uint8_t a1 = std::to_integer<std::uint8_t>(block[1]);
    std::array<std::uint8_t, 8> palette{};
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        for (int k = 2; k <= 7; ++k) {
            palette[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(
                ((8 - k) * static_cast<int>(a0) + (k - 1) * static_cast<int>(a1)) / 7);
        }
    } else {
        for (int k = 2; k <= 5; ++k) {
            palette[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(
                ((6 - k) * static_cast<int>(a0) + (k - 1) * static_cast<int>(a1)) / 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(block[2 + i])) << (8 * i);
    }
    std::array<std::uint8_t, 16> alphas{};
    for (int i = 0; i < 16; ++i) {
        const std::size_t idx = static_cast<std::size_t>((bits >> (3 * i)) & 0x7ULL);
        alphas[static_cast<std::size_t>(i)] = palette[idx];
    }
    return alphas;
}

inline void write_pixel(std::vector<std::uint8_t>& dst,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t x,
                        std::uint32_t y,
                        std::uint8_t r,
                        std::uint8_t g,
                        std::uint8_t b,
                        std::uint8_t a) noexcept {
    if (x >= width || y >= height) return;
    const std::size_t off = (static_cast<std::size_t>(y) * width + x) * 4U;
    dst[off + 0] = r;
    dst[off + 1] = g;
    dst[off + 2] = b;
    dst[off + 3] = a;
}

}  // namespace dds_detail

inline bool parse_dds(std::span<const std::byte> data,
                      DdsImageView& out,
                      std::string* error_out = nullptr) {
    out = DdsImageView{};
    wse::io::BinaryReader r(data);
    std::uint32_t magic = 0;
    if (!r.read_u32(magic) || magic != dds_detail::fourcc('D', 'D', 'S', ' ')) {
        return dds_detail::set_error(error_out, "invalid DDS magic");
    }

    std::uint32_t dwSize = 0;
    std::uint32_t dwFlags = 0;
    std::uint32_t dwHeight = 0;
    std::uint32_t dwWidth = 0;
    std::uint32_t dwPitchOrLinearSize = 0;
    std::uint32_t dwDepth = 0;
    std::uint32_t dwMipMapCount = 0;
    if (!r.read_u32(dwSize) || !r.read_u32(dwFlags) || !r.read_u32(dwHeight) ||
        !r.read_u32(dwWidth) || !r.read_u32(dwPitchOrLinearSize) ||
        !r.read_u32(dwDepth) || !r.read_u32(dwMipMapCount) ||
        dwSize != 124U || dwWidth == 0U || dwHeight == 0U) {
        return dds_detail::set_error(error_out, "invalid or truncated DDS_HEADER");
    }
    if (!r.skip(44U)) return dds_detail::set_error(error_out, "truncated DDS_HEADER reserved fields");

    std::uint32_t pf_size = 0, pf_flags = 0, pf_fourcc = 0, pf_bpp = 0;
    std::uint32_t pf_r = 0, pf_g = 0, pf_b = 0, pf_a = 0;
    if (!r.read_u32(pf_size) || !r.read_u32(pf_flags) || !r.read_u32(pf_fourcc) ||
        !r.read_u32(pf_bpp) || !r.read_u32(pf_r) || !r.read_u32(pf_g) ||
        !r.read_u32(pf_b) || !r.read_u32(pf_a) || pf_size != 32U) {
        return dds_detail::set_error(error_out, "invalid or truncated DDS_PIXELFORMAT");
    }

    std::uint32_t dwCaps = 0, dwCaps2 = 0;
    if (!r.read_u32(dwCaps) || !r.read_u32(dwCaps2) || !r.skip(12U)) {
        return dds_detail::set_error(error_out, "truncated DDS_HEADER caps");
    }
    (void)dwPitchOrLinearSize;
    (void)dwDepth;
    (void)dwCaps;

    std::uint32_t dxgi_format = 0;
    std::uint32_t dxt10_misc = 0;
    std::uint32_t array_size = 1U;
    const bool has_dx10 = (pf_flags & 0x4U) != 0U && pf_fourcc == dds_detail::fourcc('D', 'X', '1', '0');
    if (has_dx10) {
        std::uint32_t res_dim = 0, misc2 = 0;
        if (!r.read_u32(dxgi_format) || !r.read_u32(res_dim) ||
            !r.read_u32(dxt10_misc) || !r.read_u32(array_size) || !r.read_u32(misc2)) {
            return dds_detail::set_error(error_out, "truncated DDS_HEADER_DXT10");
        }
        (void)res_dim;
        (void)misc2;
        if (array_size == 0U) array_size = 1U;
    }

    out.width = dwWidth;
    out.height = dwHeight;
    out.mip_count = ((dwFlags & 0x20000U) != 0U && dwMipMapCount > 0U) ? dwMipMapCount : 1U;
    out.has_dx10_header = has_dx10;
    out.is_cubemap = ((dwCaps2 & 0x200U) != 0U) || (has_dx10 && ((dxt10_misc & 0x4U) != 0U));
    if (out.is_cubemap) {
        if ((dwCaps2 & 0xFC00U) != 0U) {
            std::uint32_t bits = dwCaps2 & 0xFC00U;
            std::uint32_t faces = 0;
            for (int i = 0; i < 6; ++i) {
                if ((bits & (0x400U << i)) != 0U) ++faces;
            }
            out.face_count = (faces > 0U) ? faces : 6U;
        } else {
            out.face_count = 6U * array_size;
        }
    } else {
        out.face_count = array_size;
    }
    if (out.face_count == 0U || out.mip_count > 32U || out.face_count > 64U) {
        return dds_detail::set_error(error_out, "unsupported DDS face or mip count");
    }

    out.format = dds_detail::identify_pixel_format(
        pf_flags, pf_fourcc, pf_bpp, pf_r, pf_g, pf_b, pf_a, dxgi_format, has_dx10, out.has_alpha);
    if (out.format == DdsFormat::Unknown) {
        return dds_detail::set_error(error_out, "unsupported DDS pixel format");
    }

    std::size_t cursor = r.cursor();
    out.slices.reserve(static_cast<std::size_t>(out.face_count) * out.mip_count);
    for (std::uint32_t f = 0; f < out.face_count; ++f) {
        std::uint32_t mw = out.width;
        std::uint32_t mh = out.height;
        for (std::uint32_t m = 0; m < out.mip_count; ++m) {
            std::uint32_t row_pitch = 0;
            std::size_t mip_bytes = 0;
            if (!dds_detail::compute_mip_layout(out.format, mw, mh, row_pitch, mip_bytes)) {
                return dds_detail::set_error(error_out, "DDS mip size overflow");
            }
            if (cursor > data.size() || mip_bytes > data.size() - cursor) {
                return dds_detail::set_error(error_out, "truncated DDS mip payload");
            }
            out.slices.push_back(DdsMipSlice{
                mw, mh, row_pitch, data.subspan(cursor, mip_bytes)
            });
            cursor += mip_bytes;
            mw = (std::max)(1U, mw >> 1);
            mh = (std::max)(1U, mh >> 1);
        }
    }
    return true;
}

inline bool decode_dds_mip_rgba8(const DdsImageView& image,
                                 std::size_t mip_index,
                                 std::size_t face_index,
                                 std::vector<std::uint8_t>& out_rgba8,
                                 std::string* error_out = nullptr) {
    if (face_index >= image.face_count || mip_index >= image.mip_count) {
        return dds_detail::set_error(error_out, "DDS mip/face index out of range");
    }
    const DdsMipSlice& slice = image.slices[face_index * image.mip_count + mip_index];
    const std::uint32_t w = slice.width;
    const std::uint32_t h = slice.height;
    out_rgba8.assign(static_cast<std::size_t>(w) * h * 4U, 0);

    const std::byte* src = slice.data.data();
    const std::uint32_t blocks_x = (w + 3U) / 4U;
    const std::uint32_t blocks_y = (h + 3U) / 4U;

    switch (image.format) {
        case DdsFormat::BC1_UNORM: {
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const std::byte* blk = src + (static_cast<std::size_t>(by) * blocks_x + bx) * 8U;
                    const std::uint16_t c0 = dds_detail::read_u16_le(blk);
                    const std::uint16_t c1 = dds_detail::read_u16_le(blk + 2);
                    const std::uint32_t idx_bits = dds_detail::read_u32_le(blk + 4);
                    std::array<std::array<std::uint8_t, 4>, 4> pal{};
                    dds_detail::decode_bc_color_palette(c0, c1, true, pal);
                    for (std::uint32_t py = 0; py < 4U; ++py) {
                        for (std::uint32_t px = 0; px < 4U; ++px) {
                            const auto& rgba = pal[(idx_bits >> ((py * 4U + px) * 2U)) & 0x3U];
                            dds_detail::write_pixel(
                                out_rgba8, w, h, bx * 4U + px, by * 4U + py,
                                rgba[0], rgba[1], rgba[2], rgba[3]);
                        }
                    }
                }
            }
            return true;
        }
        case DdsFormat::BC2_UNORM: {
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const std::byte* blk = src + (static_cast<std::size_t>(by) * blocks_x + bx) * 16U;
                    const std::uint16_t c0 = dds_detail::read_u16_le(blk + 8);
                    const std::uint16_t c1 = dds_detail::read_u16_le(blk + 10);
                    const std::uint32_t idx_bits = dds_detail::read_u32_le(blk + 12);
                    std::array<std::array<std::uint8_t, 4>, 4> pal{};
                    dds_detail::decode_bc_color_palette(c0, c1, false, pal);
                    for (std::uint32_t py = 0; py < 4U; ++py) {
                        const std::uint16_t row_a = dds_detail::read_u16_le(blk + py * 2U);
                        for (std::uint32_t px = 0; px < 4U; ++px) {
                            const std::uint8_t a4 = static_cast<std::uint8_t>((row_a >> (px * 4U)) & 0xFU);
                            const std::uint8_t alpha = static_cast<std::uint8_t>((a4 << 4) | a4);
                            const auto& rgb = pal[(idx_bits >> ((py * 4U + px) * 2U)) & 0x3U];
                            dds_detail::write_pixel(
                                out_rgba8, w, h, bx * 4U + px, by * 4U + py,
                                rgb[0], rgb[1], rgb[2], alpha);
                        }
                    }
                }
            }
            return true;
        }
        case DdsFormat::BC3_UNORM: {
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const std::byte* blk = src + (static_cast<std::size_t>(by) * blocks_x + bx) * 16U;
                    const auto alphas = dds_detail::decode_bc_alpha_block(blk);
                    const std::uint16_t c0 = dds_detail::read_u16_le(blk + 8);
                    const std::uint16_t c1 = dds_detail::read_u16_le(blk + 10);
                    const std::uint32_t idx_bits = dds_detail::read_u32_le(blk + 12);
                    std::array<std::array<std::uint8_t, 4>, 4> pal{};
                    dds_detail::decode_bc_color_palette(c0, c1, false, pal);
                    for (std::uint32_t py = 0; py < 4U; ++py) {
                        for (std::uint32_t px = 0; px < 4U; ++px) {
                            const std::uint32_t p = py * 4U + px;
                            const auto& rgb = pal[(idx_bits >> (p * 2U)) & 0x3U];
                            dds_detail::write_pixel(
                                out_rgba8, w, h, bx * 4U + px, by * 4U + py,
                                rgb[0], rgb[1], rgb[2], alphas[p]);
                        }
                    }
                }
            }
            return true;
        }
        case DdsFormat::BC4_UNORM: {
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const std::byte* blk = src + (static_cast<std::size_t>(by) * blocks_x + bx) * 8U;
                    const auto reds = dds_detail::decode_bc_alpha_block(blk);
                    for (std::uint32_t py = 0; py < 4U; ++py) {
                        for (std::uint32_t px = 0; px < 4U; ++px) {
                            const std::uint8_t v = reds[py * 4U + px];
                            dds_detail::write_pixel(out_rgba8, w, h, bx * 4U + px, by * 4U + py, v, v, v, 255);
                        }
                    }
                }
            }
            return true;
        }
        case DdsFormat::BC5_UNORM: {
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const std::byte* blk = src + (static_cast<std::size_t>(by) * blocks_x + bx) * 16U;
                    const auto reds = dds_detail::decode_bc_alpha_block(blk);
                    const auto greens = dds_detail::decode_bc_alpha_block(blk + 8);
                    for (std::uint32_t py = 0; py < 4U; ++py) {
                        for (std::uint32_t px = 0; px < 4U; ++px) {
                            const std::uint32_t p = py * 4U + px;
                            dds_detail::write_pixel(
                                out_rgba8, w, h, bx * 4U + px, by * 4U + py,
                                reds[p], greens[p], 0, 255);
                        }
                    }
                }
            }
            return true;
        }
        case DdsFormat::B8G8R8A8_UNORM:
        case DdsFormat::B8G8R8X8_UNORM:
        case DdsFormat::R8G8B8A8_UNORM:
        case DdsFormat::R8G8B8X8_UNORM: {
            const bool is_bgr = (image.format == DdsFormat::B8G8R8A8_UNORM ||
                                 image.format == DdsFormat::B8G8R8X8_UNORM);
            const bool opaque = (image.format == DdsFormat::B8G8R8X8_UNORM ||
                                 image.format == DdsFormat::R8G8B8X8_UNORM);
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t c0 = std::to_integer<std::uint8_t>(src[i * 4U + 0]);
                const std::uint8_t c1 = std::to_integer<std::uint8_t>(src[i * 4U + 1]);
                const std::uint8_t c2 = std::to_integer<std::uint8_t>(src[i * 4U + 2]);
                const std::uint8_t c3 = opaque ? 255 : std::to_integer<std::uint8_t>(src[i * 4U + 3]);
                out_rgba8[i * 4U + 0] = is_bgr ? c2 : c0;
                out_rgba8[i * 4U + 1] = c1;
                out_rgba8[i * 4U + 2] = is_bgr ? c0 : c2;
                out_rgba8[i * 4U + 3] = c3;
            }
            return true;
        }
        case DdsFormat::B8G8R8_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                out_rgba8[i * 4U + 0] = std::to_integer<std::uint8_t>(src[i * 3U + 2]);
                out_rgba8[i * 4U + 1] = std::to_integer<std::uint8_t>(src[i * 3U + 1]);
                out_rgba8[i * 4U + 2] = std::to_integer<std::uint8_t>(src[i * 3U + 0]);
                out_rgba8[i * 4U + 3] = 255;
            }
            return true;
        }
        case DdsFormat::B5G6R5_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                std::array<std::uint8_t, 4> rgba{};
                dds_detail::decode_rgb565(dds_detail::read_u16_le(src + i * 2U), rgba);
                out_rgba8[i * 4U + 0] = rgba[0];
                out_rgba8[i * 4U + 1] = rgba[1];
                out_rgba8[i * 4U + 2] = rgba[2];
                out_rgba8[i * 4U + 3] = 255;
            }
            return true;
        }
        case DdsFormat::B5G5R5A1_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint16_t px = dds_detail::read_u16_le(src + i * 2U);
                const std::uint8_t r5 = static_cast<std::uint8_t>((px >> 10) & 0x1FU);
                const std::uint8_t g5 = static_cast<std::uint8_t>((px >> 5) & 0x1FU);
                const std::uint8_t b5 = static_cast<std::uint8_t>(px & 0x1FU);
                out_rgba8[i * 4U + 0] = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
                out_rgba8[i * 4U + 1] = static_cast<std::uint8_t>((g5 << 3) | (g5 >> 2));
                out_rgba8[i * 4U + 2] = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
                out_rgba8[i * 4U + 3] = (px & 0x8000U) != 0U ? 255 : 0;
            }
            return true;
        }
        case DdsFormat::B4G4R4A4_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint16_t px = dds_detail::read_u16_le(src + i * 2U);
                const auto expand4 = [](std::uint16_t v) noexcept -> std::uint8_t {
                    return static_cast<std::uint8_t>((v << 4) | v);
                };
                out_rgba8[i * 4U + 0] = expand4((px >> 8) & 0xFU);
                out_rgba8[i * 4U + 1] = expand4((px >> 4) & 0xFU);
                out_rgba8[i * 4U + 2] = expand4(px & 0xFU);
                out_rgba8[i * 4U + 3] = expand4((px >> 12) & 0xFU);
            }
            return true;
        }
        case DdsFormat::A8_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                out_rgba8[i * 4U + 0] = 255;
                out_rgba8[i * 4U + 1] = 255;
                out_rgba8[i * 4U + 2] = 255;
                out_rgba8[i * 4U + 3] = std::to_integer<std::uint8_t>(src[i]);
            }
            return true;
        }
        case DdsFormat::L8_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t v = std::to_integer<std::uint8_t>(src[i]);
                out_rgba8[i * 4U + 0] = v;
                out_rgba8[i * 4U + 1] = v;
                out_rgba8[i * 4U + 2] = v;
                out_rgba8[i * 4U + 3] = 255;
            }
            return true;
        }
        case DdsFormat::L16_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t v = static_cast<std::uint8_t>(dds_detail::read_u16_le(src + i * 2U) >> 8);
                out_rgba8[i * 4U + 0] = v;
                out_rgba8[i * 4U + 1] = v;
                out_rgba8[i * 4U + 2] = v;
                out_rgba8[i * 4U + 3] = 255;
            }
            return true;
        }
        case DdsFormat::A8L8_UNORM: {
            const std::size_t count = static_cast<std::size_t>(w) * h;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t l = std::to_integer<std::uint8_t>(src[i * 2U + 0]);
                const std::uint8_t a = std::to_integer<std::uint8_t>(src[i * 2U + 1]);
                out_rgba8[i * 4U + 0] = l;
                out_rgba8[i * 4U + 1] = l;
                out_rgba8[i * 4U + 2] = l;
                out_rgba8[i * 4U + 3] = a;
            }
            return true;
        }
        default:
            return dds_detail::set_error(error_out, "unsupported DDS format for RGBA8 decode");
    }
}

// Resolves a texture identifier through the Warband search chain matching
// rglResourceManager::tryGetTexture (0x1402f2f10) + rglTexture::initDeviceObjects (0x140317710).
inline std::string resolve_texture_path(wse::platform::Vfs& vfs,
                                        std::string_view texture_name,
                                        std::string_view warband_root,
                                        std::string_view module_root = {},
                                        std::string_view language = {},
                                        bool use_textures_hd = false) {
    if (texture_name.empty()) return {};

    std::string clean(texture_name);
    std::replace(clean.begin(), clean.end(), '\\', '/');
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
    const std::string lower_clean = wse::platform::ascii_lower(clean);
    if (lower_clean.rfind("textures/", 0) == 0) {
        clean.erase(0, 9);
    }
    if (clean.empty()) return {};

    std::vector<std::string> candidates;
    const std::size_t slash = clean.find_last_of('/');
    const std::size_t dot = clean.find_last_of('.');
    const bool has_ext = (dot != std::string::npos) && (slash == std::string::npos || dot > slash);
    if (has_ext) {
        candidates.push_back(clean);
    } else {
        candidates.push_back(clean + ".dds");
        candidates.push_back(clean + ".jpg");
        candidates.push_back(clean + ".tga");
    }

    const auto probe_dir = [&](std::string_view dir) -> std::string {
        for (const std::string& file : candidates) {
            const std::string p = wse::game::vfs_join_path(dir, file);
            if (vfs.is_file(p)) return p;
        }
        return {};
    };

    if (!module_root.empty()) {
        if (!language.empty()) {
            const std::string mod_lang = wse::game::vfs_join_path(
                wse::game::vfs_join_path(module_root, "Textures/languages"), language);
            if (auto hit = probe_dir(mod_lang); !hit.empty()) return hit;
        }
        if (auto hit = probe_dir(wse::game::vfs_join_path(module_root, "Textures")); !hit.empty()) {
            return hit;
        }
    }
    if (!language.empty()) {
        const std::string base_lang = wse::game::vfs_join_path(
            wse::game::vfs_join_path(warband_root, "Textures/languages"), language);
        if (auto hit = probe_dir(base_lang); !hit.empty()) return hit;
    }
    if (use_textures_hd) {
        if (auto hit = probe_dir(wse::game::vfs_join_path(warband_root, "TexturesHD")); !hit.empty()) {
            return hit;
        }
    }
    if (auto hit = probe_dir(wse::game::vfs_join_path(warband_root, "Textures")); !hit.empty()) {
        return hit;
    }
    return {};
}

}  // namespace wse::asset
