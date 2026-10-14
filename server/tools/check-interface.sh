#!/bin/sh
# check-interface.sh FRONTEND MODULE: the front end binary and the module
# must agree on the module interface (version and entries), or the zip
# would pair halves that refuse each other (0.2-test11 did: the front end
# was built before the interface changed).
# Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
python3 - "$1" "$2" <<'PY'
import re, struct, sys
fe = open(sys.argv[1], 'rb').read()
mod = open(sys.argv[2], 'rb').read()
m = re.search(rb'RDPSModInterface=(\d+),(\d+)\0', fe)
if not m:
    print("check-interface: no interface tag in", sys.argv[1]); sys.exit(1)
want = (int(m.group(1)), int(m.group(2)))
magic, ver, n = struct.unpack_from('<3I', mod, 0x34)
if magic != 0x73504452 or ver != want[0] or n < want[1]:
    print("check-interface: front end needs %d,%d; module has %d,%d" % (want + (ver, n)))
    sys.exit(1)
print("check-interface: front end and module agree (interface %d, %d entries)" % (ver, n))
PY
