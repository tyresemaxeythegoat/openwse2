/* Import thunk arena and PE import binder.
 *
 * Every imported function gets a unique guest VA inside a reserved arena.
 * Guest IAT slots are patched with those VAs. Lifted indirect calls reach the
 * dispatch table, which maps the VA to a native shim (or a NULL entry that
 * aborts by name).
 */
#pragma once
#include "pe.h"
#include "dispatch.h"

typedef struct {
    uint32_t total;
    uint32_t bound;
    uint32_t missing;
} Wse2ImportStats;

int wse2_imports_init(uint64_t arena_base, uint64_t arena_size);
uint64_t wse2_imports_add(const char *dll, const char *name, Wse2Fn fn);
uint64_t wse2_imports_find(const char *dll, const char *name);
int wse2_imports_bind(const GuestSpace *g, const Wse2PeImage *img, Wse2ImportStats *stats);
