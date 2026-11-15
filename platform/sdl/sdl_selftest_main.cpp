// P1 on-device SDL3 self-test shell.
//
// Runs all 18 portable engine test suites in-process behind an SDL3 window and
// displays live PASS/FAIL results alongside Android logcat output
// (docs/PLATFORM_PLAN.md, slice P1).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "wse_selftest.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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
    int total_count = 18;
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

// SDL documents that SDL_EVENT_TERMINATING must be handled from an event
// watch: by the time it could be polled the OS may already have killed the
// process. The watch only raises a flag the main loop and worker observe.
bool SDLCALL on_app_event(void* userdata, SDL_Event* event) {
    if (event->type == SDL_EVENT_TERMINATING) {
        static_cast<std::atomic<bool>*>(userdata)->store(true);
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

}  // namespace

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    std::string cli_fixture;
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        cli_fixture = argv[1];
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
    if (!SDL_CreateWindowAndRenderer("WSE2 Engine Self-Test", kInitW, kInitH,
                                      kWindowFlags, &window, &renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    UiState ui;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> rerun_requested{true};
    std::atomic<bool> terminating{false};
    SDL_AddEventWatch(on_app_event, &terminating);

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
                ui.total_count = 18;
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

    while (!quit) {
        if (terminating.load()) {
            break;
        }
        SDL_Event ev;
        bool was_finished = false;
        {
            std::lock_guard<std::mutex> lock(ui.mutex);
            was_finished = ui.finished;
        }
        if (was_finished) {
            if (SDL_WaitEventTimeout(&ev, 100)) {
                do {
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
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                    quit = true;
                } else if (ev.type == SDL_EVENT_KEY_DOWN &&
                           (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_AC_BACK)) {
                    quit = true;
                }
            }
            SDL_Delay(16);
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
        int total_count = 18;
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
        draw_filled_rect(renderer, ox, body_bot, lw, footer_h, 24, 29, 38);
        SDL_SetRenderDrawColor(renderer, 185, 195, 210, 255);
        SDL_RenderDebugText(renderer, ox + pad, body_bot + 10.0f,
                            "Back: exit | Drag: scroll");

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
    }

    stop_requested.store(true);
    worker.join();
    SDL_RemoveEventWatch(on_app_event, &terminating);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
