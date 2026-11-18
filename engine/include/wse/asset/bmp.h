// Clean-room BMP image decoder, box-filter downscaler, and HSV seed colour
// extractor for launcher module art (`main.bmp`, docs/PLATFORM_PLAN.md, P12.2).
//
// Port infrastructure: no retail evidence or VA citations. Every input read is
// bounds-checked against the input byte span and no exceptions escape.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wse::asset {

inline constexpr std::uint32_t kBmpMaxDimension = 8192U;
inline constexpr std::uint32_t kDefaultSeedColorArgb = 0xFF5B6B8CU;

struct DecodedBmp {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> argb;  // Opaque ARGB (0xFFRRGGBB), rows top-down.
};

namespace bmp_detail {

inline bool fail(std::string* error, const char* message) noexcept {
    if (error != nullptr) {
        try {
            *error = message;
        } catch (...) {
            // Keep no-throw guarantee on error reporting.
        }
    }
    return false;
}

[[nodiscard]] inline std::uint16_t read_u16_le(std::span<const std::uint8_t> bytes,
                                               std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] inline std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes,
                                               std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] inline std::int32_t read_i32_le(std::span<const std::uint8_t> bytes,
                                              std::size_t offset) noexcept {
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

struct BitFieldMask {
    std::uint32_t mask = 0;
    unsigned shift = 0;
    unsigned bits = 0;
    std::uint32_t max_val = 0;
};

[[nodiscard]] inline bool parse_bitfield_mask(std::uint32_t mask, BitFieldMask& out) noexcept {
    out = BitFieldMask{};
    if (mask == 0U) {
        return true;
    }
    const unsigned shift = static_cast<unsigned>(std::countr_zero(mask));
    const std::uint32_t shifted = mask >> shift;
    if ((static_cast<std::uint64_t>(shifted) & (static_cast<std::uint64_t>(shifted) + 1ULL)) !=
        0ULL) {
        return false;
    }
    const unsigned bits = static_cast<unsigned>(std::popcount(shifted));
    out.mask = mask;
    out.shift = shift;
    out.bits = bits;
    out.max_val = (bits == 32U) ? 0xFFFFFFFFU : ((1U << bits) - 1U);
    return true;
}

[[nodiscard]] inline std::uint32_t extract_bitfield(std::uint32_t pixel,
                                                    const BitFieldMask& field) noexcept {
    if (field.bits == 0U) {
        return 0U;
    }
    const std::uint32_t raw = (pixel & field.mask) >> field.shift;
    if (field.bits == 8U) {
        return raw;
    }
    if (field.bits > 8U) {
        return (raw >> (field.bits - 8U)) & 0xFFU;
    }
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(raw) * 255ULL + field.max_val / 2ULL) / field.max_val);
}

}  // namespace bmp_detail

// Decodes uncompressed Windows BMPs (BITMAPINFOHEADER 40 B or V4/V5 >= 52 B):
// BI_RGB (4, 8, 24, 32 bpp) and BI_BITFIELDS (32 bpp), bottom-up and top-down.
// Output pixels are opaque ARGB (0xFFRRGGBB), rows top-down.
[[nodiscard]] inline bool decode_bmp(std::span<const std::uint8_t> bytes,
                                     DecodedBmp& out,
                                     std::string* error = nullptr) {
    out = DecodedBmp{};
    if (error != nullptr) {
        try {
            error->clear();
        } catch (...) {
            // Keep no-throw guarantee.
        }
    }

    constexpr std::size_t kFileHeaderSize = 14U;
    constexpr std::size_t kMinDibHeaderSize = 40U;
    constexpr std::size_t kBitfieldsHeaderSize = 52U;
    constexpr std::uint32_t kBiRgb = 0U;
    constexpr std::uint32_t kBiRle8 = 1U;
    constexpr std::uint32_t kBiRle4 = 2U;
    constexpr std::uint32_t kBiBitfields = 3U;

    if (bytes.size() < kFileHeaderSize + kMinDibHeaderSize) {
        return bmp_detail::fail(error, "BMP data too short for header");
    }
    if (bytes[0] != 0x42U || bytes[1] != 0x4DU) {
        return bmp_detail::fail(error, "invalid BMP signature");
    }

    const std::uint32_t pixel_offset = bmp_detail::read_u32_le(bytes, 10U);
    const std::uint32_t dib_size = bmp_detail::read_u32_le(bytes, 14U);
    if (dib_size < kMinDibHeaderSize) {
        return bmp_detail::fail(error, "unsupported BMP DIB header size");
    }
    if (static_cast<std::uint64_t>(dib_size) > bytes.size() - kFileHeaderSize) {
        return bmp_detail::fail(error, "truncated BMP DIB header");
    }

    const std::int32_t raw_width = bmp_detail::read_i32_le(bytes, 18U);
    const std::int32_t raw_height = bmp_detail::read_i32_le(bytes, 22U);
    const std::uint16_t planes = bmp_detail::read_u16_le(bytes, 26U);
    const std::uint16_t bit_count = bmp_detail::read_u16_le(bytes, 28U);
    const std::uint32_t compression = bmp_detail::read_u32_le(bytes, 30U);
    const std::uint32_t clr_used = bmp_detail::read_u32_le(bytes, 46U);

    if (raw_width <= 0 || raw_width > static_cast<std::int32_t>(kBmpMaxDimension)) {
        return bmp_detail::fail(error, "invalid BMP width");
    }
    const std::int64_t height64 = static_cast<std::int64_t>(raw_height);
    const bool top_down = (height64 < 0);
    const std::uint64_t abs_height = static_cast<std::uint64_t>(top_down ? -height64 : height64);
    if (abs_height == 0ULL || abs_height > kBmpMaxDimension) {
        return bmp_detail::fail(error, "invalid BMP height");
    }
    if (planes != 1U) {
        return bmp_detail::fail(error, "invalid BMP plane count");
    }

    if (compression == kBiRle8 || compression == kBiRle4) {
        return bmp_detail::fail(error, "BMP RLE compression is not supported");
    }

    const bool valid_rgb =
        (compression == kBiRgb) &&
        (bit_count == 4U || bit_count == 8U || bit_count == 24U || bit_count == 32U);
    const bool valid_bitfields = (compression == kBiBitfields) && (bit_count == 32U);
    if (!valid_rgb && !valid_bitfields) {
        return bmp_detail::fail(error, "unsupported BMP format or bit depth");
    }

    const std::uint32_t width = static_cast<std::uint32_t>(raw_width);
    const std::uint32_t height = static_cast<std::uint32_t>(abs_height);

    std::size_t after_header_masks = kFileHeaderSize + static_cast<std::size_t>(dib_size);
    bmp_detail::BitFieldMask r_field{};
    bmp_detail::BitFieldMask g_field{};
    bmp_detail::BitFieldMask b_field{};
    if (compression == kBiBitfields) {
        constexpr std::size_t kMasksOffset = kFileHeaderSize + kMinDibHeaderSize;  // 54
        if (dib_size == kMinDibHeaderSize) {
            after_header_masks = kMasksOffset + 12U;
        } else if (dib_size < kBitfieldsHeaderSize) {
            return bmp_detail::fail(error, "unsupported BMP header size for BI_BITFIELDS");
        }
        if (bytes.size() < after_header_masks || bytes.size() < kMasksOffset + 12U) {
            return bmp_detail::fail(error, "truncated BMP bitfield masks");
        }
        if (!bmp_detail::parse_bitfield_mask(bmp_detail::read_u32_le(bytes, kMasksOffset), r_field) ||
            !bmp_detail::parse_bitfield_mask(bmp_detail::read_u32_le(bytes, kMasksOffset + 4U),
                                             g_field) ||
            !bmp_detail::parse_bitfield_mask(bmp_detail::read_u32_le(bytes, kMasksOffset + 8U),
                                             b_field)) {
            return bmp_detail::fail(error, "invalid BMP bitfield mask");
        }
    }

    std::array<std::uint32_t, 256> palette{};
    std::uint32_t palette_count = 0;
    std::size_t min_pixel_offset = after_header_masks;
    if (bit_count == 4U || bit_count == 8U) {
        const std::uint32_t max_colors = 1U << bit_count;
        palette_count = (clr_used == 0U) ? max_colors : clr_used;
        if (palette_count > max_colors) {
            return bmp_detail::fail(error, "BMP palette size exceeds bit depth");
        }
        const std::size_t palette_bytes = static_cast<std::size_t>(palette_count) * 4U;
        if (palette_bytes > bytes.size() - after_header_masks) {
            return bmp_detail::fail(error, "truncated BMP palette");
        }
        for (std::uint32_t i = 0; i < palette_count; ++i) {
            const std::size_t p = after_header_masks + static_cast<std::size_t>(i) * 4U;
            const std::uint32_t b = bytes[p];
            const std::uint32_t g = bytes[p + 1U];
            const std::uint32_t r = bytes[p + 2U];
            palette[i] = 0xFF000000U | (r << 16U) | (g << 8U) | b;
        }
        min_pixel_offset = after_header_masks + palette_bytes;
    }

    if (static_cast<std::size_t>(pixel_offset) < min_pixel_offset ||
        static_cast<std::size_t>(pixel_offset) > bytes.size()) {
        return bmp_detail::fail(error, "invalid BMP pixel data offset");
    }

    const std::uint64_t row_bits = static_cast<std::uint64_t>(width) * bit_count;
    const std::uint64_t row_stride = ((row_bits + 31ULL) / 32ULL) * 4ULL;
    const std::uint64_t pixel_bytes = row_stride * static_cast<std::uint64_t>(height);
    const std::uint64_t available =
        static_cast<std::uint64_t>(bytes.size()) - static_cast<std::uint64_t>(pixel_offset);
    if (pixel_bytes > available) {
        return bmp_detail::fail(error, "truncated BMP pixel data");
    }

    DecodedBmp decoded{};
    try {
        decoded.width = width;
        decoded.height = height;
        decoded.argb.resize(static_cast<std::size_t>(width) * height);
    } catch (...) {
        return bmp_detail::fail(error, "failed to allocate decoded BMP buffer");
    }

    const std::size_t base = static_cast<std::size_t>(pixel_offset);
    const std::size_t stride = static_cast<std::size_t>(row_stride);

    for (std::uint32_t sy = 0; sy < height; ++sy) {
        const std::size_t row_off = base + static_cast<std::size_t>(sy) * stride;
        const std::uint32_t dy = top_down ? sy : (height - 1U - sy);
        std::uint32_t* dst = decoded.argb.data() + static_cast<std::size_t>(dy) * width;

        if (bit_count == 24U) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t p = row_off + static_cast<std::size_t>(x) * 3U;
                const std::uint32_t b = bytes[p];
                const std::uint32_t g = bytes[p + 1U];
                const std::uint32_t r = bytes[p + 2U];
                dst[x] = 0xFF000000U | (r << 16U) | (g << 8U) | b;
            }
        } else if (bit_count == 32U && compression == kBiRgb) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t p = row_off + static_cast<std::size_t>(x) * 4U;
                const std::uint32_t b = bytes[p];
                const std::uint32_t g = bytes[p + 1U];
                const std::uint32_t r = bytes[p + 2U];
                dst[x] = 0xFF000000U | (r << 16U) | (g << 8U) | b;
            }
        } else if (bit_count == 32U && compression == kBiBitfields) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t p = row_off + static_cast<std::size_t>(x) * 4U;
                const std::uint32_t px = bmp_detail::read_u32_le(bytes, p);
                const std::uint32_t r = bmp_detail::extract_bitfield(px, r_field);
                const std::uint32_t g = bmp_detail::extract_bitfield(px, g_field);
                const std::uint32_t b = bmp_detail::extract_bitfield(px, b_field);
                dst[x] = 0xFF000000U | (r << 16U) | (g << 8U) | b;
            }
        } else if (bit_count == 8U) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t idx = bytes[row_off + x];
                if (idx >= palette_count) {
                    return bmp_detail::fail(error, "BMP palette index out of range");
                }
                dst[x] = palette[idx];
            }
        } else {  // bit_count == 4U
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t packed = bytes[row_off + (x >> 1U)];
                const std::uint8_t idx =
                    ((x & 1U) == 0U) ? static_cast<std::uint8_t>((packed >> 4U) & 0x0FU)
                                     : static_cast<std::uint8_t>(packed & 0x0FU);
                if (idx >= palette_count) {
                    return bmp_detail::fail(error, "BMP palette index out of range");
                }
                dst[x] = palette[idx];
            }
        }
    }

    out = std::move(decoded);
    return true;
}

// Area-average (box filter) downscaler so the longer side is <= `max_side` while keeping aspect
// ratio (each side >= 1). `max_side == 0` or images already small enough return a copy.
[[nodiscard]] inline DecodedBmp downscale_to_fit(const DecodedBmp& in, std::uint32_t max_side) {
    if (in.width == 0U || in.height == 0U) {
        return DecodedBmp{};
    }
    const std::uint64_t expected_pixels = static_cast<std::uint64_t>(in.width) * in.height;
    if (expected_pixels > in.argb.size()) {
        return DecodedBmp{};
    }

    try {
        if (max_side == 0U || (in.width <= max_side && in.height <= max_side)) {
            DecodedBmp copy{};
            copy.width = in.width;
            copy.height = in.height;
            copy.argb.assign(in.argb.begin(),
                             in.argb.begin() + static_cast<std::ptrdiff_t>(expected_pixels));
            return copy;
        }

        std::uint32_t dst_w = 1U;
        std::uint32_t dst_h = 1U;
        if (in.width >= in.height) {
            dst_w = max_side;
            const std::uint64_t scaled =
                (static_cast<std::uint64_t>(in.height) * max_side + in.width / 2ULL) / in.width;
            dst_h = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(scaled, 1ULL, max_side));
        } else {
            dst_h = max_side;
            const std::uint64_t scaled =
                (static_cast<std::uint64_t>(in.width) * max_side + in.height / 2ULL) / in.height;
            dst_w = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(scaled, 1ULL, max_side));
        }

        DecodedBmp out{};
        out.width = dst_w;
        out.height = dst_h;
        out.argb.resize(static_cast<std::size_t>(dst_w) * dst_h);

        for (std::uint32_t dy = 0; dy < dst_h; ++dy) {
            const std::uint64_t box_y0 = static_cast<std::uint64_t>(dy) * in.height;
            const std::uint64_t box_y1 = static_cast<std::uint64_t>(dy + 1U) * in.height;
            const std::uint32_t sy_begin = static_cast<std::uint32_t>(box_y0 / dst_h);
            const std::uint32_t sy_end = static_cast<std::uint32_t>(
                std::min<std::uint64_t>((box_y1 + dst_h - 1ULL) / dst_h, in.height));

            for (std::uint32_t dx = 0; dx < dst_w; ++dx) {
                const std::uint64_t box_x0 = static_cast<std::uint64_t>(dx) * in.width;
                const std::uint64_t box_x1 = static_cast<std::uint64_t>(dx + 1U) * in.width;
                const std::uint32_t sx_begin = static_cast<std::uint32_t>(box_x0 / dst_w);
                const std::uint32_t sx_end = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>((box_x1 + dst_w - 1ULL) / dst_w, in.width));

                std::uint64_t sum_r = 0;
                std::uint64_t sum_g = 0;
                std::uint64_t sum_b = 0;
                std::uint64_t total_w = 0;

                for (std::uint32_t sy = sy_begin; sy < sy_end; ++sy) {
                    const std::uint64_t y_lo =
                        std::max(box_y0, static_cast<std::uint64_t>(sy) * dst_h);
                    const std::uint64_t y_hi =
                        std::min(box_y1, static_cast<std::uint64_t>(sy + 1U) * dst_h);
                    if (y_hi <= y_lo) {
                        continue;
                    }
                    const std::uint64_t wy = y_hi - y_lo;
                    const std::uint32_t* row =
                        in.argb.data() + static_cast<std::size_t>(sy) * in.width;

                    std::uint64_t row_r = 0;
                    std::uint64_t row_g = 0;
                    std::uint64_t row_b = 0;
                    std::uint64_t row_w = 0;
                    for (std::uint32_t sx = sx_begin; sx < sx_end; ++sx) {
                        const std::uint64_t x_lo =
                            std::max(box_x0, static_cast<std::uint64_t>(sx) * dst_w);
                        const std::uint64_t x_hi =
                            std::min(box_x1, static_cast<std::uint64_t>(sx + 1U) * dst_w);
                        if (x_hi <= x_lo) {
                            continue;
                        }
                        const std::uint64_t wx = x_hi - x_lo;
                        const std::uint32_t px = row[sx];
                        row_r += wx * ((px >> 16U) & 0xFFU);
                        row_g += wx * ((px >> 8U) & 0xFFU);
                        row_b += wx * (px & 0xFFU);
                        row_w += wx;
                    }
                    sum_r += row_r * wy;
                    sum_g += row_g * wy;
                    sum_b += row_b * wy;
                    total_w += row_w * wy;
                }

                std::uint32_t r = 0;
                std::uint32_t g = 0;
                std::uint32_t b = 0;
                if (total_w > 0ULL) {
                    r = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>((sum_r + total_w / 2ULL) / total_w, 255ULL));
                    g = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>((sum_g + total_w / 2ULL) / total_w, 255ULL));
                    b = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>((sum_b + total_w / 2ULL) / total_w, 255ULL));
                }
                out.argb[static_cast<std::size_t>(dy) * dst_w + dx] =
                    0xFF000000U | (r << 16U) | (g << 8U) | b;
            }
        }
        return out;
    } catch (...) {
        return DecodedBmp{};
    }
}

// Extracts an opaque Material You seed colour by binning pixels into 36 10-degree hue bins
// weighted by saturation * value, ignoring desaturated/dark pixels (S < 0.2 or V < 0.15).
// Returns the weighted mean colour of the heaviest bin, or 0xFF5B6B8C if none qualify.
[[nodiscard]] inline std::uint32_t seed_color_argb(const DecodedBmp& img) noexcept {
    if (img.width == 0U || img.height == 0U || img.argb.empty()) {
        return kDefaultSeedColorArgb;
    }
    const std::uint64_t max_pixels = static_cast<std::uint64_t>(img.width) * img.height;
    const std::size_t count =
        static_cast<std::size_t>(std::min<std::uint64_t>(max_pixels, img.argb.size()));

    std::array<double, 36> bin_weight{};
    std::array<double, 36> bin_r{};
    std::array<double, 36> bin_g{};
    std::array<double, 36> bin_b{};

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = img.argb[i];
        const std::uint32_t r8 = (px >> 16U) & 0xFFU;
        const std::uint32_t g8 = (px >> 8U) & 0xFFU;
        const std::uint32_t b8 = px & 0xFFU;
        const double r = static_cast<double>(r8) / 255.0;
        const double g = static_cast<double>(g8) / 255.0;
        const double b = static_cast<double>(b8) / 255.0;

        const double max_c = std::max({r, g, b});
        const double min_c = std::min({r, g, b});
        const double delta = max_c - min_c;
        const double v = max_c;
        const double s = (max_c > 0.0) ? (delta / max_c) : 0.0;
        if (s < 0.2 || v < 0.15 || delta <= 0.0) {
            continue;
        }

        double hue_deg = 0.0;
        if (max_c == r) {
            hue_deg = (g - b) / delta;
            if (hue_deg < 0.0) {
                hue_deg += 6.0;
            }
        } else if (max_c == g) {
            hue_deg = (b - r) / delta + 2.0;
        } else {
            hue_deg = (r - g) / delta + 4.0;
        }
        hue_deg *= 60.0;

        int bin = static_cast<int>(hue_deg / 10.0);
        if (bin < 0) {
            bin = 0;
        }
        bin %= 36;

        const double weight = s * v;
        const std::size_t idx = static_cast<std::size_t>(bin);
        bin_weight[idx] += weight;
        bin_r[idx] += weight * static_cast<double>(r8);
        bin_g[idx] += weight * static_cast<double>(g8);
        bin_b[idx] += weight * static_cast<double>(b8);
    }

    int best_bin = -1;
    double best_weight = 0.0;
    for (int b = 0; b < 36; ++b) {
        const double w = bin_weight[static_cast<std::size_t>(b)];
        if (w > best_weight) {
            best_weight = w;
            best_bin = b;
        }
    }
    if (best_bin < 0 || best_weight <= 0.0) {
        return kDefaultSeedColorArgb;
    }

    const std::size_t idx = static_cast<std::size_t>(best_bin);
    const auto round_channel = [best_weight](double sum) noexcept -> std::uint32_t {
        const long v = std::lround(sum / best_weight);
        return static_cast<std::uint32_t>(std::clamp(v, 0L, 255L));
    };
    const std::uint32_t out_r = round_channel(bin_r[idx]);
    const std::uint32_t out_g = round_channel(bin_g[idx]);
    const std::uint32_t out_b = round_channel(bin_b[idx]);
    return 0xFF000000U | (out_r << 16U) | (out_g << 8U) | out_b;
}

}  // namespace wse::asset
