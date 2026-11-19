// USER32 (plus the IMM32 and GDI32 imports) for translated code, headless (Phase 5 R4,
// `docs/RECOMP_PLAN.md` "R4 design": "Fake HWND, headless: CreateWindowExW returns a fake window
// whose WndProc the shims call; the message queue is ours (fed from SDL events later)";
// v1.1.5.6 imports 40 USER32 functions, 71 call sites):
//
//   RegisterClassExW CreateWindowExW DestroyWindow DefWindowProcW ShowWindow UpdateWindow
//   SetWindowPos GetWindowRect GetClientRect AdjustWindowRect ClientToScreen ScreenToClient
//   GetWindowLongA SetWindowLongA SetActiveWindow SetForegroundWindow SetRect
//   GetMessageW PeekMessageW TranslateMessage DispatchMessageW SendMessageA PostQuitMessage
//   TranslateAcceleratorA LoadAcceleratorsA DestroyAcceleratorTable
//   GetCursorPos SetCursorPos ClipCursor SetCursor LoadCursorA LoadIconA GetSystemMetrics
//   OpenClipboard CloseClipboard EmptyClipboard IsClipboardFormatAvailable GetClipboardData
//   SetClipboardData MessageBoxA
//   ImmGetContext ImmReleaseContext ImmAssociateContext ImmAssociateContextEx
//   ImmGetCompositionStringW (IMM32)          GetStockObject (GDI32)
//
// Windows are records: class, procedure, styles, position and size, the window longs. Creating one
// sends WM_NCCREATE and WM_CREATE (with a CREATESTRUCTW) to its procedure; showing it sends
// WM_SHOWWINDOW and WM_SIZE; destroying it sends WM_DESTROY and WM_NCDESTROY. Each guest thread
// (a host thread) has its own message queue, as on Windows; `post` lets the embedder queue input
// for a window. Screen and cursor come from `DisplayConfig`. Deviations (documented in
// `engine/README.md`): no non-client area (a window's client size is its size, AdjustWindowRect
// leaves the rectangle as it is), no painting, no keyboard translation, no IME (ImmGetContext
// returns NULL), MessageBoxA reports to the embedder and answers IDOK. Header-only.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

// Window messages the shims send or that tests post.
inline constexpr std::uint32_t kWmCreate = 0x0001, kWmDestroy = 0x0002, kWmSize = 0x0005, kWmClose = 0x0010,
                               kWmQuit = 0x0012, kWmShowWindow = 0x0018, kWmNcCreate = 0x0081, kWmNcDestroy = 0x0082,
                               kWmKeyDown = 0x0100, kWmChar = 0x0102, kWmMouseMove = 0x0200, kWmUser = 0x0400;
inline constexpr std::uint32_t kPmRemove = 1;
inline constexpr std::uint32_t kCwUseDefault = 0x80000000U;
inline constexpr int kGwlWndProc = -4, kGwlStyle = -16, kGwlExStyle = -20, kGwlUserData = -21, kGwlId = -12;
inline constexpr std::uint32_t kErrorInvalidWindowHandle = 1400, kErrorClassAlreadyExists = 1410,
                               kErrorCannotFindWndClass = 1407;

struct DisplayConfig {
    int screen_width = 1920;
    int screen_height = 1080;
};

class User32 {
public:
    struct Msg {
        std::uint64_t hwnd = 0;
        std::uint32_t message = 0;
        std::uint64_t wparam = 0;
        std::uint64_t lparam = 0;
        std::uint32_t time = 0;
    };
    struct Window {
        std::uint64_t hwnd = 0;
        std::u16string class_name;
        std::uint64_t proc = 0;  // guest WNDPROC
        std::uint32_t style = 0;
        std::uint32_t ex_style = 0;
        int x = 0, y = 0, width = 0, height = 0;
        bool visible = false;
        std::map<int, std::uint64_t> longs;  // GWL_USERDATA and anything else set
        std::thread::id owner;
    };

    User32(Runtime& rt, DisplayConfig display = {}) : rt_(rt), display_(display), start_(std::chrono::steady_clock::now()) {}
    User32(const User32&) = delete;
    User32& operator=(const User32&) = delete;

    std::function<void(const std::string& caption, const std::string& text)> on_message_box;

    // Queues a message for `hwnd`'s thread (the embedder's input path). False for an unknown window.
    bool post(std::uint64_t hwnd, std::uint32_t message, std::uint64_t wparam, std::uint64_t lparam) {
        std::thread::id owner;
        {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = windows_.find(hwnd);
            if (it == windows_.end()) return false;
            owner = it->second.owner;
        }
        enqueue(owner, Msg{hwnd, message, wparam, lparam, now_ms()});
        return true;
    }
    [[nodiscard]] std::vector<std::uint64_t> windows() const {
        const std::lock_guard<std::mutex> lock(m_);
        std::vector<std::uint64_t> out;
        for (const auto& kv : windows_) out.push_back(kv.first);
        return out;
    }
    [[nodiscard]] bool window(std::uint64_t hwnd, Window& out) const {
        const std::lock_guard<std::mutex> lock(m_);
        const auto it = windows_.find(hwnd);
        if (it == windows_.end()) return false;
        out = it->second;
        return true;
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* dll, const char* name, Runtime::ImportShim shim) { n += rt_.set_import(dll, name, shim); };
        auto user = [&](const char* name, Runtime::ImportShim shim) { bind("USER32.dll", name, std::move(shim)); };
        using crt_detail::get;
        using crt_detail::put;

        // RegisterClassExW(const WNDCLASSEXW*) -> ATOM
        user("RegisterClassExW", [this](CpuState& s) {
            const std::uint64_t wc = win64_arg(s, 0);
            if (wc == 0 || get<std::uint32_t>(wc) != 80) return fail(s, teb::kErrorInvalidParameter);
            const std::u16string name = class_name(get<std::uint64_t>(wc + 64));
            const std::lock_guard<std::mutex> lock(m_);
            for (const auto& kv : classes_) {
                if (same(kv.second.name, name)) return fail(s, kErrorClassAlreadyExists);
            }
            const std::uint16_t atom = next_atom_++;
            classes_[atom] = Class{name, get<std::uint64_t>(wc + 8), get<std::uint32_t>(wc + 4)};
            win64_return(s, atom);
        });
        // CreateWindowExW(exStyle, class, name, style, x, y, w, h, parent, menu, instance, param)
        user("CreateWindowExW", [this](CpuState& s) { create_window(s); });
        user("DestroyWindow", [this](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            if (!exists(h)) return fail(s, kErrorInvalidWindowHandle);
            send(s, h, kWmDestroy, 0, 0);
            send(s, h, kWmNcDestroy, 0, 0);
            {
                const std::lock_guard<std::mutex> lock(m_);
                windows_.erase(h);
            }
            win64_return(s, 1);
        });
        user("DefWindowProcW", [this](CpuState& s) {
            const auto msg = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (msg == kWmNcCreate) return win64_return(s, 1);
            if (msg == kWmClose) {
                const std::uint64_t h = win64_arg(s, 0);
                if (exists(h)) {
                    send(s, h, kWmDestroy, 0, 0);
                    send(s, h, kWmNcDestroy, 0, 0);
                    const std::lock_guard<std::mutex> lock(m_);
                    windows_.erase(h);
                }
            }
            win64_return(s, 0);
        });
        // ShowWindow(hWnd, nCmdShow) -> was visible
        user("ShowWindow", [this](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            const auto cmd = static_cast<int>(win64_arg(s, 1));
            bool was = false;
            int w = 0, ht = 0;
            {
                const std::lock_guard<std::mutex> lock(m_);
                const auto it = windows_.find(h);
                if (it == windows_.end()) return fail(s, kErrorInvalidWindowHandle);
                was = it->second.visible;
                it->second.visible = cmd != 0;  // SW_HIDE
                w = it->second.width;
                ht = it->second.height;
            }
            if (was != (cmd != 0)) {
                send(s, h, kWmShowWindow, cmd != 0 ? 1 : 0, 0);
                if (cmd != 0) send(s, h, kWmSize, 0, make_lparam(w, ht));
            }
            win64_return(s, was ? 1 : 0);
        });
        user("UpdateWindow", [this](CpuState& s) { win64_return(s, exists(win64_arg(s, 0)) ? 1 : 0); });
        // SetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags)
        user("SetWindowPos", [this](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            const auto flags = static_cast<std::uint32_t>(win64_arg(s, 6));
            bool resized = false;
            int w = 0, ht = 0;
            {
                const std::lock_guard<std::mutex> lock(m_);
                const auto it = windows_.find(h);
                if (it == windows_.end()) return fail(s, kErrorInvalidWindowHandle);
                Window& win = it->second;
                if ((flags & 2) == 0) {  // SWP_NOMOVE
                    win.x = static_cast<int>(win64_arg(s, 2));
                    win.y = static_cast<int>(win64_arg(s, 3));
                }
                if ((flags & 1) == 0) {  // SWP_NOSIZE
                    const int nw = static_cast<int>(win64_arg(s, 4));
                    const int nh = static_cast<int>(win64_arg(s, 5));
                    resized = nw != win.width || nh != win.height;
                    win.width = nw;
                    win.height = nh;
                }
                if ((flags & 0x40) != 0) win.visible = true;   // SWP_SHOWWINDOW
                if ((flags & 0x80) != 0) win.visible = false;  // SWP_HIDEWINDOW
                w = win.width;
                ht = win.height;
            }
            if (resized) send(s, h, kWmSize, 0, make_lparam(w, ht));
            win64_return(s, 1);
        });
        user("GetWindowRect", [this](CpuState& s) {
            Window w;
            if (!window(win64_arg(s, 0), w)) return fail(s, kErrorInvalidWindowHandle);
            put_rect(win64_arg(s, 1), w.x, w.y, w.x + w.width, w.y + w.height);
            win64_return(s, 1);
        });
        user("GetClientRect", [this](CpuState& s) {
            Window w;
            if (!window(win64_arg(s, 0), w)) return fail(s, kErrorInvalidWindowHandle);
            put_rect(win64_arg(s, 1), 0, 0, w.width, w.height);
            win64_return(s, 1);
        });
        user("AdjustWindowRect", [](CpuState& s) { win64_return(s, win64_arg(s, 0) != 0 ? 1 : 0); });  // no frame
        user("ClientToScreen", [this](CpuState& s) { map_point(s, +1); });
        user("ScreenToClient", [this](CpuState& s) { map_point(s, -1); });
        user("GetWindowLongA", [this](CpuState& s) {
            const auto idx = static_cast<int>(static_cast<std::int32_t>(win64_arg(s, 1)));
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = windows_.find(win64_arg(s, 0));
            if (it == windows_.end()) return fail(s, kErrorInvalidWindowHandle);
            win64_return(s, static_cast<std::uint32_t>(get_long(it->second, idx)));
        });
        user("SetWindowLongA", [this](CpuState& s) {
            const auto idx = static_cast<int>(static_cast<std::int32_t>(win64_arg(s, 1)));
            const auto value = static_cast<std::uint32_t>(win64_arg(s, 2));
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = windows_.find(win64_arg(s, 0));
            if (it == windows_.end()) return fail(s, kErrorInvalidWindowHandle);
            const auto prev = static_cast<std::uint32_t>(get_long(it->second, idx));
            Window& w = it->second;
            if (idx == kGwlStyle) w.style = value;
            else if (idx == kGwlExStyle) w.ex_style = value;
            else if (idx == kGwlWndProc) w.proc = value;  // 32 bits through the A function, as on Windows
            else w.longs[idx] = value;
            teb::set_error(s, 0);
            win64_return(s, prev);
        });
        user("SetActiveWindow", [this](CpuState& s) { win64_return(s, swap_active(win64_arg(s, 0))); });
        user("SetForegroundWindow", [this](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            if (!exists(h)) return win64_return(s, 0);
            (void)swap_active(h);
            win64_return(s, 1);
        });
        user("SetRect", [](CpuState& s) {
            if (win64_arg(s, 0) == 0) return win64_return(s, 0);
            put_rect(win64_arg(s, 0), static_cast<int>(win64_arg(s, 1)), static_cast<int>(win64_arg(s, 2)),
                     static_cast<int>(win64_arg(s, 3)), static_cast<int>(win64_arg(s, 4)));
            win64_return(s, 1);
        });

        // Messages
        user("PeekMessageW", [this](CpuState& s) {
            Msg m;
            const bool got = take(win64_arg(s, 1), static_cast<std::uint32_t>(win64_arg(s, 2)),
                                  static_cast<std::uint32_t>(win64_arg(s, 3)),
                                  (static_cast<std::uint32_t>(win64_arg(s, 4)) & kPmRemove) != 0, false, m);
            if (got) put_msg(win64_arg(s, 0), m);
            win64_return(s, got ? 1 : 0);
        });
        user("GetMessageW", [this](CpuState& s) {
            Msg m;
            (void)take(win64_arg(s, 1), static_cast<std::uint32_t>(win64_arg(s, 2)),
                       static_cast<std::uint32_t>(win64_arg(s, 3)), true, true, m);
            put_msg(win64_arg(s, 0), m);
            win64_return(s, m.message == kWmQuit ? 0 : 1);
        });
        user("TranslateMessage", [](CpuState& s) { win64_return(s, 0); });  // no keyboard layout yet
        user("DispatchMessageW", [this](CpuState& s) {
            const std::uint64_t mp = win64_arg(s, 0);
            if (mp == 0) return win64_return(s, 0);
            const std::uint64_t h = get<std::uint64_t>(mp);
            const std::uint64_t r = send(s, h, get<std::uint32_t>(mp + 8), get<std::uint64_t>(mp + 16), get<std::uint64_t>(mp + 24));
            if (s.unwind_rsp == 0) win64_return(s, r);
        });
        user("SendMessageA", [this](CpuState& s) {
            const std::uint64_t r = send(s, win64_arg(s, 0), static_cast<std::uint32_t>(win64_arg(s, 1)), win64_arg(s, 2),
                                         win64_arg(s, 3));
            if (s.unwind_rsp == 0) win64_return(s, r);
        });
        user("PostQuitMessage", [this](CpuState& s) {
            enqueue(std::this_thread::get_id(), Msg{0, kWmQuit, static_cast<std::uint32_t>(win64_arg(s, 0)), 0, now_ms()});
            win64_return(s, 0);
        });
        user("TranslateAcceleratorA", [](CpuState& s) { win64_return(s, 0); });
        user("LoadAcceleratorsA", [this](CpuState& s) { win64_return(s, fake_handle()); });
        user("DestroyAcceleratorTable", [](CpuState& s) { win64_return(s, 1); });

        // Cursor, icons, metrics
        user("GetCursorPos", [this](CpuState& s) {
            if (win64_arg(s, 0) == 0) return fail(s, teb::kErrorInvalidParameter);
            const std::lock_guard<std::mutex> lock(m_);
            put<std::int32_t>(win64_arg(s, 0), cursor_x_);
            put<std::int32_t>(win64_arg(s, 0) + 4, cursor_y_);
            win64_return(s, 1);
        });
        user("SetCursorPos", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            cursor_x_ = clamp(static_cast<int>(win64_arg(s, 0)), clip_[0], clip_[2] - 1);
            cursor_y_ = clamp(static_cast<int>(win64_arg(s, 1)), clip_[1], clip_[3] - 1);
            win64_return(s, 1);
        });
        user("ClipCursor", [this](CpuState& s) {
            const std::uint64_t r = win64_arg(s, 0);
            const std::lock_guard<std::mutex> lock(m_);
            if (r == 0) {
                clip_[0] = 0;
                clip_[1] = 0;
                clip_[2] = display_.screen_width;
                clip_[3] = display_.screen_height;
            } else {
                for (int i = 0; i < 4; ++i) clip_[i] = get<std::int32_t>(r + 4 * static_cast<std::uint64_t>(i));
            }
            win64_return(s, 1);
        });
        user("SetCursor", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            const std::uint64_t prev = cursor_;
            cursor_ = win64_arg(s, 0);
            win64_return(s, prev);
        });
        user("LoadCursorA", [this](CpuState& s) { win64_return(s, fake_handle()); });
        user("LoadIconA", [this](CpuState& s) { win64_return(s, fake_handle()); });
        user("GetSystemMetrics", [this](CpuState& s) { win64_return(s, static_cast<std::uint32_t>(metric(static_cast<int>(win64_arg(s, 0))))); });

        // Clipboard (text formats; the data handle is the bytes, as GlobalLock returns them)
        user("OpenClipboard", [](CpuState& s) { win64_return(s, 1); });
        user("CloseClipboard", [](CpuState& s) { win64_return(s, 1); });
        user("EmptyClipboard", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            clipboard_.clear();
            win64_return(s, 1);
        });
        user("IsClipboardFormatAvailable", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            win64_return(s, clipboard_.count(static_cast<std::uint32_t>(win64_arg(s, 0))) != 0 ? 1 : 0);
        });
        user("GetClipboardData", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = clipboard_.find(static_cast<std::uint32_t>(win64_arg(s, 0)));
            if (it == clipboard_.end()) return win64_return(s, 0);
            win64_return(s, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(it->second.data())));
        });
        user("SetClipboardData", [this](CpuState& s) {
            const auto fmt = static_cast<std::uint32_t>(win64_arg(s, 0));
            const std::uint64_t mem = win64_arg(s, 1);
            std::vector<std::uint8_t> bytes;
            if (mem != 0) {
                const std::size_t unit = fmt == 13 ? 2 : 1;  // CF_UNICODETEXT, else CF_TEXT-like
                for (std::size_t i = 0; i < (1U << 20); i += unit) {
                    bool zero = true;
                    for (std::size_t k = 0; k < unit; ++k) {
                        const std::uint8_t b = get<std::uint8_t>(mem + i + k);
                        bytes.push_back(b);
                        zero = zero && b == 0;
                    }
                    if (zero) break;
                }
            }
            const std::lock_guard<std::mutex> lock(m_);
            clipboard_[fmt] = std::move(bytes);
            win64_return(s, mem);
        });
        user("MessageBoxA", [this](CpuState& s) {
            const std::string text = crt_detail::guest_string(win64_arg(s, 1));
            const std::string caption = crt_detail::guest_string(win64_arg(s, 2));
            if (on_message_box) on_message_box(caption, text);
            win64_return(s, 1);  // IDOK
        });

        // IMM32: no input method editor.
        bind("IMM32.dll", "ImmGetContext", [](CpuState& s) { win64_return(s, 0); });
        bind("IMM32.dll", "ImmReleaseContext", [](CpuState& s) { win64_return(s, 1); });
        bind("IMM32.dll", "ImmAssociateContext", [](CpuState& s) { win64_return(s, 0); });
        bind("IMM32.dll", "ImmAssociateContextEx", [](CpuState& s) { win64_return(s, 1); });
        bind("IMM32.dll", "ImmGetCompositionStringW", [](CpuState& s) { win64_return(s, 0); });
        // GDI32: stock objects are fixed handles.
        bind("GDI32.dll", "GetStockObject", [](CpuState& s) { win64_return(s, 0x00900010ULL + (win64_arg(s, 0) & 0x1F)); });
        return n;
    }

private:
    struct Class {
        std::u16string name;
        std::uint64_t proc = 0;
        std::uint32_t style = 0;
    };
    struct Queue {
        std::deque<Msg> msgs;
        std::condition_variable cv;
    };

    static void fail(CpuState& s, std::uint32_t error) {
        teb::set_error(s, error);
        win64_return(s, 0);
    }
    static std::uint64_t make_lparam(int lo, int hi) {
        return static_cast<std::uint64_t>(static_cast<std::uint16_t>(lo)) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(hi)) << 16);
    }
    static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static void put_rect(std::uint64_t r, int l, int t, int rr, int b) {
        if (r == 0) return;
        crt_detail::put<std::int32_t>(r, l);
        crt_detail::put<std::int32_t>(r + 4, t);
        crt_detail::put<std::int32_t>(r + 8, rr);
        crt_detail::put<std::int32_t>(r + 12, b);
    }
    static bool same(const std::u16string& a, const std::u16string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            auto up = [](char16_t c) { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - 32) : c; };
            if (up(a[i]) != up(b[i])) return false;
        }
        return true;
    }
    // A class argument: an atom (MAKEINTATOM) or a name.
    std::u16string class_name(std::uint64_t p) const {
        if (p < 0x10000) {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = classes_.find(static_cast<std::uint16_t>(p));
            return it == classes_.end() ? std::u16string() : it->second.name;
        }
        return crt_detail::guest_wstring(p);
    }
    std::uint32_t now_ms() const {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - start_).count());
    }
    std::uint64_t fake_handle() {
        const std::lock_guard<std::mutex> lock(m_);
        next_handle_ += 4;
        return next_handle_;
    }
    bool exists(std::uint64_t h) const {
        const std::lock_guard<std::mutex> lock(m_);
        return windows_.count(h) != 0;
    }
    std::uint64_t swap_active(std::uint64_t h) {
        const std::lock_guard<std::mutex> lock(m_);
        const std::uint64_t prev = active_;
        if (windows_.count(h) != 0) active_ = h;
        return prev;
    }
    static std::uint64_t get_long(const Window& w, int idx) {
        if (idx == kGwlStyle) return w.style;
        if (idx == kGwlExStyle) return w.ex_style;
        if (idx == kGwlWndProc) return w.proc;
        const auto it = w.longs.find(idx);
        return it == w.longs.end() ? 0 : it->second;
    }
    int metric(int index) const {
        switch (index) {
        case 0: case 16: case 61: case 78: return display_.screen_width;   // SM_CXSCREEN, CXFULLSCREEN, CXMAXIMIZED, CXVIRTUALSCREEN
        case 1: case 17: case 62: case 79: return display_.screen_height;  // SM_CYSCREEN, ...
        case 13: case 14: return 32;                                       // SM_CXCURSOR, SM_CYCURSOR
        case 80: return 1;                                                 // SM_CMONITORS
        case 43: return 3;                                                 // SM_CMOUSEBUTTONS
        default: return 0;
        }
    }
    void map_point(CpuState& s, int sign) {
        Window w;
        const std::uint64_t p = win64_arg(s, 1);
        if (!window(win64_arg(s, 0), w) || p == 0) return win64_return(s, 0);
        crt_detail::put<std::int32_t>(p, crt_detail::get<std::int32_t>(p) + sign * w.x);
        crt_detail::put<std::int32_t>(p + 4, crt_detail::get<std::int32_t>(p + 4) + sign * w.y);
        win64_return(s, 1);
    }

    void create_window(CpuState& s) {
        const auto ex_style = static_cast<std::uint32_t>(win64_arg(s, 0));
        const std::uint64_t cls_arg = win64_arg(s, 1);
        const std::uint64_t title = win64_arg(s, 2);
        const auto style = static_cast<std::uint32_t>(win64_arg(s, 3));
        int x = static_cast<int>(win64_arg(s, 4));
        int y = static_cast<int>(win64_arg(s, 5));
        int w = static_cast<int>(win64_arg(s, 6));
        int h = static_cast<int>(win64_arg(s, 7));
        const std::uint64_t parent = win64_arg(s, 8);
        const std::uint64_t menu = win64_arg(s, 9);
        const std::uint64_t instance = win64_arg(s, 10);
        const std::uint64_t param = win64_arg(s, 11);
        if (static_cast<std::uint32_t>(x) == kCwUseDefault) x = y = 0;
        if (static_cast<std::uint32_t>(w) == kCwUseDefault) {
            w = display_.screen_width;
            h = display_.screen_height;
        }
        const std::u16string cname = class_name(cls_arg);
        Window win;
        {
            const std::lock_guard<std::mutex> lock(m_);
            const Class* cls = nullptr;
            for (const auto& kv : classes_) {
                if (same(kv.second.name, cname)) cls = &kv.second;
            }
            if (cls == nullptr) return fail(s, kErrorCannotFindWndClass);
            next_hwnd_ += 2;
            win.hwnd = next_hwnd_;
            win.class_name = cls->name;
            win.proc = cls->proc;
            win.style = style;
            win.ex_style = ex_style;
            win.x = x;
            win.y = y;
            win.width = w;
            win.height = h;
            win.visible = (style & 0x10000000U) != 0;  // WS_VISIBLE
            win.owner = std::this_thread::get_id();
            windows_[win.hwnd] = win;
        }
        // CREATESTRUCTW: lpCreateParams hInstance hMenu hwndParent cy cx y x style lpszName lpszClass dwExStyle
        auto cs = std::make_unique<std::uint8_t[]>(80);
        const auto csv = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(cs.get()));
        std::memset(cs.get(), 0, 80);
        crt_detail::put<std::uint64_t>(csv + 0, param);
        crt_detail::put<std::uint64_t>(csv + 8, instance);
        crt_detail::put<std::uint64_t>(csv + 16, menu);
        crt_detail::put<std::uint64_t>(csv + 24, parent);
        crt_detail::put<std::int32_t>(csv + 32, h);
        crt_detail::put<std::int32_t>(csv + 36, w);
        crt_detail::put<std::int32_t>(csv + 40, y);
        crt_detail::put<std::int32_t>(csv + 44, x);
        crt_detail::put<std::uint32_t>(csv + 48, style);
        crt_detail::put<std::uint64_t>(csv + 56, title);
        crt_detail::put<std::uint64_t>(csv + 64, cls_arg);
        crt_detail::put<std::uint32_t>(csv + 72, ex_style);
        const std::uint64_t ok = send(s, win.hwnd, kWmNcCreate, 0, csv);
        if (s.unwind_rsp != 0) return;
        const std::uint64_t created = ok != 0 ? send(s, win.hwnd, kWmCreate, 0, csv) : 0;
        if (s.unwind_rsp != 0) return;
        if (ok == 0 || static_cast<std::int32_t>(created) == -1) {
            const std::lock_guard<std::mutex> lock(m_);
            windows_.erase(win.hwnd);
            return win64_return(s, 0);
        }
        win64_return(s, win.hwnd);
    }

    // Calls the window's procedure on this thread: LRESULT proc(hwnd, msg, wParam, lParam).
    std::uint64_t send(CpuState& s, std::uint64_t hwnd, std::uint32_t msg, std::uint64_t wparam, std::uint64_t lparam) {
        std::uint64_t proc = 0;
        {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = windows_.find(hwnd);
            if (it == windows_.end()) return 0;
            proc = it->second.proc;
        }
        if (proc == 0) return 0;
        return rt_.call_guest(s, proc, {hwnd, msg, wparam, lparam});
    }

    void enqueue(std::thread::id owner, const Msg& m) {
        Queue* q = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_);
            auto& slot = queues_[owner];
            if (!slot) slot = std::make_unique<Queue>();
            slot->msgs.push_back(m);
            q = slot.get();
        }
        q->cv.notify_all();
    }
    // The first queued message of this thread matching the filter (hwnd 0 = any; min = max = 0: any
    // message); `wait` blocks until one arrives.
    bool take(std::uint64_t hwnd, std::uint32_t min, std::uint32_t max, bool remove, bool wait, Msg& out) {
        std::unique_lock<std::mutex> lock(m_);
        auto& slot = queues_[std::this_thread::get_id()];
        if (!slot) slot = std::make_unique<Queue>();
        Queue& q = *slot;
        for (;;) {
            for (auto it = q.msgs.begin(); it != q.msgs.end(); ++it) {
                const bool msg_ok = (min == 0 && max == 0) || (it->message >= min && it->message <= max) || it->message == kWmQuit;
                const bool win_ok = hwnd == 0 || it->hwnd == hwnd || it->message == kWmQuit;
                if (msg_ok && win_ok) {
                    out = *it;
                    if (remove) q.msgs.erase(it);
                    return true;
                }
            }
            if (!wait) return false;
            q.cv.wait(lock);
        }
    }
    void put_msg(std::uint64_t p, const Msg& m) const {
        if (p == 0) return;
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), 0, 48);
        crt_detail::put<std::uint64_t>(p, m.hwnd);
        crt_detail::put<std::uint32_t>(p + 8, m.message);
        crt_detail::put<std::uint64_t>(p + 16, m.wparam);
        crt_detail::put<std::uint64_t>(p + 24, m.lparam);
        crt_detail::put<std::uint32_t>(p + 32, m.time);
        crt_detail::put<std::int32_t>(p + 36, cursor_x_);
        crt_detail::put<std::int32_t>(p + 40, cursor_y_);
    }

    Runtime& rt_;
    DisplayConfig display_;
    std::chrono::steady_clock::time_point start_;
    mutable std::mutex m_;
    std::map<std::uint16_t, Class> classes_;
    std::uint16_t next_atom_ = 0xC001;
    std::map<std::uint64_t, Window> windows_;
    std::uint64_t next_hwnd_ = 0x10010;
    std::uint64_t active_ = 0;
    std::map<std::thread::id, std::unique_ptr<Queue>> queues_;
    std::uint64_t next_handle_ = 0x00A00000;
    std::uint64_t cursor_ = 0;
    int cursor_x_ = 0, cursor_y_ = 0;
    int clip_[4] = {0, 0, 1 << 20, 1 << 20};
    std::map<std::uint32_t, std::vector<std::uint8_t>> clipboard_;
};

}  // namespace wse::recomp::shim
