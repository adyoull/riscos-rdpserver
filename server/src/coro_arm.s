@ RDPServer: coro.h for RISC OS (the module). One coroutine.
@
@ A context is r4-r11, lr and sp: the registers a function must keep
@ (AAPCS). No VFP registers: the module is built with
@ -mgeneral-regs-only, so the server never touches them (and must not:
@ they belong to whichever task is paged in).
@
@ co_resume and co_yield are ordinary calls; they work in any mode as
@ long as it's the same one each time (the module: SVC). sp is switched
@ to the coroutine's own stack, so the server never runs on the small
@ SVC stack.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .syntax unified
        .arm
        .text
        .global co_start
        .global co_resume
        .global co_yield
        .global co_inside

@ The state, in .bss (the module's image).
        .bss
        .balign 4
co_state:
outside: .space 40                     @ r4-r11, lr, sp of co_resume's caller
inside:  .space 40                     @ the coroutine's
alive:   .space 4
in:      .space 4
        .text

@ int co_start (void (*fn) (void *), void *arg, void *stack, size_t size)
co_start:
        cmp     r3, #16384
        blo     1f
        add     r2, r2, r3
        bic     r2, r2, #7              @ 8-byte aligned top of the stack
        ldr     ip, =co_state
        ldr     r3, [ip, #80]           @ alive?
        cmp     r3, #0
        bne     1f
        str     r0, [ip, #40]           @ inside r4 = fn
        str     r1, [ip, #44]           @ inside r5 = arg
        adr     r0, trampoline
        str     r0, [ip, #72]           @ inside lr = trampoline
        str     r2, [ip, #76]           @ inside sp
        mov     r0, #1
        str     r0, [ip, #80]           @ alive
        mov     r0, #0
        bx      lr
1:      mvn     r0, #0
        bx      lr

@ First entry: fn (arg); when fn returns, not alive, back outside.
trampoline:
        mov     r0, r5
        blx     r4
        ldr     ip, =co_state
        mov     r0, #0
        str     r0, [ip, #80]           @ not alive
        str     r0, [ip, #84]           @ not inside
        ldmia   ip, {r4-r11, lr}        @ outside
        ldr     sp, [ip, #36]
        mov     r0, #0                  @ co_resume returns 0
        bx      lr

@ int co_resume (void)
co_resume:
        ldr     ip, =co_state
        ldr     r0, [ip, #80]           @ alive
        cmp     r0, #0
        bxeq    lr
        ldr     r1, [ip, #84]           @ already inside: refuse
        cmp     r1, #0
        bxne    lr
        stmia   ip, {r4-r11, lr}        @ save outside
        str     sp, [ip, #36]
        mov     r1, #1
        str     r1, [ip, #84]           @ in = 1
        add     r1, ip, #40
        ldmia   r1, {r4-r11, lr}        @ load inside
        ldr     sp, [r1, #36]
        bx      lr                      @ into the coroutine (co_yield's
                                        @ return, or the trampoline)

@ void co_yield (void)
co_yield:
        ldr     ip, =co_state
        ldr     r0, [ip, #84]
        cmp     r0, #0
        bxeq    lr                      @ not inside: nothing to do
        add     r1, ip, #40
        stmia   r1, {r4-r11, lr}        @ save inside
        str     sp, [r1, #36]
        mov     r0, #0
        str     r0, [ip, #84]           @ in = 0
        ldmia   ip, {r4-r11, lr}        @ load outside
        ldr     sp, [ip, #36]
        ldr     r0, [ip, #80]           @ co_resume returns alive (1)
        bx      lr

@ int co_inside (void)
co_inside:
        ldr     ip, =co_state
        ldr     r0, [ip, #84]
        bx      lr

        .ltorg
