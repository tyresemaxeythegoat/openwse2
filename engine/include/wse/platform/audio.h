// Platform seam: audio device abstraction and clean-room 2D/3D stereo mixer +
// music scheduler (Slice P6, docs/PLATFORM_PLAN.md).
//
// Port infrastructure (`AudioDevice`, `NullAudioDevice`, `CaptureAudioDevice`)
// keeps `<SDL3/SDL.h>` out of engine headers; `WseAudioMixer` reconstructs
// WSE2 v1.1.5.6 `mbSoundManager` / `mbMusicManager` semantics from PDB/binary
// evidence:
//   - mbSoundSample::playSound            0x140332a70 (priority calc, 2D/looping -> 0)
//   - mbSoundSample::readStream           0x140332580 (3D min=2.0f, max=80.0f)
//   - mbSoundManager::playSoundAtPosition 0x140333880 (0x1000000 LOD fallback, culture filter)
//   - mbSoundManager::setChannelPropertiesAndPlay 0x140333b40 (vol / 15, random start, freq)
//   - mbSoundManager::setTransform        0x140333730 (listener attributes: forward=-u, up=f)
//   - mbSoundManager::updateOcclusionDistances 0x140334020 (0.9 / 1.1 hysteresis around 80m)
//   - mbSoundManager::update              0x140334250 (virtual channels + channel-end callback)
//   - mbSoundManager::stopSound           0x140333450
//   - mbSoundManager::getSoundProgress    0x140333490 (0..100 or -1)
//   - mbSoundManager::stopAllSounds       0x140333530 (include non-looping bool)
//   - mbMusicManager::frameMove / update  0x140290da0 / 0x1402910c0 (0.5s tick, 5.0s fade, 500.0 recency)
//   - mbMusicManager::startPlaying / stopPlaying 0x140290fc0 / 0x140291080
//
// Port behaviour that is not a retail reconstruction (engine/README.md, "P6 audio"):
//   - channel-end callbacks are queued and delivered after each public call
//     returns the mixer to a consistent state, so callbacks may re-enter it;
//   - per-voice and per-slot gains ramp linearly across each mix block, and the
//     bus is soft-clipped instead of hard-clamped;
//   - the random start position (`kSoundFlagRandomStartPos`) covers the whole
//     sample, and looping samples interpolate across the loop point;
//   - `update()` re-bases its timestamps when the caller's clock jumps backwards.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wse/asset/audio.h"
#include "wse/core/math.h"

namespace wse::platform {

struct AudioDeviceSpec {
    std::uint32_t sample_rate = 48000U;
    std::uint16_t channels = 2U;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual bool open(const AudioDeviceSpec& spec) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual AudioDeviceSpec spec() const noexcept = 0;
    virtual bool submit_frames(std::span<const float> interleaved_stereo) = 0;
    [[nodiscard]] virtual std::size_t queued_frames() const noexcept = 0;
    virtual void clear() = 0;
    virtual void set_paused(bool paused) = 0;
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
};

class NullAudioDevice final : public AudioDevice {
public:
    bool open(const AudioDeviceSpec& s) override {
        open_ = (s.sample_rate > 0U && s.channels == 2U);
        if (open_) {
            spec_ = s;
        }
        return open_;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] AudioDeviceSpec spec() const noexcept override { return spec_; }
    bool submit_frames(std::span<const float> interleaved_stereo) override {
        if (!open_ || (interleaved_stereo.size() & 1U) != 0U) {
            return false;
        }
        submitted_frames_ += interleaved_stereo.size() / 2U;
        return true;
    }
    [[nodiscard]] std::size_t queued_frames() const noexcept override { return 0U; }
    void clear() override {}
    void set_paused(bool paused) override { paused_ = paused; }
    [[nodiscard]] const char* backend_name() const noexcept override { return "null"; }

    [[nodiscard]] std::size_t submitted_frames() const noexcept { return submitted_frames_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

private:
    AudioDeviceSpec spec_{};
    bool open_ = false;
    bool paused_ = false;
    std::size_t submitted_frames_ = 0;
};

// Deterministic capture sink for host tests and headless verification.
class CaptureAudioDevice final : public AudioDevice {
public:
    bool open(const AudioDeviceSpec& s) override {
        if (s.sample_rate == 0U || s.channels != 2U) {
            return false;
        }
        spec_ = s;
        open_ = true;
        samples_.clear();
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] AudioDeviceSpec spec() const noexcept override { return spec_; }
    bool submit_frames(std::span<const float> interleaved_stereo) override {
        if (!open_ || (interleaved_stereo.size() & 1U) != 0U) {
            return false;
        }
        samples_.insert(samples_.end(), interleaved_stereo.begin(), interleaved_stereo.end());
        return true;
    }
    [[nodiscard]] std::size_t queued_frames() const noexcept override {
        return samples_.size() / 2U;
    }
    void clear() override { samples_.clear(); }
    void set_paused(bool paused) override { paused_ = paused; }
    [[nodiscard]] const char* backend_name() const noexcept override { return "capture"; }

    [[nodiscard]] const std::vector<float>& captured_samples() const noexcept { return samples_; }
    [[nodiscard]] std::size_t captured_frames() const noexcept { return samples_.size() / 2U; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    [[nodiscard]] double channel_rms(std::size_t ch) const noexcept {
        if (ch >= 2U || samples_.size() < 2U) {
            return 0.0;
        }
        const std::size_t frames = samples_.size() / 2U;
        double sum_sq = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            const double v = static_cast<double>(samples_[f * 2U + ch]);
            sum_sq += v * v;
        }
        return std::sqrt(sum_sq / static_cast<double>(frames));
    }

    [[nodiscard]] double rms_left() const noexcept { return channel_rms(0U); }
    [[nodiscard]] double rms_right() const noexcept { return channel_rms(1U); }

    [[nodiscard]] std::uint64_t hash_pcm_i16() const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (float v : samples_) {
            const float clamped = std::clamp(v, -1.0f, 1.0f);
            const std::int32_t q = static_cast<std::int32_t>(std::lround(clamped * 32767.0f));
            const std::uint16_t u = static_cast<std::uint16_t>(static_cast<std::int16_t>(q));
            h ^= static_cast<std::uint64_t>(u & 0xFFU);
            h *= 1099511628211ULL;
            h ^= static_cast<std::uint64_t>((u >> 8U) & 0xFFU);
            h *= 1099511628211ULL;
        }
        return h;
    }

private:
    AudioDeviceSpec spec_{};
    bool open_ = false;
    bool paused_ = false;
    std::vector<float> samples_{};
};

// Defaults match `rglConfig::Sound` globals verified across v1.1.5.6
// `rglConfig::load` (`0x140264700`), `mbSoundManager::initializeFmod` (`0x140333d40`),
// `mbSoundSample::readStream` (`0x140332580`), and `mbSoundManager::updateOcclusionDistances`
// (`0x140334020`): `bSound=1`, `bMusic=1`, `fSoundVolume=1.0f`, `fMusicVolume=0.4f`,
// `iMaxNumChannels=64`, `iMaxNumVirtualChannels=100`, `fDistancePriorityFactor=0.47f`,
// `fOcclusionMaxDistance=80.0f`, 3D min=2.0f / max=80.0f, `bUseSoundOcclusion=1`,
// `bDistanceFilter=1`, `bHrtfFilter=1`, `bDisableFrequencyVariation=0`.
//
// Mirrors `rglConfig` audio fields loaded in `0x140264700` / `0x140333d40`.
// `mbSoundManager::initializeFmod` (`0x140333d40`) maps `bDistanceFilter` to
// `FMOD_INIT_DISTANCE_FILTERING` (`0x200`) and `bHrtfFilter` to `FMOD_INIT_HRTF_LOWPASS`
// (`0x10`) while 3D inverse rolloff (`min=2.0f, max=80.0f`) is always active; here
// `distance_filter` gates distance rolloff and `hrtf_filter` is accepted for parity.
struct AudioConfig {
    bool sound_enabled = true;
    bool music_enabled = true;
    float sound_volume = 1.0f;
    float music_volume = 0.4f;
    std::int32_t max_channels = 64;
    std::int32_t max_virtual_channels = 100;
    float distance_priority_factor = 0.47f;
    float occlusion_max_distance = 80.0f;
    float min_distance_3d = 2.0f;
    float max_distance_3d = 80.0f;
    bool occlusion_filter = true;
    bool distance_filter = true;
    bool hrtf_filter = true;
    bool disable_frequency_variation = false;
};

// Clamps user-supplied config (rgl_config.txt) into ranges the mixer can use:
// non-finite or negative values fall back to defaults, `max_distance_3d >=
// min_distance_3d > 0`, and channel counts stay within `[1, 256]`.
[[nodiscard]] inline AudioConfig sanitize_audio_config(AudioConfig c) noexcept {
    const AudioConfig d{};
    const auto finite_or = [](float v, float def) noexcept {
        return std::isfinite(v) ? v : def;
    };
    c.sound_volume = std::max(0.0f, finite_or(c.sound_volume, d.sound_volume));
    c.music_volume = std::max(0.0f, finite_or(c.music_volume, d.music_volume));
    c.max_virtual_channels = std::clamp(c.max_virtual_channels, 1, 256);
    c.max_channels = std::clamp(c.max_channels, 1, 256);
    c.distance_priority_factor =
        std::clamp(finite_or(c.distance_priority_factor, d.distance_priority_factor), 0.0f, 1.0f);
    c.occlusion_max_distance =
        std::max(0.0f, finite_or(c.occlusion_max_distance, d.occlusion_max_distance));
    c.min_distance_3d = finite_or(c.min_distance_3d, d.min_distance_3d);
    if (!(c.min_distance_3d > 1e-3f)) {
        c.min_distance_3d = 1e-3f;
    }
    c.max_distance_3d =
        std::max(c.min_distance_3d, finite_or(c.max_distance_3d, d.max_distance_3d));
    return c;
}

struct AudioListener {
    wse::core::WseVec4 position{};
    wse::core::WseVec4 velocity{};
    wse::core::WseVec4 right{1.0f, 0.0f, 0.0f, 0.0f};
    wse::core::WseVec4 forward{0.0f, 1.0f, 0.0f, 0.0f};
    wse::core::WseVec4 up{0.0f, 0.0f, 1.0f, 0.0f};
};

// Reconstructs `mbSoundManager::setTransform` (`0x140333730`): Warband passes a camera
// frame where `-u` maps to FMOD listener forward, `+f` to up, and `+s` to right.
[[nodiscard]] inline AudioListener listener_from_camera_transform(
    const wse::core::WseTransform& cam,
    const wse::core::WseVec4& velocity = {}) noexcept {
    AudioListener out{};
    out.position = cam.o;
    out.velocity = velocity;
    out.forward = {-cam.u.x, -cam.u.y, -cam.u.z, 0.0f};
    out.up = {cam.f.x, cam.f.y, cam.f.z, 0.0f};
    out.forward.normalize();
    out.up.normalize();
    out.right = {cam.s.x, cam.s.y, cam.s.z, 0.0f};
    if (out.right.length() <= 1e-6f) {
        out.right = out.forward.cross(out.up);
    }
    out.right.normalize();
    return out;
}

struct MixerSoundVariant {
    std::int32_t sample_index = -1;
    // Low byte holds optional culture filter
    // (`mbSoundManager::playSoundAtPosition` `0x140333880`, +0x1be).
    std::uint32_t flags = 0;
};

struct MixerSoundSample {
    std::uint32_t flags = 0;
    std::shared_ptr<const wse::asset::AudioPcmBuffer> pcm{};
};

struct MixerSoundDef {
    std::uint32_t flags = 0;
    std::vector<MixerSoundVariant> variants{};
};

struct MixerMusicTrack {
    std::string path{};
    wse::asset::NormalizedMusicFlags norm{};
    std::shared_ptr<const wse::asset::AudioPcmBuffer> pcm{};
};

// Mirrors `mbSoundSample::playSound` (`0x140332a70`) priority calculation:
// 0 for 2D or looping sounds (`flags & 3`), else `[2, 255]` from priority nibble and distance.
[[nodiscard]] inline std::int32_t compute_sound_priority(
    std::uint32_t flags, float distance_sq, float distance_priority_factor) noexcept {
    if ((flags & (wse::asset::kSoundFlag2D | wse::asset::kSoundFlagLooping)) != 0U) {
        return 0;
    }
    const int prio_nibble = wse::asset::decode_sound_priority_nibble(flags);
    const float p = static_cast<float>(prio_nibble) * 0.0625f;
    const float dist = std::sqrt(std::max(0.0f, distance_sq));
    const float dist_term = 3.5f / (dist + 3.5f);
    const float prio_term = (1.0f - distance_priority_factor) +
                            (p * p * distance_priority_factor);
    const float raw = (1.0f - (dist_term * prio_term)) * 255.0f;
    const int rounded = static_cast<int>(std::floor(raw) +
                                         ((raw - std::floor(raw)) > 0.5f ? 1.0f : 0.0f));
    return std::clamp(rounded, 2, 255);
}

// Reconstructs deterministic MSVCRT `rand()` sequence (`mbSoundManager::playSoundAtPosition`
// `0x140333880` +0x1ee, `mbSoundManager::setChannelPropertiesAndPlay` `0x140333b40` +0x155,
// `mbMusicManager::update` `0x1402910c0` +0x480: `seed = seed * 0x343fd + 0x269ec3`,
// `(seed >> 16) & 0x7fff`).
struct AudioPrng {
    std::uint32_t state = 1U;

    [[nodiscard]] std::int32_t next_s15() noexcept {
        state = state * 0x343fdU + 0x269ec3U;
        return static_cast<std::int32_t>((state >> 16U) & 0x7fffU);
    }

    [[nodiscard]] float next_unit() noexcept {
        return static_cast<float>(next_s15()) / 32767.0f;
    }
};

// Moved to `wse/asset/audio.h` so VM opcode headers can use it without the mixer.
using wse::asset::compute_agent_voice_pitch;

// Soft clipper for the summed bus: identity up to `kKnee`, then a tanh knee that
// approaches (never reaches) full scale, so 64 summed voices saturate smoothly
// instead of hard-clipping. Non-finite samples become silence.
[[nodiscard]] inline float soft_clip_sample(float s) noexcept {
    constexpr float kKnee = 0.8f;
    if (!std::isfinite(s)) {
        return 0.0f;
    }
    const float a = std::fabs(s);
    if (a <= kKnee) {
        return s;
    }
    const float shaped = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
    return std::copysign(shaped, s);
}

class WseAudioMixer {
public:
    using ChannelEndCallback = std::function<void(std::int32_t channel_no)>;
    using MusicStreamOpener =
        std::function<std::unique_ptr<wse::asset::AudioStreamDecoder>(std::size_t track_index)>;

    // Source frames pulled from a music stream per refill.
    static constexpr std::size_t kMusicChunkFrames = 2048U;

    explicit WseAudioMixer(AudioConfig config = {}) : config_(sanitize_audio_config(config)) {
        channels_.resize(static_cast<std::size_t>(config_.max_virtual_channels));
    }

    // Applies new settings without interrupting playback. Channels are only
    // touched when `max_virtual_channels` shrinks; the ones dropped get their
    // end callback like any other stopped sound.
    void set_config(const AudioConfig& config) {
        config_ = sanitize_audio_config(config);
        const auto target = static_cast<std::size_t>(config_.max_virtual_channels);
        for (std::size_t i = target; i < channels_.size(); ++i) {
            end_channel(i);
        }
        channels_.resize(target);
        flush_channel_ends();
    }
    [[nodiscard]] const AudioConfig& config() const noexcept { return config_; }

    void set_seed(std::uint32_t seed) noexcept { rng_.state = seed; }
    void set_output_sample_rate(std::uint32_t sr) noexcept {
        output_sample_rate_ = (sr > 0U && sr <= wse::asset::kMaxAudioSampleRate) ? sr : 48000U;
    }
    [[nodiscard]] std::uint32_t output_sample_rate() const noexcept { return output_sample_rate_; }

    void set_listener(const AudioListener& listener) noexcept { listener_ = listener; }
    [[nodiscard]] const AudioListener& listener() const noexcept { return listener_; }

    // The callback runs after the mixer's own state is consistent (never in the
    // middle of a mix or a channel scan), so it may freely play or stop sounds.
    // Sounds it starts take a free slot only: they never evict another channel
    // (`play_sound*` returns `-1` when every channel is busy).
    void set_channel_end_callback(ChannelEndCallback cb) {
        on_channel_end_ = std::move(cb);
    }
    void set_music_stream_opener(MusicStreamOpener opener) {
        music_opener_ = std::move(opener);
    }

    void load_sound_bank(std::vector<MixerSoundSample> samples,
                         std::vector<MixerSoundDef> sounds) {
        stop_all_sounds(true);
        samples_ = std::move(samples);
        sounds_ = std::move(sounds);
    }

    // Populates or replaces one lazily decoded sample without stopping active channels.
    void set_sample_pcm(std::size_t sample_index,
                        std::shared_ptr<const wse::asset::AudioPcmBuffer> pcm) {
        if (sample_index < samples_.size()) {
            samples_[sample_index].pcm = std::move(pcm);
        }
    }

    void load_music_tracks(std::vector<MixerMusicTrack> tracks) {
        stop_music();
        music_tracks_ = std::move(tracks);
        last_played_time_.assign(music_tracks_.size(), 0.0);
    }

    // Plays a sound event mirroring `mbSoundManager::playSoundAtPosition` (`0x140333880`).
    // Returns channel index `[0, max_virtual_channels)` or `-1` when suppressed/missing;
    // `last_sound_channel()` reports the same value, including `-1`.
    // `velocity` is accepted for API parity; Doppler is not implemented.
    std::int32_t play_sound_at_position(std::int32_t sound_no,
                                        const wse::core::WseVec4& position,
                                        const wse::core::WseVec4& velocity = {},
                                        std::uint32_t extra_flags = 0U,
                                        float freq_multiplier = 1.0f,
                                        std::int32_t culture_filter = 0) {
        (void)velocity;
        const std::int32_t slot = start_sound(sound_no, position, extra_flags, freq_multiplier,
                                              culture_filter);
        last_sound_channel_ = slot;
        flush_channel_ends();
        return slot;
    }

    // Plays 2D sound at listener origin (`mbGame::playSound` `0x1400e3a60`).
    std::int32_t play_sound_2d(std::int32_t sound_no, std::uint32_t extra_flags = 0U) {
        return play_sound_at_position(sound_no, listener_.position, {},
                                      extra_flags | wse::asset::kSoundFlag2D);
    }

    void stop_sound(std::int32_t channel_no) {
        if (channel_no < 0 || static_cast<std::size_t>(channel_no) >= channels_.size()) {
            return;
        }
        end_channel(static_cast<std::size_t>(channel_no));
        flush_channel_ends();
    }

    // Mirrors `mbSoundManager::stopAllSounds` (`0x140333530`):
    // `include_non_looping == false` stops looping channels only; `true` stops all sound channels.
    void stop_all_sounds(bool include_non_looping) {
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            const SoundChannelState& ch = channels_[i];
            if (!ch.active) {
                continue;
            }
            const bool is_looping = (ch.flags & wse::asset::kSoundFlagLooping) != 0U;
            if (include_non_looping || is_looping) {
                end_channel(i);
            }
        }
        flush_channel_ends();
    }

    // Mirrors `mbSoundManager::getSoundProgress` (`0x140333490`): returns `0..100` or `-1`.
    [[nodiscard]] std::int32_t get_sound_progress(std::int32_t channel_no) const noexcept {
        if (channel_no < 0 || static_cast<std::size_t>(channel_no) >= channels_.size()) {
            return -1;
        }
        const SoundChannelState& ch = channels_[static_cast<std::size_t>(channel_no)];
        if (!ch.active || ch.pcm == nullptr || ch.pcm->frame_count() == 0U) {
            return -1;
        }
        const double total = static_cast<double>(ch.pcm->frame_count());
        const double ratio = std::clamp(ch.cursor_frame / total, 0.0, 1.0);
        return std::clamp(static_cast<std::int32_t>(ratio * 100.0), 0, 100);
    }

    [[nodiscard]] bool is_channel_playing(std::int32_t channel_no) const noexcept {
        return channel_no >= 0 &&
               static_cast<std::size_t>(channel_no) < channels_.size() &&
               channels_[static_cast<std::size_t>(channel_no)].active;
    }

    [[nodiscard]] bool is_channel_looping(std::int32_t channel_no) const noexcept {
        return is_channel_playing(channel_no) &&
               (channels_[static_cast<std::size_t>(channel_no)].flags &
                wse::asset::kSoundFlagLooping) != 0U;
    }

    void set_channel_occluded(std::int32_t channel_no, bool occluded) noexcept {
        if (channel_no >= 0 && static_cast<std::size_t>(channel_no) < channels_.size()) {
            channels_[static_cast<std::size_t>(channel_no)].occluded = occluded;
        }
    }

    void set_channel_position(std::int32_t channel_no,
                              const wse::core::WseVec4& pos) noexcept {
        if (channel_no >= 0 && static_cast<std::size_t>(channel_no) < channels_.size()) {
            channels_[static_cast<std::size_t>(channel_no)].position = pos;
        }
    }

    [[nodiscard]] bool is_channel_ignoring_geometry(std::int32_t channel_no) const noexcept {
        if (channel_no < 0 || static_cast<std::size_t>(channel_no) >= channels_.size()) {
            return false;
        }
        return channels_[static_cast<std::size_t>(channel_no)].ignore_geometry;
    }

    // Mirrors `mbSoundManager::updateOcclusionDistances` (`0x140334020`):
    // hysteresis around `occlusion_max_distance * 0.9` / `* 1.1`.
    void update_occlusion_distances() noexcept {
        if (!config_.occlusion_filter || config_.occlusion_max_distance <= 0.0f) {
            return;
        }
        const float max_d = config_.occlusion_max_distance;
        const float enter_sq = (max_d * 1.1f) * (max_d * 1.1f);
        const float exit_sq = (max_d * 0.9f) * (max_d * 0.9f);
        for (auto& ch : channels_) {
            if (!ch.active || (ch.flags & wse::asset::kSoundFlag2D) != 0U) {
                continue;
            }
            const wse::core::WseVec4 d = ch.position - listener_.position;
            const float dsq = d.dot(d);
            if (ch.ignore_geometry) {
                if (dsq <= exit_sq) {
                    ch.ignore_geometry = false;
                }
            } else {
                if (dsq > enter_sq) {
                    ch.ignore_geometry = true;
                }
            }
        }
    }

    [[nodiscard]] std::int32_t last_sound_channel() const noexcept { return last_sound_channel_; }

    [[nodiscard]] std::size_t active_sound_channel_count() const noexcept {
        std::size_t n = 0;
        for (const auto& ch : channels_) {
            if (ch.active) {
                ++n;
            }
        }
        return n;
    }

    // Music control (`0x140290da0`, `0x140290fc0`, `0x1402910c0`).
    void play_track(std::int32_t track_no, std::int32_t option = 0) {
        if (track_no < 0 || static_cast<std::size_t>(track_no) >= music_tracks_.size()) {
            return;
        }
        requested_track_no_ = track_no;
        requested_track_option_ = std::clamp(option, 0, 3);
    }

    void play_cue_track(std::int32_t track_no) {
        if (track_no < 0 || static_cast<std::size_t>(track_no) >= music_tracks_.size()) {
            return;
        }
        queued_cue_track_no_ = track_no;
    }

    void set_music_situation(std::uint32_t situation_flags) noexcept {
        situation_flags_ = situation_flags;
    }

    void set_music_culture(std::uint32_t culture_flags) noexcept {
        culture_flags_ = culture_flags;
    }

    void stop_music() {
        for (auto& slot : music_slots_) {
            slot = {};
        }
        fading_out_ = false;
        active_music_track_ = -1;
        requested_track_no_ = -1;
        queued_cue_track_no_ = -1;
    }

    [[nodiscard]] std::int32_t active_music_track() const noexcept { return active_music_track_; }
    [[nodiscard]] std::int32_t active_cue_track() const noexcept {
        return music_slots_[1].active ? music_slots_[1].track_no : -1;
    }
    [[nodiscard]] bool is_music_fading() const noexcept { return fading_out_; }

    // Runs `mbMusicManager::frameMove` (`0x140290da0`): `0.5s` update tick and `5.0s` fade.
    // `now_seconds` becomes the mixer clock, which `mix_frames` then advances by the
    // audio it renders. A clock that jumps backwards (a new mission or session
    // timer) re-bases every stored timestamp instead of stalling the scheduler.
    void update(double now_seconds) {
        if (last_update_time_ > kNeverUpdated && now_seconds < last_update_time_) {
            const double shift = now_seconds - now_seconds_;
            last_update_time_ += shift;
            fade_start_time_ += shift;
            for (double& t : last_played_time_) {
                t += shift;
            }
        }
        now_seconds_ = now_seconds;
        if (config_.music_enabled && fading_out_) {
            const double elapsed = now_seconds - fade_start_time_;
            if (elapsed > kMusicFadeSeconds) {
                stop_music_slot(0);
                fading_out_ = false;
            }
        }
        if ((now_seconds - last_update_time_) > 0.5 || last_update_time_ <= kNeverUpdated) {
            if (config_.music_enabled) {
                update_music_scheduler(now_seconds);
            }
            update_occlusion_distances();
            last_update_time_ = now_seconds;
        }
    }

    // Mixes `frame_count` stereo float frames (`frame_count * 2` floats), updates cursors
    // and channel expirations, and writes soft-clipped `[-1, 1]` interleaved stereo output.
    void mix_frames(std::size_t frame_count, std::vector<float>& out_interleaved_stereo) {
        out_interleaved_stereo.assign(frame_count * 2U, 0.0f);
        if (frame_count == 0U) {
            return;
        }
        float* out = out_interleaved_stereo.data();
        if (config_.sound_enabled && config_.sound_volume >= kMinAudibleVolume) {
            mix_sound_channels(frame_count, out);
        }
        if (config_.music_enabled && config_.music_volume >= kMinAudibleVolume) {
            mix_music_slots(frame_count, out);
        }
        for (float& s : out_interleaved_stereo) {
            s = soft_clip_sample(s);
        }
        now_seconds_ += static_cast<double>(frame_count) / static_cast<double>(output_sample_rate_);
        flush_channel_ends();
    }

    // Mixes exactly `frame_count` frames and submits them.
    bool pump(AudioDevice& device, std::size_t frame_count) {
        if (!device.is_open()) {
            return false;
        }
        set_output_sample_rate(device.spec().sample_rate);
        update(now_seconds_);
        mix_frames(frame_count, pump_buf_);
        return device.submit_frames(pump_buf_);
    }

    // Tops the device queue up to `target_queued_frames` in blocks of at most
    // `max_block_frames`, so a caller can pump once per game frame without the
    // queue (latency) growing or draining. Returns the frames submitted.
    std::size_t pump_to_latency(AudioDevice& device, std::size_t target_queued_frames,
                                std::size_t max_block_frames = 1024U) {
        if (!device.is_open() || max_block_frames == 0U) {
            return 0U;
        }
        const std::size_t queued = device.queued_frames();
        if (queued >= target_queued_frames) {
            return 0U;
        }
        std::size_t remaining = target_queued_frames - queued;
        std::size_t submitted = 0U;
        while (remaining > 0U) {
            const std::size_t block = std::min(remaining, max_block_frames);
            if (!pump(device, block)) {
                break;
            }
            submitted += block;
            remaining -= block;
        }
        return submitted;
    }

private:
    static constexpr double kNeverUpdated = -1000.0;
    static constexpr double kMusicFadeSeconds = 5.0;
    static constexpr float kMinAudibleVolume = 0.02f;

    struct SoundChannelState {
        bool active = false;
        std::uint64_t generation = 0;
        std::int32_t sample_index = -1;
        std::uint32_t flags = 0;
        wse::core::WseVec4 position{};
        std::shared_ptr<const wse::asset::AudioPcmBuffer> pcm{};
        double cursor_frame = 0.0;
        float volume = 1.0f;
        float lod_gain = 1.0f;
        float freq_multiplier = 1.0f;
        std::int32_t priority = 128;
        bool occluded = false;
        bool ignore_geometry = false;
        // Occlusion low-pass state; primed from the current sample when the filter
        // engages so re-occlusion does not click on stale history.
        bool lp_primed = false;
        float lp_l = 0.0f;
        float lp_r = 0.0f;
        // Gains applied at the end of the previous block; the next block ramps
        // from these to its own targets (no zipper noise on moving sources).
        bool has_gain = false;
        float gain_l = 0.0f;
        float gain_r = 0.0f;
    };

    struct MusicSlotState {
        bool active = false;
        std::int32_t track_no = -1;
        bool looping = false;
        std::shared_ptr<const wse::asset::AudioPcmBuffer> predecoded{};
        std::unique_ptr<wse::asset::AudioStreamDecoder> stream{};
        // Streamed frames; element 0 carries the previous chunk's last frame so
        // interpolation is continuous across refills.
        std::vector<float> chunk_buf{};
        std::vector<float> read_buf{};
        std::uint32_t chunk_rate = 48000U;
        std::uint16_t chunk_channels = 2U;
        bool stream_eof = false;
        double chunk_cursor = 0.0;
        double pcm_cursor = 0.0;
        bool has_gain = false;
        float gain = 0.0f;
    };

    [[nodiscard]] std::int32_t start_sound(std::int32_t sound_no,
                                           const wse::core::WseVec4& position,
                                           std::uint32_t extra_flags, float freq_multiplier,
                                           std::int32_t culture_filter) {
        if (!config_.sound_enabled || config_.sound_volume < kMinAudibleVolume) {
            return -1;
        }
        if (sound_no < 0 || static_cast<std::size_t>(sound_no) >= sounds_.size()) {
            return -1;
        }

        // `0x140333880` +0x120: follow up to 4 LOD fallback sounds when `distance > 15.0f`
        // and `kSoundFlagLodFallback` (`0x1000000`) is set.
        std::size_t snd_idx = static_cast<std::size_t>(sound_no);
        const wse::core::WseVec4 delta0 = position - listener_.position;
        float dist = delta0.length();
        float lod_gain = 1.0f;
        for (int step = 1; step < 5; ++step) {
            if ((sounds_[snd_idx].flags & wse::asset::kSoundFlagLodFallback) == 0U ||
                dist <= 15.0f || snd_idx + 1U >= sounds_.size()) {
                break;
            }
            ++snd_idx;
            dist *= 0.3f;
            lod_gain *= 0.35f;
        }

        const MixerSoundDef& snd = sounds_[snd_idx];
        if (snd.variants.empty()) {
            return -1;
        }
        const std::int32_t var_idx = pick_variant(snd, culture_filter);
        if (var_idx < 0) {
            return -1;
        }
        const std::int32_t sample_idx =
            snd.variants[static_cast<std::size_t>(var_idx)].sample_index;
        if (sample_idx < 0 || static_cast<std::size_t>(sample_idx) >= samples_.size()) {
            return -1;
        }
        const MixerSoundSample& smp = samples_[static_cast<std::size_t>(sample_idx)];
        if (smp.pcm == nullptr || !smp.pcm->valid()) {
            return -1;
        }

        const std::uint32_t eff_flags = smp.flags | extra_flags;
        const float dist_sq = delta0.dot(delta0);
        const std::int32_t prio =
            compute_sound_priority(eff_flags, dist_sq, config_.distance_priority_factor);

        const std::int32_t slot = allocate_channel_slot(prio);
        if (slot < 0) {
            return -1;
        }
        // A callback inside the active `flush_channel_ends()` loop may only reuse an
        // already-ended slot (no eviction while dispatching); cancel that slot's
        // stale queued end notification so it cannot wipe the new sound.
        if (dispatching_ends_) {
            for (std::size_t j = dispatch_cursor_ + 1U; j < pending_channel_ends_.size(); ++j) {
                if (pending_channel_ends_[j] == slot) {
                    pending_channel_ends_[j] = -1;
                }
            }
        }

        SoundChannelState& ch = channels_[static_cast<std::size_t>(slot)];
        ch = {};
        ch.active = true;
        ch.generation = ++next_generation_;
        ch.sample_index = sample_idx;
        ch.flags = eff_flags;
        ch.position = position;
        ch.pcm = smp.pcm;
        ch.priority = prio;
        ch.lod_gain = lod_gain;
        ch.volume = wse::asset::decode_sound_volume(eff_flags);
        ch.ignore_geometry = !config_.occlusion_filter ||
                             (config_.occlusion_max_distance > 0.0f &&
                              dist_sq > (config_.occlusion_max_distance *
                                         config_.occlusion_max_distance));

        // Random start over the whole sample: scale the 15-bit draw to the frame
        // count (a plain `% frames` could only reach the first 32767 frames).
        const std::size_t frames = smp.pcm->frame_count();
        if ((eff_flags & wse::asset::kSoundFlagRandomStartPos) != 0U && frames > 1U) {
            const auto draw = static_cast<std::uint64_t>(rng_.next_s15());
            ch.cursor_frame = static_cast<double>((draw * frames) >> 15U);
        } else {
            ch.cursor_frame = 0.0;
        }

        const bool keep_base_freq =
            config_.disable_frequency_variation ||
            ((eff_flags & wse::asset::kSoundFlagDisableFreqVariation) != 0U);
        ch.freq_multiplier =
            (!keep_base_freq && freq_multiplier > 0.05f) ? std::clamp(freq_multiplier, 0.25f, 4.0f)
                                                         : 1.0f;
        return slot;
    }

    // Deactivates a channel and queues its end notification (see `flush_channel_ends`).
    void end_channel(std::size_t idx) {
        SoundChannelState& ch = channels_[idx];
        if (!ch.active) {
            return;
        }
        ch.active = false;
        ch.pcm.reset();
        pending_channel_ends_.push_back(static_cast<std::int32_t>(idx));
    }

    // Delivers queued channel-end notifications once the mixer is consistent.
    // Re-entrant calls from the callback only queue more events, which this
    // outer loop then delivers in order.
    void flush_channel_ends() {
        if (dispatching_ends_) {
            return;
        }
        if (!on_channel_end_) {
            pending_channel_ends_.clear();
            return;
        }
        dispatching_ends_ = true;
        dispatch_cursor_ = 0U;
        struct Reset {
            WseAudioMixer& m;
            ~Reset() {
                m.dispatching_ends_ = false;
                m.dispatch_cursor_ = 0U;
                m.pending_channel_ends_.clear();
            }
        } reset{*this};
        for (std::size_t i = 0; i < pending_channel_ends_.size(); ++i) {
            dispatch_cursor_ = i;
            const std::int32_t channel_no = pending_channel_ends_[i];
            if (channel_no >= 0) {
                on_channel_end_(channel_no);
            }
        }
    }

    [[nodiscard]] std::int32_t pick_variant(const MixerSoundDef& snd,
                                            std::int32_t culture_filter) noexcept {
        if (culture_filter <= 0) {
            const std::int32_t n = static_cast<std::int32_t>(snd.variants.size());
            if (n <= 1) {
                return n == 1 ? 0 : -1;
            }
            return rng_.next_s15() % n;
        }
        std::int32_t eligible = 0;
        const std::uint32_t cf = static_cast<std::uint32_t>(culture_filter);
        for (const auto& v : snd.variants) {
            const std::uint32_t vc = v.flags & 0xFFU;
            if (vc == 0U || vc == cf) {
                ++eligible;
            }
        }
        if (eligible == 0) {
            return -1;
        }
        std::int32_t pick = (eligible > 1) ? (rng_.next_s15() % eligible) : 0;
        for (std::size_t i = 0; i < snd.variants.size(); ++i) {
            const std::uint32_t vc = snd.variants[i].flags & 0xFFU;
            if (vc == 0U || vc == cf) {
                if (pick-- == 0) {
                    return static_cast<std::int32_t>(i);
                }
            }
        }
        return -1;
    }

    [[nodiscard]] std::int32_t allocate_channel_slot(std::int32_t new_prio) {
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            if (!channels_[i].active) {
                return static_cast<std::int32_t>(i);
            }
        }
        // Evict lowest-importance (highest numerical priority; oldest generation tie-break).
        std::size_t worst = 0;
        for (std::size_t i = 1; i < channels_.size(); ++i) {
            if (channels_[i].priority > channels_[worst].priority ||
                (channels_[i].priority == channels_[worst].priority &&
                 channels_[i].generation < channels_[worst].generation)) {
                worst = i;
            }
        }
        if (channels_.empty() || new_prio > channels_[worst].priority) {
            return -1;
        }
        // Port deviation: a sound started from inside a channel-end callback never
        // evicts. Its caller would record the evicted slot as its own before the
        // queued end of the evicted sound is delivered, and the callback (which only
        // carries a channel number) would then look like the new sound ending.
        if (dispatching_ends_) {
            return -1;
        }
        // The end notification is queued and delivered by the caller's flush, after
        // the new sound owns the slot, so a re-entrant callback cannot steal it.
        end_channel(worst);
        return static_cast<std::int32_t>(worst);
    }

    // Reads one linearly interpolated frame. `wrap` interpolates the last frame
    // towards frame 0 (looping); otherwise the last frame is held.
    static void sample_frame(const float* data, std::size_t total, bool stereo, bool wrap,
                             double pos, float& out_l, float& out_r) noexcept {
        const auto i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = (i0 + 1U < total) ? i0 + 1U : (wrap ? 0U : i0);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        if (stereo) {
            const float l0 = data[i0 * 2U];
            const float r0 = data[i0 * 2U + 1U];
            out_l = l0 + (data[i1 * 2U] - l0) * frac;
            out_r = r0 + (data[i1 * 2U + 1U] - r0) * frac;
        } else {
            const float a = data[i0];
            out_l = out_r = a + (data[i1] - a) * frac;
        }
    }

    // Target L/R gains for one voice (distance rolloff, constant-power pan, occlusion).
    void voice_target_gains(const SoundChannelState& ch, bool occlusion_active, float& gl,
                            float& gr) const noexcept {
        gl = ch.volume * ch.lod_gain * config_.sound_volume;
        gr = gl;
        if ((ch.flags & wse::asset::kSoundFlag2D) == 0U) {
            const wse::core::WseVec4 d = ch.position - listener_.position;
            const float dist = d.length();
            if (config_.distance_filter) {
                const float min_d = config_.min_distance_3d;
                const float max_d = config_.max_distance_3d;
                const float atten =
                    (dist >= max_d)
                        ? (min_d / max_d) * std::max(0.0f, 1.0f - (dist - max_d) * 0.05f)
                        : (min_d / std::clamp(dist, min_d, max_d));
                gl *= atten;
                gr *= atten;
            }
            if (dist > 1e-4f) {
                const float inv = 1.0f / dist;
                const wse::core::WseVec4 dir{d.x * inv, d.y * inv, d.z * inv, 0.0f};
                const float pan = std::clamp(dir.dot(listener_.right), -1.0f, 1.0f);
                // Constant-power law: centre yields cos(pi/4) * sqrt(2) == 1.0.
                constexpr float kQuarterPi = 0.7853981633974483f;
                constexpr float kSqrt2 = 1.4142135623730951f;
                const float theta = (pan + 1.0f) * kQuarterPi;
                gl *= std::cos(theta) * kSqrt2;
                gr *= std::sin(theta) * kSqrt2;
            }
        }
        if (occlusion_active) {
            gl *= 0.45f;
            gr *= 0.45f;
        }
    }

    void mix_sound_channels(std::size_t frame_count, float* out) {
        // Audible set: top `max_channels` voices by FMOD priority (0 highest, 255 lowest),
        // ties to the older voice. Only the partition is needed, not a full sort.
        mix_order_.clear();
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            if (channels_[i].active && channels_[i].pcm != nullptr) {
                mix_order_.push_back(i);
            }
        }
        const auto louder = [&](std::size_t a, std::size_t b) {
            if (channels_[a].priority != channels_[b].priority) {
                return channels_[a].priority < channels_[b].priority;
            }
            return channels_[a].generation < channels_[b].generation;
        };
        const auto audible_limit = static_cast<std::size_t>(config_.max_channels);
        if (mix_order_.size() > audible_limit) {
            std::nth_element(mix_order_.begin(),
                             mix_order_.begin() + static_cast<std::ptrdiff_t>(audible_limit),
                             mix_order_.end(), louder);
        }

        // One-pole occlusion low-pass at ~1.5 kHz (alpha 0.18 at 48 kHz), scaled
        // to the output rate.
        constexpr float kTwoPi = 6.283185307179586f;
        const float lp_alpha =
            1.0f - std::exp(-kTwoPi * 1516.0f / static_cast<float>(output_sample_rate_));
        const float inv_frames = 1.0f / static_cast<float>(frame_count);

        for (std::size_t rank = 0; rank < mix_order_.size(); ++rank) {
            const std::size_t idx = mix_order_[rank];
            SoundChannelState& ch = channels_[idx];
            if (!ch.active || ch.pcm == nullptr) {
                continue;
            }
            const wse::asset::AudioPcmBuffer& buf = *ch.pcm;
            const std::size_t total = buf.frame_count();
            const double total_d = static_cast<double>(total);
            const double step = (static_cast<double>(buf.sample_rate) /
                                 static_cast<double>(output_sample_rate_)) *
                                static_cast<double>(ch.freq_multiplier);
            const bool looping = (ch.flags & wse::asset::kSoundFlagLooping) != 0U;

            if (rank >= audible_limit || total == 0U) {
                // Virtual voice: advance the cursor arithmetically, render nothing.
                ch.has_gain = false;
                ch.lp_primed = false;
                ch.cursor_frame += step * static_cast<double>(frame_count);
                if (ch.cursor_frame >= total_d) {
                    if (looping && total > 0U) {
                        ch.cursor_frame = std::fmod(ch.cursor_frame, total_d);
                    } else {
                        end_channel(idx);
                    }
                }
                continue;
            }

            const bool occlusion_active =
                config_.occlusion_filter && ch.occluded && !ch.ignore_geometry;
            float tgl = 0.0f;
            float tgr = 0.0f;
            voice_target_gains(ch, occlusion_active, tgl, tgr);
            const float gl0 = ch.has_gain ? ch.gain_l : tgl;
            const float gr0 = ch.has_gain ? ch.gain_r : tgr;
            const float dgl = (tgl - gl0) * inv_frames;
            const float dgr = (tgr - gr0) * inv_frames;

            const float* data = buf.samples.data();
            const bool stereo = buf.channels == 2U;
            if (!occlusion_active) {
                ch.lp_primed = false;
            }

            bool finished = false;
            for (std::size_t f = 0; f < frame_count; ++f) {
                if (ch.cursor_frame >= total_d) {
                    if (looping) {
                        ch.cursor_frame = std::fmod(ch.cursor_frame, total_d);
                    } else {
                        finished = true;
                        break;
                    }
                }
                float sl = 0.0f;
                float sr = 0.0f;
                sample_frame(data, total, stereo, looping, ch.cursor_frame, sl, sr);
                if (occlusion_active) {
                    if (!ch.lp_primed) {
                        ch.lp_l = sl;
                        ch.lp_r = sr;
                        ch.lp_primed = true;
                    }
                    ch.lp_l += lp_alpha * (sl - ch.lp_l);
                    ch.lp_r += lp_alpha * (sr - ch.lp_r);
                    sl = ch.lp_l;
                    sr = ch.lp_r;
                }
                const float ff = static_cast<float>(f);
                out[f * 2U + 0U] += sl * (gl0 + dgl * ff);
                out[f * 2U + 1U] += sr * (gr0 + dgr * ff);
                ch.cursor_frame += step;
            }
            ch.gain_l = tgl;
            ch.gain_r = tgr;
            ch.has_gain = true;
            if (finished) {
                end_channel(idx);
            }
        }
    }

    void start_music_slot(std::int32_t track_no, std::size_t slot_idx) {
        if (slot_idx >= 2U || track_no < 0 ||
            static_cast<std::size_t>(track_no) >= music_tracks_.size()) {
            return;
        }
        stop_music_slot(slot_idx);
        const auto tidx = static_cast<std::size_t>(track_no);
        const MixerMusicTrack& tr = music_tracks_[tidx];
        MusicSlotState& slot = music_slots_[slot_idx];
        slot.looping = (tr.norm.flags & wse::asset::kMusicFlagLooping) != 0U;
        if (tr.pcm != nullptr && tr.pcm->valid()) {
            slot.predecoded = tr.pcm;
            slot.active = true;
        } else if (music_opener_) {
            slot.stream = music_opener_(tidx);
            if (slot.stream != nullptr) {
                slot.chunk_rate = slot.stream->sample_rate();
                slot.chunk_channels = slot.stream->channels();
                slot.active = (slot.chunk_rate > 0U &&
                               (slot.chunk_channels == 1U || slot.chunk_channels == 2U));
            }
        }
        if (slot.active) {
            slot.track_no = track_no;
            last_played_time_[tidx] = now_seconds_;
        } else {
            slot = {};
        }
    }

    void stop_music_slot(std::size_t slot_idx) {
        if (slot_idx < 2U) {
            music_slots_[slot_idx] = {};
        }
    }

    [[nodiscard]] bool music_track_eligible(std::size_t i, std::int32_t persist_track) const {
        if (static_cast<std::int32_t>(i) == persist_track) {
            return false;
        }
        const auto& tr = music_tracks_[i];
        if ((tr.norm.flags & situation_flags_) == 0U) {
            return false;
        }
        return culture_flags_ == 0U || (tr.norm.culture_flags & culture_flags_) != 0U;
    }

    [[nodiscard]] std::int32_t select_random_music_track(double now_s) noexcept {
        if (situation_flags_ == 0U || music_tracks_.empty()) {
            return -1;
        }
        std::int32_t persist_track = -1;
        if (active_music_track_ >= 0 &&
            static_cast<std::size_t>(active_music_track_) < music_tracks_.size()) {
            const auto& cur = music_tracks_[static_cast<std::size_t>(active_music_track_)];
            if ((cur.norm.continue_flags & situation_flags_) != 0U &&
                (culture_flags_ == 0U ||
                 (cur.norm.continue_culture_flags & culture_flags_) != 0U)) {
                persist_track = active_music_track_;
            }
        }

        // Recency weight `(now + 1 - last_played) + 500.0` (`0x1402910c0`).
        const double time_plus_one = now_s + 1.0;
        double total_weight = 0.0;
        for (std::size_t i = 0; i < music_tracks_.size(); ++i) {
            if (!music_track_eligible(i, persist_track)) {
                continue;
            }
            const double w = (time_plus_one - last_played_time_[i]) + 500.0;
            if (w > 0.0) {
                total_weight += w;
            }
        }
        if (total_weight <= 0.0) {
            return -1;
        }

        for (int attempt = 0; attempt < 8; ++attempt) {
            double target = static_cast<double>(rng_.next_unit()) * total_weight;
            for (std::size_t i = 0; i < music_tracks_.size(); ++i) {
                if (!music_track_eligible(i, persist_track)) {
                    continue;
                }
                const double w = (time_plus_one - last_played_time_[i]) + 500.0;
                if (w > 0.0) {
                    target -= w;
                    if (target <= 0.0) {
                        const auto& tr = music_tracks_[i];
                        const bool immediate =
                            (persist_track < 0) ||
                            ((tr.norm.flags & wse::asset::kMusicFlagImmediate) != 0U) ||
                            (rng_.next_unit() >= 0.01f);
                        return immediate ? static_cast<std::int32_t>(i) : -1;
                    }
                }
            }
        }
        return -1;
    }

    // Mirrors `mbMusicManager::update` (`0x1402910c0`).
    void update_music_scheduler(double now_s) {
        if (music_slots_[0].active) {
            const auto cur_idx = static_cast<std::size_t>(music_slots_[0].track_no);
            const auto& cur = music_tracks_[cur_idx];
            const bool persist = (cur.norm.flags & wse::asset::kMusicFlagPersist) != 0U;
            if (requested_track_no_ >= 0 && requested_track_option_ > 0 &&
                requested_track_no_ == music_slots_[0].track_no) {
                // Asking for the track that is already playing keeps it playing
                // (and cancels a pending fade) instead of stopping it.
                requested_track_no_ = -1;
                fading_out_ = false;
            } else if (requested_track_no_ >= 0 && requested_track_option_ == 2 && !persist) {
                stop_music_slot(0);
            } else if (!fading_out_ && !persist) {
                bool should_fade = false;
                if (requested_track_no_ >= 0 && requested_track_option_ == 1) {
                    should_fade = true;
                } else if (situation_flags_ != 0U &&
                           ((cur.norm.continue_flags & situation_flags_) == 0U ||
                            (culture_flags_ != 0U &&
                             (cur.norm.continue_culture_flags & culture_flags_) == 0U))) {
                    should_fade = true;
                }
                if (should_fade) {
                    fading_out_ = true;
                    fade_start_time_ = now_s;
                }
            }
        }

        if (!music_slots_[0].active) {
            fading_out_ = false;
            std::int32_t next_track = requested_track_no_;
            if (next_track >= 0) {
                requested_track_no_ = -1;
            } else {
                next_track = select_random_music_track(now_s);
            }
            if (next_track >= 0) {
                start_music_slot(next_track, 0U);
                active_music_track_ = next_track;
                if ((music_tracks_[static_cast<std::size_t>(next_track)].norm.flags &
                     wse::asset::kMusicFlagPersist) != 0U) {
                    situation_flags_ = 0U;
                }
            }
        }

        if (!music_slots_[1].active && queued_cue_track_no_ >= 0) {
            start_music_slot(queued_cue_track_no_, 1U);
            queued_cue_track_no_ = -1;
        }
    }

    // Pulls the next chunk of a streamed slot, keeping the last frame of the
    // current chunk at index 0. Returns false at end of stream.
    [[nodiscard]] static bool refill_music_stream(MusicSlotState& slot) {
        std::size_t got = slot.stream->read_frames(kMusicChunkFrames, slot.read_buf);
        if (got == 0U && slot.looping && slot.stream->seek_start()) {
            got = slot.stream->read_frames(kMusicChunkFrames, slot.read_buf);
        }
        if (got == 0U) {
            return false;
        }
        const std::size_t chs = slot.chunk_channels;
        const std::size_t avail = slot.chunk_buf.size() / chs;
        if (avail > 0U) {
            for (std::size_t k = 0; k < chs; ++k) {
                slot.chunk_buf[k] = slot.chunk_buf[(avail - 1U) * chs + k];
            }
            slot.chunk_buf.resize(chs);
            slot.chunk_cursor -= static_cast<double>(avail - 1U);
        } else {
            slot.chunk_buf.clear();
        }
        slot.chunk_buf.insert(slot.chunk_buf.end(), slot.read_buf.begin(),
                              slot.read_buf.begin() + static_cast<std::ptrdiff_t>(got * chs));
        return true;
    }

    void mix_music_slots(std::size_t frame_count, float* out) {
        const double dt =
            static_cast<double>(frame_count) / static_cast<double>(output_sample_rate_);
        const auto fade_at = [&](double t) {
            if (!fading_out_) {
                return 1.0f;
            }
            return 1.0f - static_cast<float>(
                              std::clamp((t - fade_start_time_) / kMusicFadeSeconds, 0.0, 1.0));
        };
        const float cue_duck = music_slots_[1].active ? 0.5f : 1.0f;
        const float inv_frames = 1.0f / static_cast<float>(frame_count);
        for (std::size_t s = 0; s < 2U; ++s) {
            MusicSlotState& slot = music_slots_[s];
            if (!slot.active) {
                continue;
            }
            // Gain ramps from the previous block's value to this block's end value.
            const float target = config_.music_volume *
                                 ((s == 0U) ? (fade_at(now_seconds_ + dt) * cue_duck) : 1.0f);
            const float g0 = slot.has_gain ? slot.gain : target;
            const float dg = (target - g0) * inv_frames;
            bool eof = false;
            if (slot.predecoded != nullptr) {
                const auto& buf = *slot.predecoded;
                const std::size_t total = buf.frame_count();
                const double total_d = static_cast<double>(total);
                const double step = static_cast<double>(buf.sample_rate) /
                                    static_cast<double>(output_sample_rate_);
                const float* data = buf.samples.data();
                const bool stereo = buf.channels == 2U;
                for (std::size_t f = 0; f < frame_count; ++f) {
                    if (slot.pcm_cursor >= total_d) {
                        if (slot.looping && total > 0U) {
                            slot.pcm_cursor = std::fmod(slot.pcm_cursor, total_d);
                        } else {
                            eof = true;
                            break;
                        }
                    }
                    float sl = 0.0f;
                    float sr = 0.0f;
                    sample_frame(data, total, stereo, slot.looping, slot.pcm_cursor, sl, sr);
                    const float g = g0 + dg * static_cast<float>(f);
                    out[f * 2U + 0U] += sl * g;
                    out[f * 2U + 1U] += sr * g;
                    slot.pcm_cursor += step;
                }
            } else if (slot.stream != nullptr) {
                const double step = static_cast<double>(slot.chunk_rate) /
                                    static_cast<double>(output_sample_rate_);
                const std::size_t chs = slot.chunk_channels;
                const bool stereo = chs == 2U;
                for (std::size_t f = 0; f < frame_count; ++f) {
                    auto i0 = static_cast<std::size_t>(slot.chunk_cursor);
                    while (!slot.stream_eof && i0 + 1U >= slot.chunk_buf.size() / chs) {
                        if (!refill_music_stream(slot)) {
                            slot.stream_eof = true;
                        }
                        i0 = static_cast<std::size_t>(slot.chunk_cursor);
                    }
                    const std::size_t n = slot.chunk_buf.size() / chs;
                    if (i0 >= n) {
                        eof = true;
                        break;
                    }
                    float sl = 0.0f;
                    float sr = 0.0f;
                    sample_frame(slot.chunk_buf.data(), n, stereo, false, slot.chunk_cursor, sl,
                                 sr);
                    const float g = g0 + dg * static_cast<float>(f);
                    out[f * 2U + 0U] += sl * g;
                    out[f * 2U + 1U] += sr * g;
                    slot.chunk_cursor += step;
                }
            }
            slot.gain = target;
            slot.has_gain = true;
            if (eof) {
                stop_music_slot(s);
            }
        }
    }

    AudioConfig config_{};
    std::uint32_t output_sample_rate_ = 48000U;
    AudioListener listener_{};
    AudioPrng rng_{};
    ChannelEndCallback on_channel_end_{};
    MusicStreamOpener music_opener_{};

    std::vector<MixerSoundSample> samples_{};
    std::vector<MixerSoundDef> sounds_{};
    std::vector<SoundChannelState> channels_{};
    std::uint64_t next_generation_ = 0U;
    std::int32_t last_sound_channel_ = -1;
    std::vector<std::int32_t> pending_channel_ends_{};
    bool dispatching_ends_ = false;
    std::size_t dispatch_cursor_ = 0U;
    std::vector<std::size_t> mix_order_{};
    std::vector<float> pump_buf_{};

    std::vector<MixerMusicTrack> music_tracks_{};
    std::vector<double> last_played_time_{};
    MusicSlotState music_slots_[2]{};
    std::uint32_t situation_flags_ = 0U;
    std::uint32_t culture_flags_ = 0U;
    std::int32_t active_music_track_ = -1;
    std::int32_t requested_track_no_ = -1;
    std::int32_t requested_track_option_ = 0;
    std::int32_t queued_cue_track_no_ = -1;
    bool fading_out_ = false;
    double fade_start_time_ = 0.0;
    double now_seconds_ = 0.0;
    double last_update_time_ = -2000.0;
};

}  // namespace wse::platform
