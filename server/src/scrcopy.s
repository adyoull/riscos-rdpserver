@ RDPServer: copying out of the screen (the module; the host build uses
@ memcpy, in capture.c).
@
@ The screen behaves as uncached memory, where every load is a trip to
@ RAM: loading eight words at once (LDM) is several times quicker than
@ one at a time. On a Pi 4, copying out of uncached memory ran at 131-139
@ MB/s by single words, 506-559 MB/s by LDM of four words and 725 MB/s
@ by LDM of eight (measured for ReelHWAccel's vcdec, whose copy this is).
@ The module's own memcpy (src/mod/libc.c) copies single words.
@
@ void rdps_copy_ldm8 (void *dst, const void *src, size_t n)
@ void rdps_copy_words (void *dst, const void *src, size_t n)
@   Exactly n bytes are written. If dst and src are both word aligned:
@   eight words a loop (rdps_copy_ldm8) or one (rdps_copy_words), then
@   words, then bytes; otherwise bytes. Ordinary AAPCS calls: R0-R3, R12
@   and the flags corrupted, R4-R11 preserved. Any processor mode.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .syntax unified
        .arm
        .text

@ the tail, shared: r0 dst, r1 src, r2 bytes left; words if aligned, then
@ bytes. Corrupts r0-r3, r12.
        .type   copy_tail, %function
copy_tail:
        orr     r12, r0, r1
        tst     r12, #3
        bne     5f
        subs    r2, r2, #4
        blt     4f
3:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #4
        bge     3b
4:      adds    r2, r2, #4              @ 0 to 3 bytes left
5:      cmp     r2, #0
        bxeq    lr
6:      ldrb    r3, [r1], #1
        strb    r3, [r0], #1
        subs    r2, r2, #1
        bne     6b
        bx      lr
        .size   copy_tail, . - copy_tail

        .global rdps_copy_ldm8
        .type   rdps_copy_ldm8, %function
rdps_copy_ldm8:
        orr     r12, r0, r1
        tst     r12, #3
        bne     copy_tail
        push    {r4-r9}
        subs    r2, r2, #32
        blt     2f
1:      ldmia   r1!, {r3-r9, r12}
        stmia   r0!, {r3-r9, r12}
        subs    r2, r2, #32
        bge     1b
2:      pop     {r4-r9}
        add     r2, r2, #32
        b       copy_tail
        .size   rdps_copy_ldm8, . - rdps_copy_ldm8

        .global rdps_copy_words
        .type   rdps_copy_words, %function
rdps_copy_words:
        b       copy_tail
        .size   rdps_copy_words, . - rdps_copy_words

        .section .note.GNU-stack, "", %progbits
