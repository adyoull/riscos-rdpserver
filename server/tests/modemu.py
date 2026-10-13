"""RDPServer: the module (build/module/RDPServer,ffa) in the Unicorn ARM
emulator, with a pretend RISC OS around it in Python.

What's pretend, and how close it is:
- the module is loaded at an address of the test's choosing (not the
  address it was linked at) and initialised through its header, as
  RMLoad would; finalised the same way;
- SWIs, whether called directly or through OS_CallASWIR12 (rdps_swi), are
  answered here: the dynamic area and its OS_Heap, RMA claims, tickers
  (OS_CallEvery/OS_CallAfter), transient callbacks, vectors, the screen
  (a 640x480 32 bpp TBGR picture in emulated memory, the same as
  plat_host.c's), the mouse and keys (logged), the time, CryptRandom,
  and the Internet module's Socket_ SWIs on real host sockets, so real
  RDP clients can connect;
- the kernel's side: run() calls the ticker every 2 cs and, when the
  module asked for a callback, calls it (as RISC OS does when it next
  returns to USR mode) - except while a pretend error box is open, when,
  as in the Wimp's error box loop (s/Wimp07 pollit), no callbacks run and
  OS_Byte &81 0 0 goes through the module's ByteV claimant instead;
- everything runs in SVC mode (OS_EnterOS/OS_LeaveOS change nothing), on
  one SVC stack.

Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
"""
import errno
import os
import select
import socket
import struct
import time

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_MEM_INVALID
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm_const as A

REGS = [getattr(A, "UC_ARM_REG_R%d" % i) for i in range(13)]
VFLAG = 1 << 28
XBIT = 0x20000

# memory map
LOW = 0x0                   # page 0: the return address
RET = 0x0FF0
USER = 0x00100000           # the "front end's" buffers
USER_SIZE = 0x00100000
SVC_TOP = 0x01F00000        # SVC stack (64 KB below)
SVC_SIZE = 0x00100000
SVC_REAL = 8192            # the SVC stack RISC OS 5 has
USER_TOP = 0x00200000 - 0x100   # a user stack (top of USER)
USER_LR = 0x7EAD0000       # the current task's user lr: must survive
ERRS = 0x01C00000           # error blocks from pretend SWIs
RMA = 0x02000000            # the module and RMA claims
RMA_SIZE = 0x00800000
DA = 0x10000000             # the dynamic area
DA_MAX = 48 * 1024 * 1024
SCREEN = 0x30000000
SW, SH = 640, 480          # the first mode (set_mode changes it)
SCREEN_MAP = 0x00800000    # room for two screens up to 1024x1024 (a mode change moves it)

SWI = dict(OS_Byte=0x06, OS_Word=0x07, OS_EnterOS=0x16, OS_Heap=0x1D, OS_Module=0x1E,
           OS_Claim=0x1F, OS_Release=0x20, OS_ReadVarVal=0x23, OS_SetVarVal=0x24, OS_ChangeDynamicArea=0x2A,
           OS_GenerateError=0x2B, OS_ReadVduVariables=0x31, OS_CallAVector=0x34,
           OS_CallAfter=0x3B, OS_CallEvery=0x3C, OS_RemoveTickerEvent=0x3D,
           OS_ReadMonotonicTime=0x42, OS_AddCallBack=0x54, OS_RemoveCallBack=0x5F,
           OS_DynamicArea=0x66, OS_SynchroniseCodeAreas=0x6E, OS_CallASWIR12=0x71,
           OS_LeaveOS=0x7C, TaskManager_TaskNameFromHandle=0x42680,
           CryptRandom_Block=0x51983)
SOCK = dict(Creat=0x41200, Bind=0x41201, Listen=0x41202, Connect=0x41204,
            Recv=0x41205, Send=0x41208, Shutdown=0x4120B, Setsockopt=0x4120C,
            Getsockopt=0x4120D, Close=0x41210, Select=0x41211, Ioctl=0x41212,
            Accept_1=0x4121B, Getpeername_1=0x4121F)
NAMES = {v: k for k, v in list(SWI.items()) + [("Socket_" + k, v) for k, v in SOCK.items()]}
# UnixLib's (BSD) errno numbers, which the module compares with
BSD_ERRNO = {errno.EAGAIN: 35, errno.EINPROGRESS: 36, errno.EPIPE: 32,
             errno.ECONNRESET: 54, errno.ENOTCONN: 57, errno.ECONNREFUSED: 61,
             errno.EADDRINUSE: 48, errno.EBADF: 9, errno.EINVAL: 22,
             errno.ECONNABORTED: 53, errno.ESHUTDOWN: 58}
FIONBIO = 0x8004667E


class Fatal(Exception):
    pass


class Heap:
    """OS_Heap, first fit, 8-byte blocks with a 4-byte size word before."""
    def __init__(self):
        self.base = self.size = 0
        self.free = []          # [(addr, size)]
        self.used = {}          # addr -> size

    def init(self, base, size):
        self.base, self.size = base, size
        self.free = [(base + 16, size - 16)]
        self.used = {}

    def grow(self, more):
        self.free.append((self.base + self.size, more))
        self.size += more
        self._merge()

    def _merge(self):
        self.free.sort()
        out = []
        for a, s in self.free:
            if out and out[-1][0] + out[-1][1] == a:
                out[-1] = (out[-1][0], out[-1][1] + s)
            else:
                out.append((a, s))
        self.free = out

    def claim(self, n):
        n = (n + 4 + 7) & ~7
        for i, (a, s) in enumerate(self.free):
            if s >= n:
                self.free[i] = (a + n, s - n)
                if s == n:
                    del self.free[i]
                self.used[a + 4] = n - 4
                return a + 4
        return 0

    def release(self, p):
        n = self.used.pop(p, None)
        if n is None:
            raise Fatal("OS_Heap 3 of a block that isn't claimed: &%X" % p)
        self.free.append((p - 4, n + 4))
        self._merge()


class RiscOS:
    def __init__(self, module_file, base=RMA + 0x1234 * 16, verbose=False):
        self.mu = mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for addr, size in ((LOW, 0x1000), (USER, USER_SIZE), (SVC_TOP - SVC_SIZE, SVC_SIZE),
                           (ERRS, 0x10000), (RMA, RMA_SIZE), (DA, DA_MAX),
                           (SCREEN, SCREEN_MAP)):
            mu.mem_map(addr, size)
        mu.mem_write(RET, b"\xfe\xff\xff\xea")
        mu.hook_add(UC_HOOK_INTR, self._swi_hook)
        mu.hook_add(UC_HOOK_MEM_INVALID, self._bad_mem)
        # RISC OS runs with alignment checking on: a 16/32-bit access that
        # isn't aligned to its size (LDRD/STRD and LDM/STM: to 4) aborts.
        # Unicorn doesn't trap them, so look at every access.
        self.align_check = os.environ.get("EMU_ALIGN", "1") != "0"
        if self.align_check:
            mu.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._align)
        mu.reg_write(A.UC_ARM_REG_CPSR, 0x13)       # SVC32, interrupts on
        self.verbose = verbose
        self.t0 = time.monotonic()
        self.rma_next = RMA + 0x00400000          # claims after the module
        self.err_next = ERRS
        self.heap = Heap()
        self.da_size = 0
        self.da_live = False
        self.tickers = []        # [addr, r12, period_cs or 0, due]
        self.callbacks = []      # [(addr, r12)]
        self.vectors = {}        # vector -> [(addr, r12)]
        self.keyv = []           # (down?, key)
        self.mouse = []          # (x, y) in OS units
        self.wheel = []
        self.vars = {}
        self.sockets = {}        # handle -> socket
        self.foreign_sockets = set()   # numbers "other programs" have
        self.alive_tasks = set()
        self.errorbox = False
        self.swilog = []
        self.fatal = None
        self.sw, self.sh, self.screen = SW, SH, SCREEN
        self.draw_screen()
        data = open(module_file, "rb").read()
        self.base = base
        self.module = data
        mu.mem_write(base, data)
        self.pw = self.rma_claim(4)
        mu.mem_write(self.pw, b"\0\0\0\0")
        hdr = struct.unpack_from("<13I", data, 0)
        self.hdr = hdr
        magic, ver, n = struct.unpack_from("<3I", data, 0x34)
        self.entries = struct.unpack_from("<%dI" % n, data, 0x40)

    # --- memory ---
    def _bad_mem(self, mu, access, addr, size, value, data):
        pc = mu.reg_read(A.UC_ARM_REG_PC)
        self.fatal = "bad memory access at &%X (pc &%X, module +&%X)" % (addr, pc, pc - self.base)
        return False

    def _align(self, mu, access, addr, size, value, data):
        need = 4 if size >= 4 else size
        if need > 1 and addr % need:
            pc = mu.reg_read(A.UC_ARM_REG_PC)
            if not self.fatal:
                self.fatal = ("unaligned %d-byte access at &%X (pc &%X, module +&%X): "
                              "an alignment fault on RISC OS" % (size, addr, pc, pc - self.base))
            mu.emu_stop()

    def w32(self, a, v):
        self.mu.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def r32(self, a):
        return struct.unpack("<I", self.mu.mem_read(a, 4))[0]

    def string(self, a, limit=512):
        b = bytes(self.mu.mem_read(a, limit))
        return b.split(b"\0", 1)[0].decode("latin-1")

    def rma_claim(self, n):
        a = self.rma_next
        self.rma_next += (n + 15) & ~15
        return a

    def error(self, num, msg):
        a = self.err_next
        if a > ERRS + 0xF000:
            a = self.err_next = ERRS
        self.w32(a, num)
        self.mu.mem_write(a + 4, msg.encode("latin-1") + b"\0")
        self.err_next += (4 + len(msg) + 1 + 3) & ~3
        return a

    def cs(self):
        return int((time.monotonic() - self.t0) * 100) + 0x1000

    # --- the screen: four quarters and a white square, as plat_host.c ---
    def set_mode(self, w, h):
        """A pretend mode change: a new size, and the screen moves."""
        self.sw, self.sh = w, h
        self.screen = SCREEN + 0x00400000 if self.screen == SCREEN else SCREEN
        self.draw_screen()

    def swap_bank(self):
        """Another screen bank shown (OS_Byte 113): same mode, same picture,
        a new DisplayStart."""
        old = self.screen
        self.screen = SCREEN + 0x00400000 if old == SCREEN else SCREEN
        self.mu.mem_write(self.screen, bytes(self.mu.mem_read(old, self.sw * self.sh * 4)))

    def draw_screen(self, box=None):
        SW, SH = self.sw, self.sh
        row = bytearray()
        for x in range(SW):
            row += b"\xff\x00\x00\x00" if x < SW // 2 else b"\x00\xff\x00\x00"
        low = bytearray()
        for x in range(SW):
            low += b"\x00\x00\xff\x00" if x < SW // 2 else b"\x80\x80\x80\x00"
        img = bytearray()
        for y in range(SH):
            r = bytearray(row if y < SH // 2 else low)
            if 64 <= y < 128:
                r[0:64 * 4] = b"\xff\xff\xff\x00" * 64
            img += r
        if box:     # an "error box": a filled rectangle (x0, y0, x1, y1, rgb)
            x0, y0, x1, y1, (cr, cg, cb) = box
            px = bytes((cr, cg, cb, 0))
            for y in range(y0, y1):
                img[(y * SW + x0) * 4:(y * SW + x1) * 4] = px * (x1 - x0)
        self.mu.mem_write(self.screen, bytes(img))

    # --- running code ---
    def _mode(self, mode):
        cpsr = self.mu.reg_read(A.UC_ARM_REG_CPSR)
        self.mu.reg_write(A.UC_ARM_REG_CPSR, (cpsr & 0x0FFFFF00) | mode)

    def call(self, addr, r=(), r12=None, max_insns=0, usr=False):
        """Calls addr: in SVC mode (sp = the empty SVC stack), as the kernel
        calls a module's handlers, or with usr=True in USR mode (sp = a user
        stack), as a program calls the module's table. Either way the user
        sp and lr (the current task's) must come back unchanged."""
        mu = self.mu
        regs = list(r) + [0] * (13 - len(r))
        if r12 is not None:
            regs[12] = r12
        self._mode(0x13)
        mu.reg_write(A.UC_ARM_REG_SP, SVC_TOP)
        mu.reg_write(A.UC_ARM_REG_LR, RET if not usr else 0xBAD00000)
        self._mode(0x10)
        mu.reg_write(A.UC_ARM_REG_SP, USER_TOP)
        mu.reg_write(A.UC_ARM_REG_LR, RET if usr else USER_LR)
        if not usr:
            self._mode(0x13)
        for i, x in enumerate(REGS):
            mu.reg_write(x, regs[i] & 0xFFFFFFFF)
        try:
            mu.emu_start(addr, RET, count=max_insns)
        except Exception as e:
            pc = mu.reg_read(A.UC_ARM_REG_PC)
            raise Fatal("%s at pc &%X (module +&%X)%s" % (e, pc, pc - self.base,
                        ": " + self.fatal if self.fatal else ""))
        if self.fatal:
            raise Fatal(self.fatal)
        pc = mu.reg_read(A.UC_ARM_REG_PC)
        if pc != RET:
            raise Fatal("code at &%X didn't return (pc &%X, module +&%X)" % (addr, pc, pc - self.base))
        cpsr = mu.reg_read(A.UC_ARM_REG_CPSR)
        out = [mu.reg_read(x) for x in REGS]
        if (cpsr & 0x1F) != (0x10 if usr else 0x13):
            raise Fatal("code at &%X returned in mode &%X" % (addr, cpsr & 0x1F))
        if mu.reg_read(A.UC_ARM_REG_SP) != (USER_TOP if usr else SVC_TOP):
            raise Fatal("code at &%X left the stack unbalanced" % addr)
        self._mode(0x10)
        usp, ulr = mu.reg_read(A.UC_ARM_REG_SP), mu.reg_read(A.UC_ARM_REG_LR)
        self._mode(0x13)
        if usp != USER_TOP or (not usr and ulr != USER_LR):
            raise Fatal("code at &%X changed the current task's user sp/lr "
                        "(&%X/&%X)" % (addr, usp, ulr))
        self._mode(cpsr & 0x1F)
        return out, cpsr

    def init(self):
        regs, cpsr = self.call(self.base + self.hdr[1], r12=self.pw)
        if cpsr & VFLAG:
            return self.string(regs[0] + 4)
        return None

    def final(self, fatal=1):
        regs, cpsr = self.call(self.base + self.hdr[2], r=[0] * 10 + [fatal], r12=self.pw)
        if cpsr & VFLAG:
            return self.string(regs[0] + 4)
        return None

    def service(self, r0, r1, r2=0):
        regs, _ = self.call(self.base + self.hdr[3], r=[r0, r1, r2], r12=self.pw)
        return regs

    def entry(self, n, *args):
        """Calls entry n of the front end's table with R12 = the workspace."""
        ws = self.r32(self.pw)
        regs, _ = self.call(self.base + self.entries[n], r=list(args), r12=ws, usr=True)
        return regs[0] if regs[0] < 0x80000000 else regs[0] - (1 << 32)

    def bytev(self, r0, r1, r2):
        """OS_Byte through the module's ByteV claimant (it passes on, so
        nothing else to do)."""
        for addr, ws in list(self.vectors.get(6, [])):
            self.call(addr, r=[r0, r1, r2], r12=ws)

    def run_once(self):
        """One go of the kernel's part: due tickers, then (unless an error
        box is open) callbacks; in an error box, one pass of its loop."""
        now = self.cs()
        for t in list(self.tickers):
            if t not in self.tickers:
                continue
            if now >= t[3]:
                if t[2]:
                    t[3] = now + t[2]
                else:
                    self.tickers.remove(t)
                self.call(t[0], r12=t[1])
        if self.errorbox:
            self.bytev(0x81, 0, 0)
        else:
            while self.callbacks:
                addr, ws = self.callbacks.pop(0)
                self.call(addr, r12=ws)

    # --- SWIs ---
    def _swi_hook(self, mu, intno, _):
        if intno != 2:
            pc = mu.reg_read(A.UC_ARM_REG_PC)
            self.fatal = "exception %d at &%X (module +&%X)" % (intno, pc, pc - self.base)
            mu.emu_stop()
            return
        pc = mu.reg_read(A.UC_ARM_REG_PC)
        num = struct.unpack("<I", mu.mem_read(pc - 4, 4))[0] & 0xFFFFFF
        r = [mu.reg_read(x) for x in REGS[:10]]
        if (num & ~XBIT) == SWI["OS_CallASWIR12"]:
            num = mu.reg_read(A.UC_ARM_REG_R12) & 0xFFFFFF
        cpsr = mu.reg_read(A.UC_ARM_REG_CPSR) & ~VFLAG
        # RISC OS: a SWI called in SVC mode must have sp on the SVC stack
        # (8 KB on RISC OS 5): C modules (Internet...) find their static
        # data from its base, so anywhere else they abort (0.2-test1 on the
        # Pi: inside Internet +&4A8).
        sp = mu.reg_read(A.UC_ARM_REG_SP)
        if (cpsr & 0x1F) == 0x13 and not (SVC_TOP - SVC_REAL <= sp <= SVC_TOP):
            self.fatal = ("SWI %s called with sp &%X, not on the SVC stack (pc &%X, "
                          "module +&%X)" % (NAMES.get(num & ~XBIT, "&%X" % num), sp, pc,
                                            pc - self.base))
            mu.emu_stop()
            return
        try:
            err = self.swi(num & ~XBIT, r)
        except Fatal as e:
            self.fatal = str(e)
            mu.emu_stop()
            return
        if err is not None:
            if not num & XBIT:
                self.fatal = "error from non-X SWI &%X: %s" % (num, self.string(err + 4))
                mu.emu_stop()
                return
            r[0] = err
            cpsr |= VFLAG
        for x, v in zip(REGS[:10], r):
            mu.reg_write(x, v & 0xFFFFFFFF)
        if (num & ~XBIT) == SWI["OS_EnterOS"]:
            cpsr = (cpsr & ~0x1F) | 0x13
        elif (num & ~XBIT) == SWI["OS_LeaveOS"]:
            cpsr = (cpsr & ~0x1F) | 0x10
        mu.reg_write(A.UC_ARM_REG_CPSR, cpsr)

    def sock_error(self, e):
        n = BSD_ERRNO.get(getattr(e, "errno", None) or errno.EIO, 5)
        return self.error(0x20E00 | n, os.strerror(getattr(e, "errno", 5) or 5))

    def swi(self, n, r):
        name = NAMES.get(n, "&%X" % n)
        self.swilog.append(name)
        if len(self.swilog) > 2000:
            del self.swilog[:1000]
        if n in (SWI["OS_EnterOS"], SWI["OS_LeaveOS"], SWI["OS_SynchroniseCodeAreas"]):
            return None
        if n == SWI["OS_ReadMonotonicTime"]:
            r[0] = self.cs()
        elif n == SWI["OS_Byte"]:
            if r[0] in (121, 202):
                r[1] = 0
            elif r[0] == 0x81:
                r[1], r[2] = 0, 0xFF
            else:
                raise Fatal("OS_Byte %d" % r[0])
        elif n == SWI["OS_Word"]:
            if r[0] == 21:
                b = bytes(self.mu.mem_read(r[1], 5))
                if b[0] == 3:
                    self.mouse.append((b[1] | b[2] << 8, b[3] | b[4] << 8))
            elif r[0] == 14:
                cs = int((time.time() + 2208988800) * 100)
                if self.mu.mem_read(r[1], 1)[0] == 3:
                    self.mu.mem_write(r[1], cs.to_bytes(5, "little"))
            else:
                raise Fatal("OS_Word %d" % r[0])
        elif n == SWI["OS_ReadVduVariables"]:
            vals = {0: 0x40, 3: 0xFFFFFFFF, 4: 1, 5: 1, 6: self.sw * 4, 9: 5,
                    11: self.sw - 1, 12: self.sh - 1, 149: self.screen}
            i = 0
            while True:
                v = self.r32(r[0] + 4 * i)
                if v == 0xFFFFFFFF:
                    break
                self.w32(r[1] + 4 * i, vals[v])
                i += 1
        elif n == SWI["OS_CallAVector"]:
            if r[9] == 19:
                self.keyv.append((r[0] == 2, r[1]))
                if self.verbose:
                    print("emu: KeyV %s &%02X" % ("down" if r[0] == 2 else "up", r[1]))
            elif r[9] == 38:
                self.wheel.append((r[1], r[3]))
            else:
                raise Fatal("OS_CallAVector %d" % r[9])
        elif n == SWI["OS_SetVarVal"]:
            # string variables only (types 0, 4); length -1 deletes (PRM p489)
            name = self.string(r[0])
            if r[2] & 0x80000000:
                if name not in self.vars:
                    return self.error(0x124, "System variable '%s' not found" % name)
                del self.vars[name]
            else:
                if r[4] not in (0, 4):         # strings (4: literal)
                    raise Fatal("OS_SetVarVal type %d" % r[4])
                self.vars[name] = bytes(self.mu.mem_read(r[1], r[2])).decode("latin-1")
        elif n == SWI["OS_ReadVarVal"]:
            # no wildcards, no expansion: a string variable comes back as it is
            name = self.string(r[0])
            if r[3] != 0 or name not in self.vars:
                r[2] = 0
                # (the number is made up: the module mustn't rely on it)
                return self.error(0x1E7, "System variable '%s' not found" % name)
            v = self.vars[name].encode("latin-1")
            if r[2] & 0x80000000:          # length asked: NOT length, and an error
                r[2] = ~len(v) & 0xFFFFFFFF
                return self.error(0x1E4, "Buffer overflow")
            if len(v) > r[2]:
                r[2] = ~len(v) & 0xFFFFFFFF
                return self.error(0x1E4, "Buffer overflow")
            self.mu.mem_write(r[1], v)
            r[2] = len(v)
            r[4] = 0
        elif n == SWI["OS_Module"]:
            if r[0] == 6:
                r[2] = self.rma_claim(r[3])
            elif r[0] != 7:
                raise Fatal("OS_Module %d" % r[0])
        elif n == SWI["OS_DynamicArea"]:
            if r[0] == 0:
                if self.da_live:
                    return self.error(0x1E6, "only one area here")
                self.da_live = True
                self.da_size = r[2]
                r[1], r[3], r[5] = 300, DA, min(r[5], DA_MAX)
                self.mu.mem_write(DA, b"\xa5" * 0x10000)
            elif r[0] == 1:
                self.da_live = False
                self.da_size = 0
            else:
                raise Fatal("OS_DynamicArea %d" % r[0])
        elif n == SWI["OS_ChangeDynamicArea"]:
            if self.da_size + r[1] > DA_MAX:
                return self.error(0x1E6, "area full")
            self.da_size += r[1]
        elif n == SWI["OS_Heap"]:
            if r[0] == 0:
                self.heap.init(r[1], r[3])
            elif r[0] == 2:
                a = self.heap.claim(r[3])
                if not a:
                    return self.error(0x184, "Heap full")
                r[2] = a
            elif r[0] == 3:
                self.heap.release(r[2])
            elif r[0] == 5:
                self.heap.grow(r[3])
            elif r[0] == 6:
                if r[2] not in self.heap.used:
                    raise Fatal("OS_Heap 6 of &%X" % r[2])
                r[3] = self.heap.used[r[2]]
            else:
                raise Fatal("OS_Heap %d" % r[0])
        elif n == SWI["OS_Claim"]:
            self.vectors.setdefault(r[0], []).insert(0, (r[1], r[2]))
        elif n == SWI["OS_Release"]:
            v = self.vectors.get(r[0], [])
            if (r[1], r[2]) in v:
                v.remove((r[1], r[2]))
            else:
                return self.error(0x1A1, "Bad vector release")
        elif n == SWI["OS_CallEvery"]:
            self.tickers.append([r[1], r[2], r[0] + 1, self.cs() + r[0] + 1])
        elif n == SWI["OS_CallAfter"]:
            self.tickers.append([r[1], r[2], 0, self.cs() + r[0]])
        elif n == SWI["OS_RemoveTickerEvent"]:
            self.tickers = [t for t in self.tickers if not (t[0] == r[0] and t[1] == r[1])]
        elif n == SWI["OS_AddCallBack"]:
            self.callbacks.append((r[0], r[1]))
        elif n == SWI["OS_RemoveCallBack"]:
            self.callbacks = [c for c in self.callbacks if c != (r[0], r[1])]
        elif n == SWI["TaskManager_TaskNameFromHandle"]:
            if r[0] not in self.alive_tasks:
                return self.error(0x1E6, "no such task")
            r[0] = self.rma_claim(8)
            self.mu.mem_write(r[0], b"Task\0")
        elif n == SWI["CryptRandom_Block"]:
            self.mu.mem_write(r[0], os.urandom(r[1]))
        elif n == SWI["OS_GenerateError"]:
            raise Fatal("OS_GenerateError: " + self.string(r[0] + 4))
        elif (n & ~0x3F) == 0x41200:
            return self.socket_swi(n, r)
        else:
            raise Fatal("unexpected SWI %s" % name)
        return None

    def new_handle(self):
        """The lowest free number, from 0, as the Internet module gives
        them (shared with every other program's sockets)."""
        h = 0
        while h in self.sockets or h in self.foreign_sockets:
            h += 1
        return h

    def sockaddr_out(self, addr, lenp, peer):
        ip, port = peer[0], peer[1]
        b = bytes([16, 2]) + struct.pack(">H", port) + socket.inet_aton(ip) + bytes(8)
        if addr:
            self.mu.mem_write(addr, b)
        if lenp:
            self.w32(lenp, 16)

    def socket_swi(self, n, r):
        s = self.sockets.get(r[0])
        try:
            if n == SOCK["Creat"]:
                if (r[0], r[1]) != (2, 1):
                    return self.error(0x20E00 | 47, "not TCP/IPv4")
                h = self.new_handle()
                self.sockets[h] = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                r[0] = h
                return None
            if n == SOCK["Select"]:
                return self.socket_select(r)
            if s is None:
                return self.error(0x20E00 | 9, "bad socket %d" % r[0])
            if n == SOCK["Bind"]:
                b = bytes(self.mu.mem_read(r[1], 8))
                port = struct.unpack(">H", b[2:4])[0]
                ip = socket.inet_ntoa(b[4:8])
                s.bind((ip, port))
                r[0] = 0
            elif n == SOCK["Listen"]:
                s.listen(r[1])
                r[0] = 0
            elif n == SOCK["Accept_1"]:
                c, peer = s.accept()
                h = self.new_handle()
                c.setblocking(True)
                self.sockets[h] = c
                self.sockaddr_out(r[1], r[2], peer)
                r[0] = h
            elif n == SOCK["Recv"]:
                if r[3] not in (0, 2):
                    raise Fatal("recv flags %d" % r[3])
                data = s.recv(r[2], socket.MSG_PEEK if r[3] == 2 else 0)
                self.mu.mem_write(r[1], data)
                r[0] = len(data)
            elif n == SOCK["Send"]:
                data = bytes(self.mu.mem_read(r[1], r[2]))
                r[0] = s.send(data, socket.MSG_NOSIGNAL)
            elif n == SOCK["Shutdown"]:
                s.shutdown(r[1])
                r[0] = 0
            elif n == SOCK["Close"]:
                s.close()
                del self.sockets[r[0]]
                r[0] = 0
            elif n == SOCK["Setsockopt"]:
                val = self.r32(r[3])
                if r[1] == 0xFFFF and r[2] == 4:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, val)
                elif r[1] == 6 and r[2] == 1:
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, val)
                r[0] = 0
            elif n == SOCK["Getsockopt"]:
                if r[1] == 0xFFFF and r[2] == 4103:
                    self.w32(r[3], s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR))
                else:
                    self.w32(r[3], 0)
                r[0] = 0
            elif n == SOCK["Getpeername_1"]:
                self.sockaddr_out(r[1], r[2], s.getpeername())
                r[0] = 0
            elif n == SOCK["Ioctl"]:
                if r[1] != FIONBIO:
                    raise Fatal("Socket_Ioctl &%X" % r[1])
                s.setblocking(self.r32(r[2]) == 0)
                r[0] = 0
            elif n == SOCK["Select"]:
                return self.socket_select(r)
            else:
                raise Fatal("socket SWI &%X" % n)
        except (OSError, socket.error) as e:
            if isinstance(e, Fatal):
                raise
            return self.sock_error(e)
        return None

    def socket_select(self, r):
        nfds, rp, wp, ep, tvp = r[0], r[1], r[2], r[3], r[4]
        tv = 0.0
        if tvp:
            tv = self.r32(tvp) + self.r32(tvp + 4) / 1e6
        if tv != 0:
            raise Fatal("Socket_Select with a timeout (%.2f s): the module waited" % tv)

        def bits(p):
            if not p:
                return []
            b = bytes(self.mu.mem_read(p, 32))
            return [i for i in range(min(nfds, 256)) if b[i // 8] & (1 << (i % 8))]
        rd, wr = bits(rp), bits(wp)
        rs = [self.sockets[h] for h in rd if h in self.sockets]
        ws = [self.sockets[h] for h in wr if h in self.sockets]
        a, b, _ = select.select(rs, ws, [], 0)
        count = 0
        for p, hs, ready in ((rp, rd, a), (wp, wr, b)):
            if not p:
                continue
            out = bytearray(32)
            for h in hs:
                if h in self.sockets and self.sockets[h] in ready:
                    out[h // 8] |= 1 << (h % 8)
                    count += 1
            self.mu.mem_write(p, bytes(out))
        if ep:
            self.mu.mem_write(ep, bytes(32))
        r[0] = count
        return None

    # --- the front end's side ---
    def ubuf(self, data=b"", size=None, at=0):
        """A buffer in the "front end's" memory (USER + at)."""
        a = USER + at
        if size is None:
            size = len(data)
        self.mu.mem_write(a, bytes(data) + bytes(max(0, size - len(data))))
        return a
