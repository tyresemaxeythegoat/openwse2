// Clean-room non-owning audio runtime hook interface embedded on `WseWorldState`.
//
// Port infrastructure, not a reconstruction: decouples VM opcodes (`599..604`,
// `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`) from `WseAudioMixer`
// and audio codec/device libraries, matching `WseLuaRuntime` (`wse/vm/lua_runtime.h`).

#pragma once

#include <cstdint>

#include "wse/core/math.h"

namespace wse::game {

class WseAudioRuntime {
public:
    virtual ~WseAudioRuntime() = default;

    [[nodiscard]] virtual std::int32_t play_sound(std::int32_t sound_no,
                                                  const wse::core::WseVec4& position,
                                                  std::uint32_t flags,
                                                  float freq_multiplier,
                                                  std::int32_t culture_filter) = 0;

    [[nodiscard]] virtual std::int32_t play_sound_2d(std::int32_t sound_no,
                                                     std::uint32_t flags) = 0;

    virtual void stop_channel(std::int32_t channel_no) = 0;
    [[nodiscard]] virtual bool is_channel_looping(std::int32_t channel_no) const = 0;
    virtual void stop_all_sounds(bool include_non_looping) = 0;

    [[nodiscard]] virtual std::int32_t get_sound_progress(std::int32_t channel_no) const = 0;
    // Channel of the most recent play request, `-1` when that request failed.
    [[nodiscard]] virtual std::int32_t last_sound_channel() const = 0;

    virtual void play_track(std::int32_t track_no, std::int32_t option) = 0;
    virtual void play_cue_track(std::int32_t track_no) = 0;
    virtual void set_music_situation(std::uint32_t situation_flags) = 0;
    virtual void set_music_culture(std::uint32_t culture_flags) = 0;
};

}  // namespace wse::game
