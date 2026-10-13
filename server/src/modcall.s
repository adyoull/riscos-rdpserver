@ RDPServer: calls an entry of the RDPServer module's table (modclient.c).
@ int rdps_modcall (const void *fn, void *ws, int a, int b, int c):
@ fn (a, b, c) with R12 = ws, the module's workspace (mod/modapi.h).
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
        .syntax unified
        .arm
        .text
        .global rdps_modcall
rdps_modcall:
        stmfd   sp!, {r4, lr}
        mov     r4, r0
        mov     r12, r1
        mov     r0, r2
        mov     r1, r3
        ldr     r2, [sp, #8]            @ c (the fifth argument)
        blx     r4
        ldmfd   sp!, {r4, pc}
