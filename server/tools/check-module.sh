#!/bin/sh
# RDPServer: checks on the module (ELF and file): no VFP or NEON
# instructions, no SWIs or calls the module mustn't make, header first.
#   tools/check-module.sh module.elf RDPServer,ffa
set -eu
ELF=$1 MOD=$2
OBJDUMP=${OBJDUMP:-$(dirname "$(command -v arm-linux-gnueabihf-objdump 2>/dev/null || echo /usr/bin/x)")/arm-linux-gnueabihf-objdump}
[ -x "$OBJDUMP" ] || OBJDUMP="${RISCOS_TOOLCHAIN:-$HOME/riscos-toolchain-1.4}/bin/arm-riscos-gnueabihf-objdump"
fails=0
# VFP/NEON: any instruction on coprocessors 10/11 or a v-mnemonic
if "$OBJDUMP" -d "$ELF" | grep -E '^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+v[a-z]' | head -3 | grep -q .; then
  echo "check-module: VFP/NEON instructions:"; "$OBJDUMP" -d "$ELF" | grep -E '^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+v[a-z]' | head -5
  fails=1
fi
# UnixLib must not be in it
if "$OBJDUMP" -t "$ELF" | grep -E '__unixlib|__ul_global|_main$|__pthread' | head -3 | grep -q .; then
  echo "check-module: UnixLib symbols in the module"; fails=1
fi
# The header: start 0, init and final offsets inside the file, title "RDPServer"
python3 - "$MOD" <<'PY' || fails=1
import struct, sys
d = open(sys.argv[1], 'rb').read()
h = struct.unpack_from('<13I', d, 0)
ok = h[0] == 0 and 0 < h[1] < len(d) and 0 < h[2] < len(d) and h[4] < len(d)
t = d[h[4]:d.index(b'\0', h[4])]
magic, ver, n = struct.unpack_from('<3I', d, 0x34)
ok = ok and t == b'RDPServer' and magic == 0x73504452 and ver == 6 and n == 12
if not ok:
    print("check-module: bad header", h, t, hex(magic), ver, n); sys.exit(1)
PY
[ $fails = 0 ] && echo "check-module: $MOD ok"
exit $fails
