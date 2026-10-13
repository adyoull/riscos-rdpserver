@ RDPServer: lets go of a mouse button from a ticker event, so it happens
@ even while another program, or the Wimp itself, has the machine.
@
@ Why: the Filer, given a double-click, loops reading the mouse (OS_Mouse)
@ without polling the Wimp, and if the button is still down after a delay
@ treats it as Shift-double-click (Filer s/Clicks). The server can't run
@ then, so the button-up is done by this routine, copied to the RMA (it
@ runs whichever task is paged in: RISCOS-RULES section 5).
@
@ test5 had the ticker add a transient callback that did the KeyV call.
@ On the Pi, a click on some title bars then left the button down: the
@ loop waiting for it to go up runs in SVC mode, and callbacks only run on
@ a return to user mode. So KeyV is called from the ticker event itself,
@ as keyboard and mouse drivers call it from their interrupt handlers.
@
@ rdps_clickup_ticker: given to OS_CallAfter (PRM p289: SVC mode,
@   interrupts off, preserve all registers, return with MOV PC, R14).
@   KeyV 1 (key up) for the key number at [R12], through OS_CallAVector
@   (vector 19), then clears the "pending" word at [R12, #4].
@ R12: a two-word block in the RMA, { key number, pending }.
@ SWIs from SVC mode corrupt R14_svc, so it's stacked first; all the
@ other registers are too, in case a KeyV claimant changes them.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .text
        .arm
        .global rdps_clickup_ticker
        .global rdps_clickup_end

rdps_clickup_ticker:
        stmfd   sp!, {r0-r12, lr}
        ldr     r1, [r12]
        mov     r0, #1                  @ KeyV 1: key released
        mov     r9, #19                 @ KeyV
        svc     0x20034                 @ XOS_CallAVector
        ldr     r12, [sp, #48]          @ R12 again, in case KeyV changed it
        mov     r0, #0
        str     r0, [r12, #4]           @ no longer pending
        ldmfd   sp!, {r0-r12, pc}

rdps_clickup_end:
