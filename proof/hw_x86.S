/* Hardware trampoline for the flag/operand differential.
 *
 * void run_x86(void) executes the bytes at g_code_ptr with the GPRs and XMMs
 * in g_in/g_xmm_in. The buffer must end in `ret`; the call pushes and the ret
 * pops the return address on the guest stack in g_in[4].
 */
    .text
    .globl run_x86
run_x86:
    pushq %rbx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, g_host_rsp(%rip)

    movdqu %xmm6, g_host_xmm+0(%rip)
    movdqu %xmm7, g_host_xmm+16(%rip)
    movdqu %xmm8, g_host_xmm+32(%rip)
    movdqu %xmm9, g_host_xmm+48(%rip)
    movdqu %xmm10, g_host_xmm+64(%rip)
    movdqu %xmm11, g_host_xmm+80(%rip)
    movdqu %xmm12, g_host_xmm+96(%rip)
    movdqu %xmm13, g_host_xmm+112(%rip)
    movdqu %xmm14, g_host_xmm+128(%rip)
    movdqu %xmm15, g_host_xmm+144(%rip)

    movdqu g_xmm_in+0(%rip), %xmm0
    movdqu g_xmm_in+16(%rip), %xmm1
    movdqu g_xmm_in+32(%rip), %xmm2
    movdqu g_xmm_in+48(%rip), %xmm3
    movdqu g_xmm_in+64(%rip), %xmm4
    movdqu g_xmm_in+80(%rip), %xmm5
    movdqu g_xmm_in+96(%rip), %xmm6
    movdqu g_xmm_in+112(%rip), %xmm7
    movdqu g_xmm_in+128(%rip), %xmm8
    movdqu g_xmm_in+144(%rip), %xmm9
    movdqu g_xmm_in+160(%rip), %xmm10
    movdqu g_xmm_in+176(%rip), %xmm11
    movdqu g_xmm_in+192(%rip), %xmm12
    movdqu g_xmm_in+208(%rip), %xmm13
    movdqu g_xmm_in+224(%rip), %xmm14
    movdqu g_xmm_in+240(%rip), %xmm15

    movq g_in+0(%rip), %rax
    movq g_in+8(%rip), %rcx
    movq g_in+16(%rip), %rdx
    movq g_in+24(%rip), %rbx
    movq g_in+40(%rip), %rbp
    movq g_in+48(%rip), %rsi
    movq g_in+56(%rip), %rdi
    movq g_in+64(%rip), %r8
    movq g_in+72(%rip), %r9
    movq g_in+80(%rip), %r10
    movq g_in+88(%rip), %r11
    movq g_in+96(%rip), %r12
    movq g_in+104(%rip), %r13
    movq g_in+112(%rip), %r14
    movq g_in+120(%rip), %r15

    movq g_in+32(%rip), %rsp
    pushq g_eflags_in(%rip)
    popfq
    callq *g_code_ptr(%rip)

    movdqu %xmm0, g_xmm_out+0(%rip)
    movdqu %xmm1, g_xmm_out+16(%rip)
    movdqu %xmm2, g_xmm_out+32(%rip)
    movdqu %xmm3, g_xmm_out+48(%rip)
    movdqu %xmm4, g_xmm_out+64(%rip)
    movdqu %xmm5, g_xmm_out+80(%rip)
    movdqu %xmm6, g_xmm_out+96(%rip)
    movdqu %xmm7, g_xmm_out+112(%rip)
    movdqu %xmm8, g_xmm_out+128(%rip)
    movdqu %xmm9, g_xmm_out+144(%rip)
    movdqu %xmm10, g_xmm_out+160(%rip)
    movdqu %xmm11, g_xmm_out+176(%rip)
    movdqu %xmm12, g_xmm_out+192(%rip)
    movdqu %xmm13, g_xmm_out+208(%rip)
    movdqu %xmm14, g_xmm_out+224(%rip)
    movdqu %xmm15, g_xmm_out+240(%rip)

    movq %rax, g_out+0(%rip)
    movq %rcx, g_out+8(%rip)
    movq %rdx, g_out+16(%rip)
    movq %rbx, g_out+24(%rip)
    movq %rsp, g_out+32(%rip)
    movq %rbp, g_out+40(%rip)
    movq %rsi, g_out+48(%rip)
    movq %rdi, g_out+56(%rip)
    movq %r8, g_out+64(%rip)
    movq %r9, g_out+72(%rip)
    movq %r10, g_out+80(%rip)
    movq %r11, g_out+88(%rip)
    movq %r12, g_out+96(%rip)
    movq %r13, g_out+104(%rip)
    movq %r14, g_out+112(%rip)
    movq %r15, g_out+120(%rip)

    pushfq
    popq %rax
    movq %rax, g_flags(%rip)

    movq g_host_rsp(%rip), %rsp

    movdqu g_host_xmm+0(%rip), %xmm6
    movdqu g_host_xmm+16(%rip), %xmm7
    movdqu g_host_xmm+32(%rip), %xmm8
    movdqu g_host_xmm+48(%rip), %xmm9
    movdqu g_host_xmm+64(%rip), %xmm10
    movdqu g_host_xmm+80(%rip), %xmm11
    movdqu g_host_xmm+96(%rip), %xmm12
    movdqu g_host_xmm+112(%rip), %xmm13
    movdqu g_host_xmm+128(%rip), %xmm14
    movdqu g_host_xmm+144(%rip), %xmm15

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rbx
    ret
