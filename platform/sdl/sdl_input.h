// SDL3 implementation of `wse::platform::InputBackend` (Slice P7).
// Kept outside `engine/include/` so `<SDL3/SDL.h>` never leaks into portable
// engine headers.

#pragma once

#include <memory>

#include "wse/platform/input.h"

namespace wse::platform {

class SdlInputBackend : public InputBackend {
public:
    virtual void set_viewport_size(int width, int height) noexcept = 0;
    // Translates one `const SDL_Event*` queued from the SDL event loop.
    virtual bool push_sdl_event(const void* sdl_event) = 0;
    // Slice P8: opt-in gyroscope streaming (only opens SDL_SENSOR_GYRO when enabled).
    virtual void set_gyro_enabled(bool enabled) noexcept = 0;
    [[nodiscard]] virtual bool is_gyro_enabled() const noexcept = 0;
    [[nodiscard]] virtual bool has_gyro_sensor() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<SdlInputBackend> create_sdl_input_backend();

}  // namespace wse::platform
