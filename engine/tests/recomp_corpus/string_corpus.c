/* R1 differential corpus, string family (`docs/RECOMP_PLAN.md` "R1 design", "Oracle").
 *
 * Compiled for Win64 alongside `int_corpus.c` (`--target=x86_64-pc-windows-msvc`, freestanding,
 * no SSE) and linked into `corpus.exe`. Every export is `u64 f(u64 a, u64 b, u64 c, u64 d)`;
 * `m_*` exports receive a 4096-byte 16-byte-aligned buffer in `a` and stay inside it.
 *
 * AT&T syntax note: 32-bit string ops are written `movsl`/`stosl`/`lodsl`/`cmpsl`/`scasl`,
 * which assemble to the x86 `movsd`/`stosd`/`lodsd`/`cmpsd`/`scasd` opcodes (0xA5/AB/AD/A7/AF).
 * Every `std` is paired with `cld` inside the same asm block so DF is clear on exit.
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

static inline u64 pack_flags(u8 zf, u8 cf, u8 sf, u8 of, u8 pf) {
    return ((u64)(zf & 1U)) | (((u64)(cf & 1U)) << 1) | (((u64)(sf & 1U)) << 2) |
           (((u64)(of & 1U)) << 3) | (((u64)(pf & 1U)) << 4);
}

static inline void write_u16_le(u8* p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void write_u32_le(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static inline void write_u64_le(u8* p, u64 v) {
    for (u64 i = 0; i < 8; ++i) {
        p[i] = (u8)(v >> (i * 8));
    }
}

#define RUN_CMPS(insn, p1, p2, n, t, fl_out)                                                       \
    do {                                                                                           \
        u8 _zf = 0, _cf = 0, _sf = 0, _of = 0, _pf = 0;                                            \
        u8 _t = (u8)(t);                                                                           \
        __asm__ volatile(                                                                          \
            "cmpb $1, %[t]\n\t" insn                                                               \
            "\n\t"                                                                                 \
            "setz %[zf]\n\t"                                                                       \
            "setc %[cf]\n\t"                                                                       \
            "sets %[sf]\n\t"                                                                       \
            "seto %[of]\n\t"                                                                       \
            "setp %[pf]"                                                                           \
            : "+S"(p1), "+D"(p2), "+c"(n), [zf] "=r"(_zf), [cf] "=r"(_cf), [sf] "=r"(_sf),         \
              [of] "=r"(_of), [pf] "=r"(_pf)                                                       \
            : [t] "r"(_t)                                                                          \
            : "memory", "cc");                                                                     \
        (fl_out) = pack_flags(_zf, _cf, _sf, _of, _pf);                                            \
    } while (0)

#define RUN_CMPS_1(insn, p1, p2, keep_c, fl_out)                                                   \
    do {                                                                                           \
        u8 _zf = 0, _cf = 0, _sf = 0, _of = 0, _pf = 0;                                            \
        __asm__ volatile(insn                                                                      \
                         "\n\t"                                                                    \
                         "setz %[zf]\n\t"                                                          \
                         "setc %[cf]\n\t"                                                          \
                         "sets %[sf]\n\t"                                                          \
                         "seto %[of]\n\t"                                                          \
                         "setp %[pf]"                                                              \
                         : "+S"(p1), "+D"(p2), "+c"(keep_c), [zf] "=r"(_zf), [cf] "=r"(_cf),       \
                           [sf] "=r"(_sf), [of] "=r"(_of), [pf] "=r"(_pf)                          \
                         :                                                                         \
                         : "memory", "cc");                                                        \
        (fl_out) = pack_flags(_zf, _cf, _sf, _of, _pf);                                            \
    } while (0)

#define RUN_SCAS(insn, p, n, val, t, fl_out)                                                       \
    do {                                                                                           \
        u8 _zf = 0, _cf = 0, _sf = 0, _of = 0, _pf = 0;                                            \
        u8 _t = (u8)(t);                                                                           \
        __asm__ volatile(                                                                          \
            "cmpb $1, %[t]\n\t" insn                                                               \
            "\n\t"                                                                                 \
            "setz %[zf]\n\t"                                                                       \
            "setc %[cf]\n\t"                                                                       \
            "sets %[sf]\n\t"                                                                       \
            "seto %[of]\n\t"                                                                       \
            "setp %[pf]"                                                                           \
            : "+D"(p), "+c"(n), [zf] "=r"(_zf), [cf] "=r"(_cf), [sf] "=r"(_sf), [of] "=r"(_of),    \
              [pf] "=r"(_pf)                                                                       \
            : "a"(val), [t] "r"(_t)                                                                \
            : "memory", "cc");                                                                     \
        (fl_out) = pack_flags(_zf, _cf, _sf, _of, _pf);                                            \
    } while (0)

#define RUN_SCAS_1(insn, p, keep_c, val, fl_out)                                                   \
    do {                                                                                           \
        u8 _zf = 0, _cf = 0, _sf = 0, _of = 0, _pf = 0;                                            \
        __asm__ volatile(insn                                                                      \
                         "\n\t"                                                                    \
                         "setz %[zf]\n\t"                                                          \
                         "setc %[cf]\n\t"                                                          \
                         "sets %[sf]\n\t"                                                          \
                         "seto %[of]\n\t"                                                          \
                         "setp %[pf]"                                                              \
                         : "+D"(p), "+c"(keep_c), [zf] "=r"(_zf), [cf] "=r"(_cf), [sf] "=r"(_sf),  \
                           [of] "=r"(_of), [pf] "=r"(_pf)                                          \
                         : "a"(val)                                                                \
                         : "memory", "cc");                                                        \
        (fl_out) = pack_flags(_zf, _cf, _sf, _of, _pf);                                            \
    } while (0)

/* --- MOVS ----------------------------------------------------------------------------------- */

EXPORT u64 m_str_rep_movsb(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    const u8* s0 = buf + (c & 255U);
    u8* d0 = buf + 2048 + (d & 255U);
    u64 n0 = 0;
    __asm__ volatile("rep movsb" : "+D"(d0), "+S"(s0), "+c"(n0) : : "memory");

    const u8* s1 = buf + 256 + (c & 63U);
    u8* d1 = buf + 2304 + (d & 63U);
    u64 n1 = 1;
    __asm__ volatile("rep movsb" : "+D"(d1), "+S"(s1), "+c"(n1) : : "memory");

    const u8* s2 = buf + (c & 511U);
    u8* d2 = buf + 2048 + (d & 511U);
    u64 n2 = (b & 1023U) + 1U;
    __asm__ volatile("rep movsb" : "+D"(d2), "+S"(s2), "+c"(n2) : : "memory");

    return (u64)(d0 - buf) ^ ((u64)(s0 - buf) << 8) ^ (n0 << 16) ^ ((u64)(d1 - buf) << 20) ^
           ((u64)(s1 - buf) << 30) ^ (n1 << 40) ^ ((u64)(d2 - buf) << 44) ^
           ((u64)(s2 - buf) << 54) ^ (n2 * 17ULL);
}

EXPORT u64 m_str_movsb_overlap(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    /* Forward overlap: dst < src with DF=0. */
    u64 shift1 = (c & 31U) + 1U;
    u64 n1 = (b & 511U) + 1U;
    u8* d1 = buf + 16;
    const u8* s1 = d1 + shift1;
    __asm__ volatile("rep movsb" : "+D"(d1), "+S"(s1), "+c"(n1) : : "memory");

    /* Backward overlap: dst > src with DF=1 (std ... cld). */
    u64 shift2 = (d & 31U) + 1U;
    u64 n2 = (b & 511U) + 1U;
    const u8* s2 = buf + 2048 + (n2 - 1U);
    u8* d2 = buf + 2048 + shift2 + (n2 - 1U);
    __asm__ volatile("std\n\trep movsb\n\tcld"
                     : "+D"(d2), "+S"(s2), "+c"(n2)
                     :
                     : "memory", "cc");

    return (u64)(d1 - buf) ^ ((u64)(s1 - buf) << 12) ^ (n1 << 24) ^ ((u64)(d2 - buf) << 32) ^
           ((u64)(s2 - buf) << 44) ^ (n2 * 131ULL);
}

EXPORT u64 m_str_rep_movs_wdq(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    const u8* sw = buf + (c & 15U);
    u8* dw = buf + 2048 + (d & 15U);
    u64 nw = (b & 127U) + 1U;
    __asm__ volatile("rep movsw" : "+D"(dw), "+S"(sw), "+c"(nw) : : "memory");

    const u8* sd = buf + 512 + (b & 7U);
    u8* dd = buf + 2560 + (d & 7U);
    u64 nd = (c & 63U) + 1U;
    __asm__ volatile("rep movsl" : "+D"(dd), "+S"(sd), "+c"(nd) : : "memory");

    const u8* sq = buf + 1024 + (b & 7U);
    u8* dq = buf + 3072 + (c & 7U);
    u64 nq = (d & 31U) + 1U;
    __asm__ volatile("rep movsq" : "+D"(dq), "+S"(sq), "+c"(nq) : : "memory");

    u64 nq2 = (b & 15U) + 1U;
    const u8* sq2 = buf + 1500 + (nq2 - 1U) * 8U;
    u8* dq2 = buf + 1508 + (nq2 - 1U) * 8U;
    __asm__ volatile("std\n\trep movsq\n\tcld"
                     : "+D"(dq2), "+S"(sq2), "+c"(nq2)
                     :
                     : "memory", "cc");

    return (u64)(dw - buf) ^ ((u64)(sw - buf) << 10) ^ ((u64)(dd - buf) << 20) ^
           ((u64)(sd - buf) << 30) ^ ((u64)(dq - buf) << 40) ^ ((u64)(sq - buf) << 50) ^
           ((u64)(dq2 - buf) * 7ULL) ^ ((u64)(sq2 - buf) * 13ULL) ^ nw ^ (nd << 4) ^ (nq << 8) ^
           (nq2 << 12);
}

EXPORT u64 m_str_unprefixed_movs(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    const u8* s1 = buf + 64 + (b & 15U);
    u8* d1 = buf + 2048 + (c & 15U);
    u64 keep_c = d;
    __asm__ volatile("movsb\n\tmovsw\n\tmovsl\n\tmovsq"
                     : "+D"(d1), "+S"(s1), "+c"(keep_c)
                     :
                     : "memory");

    const u8* s2 = buf + 1024 + (b & 15U);
    u8* d2 = buf + 3072 + (c & 15U);
    __asm__ volatile("std\n\tmovsb\n\tmovsw\n\tmovsl\n\tmovsq\n\tcld"
                     : "+D"(d2), "+S"(s2), "+c"(keep_c)
                     :
                     : "memory", "cc");

    return keep_c ^ (u64)(d1 - buf) ^ ((u64)(s1 - buf) << 12) ^ ((u64)(d2 - buf) << 24) ^
           ((u64)(s2 - buf) << 36);
}

/* --- STOS ----------------------------------------------------------------------------------- */

EXPORT u64 m_str_rep_stosb(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    u8* d0 = buf + (c & 255U);
    u64 n0 = 0;
    u8 v0 = (u8)d;
    __asm__ volatile("rep stosb" : "+D"(d0), "+c"(n0) : "a"(v0) : "memory");

    u8* d1 = buf + 512 + (c & 63U);
    u64 n1 = 1;
    u8 v1 = (u8)(d >> 8);
    __asm__ volatile("rep stosb" : "+D"(d1), "+c"(n1) : "a"(v1) : "memory");

    u8* d2 = buf + 1024 + (c & 127U);
    u64 n2 = (b & 1023U) + 1U;
    u8 v2 = (u8)(d >> 16);
    __asm__ volatile("rep stosb" : "+D"(d2), "+c"(n2) : "a"(v2) : "memory");

    u64 n3 = (d & 255U) + 1U;
    u8* d3 = buf + 3000 + (n3 - 1U);
    u8 v3 = (u8)(b >> 8);
    __asm__ volatile("std\n\trep stosb\n\tcld"
                     : "+D"(d3), "+c"(n3)
                     : "a"(v3)
                     : "memory", "cc");

    return (u64)(d0 - buf) ^ ((u64)(d1 - buf) << 12) ^ ((u64)(d2 - buf) << 24) ^
           ((u64)(d3 - buf) << 36) ^ n0 ^ (n1 << 2) ^ (n2 << 4) ^ (n3 << 6);
}

EXPORT u64 m_str_rep_stosd_sizes(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    u8* d0 = buf + (c & 63U);
    u64 n0 = 0;
    u32 v32 = (u32)d;
    __asm__ volatile("rep stosl" : "+D"(d0), "+c"(n0) : "a"(v32) : "memory");

    u8* d1 = buf + 64 + (d & 63U);
    u64 n1 = 1;
    __asm__ volatile("rep stosl" : "+D"(d1), "+c"(n1) : "a"(v32) : "memory");

    u8* d2 = buf + 256;
    u64 n2 = (b & 255U) + 2U;
    __asm__ volatile("rep stosl" : "+D"(d2), "+c"(n2) : "a"(v32) : "memory");

    u8* dw = buf + 1400;
    u64 nw = (c & 127U) + 1U;
    u16 v16 = (u16)(b >> 16);
    __asm__ volatile("rep stosw" : "+D"(dw), "+c"(nw) : "a"(v16) : "memory");

    u8* dq = buf + 1800;
    u64 nq = (d & 63U) + 1U;
    u64 v64 = b ^ (c << 13);
    __asm__ volatile("rep stosq" : "+D"(dq), "+c"(nq) : "a"(v64) : "memory");

    u64 nq2 = (b & 31U) + 1U;
    u8* dq2 = buf + 2500 + (nq2 - 1U) * 8U;
    __asm__ volatile("std\n\trep stosq\n\tcld"
                     : "+D"(dq2), "+c"(nq2)
                     : "a"(v64)
                     : "memory", "cc");

    u8* du = buf + 3000 + (c & 15U);
    u64 keep_c = d;
    __asm__ volatile("stosb\n\tstosw\n\tstosl\n\tstosq"
                     : "+D"(du), "+c"(keep_c)
                     : "a"(v64)
                     : "memory");

    return keep_c ^ (u64)(d0 - buf) ^ ((u64)(d1 - buf) << 10) ^ ((u64)(d2 - buf) << 20) ^
           ((u64)(dw - buf) << 30) ^ ((u64)(dq - buf) << 40) ^ ((u64)(dq2 - buf) << 50) ^
           ((u64)(du - buf) * 19ULL) ^ n0 ^ (n1 << 2) ^ (n2 << 4) ^ (nw << 6) ^ (nq << 8) ^
           (nq2 << 10);
}

/* --- LODS ----------------------------------------------------------------------------------- */

EXPORT u64 m_str_lodsq_checksum(u64 a, u64 b, u64 c, u64 d) {
    const u8* buf = (const u8*)a;
    u64 sum = d ^ 0x9E3779B97F4A7C15ULL;

    const u8* p1 = buf + (c & 255U);
    u64 n1 = (b & 127U) + 1U;
    for (u64 i = 0; i < n1; ++i) {
        u64 val = 0;
        __asm__ volatile("lodsq" : "=a"(val), "+S"(p1) : : "memory");
        sum = (sum * 1315423911ULL) ^ val;
    }

    u64 n2 = (d & 63U) + 1U;
    const u8* p2 = buf + 2048 + (n2 - 1U) * 8U;
    for (u64 i = 0; i < n2; ++i) {
        u64 val = 0;
        __asm__ volatile("std\n\tlodsq\n\tcld" : "=a"(val), "+S"(p2) : : "memory", "cc");
        sum = (sum * 31ULL) + val;
    }

    return sum ^ ((u64)(p1 - buf) << 16) ^ ((u64)(p2 - buf) << 32);
}

EXPORT u64 m_str_lods_widths(u64 a, u64 b, u64 c, u64 d) {
    const u8* buf = (const u8*)a;
    const u8* p = buf + 64 + (b & 15U);
    u64 keep_c = c ^ d;

    /* Test 8/16-bit RAX merge vs 32-bit zero-extension vs 64-bit load. */
    u64 r1 = d | 0xA5A5A5A55A5A5A5AULL;
    __asm__ volatile("lodsb" : "+a"(r1), "+S"(p), "+c"(keep_c) : : "memory");

    u64 r2 = c | 0x123456789ABCDEF0ULL;
    __asm__ volatile("lodsw" : "+a"(r2), "+S"(p) : : "memory");

    u64 r3 = b | 0xFEDCBA9800000000ULL;
    __asm__ volatile("lodsl" : "+a"(r3), "+S"(p) : : "memory");

    u64 r4 = d;
    __asm__ volatile("lodsq" : "+a"(r4), "+S"(p) : : "memory");

    u64 n0 = 0;
    u64 r5 = d ^ 0xCAFEBABEDEADBEEFULL;
    __asm__ volatile("rep lodsb" : "+a"(r5), "+S"(p), "+c"(n0) : : "memory");

    u64 nw = (b & 15U) + 1U;
    u64 r6 = c;
    __asm__ volatile("rep lodsw" : "+a"(r6), "+S"(p), "+c"(nw) : : "memory");

    u64 nd = (c & 15U) + 1U;
    u64 r7 = d;
    __asm__ volatile("rep lodsl" : "+a"(r7), "+S"(p), "+c"(nd) : : "memory");

    u64 nq = (d & 15U) + 1U;
    u64 r8 = b;
    __asm__ volatile("rep lodsq" : "+a"(r8), "+S"(p), "+c"(nq) : : "memory");

    u64 nd2 = (b & 7U) + 1U;
    const u8* p2 = buf + 1024 + (nd2 - 1U) * 4U;
    u64 r9 = c | 0xAAAAAAAA00000000ULL;
    __asm__ volatile("std\n\trep lodsl\n\tcld"
                     : "+a"(r9), "+S"(p2), "+c"(nd2)
                     :
                     : "memory", "cc");

    return keep_c ^ r1 ^ (r2 * 3ULL) ^ (r3 * 5ULL) ^ (r4 * 7ULL) ^ r5 ^ (r6 * 11ULL) ^
           (r7 * 13ULL) ^ (r8 * 17ULL) ^ r9 ^ ((u64)(p - buf) << 20) ^ ((u64)(p2 - buf) << 32) ^
           n0 ^ nw ^ nd ^ nq ^ nd2;
}

/* --- SCAS ----------------------------------------------------------------------------------- */

EXPORT u64 m_str_repne_scasb_strlen(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    u64 off = c & 255U;
    buf[off + (b & 511U)] = 0;
    buf[off + 1024U] = 0;

    const u8* p1 = buf + off;
    u64 n1 = d & 1023U;
    u64 fl1 = 0;
    RUN_SCAS("repne scasb", p1, n1, (u8)0, (u8)(d >> 10), fl1);

    /* Guaranteed hit before count expires. */
    u64 off2 = 2048U + (b & 63U);
    for (u64 i = 0; i < 64; ++i) {
        if (buf[2048U + i] == 0) {
            buf[2048U + i] = 1;
        }
    }
    buf[off2] = 0;
    const u8* p2 = buf + 2048U;
    u64 n2 = 128;
    u64 fl2 = 0;
    RUN_SCAS("repne scasb", p2, n2, (u8)0, (u8)1, fl2);

    return (u64)(p1 - buf) ^ (n1 << 12) ^ (fl1 << 24) ^ ((u64)(p2 - buf) << 30) ^ (n2 << 42) ^
           (fl2 << 52);
}

EXPORT u64 m_str_repe_scasb(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    u8 fill = (u8)d;
    u64 base = 64U + (c & 63U);
    u64 run = b & 127U;
    for (u64 i = 0; i < run; ++i) {
        buf[base + i] = fill;
    }
    buf[base + run] = (u8)(fill ^ 1U);

    const u8* p1 = buf + base;
    u64 n1 = (d >> 8) & 255U;
    u64 fl1 = 0;
    RUN_SCAS("repe scasb", p1, n1, fill, (u8)(c >> 16), fl1);

    /* DF=1 backward skip-matching scan. */
    u64 base2 = 2048U;
    u64 run2 = b & 63U;
    buf[base2] = (u8)(fill ^ 1U);
    for (u64 i = 1; i <= run2; ++i) {
        buf[base2 + i] = fill;
    }
    const u8* p2 = buf + base2 + run2;
    u64 n2 = (c >> 8) & 127U;
    u64 fl2 = 0;
    RUN_SCAS("std\n\trepe scasb\n\tcld", p2, n2, fill, (u8)(d >> 16), fl2);

    return (u64)(p1 - buf) ^ (n1 << 12) ^ (fl1 << 22) ^ ((u64)(p2 - buf) << 30) ^ (n2 << 42) ^
           (fl2 << 52);
}

EXPORT u64 m_str_scas_wdq(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;

    /* 16-bit scasw (repne / repe) */
    u16 wval = (u16)(d | 1U);
    for (u64 i = 0; i < 32; ++i) {
        write_u16_le(buf + 256U + i * 2U, (u16)(wval ^ (u16)(i + 1U)));
        write_u16_le(buf + 512U + i * 2U, wval);
    }
    write_u16_le(buf + 256U + (c & 15U) * 2U, wval);
    write_u16_le(buf + 512U + (b & 15U) * 2U, (u16)(wval ^ 0x101U));

    const u8* pw1 = buf + 256U;
    u64 nw1 = (b & 31U) + 1U;
    u64 flw1 = 0;
    RUN_SCAS("repne scasw", pw1, nw1, wval, (u8)c, flw1);

    const u8* pw2 = buf + 512U;
    u64 nw2 = (c & 31U) + 1U;
    u64 flw2 = 0;
    RUN_SCAS("repe scasw", pw2, nw2, wval, (u8)d, flw2);

    /* 32-bit scasl (repne / repe) */
    u32 dval = (u32)(b ^ 0x55AA1234U);
    for (u64 i = 0; i < 32; ++i) {
        write_u32_le(buf + 1024U + i * 4U, dval ^ (u32)(i + 1U));
        write_u32_le(buf + 1536U + i * 4U, dval);
    }
    write_u32_le(buf + 1024U + (d & 15U) * 4U, dval);
    write_u32_le(buf + 1536U + (c & 15U) * 4U, dval ^ 0x80000001U);

    const u8* pd1 = buf + 1024U;
    u64 nd1 = (b & 31U) + 1U;
    u64 fld1 = 0;
    RUN_SCAS("repne scasl", pd1, nd1, dval, (u8)b, fld1);

    const u8* pd2 = buf + 1536U;
    u64 nd2 = (d & 31U) + 1U;
    u64 fld2 = 0;
    RUN_SCAS("repe scasl", pd2, nd2, dval, (u8)c, fld2);

    /* 64-bit scasq (repne / repe) */
    u64 qval = d ^ 0x1122334455667788ULL;
    for (u64 i = 0; i < 16; ++i) {
        write_u64_le(buf + 2048U + i * 8U, qval ^ (i + 1U));
        write_u64_le(buf + 2560U + i * 8U, qval);
    }
    write_u64_le(buf + 2048U + (b & 7U) * 8U, qval);
    write_u64_le(buf + 2560U + (c & 7U) * 8U, qval ^ 0x8000000000000000ULL);

    const u8* pq1 = buf + 2048U;
    u64 nq1 = (c & 15U) + 1U;
    u64 flq1 = 0;
    RUN_SCAS("repne scasq", pq1, nq1, qval, (u8)d, flq1);

    const u8* pq2 = buf + 2560U;
    u64 nq2 = (d & 15U) + 1U;
    u64 flq2 = 0;
    RUN_SCAS("repe scasq", pq2, nq2, qval, (u8)b, flq2);

    /* Unprefixed scasb/w/l/q (DF=0 and DF=1). */
    const u8* pu = buf + 3072U;
    u64 keep_c = d;
    u64 flu1 = 0, flu2 = 0, flu3 = 0, flu4 = 0;
    RUN_SCAS_1("scasb", pu, keep_c, (u8)b, flu1);
    RUN_SCAS_1("scasw", pu, keep_c, (u16)c, flu2);
    RUN_SCAS_1("scasl", pu, keep_c, (u32)d, flu3);
    RUN_SCAS_1("std\n\tscasq\n\tcld", pu, keep_c, qval, flu4);

    return keep_c ^ (u64)(pw1 - buf) ^ (u64)(pw2 - buf) ^ (u64)(pd1 - buf) ^ (u64)(pd2 - buf) ^
           (u64)(pq1 - buf) ^ (u64)(pq2 - buf) ^ (u64)(pu - buf) ^ (nw1 << 8) ^ (nw2 << 13) ^
           (nd1 << 18) ^ (nd2 << 23) ^ (nq1 << 28) ^ (nq2 << 33) ^ (flw1 << 38) ^ (flw2 << 43) ^
           (fld1 << 48) ^ (fld2 << 53) ^ (flq1 << 58) ^ (flq2 * 3ULL) ^ (flu1 * 5ULL) ^
           (flu2 * 7ULL) ^ (flu3 * 11ULL) ^ (flu4 * 13ULL);
}

/* --- CMPS ----------------------------------------------------------------------------------- */

EXPORT u64 m_str_repe_cmpsb_memcmp(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    u64 off = c & 63U;
    u64 len = b & 511U;
    const u8* p1 = buf + off;
    u8* p2 = buf + 2048U + off;

    if ((d & 3U) == 0 || (d & 3U) == 1) {
        for (u64 i = 0; i < len; ++i) {
            p2[i] = p1[i];
        }
        if ((d & 3U) == 1 && len > 0) {
            p2[len / 2U] ^= (u8)(1U + ((d >> 4) & 0x7FU));
        }
    }

    const u8* cp2 = p2;
    u64 n = len;
    u64 fl = 0;
    RUN_CMPS("repe cmpsb", p1, cp2, n, (u8)(d >> 8), fl);

    return (u64)(p1 - buf) ^ ((u64)(cp2 - buf) << 12) ^ (n << 24) ^ (fl << 36);
}

EXPORT u64 m_str_repne_cmpsb(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    const u8* p1 = buf + 64U;
    u8* p2 = buf + 2048U;
    for (u64 i = 0; i < 128U; ++i) {
        p2[i] = (u8)(p1[i] ^ (u8)(1U + (i & 7U)));
    }
    if ((d & 1U) != 0) {
        p2[b & 63U] = p1[b & 63U];
    }

    const u8* s1 = p1;
    const u8* s2 = p2;
    u64 n1 = c & 127U;
    u64 fl1 = 0;
    RUN_CMPS("repne cmpsb", s1, s2, n1, (u8)(d >> 2), fl1);

    /* DF=1 backward repne cmpsb. */
    const u8* q1_base = buf + 1024U;
    u8* q2_base = buf + 3072U;
    for (u64 i = 0; i < 128U; ++i) {
        q2_base[i] = (u8)(q1_base[i] ^ (u8)(1U + (i & 15U)));
    }
    if ((d & 2U) != 0) {
        u64 eq_idx = 127U - (b & 31U);
        q2_base[eq_idx] = q1_base[eq_idx];
    }
    const u8* b1 = q1_base + 127U;
    const u8* b2 = q2_base + 127U;
    u64 n2 = (d >> 4) & 63U;
    u64 fl2 = 0;
    RUN_CMPS("std\n\trepne cmpsb\n\tcld", b1, b2, n2, (u8)(c >> 8), fl2);

    return (u64)(s1 - buf) ^ ((u64)(s2 - buf) << 12) ^ (n1 << 24) ^ (fl1 << 32) ^
           ((u64)(b1 - buf) << 38) ^ ((u64)(b2 - buf) << 50) ^ (n2 * 29ULL) ^ (fl2 << 58);
}

EXPORT u64 m_str_cmps_wdq(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    const u8* src = buf + 64U;
    u8* dst = buf + 2048U;
    for (u64 i = 0; i < 64U; ++i) {
        dst[i] = src[i];
    }
    if ((d & 1U) != 0) {
        dst[b & 63U] ^= (u8)(1U + (c & 127U));
    }

    const u8* p1 = src;
    const u8* p2 = dst;
    u64 nw1 = (b & 7U) + 1U;
    u64 flw1 = 0;
    RUN_CMPS("repe cmpsw", p1, p2, nw1, (u8)d, flw1);

    /* Ensure mismatch prefix then optional match for repne cmpsw. */
    for (u64 i = 0; i < 8U; ++i) {
        write_u16_le(buf + 2200U + i * 2U, (u16)((i + 1U) * 3U));
        write_u16_le(buf + 2300U + i * 2U, (u16)((i + 1U) * 3U + 1U));
    }
    if ((d & 2U) != 0) {
        write_u16_le(buf + 2300U + (c & 7U) * 2U, (u16)(((c & 7U) + 1U) * 3U));
    }
    const u8* pw1 = buf + 2200U;
    const u8* pw2 = buf + 2300U;
    u64 nw2 = (c & 7U) + 1U;
    u64 flw2 = 0;
    RUN_CMPS("repne cmpsw", pw1, pw2, nw2, (u8)b, flw2);

    /* 32-bit repe/repne cmpsl */
    p1 = src;
    p2 = dst;
    u64 nd1 = (c & 7U) + 1U;
    u64 fld1 = 0;
    RUN_CMPS("repe cmpsl", p1, p2, nd1, (u8)d, fld1);

    for (u64 i = 0; i < 8U; ++i) {
        write_u32_le(buf + 2400U + i * 4U, (u32)(i + 10U));
        write_u32_le(buf + 2500U + i * 4U, (u32)(i + 20U));
    }
    if ((d & 4U) != 0) {
        write_u32_le(buf + 2500U + (b & 7U) * 4U, (u32)((b & 7U) + 10U));
    }
    const u8* pd1 = buf + 2400U;
    const u8* pd2 = buf + 2500U;
    u64 nd2 = (b & 7U) + 1U;
    u64 fld2 = 0;
    RUN_CMPS("repne cmpsl", pd1, pd2, nd2, (u8)c, fld2);

    /* 64-bit repe/repne cmpsq */
    p1 = src;
    p2 = dst;
    u64 nq1 = (d & 7U) + 1U;
    u64 flq1 = 0;
    RUN_CMPS("repe cmpsq", p1, p2, nq1, (u8)b, flq1);

    for (u64 i = 0; i < 8U; ++i) {
        write_u64_le(buf + 2600U + i * 8U, i + 100ULL);
        write_u64_le(buf + 2700U + i * 8U, i + 200ULL);
    }
    if ((d & 8U) != 0) {
        write_u64_le(buf + 2700U + (c & 7U) * 8U, (c & 7U) + 100ULL);
    }
    const u8* pq1 = buf + 2600U;
    const u8* pq2 = buf + 2700U;
    u64 nq2 = (c & 7U) + 1U;
    u64 flq2 = 0;
    RUN_CMPS("repne cmpsq", pq1, pq2, nq2, (u8)d, flq2);

    /* Unprefixed cmpsb/w/l/q (DF=0 and DF=1). */
    const u8* u1 = buf + 1024U;
    const u8* u2 = buf + 3072U;
    u64 keep_c = b;
    u64 flu1 = 0, flu2 = 0, flu3 = 0, flu4 = 0;
    RUN_CMPS_1("cmpsb", u1, u2, keep_c, flu1);
    RUN_CMPS_1("cmpsw", u1, u2, keep_c, flu2);
    RUN_CMPS_1("cmpsl", u1, u2, keep_c, flu3);
    RUN_CMPS_1("std\n\tcmpsq\n\tcld", u1, u2, keep_c, flu4);

    return keep_c ^ (u64)(p1 - buf) ^ (u64)(p2 - buf) ^ (u64)(pw1 - buf) ^ (u64)(pw2 - buf) ^
           (u64)(pd1 - buf) ^ (u64)(pd2 - buf) ^ (u64)(pq1 - buf) ^ (u64)(pq2 - buf) ^
           (u64)(u1 - buf) ^ (u64)(u2 - buf) ^ (nw1 << 8) ^ (nw2 << 12) ^ (nd1 << 16) ^
           (nd2 << 20) ^ (nq1 << 24) ^ (nq2 << 28) ^ (flw1 << 32) ^ (flw2 << 37) ^ (fld1 << 42) ^
           (fld2 << 47) ^ (flq1 << 52) ^ (flq2 << 57) ^ (flu1 * 3ULL) ^ (flu2 * 5ULL) ^
           (flu3 * 7ULL) ^ (flu4 * 11ULL);
}

EXPORT u64 m_str_cmps_scas_flags_edge(u64 a, u64 b, u64 c, u64 d) {
    static const u64 lhs[8] = {0x80ULL,
                               0x7FULL,
                               0x00ULL,
                               0xFFULL,
                               0x8000ULL,
                               0x80000000ULL,
                               0x8000000000000000ULL,
                               0x0303030303030303ULL};
    static const u64 rhs[8] = {0x7FULL,
                               0x80ULL,
                               0x01ULL,
                               0x00ULL,
                               0x7FFFULL,
                               0x7FFFFFFFULL,
                               0x7FFFFFFFFFFFFFFFULL,
                               0x0303030303030303ULL};
    u8* buf = (u8*)a;
    u64 lval = lhs[b & 7U];
    u64 rval = rhs[c & 7U];
    if ((d & 3U) == 0) {
        rval = lval;
    }

    write_u64_le(buf + 64U, lval);
    write_u64_le(buf + 2048U, rval);

    u64 keep_c = d;
    u64 f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    const u8* p1 = buf + 64U;
    const u8* p2 = buf + 2048U;
    RUN_CMPS_1("cmpsb", p1, p2, keep_c, f0);
    p1 = buf + 64U;
    p2 = buf + 2048U;
    RUN_CMPS_1("cmpsw", p1, p2, keep_c, f1);
    p1 = buf + 64U;
    p2 = buf + 2048U;
    RUN_CMPS_1("cmpsl", p1, p2, keep_c, f2);
    p1 = buf + 64U;
    p2 = buf + 2048U;
    RUN_CMPS_1("cmpsq", p1, p2, keep_c, f3);

    u64 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    const u8* sp = buf + 2048U;
    RUN_SCAS_1("scasb", sp, keep_c, (u8)lval, s0);
    sp = buf + 2048U;
    RUN_SCAS_1("scasw", sp, keep_c, (u16)lval, s1);
    sp = buf + 2048U;
    RUN_SCAS_1("scasl", sp, keep_c, (u32)lval, s2);
    sp = buf + 2048U;
    RUN_SCAS_1("scasq", sp, keep_c, lval, s3);

    return keep_c ^ f0 ^ (f1 << 5) ^ (f2 << 10) ^ (f3 << 15) ^ (s0 << 20) ^ (s1 << 25) ^
           (s2 << 30) ^ (s3 << 35);
}

/* --- Zero count across all 20 mnemonics ----------------------------------------------------- */

EXPORT u64 m_str_all_zero_count(u64 a, u64 b, u64 c, u64 d) {
    u8* buf = (u8*)a;
    const u8* src = buf + 128U + (b & 63U);
    u8* dst = buf + 2048U + (c & 63U);
    u64 n = 0;
    u64 acc = d ^ 0x0123456789ABCDEFULL;
    u8 t = (u8)(d & 3U) == 0 ? 0U : ((u8)(d & 3U) == 1 ? 1U : 0x80U);

    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep movsw" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");

    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(acc) : "memory");
    __asm__ volatile("rep stosw" : "+D"(dst), "+c"(n) : "a"(acc) : "memory");
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(n) : "a"(acc) : "memory");
    __asm__ volatile("rep stosq" : "+D"(dst), "+c"(n) : "a"(acc) : "memory");

    __asm__ volatile("rep lodsb" : "+a"(acc), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep lodsw" : "+a"(acc), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep lodsl" : "+a"(acc), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep lodsq" : "+a"(acc), "+S"(src), "+c"(n) : : "memory");

    const u8* cdst = dst;
    u64 fl = 0, fl_acc = 0;
    RUN_CMPS("repe cmpsb", src, cdst, n, t, fl);
    fl_acc ^= fl;
    RUN_CMPS("repe cmpsw", src, cdst, n, t, fl);
    fl_acc ^= fl << 5;
    RUN_CMPS("repe cmpsl", src, cdst, n, t, fl);
    fl_acc ^= fl << 10;
    RUN_CMPS("repe cmpsq", src, cdst, n, t, fl);
    fl_acc ^= fl << 15;
    RUN_CMPS("repne cmpsb", src, cdst, n, t, fl);
    fl_acc ^= fl << 20;
    RUN_CMPS("repne cmpsw", src, cdst, n, t, fl);
    fl_acc ^= fl << 25;
    RUN_CMPS("repne cmpsl", src, cdst, n, t, fl);
    fl_acc ^= fl << 30;
    RUN_CMPS("repne cmpsq", src, cdst, n, t, fl);
    fl_acc ^= fl << 35;

    RUN_SCAS("repe scasb", cdst, n, (u8)acc, t, fl);
    fl_acc ^= fl << 40;
    RUN_SCAS("repe scasw", cdst, n, (u16)acc, t, fl);
    fl_acc ^= fl << 45;
    RUN_SCAS("repe scasl", cdst, n, (u32)acc, t, fl);
    fl_acc ^= fl << 50;
    RUN_SCAS("repe scasq", cdst, n, acc, t, fl);
    fl_acc ^= fl << 55;
    RUN_SCAS("repne scasb", cdst, n, (u8)acc, t, fl);
    fl_acc ^= fl << 2;
    RUN_SCAS("repne scasw", cdst, n, (u16)acc, t, fl);
    fl_acc ^= fl << 7;
    RUN_SCAS("repne scasl", cdst, n, (u32)acc, t, fl);
    fl_acc ^= fl << 12;
    RUN_SCAS("repne scasq", cdst, n, acc, t, fl);
    fl_acc ^= fl << 17;

    return acc ^ fl_acc ^ n ^ ((u64)(cdst - buf) << 24) ^ ((u64)(src - buf) << 36);
}

/* --- Non-buffer exports (stack / zero-count unmapped pointers) ------------------------------ */

EXPORT u64 str_local_stack(u64 a, u64 b, u64 c, u64 d) {
    u8 s1[64];
    u8 s2[64];
    for (u64 i = 0; i < 8; ++i) {
        write_u64_le(s1 + i * 8U, a ^ (b * (i + 1U)));
    }
    const u8* psrc = s1;
    u8* pdst = s2;
    u64 nq = 8;
    __asm__ volatile("rep movsq" : "+D"(pdst), "+S"(psrc), "+c"(nq) : : "memory");

    u8* fillp = s2 + 16U;
    u64 nf = 4;
    u32 fval = (u32)c;
    __asm__ volatile("rep stosl" : "+D"(fillp), "+c"(nf) : "a"(fval) : "memory");

    const u8* c1 = s1;
    const u8* c2 = s2;
    u64 nc = 32;
    u64 flc = 0;
    RUN_CMPS("repe cmpsw", c1, c2, nc, (u8)d, flc);

    const u8* sc = s2;
    u64 ns = 64;
    u64 fls = 0;
    RUN_SCAS("repne scasb", sc, ns, (u8)(a >> 8), (u8)1, fls);

    const u8* lp = s2 + (b & 31U);
    u64 lv = d;
    __asm__ volatile("lodsl" : "+a"(lv), "+S"(lp) : : "memory");

    const u8* bs = s1 + 47U;
    u8* bd = s2 + 55U;
    u64 nb = 16;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(bd), "+S"(bs), "+c"(nb) : : "memory", "cc");

    u64 hash = 0;
    for (u64 i = 0; i < 64; ++i) {
        hash = hash * 131ULL + s2[i];
    }
    return hash ^ lv ^ flc ^ (fls << 8) ^ nq ^ nf ^ nc ^ ns ^ nb ^ ((u64)(pdst - s2) << 16) ^
           ((u64)(c1 - s1) << 24) ^ ((u64)(c2 - s2) << 32) ^ ((u64)(sc - s2) << 40) ^
           ((u64)(lp - s2) << 48) ^ ((u64)(bd - s2) << 56);
}

EXPORT u64 str_null_zero_count(u64 a, u64 b, u64 c, u64 d) {
    /* When rcx == 0, x86 never dereferences [rsi] or [rdi] and preserves all flags. */
    const u8* src = (const u8*)a;
    u8* dst = (u8*)b;
    u64 n = 0;
    u64 acc = c;
    u8 t = (u8)(d & 1U);

    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(acc) : "memory");
    __asm__ volatile("rep lodsb" : "+a"(acc), "+S"(src), "+c"(n) : : "memory");

    const u8* cdst = dst;
    u64 fl1 = 0, fl2 = 0;
    RUN_CMPS("repe cmpsb", src, cdst, n, t, fl1);
    RUN_SCAS("repne scasb", cdst, n, (u8)acc, t, fl2);

    return (u64)src ^ (u64)cdst ^ n ^ acc ^ fl1 ^ (fl2 << 8);
}
