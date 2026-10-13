#!/bin/bash
# RDPServer: end-to-end test on Linux with real RDP clients.
#
# Runs the host build (pretend 640x480 32 bpp RISC OS screen: red, green,
# blue and grey quarters, a white square at 0..63,64..127) on a spare
# port, connects rdesktop and FreeRDP to it in a virtual X display at
# several colour depths, and checks the colours the client shows, the
# server's log, and the input the server received.
#
#   tests/e2e.sh            (from the server folder, after make host)
# Needs: Xvfb, rdesktop, xfreerdp3, ImageMagick, xdotool.
set -u
cd "$(dirname "$0")/.."
# QEMU=1: the ARM build under qemu-arm-aligntrap (RISC OS's alignment
# rules: an unaligned access stops the server, exit status 135).
if [ "${QEMU:-0}" = 1 ]; then
  RCD_DIR=${RCD_DIR:-$PWD/../../riscos-crossdev}
  SRV="$RCD_DIR/qemu/aligntrap.sh $PWD/build/qemu/rdpserver"
  export QEMU=$RCD_DIR/toolchain/env/bin/qemu-arm-aligntrap
else
  SRV=$PWD/build/host/rdpserver
fi
OUT=$PWD/build/tests/e2e
PORT=${PORT:-33899}
DISP=${DISP:-:97}
rm -rf "$OUT"; mkdir -p "$OUT"
export RDPS_RESOURCES="$PWD/app/!RDPServer" RDPS_CHOICES="$OUT" \
       RDPS_HOST_INPUT_LOG="$OUT/input.log" RDPS_HOST_STILL=1 DISPLAY=$DISP
fails=0
ok () { echo "ok   $1"; }
bad () { echo "FAIL $1"; fails=$((fails + 1)); }

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 & XPID=$!
sleep 1
$SRV -p "$PORT" -t 300 >"$OUT/server.log" 2>&1 & SPID=$!
trap 'kill $SPID $XPID 2>/dev/null' EXIT
# The first run makes the key.
for i in $(seq 1 60); do grep -q listening "$OUT/server.log" && break; sleep 1; done
grep -q listening "$OUT/server.log" && ok "server listening" || { bad "server didn't start"; cat "$OUT/server.log"; exit 1; }

# pixel X Y -> "r,g,b" (0-255) from a screenshot
pixel () { convert "$1" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info:; }
near () {  # "r,g,b" "r,g,b": within 12 on each
  local IFS=,; set -- $1 $2
  for d in $(( $1 - $4 )) $(( $2 - $5 )) $(( $3 - $6 )); do
    [ ${d#-} -le 12 ] || return 1
  done
}

# check NAME: the window at 0,0 (no decorations in a bare Xvfb).
# 8 bpp: grey &808080 becomes 145,145,170 in the 3:3:2 palette.
# rdesktop 1.9 at 8 and 16 bpp in this Xvfb shows red and blue swapped:
# its own X11 display code, not the server (control, 13 Oct 2026: real
# xrdp 0.9.24 in the same Xvfb, rdesktop -a 16 vs -a 32, logo colours
# swap the same way; FreeRDP shows our 8 and 16 bpp correctly).
check_picture () {
  local name=$1 shot="$OUT/$1.png" p red=255,0,0 blue=0,0,255 grey=128,128,128
  case $name in *-8) grey=145,145,170 ;; esac
  case $name in rdesktop-8) red=0,0,255; blue=255,0,0; grey=170,145,145 ;;
                rdesktop-16) red=0,0,255; blue=255,0,0 ;; esac
  import -window root "$shot" 2>/dev/null || { bad "$name: no screenshot"; return; }
  for spec in "100,200 $red red" "500,100 0,255,0 green" "100,300 $blue blue" \
              "500,400 $grey grey" "30,90 255,255,255 white-square"; do
    set -- $spec
    p=$(pixel "$shot" ${1%,*} ${1#*,})
    near "$p" "$2" && ok "$name: $3 at $1" || bad "$name: $3 at $1 is $p"
  done
}

run_client () {  # name command...
  local name=$1; shift
  local before; before=$(grep -c "client up" "$OUT/server.log")
  "$@" >"$OUT/$name.log" 2>&1 & local CPID=$!
  for i in $(seq 1 30); do
    [ "$(grep -c "client up" "$OUT/server.log")" -gt "$before" ] && break; sleep 1
  done
  sleep 3
  check_picture "$name"
  if [ "$name" = rdesktop-16 ]; then
    : >"$OUT/input.log"
    xdotool mousemove 200 150 click 1 >/dev/null 2>&1; sleep 1
    xdotool key a >/dev/null 2>&1; sleep 1
    # Held down when the client goes: the server must let go.
    xdotool keydown Shift_L >/dev/null 2>&1; sleep 1
  fi
  kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
  xdotool keyup Shift_L >/dev/null 2>&1
  for i in $(seq 1 10); do grep -q "client gone" "$OUT/server.log" && [ "$(grep -c "client gone" "$OUT/server.log")" -ge "$(grep -c "client up" "$OUT/server.log")" ] && break; sleep 1; done
}

run_client rdesktop-8  rdesktop -u test -p x -a 8  -N localhost:$PORT
run_client rdesktop-16 rdesktop -u test -p x -a 16 -N localhost:$PORT
run_client rdesktop-24 rdesktop -u test -p x -a 24 -N localhost:$PORT
run_client rdesktop-32 rdesktop -u test -p x -a 32 -N localhost:$PORT
run_client freerdp-8   xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:8
run_client freerdp-15  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:15
run_client freerdp-16  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:16
run_client freerdp-32  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32

# Input from the 16 bpp rdesktop session: a click at 200,150 and an "a".
grep -q "^mouse 200 150$" "$OUT/input.log" && ok "mouse moved to 200,150" || bad "mouse: $(head -3 "$OUT/input.log" | tr '\n' ' ')"
grep -q "^key 70 down$" "$OUT/input.log" && grep -q "^key 70 up$" "$OUT/input.log" \
  && ok "left button: Select (&70) down and up" || bad "left button not seen"
grep -q "^key 3C down$" "$OUT/input.log" && grep -q "^key 3C up$" "$OUT/input.log" \
  && ok "A key: low-level &3C down and up" || bad "A key not seen"
grep -q "^key 4C down$" "$OUT/input.log" && [ "$(grep "^key 4C" "$OUT/input.log" | tail -1)" = "key 4C up" ] \
  && ok "Shift held when the client went: released" || bad "Shift left pressed: $(grep "^key 4C" "$OUT/input.log" | tr '\n' ' ')"

# A client that connects and then says nothing is dropped after 30 s, and
# the server takes the next one.
exec 3<>/dev/tcp/127.0.0.1/$PORT; printf '\x03' >&3
for i in $(seq 1 40); do grep -q "too slow connecting" "$OUT/server.log" && break; sleep 1; done
exec 3>&-
grep -q "too slow connecting" "$OUT/server.log" && ok "a silent client is dropped" || bad "a silent client wasn't dropped"
run_client rdesktop-after rdesktop -u test -p x -a 32 -N localhost:$PORT

kill -INT $SPID; wait $SPID; status=$?
[ $status -ne 135 ] && [ $status -ne 132 ] && ok "no alignment or VFPv4 trap (exit $status)" || bad "trapped: exit $status"
grep -q "stopped" "$OUT/server.log" && ok "server stopped cleanly" || bad "server didn't stop cleanly"
n=$(grep -c "client up" "$OUT/server.log")
[ "$n" -eq 9 ] && ok "9 sessions" || bad "$n sessions came up, not 9"
echo "e2e: $fails failed"
[ $fails -eq 0 ]
