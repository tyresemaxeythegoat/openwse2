// Isolates unmodified public-domain / MIT-0 audio decoders (`stb_vorbis.c`
// and `dr_mp3.h`) inside a dedicated translation unit compiled without third-party
// warning failure flags (mirroring `lua51` in `CMakeLists.txt`).

#include "wse_audio_internal.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

namespace wse::asset::audio_vendor {

namespace {

[[nodiscard]] bool validate_stream_header(std::uint32_t sample_rate,
                                          std::uint16_t raw_channels,
                                          std::string* error_out) {
    if (sample_rate == 0U || sample_rate > kMaxAudioSampleRate) {
        if (error_out != nullptr) {
            *error_out = "Unsupported compressed audio sample rate";
        }
        return false;
    }
    if (raw_channels == 0U || raw_channels > 8U) {
        if (error_out != nullptr) {
            *error_out = "Unsupported compressed audio channel count";
        }
        return false;
    }
    return true;
}

class VorbisStreamDecoder final : public AudioStreamDecoder {
public:
    explicit VorbisStreamDecoder(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    VorbisStreamDecoder(const VorbisStreamDecoder&) = delete;
    VorbisStreamDecoder& operator=(const VorbisStreamDecoder&) = delete;

    ~VorbisStreamDecoder() override {
        if (handle_ != nullptr) {
            stb_vorbis_close(handle_);
            handle_ = nullptr;
        }
    }

    [[nodiscard]] bool init(std::string* error_out) {
        if (bytes_.empty() ||
            bytes_.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            if (error_out != nullptr) {
                *error_out = "Empty or oversized Ogg Vorbis stream";
            }
            return false;
        }
        int err = 0;
        handle_ = stb_vorbis_open_memory(bytes_.data(), static_cast<int>(bytes_.size()), &err,
                                         nullptr);
        if (handle_ == nullptr) {
            if (error_out != nullptr) {
                *error_out = "Failed to open Ogg Vorbis stream";
            }
            return false;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(handle_);
        sample_rate_ = static_cast<std::uint32_t>(info.sample_rate);
        raw_channels_ = static_cast<std::uint16_t>(info.channels);
        if (!validate_stream_header(sample_rate_, raw_channels_, error_out)) {
            stb_vorbis_close(handle_);
            handle_ = nullptr;
            return false;
        }
        out_channels_ = (raw_channels_ <= 2U) ? raw_channels_ : 2U;
        total_frames_ = static_cast<std::uint64_t>(stb_vorbis_stream_length_in_samples(handle_));
        return true;
    }

    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return sample_rate_; }
    [[nodiscard]] std::uint16_t channels() const noexcept override { return out_channels_; }
    [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_frames_; }
    [[nodiscard]] std::uint64_t cursor_frame() const noexcept override { return cursor_; }

    [[nodiscard]] bool seek_start() override {
        if (handle_ == nullptr || stb_vorbis_seek_start(handle_) == 0) {
            return false;
        }
        cursor_ = 0U;
        return true;
    }

    [[nodiscard]] std::size_t read_frames(std::size_t max_frames,
                                          std::vector<float>& out_interleaved) override {
        out_interleaved.clear();
        if (handle_ == nullptr || max_frames == 0U || raw_channels_ == 0U) {
            return 0U;
        }
        // Decode straight into the caller's buffer (capacity is reused across calls),
        // then fold surround layouts to stereo in place.
        const std::size_t capped = std::min<std::size_t>(max_frames, 65536U);
        const std::size_t ch = static_cast<std::size_t>(raw_channels_);
        out_interleaved.resize(capped * ch);
        const int decoded = stb_vorbis_get_samples_float_interleaved(
            handle_, static_cast<int>(raw_channels_), out_interleaved.data(),
            static_cast<int>(out_interleaved.size()));
        if (decoded <= 0) {
            out_interleaved.clear();
            return 0U;
        }
        const std::size_t n = static_cast<std::size_t>(decoded);
        out_interleaved.resize(n * ch);
        std::uint16_t out_ch = raw_channels_;
        audio_detail::fold_to_stereo_in_place(out_interleaved, out_ch,
                                              audio_detail::ChannelOrder::vorbis, 0U);
        cursor_ += static_cast<std::uint64_t>(n);
        return n;
    }

private:
    std::vector<std::uint8_t> bytes_;
    stb_vorbis* handle_ = nullptr;
    std::uint32_t sample_rate_ = 0;
    std::uint16_t raw_channels_ = 0;
    std::uint16_t out_channels_ = 0;
    std::uint64_t total_frames_ = 0;
    std::uint64_t cursor_ = 0;
};

class Mp3StreamDecoder final : public AudioStreamDecoder {
public:
    explicit Mp3StreamDecoder(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    Mp3StreamDecoder(const Mp3StreamDecoder&) = delete;
    Mp3StreamDecoder& operator=(const Mp3StreamDecoder&) = delete;

    ~Mp3StreamDecoder() override {
        if (opened_) {
            drmp3_uninit(&mp3_);
            opened_ = false;
        }
    }

    [[nodiscard]] bool init(std::string* error_out) {
        if (bytes_.empty()) {
            if (error_out != nullptr) {
                *error_out = "Empty MP3 stream";
            }
            return false;
        }
        if (drmp3_init_memory(&mp3_, bytes_.data(), bytes_.size(), nullptr) == 0) {
            if (error_out != nullptr) {
                *error_out = "Failed to open MP3 stream";
            }
            return false;
        }
        opened_ = true;
        raw_channels_ = static_cast<std::uint16_t>(mp3_.channels);
        if (!validate_stream_header(mp3_.sampleRate, raw_channels_, error_out)) {
            drmp3_uninit(&mp3_);
            opened_ = false;
            return false;
        }
        out_channels_ = raw_channels_;  // MPEG audio is mono or stereo
        // `drmp3_get_pcm_frame_count` would walk every frame header of the file and
        // seek back; the mixer never needs the length, so report "unknown" (0).
        return true;
    }

    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return mp3_.sampleRate; }
    [[nodiscard]] std::uint16_t channels() const noexcept override { return out_channels_; }
    [[nodiscard]] std::uint64_t total_frames() const noexcept override { return 0U; }
    [[nodiscard]] std::uint64_t cursor_frame() const noexcept override { return cursor_; }

    [[nodiscard]] bool seek_start() override {
        if (!opened_ || drmp3_seek_to_pcm_frame(&mp3_, 0U) == 0) {
            return false;
        }
        cursor_ = 0U;
        return true;
    }

    [[nodiscard]] std::size_t read_frames(std::size_t max_frames,
                                          std::vector<float>& out_interleaved) override {
        out_interleaved.clear();
        if (!opened_ || max_frames == 0U || raw_channels_ == 0U) {
            return 0U;
        }
        const std::size_t capped = std::min<std::size_t>(max_frames, 65536U);
        const std::size_t ch = static_cast<std::size_t>(raw_channels_);
        out_interleaved.resize(capped * ch);
        const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3_, capped, out_interleaved.data());
        const std::size_t n = static_cast<std::size_t>(got);
        out_interleaved.resize(n * ch);
        cursor_ += static_cast<std::uint64_t>(n);
        return n;
    }

private:
    std::vector<std::uint8_t> bytes_;
    drmp3 mp3_{};
    bool opened_ = false;
    std::uint16_t raw_channels_ = 0;
    std::uint16_t out_channels_ = 0;
    std::uint64_t cursor_ = 0;
};

}  // namespace

bool decode_vorbis_memory(std::span<const std::uint8_t> bytes,
                          DecodedVendorAudio& out,
                          std::string* error_out) {
    out = {};
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        if (error_out != nullptr) {
            *error_out = "Empty or oversized Ogg Vorbis payload";
        }
        return false;
    }
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &err,
                                           nullptr);
    if (v == nullptr) {
        if (error_out != nullptr) {
            *error_out = "Failed to open Ogg Vorbis stream";
        }
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    const std::uint32_t sr = static_cast<std::uint32_t>(info.sample_rate);
    const std::uint16_t ch = static_cast<std::uint16_t>(info.channels);
    if (!validate_stream_header(sr, ch, error_out)) {
        stb_vorbis_close(v);
        return false;
    }

    // Reserve the exact size when the stream length is known (one last-page seek),
    // then decode straight into `raw` instead of bouncing through a stack chunk.
    std::vector<float> raw;
    const std::size_t ch_sz = static_cast<std::size_t>(ch);
    const unsigned int known_frames = stb_vorbis_stream_length_in_samples(v);
    if (known_frames > 0U &&
        static_cast<std::size_t>(known_frames) <= kMaxDecodedAudioSamples / ch_sz) {
        raw.reserve(static_cast<std::size_t>(known_frames) * ch_sz);
    }
    constexpr std::size_t kChunkFrames = 4096U;
    while (true) {
        const std::size_t old = raw.size();
        raw.resize(old + kChunkFrames * ch_sz);
        const int n = stb_vorbis_get_samples_float_interleaved(
            v, info.channels, raw.data() + old, static_cast<int>(kChunkFrames * ch_sz));
        const std::size_t got = (n > 0) ? static_cast<std::size_t>(n) : 0U;
        raw.resize(old + got * ch_sz);
        if (raw.size() > kMaxDecodedAudioSamples) {
            stb_vorbis_close(v);
            if (error_out != nullptr) {
                *error_out = "Ogg Vorbis stream exceeds maximum decoded sample bound";
            }
            return false;
        }
        if (got == 0U) {
            break;
        }
    }
    stb_vorbis_close(v);

    if (raw.empty()) {
        if (error_out != nullptr) {
            *error_out = "Ogg Vorbis stream decoded zero samples";
        }
        return false;
    }
    out.sample_rate = sr;
    out.channels = ch;
    audio_detail::fold_to_stereo_in_place(raw, out.channels, audio_detail::ChannelOrder::vorbis,
                                          0U);
    out.interleaved = std::move(raw);
    return !out.interleaved.empty();
}

bool decode_mp3_memory(std::span<const std::uint8_t> bytes,
                       DecodedVendorAudio& out,
                       std::string* error_out) {
    out = {};
    if (bytes.empty()) {
        if (error_out != nullptr) {
            *error_out = "Empty MP3 payload";
        }
        return false;
    }
    drmp3 mp3{};
    if (drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr) == 0) {
        if (error_out != nullptr) {
            *error_out = "Failed to open MP3 stream";
        }
        return false;
    }
    const std::uint32_t sr = static_cast<std::uint32_t>(mp3.sampleRate);
    const std::uint16_t ch = static_cast<std::uint16_t>(mp3.channels);
    if (!validate_stream_header(sr, ch, error_out)) {
        drmp3_uninit(&mp3);
        return false;
    }

    std::vector<float> raw;
    const std::size_t ch_sz = static_cast<std::size_t>(ch);
    constexpr std::size_t kChunkFrames = 4096U;
    while (true) {
        const std::size_t old = raw.size();
        raw.resize(old + kChunkFrames * ch_sz);
        const drmp3_uint64 got =
            drmp3_read_pcm_frames_f32(&mp3, kChunkFrames, raw.data() + old);
        raw.resize(old + static_cast<std::size_t>(got) * ch_sz);
        if (raw.size() > kMaxDecodedAudioSamples) {
            drmp3_uninit(&mp3);
            if (error_out != nullptr) {
                *error_out = "MP3 stream exceeds maximum decoded sample bound";
            }
            return false;
        }
        if (got == 0U) {
            break;
        }
    }
    drmp3_uninit(&mp3);

    if (raw.empty()) {
        if (error_out != nullptr) {
            *error_out = "MP3 stream decoded zero samples";
        }
        return false;
    }
    out.sample_rate = sr;
    out.channels = ch;  // MPEG audio is mono or stereo
    out.interleaved = std::move(raw);
    return !out.interleaved.empty();
}

std::unique_ptr<AudioStreamDecoder> open_vorbis_stream(
    std::vector<std::uint8_t> owned_bytes, std::string* error_out) {
    auto stream = std::make_unique<VorbisStreamDecoder>(std::move(owned_bytes));
    if (!stream->init(error_out)) {
        return nullptr;
    }
    return stream;
}

std::unique_ptr<AudioStreamDecoder> open_mp3_stream(
    std::vector<std::uint8_t> owned_bytes, std::string* error_out) {
    auto stream = std::make_unique<Mp3StreamDecoder>(std::move(owned_bytes));
    if (!stream->init(error_out)) {
        return nullptr;
    }
    return stream;
}

}  // namespace wse::asset::audio_vendor
