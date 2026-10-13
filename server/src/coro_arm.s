@ RDPServer: coro.h for RISC OS (the module). One coroutine, the server
@ thread, which runs in USR mode on its own stack.
@
@ Why USR mode: the module resumes the thread from SVC mode (its callback,
@ its ByteV claimant, the front end's entries). In SVC mode with sp off
@ the SVC stack, a SWI or an interrupt handled by a C module crashes it:
@ C modules find their static data from the base of the SVC stack (sp
@ rounded down to the megabyte). 0.2-test1 did that and aborted inside
@ Internet (+&4A8) on the Pi. In USR mode the thread's SWIs and
@ interrupts use the SVC stack, as any program's do.
@
@ co_resume (SVC mode): saves the SVC context and the user registers sp
@   and lr of whatever task is current (a callback can come in any task:
@   they must be put back), then drops to USR mode, interrupts on, on the
@   thread's stack.
@ co_yield (USR mode, in the thread): saves the thread's context, back to
@   SVC mode with OS_EnterOS, puts the user sp and lr back, and returns
@   from co_resume.
@ A context is r4-r11, lr and sp (AAPCS). No VFP: the module is built
@ with -mfloat-abi=soft (the VFP registers are the current task's).
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .syntax unified
        .arm
        .text
        .global co_start
        .global co_resume
        .global co_yield
        .global co_inside

        .equ    XOS_EnterOS, 0x20016

@ co_state layout
        .equ    OUT, 0          @ outside: r4-r11, lr (9 words), sp
        .equ    OUT_SP, 36
        .equ    OUT_USR, 40     @ the current task's user sp, lr
        .equ    IN, 48          @ inside: r4-r11, lr, sp
        .equ    IN_LR, 80
        .equ    IN_SP, 84
        .equ    ALIVE, 88
        .equ    INSIDE, 92

        .bss
        .balign 4
co_state:
        .space  96
        .text

@ int co_start (void (*fn) (void *), void *arg, void *stack, size_t size)
co_start:
        cmp     r3, #16384
        blo     1f
        add     r2, r2, r3
        bic     r2, r2, #7              @ 8-byte aligned top of the stack
        ldr     ip, =co_state
        ldr     r3, [ip, #ALIVE]
        cmp     r3, #0
        bne     1f
        str     r0, [ip, #IN]           @ inside r4 = fn
        str     r1, [ip, #IN + 4]       @ inside r5 = arg
        adr     r0, trampoline
        str     r0, [ip, #IN_LR]        @ first resume goes to the trampoline
        str     r2, [ip, #IN_SP]
        mov     r0, #1
        str     r0, [ip, #ALIVE]
        mov     r0, #0
        bx      lr
1:      mvn     r0, #0
        bx      lr

@ USR mode: fn (arg); when fn returns, not alive, back to the resumer.
trampoline:
        mov     r0, r5
        blx     r4
        ldr     ip, =co_state
        mov     r0, #0
        str     r0, [ip, #ALIVE]
        str     r0, [ip, #INSIDE]
        b       back_to_svc

@ int co_resume (void): SVC mode only
co_resume:
        ldr     ip, =co_state
        ldr     r0, [ip, #ALIVE]
        cmp     r0, #0
        bxeq    lr
        ldr     r1, [ip, #INSIDE]       @ already inside: refuse
        cmp     r1, #0
        movne   r0, #1
        bxne    lr
        stmia   ip, {r4-r11, lr}        @ the SVC context
        str     sp, [ip, #OUT_SP]
        add     r1, ip, #OUT_USR
        stmia   r1, {sp, lr}^           @ the current task's user sp and lr
        nop
        mov     r1, #1
        str     r1, [ip, #INSIDE]
        add     r1, ip, #IN
        ldmia   r1, {r4-r11}
        ldr     r2, [ip, #IN_SP]
        ldr     r3, [ip, #IN_LR]
        mrs     r0, cpsr
        bic     r0, r0, #0x9F           @ mode bits and I
        orr     r0, r0, #0x10           @ USR32, interrupts on
        msr     cpsr_c, r0
        mov     sp, r2                  @ (USR sp now)
        bx      r3                      @ co_yield's return, or the trampoline

@ void co_yield (void): USR mode, inside the thread
co_yield:
        ldr     ip, =co_state
        ldr     r0, [ip, #INSIDE]
        cmp     r0, #0
        bxeq    lr                      @ not inside: nothing to do
        add     r1, ip, #IN
        stmia   r1, {r4-r11, lr}
        str     sp, [ip, #IN_SP]
        mov     r0, #0
        str     r0, [ip, #INSIDE]
back_to_svc:
        swi     XOS_EnterOS             @ SVC mode; sp: the SVC stack where we left it
        ldr     ip, =co_state
        add     r1, ip, #OUT_USR
        ldmia   r1, {sp, lr}^           @ the task's user sp and lr back
        nop
        ldmia   ip, {r4-r11, lr}
        ldr     sp, [ip, #OUT_SP]
        ldr     r0, [ip, #ALIVE]        @ co_resume returns alive
        bx      lr

@ int co_inside (void)
co_inside:
        ldr     ip, =co_state
        ldr     r0, [ip, #INSIDE]
        bx      lr

        .ltorg
