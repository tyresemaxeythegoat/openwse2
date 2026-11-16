// SDL3 playback implementation of `wse::platform::AudioDevice` (Slice P6).
// Kept outside `engine/include/` so `<SDL3/SDL.h>` never leaks into portable
// engine headers.

#pragma once

#include <memory>

#include "wse/platform/audio.h"

namespace wse::platform {

[[nodiscard]] std::unique_ptr<AudioDevice> create_sdl_audio_device();

}  // namespace wse::platform
