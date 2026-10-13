#!/usr/bin/env python3
"""RDPServer: src/clickup.s (the routines copied to the RMA that let go of a
mouse button from a ticker event) in the Unicorn emulator, through
riscos-crossdev's emu/rcd_emu.py. Copied to an "RMA" address as
plat_riscos.c does, then:
  - the ticker routine (OS_CallAfter) calls OS_CallAVector with R0 = 1
    (key up), R1 = the key at [R12], R9 = 19 (KeyV), clears the pending
    word at [R12, #4], preserves every register (even if KeyV changes
    them) and touches no other memory.
   tests/test_clickup.py build/riscos/clickup.o
Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE)."""
import os
import sys

RCD = os.environ.get("RCD_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "..", "..", "riscos-crossdev"))
sys.path.insert(0, os.path.join(RCD, "emu"))
from rcd_emu import Machine, Checks, load_object  # noqa: E402

OS_CallAVector = 0x34
check = Checks("RDPServer click release (emulated ARM)")
code, syms = load_object(sys.argv[1])
start, end = syms["rdps_clickup_ticker"], syms["rdps_clickup_end"]
m = Machine()
RMA = m.claim(end - start + 8 * 3 + 64)
m.load(RMA, code[start:end])
slot = RMA + ((end - start + 3) & ~3) + 8      # the second button's pair
m.w(slot, 0x71)
m.w(slot + 4, 1)
m.w(slot + 8, 0x5A5A5A5A)                     # the next pair: must stay

keyv = []


def callavector(mach, r):
    keyv.append((r[0], r[1], r[9]))
    for i in range(10):                       # a claimant that changes things
        r[i] = 0xDEAD0000 + i


m.on_swi(OS_CallAVector, callavector)
before = {"r%d" % i: 0x1000 + i for i in range(13)}
before["r12"] = slot
regs, _ = m.call(RMA, **before)
check(keyv == [(1, 0x71, 19)], "KeyV 1 (released), key &71, vector 19, from the ticker itself")
check(len(m.swis) == 1, "no other SWI (no callback: it wouldn't run in an SVC-mode loop)")
check(m.r(slot + 4) == 0, "the pending word is cleared")
check(m.r(slot) == 0x71 and m.r(slot + 8) == 0x5A5A5A5A, "nothing else is written")
check(regs == [before["r%d" % i] for i in range(13)], "R0-R12 preserved")
check(end - start <= 64, "the routine is small (%d bytes)" % (end - start))
sys.exit(check.done())
