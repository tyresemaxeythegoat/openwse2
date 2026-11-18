// Clean-room bridge connecting `LoadedSoundBank` / `LoadedMusicTrack` / `Vfs`
// and `WseWorldState` to `wse::platform::WseAudioMixer` (Slice P6).
//
// Port infrastructure built over `wse/asset/audio.h`, `wse/platform/audio.h`,
// `wse/game/module_asset_loaders.h`, and `wse/game/world_state.h`.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/asset/audio.h"
#include "wse/game/audio_runtime.h"
#include "wse/game/module_asset_loaders.h"
#include "wse/game/world_state.h"
#include "wse/platform/audio.h"
#include "wse/platform/vfs.h"

namespace wse::game {

struct WorldAudioStats {
    std::size_t sample_paths_resolved = 0;
    std::size_t sample_paths_missing = 0;
    std::size_t samples_decoded = 0;
    std::size_t sample_decode_failures = 0;
    std::size_t music_paths_resolved = 0;
    std::size_t music_paths_missing = 0;
};

class WorldAudioBridge final : public WseAudioRuntime {
public:
    WorldAudioBridge() = default;

    // Binds the bridge to `world`, resolves module sound/music paths in `vfs`
    // (`0x140332580` / `0x140290870`), and configures `WseAudioMixer`. When
    // `preload_samples` is true, every sample whose file resolved is decoded
    // eagerly; otherwise samples decode lazily on first playback (synchronously,
    // on the calling thread). A sample that fails to read or decode is remembered
    // and not retried on later plays.
    WorldAudioStats initialize(wse::platform::Vfs& vfs,
                               WseWorldState& world,
                               const LoadedSoundBank& sound_bank,
                               const std::vector<LoadedMusicTrack>& music_tracks,
                               std::string_view module_dir,
                               std::string_view language = {},
                               const wse::asset::AudioCodecBackend* codecs = nullptr,
                               bool preload_samples = false) {
        detach();
        vfs_ = &vfs;
        world_ = &world;
        codecs_ = codecs;
        module_dir_ = std::string(module_dir);
        language_ = std::string(language);

        WorldAudioStats stats{};
        resolved_sample_paths_.assign(sound_bank.samples.size(), {});
        decoded_samples_.assign(sound_bank.samples.size(), nullptr);
        decode_failed_.assign(sound_bank.samples.size(), 0U);

        std::vector<wse::platform::MixerSoundSample> mixer_samples(sound_bank.samples.size());
        for (std::size_t i = 0; i < sound_bank.samples.size(); ++i) {
            const LoadedSoundSample& src = sound_bank.samples[i];
            mixer_samples[i].flags = src.flags;
            resolved_sample_paths_[i] =
                wse::asset::resolve_sound_sample_path(vfs, module_dir_, language_, src.file);
            if (resolved_sample_paths_[i].empty()) {
                ++stats.sample_paths_missing;
            } else {
                ++stats.sample_paths_resolved;
                if (preload_samples) {
                    auto pcm = ensure_sample_decoded(i);
                    if (pcm != nullptr) {
                        ++stats.samples_decoded;
                        mixer_samples[i].pcm = pcm;
                    } else {
                        ++stats.sample_decode_failures;
                    }
                }
            }
        }

        std::vector<wse::platform::MixerSoundDef> mixer_sounds(sound_bank.sounds.size());
        for (std::size_t i = 0; i < sound_bank.sounds.size(); ++i) {
            mixer_sounds[i].flags = sound_bank.sounds[i].flags;
            mixer_sounds[i].variants.reserve(sound_bank.sounds[i].refs.size());
            for (const LoadedSoundRef& ref : sound_bank.sounds[i].refs) {
                mixer_sounds[i].variants.push_back(
                    wse::platform::MixerSoundVariant{ref.sample_no, ref.flags});
            }
        }
        sounds_ = mixer_sounds;
        mixer_.load_sound_bank(std::move(mixer_samples), std::move(mixer_sounds));

        resolved_music_paths_.assign(music_tracks.size(), {});
        std::vector<wse::platform::MixerMusicTrack> mixer_tracks(music_tracks.size());
        for (std::size_t i = 0; i < music_tracks.size(); ++i) {
            const LoadedMusicTrack& mt = music_tracks[i];
            resolved_music_paths_[i] =
                wse::asset::resolve_music_track_path(vfs, module_dir_, mt.file);
            if (resolved_music_paths_[i].empty()) {
                ++stats.music_paths_missing;
            } else {
                ++stats.music_paths_resolved;
            }
            mixer_tracks[i].path = resolved_music_paths_[i];
            mixer_tracks[i].norm =
                wse::asset::normalize_music_track_flags(mt.flags, mt.continue_flags);
        }
        mixer_.load_music_tracks(std::move(mixer_tracks));

        mixer_.set_music_stream_opener([this](std::size_t track_idx) {
            return open_music_stream(track_idx);
        });
        mixer_.set_channel_end_callback([this](std::int32_t channel_no) {
            on_channel_ended(channel_no);
        });

        world_->audio_runtime = this;
        return stats;
    }

    ~WorldAudioBridge() override { detach(); }

    void detach() noexcept {
        if (world_ != nullptr && world_->audio_runtime == this) {
            world_->audio_runtime = nullptr;
        }
        world_ = nullptr;
    }

    [[nodiscard]] wse::platform::WseAudioMixer& mixer() noexcept { return mixer_; }
    [[nodiscard]] const wse::platform::WseAudioMixer& mixer() const noexcept { return mixer_; }

    std::int32_t play_sound(std::int32_t sound_no, const wse::core::WseVec4& position,
                            std::uint32_t flags, float freq_multiplier,
                            std::int32_t culture_filter) override {
        ensure_sound_variants_decoded(sound_no);
        const std::int32_t ch = mixer_.play_sound_at_position(
            sound_no, position, {}, flags, freq_multiplier, culture_filter);
        if (world_ != nullptr) {
            world_->music.last_sound_channel = ch;
        }
        return ch;
    }

    std::int32_t play_sound_2d(std::int32_t sound_no, std::uint32_t flags) override {
        ensure_sound_variants_decoded(sound_no);
        const std::int32_t ch = mixer_.play_sound_2d(sound_no, flags);
        if (world_ != nullptr) {
            world_->music.last_sound_channel = ch;
        }
        return ch;
    }

    void stop_channel(std::int32_t channel_no) override { mixer_.stop_sound(channel_no); }
    [[nodiscard]] bool is_channel_looping(std::int32_t channel_no) const override {
        return mixer_.is_channel_looping(channel_no);
    }

    // Per-frame tick: moves every live agent / prop channel to its owner's current
    // position (sounds follow moving agents and props), then runs the mixer's
    // music scheduler and occlusion update at `now_seconds`.
    void update(double now_seconds) {
        if (world_ != nullptr) {
            for (const auto& ag : world_->agents) {
                if (ag.m5.sound_channel >= 0) {
                    mixer_.set_channel_position(ag.m5.sound_channel, ag.position.o);
                }
            }
            for (const auto& mo : world_->mission_objects) {
                if (mo.m5.sound_channel >= 0) {
                    mixer_.set_channel_position(mo.m5.sound_channel, mo.position.o);
                }
            }
        }
        mixer_.update(now_seconds);
    }
    void stop_all_sounds(bool include_non_looping) override {
        mixer_.stop_all_sounds(include_non_looping);
    }

    [[nodiscard]] std::int32_t get_sound_progress(std::int32_t channel_no) const override {
        return mixer_.get_sound_progress(channel_no);
    }

    [[nodiscard]] std::int32_t last_sound_channel() const override {
        return mixer_.last_sound_channel();
    }

    void play_track(std::int32_t track_no, std::int32_t option) override {
        mixer_.play_track(track_no, option);
    }
    void play_cue_track(std::int32_t track_no) override { mixer_.play_cue_track(track_no); }
    void set_music_situation(std::uint32_t situation_flags) override {
        mixer_.set_music_situation(situation_flags);
    }
    void set_music_culture(std::uint32_t culture_flags) override {
        mixer_.set_music_culture(culture_flags);
    }

private:
    std::shared_ptr<const wse::asset::AudioPcmBuffer> ensure_sample_decoded(std::size_t idx) {
        if (idx >= decoded_samples_.size()) {
            return nullptr;
        }
        if (decoded_samples_[idx] != nullptr) {
            return decoded_samples_[idx];
        }
        if (vfs_ == nullptr || decode_failed_[idx] != 0U ||
            resolved_sample_paths_[idx].empty()) {
            return nullptr;
        }
        std::vector<std::uint8_t> bytes;
        auto pcm = std::make_shared<wse::asset::AudioPcmBuffer>();
        if (!vfs_->read_file(resolved_sample_paths_[idx], bytes) ||
            !wse::asset::decode_audio_bytes(bytes, resolved_sample_paths_[idx], codecs_, *pcm)) {
            decode_failed_[idx] = 1U;
            return nullptr;
        }
        decoded_samples_[idx] = pcm;
        return pcm;
    }

    void ensure_sound_variants_decoded(std::int32_t sound_no) {
        if (sound_no < 0 || static_cast<std::size_t>(sound_no) >= sounds_.size()) {
            return;
        }
        const std::size_t limit =
            std::min(static_cast<std::size_t>(sound_no) + 5U, sounds_.size());
        for (std::size_t s = static_cast<std::size_t>(sound_no); s < limit; ++s) {
            for (const auto& v : sounds_[s].variants) {
                if (v.sample_index < 0 ||
                    static_cast<std::size_t>(v.sample_index) >= decoded_samples_.size()) {
                    continue;
                }
                const auto si = static_cast<std::size_t>(v.sample_index);
                if (decoded_samples_[si] == nullptr && decode_failed_[si] == 0U &&
                    !resolved_sample_paths_[si].empty()) {
                    auto pcm = ensure_sample_decoded(si);
                    if (pcm != nullptr) {
                        mixer_.set_sample_pcm(si, std::move(pcm));
                    }
                }
            }
            if ((sounds_[s].flags & wse::asset::kSoundFlagLodFallback) == 0U) {
                break;
            }
        }
    }

    std::unique_ptr<wse::asset::AudioStreamDecoder> open_music_stream(std::size_t track_idx) {
        if (vfs_ == nullptr || track_idx >= resolved_music_paths_.size() ||
            resolved_music_paths_[track_idx].empty()) {
            return nullptr;
        }
        std::vector<std::uint8_t> bytes;
        if (!vfs_->read_file(resolved_music_paths_[track_idx], bytes)) {
            return nullptr;
        }
        return wse::asset::open_audio_stream(std::move(bytes), resolved_music_paths_[track_idx],
                                             codecs_);
    }

    void on_channel_ended(std::int32_t channel_no) noexcept {
        if (world_ == nullptr || channel_no < 0) {
            return;
        }
        for (auto& ag : world_->agents) {
            if (ag.m5.sound_channel == channel_no) {
                ag.m5.sound_channel = -1;
            }
        }
        for (auto& mo : world_->mission_objects) {
            if (mo.m5.sound_channel == channel_no) {
                mo.m5.sound_channel = -1;
                mo.m5.active_sound_no = -1;
                mo.m5.sound_playing = false;
            }
        }
    }

    wse::platform::Vfs* vfs_ = nullptr;
    WseWorldState* world_ = nullptr;
    const wse::asset::AudioCodecBackend* codecs_ = nullptr;
    std::string module_dir_{};
    std::string language_{};
    std::vector<std::string> resolved_sample_paths_{};
    std::vector<std::shared_ptr<const wse::asset::AudioPcmBuffer>> decoded_samples_{};
    std::vector<std::uint8_t> decode_failed_{};
    std::vector<wse::platform::MixerSoundDef> sounds_{};
    std::vector<std::string> resolved_music_paths_{};
    wse::platform::WseAudioMixer mixer_{};
};

}  // namespace wse::game
