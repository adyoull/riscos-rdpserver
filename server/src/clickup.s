@ RDPServer: lets go of a mouse button from a ticker event, so it happens
@ even while another program has the machine.
@
@ Why: the Filer, given a double-click, loops reading the mouse (OS_Mouse)
@ without polling the Wimp, and if the button is still down after a delay
@ treats it as Shift-double-click (Filer s/Clicks). The server can't run
@ then, so the button-up is done by these routines, copied to the RMA
@ (they run whichever task is paged in: RISCOS-RULES section 5).
@
@ rdps_clickup_ticker: given to OS_CallAfter (PRM p289: SVC mode,
@   interrupts off, preserve all registers, return with MOV PC, R14). It
@   only adds a transient callback (OS_AddCallBack, PRM p221), so KeyV is
@   called when RISC OS next returns to user mode.
@ rdps_clickup_callback: KeyV 1 (key up) for the key number at [R12],
@   through OS_CallAVector (vector 19), then clears the "pending" word at
@   [R12, #4].
@ R12 for both: a two-word block in the RMA, { key number, pending }.
@ SWIs from SVC mode corrupt R14_svc, so it's stacked first.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .text
        .arm
        .global rdps_clickup_ticker
        .global rdps_clickup_callback
        .global rdps_clickup_end

rdps_clickup_ticker:
        stmfd   sp!, {r0, r1, lr}
        adr     r0, rdps_clickup_callback
        mov     r1, r12
        svc     0x20054                 @ XOS_AddCallBack
        ldmfd   sp!, {r0, r1, pc}

rdps_clickup_callback:
        stmfd   sp!, {r0-r12, lr}       @ all of them: other KeyV claimants
                                        @ may not preserve them
        ldr     r1, [r12]
        mov     r0, #1                  @ KeyV 1: key released
        mov     r9, #19                 @ KeyV
        svc     0x20034                 @ XOS_CallAVector
        ldr     r12, [sp, #48]          @ R12 again, in case KeyV changed it
        mov     r0, #0
        str     r0, [r12, #4]           @ no longer pending
        ldmfd   sp!, {r0-r12, pc}

rdps_clickup_end:
