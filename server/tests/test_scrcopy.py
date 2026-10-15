#!/usr/bin/env python3
"""RDPServer: src/scrcopy.s (copying rows out of the screen, and the
module's memcpy for aligned runs of 32 bytes or more) in the Unicorn
emulator, through riscos-crossdev's emu/rcd_emu.py. For both routines,
every alignment of destination and source (0-3) and lengths 0-100, 255,
1000 and 7687:
  - exactly n bytes written, equal to the source; the guard bytes either
    side of the destination untouched; the source untouched;
  - R4-R11 and the stack pointer preserved (AAPCS);
  - LDM 8 takes far fewer instructions than single words on a long run.
   tests/test_scrcopy.py build/module/scrcopy.o
Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE)."""
import os
import random
import sys

RCD = os.environ.get("RCD_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "..", "..", "riscos-crossdev"))
sys.path.insert(0, os.path.join(RCD, "emu"))
from rcd_emu import Machine, Checks, load_object  # noqa: E402

check = Checks("RDPServer screen copy (emulated ARM)")
code, syms = load_object(sys.argv[1])
m = Machine()
CODE = m.claim(len(code) + 16)
m.load(CODE, code)
GUARD = 40
MAX = 7687 + 8
SRC = m.claim(MAX + 2 * GUARD)
DST = m.claim(MAX + 2 * GUARD)
rnd = random.Random(21)
data = bytes(rnd.randrange(256) for _ in range(MAX + 2 * GUARD))
lengths = list(range(0, 101)) + [255, 1000, 7687]

for name in ("rdps_copy_ldm8", "rdps_copy_words"):
    entry = CODE + syms[name]
    bad_bytes = bad_regs = bad_src = 0
    for doff in range(4):
        for soff in range(4):
            for n in lengths:
                m.load(SRC, data)
                m.load(DST, b"\x5a" * (MAX + 2 * GUARD))
                d, s = DST + GUARD + doff, SRC + GUARD + soff
                regs = {"r0": d, "r1": s, "r2": n}
                regs.update({"r%d" % i: 0xC0DE0000 + i for i in range(4, 12)})
                out, _ = m.call(entry, max_insns=200000, **regs)
                got = bytes(m.mu.mem_read(DST, MAX + 2 * GUARD))
                want = bytearray(b"\x5a" * (MAX + 2 * GUARD))
                want[GUARD + doff:GUARD + doff + n] = data[GUARD + soff:GUARD + soff + n]
                if got != bytes(want):
                    bad_bytes += 1
                if out[4:12] != [0xC0DE0000 + i for i in range(4, 12)]:
                    bad_regs += 1
                if bytes(m.mu.mem_read(SRC, MAX + 2 * GUARD)) != data:
                    bad_src += 1
    cases = 16 * len(lengths)
    check(bad_bytes == 0, "%s: exactly n bytes copied, guards untouched (%d of %d wrong)"
          % (name, bad_bytes, cases))
    check(bad_regs == 0, "%s: R4-R11 preserved (%d of %d not)" % (name, bad_regs, cases))
    check(bad_src == 0, "%s: the source untouched" % name)


def finishes(name, n, limit):
    """Whether the routine copies n bytes within `limit` instructions."""
    try:
        m.call(CODE + syms[name], max_insns=limit, r0=DST + GUARD, r1=SRC + GUARD, r2=n)
        return True
    except RuntimeError:
        return False


# 7680 bytes (a 1920-pixel 32 bpp row): LDM 8 is 240 loops of 4
# instructions; single words 1920 loops of 4.
check(finishes("rdps_copy_ldm8", 7680, 1200), "a 7680-byte row by LDM 8 in under 1200 instructions")
check(not finishes("rdps_copy_words", 7680, 1200) and finishes("rdps_copy_words", 7680, 8000),
      "and by single words, 1200-8000 (so the LDM path is the one taken)")
sys.exit(check.done())
