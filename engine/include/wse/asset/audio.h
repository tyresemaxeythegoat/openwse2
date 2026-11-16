// Clean-room v1.1.5.5 audio asset decoders, retail path resolution, and
// sound/music flag helpers (Slice P6, docs/PLATFORM_PLAN.md).
//
// Grounded in WSE2 v1.1.5.5 PDB/binary evidence:
//   - mbSoundSample::readStream            0x1403317c0 (%sSounds\languages\%s\%s -> %sSounds\%s -> Sounds\languages\%s\%s -> Sounds\%s)
//   - mbSoundSample::playSound             0x140331cb0 (priority & flags 0x01=2D, 0x02=looping)
//   - mbSoundManager::readStream           0x1403322e0
//   - mbSoundManager::playSoundAtPosition  0x140332ac0 (0x1000000 LOD fallback, culture variant filter)
//   - mbSoundManager::setChannelPropertiesAndPlay 0x140332d80 (volume / 15.0f, bit 2 random start, bit 3 disable freq var)
//   - mbMusicTrack::readStream             0x140290710 (%sMusic/%s -> Music/%s; continue_flags |= flags, culture fallback 0x3f at 0x140290a78)
//
// Note: third-party compressed codec headers (stb_vorbis / dr_mp3) never appear
// here; RIFF/WAVE PCM is self-contained and OGG/MP3 decode delegates through
// `AudioCodecBackend` (implemented in `wse/asset/audio_codecs.h` + `wse_audio`).

#pragma once

#include <algorithm>
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
inline constexpr std::size_t kMaxDecodedAudioSamples = 48000ULL * 2ULL * 60ULL * 30ULL;

// mbSound / mbSoundSample flags (`0x140331cb0`, `0x140332be0`, `0x140332d80`):
inline constexpr std::uint32_t kSoundFlag2D = 0x00000001U;
inline constexpr std::uint32_t kSoundFlagLooping = 0x00000002U;
inline constexpr std::uint32_t kSoundFlagRandomStartPos = 0x00000004U;
inline constexpr std::uint32_t kSoundFlagDisableFreqVariation = 0x00000008U;
inline constexpr std::uint32_t kSoundFlagLodFallback = 0x01000000U;

// mbMusicTrack flags (`0x140290e60`, `0x140291096`, `0x1402911b3`):
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
    [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t total_frames() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t cursor_frame() const noexcept = 0;
    [[nodiscard]] virtual bool seek_start() = 0;

    // Reads up to `max_frames` interleaved float frames into `out_interleaved`.
    // Returns frames decoded; 0 indicates EOF or error.
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
           (static_cast<std::uint16_t>(p[1]) << 8U);
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

inline void downmix_or_copy_to_mono_or_stereo(const std::vector<float>& src,
                                              std::uint16_t src_channels,
                                              std::uint16_t& dst_channels,
                                              std::vector<float>& out) {
    const std::size_t frames = src.size() / static_cast<std::size_t>(src_channels);
    if (src_channels == 1U || src_channels == 2U) {
        dst_channels = src_channels;
        out = src;
        return;
    }
    dst_channels = 2U;
    out.resize(frames * 2U);
    const float inv = 1.0f / static_cast<float>(src_channels);
    for (std::size_t f = 0; f < frames; ++f) {
        float l = 0.0f;
        float r = 0.0f;
        for (std::uint16_t ch = 0; ch < src_channels; ++ch) {
            const float s = src[f * src_channels + ch];
            if ((ch & 1U) == 0U) {
                l += s;
            } else {
                r += s;
            }
        }
        out[f * 2U + 0U] = l * (2.0f * inv);
        out[f * 2U + 1U] = r * (2.0f * inv);
    }
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
    if (bytes.size() >= 2U && bytes[0] == 0xFFU && (bytes[1] & 0xE0U) == 0xE0U) {
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

// Clean-room RIFF/WAVE PCM decoder (8/16/24/32-bit integer and 32-bit float).
[[nodiscard]] inline bool decode_wav_pcm(std::span<const std::uint8_t> bytes,
                                         AudioPcmBuffer& out,
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
    std::uint16_t format_tag = 0U;
    std::uint16_t raw_channels = 0U;
    std::uint32_t sample_rate = 0U;
    std::uint16_t bits_per_sample = 0U;
    std::span<const std::uint8_t> data_span{};

    std::size_t pos = 12U;
    while (pos + 8U <= bytes.size()) {
        const std::uint8_t* hdr = bytes.data() + pos;
        const std::uint32_t chunk_size = audio_detail::read_u32_le(hdr + 4U);
        pos += 8U;
        if (static_cast<std::size_t>(chunk_size) > bytes.size() - pos) {
            return fail("Truncated WAV chunk");
        }
        const std::span<const std::uint8_t> payload = bytes.subspan(pos, chunk_size);
        if (audio_detail::matches_ascii4(hdr, "fmt ")) {
            if (payload.size() < 16U) {
                return fail("Truncated WAV fmt chunk");
            }
            format_tag = audio_detail::read_u16_le(payload.data());
            raw_channels = audio_detail::read_u16_le(payload.data() + 2U);
            sample_rate = audio_detail::read_u32_le(payload.data() + 4U);
            bits_per_sample = audio_detail::read_u16_le(payload.data() + 14U);
            if (format_tag == 0xFFFEU) {
                if (payload.size() < 40U) {
                    return fail("Truncated WAVE_FORMAT_EXTENSIBLE fmt chunk");
                }
                const std::uint16_t subfmt = audio_detail::read_u16_le(payload.data() + 24U);
                static constexpr std::uint8_t kGuidTail[12] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                const bool tail_ok =
                    (audio_detail::read_u16_le(payload.data() + 26U) == 0U) &&
                    (std::memcmp(payload.data() + 28U, kGuidTail, 12U) == 0);
                if (!tail_ok || (subfmt != 1U && subfmt != 3U)) {
                    return fail("Unsupported WAVE_FORMAT_EXTENSIBLE subformat");
                }
                format_tag = subfmt;
            }
            have_fmt = true;
        } else if (audio_detail::matches_ascii4(hdr, "data")) {
            data_span = payload;
            have_data = true;
        }
        pos += static_cast<std::size_t>(chunk_size);
        if ((chunk_size & 1U) != 0U && pos < bytes.size()) {
            ++pos;
        }
        if (have_fmt && have_data) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        return fail("Missing WAV fmt or data chunk");
    }
    if (raw_channels == 0U || raw_channels > 8U) {
        return fail("Unsupported WAV channel count");
    }
    if (sample_rate == 0U || sample_rate > kMaxAudioSampleRate) {
        return fail("Unsupported WAV sample rate");
    }

    const bool is_int = (format_tag == 1U) &&
                        (bits_per_sample == 8U || bits_per_sample == 16U ||
                         bits_per_sample == 24U || bits_per_sample == 32U);
    const bool is_float = (format_tag == 3U) && (bits_per_sample == 32U);
    if (!is_int && !is_float) {
        return fail("Unsupported WAV format or bit depth");
    }

    const std::size_t bytes_per_sample = static_cast<std::size_t>(bits_per_sample / 8U);
    const std::size_t frame_stride = bytes_per_sample * static_cast<std::size_t>(raw_channels);
    if (frame_stride == 0U) {
        return fail("Invalid WAV frame stride");
    }
    const std::size_t frames = data_span.size() / frame_stride;
    if (frames == 0U) {
        return fail("Empty WAV data chunk");
    }
    if (frames > kMaxDecodedAudioSamples / static_cast<std::size_t>(raw_channels)) {
        return fail("WAV payload exceeds maximum decoded sample bound");
    }

    std::vector<float> raw_pcm(frames * static_cast<std::size_t>(raw_channels));
    const std::uint8_t* ptr = data_span.data();
    const std::size_t total_samples = raw_pcm.size();
    if (format_tag == 3U) {
        for (std::size_t i = 0; i < total_samples; ++i) {
            float v = 0.0f;
            std::memcpy(&v, ptr + i * 4U, sizeof(float));
            raw_pcm[i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        }
    } else if (bits_per_sample == 8U) {
        for (std::size_t i = 0; i < total_samples; ++i) {
            raw_pcm[i] = (static_cast<float>(ptr[i]) - 128.0f) / 128.0f;
        }
    } else if (bits_per_sample == 16U) {
        for (std::size_t i = 0; i < total_samples; ++i) {
            const std::int16_t s =
                static_cast<std::int16_t>(audio_detail::read_u16_le(ptr + i * 2U));
            raw_pcm[i] = static_cast<float>(s) / 32768.0f;
        }
    } else if (bits_per_sample == 24U) {
        for (std::size_t i = 0; i < total_samples; ++i) {
            const std::uint8_t* p = ptr + i * 3U;
            std::uint32_t u = static_cast<std::uint32_t>(p[0]) |
                              (static_cast<std::uint32_t>(p[1]) << 8U) |
                              (static_cast<std::uint32_t>(p[2]) << 16U);
            if ((u & 0x800000U) != 0U) {
                u |= 0xFF000000U;
            }
            const std::int32_t s = static_cast<std::int32_t>(u);
            raw_pcm[i] = static_cast<float>(s) / 8388608.0f;
        }
    } else {
        for (std::size_t i = 0; i < total_samples; ++i) {
            const std::int32_t s =
                static_cast<std::int32_t>(audio_detail::read_u32_le(ptr + i * 4U));
            raw_pcm[i] = static_cast<float>(static_cast<double>(s) / 2147483648.0);
        }
    }

    out.sample_rate = sample_rate;
    audio_detail::downmix_or_copy_to_mono_or_stereo(raw_pcm, raw_channels, out.channels,
                                                    out.samples);
    return out.valid();
}

// Streams RIFF/WAVE PCM directly from an owned buffer.
class WavStreamDecoder final : public AudioStreamDecoder {
public:
    explicit WavStreamDecoder(AudioPcmBuffer buffer) : buf_(std::move(buffer)) {}

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
        const std::size_t avail = total - cursor_;
        const std::size_t count = std::min(avail, max_frames);
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
        AudioPcmBuffer wav;
        if (!decode_wav_pcm(owned_bytes, wav, error_out)) {
            return nullptr;
        }
        return std::make_unique<WavStreamDecoder>(std::move(wav));
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

// Retail `mbSoundSample::readStream` (0x1403317c0) 4-step file search order:
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

// Retail `mbMusicTrack::readStream` (0x140290710): searches `<module_dir>/Music/<file>`
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

// Mirrors `mbSoundSample::playSound` (0x140331cb0) & `mbSoundManager::setChannelPropertiesAndPlay`
// (0x140332e96): priority nibble `[4..7]` defaults to 5 when 0; volume nibble `[8..11]` defaults
// to 10 when 0 and divides by `15.0f` (`0x140332eb1`).
[[nodiscard]] constexpr int decode_sound_priority_nibble(std::uint32_t flags) noexcept {
    const std::uint32_t nibble = (flags >> 4U) & 0xFU;
    return nibble == 0U ? 5 : static_cast<int>(nibble);
}

[[nodiscard]] constexpr float decode_sound_volume(std::uint32_t flags) noexcept {
    const std::uint32_t nibble = (flags >> 8U) & 0xFU;
    const std::uint32_t eff = nibble == 0U ? 10U : nibble;
    return static_cast<float>(eff) / 15.0f;
}

// Mirrors `mbMusicTrack::readStream` (`0x140290a78`): merges `flags` into `continue_flags`
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
