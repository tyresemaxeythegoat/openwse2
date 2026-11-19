/* R1 differential corpus, SSE family (`docs/RECOMP_PLAN.md` "R1 design", "Oracle").
 *
 * Compiled with clang for Win64 (`--target=x86_64-pc-windows-msvc -O2 -ffreestanding
 * -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -msse4.1`) and linked into
 * `corpus.exe` alongside `int_corpus.c` (which defines `corpus_entry`). No headers, no
 * library calls, no traps. `m_*` exports receive a 4096-byte, 16-byte-aligned buffer in `a`.
 */

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;
typedef signed char i8;

typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef int v4i __attribute__((vector_size(16)));
typedef unsigned int v4u __attribute__((vector_size(16)));
typedef long long v2ll __attribute__((vector_size(16)));
typedef short v8s __attribute__((vector_size(16)));
typedef unsigned short v8us __attribute__((vector_size(16)));
typedef char v16c __attribute__((vector_size(16)));
typedef unsigned char v16uc __attribute__((vector_size(16)));
typedef int v2i __attribute__((vector_size(8)));
typedef float v2f __attribute__((vector_size(8)));

typedef union {
    v4f f;
    v2d d;
    v4i i;
    v4u u;
    v2ll q;
    v8s s;
    v8us us;
    v16c c;
    v16uc uc;
    u64 u64x2[2];
    i64 i64x2[2];
    u32 u32x4[4];
    i32 i32x4[4];
    u16 u16x8[8];
    i16 i16x8[8];
    u8 u8x16[16];
} v128;

/* Win64 MSVC target references `_fltused` when floating-point types are used. */
int _fltused = 1;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

static float f_of(u64 x) {
    union {
        u32 u;
        float f;
    } v;
    v.u = (u32)x;
    return v.f;
}

static u64 bits_f(float f) {
    union {
        u32 u;
        float f;
    } v;
    v.f = f;
    return (u64)v.u;
}

static double d_of(u64 x) {
    union {
        u64 u;
        double d;
    } v;
    v.u = x;
    return v.d;
}

static u64 bits_d(double d) {
    union {
        u64 u;
        double d;
    } v;
    v.d = d;
    return v.u;
}

/* Not inlined: inlined into a function with many calls, clang threads one masked index
 * through a chain of tables that discovery cannot bound (MSVC never emits that shape). */
NOINLINE static u32 pick_f32_bits(u64 raw, u64 sel) {
    switch (sel & 7ULL) {
    case 0: return 0x7FC01234U; /* QNaN */
    case 1: return 0x7F800000U; /* +inf */
    case 2: return 0xFF800000U; /* -inf */
    case 3: return 0x80000000U; /* -0.0f */
    case 4: return 0x00000000U; /* +0.0f */
    case 5: return 0x7F7FFFFFU; /* FLT_MAX */
    case 6: return 0x00000001U; /* min subnormal */
    default: return (u32)raw;
    }
}

NOINLINE static u64 pick_f64_bits(u64 raw, u64 sel) {
    switch (sel & 7ULL) {
    case 0: return 0x7FF8000000001234ULL; /* QNaN */
    case 1: return 0x7FF0000000000000ULL; /* +inf */
    case 2: return 0xFFF0000000000000ULL; /* -inf */
    case 3: return 0x8000000000000000ULL; /* -0.0 */
    case 4: return 0x0000000000000000ULL; /* +0.0 */
    case 5: return 0x7FEFFFFFFFFFFFFFULL; /* DBL_MAX */
    case 6: return 0x0000000000000001ULL; /* min subnormal */
    default: return raw;
    }
}

static u64 hash_v128(v128 v) { return v.u64x2[0] ^ (v.u64x2[1] * 0x9E3779B97F4A7C15ULL + 1ULL); }

#define DO_CMPSS(dst, src, pred) __asm__("cmpss %2, %1, %0" : "+x"(dst) : "x"(src), "i"(pred))
#define DO_CMPSD(dst, src, pred) __asm__("cmpsd %2, %1, %0" : "+x"(dst) : "x"(src), "i"(pred))
#define DO_CMPPS(dst, src, pred) __asm__("cmpps %2, %1, %0" : "+x"(dst) : "x"(src), "i"(pred))
#define DO_CMPPD(dst, src, pred) __asm__("cmppd %2, %1, %0" : "+x"(dst) : "x"(src), "i"(pred))
#define DO_ROUNDPD(dst, src, mode) __asm__("roundpd %2, %1, %0" : "=x"(dst) : "x"(src), "i"(mode))
#define DO_ROUNDSS(dst, src, mode) __asm__("roundss %2, %1, %0" : "+x"(dst) : "x"(src), "i"(mode))
#define DO_ROUNDSD(dst, src, mode) __asm__("roundsd %2, %1, %0" : "+x"(dst) : "x"(src), "i"(mode))

/* --- Scalar float / double math, comparisons, min/max -------------------------------------- */

EXPORT u64 f32_arith(u64 a, u64 b, u64 c, u64 d) {
    float x = f_of(pick_f32_bits(a, d));
    float y = f_of(pick_f32_bits(b, d >> 3));
    float z = f_of(pick_f32_bits(c, d >> 6));
    float sum = x + y;
    float diff = x - z;
    float prod = sum * (z + 1.5f);
    float quot = diff / (y == 0.0f ? 2.25f : y);
    v4f sv = (v4f){x, y, z, 1.0f};
    v4f sq = (v4f){1.0f, 2.0f, 3.0f, 4.0f};
    __asm__("sqrtss %1, %0" : "+x"(sq) : "x"(sv));
    v128 u;
    u.f = sq;
    return bits_f(sum) ^ (bits_f(diff) << 32) ^ bits_f(prod) ^ (bits_f(quot) << 16) ^ hash_v128(u);
}

EXPORT u64 f32_minmax(u64 a, u64 b, u64 c, u64 d) {
    float x = f_of(pick_f32_bits(a, d));
    float y = f_of(pick_f32_bits(b, d >> 3));
    float z = f_of(pick_f32_bits(c, d >> 6));
    v4f vx = (v4f){x, y, -0.0f, +0.0f};
    v4f vy = (v4f){y, x, +0.0f, -0.0f};
    v4f mn, mx;
    __asm__("minss %1, %0" : "=x"(mn) : "x"(vy), "0"(vx));
    __asm__("maxss %1, %0" : "=x"(mx) : "x"(vx), "0"(vy));
    float cmin = x < z ? x : z;
    float cmax = y > z ? y : z;
    v128 u1, u2;
    u1.f = mn;
    u2.f = mx;
    return hash_v128(u1) ^ (hash_v128(u2) * 3ULL) ^ bits_f(cmin) ^ (bits_f(cmax) << 32);
}

EXPORT u64 f32_cmp_flags(u64 a, u64 b, u64 c, u64 d) {
    float x = f_of(pick_f32_bits(a, d));
    float y = f_of(pick_f32_bits(b, d >> 3));
    float z = f_of(pick_f32_bits(c, d >> 6));
    u64 r = 0;
    r |= (u64)(x == y) << 0;
    r |= (u64)(x != y) << 1;
    r |= (u64)(x < y) << 2;
    r |= (u64)(x <= y) << 3;
    r |= (u64)(x > y) << 4;
    r |= (u64)(x >= y) << 5;
    r |= (u64)(x == z) << 6;
    r |= (u64)(z < y) << 7;
    u8 f_eq = 0, f_un = 0, f_lt = 0;
    __asm__("comiss %4, %3\n\tsete %0\n\tsetp %1\n\tsetb %2"
            : "=r"(f_eq), "=r"(f_un), "=r"(f_lt)
            : "x"(x), "x"(y)
            : "cc");
    r |= ((u64)f_eq << 16) | ((u64)f_un << 17) | ((u64)f_lt << 18);
    u8 u_eq = 0, u_un = 0, u_lt = 0;
    __asm__("ucomiss %4, %3\n\tsete %0\n\tsetp %1\n\tsetb %2"
            : "=r"(u_eq), "=r"(u_un), "=r"(u_lt)
            : "x"(x), "x"(z)
            : "cc");
    r |= ((u64)u_eq << 24) | ((u64)u_un << 25) | ((u64)u_lt << 26);
    return r;
}

EXPORT u64 f32_cmpss_all(u64 a, u64 b, u64 c, u64 d) {
    float x = f_of(pick_f32_bits(a, d));
    float y = f_of(pick_f32_bits(b, d >> 3));
    v4f src = (v4f){y, 1.0f, 2.0f, 3.0f};
    v4f r0 = (v4f){x, 11.0f, 22.0f, 33.0f};
    v4f r1 = r0, r2 = r0, r3 = r0, r4 = r0, r5 = r0, r6 = r0, r7 = r0;
    DO_CMPSS(r0, src, 0);
    DO_CMPSS(r1, src, 1);
    DO_CMPSS(r2, src, 2);
    DO_CMPSS(r3, src, 3);
    DO_CMPSS(r4, src, 4);
    DO_CMPSS(r5, src, 5);
    DO_CMPSS(r6, src, 6);
    DO_CMPSS(r7, src, 7);
    v128 u;
    u.f = r0;
    u64 acc = hash_v128(u);
    u.f = r1; acc = acc * 131ULL + hash_v128(u);
    u.f = r2; acc = acc * 131ULL + hash_v128(u);
    u.f = r3; acc = acc * 131ULL + hash_v128(u);
    u.f = r4; acc = acc * 131ULL + hash_v128(u);
    u.f = r5; acc = acc * 131ULL + hash_v128(u);
    u.f = r6; acc = acc * 131ULL + hash_v128(u);
    u.f = r7; acc = acc * 131ULL + hash_v128(u);
    return acc ^ c;
}

EXPORT u64 f64_arith(u64 a, u64 b, u64 c, u64 d) {
    double x = d_of(pick_f64_bits(a, d));
    double y = d_of(pick_f64_bits(b, d >> 3));
    double z = d_of(pick_f64_bits(c, d >> 6));
    double s = x + y;
    double df = x - z;
    double p = s * (z + 0.5);
    double q = df / (y == 0.0 ? 3.5 : y);
    v2d sv = (v2d){x, y};
    v2d sq = (v2d){5.0, 9.0};
    __asm__("sqrtsd %1, %0" : "+x"(sq) : "x"(sv));
    v128 u;
    u.d = sq;
    return bits_d(s) ^ bits_d(df) ^ bits_d(p) ^ bits_d(q) ^ hash_v128(u);
}

EXPORT u64 f64_minmax(u64 a, u64 b, u64 c, u64 d) {
    double x = d_of(pick_f64_bits(a, d));
    double y = d_of(pick_f64_bits(b, d >> 3));
    double z = d_of(pick_f64_bits(c, d >> 6));
    v2d vx = (v2d){x, -0.0};
    v2d vy = (v2d){y, +0.0};
    v2d mn, mx;
    __asm__("minsd %1, %0" : "=x"(mn) : "x"(vy), "0"(vx));
    __asm__("maxsd %1, %0" : "=x"(mx) : "x"(vx), "0"(vy));
    double cmin = x < z ? x : z;
    double cmax = y > z ? y : z;
    v128 u1, u2;
    u1.d = mn;
    u2.d = mx;
    return hash_v128(u1) ^ (hash_v128(u2) * 5ULL) ^ bits_d(cmin) ^ bits_d(cmax);
}

EXPORT u64 f64_cmp_flags(u64 a, u64 b, u64 c, u64 d) {
    double x = d_of(pick_f64_bits(a, d));
    double y = d_of(pick_f64_bits(b, d >> 3));
    double z = d_of(pick_f64_bits(c, d >> 6));
    u64 r = 0;
    r |= (u64)(x == y) << 0;
    r |= (u64)(x != y) << 1;
    r |= (u64)(x < y) << 2;
    r |= (u64)(x <= y) << 3;
    r |= (u64)(x > y) << 4;
    r |= (u64)(x >= y) << 5;
    u8 feq = 0, fun = 0, flt = 0;
    __asm__("comisd %4, %3\n\tsete %0\n\tsetp %1\n\tsetb %2"
            : "=r"(feq), "=r"(fun), "=r"(flt)
            : "x"(x), "x"(y)
            : "cc");
    r |= ((u64)feq << 8) | ((u64)fun << 9) | ((u64)flt << 10);
    u8 ueq = 0, uun = 0, ult = 0;
    __asm__("ucomisd %4, %3\n\tsete %0\n\tsetp %1\n\tsetb %2"
            : "=r"(ueq), "=r"(uun), "=r"(ult)
            : "x"(x), "x"(z)
            : "cc");
    r |= ((u64)ueq << 16) | ((u64)uun << 17) | ((u64)ult << 18);
    return r;
}

EXPORT u64 f64_cmpsd_all(u64 a, u64 b, u64 c, u64 d) {
    double x = d_of(pick_f64_bits(a, d));
    double y = d_of(pick_f64_bits(b, d >> 3));
    v2d src = (v2d){y, 42.0};
    v2d r0 = (v2d){x, 99.0};
    v2d r1 = r0, r2 = r0, r3 = r0, r4 = r0, r5 = r0, r6 = r0, r7 = r0;
    DO_CMPSD(r0, src, 0);
    DO_CMPSD(r1, src, 1);
    DO_CMPSD(r2, src, 2);
    DO_CMPSD(r3, src, 3);
    DO_CMPSD(r4, src, 4);
    DO_CMPSD(r5, src, 5);
    DO_CMPSD(r6, src, 6);
    DO_CMPSD(r7, src, 7);
    v128 u;
    u.d = r0; u64 acc = hash_v128(u);
    u.d = r1; acc = acc * 137ULL + hash_v128(u);
    u.d = r2; acc = acc * 137ULL + hash_v128(u);
    u.d = r3; acc = acc * 137ULL + hash_v128(u);
    u.d = r4; acc = acc * 137ULL + hash_v128(u);
    u.d = r5; acc = acc * 137ULL + hash_v128(u);
    u.d = r6; acc = acc * 137ULL + hash_v128(u);
    u.d = r7; acc = acc * 137ULL + hash_v128(u);
    return acc ^ c;
}

/* --- Conversions & MXCSR rounding ---------------------------------------------------------- */

EXPORT u64 cvt_scalar_i2f_f2f(u64 a, u64 b, u64 c, u64 d) {
    i32 s32 = (i32)a;
    i64 s64 = (i64)b;
    float f32 = (float)s32;
    float f64i = (float)s64;
    double d32 = (double)s32;
    double d64 = (double)s64;
    double from_f = (double)f_of(pick_f32_bits(c, d));
    float from_d = (float)d_of(pick_f64_bits(c ^ d, d >> 3));
    return bits_f(f32) ^ (bits_f(f64i) << 32) ^ bits_d(d32) ^ (bits_d(d64) * 3ULL) ^
           bits_d(from_f) ^ (bits_f(from_d) << 17);
}

EXPORT u64 cvt_scalar_f2i_edges(u64 a, u64 b, u64 c, u64 d) {
    static u32 const k_f32_edges[8] = {
        0xCF000000U, /* -2^31 (in i32 range) */
        0xCF000001U, /* -2^31 - 256 (out of i32 range) */
        0x4EFFFFFFU, /* 2^31 - 128 (in i32 range) */
        0x4F000000U, /* +2^31 (out of i32 range) */
        0xDF000000U, /* -2^63 (in i64 range) */
        0x5F000000U, /* +2^63 (out of i64 range) */
        0x7FC00000U, /* NaN */
        0xFF800000U  /* -inf */
    };
    static u64 const k_f64_edges[8] = {
        0xC1E0000000000000ULL, /* -2^31 */
        0xC1E0000000200000ULL, /* -2^31 - 1 */
        0x41DFFFFFFFC00000ULL, /* +2^31 - 1 */
        0x41E0000000000000ULL, /* +2^31 */
        0xC3E0000000000000ULL, /* -2^63 */
        0x43E0000000000000ULL, /* +2^63 */
        0x41DFFFFFFFE00000ULL, /* 2147483647.5 (cvttsd2si fits, cvtsd2si overflows) */
        0x7FF8000000000000ULL  /* NaN */
    };
    float f = (d & 1ULL) ? f_of(k_f32_edges[(d >> 1) & 7ULL]) : f_of((u32)a);
    double g = (d & 16ULL) ? d_of(k_f64_edges[(d >> 5) & 7ULL]) : d_of(b);
    v4f vf = (v4f){f, 0.0f, 0.0f, 0.0f};
    v2d vd = (v2d){g, 0.0};
    i32 t_f32 = 0, r_f32 = 0, t_f64 = 0, r_f64 = 0;
    i64 t_f64_64 = 0, r_f64_64 = 0, t_f32_64 = 0, r_f32_64 = 0;
    __asm__("cvttss2si %1, %0" : "=r"(t_f32) : "x"(vf));
    __asm__("cvtss2si %1, %0" : "=r"(r_f32) : "x"(vf));
    __asm__("cvttss2si %1, %0" : "=r"(t_f32_64) : "x"(vf));
    __asm__("cvtss2si %1, %0" : "=r"(r_f32_64) : "x"(vf));
    __asm__("cvttsd2si %1, %0" : "=r"(t_f64) : "x"(vd));
    __asm__("cvtsd2si %1, %0" : "=r"(r_f64) : "x"(vd));
    __asm__("cvttsd2si %1, %0" : "=r"(t_f64_64) : "x"(vd));
    r_f64_64 = __builtin_ia32_cvtsd2si64(vd);
    return (u64)(u32)t_f32 ^ ((u64)(u32)r_f32 << 16) ^ (u64)t_f32_64 ^
           ((u64)r_f32_64 * 3ULL) ^ ((u64)(u32)t_f64 << 32) ^ ((u64)(u32)r_f64 << 8) ^
           (u64)t_f64_64 ^ ((u64)r_f64_64 * 7ULL) ^ c;
}

EXPORT u64 cvt_packed_all(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a;
    ua.u64x2[1] = b;
    ub.u64x2[0] = pick_f64_bits(c, d);
    ub.u64x2[1] = pick_f64_bits(d, c);

    v4f dq2ps = __builtin_convertvector(ua.i, v4f);
    v2i lo_i = __builtin_shufflevector(ua.i, ua.i, 0, 1);
    v2d dq2pd = __builtin_convertvector(lo_i, v2d);
    v2f lo_f = __builtin_shufflevector(ua.f, ua.f, 0, 1);
    v2d ps2pd = __builtin_convertvector(lo_f, v2d);
    v2f pd2ps2 = __builtin_convertvector(ub.d, v2f);
    v4f pd2ps = __builtin_shufflevector(pd2ps2, (v2f){0.0f, 0.0f}, 0, 1, 2, 3);

    v4f fin = (v4f){
        f_of(pick_f32_bits(a, d)),
        f_of(pick_f32_bits(b, d >> 3)),
        2147483648.0f,
        -2147483904.0f
    };
    v4i ps2dq_r, ps2dq_t, pd2dq_r, pd2dq_t;
    __asm__("cvtps2dq %1, %0" : "=x"(ps2dq_r) : "x"(fin));
    __asm__("cvttps2dq %1, %0" : "=x"(ps2dq_t) : "x"(fin));
    v2d din = (v2d){d_of(pick_f64_bits(c, d)), 2147483647.75};
    __asm__("cvtpd2dq %1, %0" : "=x"(pd2dq_r) : "x"(din));
    __asm__("cvttpd2dq %1, %0" : "=x"(pd2dq_t) : "x"(din));

    v128 r1, r2, r3, r4, r5, r6, r7, r8;
    r1.f = dq2ps;
    r2.d = dq2pd;
    r3.d = ps2pd;
    r4.f = pd2ps;
    r5.i = ps2dq_r;
    r6.i = ps2dq_t;
    r7.i = pd2dq_r;
    r8.i = pd2dq_t;
    return hash_v128(r1) ^ (hash_v128(r2) * 3ULL) ^ (hash_v128(r3) * 5ULL) ^
           (hash_v128(r4) * 7ULL) ^ (hash_v128(r5) * 11ULL) ^ (hash_v128(r6) * 13ULL) ^
           (hash_v128(r7) * 17ULL) ^ (hash_v128(r8) * 19ULL);
}

EXPORT u64 mxcsr_round_cvtsd2si(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    double base = (double)((i32)(a & 0xFFFFU) - 0x8000);
    double frac = (b & 1ULL) ? 0.75 : 0.25;
    double x = (d & 1ULL) ? (base + frac) : -(base + frac);
    v2d vx = (v2d){x, 0.0};
    v2d vneg = (v2d){-x, 0.0};

    u32 saved = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(saved) : : "memory");
    u64 packed = 0;
    for (u32 rc = 0; rc < 4U; ++rc) {
        u32 mode = (saved & ~(3U << 13)) | (rc << 13);
        __asm__ volatile("ldmxcsr %0" : : "m"(mode) : "memory");
        v2d c1 = vx;
        v2d c2 = vneg;
        __asm__ volatile("" : "+x"(c1), "+x"(c2) : : "memory");
        i64 r1 = __builtin_ia32_cvtsd2si64(c1);
        i64 r2 = __builtin_ia32_cvtsd2si64(c2);
        __asm__ volatile("" : "+r"(r1), "+r"(r2) : : "memory");
        u16 slot = (u16)(((u64)r1 & 0xFFULL) | (((u64)r2 & 0xFFULL) << 8));
        packed |= (u64)slot << (16U * rc);
    }
    __asm__ volatile("ldmxcsr %0" : : "m"(saved) : "memory");
    return packed;
}

EXPORT u64 mxcsr_round_i2f_roundps(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    i64 big = (i64)((a | 0x0100000000000001ULL) & 0x7FFFFFFFFFFFFFFFULL);
    if (b & 1ULL) big = -big;
    v4f vf = (v4f){(float)((i32)a & 0xFF) + 0.5f, -2.5f, -1.25f, 3.75f};

    u32 saved = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(saved) : : "memory");
    u64 acc = 0;
    for (u32 rc = 0; rc < 4U; ++rc) {
        u32 mode = (saved & ~(3U << 13)) | (rc << 13);
        __asm__ volatile("ldmxcsr %0" : : "m"(mode) : "memory");
        i64 ci = big;
        v4f cv = vf;
        __asm__ volatile("" : "+r"(ci), "+x"(cv) : : "memory");
        double sd = (double)ci;
        float sf = (float)ci;
        v4f rp = __builtin_ia32_roundps(cv, 4); /* bit 2 set => MXCSR.RC */
        __asm__ volatile("" : "+x"(sd), "+x"(sf), "+x"(rp) : : "memory");
        v128 u;
        u.f = rp;
        acc = acc * 131ULL ^ bits_d(sd) ^ bits_f(sf) ^ hash_v128(u);
    }
    __asm__ volatile("ldmxcsr %0" : : "m"(saved) : "memory");
    return acc;
}

/* --- Packed float / double, shuffles, rounding --------------------------------------------- */

EXPORT u64 vec_f32_ops(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    v4f x = (v4f){f_of(pick_f32_bits(ua.u32x4[0], d)), ua.f[1], ua.f[2], -0.0f};
    v4f y = (v4f){f_of(pick_f32_bits(ub.u32x4[0], d >> 3)), ub.f[1], ub.f[2], +0.0f};

    v4f s = x + y;
    v4f df = x - y;
    v4f p = x * y;
    v4f q = x / (v4f){y[0] == 0.0f ? 2.0f : y[0], 1.5f, -2.5f, 4.0f};
    v4f sq;
    __asm__("sqrtps %1, %0" : "=x"(sq) : "x"(x));

    v4f anp = x, mn, mx;
    __asm__("andnps %1, %0" : "+x"(anp) : "x"(y));
    __asm__("minps %1, %0" : "=x"(mn) : "x"(y), "0"(x));
    __asm__("maxps %1, %0" : "=x"(mx) : "x"(x), "0"(y));

    v4f c0 = x, c3 = x, c4 = x, c6 = x;
    DO_CMPPS(c0, y, 0);
    DO_CMPPS(c3, y, 3);
    DO_CMPPS(c4, y, 4);
    DO_CMPPS(c6, y, 6);

    v128 r;
    u64 h = 0;
    r.f = s + df; h ^= hash_v128(r);
    r.f = p + q; h ^= hash_v128(r) * 3ULL;
    r.f = sq; h ^= hash_v128(r) * 5ULL;
    r.f = anp; h ^= hash_v128(r) * 7ULL;
    r.f = mn; h ^= hash_v128(r) * 11ULL;
    r.f = mx; h ^= hash_v128(r) * 13ULL;
    r.f = c0; h ^= hash_v128(r) * 17ULL;
    r.f = c3; h ^= hash_v128(r) * 19ULL;
    r.f = c4; h ^= hash_v128(r) * 23ULL;
    r.f = c6; h ^= hash_v128(r) * 29ULL;
    return h;
}

EXPORT u64 vec_f64_ops(u64 a, u64 b, u64 c, u64 d) {
    v2d x = (v2d){d_of(pick_f64_bits(a, d)), -0.0};
    v2d y = (v2d){d_of(pick_f64_bits(b, d >> 3)), +0.0};
    v2d z = (v2d){d_of(c), 1.5};
    v2d s = x + y;
    v2d df = x - z;
    v2d p = s * z;
    v2d q = df / (v2d){y[0] == 0.0 ? 3.0 : y[0], 2.0};
    v2d sq;
    __asm__("sqrtpd %1, %0" : "=x"(sq) : "x"(x));

    v2d anp = x, mn, mx;
    __asm__("andnpd %1, %0" : "+x"(anp) : "x"(y));
    __asm__("minpd %1, %0" : "=x"(mn) : "x"(y), "0"(x));
    __asm__("maxpd %1, %0" : "=x"(mx) : "x"(x), "0"(y));

    v2d c1 = x, c2 = x, c5 = x, c7 = x;
    DO_CMPPD(c1, y, 1);
    DO_CMPPD(c2, y, 2);
    DO_CMPPD(c5, y, 5);
    DO_CMPPD(c7, y, 7);

    v128 r;
    u64 h = 0;
    r.d = s + p; h ^= hash_v128(r);
    r.d = q; h ^= hash_v128(r) * 3ULL;
    r.d = sq; h ^= hash_v128(r) * 5ULL;
    r.d = anp; h ^= hash_v128(r) * 7ULL;
    r.d = mn; h ^= hash_v128(r) * 11ULL;
    r.d = mx; h ^= hash_v128(r) * 13ULL;
    r.d = c1; h ^= hash_v128(r) * 17ULL;
    r.d = c2; h ^= hash_v128(r) * 19ULL;
    r.d = c5; h ^= hash_v128(r) * 23ULL;
    r.d = c7; h ^= hash_v128(r) * 29ULL;
    return h;
}

EXPORT u64 vec_round_modes(u64 a, u64 b, u64 c, u64 d) {
    v4f vf = (v4f){f_of(pick_f32_bits(a, d)), -2.5f, 2.5f, -0.25f};
    v2d vd = (v2d){d_of(pick_f64_bits(b, d >> 3)), -3.5};
    v4f p0 = __builtin_ia32_roundps(vf, 0);
    v4f p1 = __builtin_ia32_roundps(vf, 1);
    v4f p2 = __builtin_ia32_roundps(vf, 2);
    v4f p3 = __builtin_ia32_roundps(vf, 3);
    v2d d0, d1, d2, d3;
    DO_ROUNDPD(d0, vd, 0);
    DO_ROUNDPD(d1, vd, 1);
    DO_ROUNDPD(d2, vd, 2);
    DO_ROUNDPD(d3, vd, 3);
    v4f ss = (v4f){f_of((u32)c), 10.0f, 20.0f, 30.0f};
    v2d sd = (v2d){d_of(c ^ d), 99.0};
    DO_ROUNDSS(ss, vf, 1);
    DO_ROUNDSD(sd, vd, 2);
    v128 r;
    u64 h = 0;
    r.f = p0; h ^= hash_v128(r);
    r.f = p1; h ^= hash_v128(r) * 3ULL;
    r.f = p2; h ^= hash_v128(r) * 5ULL;
    r.f = p3; h ^= hash_v128(r) * 7ULL;
    r.d = d0; h ^= hash_v128(r) * 11ULL;
    r.d = d1; h ^= hash_v128(r) * 13ULL;
    r.d = d2; h ^= hash_v128(r) * 17ULL;
    r.d = d3; h ^= hash_v128(r) * 19ULL;
    r.f = ss; h ^= hash_v128(r) * 23ULL;
    r.d = sd; h ^= hash_v128(r) * 29ULL;
    return h;
}

EXPORT u64 vec_shuf_unpck(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    v4f sf = __builtin_shufflevector(ua.f, ub.f, 2, 0, 7, 5);
    v2d sd = __builtin_shufflevector(ua.d, ub.d, 1, 2);
    v4f ulf = __builtin_shufflevector(ua.f, ub.f, 0, 4, 1, 5);
    v4f uhf = __builtin_shufflevector(ua.f, ub.f, 2, 6, 3, 7);
    v2d uld = __builtin_shufflevector(ua.d, ub.d, 0, 2);
    v2d uhd = __builtin_shufflevector(ua.d, ub.d, 1, 3);
    v4i pd = __builtin_shufflevector(ua.i, ua.i, 3, 1, 2, 0);
    v8s plw = __builtin_shufflevector(ua.s, ua.s, 2, 0, 3, 1, 4, 5, 6, 7);
    v8s phw = __builtin_shufflevector(ub.s, ub.s, 0, 1, 2, 3, 7, 5, 6, 4);
    v16c ulb = __builtin_shufflevector(ua.c, ub.c, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
    v16c uhb = __builtin_shufflevector(ua.c, ub.c, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);
    v4f hl = sf, lh = ulf;
    __asm__("movhlps %1, %0" : "+x"(hl) : "x"(uhf));
    __asm__("movlhps %1, %0" : "+x"(lh) : "x"(sf));

    v128 r;
    u64 h = 0;
    r.f = sf; h ^= hash_v128(r);
    r.d = sd; h ^= hash_v128(r) * 3ULL;
    r.f = ulf; h ^= hash_v128(r) * 5ULL;
    r.f = uhf; h ^= hash_v128(r) * 7ULL;
    r.d = uld; h ^= hash_v128(r) * 11ULL;
    r.d = uhd; h ^= hash_v128(r) * 13ULL;
    r.i = pd; h ^= hash_v128(r) * 17ULL;
    r.s = plw; h ^= hash_v128(r) * 19ULL;
    r.s = phw; h ^= hash_v128(r) * 23ULL;
    r.c = ulb; h ^= hash_v128(r) * 29ULL;
    r.c = uhb; h ^= hash_v128(r) * 31ULL;
    r.f = hl; h ^= hash_v128(r) * 37ULL;
    r.f = lh; h ^= hash_v128(r) * 41ULL;
    return h;
}

EXPORT u64 vec_pshufb_palignr(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    /* Set bit 7 in several control bytes to exercise zeroing. */
    ub.u8x16[2] |= 0x80U;
    ub.u8x16[7] |= 0x80U;
    ub.u8x16[13] |= 0x80U;
    v16c shuf = __builtin_ia32_pshufb128(ua.c, ub.c);
    v16c al5 = ua.c, al16 = ua.c, al21 = ua.c;
    __asm__("palignr %2, %1, %0" : "+x"(al5) : "x"(ub.c), "i"(5));
    __asm__("palignr %2, %1, %0" : "+x"(al16) : "x"(ub.c), "i"(16));
    __asm__("palignr %2, %1, %0" : "+x"(al21) : "x"(ub.c), "i"(21));
    v128 r;
    r.c = shuf;
    u64 h = hash_v128(r);
    r.c = al5; h ^= hash_v128(r) * 3ULL;
    r.c = al16; h ^= hash_v128(r) * 5ULL;
    r.c = al21; h ^= hash_v128(r) * 7ULL;
    return h;
}

EXPORT u64 vec_ptest_masks(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    if (d & 1ULL) ub.u64x2[0] &= a;
    if (d & 2ULL) ub.u64x2[1] = 0;
    int pz = __builtin_ia32_ptestz128(ua.q, ub.q);
    u8 pc = 0, pnz = 0;
    __asm__("ptest %3, %2\n\tsetc %0\n\tsetnz %1"
            : "=r"(pc), "=r"(pnz)
            : "x"(ua.q), "x"(ub.q)
            : "cc");
    u32 mps = 0, mpd = 0, mpb = 0;
    __asm__("movmskps %1, %0" : "=r"(mps) : "x"(ua.f));
    __asm__("movmskpd %1, %0" : "=r"(mpd) : "x"(ua.d));
    __asm__("pmovmskb %1, %0" : "=r"(mpb) : "x"(ub.c));
    return (u64)(pz != 0) | ((u64)(pc & 1U) << 1) | ((u64)(pnz & 1U) << 2) |
           ((u64)mps << 8) | ((u64)mpd << 16) | ((u64)mpb << 24);
}

EXPORT u64 vec_pmaddwd_psadbw(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    ua.i16x8[0] = -32768;
    ub.i16x8[0] = -32768;
    ua.i16x8[1] = -32768;
    ub.i16x8[1] = -32768;
    v4i madd = __builtin_ia32_pmaddwd128(ua.s, ub.s);
    v2ll sad = __builtin_ia32_psadbw128(ua.c, ub.c);
    v128 r1, r2;
    r1.i = madd;
    r2.q = sad;
    return hash_v128(r1) ^ (hash_v128(r2) * 13ULL);
}

EXPORT u64 vec_int_arith(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    v16c zb = (v16c){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    v8s zw = (v8s){0, 0, 0, 0, 0, 0, 0, 0};
    v4i zd = (v4i){0, 0, 0, 0};

    v16c ab = ua.c + ub.c;
    v16c sb = ua.c - ub.c;
    v8s aw = ua.s + ub.s;
    v8s sw = ua.s - ub.s;
    v4i ad = ua.i + ub.i;
    v4i sd = ua.i - ub.i;
    v2ll aq = ua.q + ub.q;
    v2ll sq = ua.q - ub.q;
    v8s mw = ua.s * ub.s;
    v4i md = ua.i * ub.i;

    v16c negb = (ua.c < zb);
    v16c absb = ((-ua.c) & negb) | (ua.c & ~negb);
    v8s negw = (ua.s < zw);
    v8s absw = ((-ua.s) & negw) | (ua.s & ~negw);
    v4i negd = (ua.i < zd);
    v4i absd = ((-ua.i) & negd) | (ua.i & ~negd);

    v16c cmpb = (ua.c == ub.c) ^ (ua.c > ub.c);
    v8s cmpw = (ua.s == ub.s) ^ (ua.s > ub.s);
    v4i cmpd = (ua.i == ub.i) ^ (ua.i > ub.i);
    v2ll cmpq = (ua.q == ub.q);

    v16c mnub = ua.c, mxub = ua.c;
    v8s mnsw = ua.s, mxsw = ua.s;
    v4i mnsd = ua.i, mxsd = ua.i, mnud = ua.i, mxud = ua.i;
    __asm__("pminub %1, %0" : "+x"(mnub) : "x"(ub.c));
    __asm__("pmaxub %1, %0" : "+x"(mxub) : "x"(ub.c));
    __asm__("pminsw %1, %0" : "+x"(mnsw) : "x"(ub.s));
    __asm__("pmaxsw %1, %0" : "+x"(mxsw) : "x"(ub.s));
    __asm__("pminsd %1, %0" : "+x"(mnsd) : "x"(ub.i));
    __asm__("pmaxsd %1, %0" : "+x"(mxsd) : "x"(ub.i));
    __asm__("pminud %1, %0" : "+x"(mnud) : "x"(ub.i));
    __asm__("pmaxud %1, %0" : "+x"(mxud) : "x"(ub.i));

    v128 r;
    u64 h = 0;
    r.c = ab ^ sb ^ absb ^ cmpb; h ^= hash_v128(r);
    r.s = aw ^ sw ^ mw ^ absw ^ cmpw; h ^= hash_v128(r) * 3ULL;
    r.i = ad ^ sd ^ md ^ absd ^ cmpd; h ^= hash_v128(r) * 5ULL;
    r.q = aq ^ sq ^ cmpq; h ^= hash_v128(r) * 7ULL;
    r.c = mnub + mxub; h ^= hash_v128(r) * 11ULL;
    r.s = mnsw + mxsw; h ^= hash_v128(r) * 13ULL;
    r.i = mnsd ^ mxsd ^ mnud ^ mxud; h ^= hash_v128(r) * 17ULL;
    return h;
}

EXPORT u64 vec_sat_mul_pack(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    v16c psb = ua.c, psub = ua.c, ssb = ua.c, ssub = ua.c;
    __asm__("paddsb %1, %0" : "+x"(psb) : "x"(ub.c));
    __asm__("paddusb %1, %0" : "+x"(psub) : "x"(ub.c));
    __asm__("psubsb %1, %0" : "+x"(ssb) : "x"(ub.c));
    __asm__("psubusb %1, %0" : "+x"(ssub) : "x"(ub.c));
    v8s psw = ua.s, psuw = ua.s, ssw = ua.s, ssuw = ua.s;
    __asm__("paddsw %1, %0" : "+x"(psw) : "x"(ub.s));
    __asm__("paddusw %1, %0" : "+x"(psuw) : "x"(ub.s));
    __asm__("psubsw %1, %0" : "+x"(ssw) : "x"(ub.s));
    __asm__("psubusw %1, %0" : "+x"(ssuw) : "x"(ub.s));

    v8s mhw = ua.s, mhuw = ua.s;
    v4i mudq = ua.i;
    __asm__("pmulhw %1, %0" : "+x"(mhw) : "x"(ub.s));
    __asm__("pmulhuw %1, %0" : "+x"(mhuw) : "x"(ub.s));
    __asm__("pmuludq %1, %0" : "+x"(mudq) : "x"(ub.i));

    v8s pk_sswb = ua.s, pk_uswb = ua.s;
    v4i pk_ssdw = ua.i, pk_usdw = ua.i;
    __asm__("packsswb %1, %0" : "+x"(pk_sswb) : "x"(ub.s));
    __asm__("packuswb %1, %0" : "+x"(pk_uswb) : "x"(ub.s));
    __asm__("packssdw %1, %0" : "+x"(pk_ssdw) : "x"(ub.i));
    __asm__("packusdw %1, %0" : "+x"(pk_usdw) : "x"(ub.i));

    v16c avgb = ua.c;
    v8s avgw = ua.s;
    __asm__("pavgb %1, %0" : "+x"(avgb) : "x"(ub.c));
    __asm__("pavgw %1, %0" : "+x"(avgw) : "x"(ub.s));

    v2ll pn = ua.q;
    __asm__("pandn %1, %0" : "+x"(pn) : "x"(ub.q));

    v128 r;
    u64 h = 0;
    r.c = psb ^ psub ^ ssb ^ ssub ^ avgb; h ^= hash_v128(r);
    r.s = psw ^ psuw ^ ssw ^ ssuw ^ mhw ^ mhuw ^ avgw; h ^= hash_v128(r) * 3ULL;
    r.i = mudq ^ pk_ssdw ^ pk_usdw; h ^= hash_v128(r) * 5ULL;
    r.s = pk_sswb ^ pk_uswb; h ^= hash_v128(r) * 7ULL;
    r.q = pn; h ^= hash_v128(r) * 11ULL;
    return h;
}

EXPORT u64 vec_shifts(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    /* Include both small counts and counts >= lane width (and > 255 in 64-bit count). */
    u64 cnt_val = (d & 1ULL) ? (d & 127ULL) : (256ULL + (c & 15ULL));
    v2ll vcnt = (v2ll){(i64)cnt_val, 0};

    v8s lw = ua.s, rw = ua.s, aw = ua.s;
    v4i ld = ua.i, rd = ua.i, ad = ua.i;
    v2ll lq = ua.q, rq = ua.q;
    __asm__("psllw %1, %0" : "+x"(lw) : "x"(vcnt));
    __asm__("psrlw %1, %0" : "+x"(rw) : "x"(vcnt));
    __asm__("psraw %1, %0" : "+x"(aw) : "x"(vcnt));
    __asm__("pslld %1, %0" : "+x"(ld) : "x"(vcnt));
    __asm__("psrld %1, %0" : "+x"(rd) : "x"(vcnt));
    __asm__("psrad %1, %0" : "+x"(ad) : "x"(vcnt));
    __asm__("psllq %1, %0" : "+x"(lq) : "x"(vcnt));
    __asm__("psrlq %1, %0" : "+x"(rq) : "x"(vcnt));

    v8s ilw = ub.s;
    v4i iad = ub.i;
    v2ll bsl = ua.q, bsr = ub.q, bsl_oob = ua.q;
    __asm__("psllw %1, %0" : "+x"(ilw) : "i"(19));
    __asm__("psrad %1, %0" : "+x"(iad) : "i"(40));
    __asm__("pslldq %1, %0" : "+x"(bsl) : "i"(5));
    __asm__("psrldq %1, %0" : "+x"(bsr) : "i"(7));
    __asm__("pslldq %1, %0" : "+x"(bsl_oob) : "i"(20));

    v128 r;
    u64 h = 0;
    r.s = lw ^ rw ^ aw ^ ilw; h ^= hash_v128(r);
    r.i = ld ^ rd ^ ad ^ iad; h ^= hash_v128(r) * 3ULL;
    r.q = lq ^ rq ^ bsl ^ bsr ^ bsl_oob; h ^= hash_v128(r) * 5ULL;
    return h;
}

EXPORT u64 vec_blend_ext_ins(u64 a, u64 b, u64 c, u64 d) {
    v128 ua, ub, um;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;
    um.u64x2[0] = a ^ c; um.u64x2[1] = b ^ d;

    /* Variable-mask blends (compiler lowers to blendvps / blendvpd / pblendvb with -msse4.1). */
    v4i m32 = um.i < (v4i){0, 0, 0, 0};
    v4i bvps = (ub.i & m32) | (ua.i & ~m32);
    v2ll m64 = um.q < (v2ll){0, 0};
    v2ll bvpd = (ub.q & m64) | (ua.q & ~m64);
    v16c m8 = um.c < (v16c){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    v16c bpvb = (ub.c & m8) | (ua.c & ~m8);

    /* Constant-mask blends (blendps / blendpd / pblendw). */
    v4f bps = __builtin_shufflevector(ua.f, ub.f, 0, 5, 2, 7);
    v2d bpd = __builtin_shufflevector(ua.d, ub.d, 2, 1);
    v8s bpw = __builtin_shufflevector(ua.s, ub.s, 0, 9, 2, 3, 12, 5, 14, 7);

    /* insertps / extractps / pextr* / pinsr*. */
    v4f ips = ua.f;
    __asm__("insertps %2, %1, %0" : "+x"(ips) : "x"(ub.f), "i"(0x9A));
    u32 eps = 0, exb = 0, exw = 0, exd = 0;
    u64 exq = 0;
    __asm__("extractps %2, %1, %0" : "=r"(eps) : "x"(ips), "i"(2));
    __asm__("pextrb %2, %1, %0" : "=r"(exb) : "x"(bpvb), "i"(11));
    __asm__("pextrw %2, %1, %0" : "=r"(exw) : "x"(bpw), "i"(5));
    __asm__("pextrd %2, %1, %0" : "=r"(exd) : "x"(bvps), "i"(3));
    __asm__("pextrq %2, %1, %0" : "=r"(exq) : "x"(bvpd), "i"(1));

    v16c pin = ua.c;
    __asm__("pinsrb %2, %1, %0" : "+x"(pin) : "r"((u32)d), "i"(9));
    __asm__("pinsrw %2, %1, %0" : "+x"(pin) : "r"((u32)c), "i"(2));
    __asm__("pinsrd %2, %1, %0" : "+x"(pin) : "r"((u32)b), "i"(3));
    __asm__("pinsrq %2, %1, %0" : "+x"(pin) : "r"(a ^ b), "i"(0));

    v128 r;
    u64 h = (u64)eps ^ ((u64)exb << 8) ^ ((u64)exw << 16) ^ ((u64)exd << 32) ^ exq;
    r.i = bvps; h ^= hash_v128(r);
    r.q = bvpd; h ^= hash_v128(r) * 3ULL;
    r.c = bpvb ^ pin; h ^= hash_v128(r) * 5ULL;
    r.f = bps; h ^= hash_v128(r) * 7ULL;
    r.d = bpd; h ^= hash_v128(r) * 11ULL;
    r.s = bpw; h ^= hash_v128(r) * 13ULL;
    r.f = ips; h ^= hash_v128(r) * 17ULL;
    return h;
}

EXPORT u64 vec_pmov_extend(u64 a, u64 b, u64 c, u64 d) {
    typedef signed char v8c __attribute__((vector_size(8)));
    typedef unsigned char v8uc __attribute__((vector_size(8)));
    typedef signed char v4c __attribute__((vector_size(4)));
    typedef unsigned char v4uc __attribute__((vector_size(4)));
    typedef signed char v2c __attribute__((vector_size(2)));
    typedef unsigned char v2uc __attribute__((vector_size(2)));
    typedef short v4s __attribute__((vector_size(8)));
    typedef unsigned short v4us __attribute__((vector_size(8)));
    typedef short v2s __attribute__((vector_size(4)));
    typedef unsigned short v2us __attribute__((vector_size(4)));
    typedef int v2si __attribute__((vector_size(8)));
    typedef unsigned int v2ui __attribute__((vector_size(8)));

    v128 ua, ub;
    ua.u64x2[0] = a; ua.u64x2[1] = b;
    ub.u64x2[0] = c; ub.u64x2[1] = d;

    v8s sxbw = __builtin_convertvector((v8c){ua.c[0], ua.c[1], ua.c[2], ua.c[3], ua.c[4], ua.c[5], ua.c[6], ua.c[7]}, v8s);
    v8s zxbw = __builtin_convertvector((v8uc){ua.uc[0], ua.uc[1], ua.uc[2], ua.uc[3], ua.uc[4], ua.uc[5], ua.uc[6], ua.uc[7]}, v8s);
    v4i sxbd = __builtin_convertvector((v4c){ua.c[8], ua.c[9], ua.c[10], ua.c[11]}, v4i);
    v4i zxbd = __builtin_convertvector((v4uc){ua.uc[8], ua.uc[9], ua.uc[10], ua.uc[11]}, v4i);
    v2ll sxbq = __builtin_convertvector((v2c){ua.c[12], ua.c[13]}, v2ll);
    v2ll zxbq = __builtin_convertvector((v2uc){ua.uc[12], ua.uc[13]}, v2ll);
    v4i sxwd = __builtin_convertvector((v4s){ub.s[0], ub.s[1], ub.s[2], ub.s[3]}, v4i);
    v4i zxwd = __builtin_convertvector((v4us){ub.us[0], ub.us[1], ub.us[2], ub.us[3]}, v4i);
    v2ll sxwq = __builtin_convertvector((v2s){ub.s[4], ub.s[5]}, v2ll);
    v2ll zxwq = __builtin_convertvector((v2us){ub.us[4], ub.us[5]}, v2ll);
    v2ll sxdq = __builtin_convertvector((v2si){ub.i[2], ub.i[3]}, v2ll);
    v2ll zxdq = __builtin_convertvector((v2ui){ub.u[2], ub.u[3]}, v2ll);

    v128 r;
    u64 h = 0;
    r.s = sxbw ^ zxbw; h ^= hash_v128(r);
    r.i = sxbd ^ zxbd ^ sxwd ^ zxwd; h ^= hash_v128(r) * 3ULL;
    r.q = sxbq ^ zxbq ^ sxwq ^ zxwq ^ sxdq ^ zxdq; h ^= hash_v128(r) * 5ULL;
    return h;
}

/* --- Auto-vectorized loops over the 4096-byte buffer `a` (`m_*`) --------------------------- */

EXPORT u64 m_sum_floats(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const u32* p = (const u32*)a;
    u64 n = (b & 1023ULL);
    float sum = 0.0f;
    for (u64 i = 0; i < n; ++i) {
        /* Clear the exponent's top bit so every input is finite: random bits are mostly NaN,
         * and with two NaN inputs x86 and LLVM may propagate different payloads (the
         * documented R1 deviation). */
        sum += f_of(p[i] & 0xBFFFFFFFU);
    }
    return sum != sum ? 0x7FC00000ULL : bits_f(sum);
}

EXPORT u64 m_saxpy_f32(u64 a, u64 b, u64 c, u64 d) {
    /* Finite inputs only (see m_sum_floats): two NaN operands may differ in payload. */
    u32* xb = (u32*)a;
    float* y = (float*)a + 512;
    u32* yb = (u32*)a + 512;
    float alpha = f_of((u32)b & 0xBFFFFFFFU);
    float beta = f_of((u32)c & 0xBFFFFFFFU);
    u64 n = (d & 511ULL);
    for (u64 i = 0; i < n; ++i) {
        y[i] = alpha * f_of(xb[i] & 0xBFFFFFFFU) + beta * f_of(yb[i] & 0xBFFFFFFFU);
    }
    return n ? bits_f(y[n - 1ULL]) : 0ULL;
}

EXPORT u64 m_saxpy_f64(u64 a, u64 b, u64 c, u64 d) {
    /* Finite inputs only (see m_sum_floats): two NaN operands may differ in payload. */
    u64* xb = (u64*)a;
    double* y = (double*)a + 256;
    u64* yb = (u64*)a + 256;
    double alpha = d_of(b & 0xBFFFFFFFFFFFFFFFULL);
    u64 n = (c & 255ULL);
    for (u64 i = 0; i < n; ++i) {
        y[i] = alpha * d_of(xb[i] & 0xBFFFFFFFFFFFFFFFULL) + d_of(yb[i] & 0xBFFFFFFFFFFFFFFFULL);
    }
    return (n + (d & 1ULL)) ? bits_d(y[n ? n - 1ULL : 0ULL]) : 0ULL;
}

EXPORT u64 m_dot_product_f32(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const u32* x = (const u32*)a;
    const u32* y = x + 512;
    u64 n = b & 511ULL;
    float acc = 0.0f;
    for (u64 i = 0; i < n; ++i) {
        /* Finite inputs only (see m_sum_floats): two NaN operands may differ in payload. */
        acc += f_of(x[i] & 0xBFFFFFFFU) * f_of(y[i] & 0xBFFFFFFFU);
    }
    return acc != acc ? 0x7FC00000ULL : bits_f(acc);
}

EXPORT u64 m_clamp_01_f32(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    float* p = (float*)a;
    u64 n = b & 1023ULL;
    float total = 0.0f;
    for (u64 i = 0; i < n; ++i) {
        float x = p[i];
        /* Evaluates to 0.0f on NaN without trapping or undefined C conversions. */
        float y = (x > 0.0f) ? x : 0.0f;
        y = (y < 1.0f) ? y : 1.0f;
        p[i] = y;
        total += y;
    }
    return bits_f(total);
}

EXPORT u64 m_pack_floats_to_bytes(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const float* src = (const float*)a;
    u8* dst = (u8*)a + 2048;
    u64 n = b & 511ULL;
    u64 checksum = 0;
    for (u64 i = 0; i < n; ++i) {
        float x = src[i];
        float y = (x > 0.0f) ? x : 0.0f;
        y = (y < 1.0f) ? y : 1.0f;
        u8 q = (u8)(i32)(y * 255.0f);
        dst[i] = q;
        checksum += q;
    }
    return checksum;
}

EXPORT u64 m_i32_minmax(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const i32* p = (const i32*)a;
    const u32* up = (const u32*)a;
    u64 n = (b & 1023ULL);
    if (n == 0) n = 1;
    i32 smin = p[0], smax = p[0];
    u32 umin = up[0], umax = up[0];
    for (u64 i = 1; i < n; ++i) {
        if (p[i] < smin) smin = p[i];
        if (p[i] > smax) smax = p[i];
        if (up[i] < umin) umin = up[i];
        if (up[i] > umax) umax = up[i];
    }
    return (u64)(u32)smin ^ ((u64)(u32)smax << 32) ^ ((u64)umin << 16) ^ ((u64)umax << 48);
}

EXPORT u64 m_u8_histogram(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    const u8* p = (const u8*)a;
    u32* hist = (u32*)((u8*)a + 2048); /* 256 * 4 = 1024 bytes inside [2048, 3072) */
    u64 n = b & 2047ULL;
    for (u64 i = 0; i < n; ++i) {
        hist[p[i]] += 1U;
    }
    u64 acc = 0;
    for (u64 k = 0; k < 256ULL; ++k) {
        acc = acc * 33ULL + hist[k];
    }
    return acc;
}

EXPORT u64 m_u16_sat_filter(u64 a, u64 b, u64 c, u64 d) {
    u8* dst = (u8*)a;
    const u8* src = (const u8*)a + 2048;
    u64 n = b & 2047ULL;
    u8 bias = (u8)c;
    u8 limit = (u8)d;
    u64 sum = 0;
    for (u64 i = 0; i < n; ++i) {
        u16 x = (u16)src[i] + bias;
        u8 clamped = x > 255U ? 255U : (u8)x;
        u8 sub = clamped > limit ? (u8)(clamped - limit) : 0U;
        dst[i] = sub;
        sum += sub;
    }
    return sum;
}

EXPORT u64 m_f32_sqrt_norm(u64 a, u64 b, u64 c, u64 d) {
    (void)c;
    (void)d;
    float* p = (float*)a;
    u64 count = (b & 255ULL);
    float acc = 0.0f;
    for (u64 i = 0; i < count; ++i) {
        float x = p[4ULL * i + 0ULL];
        float y = p[4ULL * i + 1ULL];
        float z = p[4ULL * i + 2ULL];
        float x0 = (x > -1e10f && x < 1e10f) ? x : 1.0f;
        float y0 = (y > -1e10f && y < 1e10f) ? y : 1.0f;
        float z0 = (z > -1e10f && z < 1e10f) ? z : 1.0f;
        float len2 = x0 * x0 + y0 * y0 + z0 * z0 + 1.0f;
        v4f lv = (v4f){len2, 0.0f, 0.0f, 0.0f};
        __asm__("sqrtss %1, %0" : "+x"(lv) : "x"(lv));
        float inv = 1.0f / lv[0];
        p[4ULL * i + 0ULL] = x0 * inv;
        p[4ULL * i + 1ULL] = y0 * inv;
        p[4ULL * i + 2ULL] = z0 * inv;
        p[4ULL * i + 3ULL] = lv[0];
        acc += lv[0];
    }
    return bits_f(acc);
}

EXPORT u64 m_vec_mem_moves(u64 a, u64 b, u64 c, u64 d) {
    typedef float v4f_u __attribute__((vector_size(16), aligned(1)));
    typedef long long v2ll_u __attribute__((vector_size(16), aligned(1)));
    typedef double v2d_u __attribute__((vector_size(16), aligned(1)));
    u8* base = (u8*)a;
    /* Keep aligned slot inside [0..1024) and unaligned slot inside [1024..2048). */
    u8* aligned_dst = base + ((c & 63ULL) * 16ULL);
    u8* unaligned_src = base + 1024ULL + (d & 511ULL);
    u8* half_slot = base + 2048ULL + (b & 511ULL);

    v4f vf = *(const v4f_u*)unaligned_src;
    v2ll vq = *(const v2ll_u*)(unaligned_src + 16);
    v2d vd = *(const v2d_u*)(unaligned_src + 32);

    __asm__("movlps %1, %0" : "+x"(vf) : "m"(*(const u64*)half_slot));
    __asm__("movhps %1, %0" : "+x"(vf) : "m"(*(const u64*)(half_slot + 8)));
    __asm__("movlpd %1, %0" : "+x"(vd) : "m"(*(const u64*)(half_slot + 16)));
    __asm__("movhpd %1, %0" : "+x"(vd) : "m"(*(const u64*)(half_slot + 24)));

    __asm__("movlps %1, %0" : "=m"(*(u64*)(half_slot + 32)) : "x"(vf));
    __asm__("movhps %1, %0" : "=m"(*(u64*)(half_slot + 40)) : "x"(vf));
    __asm__("movlpd %1, %0" : "=m"(*(u64*)(half_slot + 48)) : "x"(vd));
    __asm__("movhpd %1, %0" : "=m"(*(u64*)(half_slot + 56)) : "x"(vd));

    __asm__("movntps %1, %0" : "=m"(*(v4f*)aligned_dst) : "x"(vf));
    __asm__("movntdq %1, %0" : "=m"(*(v2ll*)(aligned_dst + 16)) : "x"(vq));
    __asm__("movnti %1, %0" : "=m"(*(u32*)(half_slot + 64)) : "r"((u32)b));
    __asm__("movnti %1, %0" : "=m"(*(u64*)(half_slot + 72)) : "r"(b ^ c ^ d));

    v128 r1, r2;
    r1.f = vf;
    r2.d = vd;
    return hash_v128(r1) ^ hash_v128(r2);
}

static NOINLINE u64 sse_leaf_step(u64 x, u64 y) {
    v4f vx = (v4f){(float)(x & 255U), (float)(y & 255U), 1.5f, -2.5f};
    v4f vy = (v4f){2.0f, 3.0f, 4.0f, 5.0f};
    v128 u;
    u.f = vx * vy + vx;
    return hash_v128(u);
}

EXPORT u64 m_callee_saved_xmm(u64 a, u64 b, u64 c, u64 d) {
    const v4f* p = (const v4f*)a;
    v4f a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
    v4f a4 = p[4], a5 = p[5], a6 = p[6], a7 = p[7];
    v4f a8 = p[8], a9 = p[9], a10 = p[10], a11 = p[11];
    u64 k = sse_leaf_step(b, c);
    u64 n = (d & 7ULL) + 1ULL;
    for (u64 i = 0; i < n; ++i) {
        v4f t = p[(i * 12ULL) & 255ULL];
        a0 += t; a1 -= t; a2 *= (v4f){1.0f, 1.0f, 1.0f, 1.0f}; a3 += a0;
        a4 += a1; a5 -= a2; a6 += a3; a7 -= a4;
        a8 += a5; a9 -= a6; a10 += a7; a11 -= a8;
        k ^= sse_leaf_step(k + i, c ^ d);
    }
    v128 r;
    r.f = (a0 + a1 + a2 + a3) + (a4 + a5 + a6 + a7) + (a8 + a9 + a10 + a11);
    return hash_v128(r) ^ k;
}
