#!/usr/bin/env python3
"""RDPServer: src/neon.s (NEON versions of the pixel conversion and the
screen copy, 0.2-test28) in the Unicorn emulator, through riscos-crossdev's
emu/rcd_emu.py, against what pixels.c's pack does (written out here):
  - 32 bpp TBGR and TRGB to the client's 32 bpp and to RGB 5:6:5, for 0 to
    64 pixels (multiples of 8) of random colours: exactly those bytes
    written, the guard bytes after untouched, the source untouched;
  - the copy, 0 to 1024 bytes (multiples of 64);
  - R4-R11 and SP preserved; only D0-D7 and D16-D23 changed.
   tests/test_neon.py build/module/neon.o
Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE)."""
import os
import random
import struct
import sys

RCD = os.environ.get("RCD_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "..", "..", "riscos-crossdev"))
sys.path.insert(0, os.path.join(RCD, "emu"))
from rcd_emu import Machine, Checks, load_object  # noqa: E402
import unicorn.arm_const as A  # noqa: E402

check = Checks("RDPServer NEON pixel work (emulated ARM)")
code, syms = load_object(sys.argv[1])
m = Machine()
# the FPU on, as VFPSupport leaves it for an active context
m.mu.reg_write(A.UC_ARM_REG_C1_C0_2, m.mu.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
m.mu.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
CODE = m.claim(len(code) + 16)
m.load(CODE, code)
GUARD = 32
MAX = 4096
SRC = m.claim(MAX + 2 * GUARD)
DST = m.claim(MAX + 2 * GUARD)
rnd = random.Random(28)

D_ALLOWED = set(range(0, 8)) | set(range(16, 24))
D_REGS = [getattr(A, "UC_ARM_REG_D%d" % i) for i in range(32)]


def run(name, dst, src, n):
    for i in range(32):
        m.mu.reg_write(D_REGS[i], 0x1111111100000000 + i)
    regs = {"r0": dst, "r1": src, "r2": n}
    regs.update({"r%d" % i: 0xC0DE0000 + i for i in range(4, 12)})
    out, _ = m.call(CODE + syms[name], max_insns=100000, **regs)
    d_bad = [i for i in range(32) if i not in D_ALLOWED
             and m.mu.reg_read(D_REGS[i]) != 0x1111111100000000 + i]
    return out[4:12] == [0xC0DE0000 + i for i in range(4, 12)] and not d_bad


def pack32(r, g, b):
    return struct.pack("<I", (r << 16) | (g << 8) | b)


def pack565(r, g, b):
    return struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))


CASES = [
    ("rdps_neon_tbgr_to_32", lambda p: pack32(p[0], p[1], p[2]), 4),
    ("rdps_neon_trgb_to_32", lambda p: pack32(p[2], p[1], p[0]), 4),
    ("rdps_neon_tbgr_to_565", lambda p: pack565(p[0], p[1], p[2]), 2),
    ("rdps_neon_trgb_to_565", lambda p: pack565(p[2], p[1], p[0]), 2),
]
for name, pack, out_bytes in CASES:
    bad = regs_bad = src_bad = 0
    for n in range(0, 65, 8):
        src = bytes(rnd.randrange(256) for _ in range(n * 4))
        m.load(SRC, b"\x77" * GUARD + src + b"\x77" * GUARD)
        m.load(DST, b"\x5a" * (MAX + 2 * GUARD))
        if not run(name, DST + GUARD, SRC + GUARD, n):
            regs_bad += 1
        want = b"\x5a" * GUARD + b"".join(pack(src[i:i + 4]) for i in range(0, n * 4, 4))
        want += b"\x5a" * GUARD
        if bytes(m.mu.mem_read(DST, len(want))) != want:
            bad += 1
        if bytes(m.mu.mem_read(SRC + GUARD, n * 4)) != src:
            src_bad += 1
    check(bad == 0, "%s: as pixels.c packs them, 0-64 pixels, nothing more written (%d wrong)"
          % (name, bad))
    check(regs_bad == 0 and src_bad == 0, "%s: R4-R11 and D8-D15, D24-D31 kept; source untouched" % name)

bad = regs_bad = 0
for n in range(0, 1025, 64):
    data = bytes(rnd.randrange(256) for _ in range(n))
    m.load(SRC + GUARD, data)
    m.load(DST, b"\x5a" * (MAX + 2 * GUARD))
    if not run("rdps_neon_copy", DST + GUARD, SRC + GUARD, n):
        regs_bad += 1
    want = b"\x5a" * GUARD + data + b"\x5a" * GUARD
    if bytes(m.mu.mem_read(DST, len(want))) != want:
        bad += 1
check(bad == 0, "rdps_neon_copy: 0-1024 bytes copied exactly (%d wrong)" % bad)
check(regs_bad == 0, "rdps_neon_copy: R4-R11 and the other D registers kept")
sys.exit(check.done())
