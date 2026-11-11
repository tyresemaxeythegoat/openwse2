#include "dispatch.h"

typedef struct {
    uint64_t va;
    Wse2Fn fn;
    const char *name;
} Entry;

static Entry *table;
static size_t capacity;
static size_t used;

static size_t hash_slot(uint64_t va, size_t cap) {
    uint64_t h = va + 0x9E3779B97F4A7C15ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return (size_t)h & (cap - 1);
}

static int grow(void) {
    size_t ncap = capacity ? capacity * 2 : 256;
    Entry *nt = (Entry *)calloc(ncap, sizeof *nt);
    if (!nt) {
        return -1;
    }
    for (size_t i = 0; i < capacity; i++) {
        if (!table[i].va) continue;
        size_t s = hash_slot(table[i].va, ncap);
        while (nt[s].va) s = (s + 1) & (ncap - 1);
        nt[s] = table[i];
    }
    free(table);
    table = nt;
    capacity = ncap;
    return 0;
}

int wse2_dispatch_init(size_t initial_capacity) {
    free(table);
    table = NULL;
    used = 0;
    capacity = 1;
    while (capacity < initial_capacity) capacity <<= 1;
    if (capacity < 64) capacity = 64;
    table = (Entry *)calloc(capacity, sizeof *table);
    if (!table) {
        capacity = 0;
        return -1;
    }
    return 0;
}

int wse2_dispatch_register(uint64_t guest_va, Wse2Fn fn, const char *name) {
    if (guest_va == 0) {
        return -1;
    }
    if (!table && wse2_dispatch_init(64) != 0) {
        return -1;
    }
    if ((used + 1) * 10 >= capacity * 7 && grow() != 0) {
        return -1;
    }
    size_t s = hash_slot(guest_va, capacity);
    while (table[s].va) {
        if (table[s].va == guest_va) {
            table[s].fn = fn;
            table[s].name = name;
            return 0;
        }
        s = (s + 1) & (capacity - 1);
    }
    table[s].va = guest_va;
    table[s].fn = fn;
    table[s].name = name;
    used++;
    return 0;
}

int wse2_dispatch_resolve(uint64_t guest_va, Wse2Fn *fn, const char **name_out) {
    if (!table || guest_va == 0) {
        return 0;
    }
    size_t s = hash_slot(guest_va, capacity);
    while (table[s].va) {
        if (table[s].va == guest_va) {
            if (fn) *fn = table[s].fn;
            if (name_out) *name_out = table[s].name;
            return 1;
        }
        s = (s + 1) & (capacity - 1);
    }
    return 0;
}

uint32_t wse2_dispatch_call(CPU *c, uint64_t guest_va) {
    Wse2Fn fn = NULL;
    const char *name = NULL;
    if (!wse2_dispatch_resolve(guest_va, &fn, &name)) {
        wse2_fatal("indirect call to unregistered guest address 0x%llX",
                   (unsigned long long)guest_va);
    }
    if (!fn) {
        wse2_fatal("call into unimplemented import '%s' at 0x%llX",
                   name ? name : "?", (unsigned long long)guest_va);
    }
    uint32_t rc = fn(c);
    if (rc != 0) {
        wse2_fatal("native function '%s' returned error %u",
                   name ? name : "?", rc);
    }
    return rc;
}
