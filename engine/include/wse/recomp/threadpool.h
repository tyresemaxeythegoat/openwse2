// Win64 thread-pool timers and waits for translated code (Phase 5 R3, `docs/RECOMP_PLAN.md`
// "R3 design"):
//
//   CreateThreadpoolTimer SetThreadpoolTimer WaitForThreadpoolTimerCallbacks CloseThreadpoolTimer
//   CreateThreadpoolWait SetThreadpoolWait WaitForThreadpoolWaitCallbacks CloseThreadpoolWait
//   RegisterWaitForSingleObject UnregisterWait UnregisterWaitEx
//
// Each timer or wait object has one worker: a guest thread (its own TEB, stack and TLS attach /
// detach callbacks, as Windows pool threads are) on a host thread, which sleeps until the object
// is due, then calls its callback. Waits go through the kernel's wait (`kernel.h`), so a wait on an
// auto-reset event consumes it as Windows does. Object state is guarded by the kernel mutex, and
// every change notifies the kernel's condition variable. v1.1.5.6 calls these from 1-3 sites each.
// Header-only.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/kernel.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::threadpool {

inline constexpr std::uint32_t kExecuteOnlyOnce = 0x8;  // WT_EXECUTEONLYONCE
inline constexpr std::uint32_t kErrorIoPending = 997;
inline constexpr std::uint64_t kInvalidHandle = ~std::uint64_t{0};
inline constexpr std::size_t kWorkerStack = 1U << 20;

using Clock = std::chrono::steady_clock;

// A FILETIME due time as SetThreadpoolTimer / SetThreadpoolWait read it: negative is relative
// (100 ns units), otherwise an absolute UTC time (a time already past is due now).
inline Clock::time_point due_from_filetime(std::int64_t ft) {
    const Clock::time_point now = Clock::now();
    if (ft < 0) {
        return now + std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds((-ft) * 100));
    }
    const auto unix_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count() / 100;
    const std::int64_t now_ft = unix_100ns + 116444736000000000LL;
    return now + std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(std::max<std::int64_t>(0, ft - now_ft) * 100));
}

struct Item {
    enum class Kind { timer, wait, registered } kind = Kind::timer;
    std::uint64_t callback = 0;
    std::uint64_t context = 0;
    std::uint64_t self = 0;  // the PTP_TIMER / PTP_WAIT / wait handle the guest holds
    bool armed = false;
    bool closing = false;
    std::uint64_t generation = 0;  // bumped by every Set*: a wait in progress restarts
    int running = 0;               // callbacks in progress
    std::uint32_t worker_tid = 0;
    // timer
    Clock::time_point due{};
    std::uint32_t period_ms = 0;
    // wait / registered wait
    std::uint64_t handle = 0;
    std::uint32_t timeout_ms = kernel::kInfinite;
    Clock::time_point wait_deadline{};
    bool once = false;
    // the worker
    std::vector<std::uint8_t> stack;
    alignas(16) std::uint8_t instance[64] = {};  // PTP_CALLBACK_INSTANCE (opaque to the guest)
};

class Pool {
public:
    Pool(Runtime& rt, teb::Process& process, kernel::Kernel& kernel) : rt_(rt), process_(process), kernel_(kernel) {}
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Closes every object and joins the workers.
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            for (auto& [h, it] : items_) {
                (void)h;
                it->closing = true;
            }
            items_.clear();
            kernel_.changed().notify_all();
        }
        for (Host& h : hosts_) {
            if (h.thread.joinable()) {
                h.thread.join();
            }
        }
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        bind("CreateThreadpoolTimer", [this](CpuState& s) { create(s, Item::Kind::timer); });
        bind("CreateThreadpoolWait", [this](CpuState& s) { create(s, Item::Kind::wait); });
        bind("SetThreadpoolTimer", [this](CpuState& s) { set_timer(s); });
        bind("SetThreadpoolWait", [this](CpuState& s) { set_wait(s); });
        bind("WaitForThreadpoolTimerCallbacks", [this](CpuState& s) { wait_callbacks(s); });
        bind("WaitForThreadpoolWaitCallbacks", [this](CpuState& s) { wait_callbacks(s); });
        bind("CloseThreadpoolTimer", [this](CpuState& s) {
            close(win64_arg(s, 0));
            win64_return(s, 0);
        });
        bind("CloseThreadpoolWait", [this](CpuState& s) {
            close(win64_arg(s, 0));
            win64_return(s, 0);
        });
        bind("RegisterWaitForSingleObject", [this](CpuState& s) { register_wait(s); });
        bind("UnregisterWait", [this](CpuState& s) { unregister(s, 0, false); });
        bind("UnregisterWaitEx", [this](CpuState& s) { unregister(s, win64_arg(s, 1), true); });
        return n;
    }

private:
    std::shared_ptr<Item> find_locked(std::uint64_t h) {
        const auto it = items_.find(h);
        return it == items_.end() ? nullptr : it->second;
    }

    // Creates the object and its worker; returns the handle the guest holds.
    std::uint64_t start(std::shared_ptr<Item> item) {
        item->stack.assign(kWorkerStack, 0);
        item->self = reinterpret_cast<std::uintptr_t>(item.get());
        const auto bottom = reinterpret_cast<std::uintptr_t>(item->stack.data());
        const std::uint64_t top = (bottom + item->stack.size() - 64) & ~std::uint64_t{15};
        teb::GuestThread& guest = process_.create_thread(top, bottom);
        item->worker_tid = guest.id;
        {
            std::lock_guard<std::mutex> lock(kernel_.mutex());
            items_[item->self] = item;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(hosts_m_);
        for (auto it = hosts_.begin(); it != hosts_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = hosts_.erase(it);
            } else {
                ++it;
            }
        }
        teb::GuestThread* g = &guest;
        hosts_.push_back(Host{std::thread([this, item, g, top, done] {
                                  worker(item, *g, top);
                                  *done = true;
                              }),
                              done});
        return item->self;
    }

    void worker(const std::shared_ptr<Item>& item, teb::GuestThread& guest, std::uint64_t top) {
        CpuState s;
        s.gpr[4] = top;
        rt_.attach(s);
        teb::Process::attach(s, guest);
        (void)process_.run_tls_callbacks(s, teb::kDllThreadAttach);
        std::unique_lock<std::mutex> lock(kernel_.mutex());
        while (!item->closing) {
            if (!item->armed) {
                kernel_.changed().wait(lock);
                continue;
            }
            std::uint32_t wait_result = kernel::kWaitObject0;
            if (item->kind == Item::Kind::timer) {
                if (Clock::now() < item->due) {
                    kernel_.changed().wait_until(lock, item->due);
                    continue;
                }
                if (item->period_ms != 0) {
                    item->due += std::chrono::milliseconds(item->period_ms);
                } else {
                    item->armed = false;
                }
            } else {
                const std::uint64_t gen = item->generation;
                const std::uint64_t h = item->handle;
                std::uint32_t ms = item->timeout_ms;
                if (ms != kernel::kInfinite && item->kind == Item::Kind::wait) {
                    const auto left = item->wait_deadline - Clock::now();
                    ms = static_cast<std::uint32_t>(std::max<std::int64_t>(
                        0, std::chrono::duration_cast<std::chrono::milliseconds>(left).count()));
                }
                lock.unlock();
                const std::function<bool()> stop = [&] { return item->closing || item->generation != gen; };
                const kernel::Kernel::WaitEnd end = kernel_.wait_object(s, h, ms, &stop, wait_result);
                lock.lock();
                if (end != kernel::Kernel::WaitEnd::done) {
                    continue;  // re-armed, cancelled or closed while waiting
                }
                if (item->kind == Item::Kind::wait || item->once) {
                    item->armed = false;  // a thread-pool wait fires once per SetThreadpoolWait
                }
            }
            ++item->running;
            lock.unlock();
            bool ok = true;
            const auto instance = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(item->instance));
            if (item->kind == Item::Kind::registered) {
                // WAITORTIMERCALLBACK(context, TimerOrWaitFired): TRUE when the wait timed out.
                (void)rt_.call_guest(s, item->callback, {item->context, wait_result == kernel::kWaitTimeout ? 1U : 0U}, &ok);
            } else if (item->kind == Item::Kind::wait) {
                (void)rt_.call_guest(s, item->callback, {instance, item->context, item->self, wait_result}, &ok);
            } else {
                (void)rt_.call_guest(s, item->callback, {instance, item->context, item->self}, &ok);
            }
            s.unwind_rsp = 0;  // a callback that ended its thread is not followed (pool threads live on)
            s.gpr[4] = top;
            lock.lock();
            --item->running;
            kernel_.changed().notify_all();
        }
        lock.unlock();
        CpuState d;
        d.gpr[4] = top;
        rt_.attach(d);
        teb::Process::attach(d, guest);
        process_.exit_thread(d, guest);
    }

    // CreateThreadpoolTimer / CreateThreadpoolWait(callback, context, environment).
    void create(CpuState& s, Item::Kind kind) {
        auto item = std::make_shared<Item>();
        item->kind = kind;
        item->callback = win64_arg(s, 0);
        item->context = win64_arg(s, 1);
        win64_return(s, start(item));
    }

    // SetThreadpoolTimer(timer, *dueTime, msPeriod, msWindowLength): a null due time cancels.
    void set_timer(CpuState& s) {
        const std::uint64_t due = win64_arg(s, 1);
        std::lock_guard<std::mutex> lock(kernel_.mutex());
        if (std::shared_ptr<Item> t = find_locked(win64_arg(s, 0))) {
            ++t->generation;
            t->armed = due != 0;
            if (due != 0) {
                t->due = due_from_filetime(kernel::detail::get<std::int64_t>(due));
                t->period_ms = static_cast<std::uint32_t>(win64_arg(s, 2));
            }
            kernel_.changed().notify_all();
        }
        win64_return(s, 0);
    }

    // SetThreadpoolWait(wait, handle, *timeout): a null handle cancels; a null timeout waits forever.
    void set_wait(CpuState& s) {
        const std::uint64_t h = win64_arg(s, 1);
        const std::uint64_t timeout = win64_arg(s, 2);
        std::lock_guard<std::mutex> lock(kernel_.mutex());
        if (std::shared_ptr<Item> w = find_locked(win64_arg(s, 0))) {
            ++w->generation;
            w->armed = h != 0;
            w->handle = h;
            w->timeout_ms = kernel::kInfinite;
            if (timeout != 0) {
                w->wait_deadline = due_from_filetime(kernel::detail::get<std::int64_t>(timeout));
                w->timeout_ms = 0;  // finite: the worker computes what is left from the deadline
            }
            kernel_.changed().notify_all();
        }
        win64_return(s, 0);
    }

    // WaitForThreadpool*Callbacks(object, cancelPending).
    void wait_callbacks(CpuState& s) {
        const bool cancel = (win64_arg(s, 1) & 0xFFFFFFFF) != 0;
        std::unique_lock<std::mutex> lock(kernel_.mutex());
        std::shared_ptr<Item> it = find_locked(win64_arg(s, 0));
        if (it) {
            if (cancel) {
                it->armed = false;
                ++it->generation;
                kernel_.changed().notify_all();
            }
            if (it->worker_tid != tid_of(s)) {
                kernel_.changed().wait(lock, [&] { return it->running == 0; });
            }
        }
        lock.unlock();
        win64_return(s, 0);
    }

    void close(std::uint64_t h) {
        std::lock_guard<std::mutex> lock(kernel_.mutex());
        if (std::shared_ptr<Item> it = find_locked(h)) {
            it->closing = true;
            items_.erase(h);
            kernel_.changed().notify_all();
        }
    }

    // RegisterWaitForSingleObject(*newWaitObject, object, callback, context, ms, flags).
    void register_wait(CpuState& s) {
        const std::uint64_t out = win64_arg(s, 0);
        auto item = std::make_shared<Item>();
        item->kind = Item::Kind::registered;
        item->handle = win64_arg(s, 1);
        item->callback = win64_arg(s, 2);
        item->context = win64_arg(s, 3);
        item->timeout_ms = static_cast<std::uint32_t>(win64_arg(s, 4));
        item->once = (win64_arg(s, 5) & kExecuteOnlyOnce) != 0;
        item->armed = true;
        if (out == 0 || !kernel_.resolve(s, item->handle)) {
            teb::set_error(s, out == 0 ? teb::kErrorInvalidParameter : kernel::kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        kernel::detail::put<std::uint64_t>(out, start(item));
        win64_return(s, 1);
    }

    // UnregisterWait(wait) / UnregisterWaitEx(wait, completionEvent): INVALID_HANDLE_VALUE waits
    // for running callbacks, an event is signaled once they are done (here: before returning),
    // null returns at once (ERROR_IO_PENDING while a callback is still running).
    void unregister(CpuState& s, std::uint64_t completion, bool ex) {
        const std::uint64_t h = win64_arg(s, 0);
        std::unique_lock<std::mutex> lock(kernel_.mutex());
        std::shared_ptr<Item> it = find_locked(h);
        if (!it || it->kind != Item::Kind::registered) {
            lock.unlock();
            teb::set_error(s, kernel::kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        it->closing = true;
        items_.erase(h);
        kernel_.changed().notify_all();
        const bool from_callback = it->worker_tid == tid_of(s);
        const bool block = ex && completion != 0 && !from_callback;
        if (block) {
            kernel_.changed().wait(lock, [&] { return it->running == 0; });
        }
        const bool still_running = it->running != 0;
        lock.unlock();
        if (block && completion != kInvalidHandle) {
            (void)kernel_.signal_event(s, completion);
        }
        if (!ex && still_running) {
            teb::set_error(s, kErrorIoPending);
            win64_return(s, 0);
            return;
        }
        win64_return(s, 1);
    }

    static std::uint32_t tid_of(const CpuState& s) {
        const teb::GuestThread* t = teb::current(s);
        return t == nullptr ? 0 : t->id;
    }

    struct Host {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    Runtime& rt_;
    teb::Process& process_;
    kernel::Kernel& kernel_;
    std::map<std::uint64_t, std::shared_ptr<Item>> items_;  // guarded by the kernel mutex
    std::mutex hosts_m_;
    std::vector<Host> hosts_;
};

}  // namespace wse::recomp::threadpool
