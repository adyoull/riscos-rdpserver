@ RDPServer module: the module header, the table the front end uses
@ (modapi.h), the veneers into the C (module.c), the relocation done at
@ initialisation, and rdps_swi.
@
@ The C is linked at address 0 and the module can be loaded anywhere in
@ the RMA, so every absolute address in it (the words R_ARM_ABS32
@ relocations point at: literal pools, pointers in data) is relocated by
@ init_veneer before any C runs. tools/modlink.py lists them after the
@ image and fills in modinfo. The C is compiled with -mword-relocations
@ (no MOVW/MOVT pairs) and -mfloat-abi=soft (no VFP: the VFP registers
@ belong to whichever task is paged in), and modlink.py refuses any other
@ kind of absolute relocation.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .syntax unified
        .arm
        .section .text.header, "ax"
        .global module_base
        .global modinfo

        .equ    XOS_EnterOS, 0x20016
        .equ    XOS_LeaveOS, 0x2007C
        .equ    XOS_CallASWIR12, 0x20071
        .equ    XOS_SynchroniseCodeAreas, 0x2006E

module_base:
        .word   0                               @ start
        .word   init_veneer - module_base       @ initialisation
        .word   final_veneer - module_base      @ finalisation
        .word   service_veneer - module_base    @ service calls
        .word   title - module_base
        .word   help - module_base
        .word   0                               @ no *commands
        .word   0, 0, 0, 0                      @ no SWIs
        .word   0                               @ no messages file
        .word   flags - module_base

@ &34: the front end's table (modapi.h)
table:
        .word   0x73504452                      @ "RDPs"
        .word   7                               @ interface version (7: SET_VERBOSE)
        .word   13                              @ entries
        .word   e_attach - module_base
        .word   e_detach - module_base
        .word   e_set_file - module_base
        .word   e_start - module_base
        .word   e_stop - module_base
        .word   e_disconnect - module_base
        .word   e_reload - module_base
        .word   e_status - module_base
        .word   e_messages - module_base
        .word   e_set_mute - module_base
        .word   e_set_tls - module_base
        .word   e_set_sound - module_base
        .word   e_set_verbose - module_base
        .if     table - module_base - 0x34
        .error  "the table must follow the module header (&34)"
        .endif

@ Filled in by tools/modlink.py (offsets from module_base).
modinfo:
reloc_base:     .word   0       @ the base the image is relocated for (0: never)
reloc_table:    .word   0       @ the relocation list: offsets of words
reloc_count:    .word   0
data_start:     .word   0       @ .data, and a copy of it as linked
data_end:       .word   0
data_copy:      .word   0
bss_start:      .word   0
bss_end:        .word   0

title:  .asciz  "RDPServer"
help:   .asciz  "RDPServer\t0.31 (17 Oct 2026) RDP server (the !RDPServer front end runs it)"
        .balign 4
flags:  .word   1                               @ 32-bit compatible

@ --- initialisation: relocate, clear .bss, then mod_init (r12) ------------
init_veneer:
        stmfd   sp!, {r3-r11, lr}               @ (10 words: sp stays 8-aligned)
        adr     r4, module_base                 @ where we are
        ldr     r5, reloc_base                  @ where we were (0: first time)
        @ again after a finalisation (RMReInit): .data as linked
        cmp     r5, #0
        beq     1f
        ldr     r0, data_start
        ldr     r1, data_end
        ldr     r2, data_copy
        add     r0, r0, r4
        add     r1, r1, r4
        add     r2, r2, r4
0:      cmp     r0, r1
        ldrlo   r3, [r2], #4
        strlo   r3, [r0], #4
        blo     0b
1:      @ .bss to zeros
        ldr     r0, bss_start
        ldr     r1, bss_end
        add     r0, r0, r4
        add     r1, r1, r4
        mov     r3, #0
2:      cmp     r0, r1
        strlo   r3, [r0], #4
        blo     2b
        @ relocate: words in .data (now as linked) get the base; the rest
        @ get the change since last time
        ldr     r6, reloc_table
        ldr     r7, reloc_count
        add     r6, r6, r4
        ldr     r8, data_start
        ldr     r9, data_end
        sub     r10, r4, r5                     @ delta for the rest
3:      subs    r7, r7, #1
        bmi     4f
        ldr     r0, [r6], #4                    @ offset
        ldr     r1, [r4, r0]
        cmp     r0, r8
        blo     5f
        cmp     r0, r9
        addlo   r1, r1, r4                      @ in .data
        blo     6f
5:      add     r1, r1, r10
6:      str     r1, [r4, r0]
        b       3b
4:      str     r4, reloc_base
        mov     r0, #0                          @ the whole of memory
        swi     XOS_SynchroniseCodeAreas
        mov     r0, r12                         @ the private word
        bl      mod_init
        cmp     r0, #0
        ldmfdeq sp!, {r3-r11, pc}               @ V clear (cmp #0)
        ldmfd   sp!, {r3-r11, lr}
        msr     cpsr_f, #0x10000000             @ V set: error in r0
        mov     pc, lr

@ --- finalisation: mod_final (r10 = fatal) ---------------------------------
final_veneer:
        stmfd   sp!, {r3-r11, lr}
        mov     r0, r10
        bl      mod_final
        cmp     r0, #0
        ldmfdeq sp!, {r3-r11, pc}
        ldmfd   sp!, {r3-r11, lr}
        msr     cpsr_f, #0x10000000
        mov     pc, lr

@ --- service calls: &57 (WimpReportError), &53 (WimpCloseDown), &45 (PreReset),
@     &80 (ShutdownComplete)
service_veneer:
        teq     r1, #0x57
        teqne   r1, #0x53
        teqne   r1, #0x45
        teqne   r1, #0x80
        movne   pc, lr
        stmfd   sp!, {r0-r12, lr}
        bl      mod_service                     @ (r0, r1 = service, r2)
        ldmfd   sp!, {r0-r12, pc}

@ --- the ticker (OS_CallEvery): SVC mode, interrupts off -------------------
        .global mod_ticker_veneer
mod_ticker_veneer:
        stmfd   sp!, {r0-r12, lr}
        bl      mod_ticker
        ldmfd   sp!, {r0-r12, pc}

@ --- the transient callback: SVC mode, interrupts on -----------------------
        .global mod_callback_veneer
mod_callback_veneer:
        stmfd   sp!, {r0-r12, lr}
        bl      mod_callback
        ldmfd   sp!, {r0-r12, pc}

@ --- ByteV: only OS_Byte &81 with R1 = R2 = 0 while an error box is open
@ (mod_errorbox_open: Service_WimpReportError); everything else passes
@ straight on. (ByteV is always called from the SWI handler, so in SVC
@ mode; the mode test is only a safety net.) OS_Byte is entered with
@ interrupts off (PRM ByteV): they are turned on while the server runs,
@ and the flags and interrupt state are put back before passing on.
        .global mod_bytev_veneer
mod_bytev_veneer:
        teq     r0, #0x81
        movne   pc, lr
        teq     r1, #0
        teqeq   r2, #0
        movne   pc, lr
        stmfd   sp!, {r0-r5}
        ldr     r0, errorbox_open_addr
        ldr     r0, [r0]
        mrs     r4, cpsr
        and     r1, r4, #0x1f
        teq     r1, #0x13                       @ SVC mode only
        movne   r0, #0
        cmp     r0, #0
        ldmfdeq sp!, {r0-r5}
        moveq   pc, lr                          @ pass on
        stmfd   sp!, {r6-r12, lr}
        bic     r5, r4, #0x80
        msr     cpsr_c, r5                      @ interrupts on
        bl      mod_bytev
        msr     cpsr_fc, r4                     @ flags, mode, interrupts as they were
        ldmfd   sp!, {r6-r12, lr}
        ldmfd   sp!, {r0-r5}
        mov     pc, lr                          @ pass on to the OS_Byte
errorbox_open_addr:
        .word   mod_errorbox_open

@ --- the front end's entries: USR mode in, SVC mode for the C, USR out ----
        .macro  ENTRY name, cfunc
\name:
        stmfd   sp!, {r4-r11, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        mov     r7, r3
        swi     XOS_EnterOS
        mov     r0, r4
        mov     r1, r5
        mov     r2, r6
        mov     r3, r7
        bl      \cfunc
        mov     r4, r0
        swi     XOS_LeaveOS
        mov     r0, r4
        ldmfd   sp!, {r4-r11, pc}
        .endm

        ENTRY   e_attach, mod_attach
        ENTRY   e_detach, mod_detach
        ENTRY   e_set_file, mod_set_file
        ENTRY   e_start, mod_start
        ENTRY   e_stop, mod_stop
        ENTRY   e_disconnect, mod_disconnect
        ENTRY   e_reload, mod_reload_logins
        ENTRY   e_status, mod_status
        ENTRY   e_messages, mod_messages
        ENTRY   e_set_mute, mod_set_mute
        ENTRY   e_set_tls, mod_set_require_tls
        ENTRY   e_set_sound, mod_set_sound
        ENTRY   e_set_verbose, mod_set_verbose

@ --- capturing the sound (sndcap.c) ----------------------------------------
@ A SharedSound "process" handler can't hear everything: SharedSound calls
@ process handlers on a callback with only what its callback handlers made,
@ while players using immediate handlers fill the DMA buffer directly
@ (SharedSnd source, doHandlerCallBack%). So the capture wraps SoundDMA's
@ 16-bit linear handler instead, which makes the final mix
@ (Sound_LinearHandler, PRM p639; entry: R0 = workspace, R1 = buffer, R2 =
@ end, R3 bits 0-2: 0 invalid, 1 8-bit data converted, 2 all zeros, R4 =
@ rate in 1/1024 Hz; IRQ or SVC mode; R0-R10 corruptible, R11-R13 kept).
@
        .text
@ rdps_lin_stub .. rdps_lin_stub_end: position-independent, copied into
@ the RMA with its workspace (struct lin_ws in sndcap.c) after it, so it
@ can stay behind, passing straight through, if another program took the
@ linear handler over after us and we can't put the old one back. It
@ calls the handler it replaced, then (while active) copies the buffer
@ into the ring and silences it if mute is set.
        .equ    L_OLD, 0
        .equ    L_OLDWS, 4
        .equ    L_ACTIVE, 8
        .equ    L_BASE, 12
        .equ    L_MASK, 16
        .equ    L_WR, 20
        .equ    L_MUTE, 24
        .equ    L_CALLS, 28
        .equ    L_RATE, 32
        .equ    L_FLAGS, 36
        .equ    L_SOUND, 40
        .global rdps_lin_stub
        .global rdps_lin_stub_end
rdps_lin_stub:
        stmfd   sp!, {r0-r3, r11, lr}           @ (6 words: sp stays 8-aligned)
        str     r3, [r0, #L_FLAGS]              @ (the old handler may corrupt them)
        str     r4, [r0, #L_RATE]
        ldr     r5, [r0, #L_OLD]
        cmp     r5, #0
        beq     1f
        ldr     r0, [r0, #L_OLDWS]
        blx     r5                              @ the old handler mixes first
        ldmia   sp, {r0-r2}
        b       2f
1:      and     r6, r3, #7                      @ none: invalid data -> silence
        cmp     r6, #0
        bne     2f
        mov     r6, #0
        mov     r7, r1
0:      cmp     r7, r2
        strlo   r6, [r7], #4
        blo     0b
2:      ldr     r6, [r0, #L_ACTIVE]
        cmp     r6, #0
        beq     9f
        ldr     r6, [r0, #L_CALLS]
        add     r6, r6, #1
        str     r6, [r0, #L_CALLS]
        ldr     r5, [r0, #L_BASE]
        cmp     r5, #0
        beq     9f
        ldr     r6, [r0, #L_MASK]
        ldr     r7, [r0, #L_WR]
        mov     r8, r1
        mov     r11, #0                         @ any sound at all?
3:      cmp     r8, r2
        bhs     4f
        ldr     r9, [r8], #4
        orr     r11, r11, r9
        and     r10, r7, r6
        str     r9, [r5, r10]
        add     r7, r7, #4
        b       3b
4:      str     r7, [r0, #L_WR]
        cmp     r11, #0
        ldrne   r6, [r0, #L_SOUND]
        addne   r6, r6, #1
        strne   r6, [r0, #L_SOUND]
        ldr     r9, [r0, #L_MUTE]
        cmp     r9, #0
        beq     9f
        mov     r9, #0
5:      cmp     r1, r2
        strlo   r9, [r1], #4
        blo     5b
9:      ldmfd   sp!, {r0-r3, r11, pc}

@ rdps_keep_handler (copied with the stub): a SharedSound process handler
@ that only keeps SharedSound active, so it is the linear handler before
@ we wrap it and doesn't put itself back over us. It writes zeros when
@ nothing has (SharedSound must be given data). In the RMA copy too, so
@ nothing ever calls into the module's own code.
        .global rdps_keep_handler
rdps_keep_handler:
        tst     r3, #1
        movne   pc, lr
        stmfd   sp!, {r1, r4}
        mov     r4, #0
0:      cmp     r1, r2
        strlo   r4, [r1], #4
        blo     0b
        orr     r3, r3, #1
        ldmfd   sp!, {r1, r4}
        mov     pc, lr
        .align  2
rdps_lin_stub_end:

@ --- _kernel_oserror *rdps_swi (int swi, int *r) ---------------------------
        .text
        .global rdps_swi
rdps_swi:
        stmfd   sp!, {r1, r4-r11, lr}
        orr     r12, r0, #0x20000               @ the X form
        ldmia   r1, {r0-r9}
        swi     XOS_CallASWIR12
        ldr     lr, [sp]                        @ r (the SWI may change lr)
        stmia   lr, {r0-r9}
        movvc   r0, #0
        ldmfd   sp!, {r1, r4-r11, pc}

        .end
