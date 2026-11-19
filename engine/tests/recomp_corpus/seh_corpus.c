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

    ".section .drectve,\"yn\"\n"
    "  .ascii \" -export:t_seh_unwind -export:t_seh_consolidate -export:t_seh_continue\"\n"
    "  .ascii \" -export:t_seh_nested -export:seh_middle\"\n"
    ".text\n");
