/*
 * x86_64 context switch for fiberecs.
 *
 *   void fiberecs_switch_asm(FiberContext* to, FiberContext* from)
 *
 * Two ABIs are in play, and they disagree about everything that matters here:
 *
 *   SysV (Linux/ELF)   to = %rdi   from = %rsi   callee-saved: rbx rbp r12-r15
 *   Microsoft (MinGW)  to = %rcx   from = %rdx   callee-saved: rbx rbp rdi rsi
 *                                                     r12-r15
 *
 * Getting this wrong is silent: the routine still saves and restores a stack,
 * it just reads its two pointers out of whatever happened to be in %rdi/%rsi,
 * so the first switch faults on a write to a near-null address.
 *
 * Saving: on entry %rsp points at our own return address. The stack pointer the
 * frame must resume with is therefore %rsp + 8 -- that is the value a normal
 * return would have left behind. Recording plain %rsp and reloading it with a
 * `push`/`ret` pair is off by one slot: push and ret cancel out, so the resumed
 * frame would come back 8 bytes below where it left, and the caller's epilogue
 * would then pop its own frame one slot too low and `ret` into a stack address.
 *
 * Restoring: the callee-saved register file is loaded, then rip is pushed and
 * executed with `ret`. Both ABIs require %rsp == 0 (mod 16) at a call site,
 * which is exactly what the saved %rsp + 8 gives the resumed frame, so any
 * 16-byte aligned move (movaps/movdqa) it executes is safe.
 *
 * Context layout (see FiberContext in Fiber.cpp -- the order must match):
 *
 *   0x00 rsp    (stack pointer *after* the call's return address is consumed)
 *   0x08 rip
 *   0x10 rbx
 *   0x18 rbp
 *   0x20 r12
 *   0x28 r13
 *   0x30 r14
 *   0x38 r15
 *   0x40 rdi   (Microsoft ABI only)
 *   0x48 rsi   (Microsoft ABI only)
 */

#if defined(_WIN32)
#define FIBERECS_ARG_TO      %rcx
#define FIBERECS_ARG_FROM    %rdx
#define FIBERECS_SAVE_DI_SI  1
#else
#define FIBERECS_ARG_TO      %rdi
#define FIBERECS_ARG_FROM    %rsi
#endif

    .text
    .globl  fiberecs_switch_asm
#if defined(_WIN32)
    .def    fiberecs_switch_asm; .scl 2; .type 32; .endef
#endif
    .align 16
fiberecs_switch_asm:
    /* --- save the outgoing context --- */
    leaq    8(%rsp), %rax
    movq    %rax, 0(FIBERECS_ARG_FROM)
    movq    (%rsp), %rax
    movq    %rax, 8(FIBERECS_ARG_FROM)
    movq    %rbx, 16(FIBERECS_ARG_FROM)
    movq    %rbp, 24(FIBERECS_ARG_FROM)
    movq    %r12, 32(FIBERECS_ARG_FROM)
    movq    %r13, 40(FIBERECS_ARG_FROM)
    movq    %r14, 48(FIBERECS_ARG_FROM)
    movq    %r15, 56(FIBERECS_ARG_FROM)
#if defined(FIBERECS_SAVE_DI_SI)
    movq    %rdi, 64(FIBERECS_ARG_FROM)
    movq    %rsi, 72(FIBERECS_ARG_FROM)
#endif

    /* --- restore the incoming context and resume it --- */
    movq    16(FIBERECS_ARG_TO), %rbx
    movq    24(FIBERECS_ARG_TO), %rbp
    movq    32(FIBERECS_ARG_TO), %r12
    movq    40(FIBERECS_ARG_TO), %r13
    movq    48(FIBERECS_ARG_TO), %r14
    movq    56(FIBERECS_ARG_TO), %r15
#if defined(FIBERECS_SAVE_DI_SI)
    movq    64(FIBERECS_ARG_TO), %rdi
    movq    72(FIBERECS_ARG_TO), %rsi
#endif
    /* The saved %rsp already excludes this frame's return address, so push/ret
       lands the resumed frame exactly one slot above it -- back at the caller's
       %rsp, which is 16-byte aligned at every call site in the code above. */
    movq    0(FIBERECS_ARG_TO), %rsp
    movq    8(FIBERECS_ARG_TO), %rax
    pushq   %rax
    ret

    .globl  fiberecs_bootstrap_asm
#if defined(_WIN32)
    .def    fiberecs_bootstrap_asm; .scl 2; .type 32; .endef
#endif
    .align 16
fiberecs_bootstrap_asm:
    /*
     * Entry point of a freshly built stack frame. Fiber::create seeds the
     * initial register file with %r12 = Fiber* and %r13 = &Fiber::bootstrap,
     * so this routine dispatches indirectly and the assembly never needs to
     * spell (or hard-code) a C++ mangled name.
     */
    movq    %r12, FIBERECS_ARG_TO
    call    *%r13
    ud2                                 /* bootstrap() never returns */

/* Mark the stack non-executable. */
#if !defined(_WIN32) && (defined(__linux__) || defined(__ELF__))
    .section .note.GNU-stack,"",%progbits
#endif
