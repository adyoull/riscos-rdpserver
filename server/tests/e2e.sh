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
       RDPS_HOST_INPUT_LOG="$OUT/input.log" RDPS_HOST_STILL=1 DISPLAY=$DISP \
       RDPS_HOST_STUCK_SHIFT=1 \
       RDPS_HOST_MODE_FILE="$OUT/mode"   # a pretend mode change: see the end
fails=0
ok () { echo "ok   $1"; }
bad () { echo "FAIL $1"; fails=$((fails + 1)); }

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 & XPID=$!
sleep 1
# Clients must log in as test / x (the e2e clients do).
$SRV -setlogin test x >"$OUT/setlogin.log" 2>&1 || { bad "-setlogin failed"; cat "$OUT/setlogin.log"; exit 1; }
# -g 2: a pause after each step, as on the desktop (Wimp_PollIdle).
$SRV -p "$PORT" -t 400 -g 2 -v -m >"$OUT/server.log" 2>&1 & SPID=$!
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
    xdotool click 4 >/dev/null 2>&1; sleep 1        # wheel up
    xdotool click 5 click 5 >/dev/null 2>&1; sleep 1  # wheel down twice
    # A quick sweep of 150 moves: RISC OS's pointer must be at the end of
    # it straight away, not replay the path (test3 on the Pi did).
    for x in $(seq 101 250); do echo "mousemove $x 300"; done | xdotool - >/dev/null 2>&1
    sleep 0.5
    grep "^mouse" "$OUT/input.log" | tail -1 >"$OUT/sweep-end.txt"
    sleep 1
    # A Shift-click the client means: Shift must stay down for it.
    xdotool keydown Shift_L >/dev/null 2>&1; sleep 1
    xdotool click 1 >/dev/null 2>&1; sleep 1
    xdotool keyup Shift_L >/dev/null 2>&1; sleep 1
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
# The move reaches RISC OS before the button goes down.
awk '/^mouse 200 150$/ && !m {m=NR} /^key 70 down$/ && !d {d=NR} END {exit !(m && d && m < d)}' "$OUT/input.log" \
  && ok "the pointer moves before the button goes down" || bad "button before the move"
[ "$(cat "$OUT/sweep-end.txt")" = "mouse 250 300" ] \
  && ok "a quick sweep: the pointer is at its end within 0.5 s" \
  || bad "pointer lagging: 0.5 s after the sweep it was at $(cat "$OUT/sweep-end.txt")"
# RISC OS thought Shift was held (RDPS_HOST_STUCK_SHIFT): the server lets
# go of it before the first click, but not before the Shift-click.
awk '/^key 4C up$/ && !u {u=NR} /^key 70 down$/ && !d {d=NR} END {exit !(u && d && u < d)}' "$OUT/input.log" \
  && ok "a Shift RISC OS thought held is let go of before the click" || bad "stuck Shift not released before the click"
[ "$(grep -c "Shift was held on RISC OS" "$OUT/server.log")" -eq 1 ] \
  && ok "and only then: the client's own Shift-click keeps its Shift" || bad "Shift released $(grep -c "Shift was held on RISC OS" "$OUT/server.log") times"
awk '/^key 4C down$/ {s=1} /^key 4C up$/ {s=0} /^key 70 down$/ && s {ok=1} END {exit !ok}' "$OUT/input.log" \
  && ok "Shift is down when the Shift-click's button goes down" || bad "the Shift-click lost its Shift"
grep -q "^wheel -1 0$" "$OUT/input.log" && [ "$(grep -c "^wheel 1 0$" "$OUT/input.log")" -eq 2 ] \
  && ok "wheel: one notch up (-1), two down (+1 each)" || bad "wheel: $(grep wheel "$OUT/input.log" | tr '\n' ' ')"
grep -q "^clients 1$" "$OUT/input.log" && ok "RDPServer\$Clients set while connected" || bad "RDPServer\$Clients not set"
grep -q "^key 3C down$" "$OUT/input.log" && grep -q "^key 3C up$" "$OUT/input.log" \
  && ok "A key: low-level &3C down and up" || bad "A key not seen"
grep -q "KeyV down (a key)" "$OUT/server.log" && ! grep -q "&3C\|scancode &1E" "$OUT/server.log" \
  && ok "-v logs that a key was pressed, not which" || bad "the log names ordinary keys"
grep -q "^key 4C down$" "$OUT/input.log" && [ "$(grep "^key 4C" "$OUT/input.log" | tail -1)" = "key 4C up" ] \
  && ok "Shift held when the client went: released" || bad "Shift left pressed: $(grep "^key 4C" "$OUT/input.log" | tr '\n' ' ')"

# A wrong password: refused before the client is shown anything.
up_before=$(grep -c "client up" "$OUT/server.log")
rdesktop -u test -p wrong -a 32 -N localhost:$PORT >"$OUT/wrongpw.log" 2>&1 & CPID=$!
for i in $(seq 1 20); do grep -q "login refused" "$OUT/server.log" && break; sleep 1; done
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
grep -q 'login refused for "test"' "$OUT/server.log" && [ "$(grep -c "client up" "$OUT/server.log")" -eq "$up_before" ] \
  && ok "a wrong password is refused" || bad "a wrong password wasn't refused"
sleep 4   # the 3 s pause after a refusal

# A client that connects and then says nothing is dropped after 30 s, and
# the server takes the next one.
exec 3<>/dev/tcp/127.0.0.1/$PORT; printf '\x03' >&3
for i in $(seq 1 40); do grep -q "too slow connecting" "$OUT/server.log" && break; sleep 1; done
exec 3>&-
grep -q "too slow connecting" "$OUT/server.log" && ok "a silent client is dropped" || bad "a silent client wasn't dropped"
run_client rdesktop-after rdesktop -u test -p x -a 32 -N localhost:$PORT

# A mode change while FreeRDP is connected: the client's desktop takes the
# new size (deactivation-reactivation) and the session carries on.
before_gone=$(grep -c "client gone" "$OUT/server.log")
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32 >"$OUT/freerdp-resize.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do [ "$(grep -c "client up" "$OUT/server.log")" -ge 10 ] && break; sleep 1; done
sleep 3
# Another screen bank shown (a program double-buffering): not a mode change.
echo bank >"$OUT/mode"
sleep 3
[ "$(grep -c "client gone" "$OUT/server.log")" -eq "$before_gone" ] && ! grep -q "desktop is now" "$OUT/server.log" \
  && ok "a screen bank swap isn't taken for a mode change" || bad "a bank swap ended or resized the session"
# Output switched to a sprite in another program's turn (a runaway one,
# say), for 40 reads (a few scans): its mode isn't the screen's, so no mode change.
echo "sprite 40" >"$OUT/mode"
sleep 8
! grep -q "screen mode changed" "$OUT/server.log" && [ "$(grep -c "client gone" "$OUT/server.log")" -eq "$before_gone" ] \
  && ok "output redirected to a sprite isn't taken for a mode change" || bad "redirected output: $(grep 'mode changed' "$OUT/server.log" | tail -1)"
import -window root "$OUT/sprite.png" 2>/dev/null
p=$(pixel "$OUT/sprite.png" 100 400); near "$p" 0,0,255 && ok "the screen still sent meanwhile: blue at 100,400" || bad "redirected: 100,400 is $p"
# A mode change at the same size (here a longer LineLength): the client
# isn't resized (that would make it flash), only the picture is sent again.
echo "640x480 pad" >"$OUT/mode"
for i in $(seq 1 20); do grep -q "at the same size" "$OUT/server.log" && break; sleep 0.5; done
sleep 3
grep -q "at the same size: the client's desktop stays" "$OUT/server.log" && ! grep -q "desktop is now" "$OUT/server.log" \
  && ok "a mode change at the same size doesn't resize the client" || bad "same-size mode change: $(grep -i 'mode' "$OUT/server.log" | tail -2)"
import -window root "$OUT/samesize.png" 2>/dev/null
p=$(pixel "$OUT/samesize.png" 600 450); near "$p" 128,128,128 && ok "same size: grey at 600,450 (the new line length read)" || bad "same size: 600,450 is $p"
p=$(pixel "$OUT/samesize.png" 100 300); near "$p" 0,0,255 && ok "same size: blue at 100,300" || bad "same size: 100,300 is $p"
echo 800x600 >"$OUT/mode"
for i in $(seq 1 20); do grep -q "desktop is now 800 x 600" "$OUT/server.log" && break; sleep 0.5; done
sleep 4
grep -q "the client's desktop is now 800 x 600" "$OUT/server.log" && ok "mode change: the client is resized to 800 x 600" || bad "no resize: $(grep -i 'mode' "$OUT/server.log" | tail -2)"
import -window root "$OUT/resized.png" 2>/dev/null
p=$(pixel "$OUT/resized.png" 700 550); near "$p" 128,128,128 && ok "resized: the new screen's grey at 700,550" || bad "resized: 700,550 is $p"
p=$(pixel "$OUT/resized.png" 100 400); near "$p" 0,0,255 && ok "resized: blue at 100,400" || bad "resized: 100,400 is $p"
[ "$(grep -c "client gone" "$OUT/server.log")" -eq "$before_gone" ] && kill -0 $CPID 2>/dev/null \
  && ok "and the session carried on" || bad "the session ended at the mode change"
echo 640x480 >"$OUT/mode"
for i in $(seq 1 20); do grep -q "desktop is now 640 x 480" "$OUT/server.log" && break; sleep 0.5; done
grep -q "the client's desktop is now 640 x 480" "$OUT/server.log" && ok "and back to 640 x 480" || bad "no resize back"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
for i in $(seq 1 10); do [ "$(grep -c "client gone" "$OUT/server.log")" -gt "$before_gone" ] && break; sleep 1; done

# Sound (FreeRDP's fake output): the client joins rdpsnd, takes our
# format, and confirms the blocks it's sent. The server was started with -m:
# this computer is muted while the client listens, and unmuted after.
: >"$OUT/input.log"
before_gone=$(grep -c "client gone" "$OUT/server.log")
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32 /sound:sys:fake >"$OUT/freerdp-sound.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do [ "$(grep -c "client up" "$OUT/server.log")" -ge 11 ] && break; sleep 1; done
sleep 5
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
for i in $(seq 1 10); do [ "$(grep -c "client gone" "$OUT/server.log")" -gt "$before_gone" ] && break; sleep 1; done
grep -q "sound: to the client, 48000 Hz 16-bit stereo" "$OUT/server.log" \
  && ok "sound: the client takes 48 kHz 16-bit stereo" || bad "sound: no format agreed: $(grep 'sound' "$OUT/server.log" | tail -2)"
line=$(grep "sound: [0-9]* blocks sent" "$OUT/server.log" | tail -1)
sent=$(echo "$line" | sed -n 's/.*sound: \([0-9]*\) blocks sent, \([0-9]*\) confirmed.*/\1/p')
conf=$(echo "$line" | sed -n 's/.*sound: \([0-9]*\) blocks sent, \([0-9]*\) confirmed.*/\2/p')
# 5 s of 40 ms blocks: about 125; the client confirms nearly all of them
[ "${sent:-0}" -ge 60 ] && [ "${conf:-0}" -ge $((sent - 10)) ] && [ "${conf:-0}" -le "${sent:-0}" ] \
  && ok "sound: $sent blocks sent, $conf confirmed" || bad "sound: '$line'"
grep -q "^sound mute 1$" "$OUT/input.log" && [ "$(grep '^sound mute' "$OUT/input.log" | tail -1)" = "sound mute 0" ] \
  && ok "this computer muted while the client listened (-m), unmuted after" || bad "mute: $(grep '^sound mute' "$OUT/input.log" | tr '\n' ' ')"
# rdesktop (no -r sound) joins rdpsnd but has no output: no sound, and
# (-v) said so; FreeRDP plays sound by default.
[ "$(grep -c "sound: the client has no sound output" "$OUT/server.log")" -eq 5 ] \
  && ok "no sound for the clients without an output (rdesktop)" || bad "no-output clients: $(grep -c 'no sound output' "$OUT/server.log")"
[ "$(grep -c "sound: [0-9]* blocks sent" "$OUT/server.log")" -eq "$(grep -c "sound: to the client" "$OUT/server.log")" ] \
  && ok "one sound summary per session that had sound" || bad "sound summaries: $(grep -c 'blocks sent' "$OUT/server.log") for $(grep -c 'sound: to the client' "$OUT/server.log") sessions"

kill -INT $SPID; wait $SPID; status=$?
[ $status -ne 135 ] && [ $status -ne 132 ] && ok "no alignment or VFPv4 trap (exit $status)" || bad "trapped: exit $status"
grep -q "stopped" "$OUT/server.log" && ok "server stopped cleanly" || bad "server didn't stop cleanly"
n=$(grep -c "client up" "$OUT/server.log")
[ "$n" -eq 11 ] && ok "11 sessions" || bad "$n sessions came up, not 11"
echo "e2e: $fails failed"
[ $fails -eq 0 ]
