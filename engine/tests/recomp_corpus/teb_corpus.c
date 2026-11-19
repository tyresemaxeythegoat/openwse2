/* R3 corpus: the Win64 thread environment through the runtime (`docs/RECOMP_PLAN.md` "R3 design",
 * `wse/recomp/teb.h`).
 *
 * Our own code. The `t_*` exports read the TEB through `gs:`, use static TLS (`__declspec(thread)`:
 * clang emits the `_tls_index` / `gs:[0x58]` sequence MSVC does), a TLS callback, and the
 * KERNEL32 TLS, FLS and last-error calls. Like the SEH corpus they are self-checking (0x600D when
 * correct) because KERNEL32 does not exist on the Linux runners. There is no CRT, so the static TLS
 * directory (`_tls_used` and the `.tls` / `.CRT$XL*` bounds, normally from tlssup.obj) is defined
 * here; lld-link points the TLS data directory at `_tls_used`.
 */

typedef unsigned long long u64;
typedef unsigned int u32;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

__declspec(dllimport) u32 TlsAlloc(void);
__declspec(dllimport) int TlsFree(u32 index);
__declspec(dllimport) void* TlsGetValue(u32 index);
__declspec(dllimport) int TlsSetValue(u32 index, void* value);
__declspec(dllimport) u32 FlsAlloc(void (*callback)(void*));
__declspec(dllimport) int FlsFree(u32 index);
__declspec(dllimport) void* FlsGetValue(u32 index);
__declspec(dllimport) int FlsSetValue(u32 index, void* value);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) void SetLastError(u32 code);
__declspec(dllimport) u32 GetCurrentThreadId(void);
extern char __ImageBase;

/* --- The static TLS directory (what tlssup.obj provides) -------------------------------------- */
typedef void (*TlsCallback)(void* base, u32 reason, void* reserved);
typedef struct {
    u64 raw_begin, raw_end, index, callbacks;
    u32 zero_fill, characteristics;
} TlsDirectory;

u32 _tls_index = 0;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__attribute__((section(".CRT$XLA"))) TlsCallback __xl_a = 0;
__attribute__((section(".CRT$XLZ"))) TlsCallback __xl_z = 0;

static volatile u64 g_attach_count;
static volatile u64 g_attach_base;

EXPORT void teb_tls_callback(void* base, u32 reason, void* reserved) {
    (void)reserved;
    if (reason == 1) { /* DLL_PROCESS_ATTACH */
        ++g_attach_count;
        g_attach_base = (u64)base;
    }
}
__attribute__((section(".CRT$XLB"))) TlsCallback teb_tls_callback_ptr = teb_tls_callback;

const TlsDirectory _tls_used = {(u64)&_tls_start, (u64)&_tls_end, (u64)&_tls_index, (u64)(&__xl_a + 1), 0, 0};

/* --- Tests ------------------------------------------------------------------------------------ */
static u64 gs_qword(u32 offset) {
    u64 v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((u64)offset));
    return v;
}

/* The TEB: self pointer, PEB with the image base, ClientId.UniqueThread. */
EXPORT NOINLINE u64 t_teb_fields(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const u64 self = gs_qword(0x30);
    const u64 peb = gs_qword(0x60);
    if (self == 0 || *(const u64*)(self + 0x30) != self) {
        return 1;
    }
    if (peb == 0 || *(const u64*)(peb + 0x10) != (u64)&__ImageBase) {
        return 2;
    }
    if ((u32)gs_qword(0x48) != GetCurrentThreadId()) {
        return 3;
    }
    return 0x600D;
}

__declspec(thread) u64 tls_counter = 5;
__declspec(thread) u64 tls_constant = 0x7E57;

/* Static TLS: the template value arrives in the thread's block, and writes stick. */
EXPORT NOINLINE u64 t_static_tls(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    if (tls_constant != 0x7E57) {
        return 1;
    }
    const u64 v = tls_counter;
    tls_counter = v + (a | 1);
    if (tls_counter != v + (a | 1)) {
        return 2;
    }
    return 0x600D;
}

/* The TLS callback ran once, at process attach, with the image base. */
EXPORT NOINLINE u64 t_tls_callback(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return g_attach_count == 1 && g_attach_base == (u64)&__ImageBase ? 0x600D : 1;
}

/* TlsAlloc / TlsSetValue / TlsGetValue / TlsFree, and the last error they set. */
EXPORT NOINLINE u64 t_tls_slots(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u32 i = TlsAlloc();
    if (i == 0xFFFFFFFFu) {
        return 1;
    }
    SetLastError(5);
    if (!TlsSetValue(i, (void*)a) || TlsGetValue(i) != (void*)a || GetLastError() != 0) {
        return 2;
    }
    if (!TlsFree(i) || TlsFree(i) || GetLastError() != 87) {
        return 3;
    }
    return 0x600D;
}

static volatile u64 g_fls_seen;
EXPORT void teb_fls_callback(void* value) { g_fls_seen = (u64)value; }

/* FlsFree runs the slot's callback with the thread's value. */
EXPORT NOINLINE u64 t_fls(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 value = a | 1;
    g_fls_seen = 0;
    const u32 i = FlsAlloc(teb_fls_callback);
    if (i == 0xFFFFFFFFu || !FlsSetValue(i, (void*)value) || FlsGetValue(i) != (void*)value) {
        return 1;
    }
    if (!FlsFree(i) || g_fls_seen != value) {
        return 2;
    }
    return 0x600D;
}

EXPORT NOINLINE u64 t_last_error(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    SetLastError((u32)a);
    return GetLastError() == (u32)a ? 0x600D : 1;
}

/* --- Threads and sync (`wse/recomp/threads.h`, `kernel.h`) -------------------------------------- */
__declspec(dllimport) u64 CreateThread(void* attributes, u64 stack, u32 (*start)(void*), void* param, u32 flags,
                                       u32* id);
__declspec(dllimport) void ExitThread(u32 code);
__declspec(dllimport) u32 ResumeThread(u64 thread);
__declspec(dllimport) int TerminateThread(u64 thread, u32 code);
__declspec(dllimport) int GetExitCodeThread(u64 thread, u32* code);
__declspec(dllimport) u32 WaitForSingleObject(u64 handle, u32 ms);
__declspec(dllimport) int CloseHandle(u64 handle);
__declspec(dllimport) void Sleep(u32 ms);
__declspec(dllimport) u64 CreateEventW(void* attributes, int manual, int initial, const void* name);
__declspec(dllimport) int SetEvent(u64 event);
typedef struct {
    u64 debug_info;
    int lock_count, recursion;
    u64 owner, lock_semaphore, spin_count;
} CriticalSection;
__declspec(dllimport) void InitializeCriticalSection(CriticalSection* cs);
__declspec(dllimport) void EnterCriticalSection(CriticalSection* cs);
__declspec(dllimport) void LeaveCriticalSection(CriticalSection* cs);
__declspec(dllimport) void DeleteCriticalSection(CriticalSection* cs);

#define INFINITE 0xFFFFFFFFu

/* A new thread gets a fresh copy of the static TLS template and its own id. */
EXPORT u32 teb_thread_fresh_tls(void* param) {
    if (tls_constant != 0x7E57 || tls_counter != 5) {
        return 1;
    }
    tls_counter = (u64)param;
    return GetCurrentThreadId() != 0 && tls_counter == (u64)param ? 0x600D : 2;
}

static CriticalSection g_cs;
static volatile u64 g_shared;
EXPORT u32 teb_thread_counter(void* param) {
    (void)param;
    for (int i = 0; i < 200; ++i) {
        EnterCriticalSection(&g_cs);
        g_shared = g_shared + 1;
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

EXPORT NOINLINE void teb_exit_deep(u32 code) { ExitThread(code); }
EXPORT u32 teb_thread_exit(void* param) {
    teb_exit_deep((u32)(u64)param);
    return 1; /* not reached */
}

static volatile u64 g_event;
EXPORT u32 teb_thread_sleeper(void* param) {
    (void)param;
    SetEvent(g_event);
    for (;;) {
        Sleep(INFINITE);
    }
}

static u32 run_and_wait(u32 (*start)(void*), void* param, u32 flags, u32* code) {
    const u64 h = CreateThread(0, 0, start, param, flags, 0);
    if (h == 0) {
        return 0;
    }
    if (flags & 4) { /* CREATE_SUSPENDED */
        u32 c = 0;
        if (!GetExitCodeThread(h, &c) || c != 259 || ResumeThread(h) != 1) {
            return 0;
        }
    }
    if (WaitForSingleObject(h, INFINITE) != 0 || !GetExitCodeThread(h, code) || !CloseHandle(h)) {
        return 0;
    }
    return 1;
}

/* A thread's static TLS is its own; the creator's value is untouched. */
EXPORT NOINLINE u64 t_thread_tls(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 mine = tls_counter;
    u32 code = 0;
    if (!run_and_wait(teb_thread_fresh_tls, (void*)(a | 0x100), 4, &code) || code != 0x600D) {
        return 1;
    }
    return tls_counter == mine ? 0x600D : 2;
}

/* Four threads and this one increment under one critical section. */
EXPORT NOINLINE u64 t_thread_cs(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    InitializeCriticalSection(&g_cs);
    g_shared = 0;
    u64 h[4];
    for (int i = 0; i < 4; ++i) {
        h[i] = CreateThread(0, 0, teb_thread_counter, 0, 0, 0);
    }
    teb_thread_counter(0);
    for (int i = 0; i < 4; ++i) {
        if (h[i] == 0 || WaitForSingleObject(h[i], INFINITE) != 0) {
            return 1;
        }
        CloseHandle(h[i]);
    }
    DeleteCriticalSection(&g_cs);
    return g_shared == 5 * 200 ? 0x600D : 2;
}

/* ExitThread from a nested call ends the thread with its code; TerminateThread ends a sleeper. */
EXPORT NOINLINE u64 t_thread_exit(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    u32 code = 0;
    const u32 want = (u32)(a & 0xFFFF) + 1;
    if (!run_and_wait(teb_thread_exit, (void*)(u64)want, 0, &code) || code != want) {
        return 1;
    }
    g_event = CreateEventW(0, 1, 0, 0);
    const u64 h = CreateThread(0, 0, teb_thread_sleeper, 0, 0, 0);
    if (g_event == 0 || h == 0 || WaitForSingleObject(g_event, INFINITE) != 0) {
        return 2;
    }
    if (!TerminateThread(h, 0x5EED) || WaitForSingleObject(h, INFINITE) != 0 || !GetExitCodeThread(h, &code) ||
        code != 0x5EED) {
        return 3;
    }
    CloseHandle(h);
    CloseHandle(g_event);
    return 0x600D;
}

/* --- Thread-pool timers and waits (`wse/recomp/threadpool.h`) ------------------------------------ */
__declspec(dllimport) u64 CreateThreadpoolTimer(void (*callback)(void*, void*, u64), void* context, void* env);
__declspec(dllimport) void SetThreadpoolTimer(u64 timer, const long long* due, u32 period, u32 window);
__declspec(dllimport) void WaitForThreadpoolTimerCallbacks(u64 timer, int cancel);
__declspec(dllimport) void CloseThreadpoolTimer(u64 timer);
__declspec(dllimport) u64 CreateThreadpoolWait(void (*callback)(void*, void*, u64, u32), void* context, void* env);
__declspec(dllimport) void SetThreadpoolWait(u64 wait, u64 handle, const long long* timeout);
__declspec(dllimport) void CloseThreadpoolWait(u64 wait);
__declspec(dllimport) int RegisterWaitForSingleObject(u64* wait, u64 handle, void (*callback)(void*, unsigned char),
                                                      void* context, u32 ms, u32 flags);
__declspec(dllimport) int UnregisterWaitEx(u64 wait, u64 completion);
__declspec(dllimport) int ResetEvent(u64 event);

static volatile u64 g_pool_count;
static volatile u64 g_pool_seen;
static volatile u64 g_pool_done; /* an event the callbacks set */

EXPORT void teb_timer_callback(void* instance, void* context, u64 timer) {
    (void)instance;
    (void)timer;
    g_pool_seen = (u64)context;
    if (++g_pool_count == 3) {
        SetEvent(g_pool_done);
    }
}

/* A periodic timer (first due in 1 ms, then every 2 ms) fires three times on a pool thread. */
EXPORT NOINLINE u64 t_pool_timer(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    g_pool_count = 0;
    g_pool_done = CreateEventW(0, 1, 0, 0);
    const u64 t = CreateThreadpoolTimer(teb_timer_callback, (void*)(a | 1), 0);
    const long long due = -10000; /* 1 ms */
    if (t == 0 || g_pool_done == 0) {
        return 1;
    }
    SetThreadpoolTimer(t, &due, 2, 0);
    if (WaitForSingleObject(g_pool_done, INFINITE) != 0) {
        return 2;
    }
    SetThreadpoolTimer(t, 0, 0, 0);
    WaitForThreadpoolTimerCallbacks(t, 1);
    CloseThreadpoolTimer(t);
    CloseHandle(g_pool_done);
    return g_pool_count >= 3 && g_pool_seen == (a | 1) ? 0x600D : 3;
}

static volatile u64 g_wait_result;
EXPORT void teb_wait_callback(void* instance, void* context, u64 wait, u32 result) {
    (void)instance;
    (void)wait;
    g_pool_seen = (u64)context;
    g_wait_result = result;
    SetEvent(g_pool_done);
}

/* A thread-pool wait fires when its event is set (and consumes the auto-reset event), and once
 * more with WAIT_TIMEOUT when re-armed with a 1 ms timeout. */
EXPORT NOINLINE u64 t_pool_wait(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    g_pool_done = CreateEventW(0, 0, 0, 0);
    const u64 ev = CreateEventW(0, 0, 0, 0);
    const u64 w = CreateThreadpoolWait(teb_wait_callback, (void*)(a | 1), 0);
    if (w == 0 || ev == 0 || g_pool_done == 0) {
        return 1;
    }
    SetThreadpoolWait(w, ev, 0);
    SetEvent(ev);
    if (WaitForSingleObject(g_pool_done, INFINITE) != 0 || g_wait_result != 0 || g_pool_seen != (a | 1) ||
        WaitForSingleObject(ev, 0) != 0x102) {
        return 2;
    }
    const long long timeout = -10000;
    SetThreadpoolWait(w, ev, &timeout);
    if (WaitForSingleObject(g_pool_done, INFINITE) != 0 || g_wait_result != 0x102) {
        return 3;
    }
    CloseThreadpoolWait(w);
    CloseHandle(ev);
    CloseHandle(g_pool_done);
    return 0x600D;
}

EXPORT void teb_registered_callback(void* context, unsigned char timed_out) {
    g_pool_seen = (u64)context + timed_out;
    ++g_pool_count;
    SetEvent(g_pool_done);
}

/* RegisterWaitForSingleObject calls back on each signal of its event until UnregisterWaitEx. */
EXPORT NOINLINE u64 t_register_wait(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 ctx = (a & ~(u64)0xFF) | 0x10;
    g_pool_count = 0;
    g_pool_done = CreateEventW(0, 0, 0, 0);
    const u64 ev = CreateEventW(0, 0, 0, 0);
    u64 reg = 0;
    if (!RegisterWaitForSingleObject(&reg, ev, teb_registered_callback, (void*)ctx, INFINITE, 0) || reg == 0) {
        return 1;
    }
    for (int i = 0; i < 2; ++i) {
        SetEvent(ev);
        if (WaitForSingleObject(g_pool_done, INFINITE) != 0) {
            return 2;
        }
    }
    if (!UnregisterWaitEx(reg, ~(u64)0) || g_pool_count != 2 || g_pool_seen != ctx) {
        return 3;
    }
    CloseHandle(ev);
    CloseHandle(g_pool_done);
    return 0x600D;
}

/* `_tls_used` and the callback pointer are reached by no code: keep them. */
__asm__(".section .drectve,\"yn\"\n"
        "  .ascii \" -include:_tls_used -include:teb_tls_callback_ptr\"\n"
        ".text\n");
