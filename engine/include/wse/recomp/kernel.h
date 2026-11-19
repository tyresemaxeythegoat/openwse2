// Win64 kernel objects and synchronization for translated code (Phase 5 R3, `docs/RECOMP_PLAN.md`
// "R3 design"; decided with the user: threads come with the handle table and the sync calls the
// exe uses, so translated threads can coordinate).
//
//   handles      CloseHandle DuplicateHandle; pseudo-handles -1 (process) and -2 (thread)
//   waits        WaitForSingleObject(Ex) SignalObjectAndWait on threads, events, mutexes,
//                semaphores; Sleep SwitchToThread
//   objects      CreateEventA/W SetEvent ResetEvent, CreateMutexA/W, CreateSemaphoreA/ExW
//                ReleaseSemaphore (named objects are shared, with ERROR_ALREADY_EXISTS)
//   user mode    InitializeCriticalSection(AndSpinCount/Ex) Enter/TryEnter/Leave/Delete
//                SetCriticalSectionSpinCount; InitializeConditionVariable SleepConditionVariableCS
//                WakeConditionVariable WakeAllConditionVariable; InitializeSListHead
//                InterlockedPush/Pop/FlushSList QueryDepthSList
//
// Kernel-object state is host-side; one mutex and condition variable serve every wait (each
// signal wakes all waiters, which re-check). A critical section keeps a host lock whose pointer is
// stored in its `LockSemaphore` field, and mirrors `OwningThread`/`RecursionCount` into the guest
// structure; a condition variable's word holds a host object created on first use. Thread objects
// are created by `threads.h`. Header-only.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::kernel {

inline constexpr std::uint32_t kInfinite = 0xFFFFFFFFU;
inline constexpr std::uint32_t kWaitObject0 = 0;
inline constexpr std::uint32_t kWaitAbandoned = 0x80;
inline constexpr std::uint32_t kWaitTimeout = 0x102;
inline constexpr std::uint32_t kWaitFailed = 0xFFFFFFFFU;
inline constexpr std::uint32_t kStillActive = 259;
inline constexpr std::uint32_t kErrorInvalidHandle = 6;
inline constexpr std::uint32_t kErrorAlreadyExists = 183;
inline constexpr std::uint32_t kErrorNotOwner = 288;
inline constexpr std::uint32_t kErrorTooManyPosts = 298;
inline constexpr std::uint32_t kErrorTimeout = 1460;
inline constexpr std::uint64_t kFirstHandle = 0x100;

// RTL_CRITICAL_SECTION (x64, 40 bytes).
namespace cso {
inline constexpr std::size_t debug_info = 0, lock_count = 8, recursion = 12, owner = 16, lock_semaphore = 24,
                             spin_count = 32;
}

enum class Kind { thread, event, mutex, semaphore, process };

struct Object {
    Kind kind = Kind::event;
    // event
    bool manual = false;
    bool signaled = false;
    // semaphore
    std::int32_t count = 0;
    std::int32_t maximum = 0;
    // mutex
    std::uint32_t owner = 0;
    std::uint32_t recursion = 0;
    bool abandoned = false;
    // thread (`threads.h`)
    std::uint32_t tid = 0;
    teb::GuestThread* guest = nullptr;  // null once the thread has exited
    bool exited = false;
    std::uint32_t exit_code = 0;
    std::uint32_t suspend = 0;
    std::int32_t priority = 0;
    std::uint64_t affinity = 0;
    std::vector<std::uint8_t> stack;  // the guest stack of a thread started by CreateThread
};

namespace detail {
template <class T>
T get(std::uint64_t va) noexcept {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(va), sizeof(T));
    return v;
}
template <class T>
void put(std::uint64_t va, T v) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &v, sizeof(T));
}
// A guest string as a UTF-16 key (ANSI bytes widened); empty for a null pointer.
inline std::u16string guest_name(std::uint64_t va, bool wide) {
    std::u16string out;
    for (std::size_t i = 0; va != 0 && i < 32768; ++i) {
        const char16_t c = wide ? get<char16_t>(va + 2 * i) : static_cast<char16_t>(get<std::uint8_t>(va + i));
        if (c == 0) {
            break;
        }
        out.push_back(c);
    }
    return out;
}
}  // namespace detail

struct HostCs {
    std::mutex m;
    std::atomic<std::uint32_t> owner{0};
    std::uint32_t recursion = 0;
};
struct HostCv {
    std::condition_variable cv;
};

class Kernel {
public:
    explicit Kernel(Runtime& rt) : rt_(rt) {}
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        bind("CloseHandle", [this](CpuState& s) { close_handle(s); });
        bind("DuplicateHandle", [this](CpuState& s) { duplicate_handle(s); });
        bind("WaitForSingleObject", [this](CpuState& s) { wait_shim(s); });
        bind("WaitForSingleObjectEx", [this](CpuState& s) { wait_shim(s); });
        bind("SignalObjectAndWait", [this](CpuState& s) { signal_and_wait(s); });
        bind("Sleep", [this](CpuState& s) { sleep(s); });
        bind("SwitchToThread", [this](CpuState& s) {
            std::this_thread::yield();
            if (!exiting(s)) win64_return(s, 1);
        });
        bind("CreateEventA", [this](CpuState& s) { create_event(s, false); });
        bind("CreateEventW", [this](CpuState& s) { create_event(s, true); });
        bind("SetEvent", [this](CpuState& s) { set_event(s, true); });
        bind("ResetEvent", [this](CpuState& s) { set_event(s, false); });
        bind("CreateMutexA", [this](CpuState& s) { create_mutex(s, false); });
        bind("CreateMutexW", [this](CpuState& s) { create_mutex(s, true); });
        bind("ReleaseMutex", [this](CpuState& s) { release_mutex(s); });
        bind("CreateSemaphoreA", [this](CpuState& s) { create_semaphore(s, false); });
        bind("CreateSemaphoreW", [this](CpuState& s) { create_semaphore(s, true); });
        bind("CreateSemaphoreExW", [this](CpuState& s) { create_semaphore(s, true); });
        bind("ReleaseSemaphore", [this](CpuState& s) { release_semaphore(s); });
        bind("InitializeCriticalSection", [this](CpuState& s) { cs_init(s, 0); });
        bind("InitializeCriticalSectionAndSpinCount", [this](CpuState& s) { cs_init(s, win64_arg(s, 1)); });
        bind("InitializeCriticalSectionEx", [this](CpuState& s) { cs_init(s, win64_arg(s, 1)); });
        bind("SetCriticalSectionSpinCount", [](CpuState& s) {
            const std::uint64_t cs = win64_arg(s, 0);
            const std::uint64_t old = detail::get<std::uint64_t>(cs + cso::spin_count);
            detail::put<std::uint64_t>(cs + cso::spin_count, win64_arg(s, 1));
            win64_return(s, old);
        });
        bind("EnterCriticalSection", [this](CpuState& s) { cs_enter(s); });
        bind("TryEnterCriticalSection", [this](CpuState& s) { cs_try_enter(s); });
        bind("LeaveCriticalSection", [this](CpuState& s) { cs_leave(s); });
        bind("DeleteCriticalSection", [this](CpuState& s) { cs_delete(s); });
        bind("InitializeConditionVariable", [](CpuState& s) {
            detail::put<std::uint64_t>(win64_arg(s, 0), 0);
            win64_return(s, 0);
        });
        bind("SleepConditionVariableCS", [this](CpuState& s) { cv_sleep(s); });
        bind("WakeConditionVariable", [this](CpuState& s) { cv_wake(s, false); });
        bind("WakeAllConditionVariable", [this](CpuState& s) { cv_wake(s, true); });
        bind("InitializeSListHead", [](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            detail::put<std::uint64_t>(h, 0);
            detail::put<std::uint64_t>(h + 8, 1);  // HeaderType 1 (the 16-byte x64 header)
            win64_return(s, 0);
        });
        bind("InterlockedPushEntrySList", [this](CpuState& s) { slist_push(s); });
        bind("InterlockedPopEntrySList", [this](CpuState& s) { slist_pop(s); });
        bind("InterlockedFlushSList", [this](CpuState& s) { slist_flush(s); });
        bind("QueryDepthSList", [](CpuState& s) {
            win64_return(s, detail::get<std::uint64_t>(win64_arg(s, 0)) & 0xFFFF);
        });
        return n;
    }

    // --- Used by `threads.h` -------------------------------------------------------------------
    std::mutex& mutex() noexcept { return m_; }
    std::condition_variable& changed() noexcept { return cv_; }
    std::uint64_t add_handle(std::shared_ptr<Object> o) {
        std::lock_guard<std::mutex> lock(m_);
        return add_locked(std::move(o));
    }
    // The object behind `h` (pseudo-handle -2 is `s`'s own thread); null when invalid.
    std::shared_ptr<Object> resolve(const CpuState& s, std::uint64_t h) {
        std::lock_guard<std::mutex> lock(m_);
        return resolve_locked(s, h);
    }
    // The thread object for a guest thread (created for threads not started by CreateThread,
    // such as the main thread).
    std::shared_ptr<Object> thread_object(teb::GuestThread& t) {
        std::lock_guard<std::mutex> lock(m_);
        return thread_locked(t);
    }
    // SetEvent from host code (`threadpool.h` UnregisterWaitEx); false when `h` is not an event.
    bool signal_event(const CpuState& s, std::uint64_t h) {
        std::lock_guard<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, h);
        if (!o || o->kind != Kind::event) {
            return false;
        }
        o->signaled = true;
        cv_.notify_all();
        return true;
    }
    // A thread started by CreateThread (`threads.h`): found by id from now on.
    void register_thread(const std::shared_ptr<Object>& o) {
        std::lock_guard<std::mutex> lock(m_);
        if (o->affinity == 0) {
            o->affinity = default_affinity();
        }
        threads_[o->tid] = o;
    }
    std::shared_ptr<Object> find_thread(std::uint32_t tid) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = threads_.find(tid);
        return it == threads_.end() ? nullptr : it->second.lock();
    }
    // A thread ended: its handle signals, the mutexes it owned are abandoned.
    void thread_exited(Object& t, std::uint32_t code) {
        std::lock_guard<std::mutex> lock(m_);
        t.exited = true;
        t.exit_code = code;
        t.guest = nullptr;
        for (auto& [h, o] : handles_) {
            (void)h;
            if (o->kind == Kind::mutex && o->owner == t.tid && o->owner != 0) {
                o->owner = 0;
                o->recursion = 0;
                o->abandoned = true;
            }
        }
        threads_.erase(t.tid);
        cv_.notify_all();
    }
    // True when `s`'s thread was marked by TerminateThread: its exit has begun (the caller must
    // not return from its shim).
    bool exiting(CpuState& s) {
        teb::GuestThread* t = teb::current(s);
        if (t != nullptr && t->terminate_requested) {
            teb::begin_thread_exit(s, t->exit_code);
            return true;
        }
        return false;
    }
    [[nodiscard]] std::size_t handle_count() {
        std::lock_guard<std::mutex> lock(m_);
        return handles_.size();
    }

    ~Kernel() {
        for (HostCs* c : cs_) {
            delete c;
        }
    }

private:
    std::uint64_t add_locked(std::shared_ptr<Object> o) {
        const std::uint64_t h = next_handle_;
        next_handle_ += 4;
        handles_[h] = std::move(o);
        return h;
    }
    std::shared_ptr<Object> thread_locked(teb::GuestThread& t) {
        const auto it = threads_.find(t.id);
        if (it != threads_.end()) {
            if (auto o = it->second.lock()) {
                return o;
            }
        }
        auto o = std::make_shared<Object>();
        o->kind = Kind::thread;
        o->tid = t.id;
        o->guest = &t;
        o->affinity = default_affinity();
        threads_[t.id] = o;
        main_threads_.push_back(o);  // keeps it alive while it has no handle
        return o;
    }
    std::shared_ptr<Object> resolve_locked(const CpuState& s, std::uint64_t h) {
        if (h == teb::kCurrentThread) {
            teb::GuestThread* t = teb::current(s);
            return t == nullptr ? nullptr : thread_locked(*t);
        }
        if (h == teb::kCurrentProcess) {
            if (!process_) {
                process_ = std::make_shared<Object>();
                process_->kind = Kind::process;
            }
            return process_;
        }
        const auto it = handles_.find(h);
        return it == handles_.end() ? nullptr : it->second;
    }
    static std::uint32_t tid_of(const CpuState& s) {
        const teb::GuestThread* t = teb::current(s);
        return t == nullptr ? 0 : t->id;
    }
    static std::uint64_t default_affinity() {
        const unsigned n = std::thread::hardware_concurrency();
        return n == 0 || n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
    }

    // Takes the object for `tid` when it is signaled; `result` is the wait result.
    static bool try_acquire(Object& o, std::uint32_t tid, std::uint32_t& result) {
        result = kWaitObject0;
        switch (o.kind) {
        case Kind::event:
            if (!o.signaled) return false;
            if (!o.manual) o.signaled = false;
            return true;
        case Kind::semaphore:
            if (o.count <= 0) return false;
            --o.count;
            return true;
        case Kind::mutex:
            if (o.owner != 0 && o.owner != tid) return false;
            o.owner = tid;
            ++o.recursion;
            if (o.abandoned) {
                o.abandoned = false;
                result = kWaitAbandoned;
            }
            return true;
        case Kind::thread:
            return o.exited;
        case Kind::process:
            return false;
        }
        return false;
    }

    // The wait core: returns the WAIT_* result, or nothing when the thread's exit began.
    bool wait(CpuState& s, std::uint64_t h, std::uint32_t ms, std::uint32_t& result) {
        return wait_object(s, h, ms, nullptr, result) != WaitEnd::exiting;
    }

public:
    enum class WaitEnd { done, stopped, exiting };
    // The wait core: `result` is the WAIT_* result when `done`. `stop` (read under `mutex()`;
    // whoever makes it true notifies `changed()`) ends the wait early (`stopped`, nothing
    // acquired); `exiting` means `s`'s thread was marked by TerminateThread and its exit began.
    WaitEnd wait_object(CpuState& s, std::uint64_t h, std::uint32_t ms, const std::function<bool()>* stop,
                        std::uint32_t& result) {
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, h);
        if (!o) {
            lock.unlock();
            teb::set_error(s, kErrorInvalidHandle);
            result = kWaitFailed;
            return WaitEnd::done;
        }
        const std::uint32_t tid = tid_of(s);
        teb::GuestThread* me = teb::current(s);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (true) {
            if (stop != nullptr && (*stop)()) {
                return WaitEnd::stopped;
            }
            if (try_acquire(*o, tid, result)) {
                return WaitEnd::done;
            }
            if (me != nullptr && me->terminate_requested) {
                lock.unlock();
                teb::begin_thread_exit(s, me->exit_code);
                return WaitEnd::exiting;
            }
            if (ms == 0) {
                result = kWaitTimeout;
                return WaitEnd::done;
            }
            if (ms == kInfinite) {
                cv_.wait(lock);
            } else if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (stop != nullptr && (*stop)()) {
                    return WaitEnd::stopped;
                }
                if (try_acquire(*o, tid, result)) {
                    return WaitEnd::done;
                }
                result = kWaitTimeout;
                return WaitEnd::done;
            }
        }
    }

private:

    void wait_shim(CpuState& s) {
        std::uint32_t r = 0;
        if (wait(s, win64_arg(s, 0), static_cast<std::uint32_t>(win64_arg(s, 1)), r)) {
            win64_return(s, r);
        }
    }

    // Signals `o` once (SetEvent / ReleaseSemaphore(1) / ReleaseMutex); false when not allowed.
    static bool signal_locked(Object& o, std::uint32_t tid) {
        switch (o.kind) {
        case Kind::event:
            o.signaled = true;
            return true;
        case Kind::semaphore:
            if (o.count >= o.maximum) return false;
            ++o.count;
            return true;
        case Kind::mutex:
            if (o.owner != tid || o.recursion == 0) return false;
            if (--o.recursion == 0) o.owner = 0;
            return true;
        default:
            return false;
        }
    }

    void signal_and_wait(CpuState& s) {
        {
            std::lock_guard<std::mutex> lock(m_);
            std::shared_ptr<Object> o = resolve_locked(s, win64_arg(s, 0));
            if (!o || !signal_locked(*o, tid_of(s))) {
                teb::set_error(s, o ? kErrorNotOwner : kErrorInvalidHandle);
                win64_return(s, kWaitFailed);
                return;
            }
            cv_.notify_all();
        }
        std::uint32_t r = 0;
        if (wait(s, win64_arg(s, 1), static_cast<std::uint32_t>(win64_arg(s, 2)), r)) {
            win64_return(s, r);
        }
    }

    void sleep(CpuState& s) {
        const auto ms = static_cast<std::uint32_t>(win64_arg(s, 0));
        teb::GuestThread* me = teb::current(s);
        if (ms == 0) {
            std::this_thread::yield();
        } else {
            std::unique_lock<std::mutex> lock(m_);
            auto stop = [&] { return me != nullptr && me->terminate_requested.load(); };
            if (ms == kInfinite) {
                cv_.wait(lock, stop);
            } else {
                (void)cv_.wait_for(lock, std::chrono::milliseconds(ms), stop);
            }
        }
        if (!exiting(s)) {
            win64_return(s, 0);
        }
    }

    void close_handle(CpuState& s) {
        const std::uint64_t h = win64_arg(s, 0);
        if (h == teb::kCurrentThread || h == teb::kCurrentProcess) {
            win64_return(s, 1);
            return;
        }
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o;  // released after the lock: a thread object may own a stack
        const auto it = handles_.find(h);
        if (it == handles_.end()) {
            lock.unlock();
            teb::set_error(s, kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        o = std::move(it->second);
        handles_.erase(it);
        lock.unlock();
        win64_return(s, 1);
    }

    // DuplicateHandle(srcProcess, src, dstProcess, *dst, access, inherit, options).
    void duplicate_handle(CpuState& s) {
        const std::uint64_t src = win64_arg(s, 1);
        const std::uint64_t dst = win64_arg(s, 3);
        const std::uint64_t options = win64_arg(s, 6);
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, src);
        if (!o) {
            lock.unlock();
            teb::set_error(s, kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        const std::uint64_t h = add_locked(o);
        if ((options & 1) != 0) {  // DUPLICATE_CLOSE_SOURCE
            handles_.erase(src);
        }
        lock.unlock();
        if (dst != 0) {
            detail::put<std::uint64_t>(dst, h);
        }
        win64_return(s, 1);
    }

    // A named object: the existing one of the same kind (ERROR_ALREADY_EXISTS) or `fresh`.
    std::uint64_t create_named(CpuState& s, const std::u16string& name, std::shared_ptr<Object> fresh) {
        std::unique_lock<std::mutex> lock(m_);
        if (!name.empty()) {
            const auto it = names_.find(name);
            if (it != names_.end()) {
                if (auto existing = it->second.lock()) {
                    if (existing->kind != fresh->kind) {
                        lock.unlock();
                        teb::set_error(s, kErrorInvalidHandle);
                        return 0;
                    }
                    const std::uint64_t h = add_locked(existing);
                    lock.unlock();
                    teb::set_error(s, kErrorAlreadyExists);
                    return h;
                }
            }
            names_[name] = fresh;
        }
        const std::uint64_t h = add_locked(std::move(fresh));
        lock.unlock();
        teb::set_error(s, 0);
        return h;
    }

    // CreateEvent(attributes, manualReset, initialState, name).
    void create_event(CpuState& s, bool wide) {
        auto o = std::make_shared<Object>();
        o->kind = Kind::event;
        o->manual = (win64_arg(s, 1) & 0xFFFFFFFF) != 0;
        o->signaled = (win64_arg(s, 2) & 0xFFFFFFFF) != 0;
        win64_return(s, create_named(s, detail::guest_name(win64_arg(s, 3), wide), std::move(o)));
    }

    void set_event(CpuState& s, bool on) {
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, win64_arg(s, 0));
        if (!o || o->kind != Kind::event) {
            lock.unlock();
            teb::set_error(s, kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        o->signaled = on;
        if (on) {
            cv_.notify_all();
        }
        lock.unlock();
        win64_return(s, 1);
    }

    // CreateMutex(attributes, initialOwner, name).
    void create_mutex(CpuState& s, bool wide) {
        auto o = std::make_shared<Object>();
        o->kind = Kind::mutex;
        const bool own = (win64_arg(s, 1) & 0xFFFFFFFF) != 0;
        const std::u16string name = detail::guest_name(win64_arg(s, 2), wide);
        if (own) {
            o->owner = tid_of(s);
            o->recursion = 1;
        }
        // An existing named mutex is not taken by initialOwner: the caller only gets a handle.
        win64_return(s, create_named(s, name, std::move(o)));
    }

    void release_mutex(CpuState& s) {
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, win64_arg(s, 0));
        if (!o || o->kind != Kind::mutex || !signal_locked(*o, tid_of(s))) {
            lock.unlock();
            teb::set_error(s, o ? kErrorNotOwner : kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        cv_.notify_all();
        lock.unlock();
        win64_return(s, 1);
    }

    // CreateSemaphore(attributes, initial, maximum, name[, flags, access]).
    void create_semaphore(CpuState& s, bool wide) {
        const auto initial = static_cast<std::int32_t>(win64_arg(s, 1));
        const auto maximum = static_cast<std::int32_t>(win64_arg(s, 2));
        if (maximum <= 0 || initial < 0 || initial > maximum) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        auto o = std::make_shared<Object>();
        o->kind = Kind::semaphore;
        o->count = initial;
        o->maximum = maximum;
        win64_return(s, create_named(s, detail::guest_name(win64_arg(s, 3), wide), std::move(o)));
    }

    // ReleaseSemaphore(handle, count, *previous).
    void release_semaphore(CpuState& s) {
        const auto n = static_cast<std::int32_t>(win64_arg(s, 1));
        const std::uint64_t prev = win64_arg(s, 2);
        std::unique_lock<std::mutex> lock(m_);
        std::shared_ptr<Object> o = resolve_locked(s, win64_arg(s, 0));
        if (!o || o->kind != Kind::semaphore || n <= 0 || o->count > o->maximum - n) {
            lock.unlock();
            teb::set_error(s, !o || o->kind != Kind::semaphore ? kErrorInvalidHandle : kErrorTooManyPosts);
            win64_return(s, 0);
            return;
        }
        const std::int32_t before = o->count;
        o->count += n;
        cv_.notify_all();
        lock.unlock();
        if (prev != 0) {
            detail::put<std::int32_t>(prev, before);
        }
        win64_return(s, 1);
    }

    // --- Critical sections -----------------------------------------------------------------------
    void cs_init(CpuState& s, std::uint64_t spin) {
        const std::uint64_t cs = win64_arg(s, 0);
        auto* c = new HostCs;
        {
            std::lock_guard<std::mutex> lock(m_);
            cs_.insert(c);
        }
        detail::put<std::uint64_t>(cs + cso::debug_info, ~std::uint64_t{0});  // no debug info
        detail::put<std::int32_t>(cs + cso::lock_count, -1);
        detail::put<std::int32_t>(cs + cso::recursion, 0);
        detail::put<std::uint64_t>(cs + cso::owner, 0);
        detail::put<std::uint64_t>(cs + cso::lock_semaphore, reinterpret_cast<std::uintptr_t>(c));
        detail::put<std::uint64_t>(cs + cso::spin_count, spin & 0x00FFFFFF);
        win64_return(s, 1);
    }

    HostCs* cs_of(CpuState& s, std::uint64_t cs, const char* shim) {
        auto* c = reinterpret_cast<HostCs*>(static_cast<std::uintptr_t>(detail::get<std::uint64_t>(cs + cso::lock_semaphore)));
        if (c == nullptr || detail::get<std::uint64_t>(cs + cso::debug_info) != ~std::uint64_t{0}) {
            const std::string what = std::string(shim) + ": critical section not initialized";
            s.hooks->unsupported(&s, s.rip, what.c_str());
            return nullptr;
        }
        if (tid_of(s) == 0) {
            const std::string what = std::string(shim) + ": the calling CpuState has no TEB";
            s.hooks->unsupported(&s, s.rip, what.c_str());
            return nullptr;
        }
        return c;
    }
    static void cs_mirror(std::uint64_t cs, const HostCs& c) {
        detail::put<std::uint64_t>(cs + cso::owner, c.owner.load());
        detail::put<std::int32_t>(cs + cso::recursion, static_cast<std::int32_t>(c.recursion));
        detail::put<std::int32_t>(cs + cso::lock_count, c.recursion == 0 ? -1 : -2);
    }

    void cs_enter(CpuState& s) {
        const std::uint64_t cs = win64_arg(s, 0);
        HostCs* c = cs_of(s, cs, "EnterCriticalSection");
        if (c == nullptr) {
            return;
        }
        const std::uint32_t tid = tid_of(s);
        if (c->owner.load() == tid) {
            ++c->recursion;
        } else {
            c->m.lock();
            c->owner = tid;
            c->recursion = 1;
        }
        cs_mirror(cs, *c);
        win64_return(s, 0);
    }

    void cs_try_enter(CpuState& s) {
        const std::uint64_t cs = win64_arg(s, 0);
        HostCs* c = cs_of(s, cs, "TryEnterCriticalSection");
        if (c == nullptr) {
            return;
        }
        const std::uint32_t tid = tid_of(s);
        if (c->owner.load() == tid) {
            ++c->recursion;
        } else if (c->m.try_lock()) {
            c->owner = tid;
            c->recursion = 1;
        } else {
            win64_return(s, 0);
            return;
        }
        cs_mirror(cs, *c);
        win64_return(s, 1);
    }

    void cs_leave(CpuState& s) {
        const std::uint64_t cs = win64_arg(s, 0);
        HostCs* c = cs_of(s, cs, "LeaveCriticalSection");
        if (c == nullptr) {
            return;
        }
        if (c->owner.load() != tid_of(s) || c->recursion == 0) {
            s.hooks->unsupported(&s, s.rip, "LeaveCriticalSection: not the owner");
            return;
        }
        if (--c->recursion == 0) {
            c->owner = 0;
            cs_mirror(cs, *c);
            c->m.unlock();
        } else {
            cs_mirror(cs, *c);
        }
        win64_return(s, 0);
    }

    void cs_delete(CpuState& s) {
        const std::uint64_t cs = win64_arg(s, 0);
        auto* c = reinterpret_cast<HostCs*>(static_cast<std::uintptr_t>(detail::get<std::uint64_t>(cs + cso::lock_semaphore)));
        if (c != nullptr) {
            std::lock_guard<std::mutex> lock(m_);
            if (cs_.erase(c) != 0) {
                delete c;
            }
        }
        detail::put<std::uint64_t>(cs + cso::lock_semaphore, 0);
        detail::put<std::uint64_t>(cs + cso::debug_info, 0);
        win64_return(s, 0);
    }

    // --- Condition variables ------------------------------------------------------------------------
    HostCv* cv_of(std::uint64_t word) {
        auto* p = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(word));
        std::uint64_t cur = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (cur != 0) {
            return reinterpret_cast<HostCv*>(static_cast<std::uintptr_t>(cur));
        }
        auto fresh = std::make_unique<HostCv>();
        std::uint64_t expected = 0;
        const auto mine = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fresh.get()));
        if (__atomic_compare_exchange_n(p, &expected, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            std::lock_guard<std::mutex> lock(m_);
            cvs_.push_back(std::move(fresh));
            return cvs_.back().get();
        }
        return reinterpret_cast<HostCv*>(static_cast<std::uintptr_t>(expected));
    }

    // SleepConditionVariableCS(cv, cs, ms): the caller owns `cs` once; it is released while waiting.
    void cv_sleep(CpuState& s) {
        const std::uint64_t cs = win64_arg(s, 1);
        const auto ms = static_cast<std::uint32_t>(win64_arg(s, 2));
        HostCs* c = cs_of(s, cs, "SleepConditionVariableCS");
        if (c == nullptr) {
            return;
        }
        const std::uint32_t tid = tid_of(s);
        if (c->owner.load() != tid) {
            s.hooks->unsupported(&s, s.rip, "SleepConditionVariableCS: the critical section is not owned");
            return;
        }
        HostCv* v = cv_of(win64_arg(s, 0));
        const std::uint32_t saved = c->recursion;
        c->owner = 0;
        c->recursion = 0;
        cs_mirror(cs, *c);
        bool woke = true;
        {
            std::unique_lock<std::mutex> lk(c->m, std::adopt_lock);
            if (ms == kInfinite) {
                v->cv.wait(lk);
            } else {
                woke = v->cv.wait_for(lk, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
            }
            lk.release();  // still locked: the caller owns the section again
        }
        c->owner = tid;
        c->recursion = saved;
        cs_mirror(cs, *c);
        if (!woke) {
            teb::set_error(s, kErrorTimeout);
        }
        win64_return(s, woke ? 1 : 0);
    }

    void cv_wake(CpuState& s, bool all) {
        HostCv* v = cv_of(win64_arg(s, 0));
        if (all) {
            v->cv.notify_all();
        } else {
            v->cv.notify_one();
        }
        win64_return(s, 0);
    }

    // --- SLists (x64 16-byte header: word 0 = depth | sequence << 16, word 1 = first | 1) --------
    void slist_push(CpuState& s) {
        const std::uint64_t h = win64_arg(s, 0);
        const std::uint64_t e = win64_arg(s, 1);
        std::lock_guard<std::mutex> lock(slist_m_);
        const std::uint64_t w0 = detail::get<std::uint64_t>(h);
        const std::uint64_t first = detail::get<std::uint64_t>(h + 8) & ~std::uint64_t{0xF};
        detail::put<std::uint64_t>(e, first);
        detail::put<std::uint64_t>(h + 8, e | 1);
        detail::put<std::uint64_t>(h, (((w0 >> 16) + 1) << 16) | (((w0 & 0xFFFF) + 1) & 0xFFFF));
        win64_return(s, first);
    }
    void slist_pop(CpuState& s) {
        const std::uint64_t h = win64_arg(s, 0);
        std::lock_guard<std::mutex> lock(slist_m_);
        const std::uint64_t w0 = detail::get<std::uint64_t>(h);
        const std::uint64_t first = detail::get<std::uint64_t>(h + 8) & ~std::uint64_t{0xF};
        if (first != 0) {
            detail::put<std::uint64_t>(h + 8, detail::get<std::uint64_t>(first) | 1);
            detail::put<std::uint64_t>(h, (((w0 >> 16) + 1) << 16) | (((w0 & 0xFFFF) - 1) & 0xFFFF));
        }
        win64_return(s, first);
    }
    void slist_flush(CpuState& s) {
        const std::uint64_t h = win64_arg(s, 0);
        std::lock_guard<std::mutex> lock(slist_m_);
        const std::uint64_t w0 = detail::get<std::uint64_t>(h);
        const std::uint64_t first = detail::get<std::uint64_t>(h + 8) & ~std::uint64_t{0xF};
        detail::put<std::uint64_t>(h + 8, 1);
        detail::put<std::uint64_t>(h, w0 & ~std::uint64_t{0xFFFF});
        win64_return(s, first);
    }

    Runtime& rt_;
    std::mutex m_;
    std::condition_variable cv_;
    std::mutex slist_m_;
    std::map<std::uint64_t, std::shared_ptr<Object>> handles_;
    std::map<std::u16string, std::weak_ptr<Object>> names_;
    std::map<std::uint32_t, std::weak_ptr<Object>> threads_;
    std::vector<std::shared_ptr<Object>> main_threads_;
    std::shared_ptr<Object> process_;
    std::uint64_t next_handle_ = kFirstHandle;
    std::set<HostCs*> cs_;
    std::vector<std::unique_ptr<HostCv>> cvs_;
};

}  // namespace wse::recomp::kernel
