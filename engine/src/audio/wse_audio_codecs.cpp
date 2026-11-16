// Clean-room C++20 codec dispatcher for `AudioCodecBackend` (`stb_vorbis` + `dr_mp3`).

#include "wse/asset/audio_codecs.h"

#include <memory>
#include <utility>

#include "wse_audio_internal.hpp"

namespace wse::asset {

namespace {

class CompressedAudioCodecBackend final : public AudioCodecBackend {
public:
    [[nodiscard]] bool supports(AudioCodecKind kind) const noexcept override {
        return kind == AudioCodecKind::ogg || kind == AudioCodecKind::mp3;
    }

    [[nodiscard]] bool decode_memory(AudioCodecKind kind,
                                     std::span<const std::uint8_t> bytes,
                                     AudioPcmBuffer& out,
                                     std::string* error_out) const override {
        out = {};
        audio_vendor::DecodedVendorAudio decoded{};
        bool ok = false;
        if (kind == AudioCodecKind::ogg) {
            ok = audio_vendor::decode_vorbis_memory(bytes, decoded, error_out);
        } else if (kind == AudioCodecKind::mp3) {
            ok = audio_vendor::decode_mp3_memory(bytes, decoded, error_out);
        } else {
            if (error_out != nullptr) {
                *error_out = "Unsupported compressed audio codec";
            }
            return false;
        }
        if (!ok) {
            return false;
        }
        out.sample_rate = decoded.sample_rate;
        out.channels = decoded.channels;
        out.samples = std::move(decoded.interleaved);
        return out.valid();
    }

    [[nodiscard]] std::unique_ptr<AudioStreamDecoder> open_stream(
        AudioCodecKind kind, std::vector<std::uint8_t> owned_bytes,
        std::string* error_out) const override {
        if (kind == AudioCodecKind::ogg) {
            return audio_vendor::open_vorbis_stream(std::move(owned_bytes), error_out);
        }
        if (kind == AudioCodecKind::mp3) {
            return audio_vendor::open_mp3_stream(std::move(owned_bytes), error_out);
        }
        if (error_out != nullptr) {
            *error_out = "Unsupported compressed audio codec";
        }
        return nullptr;
    }
};

}  // namespace

std::shared_ptr<AudioCodecBackend> create_audio_codec_backend() {
    return std::make_shared<CompressedAudioCodecBackend>();
}

}  // namespace wse::asset
