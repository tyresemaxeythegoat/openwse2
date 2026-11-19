// Win64 threads for translated code (Phase 5 R3, `docs/RECOMP_PLAN.md` "R3 design"): every guest
// thread runs on its own host thread with its own `CpuState`, guest stack and TEB (`teb.h`); its
// handle is a kernel object (`kernel.h`) that signals when the thread ends.
//
//   CreateThread ExitThread FreeLibraryAndExitThread ResumeThread GetExitCodeThread OpenThread
//   TerminateThread SetThreadPriority GetThreadPriority SetThreadAffinityMask GetThreadTimes
//
// A thread starts with the TLS callbacks (DLL_THREAD_ATTACH), runs its entry point through
// `Runtime::call_guest`, and ends with its FLS callbacks and DLL_THREAD_DETACH. `ExitThread`
// unwinds out of every guest frame (`teb::begin_thread_exit`). `TerminateThread` of another
// thread is deferred (decided with the user): the target exits the next time it calls an import,
// waits or sleeps, never in the middle of host or guest code. Header-only.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/kernel.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::threads {

inline constexpr std::uint32_t kCreateSuspended = 0x4;
inline constexpr std::uint32_t kStackSizeIsReservation = 0x10000;
inline constexpr std::size_t kDefaultStack = 1U << 20;   // 1 MiB, the MSVC default reserve
inline constexpr std::size_t kMaxStack = 256U << 20;
inline constexpr std::uint32_t kPriorityErrorReturn = 0x7FFFFFFF;

class Threads {
public:
    Threads(Runtime& rt, teb::Process& process, kernel::Kernel& kernel)
        : rt_(rt), process_(process), kernel_(kernel) {}
    Threads(const Threads&) = delete;
    Threads& operator=(const Threads&) = delete;

    // Waits for every thread this object started (a thread that never ends blocks here).
    ~Threads() { join_all(); }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        bind("CreateThread", [this](CpuState& s) { create_thread(s); });
        bind("ExitThread", [](CpuState& s) { teb::begin_thread_exit(s, static_cast<std::uint32_t>(win64_arg(s, 0))); });
        bind("FreeLibraryAndExitThread",
             [](CpuState& s) { teb::begin_thread_exit(s, static_cast<std::uint32_t>(win64_arg(s, 1))); });
        bind("ResumeThread", [this](CpuState& s) { resume_thread(s); });
        bind("GetExitCodeThread", [this](CpuState& s) { exit_code(s); });
        bind("OpenThread", [this](CpuState& s) { open_thread(s); });
        bind("TerminateThread", [this](CpuState& s) { terminate_thread(s); });
        bind("SetThreadPriority", [this](CpuState& s) { set_priority(s); });
        bind("GetThreadPriority", [this](CpuState& s) { get_priority(s); });
        bind("SetThreadAffinityMask", [this](CpuState& s) { set_affinity(s); });
        bind("GetThreadTimes", [this](CpuState& s) { thread_times(s); });
        // A thread marked by TerminateThread exits at its next import call.
        rt_.before_import = [this](CpuState& s) { return kernel_.exiting(s); };
        return n;
    }

    void join_all() {
        std::vector<Host> hosts;
        {
            std::lock_guard<std::mutex> lock(hosts_m_);
            hosts.swap(hosts_);
        }
        for (Host& h : hosts) {
            if (h.thread.joinable()) {
                h.thread.join();
            }
        }
    }

private:
    std::shared_ptr<kernel::Object> thread_handle(CpuState& s, std::uint64_t h) {
        std::shared_ptr<kernel::Object> o = kernel_.resolve(s, h);
        if (!o || o->kind != kernel::Kind::thread) {
            teb::set_error(s, kernel::kErrorInvalidHandle);
            return nullptr;
        }
        return o;
    }

    // CreateThread(attributes, stackSize, start, parameter, flags, *threadId).
    void create_thread(CpuState& s) {
        const std::uint64_t stack_arg = win64_arg(s, 1);
        const std::uint64_t start = win64_arg(s, 2);
        const std::uint64_t param = win64_arg(s, 3);
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 4));
        const std::uint64_t tid_out = win64_arg(s, 5);
        std::size_t bytes = std::max<std::size_t>(static_cast<std::size_t>(std::min<std::uint64_t>(stack_arg, kMaxStack)),
                                                  kDefaultStack);
        bytes = (bytes + 0xFFFF) & ~static_cast<std::size_t>(0xFFFF);
        auto obj = std::make_shared<kernel::Object>();
        obj->kind = kernel::Kind::thread;
        obj->suspend = (flags & kCreateSuspended) != 0 ? 1 : 0;
        obj->stack.assign(bytes, 0);
        const auto bottom = reinterpret_cast<std::uintptr_t>(obj->stack.data());
        const std::uint64_t top = (bottom + bytes - 64) & ~std::uint64_t{15};
        teb::GuestThread& guest = process_.create_thread(top, bottom);
        obj->tid = guest.id;
        obj->guest = &guest;
        kernel_.register_thread(obj);  // `OpenThread`, and pseudo-handle -2 inside the thread
        const std::uint64_t handle = kernel_.add_handle(obj);
        if (tid_out != 0) {
            kernel::detail::put<std::uint32_t>(tid_out, guest.id);
        }
        std::lock_guard<std::mutex> lock(hosts_m_);
        // Join the host threads that have finished, so a game that starts many threads does not
        // keep their host stacks.
        for (auto it = hosts_.begin(); it != hosts_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = hosts_.erase(it);
            } else {
                ++it;
            }
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        hosts_.push_back(Host{std::thread([this, obj, start, param, top, done] {
                                  run(obj, start, param, top);
                                  *done = true;
                              }),
                              done});
        win64_return(s, handle);
    }

    void run(std::shared_ptr<kernel::Object> obj, std::uint64_t start, std::uint64_t param, std::uint64_t top) {
        teb::GuestThread* guest = nullptr;
        {
            std::unique_lock<std::mutex> lock(kernel_.mutex());
            kernel_.changed().wait(lock, [&] {
                return obj->suspend == 0 || (obj->guest != nullptr && obj->guest->terminate_requested.load());
            });
            guest = obj->guest;
        }
        CpuState s;
        s.gpr[4] = top;
        rt_.attach(s);
        teb::Process::attach(s, *guest);
        std::uint32_t code = 0;
        if (!guest->terminate_requested) {
            (void)process_.run_tls_callbacks(s, teb::kDllThreadAttach);
            if (s.unwind_rsp == 0) {
                bool ok = false;
                const std::uint64_t rax = rt_.call_guest(s, start, {param}, &ok);
                code = static_cast<std::uint32_t>(rax);
            }
        }
        if (s.unwind_rsp == teb::kThreadExitUnwind || guest->terminate_requested) {
            code = guest->exit_code;
        }
        // Detach on a clean state at the top of the stack.
        s = CpuState{};
        s.gpr[4] = top;
        rt_.attach(s);
        teb::Process::attach(s, *guest);
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            obj->guest = nullptr;  // TerminateThread no longer reaches it
        }
        process_.exit_thread(s, *guest);  // frees the TEB
        kernel_.thread_exited(*obj, code);
    }

    void resume_thread(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        if (!o) {
            win64_return(s, 0xFFFFFFFFU);
            return;
        }
        std::uint32_t prev = 0;
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            prev = o->suspend;
            if (o->suspend > 0) {
                --o->suspend;
            }
            kernel_.changed().notify_all();
        }
        win64_return(s, prev);
    }

    void exit_code(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        const std::uint64_t out = win64_arg(s, 1);
        if (!o) {
            win64_return(s, 0);
            return;
        }
        std::uint32_t code = kernel::kStillActive;
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            if (o->exited) {
                code = o->exit_code;
            }
        }
        if (out != 0) {
            kernel::detail::put<std::uint32_t>(out, code);
        }
        win64_return(s, 1);
    }

    // OpenThread(access, inherit, threadId).
    void open_thread(CpuState& s) {
        std::shared_ptr<kernel::Object> o = kernel_.find_thread(static_cast<std::uint32_t>(win64_arg(s, 2)));
        if (!o) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        win64_return(s, kernel_.add_handle(o));
    }

    void terminate_thread(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        const auto code = static_cast<std::uint32_t>(win64_arg(s, 1));
        if (!o) {
            win64_return(s, 0);
            return;
        }
        teb::GuestThread* me = teb::current(s);
        if (me != nullptr && o->tid == me->id) {
            teb::begin_thread_exit(s, code);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            if (o->guest != nullptr && !o->exited) {
                o->guest->exit_code = code;
                o->guest->terminate_requested = true;
            }
            kernel_.changed().notify_all();
        }
        win64_return(s, 1);
    }

    void set_priority(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        if (o) {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            o->priority = static_cast<std::int32_t>(win64_arg(s, 1));
        }
        win64_return(s, o ? 1 : 0);
    }

    void get_priority(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        if (!o) {
            win64_return(s, kPriorityErrorReturn);
            return;
        }
        std::lock_guard<std::mutex> lock(kernel_.mutex());
        win64_return(s, static_cast<std::uint32_t>(o->priority));
    }

    // Returns the previous mask; the mask is recorded, not applied to the host thread.
    void set_affinity(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        const std::uint64_t mask = win64_arg(s, 1);
        if (!o || mask == 0) {
            if (o) {
                teb::set_error(s, teb::kErrorInvalidParameter);
            }
            win64_return(s, 0);
            return;
        }
        std::lock_guard<std::mutex> lock(kernel_.mutex());
        const std::uint64_t prev = o->affinity;
        o->affinity = mask;
        win64_return(s, prev);
    }

    // GetThreadTimes(handle, *creation, *exit, *kernel, *user): all zero (not measured yet).
    void thread_times(CpuState& s) {
        std::shared_ptr<kernel::Object> o = thread_handle(s, win64_arg(s, 0));
        if (!o) {
            win64_return(s, 0);
            return;
        }
        for (int i = 1; i <= 4; ++i) {
            if (const std::uint64_t p = win64_arg(s, i)) {
                kernel::detail::put<std::uint64_t>(p, 0);
            }
        }
        win64_return(s, 1);
    }

    Runtime& rt_;
    teb::Process& process_;
    kernel::Kernel& kernel_;
    struct Host {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex hosts_m_;
    std::vector<Host> hosts_;
};

}  // namespace wse::recomp::threads
