/* R4 corpus DLL: a second translated module (`docs/RECOMP_PLAN.md` "R4 design", slice 2;
 * `wse/recomp/shim_modules.h`).
 *
 * Our own code, linked by lld-link into `corpus_dll.dll` (preferred base 0x180000000, with base
 * relocations) and imported by the corpus exe (`mod_corpus.c`). The harness maps it at another base
 * so the relocations apply, translates it as its own module and links the two.
 *   d_*    pure functions over relocated data and pointers: run natively and translated (the
 *          differential), like the exe's exports
 *   dt_*   called only by the exe's `t_mod_*` tests: DllMain state, a callback into the exe, an
 *          exception raised here and caught in the exe
 * `corpus_dll.def` adds a forwarder (`d_fwd` = KERNEL32.GetCurrentThreadId) and an export by
 * ordinal only (`dt_by_ordinal` @42).
 */

typedef unsigned long long u64;
typedef unsigned int u32;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

__declspec(dllimport) void RaiseException(u32 code, u32 flags, u32 nargs, const u64* args);
extern char __ImageBase;

/* Reached only through the relocated table: exported so function discovery finds them. */
EXPORT NOINLINE u64 dt_op_add(u64 a, u64 b) { return a + b; }
EXPORT NOINLINE u64 dt_op_xor(u64 a, u64 b) { return a ^ (b << 3); }
EXPORT NOINLINE u64 dt_op_mul(u64 a, u64 b) { return a * (b | 1); }

/* Absolute pointers in .rdata / .data: base relocations (DIR64) fix them when the DLL moves. */
static u64 (*const k_ops[3])(u64, u64) = {dt_op_add, dt_op_xor, dt_op_mul};
static const char k_text[] = "corpus";
static const char* const k_text_ptr = k_text;
static u64 g_counter = 5;
static u64* const g_counter_ptr = &g_counter;

EXPORT NOINLINE u64 d_add(u64 a, u64 b) { return a + b; }

EXPORT NOINLINE u64 d_dispatch(u64 op, u64 a, u64 b, u64 c) {
    (void)c;
    return k_ops[op % 3](a, b) ^ (u64)(unsigned char)k_text_ptr[op % 6];
}

/* The base the code runs at, through a RIP-relative address and through a relocated pointer. */
EXPORT NOINLINE u64 d_image_base(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const u64 base = (u64)&__ImageBase;
    return (u64)k_text_ptr - (u64)k_text == 0 ? base : 1;
}

/* Reads and writes through a relocated pointer to .data (memory, so the differential compares). */
EXPORT NOINLINE u64 d_counter(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    *g_counter_ptr = *g_counter_ptr * 3 + a;
    const u64 v = *g_counter_ptr;
    *g_counter_ptr = 5;
    return v;
}

/* DllMain: records how the loader called it. */
static volatile u64 g_attach_count;
static volatile u64 g_attach_reason;
static volatile u64 g_attach_handle;
static volatile u64 g_attach_reserved;

EXPORT u32 dt_main(void* handle, u32 reason, void* reserved) {
    ++g_attach_count;
    g_attach_reason = reason;
    g_attach_handle = (u64)handle;
    g_attach_reserved = (u64)reserved;
    return 1;
}

/* 0x100 when the handle was this DLL's base, + 0x10 per call, + the last reason; bit 12 when
 * lpReserved was non-NULL (a static load). */
EXPORT NOINLINE u64 dt_attach_info(void) {
    return (g_attach_handle == (u64)&__ImageBase ? 0x100 : 0) + 0x10 * g_attach_count + g_attach_reason +
           (g_attach_reserved != 0 ? 0x1000 : 0);
}

/* A call back into the exe through a pointer (cross-module dispatch). */
EXPORT NOINLINE u64 dt_call(u64 (*fn)(u64), u64 x) { return fn(x) + 1; }

/* An exception raised in this module; the exe catches it. */
EXPORT NOINLINE void dt_raise(u32 code) { RaiseException(code, 0, 0, 0); }

/* Exported by ordinal 42 only (no name), through `corpus_dll.def`. */
NOINLINE u64 dt_by_ordinal(void) { return 0x42; }
