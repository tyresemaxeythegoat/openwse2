/* R3 corpus: the Win64 thread environment through the runtime (`docs/RECOMP_PLAN.md` "R3 design",
 * `wse/recomp/teb.h`).
 *
 * Our own code. The `t_*` exports read the TEB through `gs:`, use static TLS (`__declspec(thread)`:
 * clang emits the `_tls_index` / `gs:[0x58]` sequence MSVC does), a TLS callback, and the
 * KERNEL32 TLS, FLS and last-error calls. Like the SEH corpus they are self-checking (0x600D when
 * correct) because KERNEL32 does not exist on the Linux runners. There is no CRT, so the static TLS
 * directory (`_tls_used` and the `.tls` / `.CRT$XL*` bounds, normally from tlssup.obj) is defined
 * here; lld-link points the TLS data directory at `_tls_used`.
 */

typedef unsigned long long u64;
typedef unsigned int u32;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

__declspec(dllimport) u32 TlsAlloc(void);
__declspec(dllimport) int TlsFree(u32 index);
__declspec(dllimport) void* TlsGetValue(u32 index);
__declspec(dllimport) int TlsSetValue(u32 index, void* value);
__declspec(dllimport) u32 FlsAlloc(void (*callback)(void*));
__declspec(dllimport) int FlsFree(u32 index);
__declspec(dllimport) void* FlsGetValue(u32 index);
__declspec(dllimport) int FlsSetValue(u32 index, void* value);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) void SetLastError(u32 code);
__declspec(dllimport) u32 GetCurrentThreadId(void);
extern char __ImageBase;

/* --- The static TLS directory (what tlssup.obj provides) -------------------------------------- */
typedef void (*TlsCallback)(void* base, u32 reason, void* reserved);
typedef struct {
    u64 raw_begin, raw_end, index, callbacks;
    u32 zero_fill, characteristics;
} TlsDirectory;

u32 _tls_index = 0;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__attribute__((section(".CRT$XLA"))) TlsCallback __xl_a = 0;
__attribute__((section(".CRT$XLZ"))) TlsCallback __xl_z = 0;

static volatile u64 g_attach_count;
static volatile u64 g_attach_base;

EXPORT void teb_tls_callback(void* base, u32 reason, void* reserved) {
    (void)reserved;
    if (reason == 1) { /* DLL_PROCESS_ATTACH */
        ++g_attach_count;
        g_attach_base = (u64)base;
    }
}
__attribute__((section(".CRT$XLB"))) TlsCallback teb_tls_callback_ptr = teb_tls_callback;

const TlsDirectory _tls_used = {(u64)&_tls_start, (u64)&_tls_end, (u64)&_tls_index, (u64)(&__xl_a + 1), 0, 0};

/* --- Tests ------------------------------------------------------------------------------------ */
static u64 gs_qword(u32 offset) {
    u64 v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((u64)offset));
    return v;
}

/* The TEB: self pointer, PEB with the image base, ClientId.UniqueThread. */
EXPORT NOINLINE u64 t_teb_fields(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    const u64 self = gs_qword(0x30);
    const u64 peb = gs_qword(0x60);
    if (self == 0 || *(const u64*)(self + 0x30) != self) {
        return 1;
    }
    if (peb == 0 || *(const u64*)(peb + 0x10) != (u64)&__ImageBase) {
        return 2;
    }
    if ((u32)gs_qword(0x48) != GetCurrentThreadId()) {
        return 3;
    }
    return 0x600D;
}

__declspec(thread) u64 tls_counter = 5;
__declspec(thread) u64 tls_constant = 0x7E57;

/* Static TLS: the template value arrives in the thread's block, and writes stick. */
EXPORT NOINLINE u64 t_static_tls(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    if (tls_constant != 0x7E57) {
        return 1;
    }
    const u64 v = tls_counter;
    tls_counter = v + (a | 1);
    if (tls_counter != v + (a | 1)) {
        return 2;
    }
    return 0x600D;
}

/* The TLS callback ran once, at process attach, with the image base. */
EXPORT NOINLINE u64 t_tls_callback(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return g_attach_count == 1 && g_attach_base == (u64)&__ImageBase ? 0x600D : 1;
}

/* TlsAlloc / TlsSetValue / TlsGetValue / TlsFree, and the last error they set. */
EXPORT NOINLINE u64 t_tls_slots(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u32 i = TlsAlloc();
    if (i == 0xFFFFFFFFu) {
        return 1;
    }
    SetLastError(5);
    if (!TlsSetValue(i, (void*)a) || TlsGetValue(i) != (void*)a || GetLastError() != 0) {
        return 2;
    }
    if (!TlsFree(i) || TlsFree(i) || GetLastError() != 87) {
        return 3;
    }
    return 0x600D;
}

static volatile u64 g_fls_seen;
EXPORT void teb_fls_callback(void* value) { g_fls_seen = (u64)value; }

/* FlsFree runs the slot's callback with the thread's value. */
EXPORT NOINLINE u64 t_fls(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    const u64 value = a | 1;
    g_fls_seen = 0;
    const u32 i = FlsAlloc(teb_fls_callback);
    if (i == 0xFFFFFFFFu || !FlsSetValue(i, (void*)value) || FlsGetValue(i) != (void*)value) {
        return 1;
    }
    if (!FlsFree(i) || g_fls_seen != value) {
        return 2;
    }
    return 0x600D;
}

EXPORT NOINLINE u64 t_last_error(u64 a, u64 b, u64 c, u64 d) {
    (void)b;
    (void)c;
    (void)d;
    SetLastError((u32)a);
    return GetLastError() == (u32)a ? 0x600D : 1;
}

/* `_tls_used` and the callback pointer are reached by no code: keep them. */
__asm__(".section .drectve,\"yn\"\n"
        "  .ascii \" -include:_tls_used -include:teb_tls_callback_ptr\"\n"
        ".text\n");
