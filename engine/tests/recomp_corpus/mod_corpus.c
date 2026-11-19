/* R4 corpus: modules and resources (`docs/RECOMP_PLAN.md` "R4 design", slice 2;
 * `wse/recomp/shim_modules.h`), and the SEH entry points `lua5164` adds (`seh.h`: RtlUnwind,
 * RtlRestoreContext).
 *
 * Our own code. The exe imports `corpus_dll.dll` (`dll_corpus.c`), so every `t_mod_*` runs across
 * two translated modules: handles, exports (by name, by ordinal, a forwarder), file names, this
 * exe's resources (`corpus.rc`), a callback from the DLL into the exe, and an exception raised in
 * the DLL that unwinds through frames of both modules. Self-checking (0x600D means correct, any
 * other value names the failed check); facts that differ between machines are not tested (paths
 * only by their file name, no language-dependent resource choice).
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)
#define IMPORT __declspec(dllimport)

typedef struct {
    u32 code;
    u32 flags;
    u64 record;
    u64 address;
    u32 nparams;
    u32 pad;
    u64 info[15];
} ModExRecord;

typedef struct {
    u64 control_pc, image_base, function_entry, establisher, target_ip, context, language_handler, handler_data,
        history;
    u32 scope, fill;
} ModDispatcherContext;

IMPORT void* GetModuleHandleA(const char* name);
IMPORT void* GetModuleHandleW(const u16* name);
IMPORT int GetModuleHandleExW(u32 flags, const u16* name, void** module);
IMPORT void* GetProcAddress(void* module, const char* name);
IMPORT void* LoadLibraryA(const char* name);
IMPORT void* LoadLibraryExW(const u16* name, void* file, u32 flags);
IMPORT int FreeLibrary(void* module);
IMPORT u32 GetModuleFileNameA(void* module, char* buf, u32 size);
IMPORT u32 GetModuleFileNameW(void* module, u16* buf, u32 size);
IMPORT void* RtlPcToFileHeader(void* pc, void** base);
IMPORT void* FindResourceA(void* module, const char* name, const char* type);
IMPORT void* FindResourceW(void* module, const u16* name, const u16* type);
IMPORT void* FindResourceExW(void* module, const u16* type, const u16* name, u16 language);
IMPORT void* LoadResource(void* module, void* res);
IMPORT void* LockResource(void* data);
IMPORT u32 SizeofResource(void* module, void* res);
IMPORT u32 GetLastError(void);
IMPORT void SetLastError(u32 e);
IMPORT u32 GetCurrentThreadId(void);
IMPORT void RtlCaptureContext(void* ctx);
IMPORT void RtlRestoreContext(void* ctx, ModExRecord* rec);
IMPORT void RtlUnwind(void* frame, void* ip, ModExRecord* rec, void* ret);

IMPORT u64 d_add(u64 a, u64 b);
IMPORT u64 d_image_base(u64 a, u64 b, u64 c, u64 d);
IMPORT u64 dt_attach_info(void);
IMPORT u64 dt_call(u64 (*fn)(u64), u64 x);
IMPORT void dt_raise(u32 code);

extern char __ImageBase;

#define MAKEINT(n) ((const char*)(u64)(n))
#define MAKEINTW(n) ((const u16*)(u64)(n))
#define RT_RCDATA 10

static u32 str_len(const char* s) {
    u32 n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
/* Case-insensitive suffix test (paths differ between machines; only the file name is checked). */
static int ends_with(const char* s, u32 n, const char* suffix) {
    const u32 k = str_len(suffix);
    if (n < k) {
        return 0;
    }
    for (u32 i = 0; i < k; ++i) {
        if (lower(s[n - k + i]) != lower(suffix[i])) {
            return 0;
        }
    }
    return 1;
}
static int ends_with_w(const u16* s, u32 n, const char* suffix) {
    const u32 k = str_len(suffix);
    if (n < k) {
        return 0;
    }
    for (u32 i = 0; i < k; ++i) {
        if (s[n - k + i] > 0x7F || lower(s[n - k + i]) != lower(suffix[i])) {
            return 0;
        }
    }
    return 1;
}

/* Reached only through pointers from the tests below (exported so function discovery finds them;
 * the differential skips the `mod_` prefix). */
EXPORT NOINLINE u64 mod_answer(void) { return 0x2A; }
EXPORT NOINLINE u64 mod_twice(u64 x) { return 2 * x; }
EXPORT NOINLINE u64 mod_raise_cb(u64 x) {
    dt_raise(0xE000000Au);
    return x;
}

/* Module handles: by name (case, no extension), by address, RtlPcToFileHeader, LoadLibrary. */
EXPORT NOINLINE u64 t_mod_handles(void) {
    void* self = 0;
    if (!GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, (const u16*)(void*)&t_mod_handles, &self) ||
        self != (void*)&__ImageBase) {
        return 1;
    }
    void* d = GetModuleHandleA("corpus_dll.dll");
    if (d == 0 || (u64)d != d_image_base(0, 0, 0, 0)) {
        return 2;
    }
    if (GetModuleHandleW(L"CORPUS_DLL") != d || GetModuleHandleA("Corpus_Dll") != d) {
        return 3;
    }
    void* b = 0;
    if (RtlPcToFileHeader((void*)&d_add, &b) != d || b != d) {
        return 4;
    }
    if (RtlPcToFileHeader((void*)&t_mod_handles, &b) != (void*)&__ImageBase) {
        return 5;
    }
    SetLastError(0);
    if (GetModuleHandleA("no_such_module.dll") != 0 || GetLastError() != 126) {
        return 6;
    }
    if (LoadLibraryA("corpus_dll.dll") != d || !FreeLibrary(d)) {
        return 7;
    }
    void* k = GetModuleHandleA("kernel32.dll");
    if (k == 0 || GetModuleHandleW(L"KERNEL32") != k || LoadLibraryExW(L"kernel32.dll", 0, 0) != k) {
        return 8;
    }
    FreeLibrary(k);
    void* h = 0;
    if (!GetModuleHandleExW(4 /* FROM_ADDRESS */, (const u16*)(void*)((const char*)(void*)&d_add + 1), &h) ||
        h != d) {
        return 9;
    }
    FreeLibrary(d);
    /* DllMain ran once, with DLL_PROCESS_ATTACH and this DLL's handle (lpReserved differs between
     * a static and a dynamic load: the harness checks it). */
    if ((dt_attach_info() & 0xFFF) != 0x111) {
        return 10;
    }
    return 0x600D;
}

/* GetProcAddress: by name (case-sensitive), by ordinal, a forwarder, KERNEL32, the exe itself. */
typedef u64 (*Fn2)(u64, u64);
typedef u64 (*Fn0)(void);
typedef u32 (*FnU32)(void);
EXPORT NOINLINE u64 t_mod_procs(void) {
    void* d = GetModuleHandleA("corpus_dll.dll");
    void* k = GetModuleHandleA("kernel32.dll");
    Fn2 add = (Fn2)GetProcAddress(d, "d_add");
    if (add == 0 || (void*)add != (void*)&d_add || add(40, 2) != 42) {
        return 1;
    }
    Fn0 by_ordinal = (Fn0)GetProcAddress(d, MAKEINT(42));
    if (by_ordinal == 0 || by_ordinal() != 0x42) {
        return 2;
    }
    SetLastError(0);
    if (GetProcAddress(d, "D_ADD") != 0 || GetLastError() != 127) {
        return 3;
    }
    if (GetProcAddress(d, MAKEINT(41)) != 0) {
        return 4;
    }
    FnU32 tid = (FnU32)GetProcAddress(k, "GetCurrentThreadId");
    if (tid == 0 || tid() != GetCurrentThreadId()) {
        return 5;
    }
    if (GetProcAddress(d, "d_fwd") != (void*)tid) {
        return 6;
    }
    SetLastError(0);
    if (GetProcAddress(k, "WseNoSuchExport") != 0 || GetLastError() != 127) {
        return 7;
    }
    Fn0 answer = (Fn0)GetProcAddress((void*)&__ImageBase, "mod_answer");
    if (answer == 0 || answer() != 0x2A) {
        return 8;
    }
    return 0x600D;
}

/* GetModuleFileName: the file name, the length, a short buffer (truncated, ERROR_INSUFFICIENT_BUFFER). */
EXPORT NOINLINE u64 t_mod_filename(void) {
    char a[300];
    u16 w[300];
    void* d = GetModuleHandleA("corpus_dll.dll");
    u32 n = GetModuleFileNameA(d, a, 300);
    if (n == 0 || n >= 300 || a[n] != 0 || str_len(a) != n || !ends_with(a, n, "\\corpus_dll.dll")) {
        return 1;
    }
    SetLastError(0);
    if (GetModuleFileNameA(d, a, 5) != 5 || a[4] != 0 || GetLastError() != 122) {
        return 2;
    }
    const u32 nw = GetModuleFileNameW(d, w, 300);
    if (nw != n || w[nw] != 0 || !ends_with_w(w, nw, "\\corpus_dll.dll")) {
        return 3;
    }
    n = GetModuleFileNameA((void*)&__ImageBase, a, 300);
    if (n == 0 || !ends_with(a, n, "\\corpus.exe")) {
        return 4;
    }
    n = GetModuleFileNameA(GetModuleHandleA("kernel32.dll"), a, 300);
    if (n == 0 || !ends_with(a, n, "\\kernel32.dll")) {
        return 5;
    }
    return 0x600D;
}

/* This exe's resources (`corpus.rc`). */
EXPORT NOINLINE u64 t_mod_resources(void) {
    void* self = (void*)&__ImageBase;
    void* r = FindResourceA(self, MAKEINT(104), MAKEINT(256));
    if (r == 0 || SizeofResource(self, r) != 10) {
        return 1;
    }
    const char* p = (const char*)LockResource(LoadResource(self, r));
    if (p == 0 || p[0] != 'W' || p[3] != '2' || p[9] != 's') {
        return 2;
    }
    if (FindResourceA(self, "#104", "#256") != r) {
        return 3;
    }
    void* named = FindResourceW(self, L"wse_named", MAKEINTW(RT_RCDATA));
    if (named == 0 || SizeofResource(self, named) != 5 || ((const char*)LockResource(LoadResource(self, named)))[0] != 'n') {
        return 4;
    }
    void* de = FindResourceExW(self, MAKEINTW(RT_RCDATA), MAKEINTW(200), 0x407);
    void* en = FindResourceExW(self, MAKEINTW(RT_RCDATA), MAKEINTW(200), 0x409);
    if (de == 0 || en == 0 || de == en) {
        return 5;
    }
    if (((const char*)LockResource(LoadResource(self, de)))[0] != 'd' ||
        ((const char*)LockResource(LoadResource(self, en)))[0] != 'e') {
        return 6;
    }
    SetLastError(0);
    if (FindResourceA(self, MAKEINT(999), MAKEINT(RT_RCDATA)) != 0 || GetLastError() != 1814) {
        return 7;
    }
    SetLastError(0);
    if (FindResourceA(self, MAKEINT(104), MAKEINT(999)) != 0 || GetLastError() != 1813) {
        return 8;
    }
    SetLastError(0);
    if (FindResourceExW(self, MAKEINTW(RT_RCDATA), MAKEINTW(200), 0x40C) != 0 || GetLastError() != 1815) {
        return 9;
    }
    SetLastError(0);
    if (FindResourceA(GetModuleHandleA("corpus_dll.dll"), MAKEINT(1), MAKEINT(RT_RCDATA)) != 0 ||
        GetLastError() != 1812) {
        return 10;
    }
    return 0x600D;
}

/* The DLL calls back into the exe through a pointer. */
EXPORT NOINLINE u64 t_mod_cross(u64 a) {
    const u64 x = a & 0xFFFF;
    return dt_call(&mod_twice, x) == 2 * x + 1 && d_add(x, 7) == x + 7 ? 0x600D : 1;
}

/* RtlUnwind (no context record) from a language handler: like seh_code_handler, one code. */
EXPORT u32 mod_unwind_handler(ModExRecord* rec, void* frame, void* ctx, ModDispatcherContext* dc) {
    (void)ctx;
    const u32* data = (const u32*)dc->handler_data;
    if ((rec->flags & 2u) == 0 && rec->code == data[1]) {
        RtlUnwind(frame, (void*)(dc->image_base + data[0]), rec, (void*)(u64)data[2]);
    }
    return 1; /* CONTINUE_SEARCH */
}

/* RtlRestoreContext with STATUS_LONGJUMP: the registers, rsp and rip come from a _JUMP_BUFFER built
 * from the context `a` captured in the caller (what MSVC's setjmp records). */
volatile u64 mod_restore_count;
static u64 rd64(const u8* p, u32 off) { return *(const u64*)(const void*)(p + off); }
EXPORT NOINLINE void mod_longjump(const u8* a) {
    __attribute__((aligned(16))) u8 ctx[0x4D0];
    __attribute__((aligned(16))) volatile u64 jb[32];
    static const u32 kRegs[9] = {0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xD8, 0xE0, 0xE8, 0xF0}; /* rbx rsp rbp rsi rdi r12-r15 */
    jb[0] = 0;
    for (u32 i = 0; i < 9; ++i) {
        jb[1 + i] = rd64(a, kRegs[i]);
    }
    jb[10] = rd64(a, 0xF8);                                       /* Rip */
    jb[11] = (u64)*(const u32*)(const void*)(a + 0x34) | (0x27FULL << 32); /* MxCsr, FpCsr */
    for (u32 i = 0; i < 20; ++i) {
        jb[12 + i] = rd64(a, 0x200 + 8 * i); /* Xmm6..Xmm15 */
    }
    ModExRecord rec;
    rec.code = 0x80000026u; /* STATUS_LONGJUMP */
    rec.flags = 0;
    rec.record = 0;
    rec.address = 0;
    rec.nparams = 1;
    rec.pad = 0;
    rec.info[0] = (u64)(void*)jb;
    RtlCaptureContext(ctx);
    RtlRestoreContext(ctx, &rec);
}

/* Protected functions in assembly (their unwind info names a handler, so their frames can resume:
 * `ImageInfo::resumable`). */
__asm__(
    ".text\n"

    /* An exception raised in the DLL by a callback the DLL called: the unwind crosses
     * t_mod_seh (exe) <- dt_call (DLL) <- mod_raise_cb (exe) <- dt_raise (DLL). */
    ".globl t_mod_seh\n"
    ".def t_mod_seh; .scl 2; .type 32; .endef\n"
    ".seh_proc t_mod_seh\n"
    "t_mod_seh:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  leaq mod_raise_cb(%rip), %rcx\n"
    "  movl $7, %edx\n"
    "  callq *__imp_dt_call(%rip)\n"
    "  cmpq $0x600D, %rax\n"
    "  je t_mod_seh_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_mod_seh_out\n"
    "t_mod_seh_cont:\n"
    "t_mod_seh_out:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_code_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_mod_seh_cont@IMGREL\n"
    "  .long 1\n"
    "  .long 0xE000000A, 0x600D\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* RtlUnwind from the handler resumes at the continuation with rax = 0x600D. */
    ".globl t_rtl_unwind\n"
    ".def t_rtl_unwind; .scl 2; .type 32; .endef\n"
    ".seh_proc t_rtl_unwind\n"
    "t_rtl_unwind:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movl $0xE000000B, %ecx\n"
    "  callq seh_raise\n"
    "  cmpq $0x600D, %rax\n"
    "  je t_rtl_unwind_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_rtl_unwind_out\n"
    "t_rtl_unwind_cont:\n"
    "t_rtl_unwind_out:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler mod_unwind_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_rtl_unwind_cont@IMGREL\n"
    "  .long 0xE000000B, 0x600D\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* RtlCaptureContext, then RtlRestoreContext(ctx, NULL) resumes after the capture once more. */
    ".globl t_restore_context\n"
    ".def t_restore_context; .scl 2; .type 32; .endef\n"
    ".seh_proc t_restore_context\n"
    "t_restore_context:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $0x500, %rsp\n"
    "  .seh_stackalloc 0x500\n"
    "  .seh_endprologue\n"
    "  movq $0, mod_restore_count(%rip)\n"
    "  movl $0x1234, %ebx\n"
    "  leaq 0x20(%rsp), %rcx\n"
    "  callq *__imp_RtlCaptureContext(%rip)\n"
    "t_restore_context_resume:\n" /* the call's return address: no branch reaches it */
    "  incq mod_restore_count(%rip)\n"
    "  cmpq $2, mod_restore_count(%rip)\n"
    "  jae t_restore_context_done\n"
    "  movl $0x5678, %ebx\n"
    "  leaq 0x20(%rsp), %rcx\n"
    "  xorl %edx, %edx\n"
    "  callq *__imp_RtlRestoreContext(%rip)\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_restore_context_out\n"
    "t_restore_context_done:\n"
    "  movl $0xBAD2, %eax\n"
    "  cmpl $0x1234, %ebx\n" /* rbx came back from the captured context */
    "  jne t_restore_context_out\n"
    "  movl $0x600D, %eax\n"
    "t_restore_context_out:\n"
    "  addq $0x500, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_count_handler, @unwind\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* A STATUS_LONGJUMP restore from a deeper frame (`mod_longjump`) resumes here. */
    ".globl t_restore_longjump\n"
    ".def t_restore_longjump; .scl 2; .type 32; .endef\n"
    ".seh_proc t_restore_longjump\n"
    "t_restore_longjump:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $0x500, %rsp\n"
    "  .seh_stackalloc 0x500\n"
    "  .seh_endprologue\n"
    "  movq $0, mod_restore_count(%rip)\n"
    "  movl $0x4321, %ebx\n"
    "  leaq 0x20(%rsp), %rcx\n"
    "  callq *__imp_RtlCaptureContext(%rip)\n"
    "t_restore_longjump_resume:\n" /* the call's return address: no branch reaches it */
    "  incq mod_restore_count(%rip)\n"
    "  cmpq $2, mod_restore_count(%rip)\n"
    "  jae t_restore_longjump_done\n"
    "  movl $0x8765, %ebx\n"
    "  leaq 0x20(%rsp), %rcx\n"
    "  callq mod_longjump\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_restore_longjump_out\n"
    "t_restore_longjump_done:\n"
    "  movl $0xBAD2, %eax\n"
    "  cmpl $0x4321, %ebx\n" /* rbx came back from the jump buffer */
    "  jne t_restore_longjump_out\n"
    "  movl $0x600D, %eax\n"
    "t_restore_longjump_out:\n"
    "  addq $0x500, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_count_handler, @unwind\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".section .drectve,\"yn\"\n"
    "  .ascii \" -export:t_mod_seh -export:t_rtl_unwind -export:t_restore_context -export:t_restore_longjump\"\n"
    "  .ascii \" -export:o_dispatch\"\n"
    ".text\n");

/* An interpreter shaped like LuaJIT's (`find_offset_table_entries`): bytecode handlers inside one
 * .pdata function, reached only through a dispatch table the program fills at run time from 16-bit
 * offsets added to that function's start, each handler ending in `jmp [table + op * 8]`.
 * o_dispatch(n) runs the program {INC, LOOP} n times (n masked to 16 bits, at least 1) and returns
 * the count: up to 131,070 handler-to-handler jumps in one chain, so a translation whose jumps are
 * not tail calls runs out of host stack. t_offset_deep makes 46 full-length runs. */
u64 o_dispatch(u64 n);
u64 g_vm_table[4]; /* referenced from the assembly only */
__asm__(
    ".section .rdata,\"dr\"\n"
    "vm_program:\n"
    "  .byte 0, 1\n"
    ".p2align 1\n"
    "vm_offsets:\n"
    "  .short vm_inc - vm_begin, vm_loop - vm_begin, vm_halt - vm_begin\n"
    "  .short 0xFFFF\n"
    ".text\n"

    ".def vm_begin; .scl 3; .type 32; .endef\n"
    ".seh_proc vm_begin\n"
    "vm_begin:\n"
    "  .seh_endprologue\n"
    "vm_inc:\n"
    "  addq $1, %rax\n"
    "  movzbl (%rdx), %ecx\n"
    "  incq %rdx\n"
    "  jmpq *(%r8,%rcx,8)\n"
    "vm_loop:\n"
    "  subq $1, %r11\n"
    "  jz vm_halt\n"
    "  leaq vm_program(%rip), %rdx\n"
    "  movzbl (%rdx), %ecx\n"
    "  incq %rdx\n"
    "  jmpq *(%r8,%rcx,8)\n"
    "vm_halt:\n"
    "  retq\n"
    "  .seh_endproc\n"

    /* u64 o_dispatch(u64 n): fill the table (the LuaJIT pattern), then run. */
    ".globl o_dispatch\n"
    ".def o_dispatch; .scl 2; .type 32; .endef\n"
    ".seh_proc o_dispatch\n"
    "o_dispatch:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movq %rcx, %r11\n"
    "  andq $0xFFFF, %r11\n"
    "  orq $1, %r11\n"
    "  leaq g_vm_table(%rip), %r8\n"
    "  leaq vm_offsets(%rip), %r9\n"
    "  leaq vm_begin(%rip), %r10\n"
    "  xorl %ebx, %ebx\n"
    "o_dispatch_fill:\n"
    "  movzwl (%r9,%rbx,2), %ecx\n"
    "  addq %r10, %rcx\n"
    "  movq %rcx, (%r8,%rbx,8)\n"
    "  incq %rbx\n"
    "  cmpq $3, %rbx\n"
    "  jb o_dispatch_fill\n"
    "  xorl %eax, %eax\n"
    "  leaq vm_program(%rip), %rdx\n"
    "  callq vm_enter\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_endproc\n"

    /* The first dispatch (the handlers return to its caller through vm_halt). */
    ".def vm_enter; .scl 3; .type 32; .endef\n"
    "vm_enter:\n"
    "  movzbl (%rdx), %ecx\n"
    "  incq %rdx\n"
    "  jmpq *(%r8,%rcx,8)\n");

/* An MSVC jump table that no compare bounds (the CRT's memset indexes by size >> 4): read while its
 * entries land inside the function (`Discoverer::set_function_ranges`). o_unbounded_switch(x)
 * returns 10, 21, 32 or 43 for (x & 0x3F) >> 4. */
__asm__(
    ".globl o_unbounded_switch\n"
    ".def o_unbounded_switch; .scl 2; .type 32; .endef\n"
    ".seh_proc o_unbounded_switch\n"
    "o_unbounded_switch:\n"
    "  .seh_endprologue\n"
    "  andl $0x3F, %ecx\n"
    "  shrl $4, %ecx\n"
    "  leaq __ImageBase(%rip), %r10\n"
    "  movl ob_table@IMGREL(%r10,%rcx,4), %r11d\n"
    "  addq %r10, %r11\n"
    "  jmpq *%r11\n"
    "ob_c0:\n"
    "  movl $10, %eax\n"
    "  retq\n"
    "ob_c1:\n"
    "  movl $21, %eax\n"
    "  retq\n"
    "ob_c2:\n"
    "  movl $32, %eax\n"
    "  retq\n"
    "ob_c3:\n"
    "  movl $43, %eax\n"
    "  retq\n"
    "  .seh_endproc\n"
    ".section .rdata,\"dr\"\n"
    ".p2align 2\n"
    "ob_table:\n"
    "  .long ob_c0@IMGREL, ob_c1@IMGREL, ob_c2@IMGREL, ob_c3@IMGREL\n"
    "  .long 0\n"
    ".section .drectve,\"yn\"\n"
    "  .ascii \" -export:o_unbounded_switch\"\n"
    ".text\n");

/* 46 runs of 65,535 loops: 3,014,610 handler pairs in all. */
EXPORT NOINLINE u64 t_offset_deep(void) {
    u64 total = 0;
    for (int i = 0; i < 46; ++i) {
        total += o_dispatch(0xFFFF);  /* 46 x 65535 = 3,014,610 */
    }
    return total == 46ULL * 0xFFFF ? 0x600D : 1;
}
