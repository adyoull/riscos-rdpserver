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
    a = m.ubuf(size=512, at=0x20000)
    m.entry(STATUS, a)
    # server_status: listening, connecting, connected, login_set, client[64],
    # user[64], width, height, bpp, tiles_sent; then the module's fields
    v = struct.unpack_from("<4i64s64s4I", bytes(m.mu.mem_read(a, 200)))
    extra = struct.unpack_from("<iIIIi", bytes(m.mu.mem_read(a + struct.calcsize("<4i64s64s4I"), 20)))
    return dict(listening=v[0], connecting=v[1], connected=v[2], login_set=v[3],
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
run_for(m, 40)
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
check(slowest < 0.1, "every turn returns quickly during a stalled handshake (%.0f ms)" % (slowest * 1000))
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

# --- 7: stop, detach, finalise ----------------------------------------------
m.entry(STOP)
check(status(m)["listening"] == 0, "stopped")
check("stopped" in messages(m), "said so")
check(m.final(1) is not None, "finalisation refused while the front end is attached and alive")
check(m.final(0) is not None, "RMTidy (non-fatal) refused")
m.entry(DETACH)
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
m.entry(DETACH)
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
m2.entry(DETACH)
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

print("RDPServer module (emulated ARM): %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
