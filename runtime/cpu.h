/* Production x86-64 guest CPU model for the WSE2 native runtime.
 *
 * Semantics are identical to proof/cpu.h, which was validated against real
 * WSE2 machine code in Unicorn (integer, flags, SSE, stack). The difference
 * here is addressing: guest addresses are full VAs (image base 0x140000000),
 * so the memory layer applies MEM_BIAS before touching the host mapping.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

typedef struct {
    uint64_t r[16];
    uint32_t flags;
    uint64_t rip;
    struct { uint64_t q[2]; } xmm[16];
    uint64_t teb;       /* guest VA of the current thread's TEB */
    uint64_t reserved;  /* keep 16-byte alignment */
} CPU;

#define FLAG_CF 0x0001u
#define FLAG_PF 0x0004u
#define FLAG_AF 0x0010u
#define FLAG_ZF 0x0040u
#define FLAG_SF 0x0080u
#define FLAG_OF 0x0800u

#define CF(c) (((c)->flags & FLAG_CF) != 0)
#define PF(c) (((c)->flags & FLAG_PF) != 0)
#define ZF(c) (((c)->flags & FLAG_ZF) != 0)
#define SF(c) (((c)->flags & FLAG_SF) != 0)
#define OF(c) (((c)->flags & FLAG_OF) != 0)

void wse2_fatal(const char *fmt, ...);

/* Guest memory. MEM points at guest VA MEM_BIAS; valid size is MEM_LIMIT. */
extern uint8_t *MEM;
extern uint64_t MEM_BIAS;
extern uint64_t MEM_LIMIT;

static inline uint8_t *mem_ptr(uint64_t a) {
    return MEM + (a - MEM_BIAS);
}

static inline void mem_check(uint64_t a, uint64_t n) {
    uint64_t off;
    if (a < MEM_BIAS) {
        wse2_fatal("guest memory fault: addr=0x%llX below base 0x%llX",
                   (unsigned long long)a, (unsigned long long)MEM_BIAS);
    }
    off = a - MEM_BIAS;
    if (off > MEM_LIMIT || n > MEM_LIMIT - off) {
        wse2_fatal("guest memory fault: addr=0x%llX size=%llu",
                   (unsigned long long)a, (unsigned long long)n);
    }
}

static inline uint8_t ld8(uint64_t a) { mem_check(a, 1); return mem_ptr(a)[0]; }
static inline uint16_t ld16(uint64_t a) { uint16_t v; mem_check(a, 2); memcpy(&v, mem_ptr(a), 2); return v; }
static inline uint32_t ld32(uint64_t a) { uint32_t v; mem_check(a, 4); memcpy(&v, mem_ptr(a), 4); return v; }
static inline uint64_t ld64(uint64_t a) { uint64_t v; mem_check(a, 8); memcpy(&v, mem_ptr(a), 8); return v; }

static inline void st8(uint64_t a, uint8_t v) { mem_check(a, 1); mem_ptr(a)[0] = v; }
static inline void st16(uint64_t a, uint16_t v) { mem_check(a, 2); memcpy(mem_ptr(a), &v, 2); }
static inline void st32(uint64_t a, uint32_t v) { mem_check(a, 4); memcpy(mem_ptr(a), &v, 4); }
static inline void st64(uint64_t a, uint64_t v) { mem_check(a, 8); memcpy(mem_ptr(a), &v, 8); }

static inline uint32_t xmm32(const CPU *c, int i) { return (uint32_t)c->xmm[i].q[0]; }
static inline uint64_t xmm64(const CPU *c, int i) { return c->xmm[i].q[0]; }
static inline void xmm_set32(CPU *c, int i, uint32_t v) { c->xmm[i].q[0] = v; c->xmm[i].q[1] = 0; }
static inline void xmm_set32_keep(CPU *c, int i, uint32_t v) {
    c->xmm[i].q[0] = (c->xmm[i].q[0] & ~0xFFFFFFFFull) | v;
}
static inline void xmm_set64(CPU *c, int i, uint64_t v) { c->xmm[i].q[0] = v; c->xmm[i].q[1] = 0; }
static inline void xmm_ld128(CPU *c, int i, uint64_t a) {
    c->xmm[i].q[0] = ld64(a);
    c->xmm[i].q[1] = ld64(a + 8);
}
static inline void xmm_st128(CPU *c, int i, uint64_t a) {
    st64(a, c->xmm[i].q[0]);
    st64(a + 8, c->xmm[i].q[1]);
}

static inline uint64_t pop64(CPU *c) {
    uint64_t v = ld64(c->r[RSP]);
    c->r[RSP] += 8;
    return v;
}

static inline uint64_t maskbits(int bits) {
    return bits == 64 ? ~0ull : ((1ull << bits) - 1);
}

static inline uint32_t parity64(uint64_t v) {
    v &= 0xFF;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v & 1) ? FLAG_PF : 0;
}

static inline void flags_logic(CPU *c, uint64_t r, int bits) {
    uint64_t m = maskbits(bits);
    r &= m;
    uint32_t f = parity64(r);
    if (r == 0) f |= FLAG_ZF;
    if (r & (1ull << (bits - 1))) f |= FLAG_SF;
    c->flags = f;
}

/* Add with carry-in. `ci` is 0 for add/cmp, CF for adc. */
static inline void flags_add(CPU *c, uint64_t a, uint64_t b, uint64_t ci, uint64_t r, int bits) {
    uint64_t m = maskbits(bits);
    a &= m; b &= m; ci &= 1; r &= m;
    uint32_t f = parity64(r);
    if (r == 0) f |= FLAG_ZF;
    if (r & (1ull << (bits - 1))) f |= FLAG_SF;
    if ((unsigned __int128)a + b + ci > (unsigned __int128)m) f |= FLAG_CF;
    if ((~(a ^ b) & (a ^ r) & (1ull << (bits - 1))) != 0) f |= FLAG_OF;
    if (((a ^ b ^ r) & 0x10) != 0) f |= FLAG_AF;
    c->flags = f;
}

/* Subtract with borrow-in. `bi` is 0 for sub/cmp, CF for sbb. */
static inline void flags_sub(CPU *c, uint64_t a, uint64_t b, uint64_t bi, uint64_t r, int bits) {
    uint64_t m = maskbits(bits);
    a &= m; b &= m; bi &= 1; r &= m;
    uint32_t f = parity64(r);
    if (r == 0) f |= FLAG_ZF;
    if (r & (1ull << (bits - 1))) f |= FLAG_SF;
    if ((unsigned __int128)a < (unsigned __int128)b + bi) f |= FLAG_CF;
    if (((a ^ b) & (a ^ r) & (1ull << (bits - 1))) != 0) f |= FLAG_OF;
    if (((a ^ b ^ r) & 0x10) != 0) f |= FLAG_AF;
    c->flags = f;
}

static inline void flags_inc(CPU *c, uint64_t a, uint64_t r, int bits) {
    uint32_t cf = c->flags & FLAG_CF;
    flags_add(c, a, 1, 0, r, bits);
    c->flags = (c->flags & ~FLAG_CF) | cf;
}

static inline void flags_dec(CPU *c, uint64_t a, uint64_t r, int bits) {
    uint32_t cf = c->flags & FLAG_CF;
    flags_sub(c, a, 1, 0, r, bits);
    c->flags = (c->flags & ~FLAG_CF) | cf;
}

static inline void flags_shift(CPU *c, uint64_t a, uint64_t r, unsigned cnt, int bits, int kind) {
    uint64_t m = maskbits(bits);
    a &= m; r &= m;
    uint32_t f = 0;
    if (cnt == 0) return;
    if (kind == 0) { /* shl */
        if ((a >> (bits - cnt)) & 1) f |= FLAG_CF;
        if (cnt == 1 && (((r >> (bits - 1)) & 1) ^ (f & FLAG_CF ? 1 : 0))) f |= FLAG_OF;
    } else if (kind == 1) { /* shr */
        if ((a >> (cnt - 1)) & 1) f |= FLAG_CF;
        if (cnt == 1 && ((a >> (bits - 1)) & 1)) f |= FLAG_OF;
    } else { /* sar */
        if ((a >> (cnt - 1)) & 1) f |= FLAG_CF;
    }
    f |= parity64(r);
    if (r == 0) f |= FLAG_ZF;
    if (r & (1ull << (bits - 1))) f |= FLAG_SF;
    c->flags = f;
}

static inline void flags_imul(CPU *c, uint64_t a, uint64_t b, uint64_t r) {
    __int128 p = (__int128)(int64_t)a * (__int128)(int64_t)b;
    if (p != (__int128)(int64_t)r) c->flags = (c->flags & ~(FLAG_CF | FLAG_OF)) | FLAG_CF | FLAG_OF;
    else c->flags &= ~(FLAG_CF | FLAG_OF);
}

static inline void flags_bt(CPU *c, uint64_t a, uint64_t b, int bits) {
    c->flags = (c->flags & ~FLAG_CF) | (((a >> (b & (bits - 1))) & 1) ? FLAG_CF : 0);
}
