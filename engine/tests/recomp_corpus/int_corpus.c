/* R1 differential corpus, integer family (`docs/RECOMP_PLAN.md` "R1 design", "Oracle").
 *
 * Our own code, written for this test. CMake compiles it with clang for Win64
 * (`--target=x86_64-pc-windows-msvc`, freestanding, no SSE) and links it with lld-link into a
 * PE at 0x140000000 with no relocations. `recomp_llvm_test` maps that PE at its base, finds the
 * functions with the R1 discovery, translates them, and runs every exported function both
 * natively (through an `ms_abi` pointer) and translated on the same inputs, comparing `rax`,
 * the memory buffer, and the callee-saved registers.
 *
 * Every export is `u64 f(u64 a, u64 b, u64 c, u64 d)`. Exports named `m_*` get a 4096-byte,
 * 16-byte-aligned buffer in `a` and must stay inside it. Nothing here may trap natively
 * (division guards) or call a library function (no imports: the PE has none).
 */

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;
typedef signed char i8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

void corpus_entry(void) {}

/* --- ALU, flags, conditions ---------------------------------------------------------------- */

EXPORT u64 alu_mix(u64 a, u64 b, u64 c, u64 d) {
    u64 x = a + b;
    x ^= c - d;
    x |= (a & 0xFF00FF00FF00FF00ULL);
    x -= ~b;
    return x + (a < b) + 2 * (c > d) + 4 * ((i64)a < (i64)d) + 8 * (x == c);
}

EXPORT u64 cmp32(u64 a, u64 b, u64 c, u64 d) {
    i32 x = (i32)a, y = (i32)b;
    u32 ux = (u32)c, uy = (u32)d;
    u64 r = 0;
    r |= (u64)(x < y) << 0;
    r |= (u64)(x <= y) << 1;
    r |= (u64)(x > y) << 2;
    r |= (u64)(x >= y) << 3;
    r |= (u64)(ux < uy) << 4;
    r |= (u64)(ux <= uy) << 5;
    r |= (u64)(ux > uy) << 6;
    r |= (u64)(ux >= uy) << 7;
    r |= (u64)(x == (i32)ux) << 8;
    r |= (u64)((x - y) < 0) << 9;
    return r;
}

EXPORT u64 select_minmax(u64 a, u64 b, u64 c, u64 d) {
    i64 lo = (i64)a < (i64)b ? (i64)a : (i64)b;
    u64 hi = c > d ? c : d;
    u32 m = (u32)a < (u32)c ? (u32)a : (u32)c;
    return (u64)lo ^ (hi << 1) ^ ((u64)m << 7);
}

EXPORT u64 narrow_ops(u64 a, u64 b, u64 c, u64 d) {
    u8 x8 = (u8)a, y8 = (u8)b;
    i8 s8 = (i8)c;
    u16 x16 = (u16)d, y16 = (u16)(a >> 16);
    i16 s16 = (i16)(b >> 8);
    u8 r8 = (u8)(x8 * y8 + (u8)(x8 >> 3));
    u16 r16 = (u16)(x16 - y16) ^ (u16)(x16 << 3);
    i32 sx = s8 * 3 + s16 / 7;
    return (u64)r8 | ((u64)r16 << 8) | ((u64)(u32)sx << 24) | ((u64)(x8 > y8) << 63);
}

EXPORT u64 add128(u64 a, u64 b, u64 c, u64 d) {
    unsigned __int128 x = ((unsigned __int128)a << 64) | b;
    unsigned __int128 y = ((unsigned __int128)c << 64) | d;
    unsigned __int128 s = x + y;
    unsigned __int128 t = x - y;
    return (u64)(s >> 64) ^ (u64)s ^ (u64)(t >> 64) * 3 ^ (u64)t;
}

/* --- shifts, rotates, bits ----------------------------------------------------------------- */

EXPORT u64 shifts(u64 a, u64 b, u64 c, u64 d) {
    unsigned n = (unsigned)b & 63, m = (unsigned)c & 31;
    u64 r = (a << n) ^ (a >> (63 - n)) ^ (u64)((i64)a >> n);
    r += (u64)((u32)d << m) + (u64)((i32)d >> m) + (u64)((u32)d >> m);
    r ^= (u64)((u16)a << 3) | (u64)((u8)c >> 2);
    return r;
}

EXPORT u64 rotates(u64 a, u64 b, u64 c, u64 d) {
    unsigned n = (unsigned)b & 63, m = (unsigned)c & 31;
    u64 r64 = (a << n) | (a >> ((64 - n) & 63));
    u32 x = (u32)d;
    u32 r32 = (x >> m) | (x << ((32 - m) & 31));
    return r64 ^ ((u64)r32 << 13);
}

EXPORT u64 bit_scan(u64 a, u64 b, u64 c, u64 d) {
    (void)d;
    u64 r = 0;
    if (a != 0) r += (u64)__builtin_ctzll(a) + 64 * (u64)__builtin_clzll(a);
    if ((u32)b != 0) r += 4096 * (u64)__builtin_ctz((u32)b) + 131072 * (u64)__builtin_clz((u32)b);
    r ^= (u64)__builtin_bswap64(c) ^ ((u64)__builtin_bswap32((u32)c) << 1);
    return r;
}

EXPORT u64 popcount_soft(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    u64 n = 0;
    for (u64 x = a; x != 0; x &= x - 1) ++n;
    u64 y = b - ((b >> 1) & 0x5555555555555555ULL);
    y = (y & 0x3333333333333333ULL) + ((y >> 2) & 0x3333333333333333ULL);
    y = (y + (y >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return n * 1000 + ((y * 0x0101010101010101ULL) >> 56);
}

EXPORT u64 bit_test(u64 a, u64 b, u64 c, u64 d) {
    unsigned n = (unsigned)b & 63;
    u64 r = (a >> n) & 1;
    r |= ((c | (1ULL << n)) ^ (d & ~(1ULL << ((n + 7) & 63)))) << 1;
    r ^= (a ^ (1ULL << ((n * 3) & 63)));
    return r;
}

EXPORT u64 bit_reverse(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    u64 r = 0;
    for (int i = 0; i < 64; ++i) {
        r = (r << 1) | (a & 1);
        a >>= 1;
    }
    return r;
}

/* --- multiply and divide -------------------------------------------------------------------- */

EXPORT u64 mul_variants(u64 a, u64 b, u64 c, u64 d) {
    unsigned __int128 p = (unsigned __int128)a * b;
    __int128 q = (__int128)(i64)c * (i64)d;
    u32 m32 = (u32)a * (u32)c;
    i32 s32 = (i32)b * -77;
    return (u64)(p >> 64) ^ (u64)p ^ (u64)(q >> 64) * 5 ^ (u64)q ^ m32 ^ ((u64)(u32)s32 << 32);
}

EXPORT u64 div_unsigned(u64 a, u64 b, u64 c, u64 d) {
    u64 r = 0;
    if (b != 0) r += a / b + 3 * (a % b);
    if ((u32)d != 0) r ^= ((u64)((u32)c / (u32)d) << 1) + (u32)c % (u32)d;
    r += a / 10 + a % 1000 + (u32)c / 7;
    return r;
}

EXPORT u64 div_signed(u64 a, u64 b, u64 c, u64 d) {
    i64 x = (i64)a, y = (i64)b;
    i32 u = (i32)c, v = (i32)d;
    i64 r = 0;
    if (y != 0 && !(x == (i64)0x8000000000000000ULL && y == -1)) r += x / y + 3 * (x % y);
    if (v != 0 && !(u == (i32)0x80000000U && v == -1)) r ^= (i64)(u / v) * 7 + u % v;
    r += x / 3 - x % 16 + u / -9;
    return (u64)r;
}

/* --- control flow --------------------------------------------------------------------------- */

EXPORT u64 switch_dense(u64 a, u64 b, u64 c, u64 d) {
    switch (a % 13) {
    case 0: return b + c;
    case 1: return b - d;
    case 2: return c * 3;
    case 3: return d ^ 0x5555;
    case 4: return b << 2;
    case 5: return c >> 3;
    case 6: return b | d;
    case 7: return c & d;
    case 8: return ~b;
    case 9: return b * d;
    case 10: return c + 1000;
    case 11: return d - b;
    default: return 0x123456789ULL;
    }
}

EXPORT u64 switch_sparse(u64 a, u64 b, u64 c, u64 d) {
    (void)d;
    switch ((u32)a & 0xFFF) {
    case 3: return b;
    case 100: return c;
    case 2000: return b + c;
    case 4000: return b ^ c;
    default: return a;
    }
}

NOINLINE u64 helper_gcd(u64 a, u64 b) {
    while (b != 0) {
        u64 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

EXPORT u64 call_direct(u64 a, u64 b, u64 c, u64 d) {
    return helper_gcd(a | 1, b | 1) + helper_gcd(c & 0xFFFF, (d & 0xFFFF) + 1) * 3;
}

NOINLINE u64 fact_rec(u64 n) { return n <= 1 ? 1 : n * fact_rec(n - 1); }

NOINLINE u64 fib_rec(u64 n) { return n < 2 ? n : fib_rec(n - 1) + fib_rec(n - 2); }

EXPORT u64 call_recursive(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    return fact_rec(a % 21) ^ fib_rec(b % 16);
}

NOINLINE u64 op_add(u64 x, u64 y) { return x + y; }
NOINLINE u64 op_sub(u64 x, u64 y) { return x - y; }
NOINLINE u64 op_mul(u64 x, u64 y) { return x * y; }
NOINLINE u64 op_xor(u64 x, u64 y) { return x ^ y; }

typedef u64 (*binop)(u64, u64);
static binop const k_ops[4] = {op_add, op_sub, op_mul, op_xor};

EXPORT u64 call_indirect(u64 a, u64 b, u64 c, u64 d) {
    u64 r = k_ops[a & 3](b, c);
    binop volatile f = k_ops[(d >> 5) & 3];
    return r ^ f(r, d);
}

NOINLINE u64 tail_target(u64 x, u64 y, u64 z) { return x * 31 + y * 7 + z; }

EXPORT u64 tail_call(u64 a, u64 b, u64 c, u64 d) {
    if (d & 1) return tail_target(a, b, c);
    return tail_target(c, a, b ^ d);
}

EXPORT u64 collatz(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    u64 n = (a & 0xFFFFF) + 1, steps = 0;
    while (n != 1 && steps < 1000) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        ++steps;
    }
    return steps * 1000000 + (b & 0xFF);
}

NOINLINE u64 six_args(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

EXPORT u64 stack_args(u64 a, u64 b, u64 c, u64 d) { return six_args(d, c, b, a, a ^ b, c ^ d); }

/* --- memory --------------------------------------------------------------------------------- */

EXPORT u64 m_sum_bytes(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const u8* p = (const u8*)a;
    u64 n = b & 4095, s = 0;
    for (u64 i = 0; i < n; ++i) s += p[i];
    return s;
}

EXPORT u64 m_reverse(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    u32* p = (u32*)a;
    u64 n = b & 1023;
    for (u64 i = 0, j = n; i + 1 < j; ++i, --j) {
        u32 t = p[i];
        p[i] = p[j - 1];
        p[j - 1] = t;
    }
    return n;
}

EXPORT u64 m_insertion_sort(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    i32* p = (i32*)a;
    u64 n = b & 127;
    for (u64 i = 1; i < n; ++i) {
        i32 k = p[i];
        u64 j = i;
        while (j > 0 && p[j - 1] > k) {
            p[j] = p[j - 1];
            --j;
        }
        p[j] = k;
    }
    return n ? (u64)(u32)p[0] : 0;
}

EXPORT u64 m_adler32(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const u8* p = (const u8*)a;
    u32 s1 = 1, s2 = 0;
    for (u64 i = 0; i < (b & 4095); ++i) {
        s1 = (s1 + p[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return ((u64)s2 << 16) | s1;
}

EXPORT u64 m_crc32(u64 a, u64 b, u64 c, u64 d) {
    (void)d;
    const u8* p = (const u8*)a;
    u32 crc = ~(u32)c;
    for (u64 i = 0; i < (b & 1023); ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

EXPORT u64 m_strlen_cmp(u64 a, u64 b, u64 c, u64 d) {
    (void)d;
    char* p = (char*)a;
    p[4095] = 0;
    p[(b & 2047) + 1] = 0;
    u64 n = 0;
    while (p[n] != 0) ++n;
    const char* q = p + 2048;
    p[2048 + (c & 1023)] = 0;
    int r = 0;
    for (u64 i = 0;; ++i) {
        if (p[i] != q[i] || p[i] == 0) {
            r = (u8)p[i] - (u8)q[i];
            break;
        }
    }
    return n * 1000 + (u64)(i64)r;
}

EXPORT u64 m_store_widths(u64 a, u64 b, u64 c, u64 d) {
    u8* p = (u8*)a + (c & 1023);
    p[0] = (u8)b;
    *(u16*)(p + 1) = (u16)(b >> 8);
    *(u32*)(p + 3) = (u32)(b >> 16);
    *(u64*)(p + 7) = b ^ d;
    *(i8*)(p + 15) = (i8)-(i8)b;
    return *(u64*)(p + 4) + *(i16*)(p + 1) + *(i8*)(p + 15);
}

EXPORT u64 m_struct_walk(u64 a, u64 b, u64 c, u64 d) {
    struct node {
        u32 next;
        i32 value;
        u16 tag;
        u8 flags;
    };
    struct node* nodes = (struct node*)a;
    u64 count = 4096 / sizeof(struct node);
    for (u64 i = 0; i < count; ++i) {
        nodes[i].next = (u32)((i * (b | 1) + c) % count);
        nodes[i].tag = (u16)(nodes[i].tag + (u16)i);
    }
    u64 i = d % count, s = 0;
    for (int k = 0; k < 200; ++k) {
        s += (u64)(i64)nodes[i].value + nodes[i].tag + (nodes[i].flags & 3);
        i = nodes[i].next;
    }
    return s;
}

EXPORT u64 m_atomics(u64 a, u64 b, u64 c, u64 d) {
    u64* p = (u64*)a + (c & 255);
    u32* q = (u32*)a + 600 + (d & 63);
    u64 old = __atomic_fetch_add(p, b, __ATOMIC_SEQ_CST);
    u32 prev = __atomic_exchange_n(q, (u32)c, __ATOMIC_SEQ_CST);
    u64 expect = old + b;
    int swapped = __atomic_compare_exchange_n(p, &expect, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(q + 1, (u32)b, __ATOMIC_SEQ_CST);
    __atomic_fetch_and(q + 2, (u32)d, __ATOMIC_SEQ_CST);
    return old ^ prev ^ (u64)swapped ^ expect;
}

EXPORT u64 m_matrix(u64 a, u64 b, u64 c, u64 d) {
    (void)d;
    i32* m = (i32*)a;           /* 3 matrices of 8x8 */
    i32* x = m, *y = m + 64, *z = m + 128;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            i32 s = (i32)b;
            for (int k = 0; k < 8; ++k) s += x[i * 8 + k] * y[k * 8 + j];
            z[i * 8 + j] = s ^ (i32)c;
        }
    return (u64)(u32)z[63] + z[0];
}
