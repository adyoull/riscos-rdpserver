#!/usr/bin/env python3
"""RDPServer: runs the module in the emulator (tests/modemu.py) as a
server real RDP clients can connect to, for tests/e2e_module.sh.

    modserver.py MODULE KEYSDIR PORT OUTDIR [seconds]

Writes OUTDIR/server.log (the module's messages, as the front end gets
them, and this script's own "emu:" lines) and OUTDIR/input.log ("mouse X
Y" in pixels, "key LL down|up", "wheel DZ DW", "clients N"), and obeys
commands written to OUTDIR/ctl (one per line, read as they appear):

  errorbox    draw a box (magenta, 200..440 x 180..300) and open a pretend
              Wimp error box: Service_WimpReportError 1, then no callbacks,
              only the error box loop's OS_Byte &81 through ByteV, until
              the mouse's Select button goes down (a click from the client);
              then the box goes, Service_WimpReportError 0
  mode WxH    a pretend mode change (new size, the screen moves)
  disconnect  the front end's Disconnect (drops the client at once)
  stop        stop and exit

Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from modemu import RiscOS, RMA, SW, SH, Fatal  # noqa: E402

ATTACH, DETACH, SET_FILE, START, STOP, DISCONNECT, RELOAD, STATUS, MESSAGES = range(9)
MOD, KEYS, PORT, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
SECONDS = float(sys.argv[5]) if len(sys.argv) > 5 else 600
HERE = os.path.dirname(os.path.abspath(__file__))
INI_PATH = b"<RDPServer$Dir>/xrdp.ini"
KEYS_PATH = b"<RDPServer$Choices>/rsakeys.ini"
LOGINS_PATH = b"<RDPServer$Choices>/logins"
BOX = (200, 180, 440, 300, (255, 0, 255))

log = open(os.path.join(OUT, "server.log"), "a", buffering=1)
inp = open(os.path.join(OUT, "input.log"), "a", buffering=1)


def say(text):
    log.write("emu: %s\n" % text)


m = RiscOS(MOD, base=RMA + 0x2468 * 4)
if os.environ.get("EMU_EGL_OVERLAY"):  # the user's own setting, to be put back
    m.vars["EGL$Overlay"] = os.environ["EMU_EGL_OVERLAY"]
err = m.init()
if err:
    say("init failed: %s" % err)
    sys.exit(1)
m.alive_tasks = {0x4C0}
m.entry(ATTACH, 0x4C0)


def set_file(path, data):
    a = m.ubuf(path + b"\0", at=0x1000)
    d = m.ubuf(data, at=0x2000)
    return m.entry(SET_FILE, a, d, len(data))


set_file(INI_PATH, open(os.path.join(HERE, "..", "app", "!RDPServer", "xrdp.ini"), "rb").read())
set_file(KEYS_PATH, open(os.path.join(KEYS, "rsakeys.ini"), "rb").read())
logins = os.path.join(KEYS, "logins")
set_file(LOGINS_PATH, open(logins, "rb").read() if os.path.exists(logins) else b"")
opts = struct.pack("<8s64s256s256sIIIii", str(PORT).encode(), b"", INI_PATH, LOGINS_PATH,
                   10, 25, 3000, 128, 1)
seq = [0]


def pump_messages():
    sp = m.ubuf(struct.pack("<I", seq[0]), at=0x10000)
    buf = m.ubuf(size=16384, at=0x11000)
    n = m.entry(MESSAGES, sp, buf, 16384)
    seq[0] = m.r32(sp)
    if n > 0:
        for line in bytes(m.mu.mem_read(buf, n)).decode("latin-1").splitlines():
            log.write(("RDPServer: " if line.startswith("s ") else "xrdp: ") + line[2:] + "\n")


def status():
    a = m.ubuf(size=512, at=0x20000)
    m.entry(STATUS, a)
    v = struct.unpack_from("<4i", bytes(m.mu.mem_read(a, 16)))
    return v


r = m.entry(START, m.ubuf(opts, at=0x8000))
pump_messages()
if r != 0:
    say("start failed")
    sys.exit(1)

ctl_path = os.path.join(OUT, "ctl")
ctl_done = 0
keys_seen = 0
mouse_seen = 0
wheel_seen = 0
clients = None
box_open_at = None
slowest = 0.0
slowest_in_box = 0.0
slow_logged = 0
end = time.monotonic() + SECONDS
next_front = 0
try:
    while time.monotonic() < end:
        t = time.monotonic()
        n_swis = len(m.swilog)
        m.swilog_total = getattr(m, "swilog_total", 0)
        m.run_once()
        took = time.monotonic() - t
        if took > 0.25 and slow_logged < 8:
            from collections import Counter
            slow_logged += 1
            say("slow turn %.0f ms: %s" % (took * 1000, dict(Counter(m.swilog[n_swis:]).most_common(6))))
        slowest = max(slowest, took)
        if m.errorbox:
            slowest_in_box = max(slowest_in_box, took)
        # what the server did to "RISC OS"
        for x, y in m.mouse[mouse_seen:]:
            inp.write("mouse %d %d\n" % (x >> 1, m.sh - 1 - (y >> 1)))
        mouse_seen = len(m.mouse)
        for down, k in m.keyv[keys_seen:]:
            inp.write("key %02X %s\n" % (k, "down" if down else "up"))
            if m.errorbox and down and k == 0x70:
                # the client clicked: the error box goes
                m.draw_screen()
                m.service(0, 0x57)
                m.errorbox = False
                say("error box closed by a click from the client after %.1f s"
                    % (time.monotonic() - box_open_at))
        keys_seen = len(m.keyv)
        for dz, dw in m.wheel[wheel_seen:]:
            inp.write("wheel %d %d\n" % (dz if dz < 0x80000000 else dz - (1 << 32),
                                         dw if dw < 0x80000000 else dw - (1 << 32)))
        wheel_seen = len(m.wheel)
        c = m.vars.get("RDPServer$Clients")
        if c != clients and c is not None:
            inp.write("clients %s\n" % c)
            inp.write("overlay %s\n" % m.vars.get("EGL$Overlay", "(unset)"))
            clients = c
        if t >= next_front:
            # the front end's poll: messages, and commands
            next_front = t + 0.1
            pump_messages()
            if os.path.exists(ctl_path):
                lines = open(ctl_path).read().splitlines()
                for cmd in lines[ctl_done:]:
                    say("command: " + cmd)
                    if cmd == "errorbox":
                        m.draw_screen(BOX)
                        m.service(1, 0x57)
                        m.errorbox = True
                        box_open_at = time.monotonic()
                    elif cmd == "bank":
                        m.swap_bank()
                    elif cmd.startswith("mode "):
                        w, h = (int(v) for v in cmd[5:].split("x"))
                        m.set_mode(w, h)
                    elif cmd == "disconnect":
                        t0 = time.monotonic()
                        m.entry(DISCONNECT)
                        st = status()
                        say("disconnect took %.0f ms; connecting %d, connected %d"
                            % ((time.monotonic() - t0) * 1000, st[1], st[2]))
                    elif cmd == "status":
                        st = status()
                        say("status: listening %d, connecting %d, connected %d" % st[:3])
                    elif cmd == "stop":
                        end = 0
                ctl_done = len(lines)
        time.sleep(0.002)
    m.entry(STOP)
    pump_messages()
    m.entry(DETACH)
    err = m.final(1)
    say("finalised: %s" % (err or "ok"))
    say("longest turn %.0f ms (in the error box %.0f ms)" % (slowest * 1000, slowest_in_box * 1000))
except Fatal as e:
    pump_messages()
    say("FATAL: %s" % e)
    say("last SWIs: %s" % " ".join(m.swilog[-30:]))
    sys.exit(3)
