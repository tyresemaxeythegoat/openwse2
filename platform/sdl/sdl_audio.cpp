// SDL3 audio-stream device backend (`SDL_OpenAudioDeviceStream`, Slice P6).

#include "sdl_audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <limits>
#include <mutex>

namespace wse::platform {

namespace {

class SdlAudioDevice final : public AudioDevice {
public:
    SdlAudioDevice() = default;

    SdlAudioDevice(const SdlAudioDevice&) = delete;
    SdlAudioDevice& operator=(const SdlAudioDevice&) = delete;

    ~SdlAudioDevice() override { close(); }

    bool open(const AudioDeviceSpec& spec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_unlocked();
        if (spec.sample_rate == 0U || spec.channels != 2U) {
            return false;
        }
        if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0U) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                SDL_Log("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
                return false;
            }
            owns_audio_subsys_ = true;
        }

        SDL_AudioSpec sdl_spec{};
        sdl_spec.format = SDL_AUDIO_F32;
        sdl_spec.channels = static_cast<int>(spec.channels);
        sdl_spec.freq = static_cast<int>(spec.sample_rate);
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &sdl_spec, nullptr, nullptr);
        if (stream_ == nullptr) {
            SDL_Log("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
            close_unlocked();
            return false;
        }
        spec_ = spec;
        paused_ = false;
        if (!SDL_ResumeAudioStreamDevice(stream_)) {
            SDL_Log("SDL_ResumeAudioStreamDevice warning: %s", SDL_GetError());
        }
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_unlocked();
    }

    [[nodiscard]] bool is_open() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_ != nullptr;
    }

    [[nodiscard]] AudioDeviceSpec spec() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return spec_;
    }

    bool submit_frames(std::span<const float> interleaved_stereo) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_ == nullptr || (interleaved_stereo.size() & 1U) != 0U) {
            return false;
        }
        if (interleaved_stereo.empty()) {
            return true;
        }
        const std::size_t bytes = interleaved_stereo.size() * sizeof(float);
        if (bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        return SDL_PutAudioStreamData(stream_, interleaved_stereo.data(),
                                      static_cast<int>(bytes));
    }

    [[nodiscard]] std::size_t queued_frames() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_ == nullptr || spec_.channels == 0U) {
            return 0U;
        }
        const int queued_bytes = SDL_GetAudioStreamQueued(stream_);
        if (queued_bytes <= 0) {
            return 0U;
        }
        const std::size_t frame_bytes = sizeof(float) * static_cast<std::size_t>(spec_.channels);
        return static_cast<std::size_t>(queued_bytes) / frame_bytes;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_ != nullptr) {
            SDL_ClearAudioStream(stream_);
        }
    }

    void set_paused(bool paused) override {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
        if (stream_ == nullptr) {
            return;
        }
        if (paused) {
            SDL_PauseAudioStreamDevice(stream_);
        } else {
            SDL_ResumeAudioStreamDevice(stream_);
        }
    }

    [[nodiscard]] const char* backend_name() const noexcept override { return "sdl3"; }

private:
    void close_unlocked() noexcept {
        if (stream_ != nullptr) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
        if (owns_audio_subsys_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            owns_audio_subsys_ = false;
        }
        spec_ = {};
        paused_ = false;
    }

    mutable std::mutex mutex_{};
    SDL_AudioStream* stream_ = nullptr;
    AudioDeviceSpec spec_{};
    bool paused_ = false;
    bool owns_audio_subsys_ = false;
};

}  // namespace

std::unique_ptr<AudioDevice> create_sdl_audio_device() {
    return std::make_unique<SdlAudioDevice>();
}

}  // namespace wse::platform
