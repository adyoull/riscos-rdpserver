#!/usr/bin/env python3
"""RDPServer: the module (build/module/RDPServer,ffa) in the emulator
(tests/modemu.py), without clients: loading at two addresses, RMReInit,
the front end's entries, the ticker and callback, the error box path
(ByteV, no callbacks), and finalisation leaving nothing behind.

    tests/test_module.py build/module/RDPServer,ffa build/tests/keys
(the second: a folder with an rsakeys.ini, as the host build makes)

Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
"""
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from modemu import RiscOS, RMA, USER, Fatal  # noqa: E402

ATTACH, DETACH, SET_FILE, START, STOP, DISCONNECT, RELOAD, STATUS, MESSAGES = range(9)
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


MOD, KEYS = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
INI = open(os.path.join(HERE, "..", "app", "!RDPServer", "xrdp.ini"), "rb").read()
RSA = open(os.path.join(KEYS, "rsakeys.ini"), "rb").read()
INI_PATH = b"<RDPServer$Dir>/xrdp.ini"
KEYS_PATH = b"<RDPServer$Choices>/rsakeys.ini"
LOGINS_PATH = b"<RDPServer$Choices>/logins"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def set_file(m, path, data):
    a = m.ubuf(path + b"\0", at=0x1000)
    d = m.ubuf(data, at=0x2000) if data is not None else 0
    return m.entry(SET_FILE, a, d, len(data) if data is not None else 0)


def options(m, port, verbose=0):
    # rdps_mod_options: port[8] address[64] ini[256] logins[256], 3 unsigned, 2 int
    b = struct.pack("<8s64s256s256sIIIii", str(port).encode(), b"127.0.0.1", INI_PATH,
                    LOGINS_PATH, 10, 25, 3000, 128, verbose)
    return m.ubuf(b, at=0x8000)


def messages(m, seq=[0]):
    sp = m.ubuf(struct.pack("<I", seq[0]), at=0x10000)
    buf = m.ubuf(size=8192, at=0x11000)
    n = m.entry(MESSAGES, sp, buf, 8192)
    seq[0] = m.r32(sp)
    return bytes(m.mu.mem_read(buf, n)).decode("latin-1") if n > 0 else ""


def status(m):
    a = m.ubuf(size=1024, at=0x20000)
    m.entry(STATUS, a)
    # server_status: listening, connecting, connected, login_set, client[64],
    # user[64], width, height, bpp, tiles_sent; (interface 4) at_login,
    # hostname[32], protocol[32], security[96], screen[48], keepalive[64],
    # sound[64], connected_s, kbytes_sent; then the module's fields
    fmt = "<4i64s64s4Ii32s32s96s48s64s64s2I"
    v = struct.unpack_from(fmt, bytes(m.mu.mem_read(a, struct.calcsize(fmt))))
    extra = struct.unpack_from("<iIIIi", bytes(m.mu.mem_read(a + struct.calcsize(fmt), 20)))
    text = lambda b: b.split(b"\0")[0].decode("latin-1")
    return dict(listening=v[0], connecting=v[1], connected=v[2], login_set=v[3],
                screen=text(v[14]), kbytes=v[18],
                errorbox=extra[0], callbacks=extra[1], errorbox_steps=extra[2],
                heap=extra[3], dead=extra[4])


def run_for(m, cs):
    end = time.monotonic() + cs / 100
    while time.monotonic() < end:
        m.run_once()
        time.sleep(0.003)


# --- 1: load, initialise ---------------------------------------------------
base1 = RMA + 0x1234 * 16 + 4      # not 8-aligned: only 4 is promised
m = RiscOS(MOD, base=base1)
m.foreign_sockets = {0}            # another program's socket: ours get 1, 2, ...
err = m.init()
check(err is None, "initialised (%s)" % err)
check(m.r32(m.pw) != 0, "the private word holds the workspace")
check(len(m.vectors.get(6, [])) == 1, "ByteV claimed")
tick = [t for t in m.tickers if t[2] == 2]
check(len(tick) == 1, "a ticker every 2 cs")
check(m.da_live, "a dynamic area for the heap")
msgs = messages(m)
check("module started" in msgs, "a message saying so (%r)" % msgs)

# --- 2: the front end -------------------------------------------------------
m.alive_tasks = {0x1234, 0x5678}
check(m.entry(ATTACH, 0x1234) == 0, "attach")
check(m.entry(ATTACH, 0x5678) == 1, "a second front end is turned away while the first lives")
check(set_file(m, INI_PATH, INI) == 0, "xrdp.ini given")
check(set_file(m, KEYS_PATH, RSA) == 0, "rsakeys.ini given")
check(set_file(m, LOGINS_PATH, b"") == 0, "an empty logins file given")
port = free_port()
r = m.entry(START, options(m, port))
msgs = messages(m)
check(r == 0, "started (%s)" % msgs)
check("listening on port %d" % port in msgs, "listening (%r)" % msgs)
# NEON, measured at start-up only (0.2-test28): in the module's own VFP
# context (the emulator aborts NEON outside one, and fails any call that
# returns with it still active), giving the same pixels as pix_convert
check("NEON: its own VFP context, a switch there and back" in msgs and m.vfp_switches >= 2,
      "NEON: the module made its own VFP context and switched to it and back (%d switches)"
      % m.vfp_switches)
check(re.search(r"screen read: single words \d+ MB/s, LDM 8 \d+ MB/s, NEON \d+ MB/s", msgs),
      "the screen read by NEON too (%r)" % [x for x in msgs.split("\n") if "screen read" in x])
check("converting by NEON (32 bpp screens" in msgs,
      "and converting by NEON is then used (%r)" % [x for x in msgs.split("\n") if "converting by" in x])
conv = [x for x in msgs.split("\n") if "other ways" in x]
check(len(conv) == 2 and all("NEON" in x and "(the same pixels)" in x and "DIFFERENT" not in x
                             for x in conv),
      "converting by a simpler loop and by NEON: the same pixels, at 32 and 16 bpp (%r)" % conv)
st = status(m)
check(st["listening"] == 1 and st["connected"] == 0, "status: listening, no client")
try:
    c = socket.create_connection(("127.0.0.1", port), timeout=2)
    c.close()
    check(True, "the port takes connections")
except OSError as e:
    check(False, "the port takes connections (%s)" % e)

# --- 3: callbacks run the server --------------------------------------------
before = status(m)["callbacks"]
run_for(m, 30)
after = status(m)["callbacks"]
check(after > before, "the callback ran the server (%d -> %d)" % (before, after))

# --- 4: an error box: no callbacks, ByteV instead ---------------------------
m.service(1, 0x57)
m.errorbox = True        # and the kernel runs no callbacks meanwhile
check(status(m)["errorbox"] == 1, "Service_WimpReportError 1: the module knows a box is open")
b0, c0 = status(m)["errorbox_steps"], status(m)["callbacks"]
run_for(m, 80)     # an idle server runs every 25 cs
st = status(m)
check(st["errorbox_steps"] > b0, "inside the error box the server ran from ByteV (%d)" % st["errorbox_steps"])
check(st["callbacks"] == c0, "and not from a callback")
# a client connecting during the box is accepted (the server isn't frozen)
c = socket.create_connection(("127.0.0.1", port), timeout=2)
run_for(m, 60)     # an idle server looks every 25 cs
st = status(m)
check(st["connecting"] == 1, "a client connecting during the box is taken on (%s %r)" % (st, messages(m)))
# the front end drops it (force) while it's mid-handshake
m.entry(DISCONNECT)
st = status(m)
check(st["connecting"] == 0 and st["connected"] == 0, "Disconnect drops it at once")
c.settimeout(2)
try:
    gone = c.recv(10) == b""
except OSError:
    gone = True
check(gone, "and its connection is closed")
c.close()
m.service(0, 0x57)
m.errorbox = False
check(status(m)["errorbox"] == 0, "Service_WimpReportError 0: closed")
# ByteV with other reasons, or outside a box, does nothing
b0 = status(m)["errorbox_steps"]
m.bytev(0x81, 0, 0)
m.bytev(0x79, 0x80, 0)
check(status(m)["errorbox_steps"] == b0, "ByteV outside a box: nothing")

# --- 5: a connection that never finishes doesn't hold anything up ----------
c = socket.create_connection(("127.0.0.1", port), timeout=2)
c.sendall(b"\x03\x00")     # half a TPKT header, then nothing
slowest = 0.0
end = time.monotonic() + 0.5
while time.monotonic() < end:
    t = time.monotonic()
    m.run_once()
    slowest = max(slowest, time.monotonic() - t)
    time.sleep(0.003)
# (in the emulator, with every memory access checked for alignment: a
# turn that waited for the client would take 10 cs or more on its own)
check(slowest < 0.3, "every turn returns quickly during a stalled handshake (%.0f ms)" % (slowest * 1000))
m.entry(DISCONNECT)
c.close()

# --- 5b: logins changed while a client is mid-handshake ---------------------
c = socket.create_connection(("127.0.0.1", port), timeout=2)
c.sendall(b"\x03\x00")
run_for(m, 60)
check(status(m)["connecting"] == 1, "a client is mid-handshake")
check(set_file(m, LOGINS_PATH, b"late " + b"11" * 16 + b" " + b"22" * 32 + b"\n") == 0
      and m.entry(RELOAD) == 0, "new logins reloaded at once, mid-handshake")
check(status(m)["login_set"] == 1, "and in force before that client's login is checked")
set_file(m, LOGINS_PATH, b"")
check(m.entry(RELOAD) == 0 and status(m)["login_set"] == 0, "cleared again")
m.entry(DISCONNECT)
c.close()

# --- 6: logins: reload after SET_FILE ---------------------------------------
check(set_file(m, LOGINS_PATH, b"test " + b"00" * 16 + b" " + b"00" * 32 + b"\n") == 0, "logins changed")
check(m.entry(RELOAD) == 0, "reloaded")
check(status(m)["login_set"] == 1, "a login is now needed")

# --- 6b: a program stopped (Alt-Break) while the thread ran in its time ----
# (owner, 14 Oct 2026: Folio ran away, Alt-Break stopped it, and the
# server never ran again.) The kernel runs transient callbacks on a
# return to USR mode, where the thread runs: one that stops the current
# task comes inside the thread. The safety net starts the server again.
messages(m)
m.kill_armed = True
run_for(m, 40)
m.kill_armed = False
check(m.killed_in_thread == 1, "a task stop came inside the server thread (USR mode)")
run_for(m, 60)
msgs = messages(m)
check("the server was lost" in msgs and "listening on port %d" % port in msgs,
      "the server was started again (%r)" % msgs[-200:])
check(status(m)["listening"] == 1, "listening again")
# The safety net: the thread thrown away in the middle of a SWI anyway.
m.unwind_next = True
run_for(m, 60)
check(m.unwound == 1, "the thread was thrown away in a SWI (the test's doing)")
msgs = messages(m)
check("the server was lost" in msgs, "the module noticed (%r)" % msgs[-200:])
check("listening on port %d" % port in msgs, "and started the server again (%r)" % msgs[-200:])
check(status(m)["listening"] == 1, "listening again")
try:
    c = socket.create_connection(("127.0.0.1", port), timeout=2)
    run_for(m, 60)     # an idle server looks every 25 cs
    st = status(m)
    check(m.sndbuf == 32 * 1024, "the client's send buffer: 64 KB refused, 32 KB taken (%d)" % m.sndbuf)
    check(st["connecting"] == 1, "and a client is taken on (%r %r)" % (st, messages(m)[-300:]))
    c.close()
    run_for(m, 20)
except OSError as e:
    check(False, "the port takes connections again (%s)" % e)

# Lost again, and the front end goes (Service_WimpCloseDown) before any
# callback: the server mustn't start again behind its back.
m.unwind_next = True
for i in range(200):
    if m.unwound == 2:
        break
    m.run_once()
    time.sleep(0.003)
check(m.unwound == 2, "thrown away again")
m.service(0, 0x53, 0x1234)            # Service_WimpCloseDown, the front end
run_for(m, 60)
msgs = messages(m)
check(status(m)["listening"] == 0, "the front end went: not started again (%r)" % msgs[-200:])
m.vars["RDPServer$ExtraTurns"] = "2"
check(m.entry(ATTACH, 0x1234) == 0 and m.entry(START, options(m, port)) == 0,
      "attached and started again by the front end")
check("extra turns: 2" in messages(m), "RDPServer$ExtraTurns read at the start")
del m.vars["RDPServer$ExtraTurns"]
# RDPServer$ThreadMode SYS (comparing): the thread runs in SYS mode,
# where no callbacks come inside it
m.entry(STOP)
m.vars["RDPServer$ThreadMode"] = "SYS"
check(m.entry(START, options(m, port)) == 0 and "runs in SYS mode" in messages(m),
      "RDPServer$ThreadMode SYS: started, and said so")
m.kill_armed = True
run_for(m, 40)
m.kill_armed = False
check(m.killed_in_thread == 1, "SYS mode: no task stop came inside the thread")
try:
    c = socket.create_connection(("127.0.0.1", port), timeout=2)
    run_for(m, 60)
    check(status(m)["connecting"] == 1, "SYS mode: a client is taken on")
    c.close()
    run_for(m, 20)
except OSError as e:
    check(False, "SYS mode: the port takes connections (%s)" % e)
m.entry(STOP)
del m.vars["RDPServer$ThreadMode"]
check(m.entry(START, options(m, port)) == 0 and "runs in SYS mode" not in messages(m),
      "and back in USR mode")
check(status(m)["listening"] == 1, "listening")

# --- 7: stop, detach, finalise ----------------------------------------------
m.entry(STOP)
check(status(m)["listening"] == 0, "stopped")
check("stopped" in messages(m), "said so")
check(m.final(1) is not None, "finalisation refused while the front end is attached and alive")
check(m.final(0) is not None, "RMTidy (non-fatal) refused")
m.entry(DETACH, 0)
open_socks = len(m.sockets)
check(not m.sockets, "every socket closed on stop, numbers 1 and 2 included (%s open)" % sorted(m.sockets))
err = m.final(1)
check(err is None, "finalised (%s)" % err)
check(not m.vectors.get(6), "ByteV released")
check(not [t for t in m.tickers if t[2] == 2], "the ticker removed")
check(not m.callbacks, "no callback left")
check(not m.da_live, "the dynamic area removed")
check(len(m.sockets) == 0, "every socket closed (%d were open)" % open_socks)

# --- 8: RMReInit: initialise the same copy again ----------------------------
err = m.init()
check(err is None, "initialised again (%s)" % err)
m.alive_tasks = {0x1234}
check(m.entry(ATTACH, 0x1234) == 0, "attach again")
set_file(m, INI_PATH, INI)
set_file(m, KEYS_PATH, RSA)
set_file(m, LOGINS_PATH, b"")
port = free_port()
check(m.entry(START, options(m, port)) == 0, "started again (%s)" % messages(m))
run_for(m, 10)
m.entry(DETACH, 0)
check(m.final(1) is None, "finalised again")

# --- 9: another address -----------------------------------------------------
m2 = RiscOS(MOD, base=RMA + 0x3000)
check(m2.init() is None, "initialised at another address")
m2.alive_tasks = {1}
m2.entry(ATTACH, 1)
set_file(m2, INI_PATH, INI)
set_file(m2, KEYS_PATH, RSA)
set_file(m2, LOGINS_PATH, b"")
port = free_port()
check(m2.entry(START, options(m2, port)) == 0, "and started there")
m2.entry(DETACH, 0)
check(m2.final(1) is None, "finalised there")

# --- 10: the front end goes without detaching (Service_WimpCloseDown) ------
m3 = RiscOS(MOD, base=RMA + 0x8000)
m3.init()
m3.alive_tasks = {7}
m3.entry(ATTACH, 7)
set_file(m3, INI_PATH, INI)
set_file(m3, KEYS_PATH, RSA)
set_file(m3, LOGINS_PATH, b"")
m3.entry(START, options(m3, free_port()))
m3.service(0, 0x53, 7)
m3.alive_tasks = set()
check(status(m3)["listening"] == 0, "the front end's Wimp_CloseDown stops the server")
check(m3.final(1) is None, "and the module can then be killed")

# --- 11: the desktop shutting down: DETACH with keep (0.2-test23) ------------
# (and a RISC OS without VFPSupport: no NEON, nothing else different)
os.environ["EMU_NO_VFP"] = "1"
m4 = RiscOS(MOD, base=RMA + 0xC000)
del os.environ["EMU_NO_VFP"]
m4.init()
m4.alive_tasks = {9}
m4.entry(ATTACH, 9)
set_file(m4, INI_PATH, INI)
set_file(m4, KEYS_PATH, RSA)
set_file(m4, LOGINS_PATH, b"")
port = free_port()
m4.entry(START, options(m4, port))
run_for(m4, 10)
msgs = messages(m4, [0])
check("NEON: not here" in msgs and "listening on port %d" % port in msgs,
      "without VFPSupport: NEON not here, serving as ever (%r)"
      % [x for x in msgs.split("\n") if "NEON" in x])
m4.entry(DETACH, 1)
m4.service(0, 0x53, 9)                 # and then its Wimp_CloseDown
m4.alive_tasks = set()
run_for(m4, 10)
msgs = messages(m4)
check(status(m4)["listening"] == 1 and "the desktop is shutting down: serving on" in msgs,
      "kept: serving on with no front end, past its Wimp_CloseDown (%r)" % msgs[-200:])
m4.alive_tasks = {11}
check(m4.entry(ATTACH, 11) == 0, "a new front end can attach")
check(status(m4)["listening"] == 0, "and takes over: the old server stopped")
m4.entry(DETACH, 0)
check(m4.final(1) is None, "and the module can then be killed")

# --- 12: RDPServer$NEON 0 (0.2-test30): no NEON at all, even with the
# screen read forced to NEON, and even after a server run that used it ---
m5 = RiscOS(MOD, base=RMA + 0x10000)
m5.init()
m5.alive_tasks = {11}
m5.entry(ATTACH, 11)
set_file(m5, INI_PATH, INI)
set_file(m5, KEYS_PATH, RSA)
set_file(m5, LOGINS_PATH, b"")
port = free_port()
m5.entry(START, options(m5, port))
run_for(m5, 5)
msgs = messages(m5, [0])
check("converting by NEON" in msgs and m5.vfp_contexts, "a first run uses NEON (%r)"
      % [x for x in msgs.split("\n") if "NEON" in x])
m5.entry(STOP)
m5.vars["RDPServer$NEON"] = "0"
m5.vars["RDPServer$ScreenCopy"] = "3"
switches = m5.vfp_switches
seq = [0]
messages(m5, seq)
m5.entry(START, options(m5, port))
run_for(m5, 5)
msgs = messages(m5, seq)
check("NEON: not used (RDPServer$NEON 0)" in msgs and "screen read by NEON" not in msgs
      and "converting by NEON" not in msgs and not m5.vfp_contexts and m5.vfp_switches == switches,
      "RDPServer$NEON 0: its context destroyed, NEON neither read with nor converted with (%r)"
      % [x for x in msgs.split("\n") if "NEON" in x or "screen read by" in x or "converting by" in x])
m5.entry(DETACH, 0)
check(m5.final(1) is None, "and the module can then be killed")

print("RDPServer module (emulated ARM): %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
