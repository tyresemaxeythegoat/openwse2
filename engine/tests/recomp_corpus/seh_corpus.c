/* R3 corpus: Win64 structured exception handling through the runtime (`docs/RECOMP_PLAN.md`
 * "R3 design", `wse/recomp/seh.h`).
 *
 * Our own code. The `t_*` exports raise exceptions with KERNEL32!RaiseException and catch them
 * with language handlers written here in C, attached to assembly functions with `.seh_handler`.
 * They cannot run natively on the Linux CI runners (no KERNEL32), so they are self-checking: each
 * returns 0x600D only when the exception went exactly as Windows would take it. The import library
 * comes from `kernel32.def` (llvm-dlltool). Handlers, the consolidation callback and the asm
 * functions are exported so function discovery finds them (retail code finds them in the PDB).
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define EXPORT __declspec(dllexport)
#define NOINLINE __declspec(noinline)

typedef struct {
    u32 code;
    u32 flags;
    u64 record;
    u64 address;
    u32 nparams;
    u32 pad;
    u64 info[15];
} ExRecord;

typedef struct {
    u64 control_pc, image_base, function_entry, establisher, target_ip, context, language_handler, handler_data,
        history;
    u32 scope, fill;
} DispatcherContext;

__declspec(dllimport) void RaiseException(u32 code, u32 flags, u32 nargs, const u64* args);
__declspec(dllimport) void RtlUnwindEx(void* frame, void* ip, ExRecord* rec, void* ret, void* ctx, void* history);
__declspec(dllimport) void RtlCaptureContext(void* ctx);
__declspec(dllimport) void* RtlLookupFunctionEntry(u64 pc, u64* base, void* history);
__declspec(dllimport) void* RtlVirtualUnwind(u32 type, u64 base, u64 pc, void* entry, void* ctx, void** data,
                                             u64* frame, void* pointers);
extern char __ImageBase;

#define UNWINDING 2u
#define CONTINUE_EXECUTION 0u
#define CONTINUE_SEARCH 1u

static volatile u64 g_terminations;

EXPORT NOINLINE void seh_raise(u32 code) { RaiseException(code, 0, 0, 0); }

/* MSVC-style unwind consolidation: the "catch block" returns where to continue. */
EXPORT NOINLINE u64 seh_consolidate(ExRecord* rec) { return rec->info[1]; }

/* Handler data: two image-relative continuation addresses in the protected function. */
EXPORT u32 seh_test_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    const u32* data = (const u32*)dc->handler_data;
    if (rec->flags & UNWINDING) {
        return CONTINUE_SEARCH;
    }
    switch (rec->code) {
    case 0xE0000001u:
        RtlUnwindEx(frame, (void*)(dc->image_base + data[0]), rec, (void*)0x600D, ctx, 0);
        return CONTINUE_SEARCH; /* not reached */
    case 0xE0000002u: {
        ExRecord r; /* field by field: a struct copy would call memcpy, and there is no CRT */
        r.flags = rec->flags;
        r.record = 0;
        r.address = rec->address;
        r.pad = 0;
        r.code = 0x80000029u; /* STATUS_UNWIND_CONSOLIDATE */
        r.nparams = 2;
        r.info[0] = (u64)&seh_consolidate;
        r.info[1] = dc->image_base + data[1];
        RtlUnwindEx(frame, 0, &r, 0, ctx, 0);
        return CONTINUE_SEARCH; /* not reached */
    }
    case 0xE0000003u:
        return CONTINUE_EXECUTION;
    default:
        return CONTINUE_SEARCH;
    }
}

/* MSVC C++ exceptions, reduced to their shape (`find_eh_continuations` in `discover.h`): the
 * handler data is the RVA of a FuncInfo (magic 0x19930522); the handler unwinds with consolidation
 * and the callback runs the first catch funclet of the first try block with the establisher frame
 * in rdx, as `__CxxFrameHandler3` does through `_CallSettingFrame`. The funclet returns the
 * continuation, which no branch reaches. */
EXPORT NOINLINE u64 seh_call_catch(ExRecord* rec) {
    typedef u64 (*Funclet)(u64, u64);
    return ((Funclet)rec->info[1])(0, rec->info[2]);
}

EXPORT u32 seh_cxx_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    if ((rec->flags & UNWINDING) || rec->code != 0xE0000004u) {
        return CONTINUE_SEARCH;
    }
    const u8* base = (const u8*)dc->image_base;
    const u32* fi = (const u32*)(base + *(const u32*)dc->handler_data);
    if (fi[0] != 0x19930522u || fi[3] == 0) {
        return CONTINUE_SEARCH;
    }
    /* The frame's word at +40 is 1 while its catch block runs (MSVC keeps the frame's EH state):
     * an exception from inside the catch is not caught by the same try block. */
    if (*(const u64*)((const u8*)frame + 40) != 0) {
        return CONTINUE_SEARCH;
    }
    const u32* try0 = (const u32*)(base + fi[4]);   /* TryBlockMapEntry */
    const u32* catch0 = (const u32*)(base + try0[4]); /* HandlerType */
    ExRecord r;
    r.flags = rec->flags;
    r.record = 0;
    r.address = rec->address;
    r.pad = 0;
    r.code = 0x80000029u; /* STATUS_UNWIND_CONSOLIDATE */
    r.nparams = 3;
    r.info[0] = (u64)&seh_call_catch;
    r.info[1] = dc->image_base + catch0[3];
    r.info[2] = (u64)frame;
    RtlUnwindEx(frame, 0, &r, 0, ctx, 0);
    return CONTINUE_SEARCH; /* not reached */
}

#define NESTED_CALL 0x10u
#define COLLIDED_UNWIND 0x40u

/* Handler data: continuation RVA, n, then n (code, return value) pairs: an exception with one of
 * the codes unwinds to the continuation with that return value in rax. */
EXPORT u32 seh_code_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    const u32* data = (const u32*)dc->handler_data;
    if (rec->flags & UNWINDING) {
        return CONTINUE_SEARCH;
    }
    for (u32 i = 0; i < data[1]; ++i) {
        if (rec->code == data[2 + 2 * i]) {
            RtlUnwindEx(frame, (void*)(dc->image_base + data[0]), rec, (void*)(u64)data[3 + 2 * i], ctx, 0);
        }
    }
    return CONTINUE_SEARCH;
}

/* A nested exception: on 0xE0000005 the handler raises 0xE0000006 itself; that one must come back
 * to this frame flagged EXCEPTION_NESTED_CALL, and unwinds to the continuation. */
EXPORT u32 seh_nesting_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    const u32* data = (const u32*)dc->handler_data;
    if (rec->flags & UNWINDING) {
        return CONTINUE_SEARCH;
    }
    if (rec->code == 0xE0000005u) {
        seh_raise(0xE0000006u);
        return CONTINUE_SEARCH; /* not reached */
    }
    if (rec->code == 0xE0000006u) {
        const u64 value = (rec->flags & NESTED_CALL) ? 0x600D : 0xBAD5;
        RtlUnwindEx(frame, (void*)(dc->image_base + data[0]), rec, (void*)value, ctx, 0);
    }
    return CONTINUE_SEARCH;
}

/* A collided unwind: the first time an unwind passes this frame its termination handler raises
 * 0xE0000008; the second unwind collides with the first and calls this handler again. */
static volatile u64 g_collide_calls;
static volatile u64 g_collide_flags;
EXPORT u32 seh_collide_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    (void)frame;
    (void)ctx;
    (void)dc;
    if (!(rec->flags & UNWINDING)) {
        return CONTINUE_SEARCH;
    }
    ++g_collide_calls;
    g_collide_flags |= rec->flags;
    if (g_collide_calls == 1) {
        seh_raise(0xE0000008u);
    }
    return CONTINUE_SEARCH;
}
EXPORT NOINLINE void seh_collide_reset(void) {
    g_collide_calls = 0;
    g_collide_flags = 0;
}
EXPORT NOINLINE u64 seh_collide_check(u64 value) {
    return g_collide_calls == 2 && (g_collide_flags & COLLIDED_UNWIND) ? value : 0xBAD8;
}

/* A termination handler: counts how often an unwind passes through its frame. */
EXPORT u32 seh_count_handler(ExRecord* rec, void* frame, void* ctx, DispatcherContext* dc) {
    (void)frame;
    (void)ctx;
    (void)dc;
    if (rec->flags & UNWINDING) {
        ++g_terminations;
    }
    return CONTINUE_SEARCH;
}

/* RtlCaptureContext / RtlLookupFunctionEntry / RtlVirtualUnwind agree with the real frame. */
EXPORT NOINLINE u64 t_unwind_query(u64 a, u64 b, u64 c, u64 d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    __attribute__((aligned(16))) u8 ctx[0x4D0];
    RtlCaptureContext(ctx);
    u64 rip = *(u64*)(ctx + 0xF8);
    u64 base = 0;
    void* entry = RtlLookupFunctionEntry(rip, &base, 0);
    if (entry == 0 || base != (u64)&__ImageBase) {
        return 1;
    }
    void* data = 0;
    u64 frame = 0;
    RtlVirtualUnwind(0, base, rip, entry, ctx, &data, &frame, 0);
    if (*(u64*)(ctx + 0xF8) != (u64)__builtin_return_address(0)) {
        return 2;
    }
    return 0x600D;
}

/* The protected functions, in assembly so their unwind info carries our handlers. A never-taken
 * branch makes each continuation an ordinary block for function discovery. */
__asm__(
    ".text\n"

    /* An exception raised two calls down, caught here: RtlUnwindEx resumes at the continuation
     * with rax = 0x600D. */
    ".globl t_seh_unwind\n"
    ".def t_seh_unwind; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_unwind\n"
    "t_seh_unwind:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000001, %ecx\n"
    "  callq seh_raise\n"
    "  testl %eax, %eax\n"
    "  jnz t_seh_unwind_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_seh_unwind_out\n"
    "t_seh_unwind_cont:\n"
    "t_seh_unwind_out:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_test_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_seh_unwind_cont@IMGREL\n"
    "  .long t_seh_unwind_cont@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* Unwind consolidation, as MSVC catch blocks run: the callback returns the continuation. */
    ".globl t_seh_consolidate\n"
    ".def t_seh_consolidate; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_consolidate\n"
    "t_seh_consolidate:\n"
    "  subq $40, %rsp\n"
    "  .seh_stackalloc 40\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000002, %ecx\n"
    "  callq seh_raise\n"
    "  testl %eax, %eax\n"
    "  jnz t_seh_consolidate_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_seh_consolidate_out\n"
    "t_seh_consolidate_cont:\n"
    "  movl $0x600D, %eax\n"
    "t_seh_consolidate_out:\n"
    "  addq $40, %rsp\n"
    "  retq\n"
    "  .seh_handler seh_test_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_seh_consolidate_cont@IMGREL\n"
    "  .long t_seh_consolidate_cont@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* The handler says "continue execution": RaiseException returns normally. */
    ".globl t_seh_continue\n"
    ".def t_seh_continue; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_continue\n"
    "t_seh_continue:\n"
    "  subq $40, %rsp\n"
    "  .seh_stackalloc 40\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000003, %ecx\n"
    "  callq seh_raise\n"
    "  movl $0x600D, %eax\n"
    "  addq $40, %rsp\n"
    "  retq\n"
    "  .seh_handler seh_test_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long 0\n"
    "  .long 0\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* A middle frame with a termination handler: the unwind from seh_raise to t_seh_nested must
     * call it exactly once. */
    ".globl seh_middle\n"
    ".def seh_middle; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_middle\n"
    "seh_middle:\n"
    "  subq $40, %rsp\n"
    "  .seh_stackalloc 40\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000001, %ecx\n"
    "  callq seh_raise\n"
    /* Not an epilog yet: Windows calls no handler for a frame whose pc is in its epilog. */
    "  movl $1, %eax\n"
    "  addq $40, %rsp\n"
    "  retq\n"
    "  .seh_handler seh_count_handler, @unwind\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".globl t_seh_nested\n"
    ".def t_seh_nested; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_nested\n"
    "t_seh_nested:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movq g_terminations(%rip), %rbx\n"
    "  callq seh_middle\n"
    "  testl %eax, %eax\n"
    "  jnz t_seh_nested_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_seh_nested_out\n"
    "t_seh_nested_cont:\n"
    "  movq g_terminations(%rip), %rcx\n"
    "  subq %rbx, %rcx\n"
    "  cmpq $1, %rcx\n"
    "  movl $0x600D, %eax\n"
    "  movl $0xBAD1, %ecx\n"
    "  cmovnel %ecx, %eax\n"
    "t_seh_nested_out:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_test_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_seh_nested_cont@IMGREL\n"
    "  .long t_seh_nested_cont@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* A C++-shaped catch: the funclet writes the frame's local through the establisher frame and
     * returns the continuation, which follows an unconditional jump (no branch reaches it). */
    ".globl t_cxx_catch\n"
    ".def t_cxx_catch; .scl 2; .type 32; .endef\n"
    ".seh_proc t_cxx_catch\n"
    "t_cxx_catch:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $48, %rsp\n"
    "  .seh_stackalloc 48\n"
    "  .seh_endprologue\n"
    "  movq $0xBAD2, 32(%rsp)\n"
    "  movq $0, 40(%rsp)\n"
    "  movl $0xE0000004, %ecx\n"
    "  callq seh_raise\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_cxx_catch_out\n"
    "t_cxx_catch_cont:\n"
    "  movq 32(%rsp), %rax\n"
    "t_cxx_catch_out:\n"
    "  addq $48, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_cxx_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_cxx_funcinfo@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".globl seh_cxx_funclet\n"
    ".def seh_cxx_funclet; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_cxx_funclet\n"
    "seh_cxx_funclet:\n"
    "  movq %rdx, 16(%rsp)\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movq %rdx, %rbp\n"
    "  movq $0x600D, 32(%rbp)\n"
    "  leaq t_cxx_catch_cont(%rip), %rax\n"
    "  addq $32, %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    "  .seh_endproc\n"

    /* FuncInfo (magic, maxState, dispUnwindMap, nTryBlocks, dispTryBlockMap, ...), one try block
     * (tryLow, tryHigh, catchHigh, nCatches, dispHandlerArray), one catch-all handler
     * (adjectives, dispType, dispCatchObj, dispOfHandler, dispFrame). */
    ".section .rdata,\"dr\"\n"
    ".p2align 2\n"
    "t_cxx_funcinfo:\n"
    "  .long 0x19930522, 1, 0, 1, t_cxx_trymap@IMGREL, 0, 0, 0, 0, 0\n"
    "t_cxx_trymap:\n"
    "  .long 0, 0, 1, 1, t_cxx_catches@IMGREL\n"
    "t_cxx_catches:\n"
    "  .long 0x40, 0, 0, seh_cxx_funclet@IMGREL, 0\n"

    /* A rethrow from a catch block: seh_rethrow_inner's catch funclet marks its frame and raises
     * again; the walk continues from the catch into seh_rethrow_inner's frame (catch in progress:
     * not caught there) and on to t_cxx_rethrow, whose own catch runs and continues. */
    ".text\n"
    ".globl t_cxx_rethrow\n"
    ".def t_cxx_rethrow; .scl 2; .type 32; .endef\n"
    ".seh_proc t_cxx_rethrow\n"
    "t_cxx_rethrow:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $48, %rsp\n"
    "  .seh_stackalloc 48\n"
    "  .seh_endprologue\n"
    "  movq $0xBAD3, 32(%rsp)\n"
    "  movq $0, 40(%rsp)\n"
    "  callq seh_rethrow_inner\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_cxx_rethrow_out\n"
    "t_cxx_rethrow_cont:\n"
    "  movq 32(%rsp), %rax\n"
    "t_cxx_rethrow_out:\n"
    "  addq $48, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_cxx_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_cxx_rethrow_funcinfo@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".globl seh_rethrow_outer_funclet\n"
    ".def seh_rethrow_outer_funclet; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_rethrow_outer_funclet\n"
    "seh_rethrow_outer_funclet:\n"
    "  movq %rdx, 16(%rsp)\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movq %rdx, %rbp\n"
    "  movq $0x600D, 32(%rbp)\n"
    "  leaq t_cxx_rethrow_cont(%rip), %rax\n"
    "  addq $32, %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    "  .seh_endproc\n"

    ".globl seh_rethrow_inner\n"
    ".def seh_rethrow_inner; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_rethrow_inner\n"
    "seh_rethrow_inner:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $48, %rsp\n"
    "  .seh_stackalloc 48\n"
    "  .seh_endprologue\n"
    "  movq $0, 40(%rsp)\n"
    "  movl $0xE0000004, %ecx\n"
    "  callq seh_raise\n"
    "  movl $1, %eax\n"
    "  addq $48, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_cxx_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long seh_rethrow_inner_funcinfo@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".globl seh_rethrow_funclet\n"
    ".def seh_rethrow_funclet; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_rethrow_funclet\n"
    "seh_rethrow_funclet:\n"
    "  movq %rdx, 16(%rsp)\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movq %rdx, %rbp\n"
    "  movq $1, 40(%rbp)\n"
    "  movl $0xE0000004, %ecx\n"
    "  callq seh_raise\n"
    "  int3\n"
    "  .seh_endproc\n"

    /* A nested exception raised by a handler during phase 1. */
    ".globl t_seh_nested_raise\n"
    ".def t_seh_nested_raise; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_nested_raise\n"
    "t_seh_nested_raise:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000005, %ecx\n"
    "  callq seh_raise\n"
    "  testl %eax, %eax\n"
    "  jnz t_seh_nested_raise_cont\n"
    "  movl $0xBAD, %eax\n"
    "t_seh_nested_raise_cont:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_nesting_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_seh_nested_raise_cont@IMGREL\n"
    "  .text\n"
    "  .seh_endproc\n"

    /* A collided unwind: 0xE0000007 is caught here; the unwind's termination handler in
     * seh_collide_middle raises 0xE0000008, also caught here; the second unwind collides with the
     * first and finishes with its own return value. */
    ".globl t_seh_collided\n"
    ".def t_seh_collided; .scl 2; .type 32; .endef\n"
    ".seh_proc t_seh_collided\n"
    "t_seh_collided:\n"
    "  pushq %rbx\n"
    "  .seh_pushreg %rbx\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_endprologue\n"
    "  callq seh_collide_reset\n"
    "  callq seh_collide_middle\n"
    "  testl %eax, %eax\n"
    "  jnz t_seh_collided_cont\n"
    "  movl $0xBAD, %eax\n"
    "  jmp t_seh_collided_out\n"
    "t_seh_collided_cont:\n"
    "  movq %rax, %rcx\n"
    "  callq seh_collide_check\n"
    "t_seh_collided_out:\n"
    "  addq $32, %rsp\n"
    "  popq %rbx\n"
    "  retq\n"
    "  .seh_handler seh_code_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long t_seh_collided_cont@IMGREL\n"
    "  .long 2\n"
    "  .long 0xE0000007, 0xBAD7\n"
    "  .long 0xE0000008, 0x600D\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".globl seh_collide_middle\n"
    ".def seh_collide_middle; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_collide_middle\n"
    "seh_collide_middle:\n"
    "  subq $40, %rsp\n"
    "  .seh_stackalloc 40\n"
    "  .seh_endprologue\n"
    "  movl $0xE0000007, %ecx\n"
    "  callq seh_raise\n"
    "  movl $1, %eax\n"
    "  addq $40, %rsp\n"
    "  retq\n"
    "  .seh_handler seh_collide_handler, @unwind\n"
    "  .text\n"
    "  .seh_endproc\n"

    ".section .rdata,\"dr\"\n"
    ".p2align 2\n"
    "t_cxx_rethrow_funcinfo:\n"
    "  .long 0x19930522, 1, 0, 1, t_cxx_rethrow_trymap@IMGREL, 0, 0, 0, 0, 0\n"
    "t_cxx_rethrow_trymap:\n"
    "  .long 0, 0, 1, 1, t_cxx_rethrow_catches@IMGREL\n"
    "t_cxx_rethrow_catches:\n"
    "  .long 0x40, 0, 0, seh_rethrow_outer_funclet@IMGREL, 0\n"
    "seh_rethrow_inner_funcinfo:\n"
    "  .long 0x19930522, 1, 0, 1, seh_rethrow_inner_trymap@IMGREL, 0, 0, 0, 0, 0\n"
    "seh_rethrow_inner_trymap:\n"
    "  .long 0, 0, 1, 1, seh_rethrow_inner_catches@IMGREL\n"
    "seh_rethrow_inner_catches:\n"
    "  .long 0x40, 0, 0, seh_rethrow_funclet@IMGREL, 0\n"

    ".section .drectve,\"yn\"\n"
    "  .ascii \" -export:t_cxx_rethrow -export:seh_rethrow_outer_funclet -export:seh_rethrow_inner\"\n"
    "  .ascii \" -export:seh_rethrow_funclet -export:t_seh_nested_raise -export:t_seh_collided\"\n"
    "  .ascii \" -export:seh_collide_middle\"\n"
    "  .ascii \" -export:t_seh_unwind -export:t_seh_consolidate -export:t_seh_continue\"\n"
    "  .ascii \" -export:t_seh_nested -export:seh_middle -export:t_cxx_catch -export:seh_cxx_funclet\"\n"
    ".text\n");
