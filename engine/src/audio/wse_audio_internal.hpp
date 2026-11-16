#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "wse/asset/audio.h"

namespace wse::asset::audio_vendor {

struct DecodedVendorAudio {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::vector<float> interleaved{};
};

[[nodiscard]] bool decode_vorbis_memory(std::span<const std::uint8_t> bytes,
                                        DecodedVendorAudio& out,
                                        std::string* error_out);

[[nodiscard]] bool decode_mp3_memory(std::span<const std::uint8_t> bytes,
                                     DecodedVendorAudio& out,
                                     std::string* error_out);

[[nodiscard]] std::unique_ptr<AudioStreamDecoder> open_vorbis_stream(
    std::vector<std::uint8_t> owned_bytes, std::string* error_out);

[[nodiscard]] std::unique_ptr<AudioStreamDecoder> open_mp3_stream(
    std::vector<std::uint8_t> owned_bytes, std::string* error_out);

}  // namespace wse::asset::audio_vendor
