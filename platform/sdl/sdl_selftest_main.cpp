// P1-P7 on-device SDL3 self-test shell.
//
// Runs all 21 portable engine test suites in-process behind an SDL3 window and
// exercises the SDL3 audio stream & input backends (docs/PLATFORM_PLAN.md). With
// `--module <warband_root> [module_rel_dir]` it runs a module's UI instead (`run_module_shell`).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "sdl_audio.h"
#include "sdl_input.h"
#include "../android/perf_hint.h"
#include "wse/game/module_loader_common.h"
#include "wse/platform/audio_worker.h"
#include "wse/platform/clock.h"
#include "wse/platform/performance.h"
#include "wse/platform/rhi_geometry.h"
#include "wse/platform/ui_game_shell.h"
#include "wse/platform/vfs.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>

#include "../android/saf_game_tree.h"
#endif
#include "wse_selftest.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include <stdlib.h>
#endif

namespace {

namespace fs = std::filesystem;

enum class RowStatus { wait, running, pass, fail };

struct RowEntry {
    std::string name;
    RowStatus status = RowStatus::wait;
    std::int64_t duration_ms = 0;
};

struct UiState {
    std::mutex mutex;
    bool finished = false;
    bool all_passed = false;
    int completed_count = 0;
    int passed_count = 0;
    int total_count = 22;
    std::int64_t total_ms = 0;
    std::string active_name;
    std::string note;
    std::vector<RowEntry> rows;
    std::vector<std::string> failure_lines;
};

struct RuntimePaths {
    std::string fixture_path;
    std::string log_path;
};

RuntimePaths prepare_runtime_paths(std::string& error_out) {
    RuntimePaths paths;
#ifdef __ANDROID__
    const char* base_cstr = SDL_GetAndroidInternalStoragePath();
    if (!base_cstr || !*base_cstr) {
        base_cstr = SDL_GetAndroidCachePath();
    }
    if (!base_cstr || !*base_cstr) {
        error_out = "No writable Android storage path returned by SDL";
        return paths;
    }

    std::error_code ec;
    const fs::path base(base_cstr);
    const fs::path tmp_dir = base / "wse_selftest_tmp";
    const fs::path asset_dir = base / "wse_selftest";
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);
    fs::create_directories(asset_dir, ec);
    ::setenv("TMPDIR", tmp_dir.string().c_str(), 1);

    size_t fixture_size = 0;
    void* fixture_data = SDL_LoadFile("scripts.txt", &fixture_size);
    const fs::path staged_fixture = asset_dir / "scripts.txt";
    if (fixture_data && fixture_size > 0) {
        std::ofstream out(staged_fixture, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(fixture_data),
                  static_cast<std::streamsize>(fixture_size));
        out.close();
        SDL_free(fixture_data);
        if (!out) {
            error_out = "Failed writing staged scripts.txt";
        }
    } else {
        if (fixture_data) {
            SDL_free(fixture_data);
        }
        error_out = std::string("Missing APK asset scripts.txt: ") + SDL_GetError();
    }

    paths.fixture_path = staged_fixture.string();
    paths.log_path = (tmp_dir / "selftest_stdout.log").string();
#else
    (void)error_out;
    paths.fixture_path = "engine/fixtures/scripts.txt";
    paths.log_path.clear();
#endif
    return paths;
}

void collect_failure_lines(const std::string& log_path,
                           std::vector<std::string>& out) {
    out.clear();
    if (log_path.empty()) {
        return;
    }
    std::ifstream in(log_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("FAIL") != std::string::npos ||
            line.find("failures") != std::string::npos) {
            if (line.size() > 54) {
                line.resize(51);
                line += "...";
            }
            out.push_back(line);
            if (out.size() >= 8) {
                break;
            }
        }
    }
}

struct AppLifecycle {
    std::atomic<bool> terminating{false};
    wse::platform::AudioMixerWorker* audio = nullptr;  // set before the watch is added
};

// SDL documents that the Android lifecycle events (SDL_EVENT_TERMINATING,
// SDL_EVENT_WILL_ENTER_BACKGROUND, SDL_EVENT_DID_ENTER_FOREGROUND) must be handled
// from an event watch: by the time they could be polled the OS may already have
// suspended or killed the process. The watch raises the terminate flag the main loop
// and worker observe, and pauses / resumes the audio device with the activity
// (`AudioDevice::set_paused`, STATUS.md §5 P6 review item 1).
bool SDLCALL on_app_event(void* userdata, SDL_Event* event) {
    auto* app = static_cast<AppLifecycle*>(userdata);
    if (event->type == SDL_EVENT_TERMINATING) {
        app->terminating.store(true);
    } else if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND && app->audio != nullptr) {
        app->audio->set_paused(true);
    } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND && app->audio != nullptr) {
        app->audio->set_paused(false);
    }
    return true;
}

[[nodiscard]] bool point_in_rect(float x, float y, const SDL_FRect& r) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

void draw_filled_rect(SDL_Renderer* renderer, float x, float y, float w, float h,
                      Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

void apply_display_frame_rate(SDL_Window* window,
                              SDL_Renderer* renderer,
                              std::int32_t target_fps,
                              wse::platform::PresentModePreference present_mode,
                              bool user_selected) noexcept {
    if (renderer != nullptr) {
        const wse::platform::FrameRateCap cap =
            wse::platform::frame_rate_cap_from_hz(target_fps);
        SDL_SetRenderVSync(renderer,
                           wse::platform::sdl_vsync_for_present_mode(present_mode, cap));
    }
#ifdef __ANDROID__
    if (window != nullptr) {
        using SetFrameRateWithStrategyFn =
            int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
        using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);
        void* nw_ptr = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
        auto* anw = static_cast<ANativeWindow*>(nw_ptr);
        if (anw != nullptr) {
            constexpr int8_t kCompatDefault = 0;   // ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT
            constexpr int8_t kOnlyIfSeamless = 0;  // ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS
            constexpr int8_t kAlways = 1;          // ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS
            const float rate = (target_fps > 0) ? static_cast<float>(target_fps) : 0.0f;
            // The rate the player picked may need a (non-seamless) display mode switch,
            // which ONLY_IF_SEAMLESS silently refuses on many panels, so the first
            // request uses ALWAYS. Thermal step-downs mid-session stay seamless-only so
            // they never blank the screen.
            const int8_t strategy = user_selected ? kAlways : kOnlyIfSeamless;
            if (auto fn31 = reinterpret_cast<SetFrameRateWithStrategyFn>(
                    dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeStrategy"));
                fn31 != nullptr) {
                (void)fn31(anw, rate, kCompatDefault, strategy);
            } else if (auto fn30 = reinterpret_cast<SetFrameRateFn>(
                           dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
                       fn30 != nullptr) {
                (void)fn30(anw, rate, kCompatDefault);
            }
        }
    }
#else
    (void)window;
    (void)user_selected;
#endif
}

// Shared per-loop settings / thermal / present-mode / pacing coordinator used by both
// the self-test UI loop and `run_module_shell`.
struct PerfLoopState {
    wse::platform::ThermalPerformanceController perf_ctrl;
    wse::platform::RuntimeLaunchPerfConfig launch_cfg{};
    wse::platform::FrameTimeStats frame_stats{};
    std::uint64_t launch_cfg_rev = 0U;
    std::uint64_t thermal_rev = 0U;
    std::int32_t last_applied_fps_cap = -1;
    std::int32_t last_requested_fps_cap = -1;
    wse::platform::PresentModePreference last_applied_present_mode =
        wse::platform::PresentModePreference::Fifo;
    bool has_applied_display = false;

    void apply_launch_config(const wse::platform::RuntimeLaunchPerfConfig& cfg,
                             wse::platform::WseInputManager* input_mgr,
                             wse::platform::SdlInputBackend* sdl_input) {
        launch_cfg = cfg;
        if (input_mgr != nullptr) {
            auto prof = input_mgr->profile();
            prof.gyro_aim_enabled = cfg.settings.gyro_aim_enabled;
            prof.pinch_zoom_enabled = cfg.settings.pinch_zoom_enabled;
            wse::platform::sanitize_input_profile(prof);
            input_mgr->profile() = prof;
            if (sdl_input != nullptr) {
                sdl_input->set_gyro_enabled(prof.gyro_aim_enabled);
            }
        } else if (sdl_input != nullptr) {
            sdl_input->set_gyro_enabled(cfg.settings.gyro_aim_enabled);
        }
        perf_ctrl.set_settings(cfg.settings);
    }

    bool poll_launch_config(wse::platform::WseInputManager* input_mgr,
                            wse::platform::SdlInputBackend* sdl_input) {
        if (!wse::platform::RuntimePerformanceHub::global().poll_launch_config(
                launch_cfg_rev, launch_cfg)) {
            return false;
        }
        apply_launch_config(launch_cfg, input_mgr, sdl_input);
        return true;
    }

    // Polls the hub thermal snapshot, updates `frame_pacer` and SDL/Android display
    // rate + VSync whenever the effective FPS cap or active present mode changes, and
    // returns the effective FPS target (0 means uncapped).
    std::int32_t update_thermal(SDL_Window* window,
                                SDL_Renderer* renderer,
                                wse::platform::FramePacer& frame_pacer,
                                bool* target_changed_out = nullptr,
                                wse::platform::PerformanceAdaptation* adaptation_out = nullptr) {
        std::uint64_t next_thermal_rev = thermal_rev;
        const auto therm =
            wse::platform::RuntimePerformanceHub::global().thermal_snapshot(&next_thermal_rev);
        if (next_thermal_rev != thermal_rev) {
            thermal_rev = next_thermal_rev;
            perf_ctrl.update_thermal(therm);
        }
        const auto adapt = perf_ctrl.evaluate();
        const bool fps_changed = adapt.effective_fps_cap != last_applied_fps_cap;
        const bool mode_changed =
            !has_applied_display || adapt.active_present_mode != last_applied_present_mode;
        // A new launcher/settings choice, not a thermal step-down.
        const bool user_selected = adapt.requested_fps_cap != last_requested_fps_cap;
        last_requested_fps_cap = adapt.requested_fps_cap;
        if (fps_changed) {
            last_applied_fps_cap = adapt.effective_fps_cap;
            frame_pacer.set_target_fps(last_applied_fps_cap);
            frame_stats.reset();
        }
        if (fps_changed || mode_changed) {
            last_applied_present_mode = adapt.active_present_mode;
            has_applied_display = true;
            apply_display_frame_rate(window, renderer, adapt.effective_fps_cap,
                                     adapt.active_present_mode, user_selected);
        }
        if (target_changed_out != nullptr) {
            *target_changed_out = fps_changed;
        }
        if (adaptation_out != nullptr) {
            *adaptation_out = adapt;
        }
        return adapt.effective_fps_cap;
    }

    std::int32_t step(SDL_Window* window,
                      SDL_Renderer* renderer,
                      wse::platform::FramePacer& frame_pacer,
                      wse::platform::WseInputManager* input_mgr,
                      wse::platform::SdlInputBackend* sdl_input,
                      bool* target_changed_out = nullptr,
                      wse::platform::PerformanceAdaptation* adaptation_out = nullptr) {
        (void)poll_launch_config(input_mgr, sdl_input);
        return update_thermal(window, renderer, frame_pacer, target_changed_out, adaptation_out);
    }

    [[nodiscard]] wse::platform::PerformanceAdaptation evaluate() const noexcept {
        return perf_ctrl.evaluate();
    }
};

[[nodiscard]] std::string resolve_install_language(wse::platform::Vfs& vfs,
                                                   std::string_view module_dir,
                                                   std::string_view requested) {
    const std::string lower = wse::platform::ascii_lower(requested);
    if (lower.empty() || lower == "en") {
        return "en";
    }
    const std::string root_lang_dir = wse::game::vfs_join_path("languages", lower);
    const std::string mod_langs_dir = wse::game::vfs_join_path(module_dir, "languages");
    const std::string mod_lang_dir = wse::game::vfs_join_path(mod_langs_dir, lower);
    if (vfs.is_dir(root_lang_dir) || vfs.is_dir(mod_lang_dir)) {
        return lower;
    }
    return "en";
}

void log_module_fps_target(const wse::platform::PerformanceAdaptation& adapt) noexcept {
    const std::string_view tier_sv = wse::platform::thermal_tier_name(adapt.tier);
    const std::string_view mode_sv = wse::platform::present_mode_name(adapt.active_present_mode);
    if (adapt.effective_fps_cap > 0) {
        SDL_Log("[WSE2] module fps target %d Hz (requested %d Hz, tier %.*s, mode %.*s)",
                adapt.effective_fps_cap, adapt.requested_fps_cap,
                static_cast<int>(tier_sv.size()), tier_sv.data(),
                static_cast<int>(mode_sv.size()), mode_sv.data());
    } else {
        SDL_Log("[WSE2] module fps target uncapped (requested %d Hz, tier %.*s, mode %.*s)",
                adapt.requested_fps_cap, static_cast<int>(tier_sv.size()), tier_sv.data(),
                static_cast<int>(mode_sv.size()), mode_sv.data());
    }
}

struct FpsOverlayState {
    static constexpr std::uint64_t kUpdateIntervalNs = 500000000ULL;  // 500 ms (max 2 Hz)
    std::uint64_t last_update_ns = 0ULL;
    std::uint64_t cpu_accum_ns = 0ULL;
    std::uint32_t cpu_samples = 0U;
    bool initialized = false;
    char line1[64]{};
    char line2[64]{};

    void record_cpu(std::uint64_t cpu_ns) noexcept {
        cpu_accum_ns += std::min<std::uint64_t>(cpu_ns, 1000000000ULL);
        ++cpu_samples;
    }

    [[nodiscard]] bool update_if_due(std::uint64_t now_ns,
                                     const wse::platform::FrameTimeStats& stats,
                                     std::int32_t pacer_target_fps,
                                     wse::platform::ThermalTier tier) noexcept {
        if (initialized && now_ns - last_update_ns < kUpdateIntervalNs) {
            return false;
        }
        initialized = true;
        last_update_ns = now_ns;

        const double avg_cpu_ms =
            cpu_samples > 0U
                ? static_cast<double>(cpu_accum_ns) / static_cast<double>(cpu_samples) / 1.0e6
                : -1.0;
        cpu_accum_ns = 0ULL;
        cpu_samples = 0U;

        if (stats.sample_count() > 0U && avg_cpu_ms >= 0.0) {
            std::snprintf(line1, sizeof(line1), "%.1f fps | cpu %.2f ms", stats.average_fps(),
                          avg_cpu_ms);
        } else if (stats.sample_count() > 0U) {
            std::snprintf(line1, sizeof(line1), "%.1f fps | cpu --.- ms", stats.average_fps());
        } else if (avg_cpu_ms >= 0.0) {
            std::snprintf(line1, sizeof(line1), "--.- fps | cpu %.2f ms", avg_cpu_ms);
        } else {
            std::snprintf(line1, sizeof(line1), "--.- fps | cpu --.- ms");
        }

        const std::string_view tier_sv = wse::platform::thermal_tier_name(tier);
        char target_buf[16];
        if (pacer_target_fps > 0) {
            std::snprintf(target_buf, sizeof(target_buf), "%dHz", pacer_target_fps);
        } else {
            std::snprintf(target_buf, sizeof(target_buf), "uncap");
        }
        std::snprintf(line2, sizeof(line2), "cap %s | %.*s", target_buf,
                      static_cast<int>(tier_sv.size()), tier_sv.data());
        return true;
    }
};

void draw_fps_overlay(SDL_Renderer* renderer,
                      SDL_Window* window,
                      int out_w,
                      int out_h,
                      const FpsOverlayState& overlay) noexcept {
    if (renderer == nullptr || out_w <= 0 || out_h <= 0) {
        return;
    }
    int win_w = out_w;
    int win_h = out_h;
    if (window != nullptr) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    SDL_Rect safe{0, 0, std::max(win_w, 1), std::max(win_h, 1)};
    if (window == nullptr || !SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 ||
        safe.h <= 0) {
        safe = {0, 0, std::max(win_w, 1), std::max(win_h, 1)};
    }
    const float px_scale_x =
        win_w > 0 ? static_cast<float>(out_w) / static_cast<float>(win_w) : 1.0f;
    const float px_scale_y =
        win_h > 0 ? static_cast<float>(out_h) / static_cast<float>(win_h) : 1.0f;
    const float safe_x = static_cast<float>(safe.x) * px_scale_x;
    const float safe_y = static_cast<float>(safe.y) * px_scale_y;
    const float min_dim = static_cast<float>(std::min(out_w, out_h));
    const float scale = std::clamp(std::floor(min_dim / 340.0f), 1.0f, 6.0f);

    const std::size_t max_chars =
        std::max(std::string_view(overlay.line1).size(), std::string_view(overlay.line2).size());
    const float pad = 4.0f;
    const float box_w = static_cast<float>(max_chars) * 8.0f + pad * 2.0f;
    const float box_h = 27.0f;
    const float x = safe_x / scale + 6.0f;
    const float y = safe_y / scale + 6.0f;

    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    draw_filled_rect(renderer, x, y, box_w, box_h, 12, 16, 22, 190);
    SDL_SetRenderDrawColor(renderer, 120, 230, 140, 255);
    SDL_RenderDebugText(renderer, x + pad, y + 4.0f, overlay.line1);
    SDL_SetRenderDrawColor(renderer, 210, 225, 240, 255);
    SDL_RenderDebugText(renderer, x + pad, y + 15.0f, overlay.line2);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

// Draws `GeometryFrame` batches with `SDL_RenderGeometry` on whatever GPU backend the SDL
// renderer uses (GLES / Vulkan on Android). Textures are copied from the capture device on
// first use and freed when the UI renderer destroys them.
class SdlGeometryPresenter {
public:
    explicit SdlGeometryPresenter(SDL_Renderer* renderer) noexcept : renderer_(renderer) {}
    SdlGeometryPresenter(const SdlGeometryPresenter&) = delete;
    SdlGeometryPresenter& operator=(const SdlGeometryPresenter&) = delete;
    ~SdlGeometryPresenter() { drop_all(); }

    // After `SDL_EVENT_RENDER_DEVICE_RESET` every texture is gone; they are recreated lazily.
    void drop_all() noexcept {
        for (auto& [handle, entry] : textures_) {
            (void)handle;
            SDL_DestroyTexture(entry.tex);
        }
        textures_.clear();
    }

    // Returns false if a batch failed to draw (logged once).
    bool draw(const wse::platform::GeometryFrame& frame,
              wse::platform::GeometryCaptureRhiDevice& device) {
        for (const wse::platform::TextureHandle gone : device.take_destroyed_textures()) {
            if (const auto it = textures_.find(gone); it != textures_.end()) {
                SDL_DestroyTexture(it->second.tex);
                textures_.erase(it);
            }
        }
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (frame.viewport_width > 0U && frame.viewport_height > 0U &&
            (frame.clear_r != 0U || frame.clear_g != 0U || frame.clear_b != 0U)) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            draw_filled_rect(renderer_, static_cast<float>(frame.viewport_x),
                             static_cast<float>(frame.viewport_y),
                             static_cast<float>(frame.viewport_width),
                             static_cast<float>(frame.viewport_height), frame.clear_r,
                             frame.clear_g, frame.clear_b);
        }
        bool ok = true;
        const auto* verts = reinterpret_cast<const SDL_Vertex*>(frame.vertices.data());
        for (const wse::platform::GeometryBatch& b : frame.batches) {
            if (b.index_count == 0U) {
                continue;
            }
            if (b.has_clip && (b.clip_w == 0U || b.clip_h == 0U)) {
                continue;
            }
            if (b.has_clip) {
                const auto clamp_i32 = [](std::uint32_t v) noexcept -> int {
                    return static_cast<int>(
                        std::min<std::uint32_t>(v, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
                };
                const SDL_Rect clip_rect{
                    clamp_i32(b.clip_x),
                    clamp_i32(b.clip_y),
                    clamp_i32(b.clip_w),
                    clamp_i32(b.clip_h),
                };
                SDL_SetRenderClipRect(renderer_, &clip_rect);
            } else {
                SDL_SetRenderClipRect(renderer_, nullptr);
            }
            const SDL_BlendMode mode = sdl_blend(b.blend);
            const SDL_ScaleMode scale_mode = sdl_scale_mode(b.filter);
            TextureEntry* tex_entry = texture_for(b.texture, device, scale_mode);
            SDL_Texture* tex = tex_entry != nullptr ? tex_entry->tex : nullptr;
            if (tex != nullptr) {
                SDL_SetTextureBlendMode(tex, mode);
                if (tex_entry->scale_mode != scale_mode) {
                    SDL_SetTextureScaleMode(tex, scale_mode);
                    tex_entry->scale_mode = scale_mode;
                }
            } else {
                SDL_SetRenderDrawBlendMode(renderer_, mode);
            }
            if (!SDL_RenderGeometry(renderer_, tex, verts + b.first_vertex,
                                    static_cast<int>(b.vertex_count),
                                    frame.indices.data() + b.first_index,
                                    static_cast<int>(b.index_count))) {
                ok = false;
                if (!logged_failure_) {
                    logged_failure_ = true;
                    SDL_Log("[WSE2] SDL_RenderGeometry failed: %s", SDL_GetError());
                }
            }
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        return ok;
    }

private:
    struct TextureEntry {
        SDL_Texture* tex = nullptr;
        SDL_ScaleMode scale_mode = SDL_SCALEMODE_LINEAR;
    };

    static SDL_ScaleMode sdl_scale_mode(wse::platform::GeometryFilter filter) noexcept {
        return filter == wse::platform::GeometryFilter::nearest ? SDL_SCALEMODE_NEAREST
                                                                : SDL_SCALEMODE_LINEAR;
    }

    static SDL_BlendMode sdl_blend(wse::platform::BlendMode mode) noexcept {
        switch (mode) {
            case wse::platform::BlendMode::opaque:
                return SDL_BLENDMODE_NONE;
            case wse::platform::BlendMode::alpha:  // SRCALPHA / INVSRCALPHA (colour)
                return SDL_BLENDMODE_BLEND;
            case wse::platform::BlendMode::additive_alpha:  // SRCALPHA / ONE
                return SDL_BLENDMODE_ADD;
            case wse::platform::BlendMode::multiply:  // ZERO / SRCCOLOR
                return SDL_BLENDMODE_MOD;
            case wse::platform::BlendMode::additive:  // ONE / ONE
                return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
                                                  SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
                                                  SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        }
        return SDL_BLENDMODE_BLEND;
    }

    TextureEntry* texture_for(wse::platform::TextureHandle handle,
                              const wse::platform::GeometryCaptureRhiDevice& device,
                              SDL_ScaleMode initial_scale_mode) {
        if (handle == 0) {
            return nullptr;
        }
        if (const auto it = textures_.find(handle); it != textures_.end()) {
            return &it->second;
        }
        const auto* data = device.texture(handle);
        if (data == nullptr) {
            return nullptr;
        }
        SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC,
                                             static_cast<int>(data->width),
                                             static_cast<int>(data->height));
        if (tex == nullptr) {
            return nullptr;
        }
        SDL_UpdateTexture(tex, nullptr, data->rgba8.data(), static_cast<int>(data->width * 4U));
        SDL_SetTextureScaleMode(tex, initial_scale_mode);
        const auto it =
            textures_.emplace(handle, TextureEntry{tex, initial_scale_mode}).first;
        return &it->second;
    }

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<wse::platform::TextureHandle, TextureEntry> textures_{};
    bool logged_failure_ = false;
};

static_assert(sizeof(SDL_Vertex) == sizeof(wse::platform::GeometryVertex));
static_assert(offsetof(SDL_Vertex, position) == offsetof(wse::platform::GeometryVertex, x));
static_assert(offsetof(SDL_Vertex, color) == offsetof(wse::platform::GeometryVertex, r));
static_assert(offsetof(SDL_Vertex, tex_coord) == offsetof(wse::platform::GeometryVertex, u));

[[nodiscard]] std::string module_label_from_dir(std::string_view module_dir) {
    while (!module_dir.empty() && (module_dir.back() == '/' || module_dir.back() == '\\')) {
        module_dir.remove_suffix(1);
    }
    const std::size_t slash = module_dir.find_last_of("/\\");
    const std::string_view label =
        (slash == std::string_view::npos) ? module_dir : module_dir.substr(slash + 1U);
    return label.empty() ? std::string("Warband") : std::string(label);
}

void draw_loading_screen(SDL_Renderer* renderer,
                         int out_w,
                         int out_h,
                         const std::string& module_name,
                         const wse::platform::UiShellStartProgress& progress) {
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(renderer, 16, 20, 26, 255);
    SDL_RenderClear(renderer);

    const float safe_w = static_cast<float>(std::max(out_w, 1));
    const float safe_h = static_cast<float>(std::max(out_h, 1));
    const float scale = std::clamp(std::floor(safe_w / 340.0f), 1.0f, 6.0f);
    SDL_SetRenderScale(renderer, scale, scale);

    const float lw = safe_w / scale;
    const float lh = safe_h / scale;
    const float bar_w = std::clamp(lw - 48.0f, 80.0f, 260.0f);
    const float bar_h = 12.0f;
    const float block_h = 52.0f;
    const float x0 = std::max(12.0f, (lw - bar_w) * 0.5f);
    const float y0 = std::max(12.0f, (lh - block_h) * 0.5f);

    std::string title = "Loading " + module_name;
    if (title.size() > 48U) {
        title.resize(45U);
        title += "...";
    }
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, x0, y0, title.c_str());

    const float bar_y = y0 + 16.0f;
    draw_filled_rect(renderer, x0, bar_y, bar_w, bar_h, 36, 44, 56);
    const float frac = std::isfinite(progress.fraction)
                           ? std::clamp(progress.fraction, 0.0f, 1.0f)
                           : 0.0f;
    const float fill_w = (bar_w - 2.0f) * frac;
    if (fill_w > 0.0f) {
        draw_filled_rect(renderer, x0 + 1.0f, bar_y + 1.0f, fill_w, bar_h - 2.0f, 72, 152, 232);
    }

    const char* stage = progress.stage.empty() ? "Starting..." : progress.stage.c_str();
    char stage_buf[96];
    std::snprintf(stage_buf, sizeof(stage_buf), "%s (%d%%)", stage,
                  static_cast<int>(std::lround(static_cast<double>(frac) * 100.0)));
    SDL_SetRenderDrawColor(renderer, 185, 195, 210, 255);
    SDL_RenderDebugText(renderer, x0, bar_y + bar_h + 8.0f, stage_buf);
}

#ifdef __ANDROID__
[[nodiscard]] JNIEnv* attach_jni_thread(JavaVM* vm, bool& attached_out) noexcept {
    attached_out = false;
    if (vm == nullptr) {
        return nullptr;
    }
    void* raw_env = nullptr;
    const jint rc = vm->GetEnv(&raw_env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        return static_cast<JNIEnv*>(raw_env);
    }
    if (rc == JNI_EDETACHED) {
        JNIEnv* env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached_out = true;
            return env;
        }
    }
    return nullptr;
}

void detach_jni_thread(JavaVM* vm, bool& attached) noexcept {
    if (attached && vm != nullptr) {
        vm->DetachCurrentThread();
        attached = false;
    }
}

struct ScopedJniThread {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    bool attached = false;

    explicit ScopedJniThread(JavaVM* java_vm) noexcept
        : vm(java_vm), env(attach_jni_thread(java_vm, attached)) {}

    ~ScopedJniThread() { detach_jni_thread(vm, attached); }

    ScopedJniThread(const ScopedJniThread&) = delete;
    ScopedJniThread& operator=(const ScopedJniThread&) = delete;
};
#endif

// Module mode (`--module <warband_root> [module_rel_dir]`; on Android the `wse.module_root` /
// `wse.module` intent extras, `WseActivity.getArguments`): instead of the self-test, load the
// module from a plain directory (`HostVfs`) and run it through `UiGameShell` every display
// frame: SDL events -> `SdlInputBackend` -> `UiInputAccumulator`, one module frame, then the
// UI is drawn on the GPU (`UiGameShellOptions::Output::geometry` + `SdlGeometryPresenter`) at
// the window's pixel size into the `ui_aspect` viewport (black bars outside it), with pointer
// positions mapped through the same rectangle. The window is only redrawn and presented when the frame changed (or
// after a resize, a render reset or a return to the foreground). When the module stops (a VM
// error with `stop_on_error`), the last frame stays up with the error on top.
// With `from_saf` (Android `--saf-module`, launched from `WseLauncherActivity`) the install
// is the SAF tree `WseActivity` registered, indexed on a JVM-attached worker thread while the
// SDL loop draws a loading screen, then `UiGameShell::start_async` loads the module on a
// worker rebound to that thread before rebinding back to the SDL thread for frames.
int run_module_shell(SDL_Window* window,
                     SDL_Renderer* renderer,
                     wse::platform::SdlInputBackend* sdl_input,
                     wse::platform::WseInputManager* input_mgr,
                     AppLifecycle& app,
                     wse::platform::FramePacer& frame_pacer,
                     PerfLoopState& perf,
                     const std::string& warband_root,
                     const std::string& module_dir,
                     bool from_saf,
                     bool show_fps_overlay) {
    std::string error;
    const std::uint64_t load_start_ns = SDL_GetTicksNS();
    const std::string module_name = module_label_from_dir(module_dir);
    // Declared before `shell`, which keeps a reference to the VFS until it is destroyed.
    std::unique_ptr<wse::platform::HostVfs> host_vfs;
#ifdef __ANDROID__
    std::unique_ptr<wse::platform::android_saf::RegisteredGameTree> saf_tree;
    JavaVM* jvm = nullptr;
    bool worker_attached = false;
#endif
    wse::platform::Vfs* vfs = nullptr;

    const auto log_load_result = [&](const char* status) {
        SDL_Log("[WSE2] module shell: %s %s (%s, %llu ms)",
                from_saf ? "(SAF tree)" : warband_root.c_str(), module_dir.c_str(), status,
                static_cast<unsigned long long>((SDL_GetTicksNS() - load_start_ns) / 1000000ULL));
    };
    const auto show_start_error = [&]() {
        // Missing Warband files (`find_missing_warband_files`), an unreadable SAF tree, or a
        // loader / `game_start` error: say so in a dialog, then return to the launcher.
        const std::string message =
            error.empty() ? std::string("The module could not be started.") : error;
        if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WSE2: cannot start the module",
                                      message.c_str(), window)) {
            SDL_Log("[WSE2] message box failed: %s", SDL_GetError());
        }
    };
    SdlGeometryPresenter presenter(renderer);
    frame_pacer.set_target_fps(30);
    bool quit = false;
    bool loading_dirty = true;
    int loading_out_w = 0;
    int loading_out_h = 0;
    float loading_fraction = -1.0f;
    std::string loading_stage;

    const auto pump_loading_events = [&]() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                quit = true;
            } else if (ev.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                presenter.drop_all();
                loading_dirty = true;
            } else if (ev.type == SDL_EVENT_RENDER_TARGETS_RESET ||
                       ev.type == SDL_EVENT_DID_ENTER_FOREGROUND ||
                       ev.type == SDL_EVENT_WINDOW_EXPOSED) {
                loading_dirty = true;
            }
            if (sdl_input) {
                sdl_input->push_sdl_event(&ev);
            }
        }
    };
    const auto present_loading_iteration = [&](const wse::platform::UiShellStartProgress& prog) {
        int win_w = 1;
        int win_h = 1;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int out_w = win_w;
        int out_h = win_h;
        SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
        const bool frac_changed =
            (std::isnan(prog.fraction) != std::isnan(loading_fraction)) ||
            (!std::isnan(prog.fraction) && std::fabs(prog.fraction - loading_fraction) > 1e-4f);
        if (loading_dirty || out_w != loading_out_w || out_h != loading_out_h || frac_changed ||
            prog.stage != loading_stage) {
            draw_loading_screen(renderer, out_w, out_h, module_name, prog);
            SDL_RenderPresent(renderer);
            loading_dirty = false;
            loading_out_w = out_w;
            loading_out_h = out_h;
            loading_fraction = prog.fraction;
            loading_stage = prog.stage;
        }
        const std::uint64_t wait_ticks =
            static_cast<std::uint64_t>(frame_pacer.remaining_wait_ticks());
        if (wait_ticks > 0U) {
            SDL_DelayNS(wait_ticks *
                        static_cast<std::uint64_t>(wse::platform::FramePacer::kNsPerTick));
        }
    };

    if (from_saf) {
#ifdef __ANDROID__
        auto* sdl_env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        if (sdl_env == nullptr || sdl_env->GetJavaVM(&jvm) != JNI_OK || jvm == nullptr) {
            error = "could not obtain JavaVM for SDL thread";
        } else {
            std::atomic<bool> saf_open_done{false};
            std::string saf_open_error;
            std::thread saf_open_thread([&]() {
                {
                    ScopedJniThread jni(jvm);
                    if (jni.env == nullptr) {
                        saf_open_error = "could not attach SAF open thread to JVM";
                    } else {
                        saf_tree = wse::platform::android_saf::RegisteredGameTree::open(
                            jni.env, &saf_open_error);
                    }
                }
                saf_open_done.store(true, std::memory_order_release);
            });
            const wse::platform::UiShellStartProgress saf_progress{0.02f, "Indexing game files"};
            while (!saf_open_done.load(std::memory_order_acquire) && !quit &&
                   !app.terminating.load()) {
                frame_pacer.begin_frame();
                pump_loading_events();
                if (quit || app.terminating.load()) {
                    break;
                }
                present_loading_iteration(saf_progress);
            }
            saf_open_thread.join();
            if (quit || app.terminating.load()) {
                if (saf_tree) {
                    saf_tree->rebind_thread(SDL_GetAndroidJNIEnv());
                }
                presenter.drop_all();
                return 0;
            }
            if (saf_tree) {
                vfs = &saf_tree->vfs();
                SDL_Log("[WSE2] SAF tree indexed: %zu files (%llu ms)", saf_tree->file_count(),
                        static_cast<unsigned long long>((SDL_GetTicksNS() - load_start_ns) /
                                                        1000000ULL));
            } else {
                error = saf_open_error.empty() ? std::string("failed to open SAF tree")
                                               : std::move(saf_open_error);
            }
        }
#else
        error = "--saf-module is Android-only";
#endif
    } else {
        host_vfs = std::make_unique<wse::platform::HostVfs>(fs::path(warband_root));
        vfs = host_vfs.get();
    }

    if (vfs == nullptr) {
        log_load_result(error.c_str());
        show_start_error();
        presenter.drop_all();
        return 2;
    }

    (void)perf.poll_launch_config(input_mgr, sdl_input);
    wse::platform::UiGameShellOptions opts{};
    opts.run.module_dir = module_dir;
    opts.run.load_options.native_dir = "Modules/Native";
    opts.run.load_options.root_data_dir = "Data";
    opts.run.load_options.require_core_files = true;
    opts.run.enable_lua = false;  // the app links no `wse_lua` runtime yet
    opts.language =
        resolve_install_language(*vfs, module_dir, perf.launch_cfg.settings.language);
    opts.max_texture_size = perf.launch_cfg.settings.max_texture_size;
    opts.ui_aspect = perf.launch_cfg.settings.ui_aspect;
    opts.ui_text_scale = perf.launch_cfg.settings.ui_scale;
    opts.output = wse::platform::UiGameShellOptions::Output::geometry;
#ifdef __ANDROID__
    if (from_saf) {
        opts.worker_thread_init = [jvm, &saf_tree, &worker_attached]() {
            JNIEnv* worker_env = attach_jni_thread(jvm, worker_attached);
            if (saf_tree) {
                saf_tree->rebind_thread(worker_env);
            }
        };
        opts.worker_thread_exit = [jvm, &saf_tree, &worker_attached]() {
            if (saf_tree) {
                saf_tree->rebind_thread(nullptr);
            }
            detach_jni_thread(jvm, worker_attached);
        };
    }
#endif

    wse::platform::UiGameShell shell;
    if (!shell.start_async(*vfs, std::move(opts))) {
        error = "could not start async module loader";
        log_load_result(error.c_str());
        show_start_error();
        presenter.drop_all();
        return 2;
    }

    wse::platform::UiShellStartState start_state = shell.poll_start();
    while (start_state == wse::platform::UiShellStartState::loading && !quit &&
           !app.terminating.load()) {
        frame_pacer.begin_frame();
        pump_loading_events();
        if (quit || app.terminating.load()) {
            break;
        }
        start_state = shell.poll_start();
        if (start_state != wse::platform::UiShellStartState::loading) {
            break;
        }
        present_loading_iteration(shell.start_progress());
    }

    if (quit || app.terminating.load()) {
        shell.cancel_start();
        shell.stop();
#ifdef __ANDROID__
        if (saf_tree) {
            saf_tree->rebind_thread(SDL_GetAndroidJNIEnv());
        }
#endif
        presenter.drop_all();
        return 0;
    }

    if (start_state != wse::platform::UiShellStartState::ready) {
        error = shell.start_error();
        log_load_result(error.c_str());
        show_start_error();
        shell.stop();
#ifdef __ANDROID__
        if (saf_tree) {
            saf_tree->rebind_thread(SDL_GetAndroidJNIEnv());
        }
#endif
        presenter.drop_all();
        return 2;
    }

#ifdef __ANDROID__
    if (saf_tree) {
        saf_tree->rebind_thread(SDL_GetAndroidJNIEnv());
    }
#endif
    log_load_result("started");

    wse::platform::PerformanceAdaptation module_adapt{};
    bool target_changed = false;
    (void)perf.step(window, renderer, frame_pacer, input_mgr, sdl_input, &target_changed,
                    &module_adapt);
    if (target_changed) {
        log_module_fps_target(module_adapt);
    }

    std::uint64_t presented_serial = ~std::uint64_t{0};
    int presented_w = 0;
    int presented_h = 0;
    bool presented_stopped = false;
    std::vector<wse::platform::RawInputEvent> events;
    if (sdl_input) {
        sdl_input->poll_events(events);
        events.clear();
    }
    std::uint64_t last_ns = SDL_GetTicksNS();
    bool running = true;
    const auto target_ns_for_fps = [](std::int32_t fps) noexcept -> std::int64_t {
        return fps > 0 ? wse::platform::FramePacer::kNsPerSecond / fps : 0;
    };
    std::int32_t perf_target_fps = frame_pacer.target_fps();
    auto perf_session = wse::platform::android_perf::PerformanceHintSession::create(
        target_ns_for_fps(perf_target_fps));
    if (perf_session) {
        SDL_Log("[WSE2] performance hint session active (target %lld ns)",
                static_cast<long long>(target_ns_for_fps(perf_target_fps)));
    } else if (wse::platform::android_perf::raise_current_thread_priority()) {
        SDL_Log("[WSE2] performance hint unavailable; raised thread priority");
    } else {
        SDL_Log("[WSE2] performance hint unavailable; default thread priority");
    }
    FpsOverlayState fps_overlay;
    bool text_input_active = false;
    // Touch overlay (P11.3): the user's layout, clamped into the window's safe area whenever
    // the window or its safe area changes. The clamped copy lives in `input_mgr` so taps are
    // hit-tested against the same rectangles the shell draws.
    const auto base_widgets =
        input_mgr != nullptr ? input_mgr->profile().widgets
                             : wse::platform::TouchOverlayProfile{}.widgets;
    wse::platform::SafeAreaInsets overlay_insets{};
    int overlay_win_w = 0;
    int overlay_win_h = 0;
    bool overlay_ready = false;
    while (!quit && !app.terminating.load()) {
        bool step_target_changed = false;
        const std::int32_t effective_fps =
            perf.step(window, renderer, frame_pacer, input_mgr, sdl_input,
                      &step_target_changed, &module_adapt);
        if (step_target_changed) {
            log_module_fps_target(module_adapt);
        }
        frame_pacer.begin_frame();
        const std::uint64_t work_start_ns = SDL_GetTicksNS();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                quit = true;
            } else if (ev.type == SDL_EVENT_TEXT_INPUT && ev.text.text != nullptr) {
                shell.feed_text(ev.text.text);
            } else if (ev.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                presenter.drop_all();  // every texture is gone; recreated on the next draw
                presented_serial = ~std::uint64_t{0};
            } else if (ev.type == SDL_EVENT_RENDER_TARGETS_RESET ||
                       ev.type == SDL_EVENT_DID_ENTER_FOREGROUND ||
                       ev.type == SDL_EVENT_WINDOW_EXPOSED) {
                presented_serial = ~std::uint64_t{0};  // the window contents may be gone
            }
            if (sdl_input) {
                sdl_input->push_sdl_event(&ev);
            }
        }
        int win_w = 1;
        int win_h = 1;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int out_w = win_w;
        int out_h = win_h;
        SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
        if (input_mgr != nullptr) {
            const int w = std::max(win_w, 1);
            const int h = std::max(win_h, 1);
            SDL_Rect safe{0, 0, w, h};
            if (!SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 || safe.h <= 0) {
                safe = {0, 0, w, h};
            }
            const float fw = static_cast<float>(w);
            const float fh = static_cast<float>(h);
            const wse::platform::SafeAreaInsets insets{
                std::clamp(static_cast<float>(safe.x) / fw, 0.0f, 1.0f),
                std::clamp(static_cast<float>(safe.y) / fh, 0.0f, 1.0f),
                std::clamp(static_cast<float>(w - safe.x - safe.w) / fw, 0.0f, 1.0f),
                std::clamp(static_cast<float>(h - safe.y - safe.h) / fh, 0.0f, 1.0f),
            };
            if (!overlay_ready || insets != overlay_insets || w != overlay_win_w ||
                h != overlay_win_h) {
                overlay_ready = true;
                overlay_insets = insets;
                overlay_win_w = w;
                overlay_win_h = h;
                input_mgr->set_viewport_size(w, h);
                auto& prof = input_mgr->profile();
                prof.widgets = base_widgets;
                wse::platform::apply_safe_area(prof, insets, fw / fh);
                shell.set_touch_overlay(&input_mgr->profile(), shell.world_input_context(),
                                        insets, &input_mgr->frame_state());
            }
        }
        const std::uint64_t now_ns = SDL_GetTicksNS();
        const float dt = static_cast<float>(now_ns - last_ns) * 1e-9f;
        last_ns = now_ns;
        if (sdl_input) {
            sdl_input->set_viewport_size(std::max(win_w, 1), std::max(win_h, 1));
            sdl_input->poll_events(events);
            if (input_mgr != nullptr) {
                shell.step_input(*input_mgr, events, dt);
            } else {
                for (const auto& e : events) {
                    shell.feed(e);
                }
            }
        }
        if (running) {
            running = shell.frame(dt, static_cast<std::uint32_t>(std::max(out_w, 1)),
                                  static_cast<std::uint32_t>(std::max(out_h, 1)));
            if (!running) {
                error = shell.report().last_error;
                SDL_Log("[WSE2] module shell stopped: %s", error.c_str());
            }
        }
        const bool want_text_input = running && shell.text_input_wanted();
        if (want_text_input != text_input_active) {
            text_input_active = want_text_input;
            if (text_input_active) {
                SDL_StartTextInput(window);
            } else {
                SDL_StopTextInput(window);
            }
        }

        const bool ui_changed = presented_serial != shell.frame_serial() || presented_w != out_w ||
                                presented_h != out_h || presented_stopped != !running;
        const std::uint64_t pre_draw_ns = SDL_GetTicksNS();
        const bool overlay_due =
            show_fps_overlay && running &&
            fps_overlay.update_if_due(pre_draw_ns, perf.frame_stats, frame_pacer.target_fps(),
                                      module_adapt.tier);
        const bool changed = ui_changed || overlay_due;
        if (changed) {
            const wse::platform::GeometryFrame* frame = shell.geometry();
            if (frame != nullptr && shell.geometry_device() != nullptr) {
                (void)presenter.draw(*frame, *shell.geometry_device());
            } else {
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
            }
            if (!running) {
                const float scale =
                    std::clamp(std::floor(static_cast<float>(out_w) / 340.0f), 1.0f, 6.0f);
                SDL_SetRenderScale(renderer, scale, scale);
                draw_filled_rect(renderer, 0.0f, 0.0f, static_cast<float>(out_w) / scale, 30.0f,
                                 150, 34, 34);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDebugText(renderer, 6.0f, 6.0f, "WSE2 module stopped");
                const std::string line =
                    error.size() > 80U ? error.substr(0U, 77U) + "..." : error;
                SDL_RenderDebugText(renderer, 6.0f, 18.0f, line.c_str());
            } else if (show_fps_overlay) {
                draw_fps_overlay(renderer, window, out_w, out_h, fps_overlay);
            }
            if (show_fps_overlay) {
                const std::uint64_t cpu_end_ns = SDL_GetTicksNS();
                fps_overlay.record_cpu(cpu_end_ns > work_start_ns ? cpu_end_ns - work_start_ns
                                                                  : 0ULL);
            }
            SDL_RenderPresent(renderer);
            presented_serial = shell.frame_serial();
            presented_w = out_w;
            presented_h = out_h;
            presented_stopped = !running;
        } else if (show_fps_overlay) {
            fps_overlay.record_cpu(pre_draw_ns > work_start_ns ? pre_draw_ns - work_start_ns
                                                               : 0ULL);
        }
        const std::uint64_t after_present_ns = SDL_GetTicksNS();
        const std::int32_t current_fps = frame_pacer.target_fps();
        if (current_fps != perf_target_fps) {
            perf_target_fps = current_fps;
            if (perf_session) {
                perf_session->update_target(target_ns_for_fps(current_fps));
            } else if (current_fps > 0) {
                perf_session = wse::platform::android_perf::PerformanceHintSession::create(
                    target_ns_for_fps(current_fps));
            }
        }
        if (perf_session) {
            if (after_present_ns > work_start_ns) {
                const std::uint64_t elapsed_ns = after_present_ns - work_start_ns;
                perf_session->report_actual(static_cast<std::int64_t>(
                    std::min<std::uint64_t>(elapsed_ns, 1000000000ULL)));
            }
        }
        perf.frame_stats.record_frame(static_cast<std::int64_t>(after_present_ns));
        const std::uint64_t wait_ticks =
            static_cast<std::uint64_t>(frame_pacer.remaining_wait_ticks());
        if (wait_ticks > 0U && effective_fps > 0) {
            SDL_DelayNS(wait_ticks *
                        static_cast<std::uint64_t>(wse::platform::FramePacer::kNsPerTick));
        }
    }
    if (text_input_active) {
        SDL_StopTextInput(window);
    }
    presenter.drop_all();
    shell.stop();
    return running || error.empty() ? 0 : 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // The SDL3 audio stream stays open for the app's lifetime: a `WseAudioMixer` mixes on
    // `AudioMixerWorker`'s thread (no sounds are loaded until a game world drives a
    // `WorldAudioBridge` through `audio_worker->lock()`), and the lifecycle watch pauses it
    // while the activity is in the background. Audio is optional (non-fatal).
    auto audio_dev = wse::platform::create_sdl_audio_device();
    wse::platform::WseAudioMixer audio_mixer{};
    std::unique_ptr<wse::platform::AudioMixerWorker> audio_worker;
    if (audio_dev && audio_dev->open(wse::platform::AudioDeviceSpec{48000U, 2U})) {
        audio_mixer.set_output_sample_rate(audio_dev->spec().sample_rate);
        audio_worker = std::make_unique<wse::platform::AudioMixerWorker>(audio_mixer, *audio_dev);
        const bool started = audio_worker->start();
        SDL_Log("SDL3 audio stream ready (%s, %u Hz, mixer thread %s)", audio_dev->backend_name(),
                audio_dev->spec().sample_rate, started ? "running" : "not started");
    } else {
        SDL_Log("SDL3 audio unavailable (non-fatal)");
    }
    auto sdl_input = wse::platform::create_sdl_input_backend();
    wse::platform::WseInputManager input_mgr;
    if (sdl_input) {
        SDL_Log("SDL3 input backend ready (%s)", sdl_input->backend_name());
    }
    wse::platform::SteadyClock pacer_clock;
    wse::platform::FramePacer frame_pacer(pacer_clock);
    PerfLoopState perf;
    // `config/performance.json` is the one saved copy of these settings: on Android
    // `WseActivity` loads it (`nativeApplyPerformanceSettings`) into the hub; elsewhere it is
    // read from SDL's per-user preference directory.
#ifndef __ANDROID__
    if (char* pref = SDL_GetPrefPath("wse2", "wse2-android"); pref != nullptr) {
        wse::platform::HostVfs pref_vfs{std::filesystem::path(pref)};
        SDL_free(pref);
        wse::platform::RuntimeLaunchPerfConfig desktop_cfg{};
        bool present = false;
        if (wse::platform::load_performance_settings_or_default(pref_vfs, desktop_cfg.settings,
                                                                &present) &&
            present) {
            wse::platform::RuntimePerformanceHub::global().set_launch_config(desktop_cfg);
        }
    }
#endif
    (void)perf.poll_launch_config(&input_mgr, sdl_input.get());

    // `--module <warband_root> [module_rel_dir]` runs a module from a directory,
    // `--saf-module <module_rel_dir>` runs one from the SAF tree `WseActivity` registered
    // (`run_module_shell`), and `--show-fps` enables the module FPS/thermal debug overlay;
    // otherwise an optional first argument overrides the self-test fixture path.
    std::string cli_fixture;
    std::string module_root;
    std::string module_rel = "Modules/Native";
    bool module_mode = false;
    bool module_from_saf = false;
    bool show_fps_overlay = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr || argv[i][0] == '\0') {
            continue;
        }
        const std::string_view arg(argv[i]);
        if (arg == "--show-fps") {
            show_fps_overlay = true;
        } else if (arg == "--module" && i + 1 < argc && argv[i + 1] != nullptr &&
                   argv[i + 1][0] != '\0') {
            module_mode = true;
            module_from_saf = false;
            module_root = argv[++i];
            if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '\0') {
                const std::string_view next(argv[i + 1]);
                if (next != "--show-fps" && next != "--module" && next != "--saf-module") {
                    module_rel = argv[++i];
                }
            }
        } else if (arg == "--saf-module" && i + 1 < argc && argv[i + 1] != nullptr &&
                   argv[i + 1][0] != '\0') {
            module_mode = true;
            module_from_saf = true;
            module_rel = argv[++i];
        } else if (!module_mode && cli_fixture.empty()) {
            cli_fixture = std::string(arg);
        }
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
#ifdef __ANDROID__
    constexpr SDL_WindowFlags kWindowFlags = SDL_WINDOW_FULLSCREEN;
    constexpr int kInitW = 0;
    constexpr int kInitH = 0;
#else
    constexpr SDL_WindowFlags kWindowFlags = SDL_WINDOW_RESIZABLE;
    constexpr int kInitW = 800;
    constexpr int kInitH = 1000;
#endif
    // Warband is a landscape game: module mode locks the window to either landscape rotation
    // (`WseActivity` also requests sensor-landscape before SDL starts, so the first frame is
    // already landscape); the self-test list stays free to rotate.
    if (module_mode) {
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    }
    const int init_w = module_mode && kInitW != 0 ? 1280 : kInitW;
    const int init_h = module_mode && kInitH != 0 ? 720 : kInitH;
    if (!SDL_CreateWindowAndRenderer(module_mode ? "WSE2" : "WSE2 Engine Self-Test", init_w,
                                      init_h, kWindowFlags, &window, &renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    UiState ui;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> rerun_requested{true};
    AppLifecycle app;
    app.audio = audio_worker.get();
    std::atomic<bool>& terminating = app.terminating;
    SDL_AddEventWatch(on_app_event, &app);

    if (module_mode) {
        const int rc = run_module_shell(window, renderer, sdl_input.get(), &input_mgr, app,
                                        frame_pacer, perf, module_root, module_rel,
                                        module_from_saf, show_fps_overlay);
        SDL_RemoveEventWatch(on_app_event, &app);
        if (audio_worker) {
            audio_worker->stop();
        }
        if (audio_dev) {
            audio_dev->close();
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return rc;
    }

    std::thread worker([&]() {
        while (!stop_requested.load()) {
            if (!rerun_requested.exchange(false)) {
                SDL_Delay(50);
                continue;
            }

            std::string prep_err;
            RuntimePaths paths = prepare_runtime_paths(prep_err);
            if (!cli_fixture.empty()) {
                paths.fixture_path = cli_fixture;
            }

            {
                std::lock_guard<std::mutex> lock(ui.mutex);
                ui.finished = false;
                ui.all_passed = false;
                ui.completed_count = 0;
                ui.passed_count = 0;
                ui.total_count = 20;
                ui.total_ms = 0;
                ui.active_name.clear();
                ui.note = prep_err;
                ui.rows.clear();
                ui.failure_lines.clear();
            }

#ifdef __ANDROID__
            if (!paths.log_path.empty()) {
                if (std::freopen(paths.log_path.c_str(), "w", stdout) == nullptr) {
                    SDL_Log("[WSE2] warning: failed to redirect stdout to %s",
                            paths.log_path.c_str());
                }
            }
#endif

            const wse::selftest::SuiteSummary summary = wse::selftest::run_all_suites(
                paths.fixture_path.c_str(),
                [&](int index, int total, const char* suite_name,
                    const wse::selftest::SuiteResult* completed) {
                    std::lock_guard<std::mutex> lock(ui.mutex);
                    ui.total_count = total;
                    if (static_cast<int>(ui.rows.size()) < total) {
                        ui.rows.resize(static_cast<std::size_t>(total));
                    }
                    RowEntry& row = ui.rows[static_cast<std::size_t>(index)];
                    row.name = suite_name;
                    if (completed == nullptr) {
                        row.status = RowStatus::running;
                        ui.active_name = suite_name;
                    } else {
                        row.status = completed->passed ? RowStatus::pass : RowStatus::fail;
                        row.duration_ms = completed->duration_ms;
                        ++ui.completed_count;
                        if (completed->passed) {
                            ++ui.passed_count;
                        }
                        std::fflush(stdout);
                        SDL_Log("[WSE2] [%s] %s (%lld ms)",
                                completed->passed ? "PASS" : "FAIL",
                                completed->name,
                                static_cast<long long>(completed->duration_ms));
                    }
                },
                // Checked between suites so Back/quit does not wait for the
                // whole run to finish.
                [&]() { return stop_requested.load() || terminating.load(); });

            std::fflush(stdout);
            if (summary.cancelled) {
                break;
            }
            std::vector<std::string> fails;
            collect_failure_lines(paths.log_path, fails);
            for (const std::string& fl : fails) {
                SDL_Log("[WSE2] %s", fl.c_str());
            }

            {
                std::lock_guard<std::mutex> lock(ui.mutex);
                ui.finished = true;
                ui.all_passed = summary.all_passed();
                ui.passed_count = summary.passed_count;
                ui.total_count = summary.total_count;
                ui.total_ms = summary.total_ms;
                ui.active_name.clear();
                ui.failure_lines = std::move(fails);
            }

            SDL_Log("[WSE2] ENGINE SELF-TEST: %s (%d/%d passed, %lld ms)",
                    summary.all_passed() ? "PASS" : "FAIL",
                    summary.passed_count, summary.total_count,
                    static_cast<long long>(summary.total_ms));
        }
    });

    bool quit = false;
    float scroll_y = 0.0f;
    float last_finger_y = 0.0f;
    bool finger_dragging = false;
    SDL_FRect rerun_btn{0.0f, 0.0f, 0.0f, 0.0f};
    int last_win_w = 1;
    int last_win_h = 1;
    // Window points -> logical UI units (the UI is drawn under SDL_SetRenderScale).
    float points_to_logical = 1.0f;
    std::uint64_t last_input_ns = SDL_GetTicksNS();

    while (!quit) {
        if (terminating.load()) {
            break;
        }
        (void)perf.step(window, renderer, frame_pacer, &input_mgr, sdl_input.get());
        frame_pacer.begin_frame();
        SDL_Event ev;
        bool was_finished = false;
        {
            std::lock_guard<std::mutex> lock(ui.mutex);
            was_finished = ui.finished;
        }
        if (was_finished) {
            if (SDL_WaitEventTimeout(&ev, 100)) {
                do {
                    if (sdl_input) {
                        sdl_input->push_sdl_event(&ev);
                    }
                    if (ev.type == SDL_EVENT_QUIT ||
                        ev.type == SDL_EVENT_TERMINATING) {
                        quit = true;
                    } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                        if (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_AC_BACK) {
                            quit = true;
                        } else if (ev.key.key == SDLK_R && was_finished) {
                            scroll_y = 0.0f;
                            rerun_requested.store(true);
                        }
                    } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                        scroll_y -= ev.wheel.y * 24.0f;
                    } else if (ev.type == SDL_EVENT_FINGER_DOWN) {
                        last_finger_y = ev.tfinger.y * static_cast<float>(last_win_h);
                        finger_dragging = false;
                    } else if (ev.type == SDL_EVENT_FINGER_MOTION) {
                        const float cur_y = ev.tfinger.y * static_cast<float>(last_win_h);
                        const float dy = cur_y - last_finger_y;
                        if (std::fabs(dy) > 2.0f) {
                            finger_dragging = true;
                            scroll_y -= dy * points_to_logical;
                            last_finger_y = cur_y;
                        }
                    } else if (ev.type == SDL_EVENT_FINGER_UP) {
                        if (!finger_dragging && was_finished) {
                            const float fx = ev.tfinger.x * static_cast<float>(last_win_w);
                            const float fy = ev.tfinger.y * static_cast<float>(last_win_h);
                            if (point_in_rect(fx, fy, rerun_btn)) {
                                scroll_y = 0.0f;
                                rerun_requested.store(true);
                            }
                        }
                    } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && was_finished &&
                               ev.button.which != SDL_TOUCH_MOUSEID) {
                        // Touches also arrive as synthesized mouse events
                        // (SDL_HINT_TOUCH_MOUSE_EVENTS); those are handled by
                        // the finger path above, which knows about drags.
                        if (point_in_rect(ev.button.x, ev.button.y, rerun_btn)) {
                            scroll_y = 0.0f;
                            rerun_requested.store(true);
                        }
                    }
                } while (SDL_PollEvent(&ev));
            }
        } else {
            while (SDL_PollEvent(&ev)) {
                if (sdl_input) {
                    sdl_input->push_sdl_event(&ev);
                }
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                    quit = true;
                } else if (ev.type == SDL_EVENT_KEY_DOWN &&
                           (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_AC_BACK)) {
                    quit = true;
                }
            }
        }

        int win_w = 800;
        int win_h = 1000;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int out_w = win_w;
        int out_h = win_h;
        SDL_GetRenderOutputSize(renderer, &out_w, &out_h);

        SDL_Rect safe{0, 0, win_w, win_h};
        if (!SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 || safe.h <= 0) {
            safe = {0, 0, win_w, win_h};
        }
        last_win_w = std::max(win_w, 1);
        last_win_h = std::max(win_h, 1);
        if (sdl_input) {
            const std::uint64_t now_ns = SDL_GetTicksNS();
            const float dt = static_cast<float>(now_ns - last_input_ns) * 1e-9f;
            last_input_ns = now_ns;
            sdl_input->set_viewport_size(last_win_w, last_win_h);
            input_mgr.set_viewport_size(last_win_w, last_win_h);
            input_mgr.set_mouse_look_enabled(SDL_GetWindowRelativeMouseMode(window));
            input_mgr.step(*sdl_input, dt);
        }
        const float px_scale_x =
            win_w > 0 ? static_cast<float>(out_w) / static_cast<float>(win_w) : 1.0f;
        const float px_scale_y =
            win_h > 0 ? static_cast<float>(out_h) / static_cast<float>(win_h) : 1.0f;
        const float safe_x = static_cast<float>(safe.x) * px_scale_x;
        const float safe_y = static_cast<float>(safe.y) * px_scale_y;
        const float safe_w = static_cast<float>(safe.w) * px_scale_x;
        const float safe_h = static_cast<float>(safe.h) * px_scale_y;

        const float scale =
            std::clamp(std::floor(safe_w / 340.0f), 1.0f, 6.0f);
        const float lw = safe_w / scale;
        const float lh = safe_h / scale;
        const float ox = safe_x / scale;
        const float oy = safe_y / scale;
        points_to_logical = px_scale_y / scale;

        SDL_SetRenderScale(renderer, scale, scale);
        SDL_SetRenderDrawColor(renderer, 16, 20, 26, 255);
        SDL_RenderClear(renderer);

        // Snapshot UI state
        bool finished = false;
        bool all_passed = false;
        int completed_count = 0;
        int passed_count = 0;
        int total_count = 21;
        std::int64_t total_ms = 0;
        std::string active_name;
        std::string note;
        std::vector<RowEntry> rows;
        std::vector<std::string> failure_lines;
        {
            std::lock_guard<std::mutex> lock(ui.mutex);
            finished = ui.finished;
            all_passed = ui.all_passed;
            completed_count = ui.completed_count;
            passed_count = ui.passed_count;
            total_count = ui.total_count;
            total_ms = ui.total_ms;
            active_name = ui.active_name;
            note = ui.note;
            rows = ui.rows;
            failure_lines = ui.failure_lines;
        }

        const float pad = 10.0f;
        const float banner_h = 44.0f;
        if (!finished) {
            draw_filled_rect(renderer, ox, oy, lw, banner_h, 29, 64, 118);
        } else if (all_passed) {
            draw_filled_rect(renderer, ox, oy, lw, banner_h, 24, 110, 56);
        } else {
            draw_filled_rect(renderer, ox, oy, lw, banner_h, 150, 34, 34);
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer, ox + pad, oy + 8.0f,
                            "WSE2-Android Self-Test (arm64 / host)");
        char summary_buf[96];
        if (!finished) {
            std::snprintf(summary_buf, sizeof(summary_buf),
                          "RUNNING %d/%d: %s", completed_count, total_count,
                          active_name.empty() ? "preparing..." : active_name.c_str());
        } else {
            std::snprintf(summary_buf, sizeof(summary_buf),
                          "RESULT: %s (%d/%d passed, %lld ms)",
                          all_passed ? "PASS" : "FAIL", passed_count, total_count,
                          static_cast<long long>(total_ms));
        }
        SDL_RenderDebugText(renderer, ox + pad, oy + 24.0f, summary_buf);

        const float footer_h = 28.0f;
        const float body_top = oy + banner_h + 8.0f;
        const float body_bot = oy + lh - footer_h;
        const float row_h = 13.0f;
        const int extra_lines =
            (note.empty() ? 0 : 1) +
            (failure_lines.empty() ? 0 : static_cast<int>(failure_lines.size()) + 1);
        const float content_h =
            static_cast<float>(total_count + extra_lines) * row_h + 8.0f;
        const float max_scroll = std::max(0.0f, content_h - (body_bot - body_top));
        scroll_y = std::clamp(scroll_y, 0.0f, max_scroll);

        // The clip rectangle is in the same (scaled) units as the draw calls:
        // SDL multiplies it by the render scale.
        SDL_Rect clip_rect{
            static_cast<int>(std::floor(ox)),
            static_cast<int>(std::floor(oy + banner_h)),
            std::max(1, static_cast<int>(std::ceil(lw))),
            std::max(1, static_cast<int>(std::ceil(lh - banner_h - footer_h))),
        };
        SDL_SetRenderClipRect(renderer, &clip_rect);

        float y = body_top - scroll_y;
        if (!note.empty()) {
            SDL_SetRenderDrawColor(renderer, 240, 190, 70, 255);
            SDL_RenderDebugText(renderer, ox + pad, y, note.c_str());
            y += row_h;
        }

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const RowEntry& row = rows[static_cast<std::size_t>(i)];
            const char* badge = "WAIT";
            if (row.status == RowStatus::running) {
                badge = "RUN ";
                SDL_SetRenderDrawColor(renderer, 245, 210, 80, 255);
            } else if (row.status == RowStatus::pass) {
                badge = "PASS";
                SDL_SetRenderDrawColor(renderer, 110, 235, 145, 255);
            } else if (row.status == RowStatus::fail) {
                badge = "FAIL";
                SDL_SetRenderDrawColor(renderer, 255, 115, 115, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 140, 150, 165, 255);
            }

            char line_buf[96];
            if (row.status == RowStatus::pass || row.status == RowStatus::fail) {
                std::snprintf(line_buf, sizeof(line_buf), "%2d. [%s] %-16s %5lld ms",
                              i + 1, badge, row.name.c_str(),
                              static_cast<long long>(row.duration_ms));
            } else {
                std::snprintf(line_buf, sizeof(line_buf), "%2d. [%s] %s",
                              i + 1, badge, row.name.c_str());
            }
            SDL_RenderDebugText(renderer, ox + pad, y, line_buf);
            y += row_h;
        }

        if (!failure_lines.empty()) {
            y += row_h * 0.5f;
            SDL_SetRenderDrawColor(renderer, 255, 115, 115, 255);
            SDL_RenderDebugText(renderer, ox + pad, y, "--- Failure details ---");
            y += row_h;
            for (const std::string& fl : failure_lines) {
                SDL_RenderDebugText(renderer, ox + pad, y, fl.c_str());
                y += row_h;
            }
        }
        SDL_SetRenderClipRect(renderer, nullptr);

        // Footer bar + Re-run button (in logical & pixel coordinates)
        const auto adaptation = perf.evaluate();
        draw_filled_rect(renderer, ox, body_bot, lw, footer_h, 24, 29, 38);
        SDL_SetRenderDrawColor(renderer, 185, 195, 210, 255);
        // Cap, measured rate and its 1% low (what the device actually delivers), and the
        // thermal tier with its resolution scale; short enough for a 340-unit footer.
        char footer_buf[96];
        char cap_buf[16];
        const auto tier_sv = wse::platform::thermal_tier_name(adaptation.tier);
        if (adaptation.effective_fps_cap > 0) {
            std::snprintf(cap_buf, sizeof(cap_buf), "%dHz", adaptation.effective_fps_cap);
        } else {
            std::snprintf(cap_buf, sizeof(cap_buf), "uncap");
        }
        if (perf.frame_stats.sample_count() >= 10U) {
            std::snprintf(footer_buf, sizeof(footer_buf), "%s %.1ffps 1%%:%.0f %.*s %.2fx",
                          cap_buf, perf.frame_stats.average_fps(), perf.frame_stats.low_1pct_fps(),
                          static_cast<int>(tier_sv.size()), tier_sv.data(),
                          static_cast<double>(adaptation.resolution_scale));
        } else {
            std::snprintf(footer_buf, sizeof(footer_buf), "Back: exit | %s %.*s %.2fx", cap_buf,
                          static_cast<int>(tier_sv.size()), tier_sv.data(),
                          static_cast<double>(adaptation.resolution_scale));
        }
        SDL_RenderDebugText(renderer, ox + pad, body_bot + 10.0f, footer_buf);

        const float btn_w = 82.0f;
        const float btn_h = 20.0f;
        const float btn_x = ox + lw - btn_w - pad;
        const float btn_y = body_bot + 4.0f;
        if (finished) {
            draw_filled_rect(renderer, btn_x, btn_y, btn_w, btn_h, 46, 98, 168);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, btn_x + 10.0f, btn_y + 6.0f, "RE-RUN");
            rerun_btn = {btn_x * scale / px_scale_x, btn_y * scale / px_scale_y,
                         btn_w * scale / px_scale_x, btn_h * scale / px_scale_y};
        } else {
            rerun_btn = {0.0f, 0.0f, 0.0f, 0.0f};
        }

        SDL_RenderPresent(renderer);
        perf.frame_stats.record_frame(static_cast<std::int64_t>(SDL_GetTicksNS()));
        const std::uint64_t wait_ticks =
            static_cast<std::uint64_t>(frame_pacer.remaining_wait_ticks());
        if (wait_ticks > 0U && adaptation.effective_fps_cap > 0) {
            SDL_DelayNS(wait_ticks *
                        static_cast<std::uint64_t>(wse::platform::FramePacer::kNsPerTick));
        }
    }

    stop_requested.store(true);
    worker.join();
    SDL_RemoveEventWatch(on_app_event, &app);
    if (audio_worker) {
        audio_worker->stop();
    }
    if (audio_dev) {
        audio_dev->close();
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
