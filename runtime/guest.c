#include "guest.h"
#include "cpu.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint64_t page_align(uint64_t v, uint64_t page) {
    return (v + page - 1) & ~(page - 1);
}

int wse2_guest_reserve(GuestSpace *g, uint64_t preferred, uint64_t size) {
    memset(g, 0, sizeof *g);
    if (size == 0) {
        wse2_fatal("guest_reserve: zero size");
    }

#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size = page_align(size, si.dwPageSize);
    if (preferred) {
        void *p = VirtualAlloc((void *)(uintptr_t)preferred, (SIZE_T)size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == (void *)(uintptr_t)preferred) {
            g->base = (uint8_t *)p;
            g->guest_base = preferred;
            g->size = size;
            return 0;
        }
        if (p) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    void *p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        return -1;
    }
    g->base = (uint8_t *)p;
    g->guest_base = (uint64_t)(uintptr_t)p;
    g->size = size;
    g->rebased = preferred && g->guest_base != preferred;
    if (g->rebased) {
        fprintf(stderr, "wse2: guest reservation rebased: wanted 0x%llX got 0x%llX\n",
                (unsigned long long)preferred, (unsigned long long)g->guest_base);
    }
    return 0;
#else
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    size = page_align(size, (uint64_t)page);

    if (preferred) {
        void *p = mmap((void *)(uintptr_t)preferred, (size_t)size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)(uintptr_t)preferred) {
            g->base = (uint8_t *)p;
            g->guest_base = preferred;
            g->size = size;
            return 0;
        }
        if (p != MAP_FAILED) {
            munmap(p, (size_t)size);
        }
    }
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return -1;
    }
    g->base = (uint8_t *)p;
    g->guest_base = (uint64_t)(uintptr_t)p;
    g->size = size;
    g->rebased = preferred && g->guest_base != preferred;
    if (g->rebased) {
        fprintf(stderr, "wse2: guest reservation rebased: wanted 0x%llX got 0x%llX\n",
                (unsigned long long)preferred, (unsigned long long)g->guest_base);
    }
    return 0;
#endif
}

void wse2_guest_release(GuestSpace *g) {
    if (!g->base) {
        return;
    }
#ifdef _WIN32
    VirtualFree(g->base, 0, MEM_RELEASE);
#else
    munmap(g->base, (size_t)g->size);
#endif
    memset(g, 0, sizeof *g);
}

void wse2_guest_attach(const GuestSpace *g) {
    MEM = g->base;
    MEM_BIAS = g->guest_base;
    MEM_LIMIT = g->size;
}
