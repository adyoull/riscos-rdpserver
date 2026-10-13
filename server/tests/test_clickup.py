#!/usr/bin/env python3
"""RDPServer: src/clickup.s (the routines copied to the RMA that let go of a
mouse button from a ticker event) in the Unicorn emulator, through
riscos-crossdev's emu/rcd_emu.py. Copied to an "RMA" address as
plat_riscos.c does, then:
  - the ticker routine (OS_CallAfter) adds the callback with OS_AddCallBack,
    R0 = the callback's copied address, R1 = its R12, and preserves every
    register;
  - the callback calls OS_CallAVector with R0 = 1 (key up), R1 = the key
    at [R12], R9 = 19 (KeyV), clears the pending word at [R12, #4], and
    preserves every register; it touches no other memory.
   tests/test_clickup.py build/riscos/clickup.o
Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE)."""
import os
import sys

RCD = os.environ.get("RCD_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "..", "..", "riscos-crossdev"))
sys.path.insert(0, os.path.join(RCD, "emu"))
from rcd_emu import Machine, Checks, load_object  # noqa: E402

OS_AddCallBack, OS_CallAVector = 0x54, 0x34
check = Checks("RDPServer click release (emulated ARM)")
code, syms = load_object(sys.argv[1])
start, end = syms["rdps_clickup_ticker"], syms["rdps_clickup_end"]
cb = syms["rdps_clickup_callback"] - start
m = Machine()
RMA = m.claim(end - start + 8 * 3 + 64)
m.load(RMA, code[start:end])
slot = RMA + ((end - start + 3) & ~3) + 8      # the second button's pair
m.w(slot, 0x71)
m.w(slot + 4, 1)
m.w(slot + 8, 0x5A5A5A5A)                     # the next pair: must stay

m.on_swi(OS_AddCallBack, lambda mach, r: None)
keyv = []
m.on_swi(OS_CallAVector, lambda mach, r: keyv.append((r[0], r[1], r[9])))

before = {"r%d" % i: 0x1000 + i for i in range(13)}
before["r12"] = slot
regs, _ = m.call(RMA, **before)
calls = m.called(OS_AddCallBack)
check(len(calls) == 1, "the ticker routine adds one callback")
check(calls and calls[0][0] == RMA + cb, "R0 = the callback, where it was copied")
check(calls and calls[0][1] == slot, "R1 = the same R12")
check(regs == [before["r%d" % i] for i in range(13)], "the ticker routine preserves R0-R12")
check(not keyv, "and doesn't call KeyV itself (interrupts are off)")

regs, _ = m.call(RMA + cb, **before)
check(keyv == [(1, 0x71, 19)], "the callback: KeyV 1 (released), key &71, vector 19")
check(m.r(slot + 4) == 0, "the pending word is cleared")
check(m.r(slot) == 0x71 and m.r(slot + 8) == 0x5A5A5A5A, "nothing else is written")
check(regs == [before["r%d" % i] for i in range(13)], "the callback preserves R0-R12")
check(end - start <= 64, "the routines are small (%d bytes)" % (end - start))
sys.exit(check.done())
