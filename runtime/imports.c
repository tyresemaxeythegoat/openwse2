#include "imports.h"
#include "shim.h"

#include <stdio.h>
#include <string.h>

#define MAX_IMPORTS 4096
#define THUNK_STRIDE 16
#define MAX_NAME 96

typedef struct {
    char dll[32];
    char name[MAX_NAME];
    char full[32 + MAX_NAME + 2];
    uint64_t thunk;
    Wse2Fn fn;
} ImportEntry;

static ImportEntry entries[MAX_IMPORTS];
static size_t entry_count;
static uint64_t arena_base;
static uint64_t arena_size;
static uint64_t arena_used;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

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

static size_t bounded_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

int wse2_imports_init(uint64_t base, uint64_t size) {
    memset(entries, 0, sizeof entries);
    entry_count = 0;
    arena_base = base;
    arena_size = size;
    arena_used = 0;
    return 0;
}

uint64_t wse2_imports_add(const char *dll, const char *name, Wse2Fn fn) {
    for (size_t i = 0; i < entry_count; i++) {
        if (ci_equal(entries[i].dll, dll) && strcmp(entries[i].name, name) == 0) {
            return entries[i].thunk;
        }
    }
    if (entry_count == MAX_IMPORTS) {
        wse2_fatal("import table full (%d)", MAX_IMPORTS);
    }
    if (arena_used + THUNK_STRIDE > arena_size) {
        wse2_fatal("import thunk arena exhausted");
    }
    if (strlen(dll) >= sizeof entries[0].dll || strlen(name) >= MAX_NAME) {
        wse2_fatal("import name too long: %s!%s", dll, name);
    }
    ImportEntry *e = &entries[entry_count++];
    snprintf(e->dll, sizeof e->dll, "%s", dll);
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->full, sizeof e->full, "%s!%s", dll, name);
    e->thunk = arena_base + arena_used;
    e->fn = fn;
    arena_used += THUNK_STRIDE;
    wse2_dispatch_register(e->thunk, e->fn, e->full);
    return e->thunk;
}

uint64_t wse2_imports_find(const char *dll, const char *name) {
    for (size_t i = 0; i < entry_count; i++) {
        if (ci_equal(entries[i].dll, dll) && strcmp(entries[i].name, name) == 0) {
            return entries[i].thunk;
        }
    }
    return 0;
}

static const char *image_str(const uint8_t *base, uint64_t size, uint64_t rva) {
    if (rva >= size) {
        return NULL;
    }
    const char *s = (const char *)(base + rva);
    size_t cap = (size_t)(size - rva);
    if (bounded_len(s, cap) == cap) {
        return NULL;
    }
    return s;
}

int wse2_imports_bind(const GuestSpace *g, const Wse2PeImage *img, Wse2ImportStats *stats) {
    if (stats) memset(stats, 0, sizeof *stats);
    if (!img->import_rva || img->import_size < 20) {
        return 0;
    }
    uint8_t *base = g->base;
    uint32_t dir_end = img->import_rva + img->import_size;
    uint32_t off = img->import_rva;

    while (off + 20 <= dir_end) {
        const uint8_t *d = base + off;
        uint32_t oft = rd32(d + 0);
        uint32_t name_rva = rd32(d + 12);
        uint32_t ft = rd32(d + 16);
        if (!oft && !name_rva && !ft && !rd32(d + 4) && !rd32(d + 8)) {
            return 0; /* terminator */
        }
        const char *dll = image_str(base, img->size_of_image, name_rva);
        if (!dll) {
            wse2_fatal("import descriptor with bad DLL name RVA 0x%X", name_rva);
        }
        if (ft == 0 || ft >= img->size_of_image) {
            wse2_fatal("import descriptor for %s has bad FirstThunk", dll);
        }
        uint32_t thunk_rva = oft ? oft : ft;
        for (uint32_t i = 0;; i++) {
            uint64_t slot = (uint64_t)thunk_rva + (uint64_t)i * 8;
            if (slot + 8 > img->size_of_image) {
                wse2_fatal("import thunk table for %s runs past image", dll);
            }
            uint64_t val = rd64(base + slot);
            if (val == 0) {
                break;
            }
            char oname[MAX_NAME];
            if (val & (1ull << 63)) {
                snprintf(oname, sizeof oname, "ord#%u", (unsigned)(val & 0xFFFF));
            } else {
                uint32_t hint_rva = (uint32_t)(val & 0x7FFFFFFF);
                if ((uint64_t)hint_rva + 2 >= img->size_of_image) {
                    wse2_fatal("import name RVA out of range in %s", dll);
                }
                const char *nm = image_str(base, img->size_of_image, hint_rva + 2);
                if (!nm) {
                    wse2_fatal("unterminated import name in %s", dll);
                }
                snprintf(oname, sizeof oname, "%s", nm);
            }
            uint64_t iat = (uint64_t)ft + (uint64_t)i * 8;
            if (iat + 8 > img->size_of_image) {
                wse2_fatal("IAT for %s runs past image", dll);
            }
            Wse2Fn fn = wse2_shim_lookup(dll, oname);
            uint64_t thunk = wse2_imports_add(dll, oname, fn);
            wr64(base + iat, thunk);
            if (stats) {
                stats->total++;
                if (fn) stats->bound++;
                else stats->missing++;
            }
        }
        off += 20;
    }
    /* A full directory without a terminator is legal only if it ends exactly. */
    if (off != dir_end) {
        wse2_fatal("import directory has trailing bytes");
    }
    return 0;
}
