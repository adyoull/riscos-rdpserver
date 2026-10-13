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
        .word   1                               @ interface version
        .word   9                               @ entries
        .word   e_attach - module_base
        .word   e_detach - module_base
        .word   e_set_file - module_base
        .word   e_start - module_base
        .word   e_stop - module_base
        .word   e_disconnect - module_base
        .word   e_reload - module_base
        .word   e_status - module_base
        .word   e_messages - module_base
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
help:   .asciz  "RDPServer\t0.03 (13 Oct 2026) RDP server (the !RDPServer front end runs it)"
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

@ --- service calls: &57 (WimpReportError) and &53 (WimpCloseDown) ----------
service_veneer:
        teq     r1, #0x57
        teqne   r1, #0x53
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
