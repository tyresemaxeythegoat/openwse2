// Clean-room v1.1.5.5 audio asset decoders, retail path resolution, and
// sound/music flag helpers (Slice P6, docs/PLATFORM_PLAN.md).
//
// Grounded in WSE2 v1.1.5.5 PDB/binary evidence:
//   - mbSoundSample::readStream            0x140332580 (%sSounds\languages\%s\%s -> %sSounds\%s -> Sounds\languages\%s\%s -> Sounds\%s)
//   - mbSoundSample::playSound             0x140332a70 (priority & flags 0x01=2D, 0x02=looping)
//   - mbSoundManager::readStream           0x1403330a0
//   - mbSoundManager::playSoundAtPosition  0x140333880 (0x1000000 LOD fallback, culture variant filter)
//   - mbSoundManager::setChannelPropertiesAndPlay 0x140333b40 (volume / 15.0f, bit 2 random start, bit 3 disable freq var)
//   - mbMusicTrack::readStream             0x140290870 (%sMusic/%s -> Music/%s; continue_flags |= flags, culture fallback 0x3f)
//   - mbAgent::playSound                   0x140057170 (human voice pitch hash)
//
// Note: third-party compressed codec headers (stb_vorbis / dr_mp3) never appear
// here; RIFF/WAVE PCM is self-contained and OGG/MP3 decode delegates through
// `AudioCodecBackend` (implemented in `wse/asset/audio_codecs.h` + `wse_audio`).
//
// Documented safety deviations (engine/README.md, "P6 audio"):
//   - A `data` chunk whose declared size runs past the end of the file (streamed
//     WAV writers, `0xFFFFFFFF`) is clamped to the bytes present instead of rejected.
//   - Memory decodes are capped at `kMaxDecodedAudioSamples` (10 minutes of 48 kHz
//     stereo); long music goes through `open_audio_stream` and is never fully decoded.
//   - Sources with more than two channels are folded to stereo with per-speaker
//     coefficients (centre/surround at -3 dB, LFE dropped), normalised so the fold
//     cannot clip; FMOD performed its own downmix that is not reconstructed.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/platform/vfs.h"

namespace wse::asset {

inline constexpr std::uint32_t kMaxAudioSampleRate = 384000U;
// Interleaved float samples one memory decode may produce: 10 minutes of 48 kHz
// stereo (57.6M floats, ~230 MB). Sound effects are seconds long; music streams.
inline constexpr std::size_t kMaxDecodedAudioSamples = 48000ULL * 2ULL * 60ULL * 10ULL;
inline constexpr std::uint16_t kMaxAudioSourceChannels = 8U;

// mbSound / mbSoundSample flags (`mbSoundSample::playSound` `0x140332a70`,
// `mbSoundManager::playSoundAtPosition` `0x140333880`,
// `mbSoundManager::setChannelPropertiesAndPlay` `0x140333b40`):
inline constexpr std::uint32_t kSoundFlag2D = 0x00000001U;
inline constexpr std::uint32_t kSoundFlagLooping = 0x00000002U;
inline constexpr std::uint32_t kSoundFlagRandomStartPos = 0x00000004U;
inline constexpr std::uint32_t kSoundFlagDisableFreqVariation = 0x00000008U;
inline constexpr std::uint32_t kSoundFlagLodFallback = 0x01000000U;

// mbMusicTrack flags (`mbMusicManager::startPlaying` `0x140290fc0`,
// `mbMusicManager::update` `0x1402910c0`):
inline constexpr std::uint32_t kMusicFlagCultureMask = 0x0000003FU;
inline constexpr std::uint32_t kMusicFlagLooping = 0x00000040U;
inline constexpr std::uint32_t kMusicFlagImmediate = 0x00000080U;
inline constexpr std::uint32_t kMusicFlagPersist = 0x00000100U;

enum class AudioCodecKind : std::uint8_t { unknown = 0, wav = 1, ogg = 2, mp3 = 3 };

struct AudioPcmBuffer {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    // Interleaved float32 PCM normalized to [-1, 1].
    std::vector<float> samples{};

    [[nodiscard]] std::size_t frame_count() const noexcept {
        return (channels > 0U) ? (samples.size() / static_cast<std::size_t>(channels)) : 0U;
    }

    [[nodiscard]] double duration_seconds() const noexcept {
        if (sample_rate == 0U || channels == 0U) {
            return 0.0;
        }
        return static_cast<double>(frame_count()) / static_cast<double>(sample_rate);
    }

    [[nodiscard]] bool valid() const noexcept {
        return sample_rate > 0U && sample_rate <= kMaxAudioSampleRate &&
               (channels == 1U || channels == 2U) && !samples.empty() &&
               (samples.size() % static_cast<std::size_t>(channels) == 0U);
    }
};

// Pull-based streaming decoder for long music tracks.
class AudioStreamDecoder {
public:
    virtual ~AudioStreamDecoder() = default;

    [[nodiscard]] virtual std::uint32_t sample_rate() const noexcept = 0;
    // Output channel count after the stereo fold: always 1 or 2.
    [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
    // Total frames when known without a decode pass; 0 means "unknown".
    [[nodiscard]] virtual std::uint64_t total_frames() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t cursor_frame() const noexcept = 0;
    [[nodiscard]] virtual bool seek_start() = 0;

    // Replaces `out_interleaved` with up to `max_frames` interleaved float frames.
    // Returns frames decoded; 0 indicates EOF or error. Implementations reuse the
    // vector's capacity, so a caller that keeps one buffer allocates only once.
    [[nodiscard]] virtual std::size_t read_frames(std::size_t max_frames,
                                                  std::vector<float>& out_interleaved) = 0;
};

// Abstract backend for compressed codecs (`.ogg` Vorbis, `.mp3` MPEG Audio).
class AudioCodecBackend {
public:
    virtual ~AudioCodecBackend() = default;

    [[nodiscard]] virtual bool supports(AudioCodecKind kind) const noexcept = 0;

    [[nodiscard]] virtual bool decode_memory(AudioCodecKind kind,
                                             std::span<const std::uint8_t> bytes,
                                             AudioPcmBuffer& out,
                                             std::string* error_out = nullptr) const = 0;

    [[nodiscard]] virtual std::unique_ptr<AudioStreamDecoder> open_stream(
        AudioCodecKind kind, std::vector<std::uint8_t> owned_bytes,
        std::string* error_out = nullptr) const = 0;
};

namespace audio_detail {

[[nodiscard]] inline std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U);
}

[[nodiscard]] inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

[[nodiscard]] inline bool matches_ascii4(const std::uint8_t* p, const char* tag) noexcept {
    return p[0] == static_cast<std::uint8_t>(tag[0]) &&
           p[1] == static_cast<std::uint8_t>(tag[1]) &&
           p[2] == static_cast<std::uint8_t>(tag[2]) &&
           p[3] == static_cast<std::uint8_t>(tag[3]);
}

[[nodiscard]] inline bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = s.substr(s.size() - suffix.size());
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const char a = wse::platform::ascii_lower(tail[i]);
        const char b = wse::platform::ascii_lower(suffix[i]);
        if (a != b) {
            return false;
        }
    }
    return true;
}

// Speaker positions, using the `WAVEFORMATEXTENSIBLE::dwChannelMask` bit values.
inline constexpr std::uint32_t kSpkFrontLeft = 0x1U;
inline constexpr std::uint32_t kSpkFrontRight = 0x2U;
inline constexpr std::uint32_t kSpkFrontCenter = 0x4U;
inline constexpr std::uint32_t kSpkLowFrequency = 0x8U;
inline constexpr std::uint32_t kSpkBackLeft = 0x10U;
inline constexpr std::uint32_t kSpkBackRight = 0x20U;
inline constexpr std::uint32_t kSpkFrontLeftOfCenter = 0x40U;
inline constexpr std::uint32_t kSpkFrontRightOfCenter = 0x80U;
inline constexpr std::uint32_t kSpkBackCenter = 0x100U;
inline constexpr std::uint32_t kSpkSideLeft = 0x200U;
inline constexpr std::uint32_t kSpkSideRight = 0x400U;

// Interleaving order of a multichannel source.
enum class ChannelOrder : std::uint8_t {
    wav,     // RIFF/WAVE: ascending `dwChannelMask` bits (default masks when absent)
    vorbis,  // Vorbis I spec section 4.3.9
};

[[nodiscard]] inline std::uint32_t default_wav_channel_mask(std::uint16_t channels) noexcept {
    switch (channels) {
        case 1: return kSpkFrontCenter;
        case 2: return kSpkFrontLeft | kSpkFrontRight;
        case 3: return 0x007U;  // FL FR FC
        case 4: return 0x033U;  // FL FR BL BR
        case 5: return 0x037U;  // FL FR FC BL BR
        case 6: return 0x03FU;  // FL FR FC LFE BL BR
        case 7: return 0x70FU;  // FL FR FC LFE BC SL SR
        case 8: return 0x63FU;  // FL FR FC LFE BL BR SL SR
        default: return 0U;
    }
}

// Speaker position of channel `ch` of a `channels`-wide source (0 when unknown).
[[nodiscard]] inline std::uint32_t speaker_of_channel(ChannelOrder order, std::uint32_t wav_mask,
                                                      std::uint16_t channels,
                                                      std::uint16_t ch) noexcept {
    if (order == ChannelOrder::vorbis) {
        static constexpr std::uint32_t L = kSpkFrontLeft, R = kSpkFrontRight,
                                       C = kSpkFrontCenter, E = kSpkLowFrequency,
                                       BL = kSpkBackLeft, BR = kSpkBackRight,
                                       BC = kSpkBackCenter, SL = kSpkSideLeft,
                                       SR = kSpkSideRight;
        static constexpr std::array<std::array<std::uint32_t, 8>, 9> kVorbis = {{
            {},
            {C},
            {L, R},
            {L, C, R},
            {L, R, BL, BR},
            {L, C, R, BL, BR},
            {L, C, R, BL, BR, E},
            {L, C, R, SL, SR, BC, E},
            {L, C, R, SL, SR, BL, BR, E},
        }};
        if (channels < kVorbis.size() && ch < channels) {
            return kVorbis[channels][ch];
        }
        return 0U;
    }
    std::uint32_t mask = wav_mask;
    if (static_cast<std::uint16_t>(std::popcount(mask)) < channels) {
        mask = default_wav_channel_mask(channels);
    }
    std::uint16_t seen = 0U;
    for (std::uint32_t bit = 1U; bit != 0U; bit <<= 1U) {
        if ((mask & bit) != 0U) {
            if (seen == ch) {
                return bit;
            }
            ++seen;
        }
    }
    return 0U;
}

// Stereo fold coefficients for one speaker: front 1.0, centre / side / back -3 dB,
// LFE dropped, unknown or overhead speakers split evenly at -6 dB.
inline void speaker_stereo_gains(std::uint32_t speaker, float& gl, float& gr) noexcept {
    constexpr float kMinus3Db = 0.70710678f;
    gl = 0.0f;
    gr = 0.0f;
    switch (speaker) {
        case kSpkFrontLeft:
        case kSpkFrontLeftOfCenter: gl = 1.0f; break;
        case kSpkFrontRight:
        case kSpkFrontRightOfCenter: gr = 1.0f; break;
        case kSpkFrontCenter: gl = gr = kMinus3Db; break;
        case kSpkBackLeft:
        case kSpkSideLeft: gl = kMinus3Db; break;
        case kSpkBackRight:
        case kSpkSideRight: gr = kMinus3Db; break;
        case kSpkBackCenter: gl = gr = 0.5f; break;
        case kSpkLowFrequency: break;
        default: gl = gr = 0.5f; break;
    }
}

// Folds an interleaved `channels`-wide buffer to stereo in place (no allocation:
// output frame f is written only after input frame f is read, and 2f+1 < 3f for
// f >= 1). Mono and stereo pass through untouched.
inline void fold_to_stereo_in_place(std::vector<float>& samples, std::uint16_t& channels,
                                    ChannelOrder order, std::uint32_t wav_mask) noexcept {
    if (channels <= 2U || channels > kMaxAudioSourceChannels) {
        return;
    }
    std::array<float, kMaxAudioSourceChannels> cl{};
    std::array<float, kMaxAudioSourceChannels> cr{};
    float sum_l = 0.0f;
    float sum_r = 0.0f;
    for (std::uint16_t ch = 0; ch < channels; ++ch) {
        speaker_stereo_gains(speaker_of_channel(order, wav_mask, channels, ch), cl[ch], cr[ch]);
        sum_l += cl[ch];
        sum_r += cr[ch];
    }
    const float norm = 1.0f / std::max(1.0f, std::max(sum_l, sum_r));
    const std::size_t stride = channels;
    const std::size_t frames = samples.size() / stride;
    for (std::size_t f = 0; f < frames; ++f) {
        const float* in = samples.data() + f * stride;
        float l = 0.0f;
        float r = 0.0f;
        for (std::size_t ch = 0; ch < stride; ++ch) {
            l += in[ch] * cl[ch];
            r += in[ch] * cr[ch];
        }
        samples[f * 2U + 0U] = l * norm;
        samples[f * 2U + 1U] = r * norm;
    }
    samples.resize(frames * 2U);
    channels = 2U;
}

inline void append_path_segment(std::string& dst, std::string_view seg) {
    if (seg.empty()) {
        return;
    }
    if (!dst.empty() && dst.back() != '/') {
        dst.push_back('/');
    }
    for (char ch : seg) {
        dst.push_back(ch == '\\' ? '/' : ch);
    }
    while (dst.size() > 1U && dst.back() == '/') {
        dst.pop_back();
    }
}

}  // namespace audio_detail

// Detects audio codec from magic bytes first, falling back to file extension.
[[nodiscard]] inline AudioCodecKind detect_audio_codec(std::span<const std::uint8_t> bytes,
                                                       std::string_view filename_hint = {}) noexcept {
    if (bytes.size() >= 12U && audio_detail::matches_ascii4(bytes.data(), "RIFF") &&
        audio_detail::matches_ascii4(bytes.data() + 8U, "WAVE")) {
        return AudioCodecKind::wav;
    }
    if (bytes.size() >= 4U && audio_detail::matches_ascii4(bytes.data(), "OggS")) {
        return AudioCodecKind::ogg;
    }
    if (bytes.size() >= 3U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        return AudioCodecKind::mp3;
    }
    // MPEG audio frame sync: 11 set bits, then a version other than the reserved
    // `01` and a layer other than the reserved `00` (which is what AAC ADTS uses).
    if (bytes.size() >= 2U && bytes[0] == 0xFFU && (bytes[1] & 0xE0U) == 0xE0U &&
        ((bytes[1] >> 3U) & 0x3U) != 0x1U && ((bytes[1] >> 1U) & 0x3U) != 0x0U) {
        return AudioCodecKind::mp3;
    }
    if (audio_detail::ends_with_ci(filename_hint, ".wav")) {
        return AudioCodecKind::wav;
    }
    if (audio_detail::ends_with_ci(filename_hint, ".ogg")) {
        return AudioCodecKind::ogg;
    }
    if (audio_detail::ends_with_ci(filename_hint, ".mp3")) {
        return AudioCodecKind::mp3;
    }
    return AudioCodecKind::unknown;
}

// Parsed RIFF/WAVE layout: sample format plus where the PCM payload lives.
struct WavLayout {
    std::uint16_t format_tag = 0;  // 1 = integer PCM, 3 = IEEE float
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channel_mask = 0;  // `WAVE_FORMAT_EXTENSIBLE` only, else 0
    std::size_t data_offset = 0;
    std::size_t data_size = 0;

    [[nodiscard]] std::size_t frame_stride() const noexcept {
        return static_cast<std::size_t>(bits_per_sample / 8U) * channels;
    }
    [[nodiscard]] std::size_t frame_count() const noexcept {
        const std::size_t stride = frame_stride();
        return stride == 0U ? 0U : data_size / stride;
    }
};

// Parses and validates a RIFF/WAVE header (8/16/24/32-bit integer, 32-bit float,
// `WAVE_FORMAT_EXTENSIBLE` with a PCM or float sub-format).
[[nodiscard]] inline bool parse_wav_layout(std::span<const std::uint8_t> bytes, WavLayout& out,
                                           std::string* error_out = nullptr) {
    out = {};
    const auto fail = [&](std::string_view msg) {
        if (error_out != nullptr) {
            *error_out = std::string(msg);
        }
        out = {};
        return false;
    };

    if (bytes.size() < 12U) {
        return fail("WAV file too small");
    }
    if (!audio_detail::matches_ascii4(bytes.data(), "RIFF") ||
        !audio_detail::matches_ascii4(bytes.data() + 8U, "WAVE")) {
        return fail("Invalid RIFF/WAVE header");
    }

    bool have_fmt = false;
    bool have_data = false;
    std::size_t pos = 12U;
    while (pos + 8U <= bytes.size()) {
        const std::uint8_t* hdr = bytes.data() + pos;
        const std::uint32_t chunk_size = audio_detail::read_u32_le(hdr + 4U);
        pos += 8U;
        const std::size_t remaining = bytes.size() - pos;
        const bool is_data = audio_detail::matches_ascii4(hdr, "data");
        std::size_t payload_size = static_cast<std::size_t>(chunk_size);
        if (payload_size > remaining) {
            if (!is_data) {
                return fail("Truncated WAV chunk");
            }
            // Safety deviation: streamed writers leave the data size unpatched
            // (`0xFFFFFFFF`) or the file is cut short; play what is present.
            payload_size = remaining;
        }
        const std::span<const std::uint8_t> payload = bytes.subspan(pos, payload_size);
        if (audio_detail::matches_ascii4(hdr, "fmt ")) {
            if (payload.size() < 16U) {
                return fail("Truncated WAV fmt chunk");
            }
            out.format_tag = audio_detail::read_u16_le(payload.data());
            out.channels = audio_detail::read_u16_le(payload.data() + 2U);
            out.sample_rate = audio_detail::read_u32_le(payload.data() + 4U);
            out.bits_per_sample = audio_detail::read_u16_le(payload.data() + 14U);
            if (out.format_tag == 0xFFFEU) {
                if (payload.size() < 40U) {
                    return fail("Truncated WAVE_FORMAT_EXTENSIBLE fmt chunk");
                }
                out.channel_mask = audio_detail::read_u32_le(payload.data() + 20U);
                const std::uint16_t subfmt = audio_detail::read_u16_le(payload.data() + 24U);
                static constexpr std::uint8_t kGuidTail[12] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                const bool tail_ok =
                    (audio_detail::read_u16_le(payload.data() + 26U) == 0U) &&
                    (std::memcmp(payload.data() + 28U, kGuidTail, 12U) == 0);
                if (!tail_ok || (subfmt != 1U && subfmt != 3U)) {
                    return fail("Unsupported WAVE_FORMAT_EXTENSIBLE subformat");
                }
                out.format_tag = subfmt;
            }
            have_fmt = true;
        } else if (is_data) {
            out.data_offset = pos;
            out.data_size = payload_size;
            have_data = true;
        }
        pos += payload_size;
        if ((payload_size & 1U) != 0U && pos < bytes.size()) {
            ++pos;
        }
        if (have_fmt && have_data) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        return fail("Missing WAV fmt or data chunk");
    }
    if (out.channels == 0U || out.channels > kMaxAudioSourceChannels) {
        return fail("Unsupported WAV channel count");
    }
    if (out.sample_rate == 0U || out.sample_rate > kMaxAudioSampleRate) {
        return fail("Unsupported WAV sample rate");
    }
    const std::uint16_t bits = out.bits_per_sample;
    const bool is_int = (out.format_tag == 1U) &&
                        (bits == 8U || bits == 16U || bits == 24U || bits == 32U);
    const bool is_float = (out.format_tag == 3U) && (bits == 32U);
    if (!is_int && !is_float) {
        return fail("Unsupported WAV format or bit depth");
    }
    if (out.frame_count() == 0U) {
        return fail("Empty WAV data chunk");
    }
    return true;
}

namespace audio_detail {

// Converts `sample_count` interleaved WAV samples starting at `src` to float.
inline void convert_wav_samples(const WavLayout& layout, const std::uint8_t* src,
                                std::size_t sample_count, float* dst) noexcept {
    if (layout.format_tag == 3U) {
        for (std::size_t i = 0; i < sample_count; ++i) {
            float v = 0.0f;
            std::memcpy(&v, src + i * 4U, sizeof(float));
            dst[i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        }
        return;
    }
    switch (layout.bits_per_sample) {
        case 8U:
            for (std::size_t i = 0; i < sample_count; ++i) {
                dst[i] = (static_cast<float>(src[i]) - 128.0f) * (1.0f / 128.0f);
            }
            break;
        case 16U:
            for (std::size_t i = 0; i < sample_count; ++i) {
                const auto s = static_cast<std::int16_t>(read_u16_le(src + i * 2U));
                dst[i] = static_cast<float>(s) * (1.0f / 32768.0f);
            }
            break;
        case 24U:
            for (std::size_t i = 0; i < sample_count; ++i) {
                const std::uint8_t* p = src + i * 3U;
                // Place the 24 bits at the top of a 32-bit word; the arithmetic
                // value is then the sign-extended sample scaled by 256.
                const std::uint32_t u = (static_cast<std::uint32_t>(p[0]) << 8U) |
                                        (static_cast<std::uint32_t>(p[1]) << 16U) |
                                        (static_cast<std::uint32_t>(p[2]) << 24U);
                dst[i] = static_cast<float>(static_cast<std::int32_t>(u)) *
                         (1.0f / 2147483648.0f);
            }
            break;
        default:
            for (std::size_t i = 0; i < sample_count; ++i) {
                const auto s = static_cast<std::int32_t>(read_u32_le(src + i * 4U));
                dst[i] = static_cast<float>(s) * (1.0f / 2147483648.0f);
            }
            break;
    }
}

}  // namespace audio_detail

// Clean-room RIFF/WAVE PCM decoder (8/16/24/32-bit integer and 32-bit float).
[[nodiscard]] inline bool decode_wav_pcm(std::span<const std::uint8_t> bytes,
                                         AudioPcmBuffer& out,
                                         std::string* error_out = nullptr) {
    out = {};
    WavLayout layout{};
    if (!parse_wav_layout(bytes, layout, error_out)) {
        return false;
    }
    const std::size_t frames = layout.frame_count();
    if (frames > kMaxDecodedAudioSamples / static_cast<std::size_t>(layout.channels)) {
        if (error_out != nullptr) {
            *error_out = "WAV payload exceeds maximum decoded sample bound";
        }
        return false;
    }
    out.samples.resize(frames * static_cast<std::size_t>(layout.channels));
    audio_detail::convert_wav_samples(layout, bytes.data() + layout.data_offset,
                                      out.samples.size(), out.samples.data());
    out.sample_rate = layout.sample_rate;
    out.channels = layout.channels;
    audio_detail::fold_to_stereo_in_place(out.samples, out.channels,
                                          audio_detail::ChannelOrder::wav, layout.channel_mask);
    return out.valid();
}

// Streams an already decoded `AudioPcmBuffer` (tests, predecoded tracks).
class PcmBufferStreamDecoder final : public AudioStreamDecoder {
public:
    explicit PcmBufferStreamDecoder(AudioPcmBuffer buffer) : buf_(std::move(buffer)) {}

    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return buf_.sample_rate; }
    [[nodiscard]] std::uint16_t channels() const noexcept override { return buf_.channels; }
    [[nodiscard]] std::uint64_t total_frames() const noexcept override {
        return static_cast<std::uint64_t>(buf_.frame_count());
    }
    [[nodiscard]] std::uint64_t cursor_frame() const noexcept override { return cursor_; }
    [[nodiscard]] bool seek_start() override {
        cursor_ = 0U;
        return true;
    }

    [[nodiscard]] std::size_t read_frames(std::size_t max_frames,
                                          std::vector<float>& out_interleaved) override {
        out_interleaved.clear();
        if (max_frames == 0U || !buf_.valid()) {
            return 0U;
        }
        const std::size_t total = buf_.frame_count();
        if (cursor_ >= total) {
            return 0U;
        }
        const std::size_t count = std::min(total - cursor_, max_frames);
        const std::size_t ch = static_cast<std::size_t>(buf_.channels);
        out_interleaved.assign(buf_.samples.begin() + static_cast<std::ptrdiff_t>(cursor_ * ch),
                               buf_.samples.begin() +
                                   static_cast<std::ptrdiff_t>((cursor_ + count) * ch));
        cursor_ += count;
        return count;
    }

private:
    AudioPcmBuffer buf_{};
    std::size_t cursor_ = 0U;
};

// Streams RIFF/WAVE PCM straight from the owned file bytes, converting only the
// frames each `read_frames` call asks for (no full-file float copy).
class WavStreamDecoder final : public AudioStreamDecoder {
public:
    // Returns nullptr (and sets `error_out`) when the header does not validate.
    [[nodiscard]] static std::unique_ptr<WavStreamDecoder> open(std::vector<std::uint8_t> bytes,
                                                                std::string* error_out = nullptr) {
        WavLayout layout{};
        if (!parse_wav_layout(bytes, layout, error_out)) {
            return nullptr;
        }
        return std::unique_ptr<WavStreamDecoder>(new WavStreamDecoder(std::move(bytes), layout));
    }

    [[nodiscard]] std::uint32_t sample_rate() const noexcept override {
        return layout_.sample_rate;
    }
    [[nodiscard]] std::uint16_t channels() const noexcept override {
        return layout_.channels <= 2U ? layout_.channels : 2U;
    }
    [[nodiscard]] std::uint64_t total_frames() const noexcept override {
        return static_cast<std::uint64_t>(layout_.frame_count());
    }
    [[nodiscard]] std::uint64_t cursor_frame() const noexcept override { return cursor_; }
    [[nodiscard]] bool seek_start() override {
        cursor_ = 0U;
        return true;
    }

    [[nodiscard]] std::size_t read_frames(std::size_t max_frames,
                                          std::vector<float>& out_interleaved) override {
        out_interleaved.clear();
        const std::size_t total = layout_.frame_count();
        if (max_frames == 0U || cursor_ >= total) {
            return 0U;
        }
        const std::size_t count = std::min(total - cursor_, max_frames);
        const std::size_t ch = layout_.channels;
        out_interleaved.resize(count * ch);
        audio_detail::convert_wav_samples(
            layout_, bytes_.data() + layout_.data_offset + cursor_ * layout_.frame_stride(),
            out_interleaved.size(), out_interleaved.data());
        std::uint16_t out_ch = layout_.channels;
        audio_detail::fold_to_stereo_in_place(out_interleaved, out_ch,
                                              audio_detail::ChannelOrder::wav,
                                              layout_.channel_mask);
        cursor_ += count;
        return count;
    }

private:
    WavStreamDecoder(std::vector<std::uint8_t> bytes, const WavLayout& layout)
        : bytes_(std::move(bytes)), layout_(layout) {}

    std::vector<std::uint8_t> bytes_{};
    WavLayout layout_{};
    std::size_t cursor_ = 0U;
};

// Decodes `.wav`, `.ogg`, or `.mp3` memory buffers.
[[nodiscard]] inline bool decode_audio_bytes(std::span<const std::uint8_t> bytes,
                                             std::string_view filename_hint,
                                             const AudioCodecBackend* backend,
                                             AudioPcmBuffer& out,
                                             std::string* error_out = nullptr) {
    out = {};
    const AudioCodecKind kind = detect_audio_codec(bytes, filename_hint);
    if (kind == AudioCodecKind::wav) {
        return decode_wav_pcm(bytes, out, error_out);
    }
    if (kind == AudioCodecKind::unknown) {
        if (error_out != nullptr) {
            *error_out = "Unrecognized audio format";
        }
        return false;
    }
    if (backend == nullptr || !backend->supports(kind)) {
        if (error_out != nullptr) {
            *error_out = "No audio codec backend available for compressed format";
        }
        return false;
    }
    return backend->decode_memory(kind, bytes, out, error_out);
}

// Opens a pull-based stream decoder for `.wav`, `.ogg`, or `.mp3`.
[[nodiscard]] inline std::unique_ptr<AudioStreamDecoder> open_audio_stream(
    std::vector<std::uint8_t> owned_bytes, std::string_view filename_hint,
    const AudioCodecBackend* backend, std::string* error_out = nullptr) {
    const AudioCodecKind kind = detect_audio_codec(owned_bytes, filename_hint);
    if (kind == AudioCodecKind::wav) {
        return WavStreamDecoder::open(std::move(owned_bytes), error_out);
    }
    if (kind == AudioCodecKind::unknown) {
        if (error_out != nullptr) {
            *error_out = "Unrecognized audio format";
        }
        return nullptr;
    }
    if (backend == nullptr || !backend->supports(kind)) {
        if (error_out != nullptr) {
            *error_out = "No audio codec backend available for compressed format";
        }
        return nullptr;
    }
    return backend->open_stream(kind, std::move(owned_bytes), error_out);
}

// Reconstructs the `mbAgent::playSound` (`0x140057170`) human voice pitch hash.
// Reads `m_no` (`+0xc`), `m_troopNo` (`+0x2c8`), and `m_partyNo` (`+0x9f0`), scales
// `(rem / 871.0f - 0.5f)` by `0.3f`, and returns `1.0f + delta` (`> 0.001f`),
// `1.0f / (1.0f - delta)` (`< -0.001f`), or `1.0f`. Products form in `uint32_t`
// so extreme negative inputs wrap identically without signed-overflow UB.
[[nodiscard]] inline float compute_agent_voice_pitch(std::int32_t agent_no,
                                                     std::int32_t troop_no,
                                                     std::int32_t party_no) noexcept {
    const std::uint32_t sum =
        (static_cast<std::uint32_t>(agent_no) + 29U) * 0x14fdU +
        static_cast<std::uint32_t>(troop_no) * 0x76efU +
        static_cast<std::uint32_t>(party_no) * 0x244adU;
    std::int32_t acc = static_cast<std::int32_t>(sum);  // C++20: modular conversion
    acc %= 0x1b41;
    acc = (acc * 0x51f) % 0x367;  // |acc| < 0x1b41, so this product cannot overflow
    const float unit = static_cast<float>(acc) / 871.0f;
    const float delta = (unit - 0.5f) * 0.3f;
    if (delta > 0.001f) {
        return 1.0f + delta;
    }
    if (delta < -0.001f) {
        return 1.0f / (1.0f - delta);
    }
    return 1.0f;
}

// Retail `mbSoundSample::readStream` (0x140332580) 4-step file search order:
//   1. `<module_dir>/Sounds/languages/<language>/<filename>` (when language non-empty)
//   2. `<module_dir>/Sounds/<filename>`
//   3. `Sounds/languages/<language>/<filename>` (when language non-empty)
//   4. `Sounds/<filename>`
[[nodiscard]] inline std::string resolve_sound_sample_path(wse::platform::Vfs& vfs,
                                                           std::string_view module_dir,
                                                           std::string_view language,
                                                           std::string_view filename) {
    if (filename.empty()) {
        return {};
    }
    auto try_candidate = [&](std::string_view p) -> std::string {
        if (vfs.is_file(p)) {
            return std::string(p);
        }
        return {};
    };

    if (!module_dir.empty()) {
        if (!language.empty()) {
            std::string p;
            audio_detail::append_path_segment(p, module_dir);
            audio_detail::append_path_segment(p, "Sounds/languages");
            audio_detail::append_path_segment(p, language);
            audio_detail::append_path_segment(p, filename);
            if (auto hit = try_candidate(p); !hit.empty()) {
                return hit;
            }
        }
        std::string p;
        audio_detail::append_path_segment(p, module_dir);
        audio_detail::append_path_segment(p, "Sounds");
        audio_detail::append_path_segment(p, filename);
        if (auto hit = try_candidate(p); !hit.empty()) {
            return hit;
        }
    }

    if (!language.empty()) {
        std::string p = "Sounds/languages";
        audio_detail::append_path_segment(p, language);
        audio_detail::append_path_segment(p, filename);
        if (auto hit = try_candidate(p); !hit.empty()) {
            return hit;
        }
    }

    std::string p = "Sounds";
    audio_detail::append_path_segment(p, filename);
    return try_candidate(p);
}

// Retail `mbMusicTrack::readStream` (0x140290870): searches `<module_dir>/Music/<file>`
// then base `Music/<file>` (`%smusic/%s` / `%sMusic/%s` / `music/%s` / `Music/%s`).
[[nodiscard]] inline std::string resolve_music_track_path(wse::platform::Vfs& vfs,
                                                          std::string_view module_dir,
                                                          std::string_view filename) {
    if (filename.empty()) {
        return {};
    }
    if (!module_dir.empty()) {
        std::string p;
        audio_detail::append_path_segment(p, module_dir);
        audio_detail::append_path_segment(p, "Music");
        audio_detail::append_path_segment(p, filename);
        if (vfs.is_file(p)) {
            return p;
        }
    }
    std::string p = "Music";
    audio_detail::append_path_segment(p, filename);
    if (vfs.is_file(p)) {
        return p;
    }
    return {};
}

// Mirrors `mbSoundSample::playSound` (0x140332a70) & `mbSoundManager::setChannelPropertiesAndPlay`
// (0x140333b40, +0x116): priority nibble `[4..7]` defaults to 5 when 0; volume nibble `[8..11]`
// defaults to 10 when 0 and divides by `15.0f` (+0x131).
[[nodiscard]] constexpr int decode_sound_priority_nibble(std::uint32_t flags) noexcept {
    const std::uint32_t nibble = (flags >> 4U) & 0xFU;
    return nibble == 0U ? 5 : static_cast<int>(nibble);
}

[[nodiscard]] constexpr float decode_sound_volume(std::uint32_t flags) noexcept {
    const std::uint32_t nibble = (flags >> 8U) & 0xFU;
    const std::uint32_t eff = nibble == 0U ? 10U : nibble;
    return static_cast<float>(eff) / 15.0f;
}

// Mirrors `mbMusicTrack::readStream` (`0x140290870`, +0x368): merges `flags` into `continue_flags`
// and expands culture mask `0` to `0x3F` (all 6 native factions/cultures).
struct NormalizedMusicFlags {
    std::uint32_t flags = 0;
    std::uint32_t continue_flags = 0;
    std::uint32_t culture_flags = 0;
    std::uint32_t continue_culture_flags = 0;
};

[[nodiscard]] constexpr NormalizedMusicFlags normalize_music_track_flags(
    std::uint32_t raw_flags, std::uint32_t raw_continue_flags) noexcept {
    NormalizedMusicFlags out{};
    out.flags = raw_flags;
    out.continue_flags = raw_continue_flags | raw_flags;
    const std::uint32_t cf = raw_flags & kMusicFlagCultureMask;
    out.culture_flags = (cf == 0U) ? kMusicFlagCultureMask : cf;
    out.continue_culture_flags =
        (out.continue_flags & kMusicFlagCultureMask) | out.culture_flags;
    return out;
}

}  // namespace wse::asset
