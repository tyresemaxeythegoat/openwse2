/* R4 corpus: time, CRT support, system information and timer queues through the shims
 * (`docs/RECOMP_PLAN.md` "R4 design", `wse/recomp/shim_time.h`, `shim_crt.h`, `threadpool.h`).
 *
 * Our own code. Like the other KERNEL32 corpora the `t_*` exports are self-checking (0x600D when
 * correct). They check only what Windows answers the same way on every machine (no CPU feature
 * bits, no processor counts), so the same PE can be run natively on Windows as a reference.
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

__declspec(dllimport) int QueryPerformanceCounter(long long* count);
__declspec(dllimport) int QueryPerformanceFrequency(long long* frequency);
__declspec(dllimport) u32 GetTickCount(void);
__declspec(dllimport) void Sleep(u32 ms);
__declspec(dllimport) int SystemTimeToFileTime(const u16* st, u64* ft);
__declspec(dllimport) int FileTimeToSystemTime(const u64* ft, u16* st);
__declspec(dllimport) void GetSystemTimeAsFileTime(u64* ft);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) void* EncodePointer(void* p);
__declspec(dllimport) void* DecodePointer(void* p);
__declspec(dllimport) int InitOnceExecuteOnce(u64* once, int (*fn)(u64*, void*, void**), void* param, void** ctx);
__declspec(dllimport) void GetSystemInfo(u8* info);
__declspec(dllimport) int GetLogicalProcessorInformation(void* buffer, u32* length);
__declspec(dllimport) int GetVersionExW(u8* info);
__declspec(dllimport) int SetEnvironmentVariableA(const char* name, const char* value);
__declspec(dllimport) u16* GetEnvironmentStringsW(void);
__declspec(dllimport) int FreeEnvironmentStringsW(u16* block);
__declspec(dllimport) u64 CreateTimerQueue(void);
__declspec(dllimport) int CreateTimerQueueTimer(u64* timer, u64 queue, void (*callback)(void*, u8), void* param, u32 due,
                                                u32 period, u32 flags);
__declspec(dllimport) int DeleteTimerQueueTimer(u64 queue, u64 timer, u64 completion);
__declspec(dllimport) u64 CreateEventW(void* attributes, int manual, int initial, const void* name);
__declspec(dllimport) int SetEvent(u64 event);
__declspec(dllimport) u32 WaitForSingleObject(u64 handle, u32 ms);
__declspec(dllimport) int CloseHandle(u64 handle);

#define INFINITE 0xFFFFFFFFu

/* The performance counter only goes forward, and at its own frequency (which depends on the
 * machine: 10 MHz in the shims, 24 MHz on the PC this corpus was checked on) a Sleep(2) lasts
 * between 1 ms and a minute; GetTickCount agrees. */
EXPORT NOINLINE u64 t_time_counters(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    long long f = 0, c0 = 0, c1 = 0;
    if (!QueryPerformanceFrequency(&f) || f < 1000000) {
        return 1;
    }
    QueryPerformanceCounter(&c0);
    const u32 t0 = GetTickCount();
    Sleep(2);
    QueryPerformanceCounter(&c1);
    const u32 t1 = GetTickCount();
    const long long us = (c1 - c0) * 1000000 / f;
    if (us < 1000 || us > 60000000 || t1 - t0 > 60000) {
        return 2;
    }
    return 0x600D;
}

/* SYSTEMTIME <-> FILETIME: 2000-01-01 is 125911584000000000; 59 days and 13:45:30.123 later is
 * Tuesday 2000-02-29; Feb 29 2023 does not exist. */
EXPORT NOINLINE u64 t_time_convert(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    u16 st[8] = {2000, 1, 0, 1, 0, 0, 0, 0};
    u64 ft = 0;
    if (!SystemTimeToFileTime(st, &ft) || ft != 125911584000000000ULL) {
        return 1;
    }
    ft += 864000000000ULL * 59 + 10000ULL * ((((13 * 60 + 45) * 60 + 30) * 1000ULL) + (a % 1000));
    u16 out[8];
    if (!FileTimeToSystemTime(&ft, out) || out[0] != 2000 || out[1] != 2 || out[2] != 2 || out[3] != 29 ||
        out[4] != 13 || out[5] != 45 || out[6] != 30 || out[7] != (u16)(a % 1000)) {
        return 2;
    }
    u16 bad[8] = {2023, 2, 0, 29, 0, 0, 0, 0};
    if (SystemTimeToFileTime(bad, &ft) || GetLastError() != 87) {
        return 3;
    }
    u64 now = 0;
    GetSystemTimeAsFileTime(&now);
    return now > 133000000000000000ULL ? 0x600D : 4; /* after 2022 */
}

/* EncodePointer / DecodePointer round-trip, and the encoding changes the pointer. */
EXPORT NOINLINE u64 t_crt_pointers(u64 a, u64 b, u64 c, u64 d) {
    const u64 v[5] = {0, a, b, c, d};
    for (int i = 0; i < 5; ++i) {
        void* e = EncodePointer((void*)v[i]);
        if ((u64)DecodePointer(e) != v[i] || (u64)e == v[i]) {
            return 1 + (u64)i;
        }
    }
    return 0x600D;
}

static volatile u64 g_once_calls;
EXPORT int teb_once_fn(u64* once, void* param, void** ctx) {
    (void)once;
    (void)ctx;
    ++g_once_calls;
    return param == (void*)0x77;
}

/* InitOnceExecuteOnce runs its callback once per INIT_ONCE. */
EXPORT NOINLINE u64 t_crt_init_once(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    u64 once = 0;
    const u64 before = g_once_calls;
    if (!InitOnceExecuteOnce(&once, teb_once_fn, (void*)0x77, 0) || !InitOnceExecuteOnce(&once, teb_once_fn, (void*)0x77, 0)) {
        return 1;
    }
    return g_once_calls - before == 1 ? 0x600D : 2;
}

/* GetSystemInfo (AMD64, 4 KiB pages, 64 KiB granularity), the GetLogicalProcessorInformation
 * size query, and GetVersionExW (6.2.9200 for an exe that declares no supported OS). */
EXPORT NOINLINE u64 t_sysinfo(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    u8 si[48];
    GetSystemInfo(si);
    if (*(u16*)si != 9 || *(u32*)(si + 4) != 4096 || *(u32*)(si + 40) != 65536 || *(u32*)(si + 32) == 0) {
        return 1;
    }
    u32 len = 0;
    if (GetLogicalProcessorInformation(0, &len) || GetLastError() != 122 || len == 0 || len % 32 != 0) {
        return 2;
    }
    __attribute__((aligned(8))) u8 ver[276]; /* GetVersionExW fills it (an initializer would call memset) */
    *(u32*)ver = 276;
    if (!GetVersionExW(ver) || *(u32*)(ver + 4) != 6 || *(u32*)(ver + 8) != 2 || *(u32*)(ver + 12) != 9200 ||
        *(u32*)(ver + 16) != 2) {
        return 3;
    }
    return 0x600D;
}

static int env_has(const char* want) {
    u16* block = GetEnvironmentStringsW();
    int found = 0;
    for (const u16* p = block; *p != 0;) {
        int i = 0;
        while (want[i] != 0 && p[i] == (u16)want[i]) {
            ++i;
        }
        if (want[i] == 0 && p[i] == 0) {
            found = 1;
        }
        while (*p != 0) {
            ++p;
        }
        ++p;
    }
    FreeEnvironmentStringsW(block);
    return found;
}

/* SetEnvironmentVariableA shows in GetEnvironmentStringsW; a null value deletes it. */
EXPORT NOINLINE u64 t_env(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    if (!SetEnvironmentVariableA("WSE_CORPUS_VAR", "v1") || !env_has("WSE_CORPUS_VAR=v1")) {
        return 1;
    }
    if (!SetEnvironmentVariableA("wse_corpus_var", 0) || env_has("WSE_CORPUS_VAR=v1")) {
        return 2;
    }
    return 0x600D;
}

static volatile u64 g_queue_event;
static volatile u64 g_queue_param;
EXPORT void teb_queue_callback(void* param, u8 fired) {
    g_queue_param = (u64)param + fired;
    SetEvent(g_queue_event);
}

/* A one-shot timer-queue timer calls back with (parameter, TRUE). */
EXPORT NOINLINE u64 t_timer_queue(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 param = (a & ~(u64)0xFF) | 0x10;
    g_queue_event = CreateEventW(0, 0, 0, 0);
    const u64 q = CreateTimerQueue();
    u64 t = 0;
    if (g_queue_event == 0 || q == 0 || !CreateTimerQueueTimer(&t, q, teb_queue_callback, (void*)param, 1, 0, 0)) {
        return 1;
    }
    if (WaitForSingleObject(g_queue_event, INFINITE) != 0 || g_queue_param != param + 1) {
        return 2;
    }
    if (!DeleteTimerQueueTimer(q, t, ~(u64)0)) {
        return 3;
    }
    CloseHandle(g_queue_event);
    return 0x600D;
}
