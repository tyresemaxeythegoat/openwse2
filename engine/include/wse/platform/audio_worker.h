// Platform seam: audio mixer worker thread.
//
// Port infrastructure, not a reconstruction: retail mixes inside FMOD, on FMOD's own thread
// (`mbSoundManager::initializeFmod` / `update` drive it from the game thread). `WseAudioMixer`
// has no internal locking, so this worker owns the one mutex that guards it: the worker thread
// tops the device queue up to a target latency under that mutex, and the game thread takes the
// same mutex (`lock()` / `with_mixer`) for every mixer call, including
// `WorldAudioBridge::update` and anything that reads state written by channel-end callbacks
// (those run on the worker thread, inside `mix_frames`). `STATUS.md` §5 P6 review item 2.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "wse/platform/audio.h"

namespace wse::platform {

class AudioMixerWorker {
public:
    struct Options {
        std::size_t target_queued_frames = 2048U;  // ~43 ms at 48 kHz
        std::size_t max_block_frames = 512U;
        std::chrono::microseconds period{4000};    // wake-up interval between top-ups
    };

    AudioMixerWorker(WseAudioMixer& mixer, AudioDevice& device) noexcept
        : AudioMixerWorker(mixer, device, Options{}) {}
    AudioMixerWorker(WseAudioMixer& mixer, AudioDevice& device, Options options) noexcept
        : mixer_(mixer), device_(device), options_(options) {
        if (options_.max_block_frames == 0U) {
            options_.max_block_frames = 512U;
        }
        if (options_.period.count() <= 0) {
            options_.period = std::chrono::microseconds{4000};
        }
    }
    AudioMixerWorker(const AudioMixerWorker&) = delete;
    AudioMixerWorker& operator=(const AudioMixerWorker&) = delete;
    ~AudioMixerWorker() { stop(); }

    // Starts the worker thread; false when the device is not open. Idempotent.
    bool start() {
        if (thread_.joinable()) {
            return true;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!device_.is_open()) {
                return false;
            }
            stop_requested_ = false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Stops and joins the worker thread. Idempotent; the mixer stays usable via `with_mixer`.
    void stop() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_requested_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    // Holds the mixer mutex for the caller's scope (game-thread mixer and bridge calls).
    [[nodiscard]] std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(mutex_); }

    template <class F>
    decltype(auto) with_mixer(F&& f) {
        std::lock_guard<std::mutex> guard(mutex_);
        return std::forward<F>(f)(mixer_);
    }

    // Pauses the device and stops topping it up (Android `onPause` / SDL
    // `SDL_EVENT_WILL_ENTER_BACKGROUND`); resuming continues from the queued audio.
    void set_paused(bool paused) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (paused_ == paused) {
                return;
            }
            paused_ = paused;
            device_.set_paused(paused);
        }
        wake_.notify_all();
    }

    [[nodiscard]] bool paused() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return paused_;
    }

    // One top-up on the calling thread (tests, or hosts that pump without the thread).
    std::size_t pump_once() {
        std::lock_guard<std::mutex> guard(mutex_);
        return pump_locked();
    }

    [[nodiscard]] std::uint64_t frames_submitted() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return frames_submitted_;
    }

private:
    std::size_t pump_locked() {
        if (paused_ || !device_.is_open()) {
            return 0U;
        }
        const std::size_t n = mixer_.pump_to_latency(device_, options_.target_queued_frames,
                                                     options_.max_block_frames);
        frames_submitted_ += n;
        return n;
    }

    void run() {
        std::unique_lock<std::mutex> guard(mutex_);
        while (!stop_requested_) {
            (void)pump_locked();
            wake_.wait_for(guard, options_.period, [this] { return stop_requested_; });
        }
    }

    WseAudioMixer& mixer_;
    AudioDevice& device_;
    Options options_{};
    mutable std::mutex mutex_{};
    std::condition_variable wake_{};
    std::thread thread_{};
    bool stop_requested_ = false;
    bool paused_ = false;
    std::uint64_t frames_submitted_ = 0U;
};

}  // namespace wse::platform
