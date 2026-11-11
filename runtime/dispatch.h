/* Indirect-call dispatch: guest VA -> native function.
 *
 * Direct calls are lowered to C calls by name at lift time. Indirect calls
 * (virtual dispatch, function pointers, IAT entries) arrive here with the
 * guest target address and fail loudly if it was never registered.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

typedef uint32_t (*Wse2Fn)(CPU *c);

int wse2_dispatch_init(size_t initial_capacity);
int wse2_dispatch_register(uint64_t guest_va, Wse2Fn fn, const char *name);

/* Returns 1 if the address is registered, 0 otherwise. fn may be NULL for a
 * known-but-unimplemented import. */
int wse2_dispatch_resolve(uint64_t guest_va, Wse2Fn *fn, const char **name_out);

uint32_t wse2_dispatch_call(CPU *c, uint64_t guest_va);
