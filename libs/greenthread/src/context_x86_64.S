/* x86-64 SysV cooperative context switch.
 *
 * void dlsm_gt_ctx_switch(void **save_rsp, void *restore_rsp);
 *   rdi = save_rsp     (store current rsp here)
 *   rsi = restore_rsp  (load this rsp)
 *
 */
    .text
    .globl  dlsm_gt_ctx_switch
    .type   dlsm_gt_ctx_switch, @function
    .align  16
dlsm_gt_ctx_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $8, %rsp
    stmxcsr (%rsp)
    fnstcw  4(%rsp)
    movq    %rsp, (%rdi)        /* *save_rsp = rsp */
    movq    %rsi, %rsp          /* rsp = restore_rsp */
    ldmxcsr (%rsp)
    fldcw   4(%rsp)
    addq    $8, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size   dlsm_gt_ctx_switch, .-dlsm_gt_ctx_switch

/* Initial entry stub for a fresh green thread. The initial stack is built so
 * that the first switch "returns" here with r12 = entry fn and r13 = arg. */
    .globl  dlsm_gt_trampoline
    .type   dlsm_gt_trampoline, @function
    .align  16
dlsm_gt_trampoline:
    movq    %r13, %rdi          /* arg */
    call    *%r12               /* entry(arg) */
    call    dlsm_gt_task_finish /* noreturn: switches back to the scheduler */
    ud2                         /* must never reach here */
    .size   dlsm_gt_trampoline, .-dlsm_gt_trampoline

/* Mark the stack as non-executable. */
    .section .note.GNU-stack, "", @progbits
