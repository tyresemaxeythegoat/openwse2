// Platform seam: audio device abstraction and clean-room 2D/3D stereo mixer +
// music scheduler (Slice P6, docs/PLATFORM_PLAN.md).
//
// Port infrastructure (`AudioDevice`, `NullAudioDevice`, `CaptureAudioDevice`)
// keeps `<SDL3/SDL.h>` out of engine headers; `WseAudioMixer` reconstructs
// WSE2 v1.1.5.5 `mbSoundManager` / `mbMusicManager` semantics from PDB/binary
// evidence:
//   - mbSoundSample::playSound            0x140331cb0 (priority calc, 2D/looping -> 0)
//   - mbSoundSample::readStream           0x1403317c0 (3D min=2.0f, max=80.0f)
//   - mbSoundManager::playSoundAtPosition 0x140332ac0 (0x1000000 LOD fallback, culture filter)
//   - mbSoundManager::setChannelPropertiesAndPlay 0x140332d80 (vol / 15, random start, freq)
//   - mbSoundManager::setTransform        0x140332970 (listener attributes: forward=-u, up=f)
//   - mbSoundManager::updateOcclusionDistances 0x140333260 (0.9 / 1.1 hysteresis around 80m)
//   - mbSoundManager::update              0x140333490 (virtual channels + channel-end callback)
//   - mbSoundManager::stopSound           0x140332690
//   - mbSoundManager::getSoundProgress    0x1403326d0 (0..100 or -1)
//   - mbSoundManager::stopAllSounds       0x140332770 (include non-looping bool)
//   - mbMusicManager::frameMove / update  0x140290c40 / 0x140290f60 (0.5s tick, 5.0s fade, 500.0 recency)
//   - mbMusicManager::startPlaying / stopPlaying 0x140290e60 / 0x140290f20

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
        spec_ = s;
        open_ = (s.sample_rate > 0U && s.channels == 2U);
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

// Defaults match `rglConfig::Sound` globals verified across v1.1.5.5
// `rglConfig::load` (`0x140264580`), `mbSoundManager::initializeFmod` (`0x140332f80`),
// `mbSoundSample::readStream` (`0x1403317c0`), and `mbSoundManager::updateOcclusionDistances`
// (`0x140333260`): `bSound=1`, `bMusic=1`, `fSoundVolume=1.0f`, `fMusicVolume=0.4f`,
// `iMaxNumChannels=64`, `iMaxNumVirtualChannels=100`, `fDistancePriorityFactor=0.47f`,
// `fOcclusionMaxDistance=80.0f`, 3D min=2.0f / max=80.0f, `bUseSoundOcclusion=1`,
// `bDistanceFilter=1`, `bHrtfFilter=1`, `bDisableFrequencyVariation=0`.
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

struct AudioListener {
    wse::core::WseVec4 position{};
    wse::core::WseVec4 velocity{};
    wse::core::WseVec4 right{1.0f, 0.0f, 0.0f, 0.0f};
    wse::core::WseVec4 forward{0.0f, 1.0f, 0.0f, 0.0f};
    wse::core::WseVec4 up{0.0f, 0.0f, 1.0f, 0.0f};
};

// Reconstructs `mbSoundManager::setTransform` (`0x140332970`): Warband passes a camera
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
    std::uint32_t flags = 0;  // Low byte holds optional culture filter (`0x140332c7e`).
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

// Mirrors `mbSoundSample::playSound` (`0x140331cb0`) priority calculation:
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

// Reconstructs deterministic MSVCRT `rand()` sequence (`0x140332cae`, `0x140332ed5`,
// `0x1402913e0`: `seed = seed * 0x343fd + 0x269ec3`, `(seed >> 16) & 0x7fff`).
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

// Reconstructs `mbAgent::playSound` (`0x140057010`) human voice pitch variation hash.
[[nodiscard]] inline float compute_agent_voice_pitch(std::int32_t agent_no,
                                                     std::int32_t troop_no,
                                                     std::int32_t skin_or_dna) noexcept {
    std::int32_t acc = (agent_no + 29) * 0x14fd + troop_no * 0x76ef + skin_or_dna * 0x244ad;
    acc %= 0x1b41;
    acc = (acc * 0x51f) % 0x367;
    const float unit = static_cast<float>(acc) / 871.0f;
    const float delta = (unit - 0.5f) * 0.14f;
    return 1.0f + std::clamp(delta, -0.15f, 0.15f);
}

class WseAudioMixer {
public:
    using ChannelEndCallback = std::function<void(std::int32_t channel_no)>;
    using MusicStreamOpener =
        std::function<std::unique_ptr<wse::asset::AudioStreamDecoder>(std::size_t track_index)>;

    explicit WseAudioMixer(AudioConfig config = {}) : config_(config) {
        reset_channels();
    }

    void set_config(const AudioConfig& config) {
        config_ = config;
        reset_channels();
    }
    [[nodiscard]] const AudioConfig& config() const noexcept { return config_; }

    void set_seed(std::uint32_t seed) noexcept { rng_.state = seed; }
    void set_output_sample_rate(std::uint32_t sr) noexcept {
        output_sample_rate_ = (sr > 0U && sr <= wse::asset::kMaxAudioSampleRate) ? sr : 48000U;
    }
    [[nodiscard]] std::uint32_t output_sample_rate() const noexcept { return output_sample_rate_; }

    void set_listener(const AudioListener& listener) noexcept { listener_ = listener; }
    [[nodiscard]] const AudioListener& listener() const noexcept { return listener_; }

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
        last_played_time_.assign(music_tracks_.size(), 0.0f);
    }

    // Plays a sound event mirroring `mbSoundManager::playSoundAtPosition` (`0x140332ac0`).
    // Returns channel index `[0, max_virtual_channels)` or `-1` when suppressed/missing.
    std::int32_t play_sound_at_position(std::int32_t sound_no,
                                        const wse::core::WseVec4& position,
                                        const wse::core::WseVec4& velocity = {},
                                        std::uint32_t extra_flags = 0U,
                                        float freq_multiplier = 1.0f,
                                        std::int32_t culture_filter = 0) {
        (void)velocity;
        if (!config_.sound_enabled || config_.sound_volume < 0.02f) {
            return -1;
        }
        if (sound_no < 0 || static_cast<std::size_t>(sound_no) >= sounds_.size()) {
            return -1;
        }

        // `0x140332be0`: follow up to 4 LOD fallback sounds when `distance > 15.0f` and
        // `kSoundFlagLodFallback` (`0x1000000`) is set.
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
        const std::int32_t sample_idx = snd.variants[static_cast<std::size_t>(var_idx)].sample_index;
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

        if ((eff_flags & wse::asset::kSoundFlagRandomStartPos) != 0U &&
            smp.pcm->frame_count() > 1U) {
            const std::size_t start_frame =
                static_cast<std::size_t>(rng_.next_s15()) % smp.pcm->frame_count();
            ch.cursor_frame = static_cast<double>(start_frame);
        } else {
            ch.cursor_frame = 0.0;
        }

        const bool keep_base_freq =
            config_.disable_frequency_variation ||
            ((eff_flags & wse::asset::kSoundFlagDisableFreqVariation) != 0U);
        ch.freq_multiplier =
            (!keep_base_freq && freq_multiplier > 0.05f) ? std::clamp(freq_multiplier, 0.25f, 4.0f)
                                                         : 1.0f;

        last_sound_channel_ = slot;
        return slot;
    }

    // Plays 2D sound at listener origin (`mbGame::playSound` `0x1400e3900`).
    std::int32_t play_sound_2d(std::int32_t sound_no, std::uint32_t extra_flags = 0U) {
        return play_sound_at_position(sound_no, listener_.position, {},
                                      extra_flags | wse::asset::kSoundFlag2D);
    }

    void stop_sound(std::int32_t channel_no) {
        if (channel_no < 0 || static_cast<std::size_t>(channel_no) >= channels_.size()) {
            return;
        }
        SoundChannelState& ch = channels_[static_cast<std::size_t>(channel_no)];
        if (!ch.active) {
            return;
        }
        ch.active = false;
        ch.pcm.reset();
        if (on_channel_end_) {
            on_channel_end_(channel_no);
        }
    }

    // Mirrors `mbSoundManager::stopAllSounds` (`0x140332770`):
    // `include_non_looping == false` stops looping channels only; `true` stops all sound channels.
    void stop_all_sounds(bool include_non_looping) {
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            SoundChannelState& ch = channels_[i];
            if (!ch.active) {
                continue;
            }
            const bool is_looping = (ch.flags & wse::asset::kSoundFlagLooping) != 0U;
            if (!include_non_looping && !is_looping) {
                continue;
            }
            ch.active = false;
            ch.pcm.reset();
            if (on_channel_end_) {
                on_channel_end_(static_cast<std::int32_t>(i));
            }
        }
    }

    // Mirrors `mbSoundManager::getSoundProgress` (`0x1403326d0`): returns `0..100` or `-1`.
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

    // Mirrors `mbSoundManager::updateOcclusionDistances` (`0x140333260`):
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

    // Music control (`0x140290c40`, `0x140290e60`, `0x140290f60`).
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

    // Runs `mbMusicManager::frameMove` (`0x140290c40`): `0.5s` update tick and `5.0s` fade.
    void update(float now_seconds) {
        now_seconds_ = now_seconds;
        if (config_.music_enabled && fading_out_) {
            const float elapsed = now_seconds - fade_start_time_;
            if (elapsed > 5.0f) {
                stop_music_slot(0);
                fading_out_ = false;
            }
        }
        if ((now_seconds - last_update_time_) > 0.5f || last_update_time_ < -1000.0f) {
            if (config_.music_enabled) {
                update_music_scheduler(now_seconds);
            }
            update_occlusion_distances();
            last_update_time_ = now_seconds;
        }
    }

    // Mixes `frame_count` stereo float frames (`frame_count * 2` floats), updates cursors
    // and channel expirations, and writes clamped `[-1, 1]` interleaved stereo output.
    void mix_frames(std::size_t frame_count, std::vector<float>& out_interleaved_stereo) {
        out_interleaved_stereo.assign(frame_count * 2U, 0.0f);
        if (frame_count == 0U) {
            return;
        }

        if (config_.sound_enabled && config_.sound_volume >= 0.02f) {
            mix_sound_channels(frame_count, out_interleaved_stereo);
        }
        if (config_.music_enabled && config_.music_volume > 0.02f) {
            mix_music_slots(frame_count, out_interleaved_stereo);
        }

        for (float& s : out_interleaved_stereo) {
            s = std::clamp(s, -1.0f, 1.0f);
        }

        const float dt =
            static_cast<float>(frame_count) / static_cast<float>(output_sample_rate_);
        now_seconds_ += dt;
    }

    bool pump(AudioDevice& device, std::size_t frame_count) {
        if (!device.is_open()) {
            return false;
        }
        set_output_sample_rate(device.spec().sample_rate);
        update(now_seconds_);
        std::vector<float> buf;
        mix_frames(frame_count, buf);
        return device.submit_frames(buf);
    }

private:
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
        float lp_l = 0.0f;
        float lp_r = 0.0f;
    };

    struct MusicSlotState {
        bool active = false;
        std::int32_t track_no = -1;
        bool looping = false;
        std::shared_ptr<const wse::asset::AudioPcmBuffer> predecoded{};
        std::unique_ptr<wse::asset::AudioStreamDecoder> stream{};
        std::vector<float> chunk_buf{};
        std::size_t chunk_rate = 48000U;
        std::uint16_t chunk_channels = 2U;
        double chunk_cursor = 0.0;
        double pcm_cursor = 0.0;
    };

    void reset_channels() {
        const std::size_t vcount = static_cast<std::size_t>(
            std::clamp(config_.max_virtual_channels, 1, 256));
        channels_.assign(vcount, SoundChannelState{});
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
        bool found = false;
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            if (!found || channels_[i].priority > channels_[worst].priority ||
                (channels_[i].priority == channels_[worst].priority &&
                 channels_[i].generation < channels_[worst].generation)) {
                worst = i;
                found = true;
            }
        }
        if (!found || new_prio > channels_[worst].priority) {
            return -1;
        }
        stop_sound(static_cast<std::int32_t>(worst));
        return static_cast<std::int32_t>(worst);
    }

    static void sample_pcm_frame(const wse::asset::AudioPcmBuffer& buf, double frame_pos,
                                 float& out_l, float& out_r) noexcept {
        const std::size_t total = buf.frame_count();
        if (total == 0U) {
            out_l = 0.0f;
            out_r = 0.0f;
            return;
        }
        const std::size_t i0 = std::min<std::size_t>(
            static_cast<std::size_t>(std::max(0.0, frame_pos)), total - 1U);
        const std::size_t i1 = std::min<std::size_t>(i0 + 1U, total - 1U);
        const float frac = static_cast<float>(std::clamp(frame_pos - static_cast<double>(i0),
                                                         0.0, 1.0));
        if (buf.channels == 1U) {
            const float a = buf.samples[i0];
            const float b = buf.samples[i1];
            const float v = a + (b - a) * frac;
            out_l = v;
            out_r = v;
        } else {
            const float l0 = buf.samples[i0 * 2U + 0U];
            const float r0 = buf.samples[i0 * 2U + 1U];
            const float l1 = buf.samples[i1 * 2U + 0U];
            const float r1 = buf.samples[i1 * 2U + 1U];
            out_l = l0 + (l1 - l0) * frac;
            out_r = r0 + (r1 - r0) * frac;
        }
    }

    void mix_sound_channels(std::size_t frame_count, std::vector<float>& out) {
        // Select top `max_channels` audible voices by FMOD priority (0 highest, 255 lowest).
        std::vector<std::size_t> active_indices;
        active_indices.reserve(channels_.size());
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            if (channels_[i].active && channels_[i].pcm != nullptr) {
                active_indices.push_back(i);
            }
        }
        std::sort(active_indices.begin(), active_indices.end(),
                  [&](std::size_t a, std::size_t b) {
                      if (channels_[a].priority != channels_[b].priority) {
                          return channels_[a].priority < channels_[b].priority;
                      }
                      return channels_[a].generation < channels_[b].generation;
                  });
        const std::size_t audible_limit =
            static_cast<std::size_t>(std::max(1, config_.max_channels));

        for (std::size_t rank = 0; rank < active_indices.size(); ++rank) {
            const std::size_t idx = active_indices[rank];
            SoundChannelState& ch = channels_[idx];
            const bool is_audible = (rank < audible_limit);
            const auto& buf = *ch.pcm;
            const std::size_t total_frames = buf.frame_count();
            const double step =
                (static_cast<double>(buf.sample_rate) /
                 static_cast<double>(output_sample_rate_)) *
                static_cast<double>(ch.freq_multiplier);
            const bool looping = (ch.flags & wse::asset::kSoundFlagLooping) != 0U;

            float gain_l = ch.volume * ch.lod_gain * config_.sound_volume;
            float gain_r = gain_l;
            if ((ch.flags & wse::asset::kSoundFlag2D) == 0U) {
                const wse::core::WseVec4 d = ch.position - listener_.position;
                const float dist = d.length();
                if (config_.distance_filter) {
                    const float clamped =
                        std::clamp(dist, config_.min_distance_3d, config_.max_distance_3d);
                    const float atten =
                        (dist >= config_.max_distance_3d)
                            ? (config_.min_distance_3d / config_.max_distance_3d) *
                                  std::max(0.0f, 1.0f - (dist - config_.max_distance_3d) * 0.05f)
                            : (config_.min_distance_3d / clamped);
                    gain_l *= atten;
                    gain_r *= atten;
                }
                if (dist > 1e-4f) {
                    const float inv = 1.0f / dist;
                    const wse::core::WseVec4 dir{d.x * inv, d.y * inv, d.z * inv, 0.0f};
                    const float pan = std::clamp(dir.dot(listener_.right), -1.0f, 1.0f);
                    // Constant-power law: center yields sqrt(0.5) * sqrt(2) == 1.0 for 2D/center.
                    constexpr float kQuarterPi = 0.7853981633974483f;
                    const float theta = (pan + 1.0f) * kQuarterPi;
                    constexpr float kSqrt2 = 1.4142135623730951f;
                    gain_l *= std::cos(theta) * kSqrt2;
                    gain_r *= std::sin(theta) * kSqrt2;
                }
            }
            if (ch.occluded && !ch.ignore_geometry) {
                gain_l *= 0.45f;
                gain_r *= 0.45f;
            }

            bool finished = false;
            for (std::size_t f = 0; f < frame_count; ++f) {
                if (ch.cursor_frame >= static_cast<double>(total_frames)) {
                    if (looping && total_frames > 0U) {
                        ch.cursor_frame =
                            std::fmod(ch.cursor_frame, static_cast<double>(total_frames));
                    } else {
                        finished = true;
                        break;
                    }
                }
                if (is_audible) {
                    float sl = 0.0f;
                    float sr = 0.0f;
                    sample_pcm_frame(buf, ch.cursor_frame, sl, sr);
                    if (ch.occluded && !ch.ignore_geometry && config_.occlusion_filter) {
                        constexpr float kAlpha = 0.18f;
                        ch.lp_l += kAlpha * (sl - ch.lp_l);
                        ch.lp_r += kAlpha * (sr - ch.lp_r);
                        sl = ch.lp_l;
                        sr = ch.lp_r;
                    }
                    out[f * 2U + 0U] += sl * gain_l;
                    out[f * 2U + 1U] += sr * gain_r;
                }
                ch.cursor_frame += step;
            }
            if (finished) {
                stop_sound(static_cast<std::int32_t>(idx));
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
        slot = {};
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
            slot.pcm_cursor = 0.0;
            slot.chunk_cursor = 0.0;
            last_played_time_[tidx] = now_seconds_;
        }
    }

    void stop_music_slot(std::size_t slot_idx) {
        if (slot_idx < 2U) {
            music_slots_[slot_idx] = {};
        }
    }

    [[nodiscard]] std::int32_t select_random_music_track(float now_s) noexcept {
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

        double total_weight = 0.0;
        const float time_plus_one = now_s + 1.0f;
        for (std::size_t i = 0; i < music_tracks_.size(); ++i) {
            if (static_cast<std::int32_t>(i) == persist_track) {
                continue;
            }
            const auto& tr = music_tracks_[i];
            if ((tr.norm.flags & situation_flags_) == 0U) {
                continue;
            }
            if (culture_flags_ != 0U && (tr.norm.culture_flags & culture_flags_) == 0U) {
                continue;
            }
            const float w = (time_plus_one - last_played_time_[i]) + 500.0f;
            if (w > 0.0f) {
                total_weight += static_cast<double>(w);
            }
        }
        if (total_weight <= 0.0) {
            return -1;
        }

        for (int attempt = 0; attempt < 8; ++attempt) {
            double target = static_cast<double>(rng_.next_unit()) * total_weight;
            for (std::size_t i = 0; i < music_tracks_.size(); ++i) {
                if (static_cast<std::int32_t>(i) == persist_track) {
                    continue;
                }
                const auto& tr = music_tracks_[i];
                if ((tr.norm.flags & situation_flags_) == 0U) {
                    continue;
                }
                if (culture_flags_ != 0U && (tr.norm.culture_flags & culture_flags_) == 0U) {
                    continue;
                }
                const float w = (time_plus_one - last_played_time_[i]) + 500.0f;
                if (w > 0.0f) {
                    target -= static_cast<double>(w);
                    if (target <= 0.0) {
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

    // Mirrors `mbMusicManager::update` (`0x140290f60`).
    void update_music_scheduler(float now_s) {
        // Active slot 0 update + interruption / natural continuation check.
        if (music_slots_[0].active) {
            const auto cur_idx = static_cast<std::size_t>(music_slots_[0].track_no);
            const auto& cur = music_tracks_[cur_idx];
            if (requested_track_no_ >= 0 && requested_track_option_ == 2 &&
                (cur.norm.flags & wse::asset::kMusicFlagPersist) == 0U) {
                stop_music_slot(0);
            } else if (!fading_out_ && (cur.norm.flags & wse::asset::kMusicFlagPersist) == 0U) {
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
                if (next_track == active_music_track_ && requested_track_option_ > 0) {
                    requested_track_no_ = -1;
                    next_track = -1;
                } else {
                    requested_track_no_ = -1;
                }
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

    void mix_music_slots(std::size_t frame_count, std::vector<float>& out) {
        const float slot0_fade =
            fading_out_
                ? std::max(0.0f, 1.0f - std::clamp((now_seconds_ - fade_start_time_) / 5.0f,
                                                   0.0f, 1.0f))
                : 1.0f;
        const float cue_atten = music_slots_[1].active ? 0.5f : 1.0f;
        for (std::size_t s = 0; s < 2U; ++s) {
            MusicSlotState& slot = music_slots_[s];
            if (!slot.active) {
                continue;
            }
            const float gain =
                config_.music_volume * ((s == 0U) ? (slot0_fade * cue_atten) : 1.0f);
            if (slot.predecoded != nullptr) {
                const auto& buf = *slot.predecoded;
                const std::size_t total = buf.frame_count();
                const double step = static_cast<double>(buf.sample_rate) /
                                    static_cast<double>(output_sample_rate_);
                bool eof = false;
                for (std::size_t f = 0; f < frame_count; ++f) {
                    if (slot.pcm_cursor >= static_cast<double>(total)) {
                        if (slot.looping && total > 0U) {
                            slot.pcm_cursor =
                                std::fmod(slot.pcm_cursor, static_cast<double>(total));
                        } else {
                            eof = true;
                            break;
                        }
                    }
                    float sl = 0.0f;
                    float sr = 0.0f;
                    sample_pcm_frame(buf, slot.pcm_cursor, sl, sr);
                    out[f * 2U + 0U] += sl * gain;
                    out[f * 2U + 1U] += sr * gain;
                    slot.pcm_cursor += step;
                }
                if (eof) {
                    stop_music_slot(s);
                }
            } else if (slot.stream != nullptr) {
                const double step = static_cast<double>(slot.chunk_rate) /
                                    static_cast<double>(output_sample_rate_);
                bool eof = false;
                for (std::size_t f = 0; f < frame_count; ++f) {
                    const std::size_t avail =
                        (slot.chunk_channels > 0U)
                            ? (slot.chunk_buf.size() / static_cast<std::size_t>(slot.chunk_channels))
                            : 0U;
                    if (slot.chunk_cursor >= static_cast<double>(avail)) {
                        std::vector<float> next;
                        const std::size_t got = slot.stream->read_frames(2048U, next);
                        if (got == 0U) {
                            if (slot.looping && slot.stream->seek_start()) {
                                const std::size_t got2 = slot.stream->read_frames(2048U, next);
                                if (got2 == 0U) {
                                    eof = true;
                                    break;
                                }
                            } else {
                                eof = true;
                                break;
                            }
                        }
                        slot.chunk_buf = std::move(next);
                        slot.chunk_cursor =
                            std::max(0.0, slot.chunk_cursor - static_cast<double>(avail));
                    }
                    const std::size_t cur_frames =
                        slot.chunk_buf.size() / static_cast<std::size_t>(slot.chunk_channels);
                    if (cur_frames == 0U) {
                        eof = true;
                        break;
                    }
                    const std::size_t i0 = std::min<std::size_t>(
                        static_cast<std::size_t>(slot.chunk_cursor), cur_frames - 1U);
                    const std::size_t i1 = std::min<std::size_t>(i0 + 1U, cur_frames - 1U);
                    const float frac = static_cast<float>(
                        std::clamp(slot.chunk_cursor - static_cast<double>(i0), 0.0, 1.0));
                    float sl = 0.0f;
                    float sr = 0.0f;
                    if (slot.chunk_channels == 1U) {
                        const float a = slot.chunk_buf[i0];
                        const float b = slot.chunk_buf[i1];
                        sl = sr = a + (b - a) * frac;
                    } else {
                        const float l0 = slot.chunk_buf[i0 * 2U + 0U];
                        const float l1 = slot.chunk_buf[i1 * 2U + 0U];
                        const float r0 = slot.chunk_buf[i0 * 2U + 1U];
                        const float r1 = slot.chunk_buf[i1 * 2U + 1U];
                        sl = l0 + (l1 - l0) * frac;
                        sr = r0 + (r1 - r0) * frac;
                    }
                    out[f * 2U + 0U] += sl * gain;
                    out[f * 2U + 1U] += sr * gain;
                    slot.chunk_cursor += step;
                }
                if (eof) {
                    stop_music_slot(s);
                }
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

    std::vector<MixerMusicTrack> music_tracks_{};
    std::vector<float> last_played_time_{};
    MusicSlotState music_slots_[2]{};
    std::uint32_t situation_flags_ = 0U;
    std::uint32_t culture_flags_ = 0U;
    std::int32_t active_music_track_ = -1;
    std::int32_t requested_track_no_ = -1;
    std::int32_t requested_track_option_ = 0;
    std::int32_t queued_cue_track_no_ = -1;
    bool fading_out_ = false;
    float fade_start_time_ = 0.0f;
    float now_seconds_ = 0.0f;
    float last_update_time_ = -2000.0f;
};

}  // namespace wse::platform
