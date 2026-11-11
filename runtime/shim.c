#include "shim.h"
#include "teb.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>

static uint64_t mono_ms(void) { return GetTickCount64(); }
static uint64_t mono_ticks(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 10000000.0 / (double)f.QuadPart);
}
static uint32_t current_tid(void) { return (uint32_t)GetCurrentThreadId(); }
static uint32_t current_pid(void) { return (uint32_t)GetCurrentProcessId(); }
static void sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t mono_ms(void) { return mono_ns() / 1000000ull; }
static uint64_t mono_ticks(void) { return mono_ns() / 100ull; } /* 10 MHz */
static uint32_t current_tid(void) { return (uint32_t)syscall(SYS_gettid); }
static uint32_t current_pid(void) { return (uint32_t)getpid(); }
static void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static uint64_t g_image_base;

void wse2_shim_set_image_base(uint64_t base) { g_image_base = base; }
uint64_t wse2_shim_image_base(void) { return g_image_base; }

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++;
        unsigned char y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

static int guest_streq_ci(uint64_t va, const char *s) {
    for (uint64_t i = 0; i < 260; i++) {
        unsigned char x = ld8(va + i);
        unsigned char y = (unsigned char)s[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 0;
}

static int guest_wstreq_ci(uint64_t va, const char *s) {
    for (uint64_t i = 0; i < 260; i++) {
        uint16_t w = ld16(va + i * 2);
        unsigned char x = w < 0x80 ? (unsigned char)w : 0;
        unsigned char y = (unsigned char)s[i];
        if (w >= 0x80) return 0;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 0;
}

static uint32_t shim_GetTickCount64(CPU *c) {
    c->r[RAX] = mono_ms();
    return 0;
}

static uint32_t shim_GetTickCount(CPU *c) {
    c->r[RAX] = (uint32_t)mono_ms();
    return 0;
}

static uint32_t shim_Sleep(CPU *c) {
    sleep_ms((uint32_t)c->r[RCX]);
    c->r[RAX] = 0;
    return 0;
}

static uint32_t shim_GetCurrentThreadId(CPU *c) {
    c->r[RAX] = current_tid();
    return 0;
}

static uint32_t shim_GetCurrentProcessId(CPU *c) {
    c->r[RAX] = current_pid();
    return 0;
}

static uint32_t shim_GetLastError(CPU *c) {
    c->r[RAX] = c->teb ? ld32(c->teb + WSE2_TEB_LAST_ERROR) : 0;
    return 0;
}

static uint32_t shim_SetLastError(CPU *c) {
    if (c->teb) {
        st32(c->teb + WSE2_TEB_LAST_ERROR, (uint32_t)c->r[RCX]);
    }
    c->r[RAX] = 0;
    return 0;
}

static uint32_t shim_GetModuleHandleA(CPU *c) {
    uint64_t name = c->r[RCX];
    if (name == 0 || guest_streq_ci(name, "mb_warband_wse2_x64.exe")) {
        c->r[RAX] = g_image_base;
    } else {
        c->r[RAX] = 0;
    }
    return 0;
}

static uint32_t shim_GetModuleHandleW(CPU *c) {
    uint64_t name = c->r[RCX];
    if (name == 0 || guest_wstreq_ci(name, "mb_warband_wse2_x64.exe")) {
        c->r[RAX] = g_image_base;
    } else {
        c->r[RAX] = 0;
    }
    return 0;
}

static uint32_t shim_GetProcAddress(CPU *c) {
    /* Dynamic lookup is served by the import registry in a later increment.
     * Returning NULL is honest here: callers treat it as a missing export. */
    c->r[RAX] = 0;
    return 0;
}

static uint32_t shim_QueryPerformanceCounter(CPU *c) {
    uint64_t out = c->r[RCX];
    if (out) {
        st64(out, mono_ticks());
    }
    c->r[RAX] = 1;
    return 0;
}

static uint32_t shim_QueryPerformanceFrequency(CPU *c) {
    uint64_t out = c->r[RCX];
    if (out) {
        st64(out, 10000000ull);
    }
    c->r[RAX] = 1;
    return 0;
}

static uint32_t shim_ExitProcess(CPU *c) {
    wse2_fatal("ExitProcess(%u)", (unsigned)(c->r[RCX] & 0xFFFFFFFFu));
    return 0;
}

typedef struct {
    const char *dll;
    const char *name;
    Wse2Fn fn;
} ShimEntry;

static const ShimEntry kShims[] = {
    { "KERNEL32.dll", "GetTickCount64", shim_GetTickCount64 },
    { "KERNEL32.dll", "GetTickCount", shim_GetTickCount },
    { "KERNEL32.dll", "Sleep", shim_Sleep },
    { "KERNEL32.dll", "GetCurrentThreadId", shim_GetCurrentThreadId },
    { "KERNEL32.dll", "GetCurrentProcessId", shim_GetCurrentProcessId },
    { "KERNEL32.dll", "GetLastError", shim_GetLastError },
    { "KERNEL32.dll", "SetLastError", shim_SetLastError },
    { "KERNEL32.dll", "GetModuleHandleA", shim_GetModuleHandleA },
    { "KERNEL32.dll", "GetModuleHandleW", shim_GetModuleHandleW },
    { "KERNEL32.dll", "GetProcAddress", shim_GetProcAddress },
    { "KERNEL32.dll", "QueryPerformanceCounter", shim_QueryPerformanceCounter },
    { "KERNEL32.dll", "QueryPerformanceFrequency", shim_QueryPerformanceFrequency },
    { "KERNEL32.dll", "ExitProcess", shim_ExitProcess },
};

Wse2Fn wse2_shim_lookup(const char *dll, const char *name) {
    for (size_t i = 0; i < sizeof kShims / sizeof kShims[0]; i++) {
        if (ci_equal(kShims[i].dll, dll) && strcmp(kShims[i].name, name) == 0) {
            return kShims[i].fn;
        }
    }
    return NULL;
}
