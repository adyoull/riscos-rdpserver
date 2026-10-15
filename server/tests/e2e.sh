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
RDESKTOP=$PWD/tests/rdesktop-yes.sh   # accepts the server's new certificate
export RDESKTOP_HOME=$PWD/build/tests/rdesktop-home
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
$SRV -p "$PORT" -t 520 -g 2 -v -m -sound >"$OUT/server.log" 2>&1 & SPID=$!
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
  if [ "$name" = freerdp-16 ]; then
    # Typed in FreeRDP's Unicode keyboard mode: characters, not keys
    # (0.2-test23; dropped before).
    : >"$OUT/input-unicode.log"
    cp "$OUT/input.log" "$OUT/input-before-unicode.log"
    # (ASCII only: FreeRDP under xdotool in Xvfb sends nothing for a
    # remapped key such as e-acute; top-bit bytes are tested in
    # test_units.c and test_fake.c)
    xdotool type --delay 100 'h!' >/dev/null 2>&1; sleep 2
    cp "$OUT/input.log" "$OUT/input-unicode.log"
  fi
  kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
  xdotool keyup Shift_L >/dev/null 2>&1
  for i in $(seq 1 10); do grep -q "client gone" "$OUT/server.log" && [ "$(grep -c "client gone" "$OUT/server.log")" -ge "$(grep -c "client up" "$OUT/server.log")" ] && break; sleep 1; done
}

run_client rdesktop-8 $RDESKTOP -u test -p x -a 8  -N localhost:$PORT
run_client rdesktop-16 $RDESKTOP -u test -p x -a 16 -N localhost:$PORT
run_client rdesktop-24 $RDESKTOP -u test -p x -a 24 -N localhost:$PORT
run_client rdesktop-32 $RDESKTOP -u test -p x -a 32 -N localhost:$PORT
run_client freerdp-8   xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:8
run_client freerdp-15  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:15
run_client freerdp-16  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:16 /kbd:unicode:on
run_client freerdp-32  xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32
# FreeRDP left to choose (it offers TLS and NLA): the server picks TLS
run_client freerdp-tls xfreerdp3 /v:localhost:$PORT /u:test /p:x /cert:ignore /bpp:32

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
grep -q "^char 68$" "$OUT/input-unicode.log" && grep -q "^char 21$" "$OUT/input-unicode.log" \
  && awk '/^char 68$/ && !h {h=NR} /^char 21$/ && !b {b=NR} END {exit !(h && b && h < b)}' "$OUT/input-unicode.log" \
  && ok "characters sent as themselves (Unicode) are typed, in order" || bad "Unicode typing: $(grep '^char' "$OUT/input-unicode.log" | tr '\n' ' ')"
grep -q "the client sends characters as themselves (Unicode)" "$OUT/server.log" && ! grep -q "char.*68\|U+0068" "$OUT/server.log" \
  && ok "and the log says so, not which" || bad "Unicode log: $(grep -i unicode "$OUT/server.log" | head -2 | tr '\n' ' ')"
grep -q "KeyV down (a key)" "$OUT/server.log" && ! grep -q "&3C\|scancode &1E" "$OUT/server.log" \
  && ok "-v logs that a key was pressed, not which" || bad "the log names ordinary keys"
grep -q "^key 4C down$" "$OUT/input.log" && [ "$(grep "^key 4C" "$OUT/input.log" | tail -1)" = "key 4C up" ] \
  && ok "Shift held when the client went: released" || bad "Shift left pressed: $(grep "^key 4C" "$OUT/input.log" | tr '\n' ' ')"

# A wrong password (0.2-test18): the login screen, not the RISC OS
# screen. A wrong one typed there is refused; the right one logs in.
up_before=$(grep -c "client up" "$OUT/server.log")
$RDESKTOP -u test -p wrong -a 32 -N localhost:$PORT >"$OUT/wrongpw.log" 2>&1 & CPID=$!
for i in $(seq 1 20); do grep -q "the login screen for" "$OUT/server.log" && break; sleep 1; done
sleep 3
import -window root "$OUT/login.png" 2>/dev/null
grep -q 'the login screen for "test" (a wrong password sent)' "$OUT/server.log" \
  && [ "$(grep -c "client up" "$OUT/server.log")" -eq "$up_before" ] \
  && ok "a wrong password: the login screen" || bad "login screen: $(grep -i 'login' "$OUT/server.log" | tail -2 | tr '\n' ' ')"
grep -q "the login screen types with the UK keyboard layout (RISC OS keyboard 1)" "$OUT/server.log" \
  && ok "the login screen types with RISC OS's keyboard layout" || bad "login keyboard: $(grep 'keyboard layout' "$OUT/server.log" | tail -1)"
# the RISC OS screen's colours where it would show them: none of them
leaked=""
for spot in "10 10:255,0,0" "100 200:255,0,0" "630 10:0,255,0" "500 100:0,255,0" "10 470:0,0,255" \
            "100 300:0,0,255" "630 470:128,128,128" "500 400:128,128,128" "30 90:255,255,255"; do
  p=$(pixel "$OUT/login.png" ${spot%%:*})
  near "$p" "${spot#*:}" && leaked="$leaked ${spot%%:*}"
done
[ -z "$leaked" ] && ok "the login screen shows none of the RISC OS screen" || bad "the RISC OS screen shows at:$leaked"
p=$(pixel "$OUT/login.png" 10 10); near "$p" 42,51,69 && ok "the login screen's background at 10,10" || bad "login background: 10,10 is $p"
p=$(pixel "$OUT/login.png" 40 350); near "$p" 238,238,238 && ok "the login panel at 40,350" || bad "login panel: 40,350 is $p"
xdotool type --delay 50 nope >/dev/null 2>&1; xdotool key Return >/dev/null 2>&1
for i in $(seq 1 10); do grep -q "at the login screen (try 2" "$OUT/server.log" && break; sleep 1; done
grep -q "login refused at the login screen (try 2 of 5)" "$OUT/server.log" \
  && ok "a wrong password at the login screen is refused" || bad "login screen wrong password: $(grep 'login refused' "$OUT/server.log" | tail -1)"
sleep 4   # the pause after a wrong one
: >"$OUT/input.log"
xdotool type --delay 50 x >/dev/null 2>&1; xdotool key Return >/dev/null 2>&1
for i in $(seq 1 15); do [ "$(grep -c "client up" "$OUT/server.log")" -gt "$up_before" ] && break; sleep 1; done
sleep 3
import -window root "$OUT/loggedin.png" 2>/dev/null
grep -q 'logged in at the login screen as "test"' "$OUT/server.log" && [ "$(grep -c "client up" "$OUT/server.log")" -gt "$up_before" ] \
  && ok "the right password at the login screen logs in" || bad "login screen: not logged in: $(grep -i 'login' "$OUT/server.log" | tail -2 | tr '\n' ' ')"
p=$(pixel "$OUT/loggedin.png" 100 200); near "$p" 255,0,0 && ok "logged in: the RISC OS screen (red at 100,200)" || bad "after login: 100,200 is $p"
! grep -q "^key " "$OUT/input.log" && ok "keys typed at the login screen didn't reach RISC OS" || bad "login keys reached RISC OS: $(grep '^key' "$OUT/input.log" | head -3 | tr '\n' ' ')"
! grep -q "nope" "$OUT/server.log" && ok "the password typed isn't in the log" || bad "a typed password is in the log"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
for i in $(seq 1 10); do [ "$(grep -c "client gone" "$OUT/server.log")" -ge "$(grep -c "client up" "$OUT/server.log")" ] && break; sleep 1; done

# A client that connects and then says nothing is dropped after 30 s, and
# the server takes the next one.
exec 3<>/dev/tcp/127.0.0.1/$PORT; printf '\x03' >&3
for i in $(seq 1 40); do grep -q "too slow connecting" "$OUT/server.log" && break; sleep 1; done
exec 3>&-
grep -q "too slow connecting" "$OUT/server.log" && ok "a silent client is dropped" || bad "a silent client wasn't dropped"
run_client rdesktop-after $RDESKTOP -u test -p x -a 32 -N localhost:$PORT

# A mode change while FreeRDP is connected: the client's desktop takes the
# new size (deactivation-reactivation) and the session carries on.
before_gone=$(grep -c "client gone" "$OUT/server.log")
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32 >"$OUT/freerdp-resize.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do [ "$(grep -c "client up" "$OUT/server.log")" -ge 12 ] && break; sleep 1; done
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
# Every RISC OS pixel format (0.2-test17), each at the same size: the
# picture again in the new format. Palette modes use the host's palette
# (black, red, green, blue, grey, white).
same=$(grep -c "at the same size" "$OUT/server.log")
mode_to () {  # spec name: the pretend mode, then a screenshot
  echo "640x480 $1" >"$OUT/mode"
  for i in $(seq 1 20); do [ "$(grep -c "at the same size" "$OUT/server.log")" -gt "$same" ] && break; sleep 0.5; done
  same=$(grep -c "at the same size" "$OUT/server.log")
  sleep 3
  import -window root "$OUT/mode-$2.png" 2>/dev/null
}
see () {  # name x y r,g,b what
  local p; p=$(pixel "$OUT/mode-$1.png" $2 $3)
  near "$p" "$4" && ok "$1: $5 at $2,$3" || bad "$1: $5 at $2,$3 is $p"
}
for m in "4:16col" "8:256col" "4k:4k" "32k rgb:32k-rgb" "64k rgb:64k-rgb" "16m rgb:16m-rgb"; do
  mode_to "${m%%:*}" "${m#*:}"
  n=${m#*:}
  see $n 100 200 255,0,0 red; see $n 500 100 0,255,0 green; see $n 100 300 0,0,255 blue
  see $n 500 400 128,128,128 grey; see $n 30 90 255,255,255 white-square
done
grep -q "this screen mode can't be shared\|can't be shared" "$OUT/server.log" \
  && bad "a mode wasn't shared: $(grep "can't be shared" "$OUT/server.log" | tail -1)" || ok "every format shared"
# 4 colours (black, red, green, blue) and 2 (black, red): what isn't in
# the palette is drawn black by the host
mode_to 2 4col
see 4col 100 200 255,0,0 red; see 4col 500 100 0,255,0 green; see 4col 100 300 0,0,255 blue
see 4col 500 400 0,0,0 "grey (not in it: black)"
mode_to 1 2col
see 2col 100 200 255,0,0 red; see 2col 500 100 0,0,0 "green (not in it: black)"
# A program changes the palette: red becomes yellow on the client.
mode_to 4 16col-b
echo pal >"$OUT/mode"
sleep 3
import -window root "$OUT/mode-pal.png" 2>/dev/null
see pal 100 200 255,255,0 "red now yellow"; see pal 500 100 0,255,0 "green still"
[ "$(grep -c "client gone" "$OUT/server.log")" -eq "$before_gone" ] && kill -0 $CPID 2>/dev/null \
  && ok "the session carried on through every format" || bad "the session ended in the format changes"
echo "640x480 16m" >"$OUT/mode"
sleep 3
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
for i in $(seq 1 10); do [ "$(grep -c "client gone" "$OUT/server.log")" -gt "$before_gone" ] && break; sleep 1; done

# Sound (FreeRDP's fake output): the client joins rdpsnd, takes our
# format, and confirms the blocks it's sent. The server was started with -m:
# this computer is muted while the client listens, and unmuted after.
: >"$OUT/input.log"
before_gone=$(grep -c "client gone" "$OUT/server.log")
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32 /sound:sys:fake >"$OUT/freerdp-sound.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do [ "$(grep -c "client up" "$OUT/server.log")" -ge 13 ] && break; sleep 1; done
sleep 5
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
for i in $(seq 1 10); do [ "$(grep -c "client gone" "$OUT/server.log")" -gt "$before_gone" ] && break; sleep 1; done
grep -q "sound: to the client, 48000 Hz 16-bit stereo" "$OUT/server.log" \
  && ok "sound: the client takes 48 kHz 16-bit stereo" || bad "sound: no format agreed: $(grep 'sound' "$OUT/server.log" | tail -2)"
line=$(grep "sound ended: [0-9]* blocks sent" "$OUT/server.log" | tail -1)
sent=$(echo "$line" | sed -n 's/.*sound ended: \([0-9]*\) blocks sent, \([0-9]*\) confirmed.*/\1/p')
conf=$(echo "$line" | sed -n 's/.*sound ended: \([0-9]*\) blocks sent, \([0-9]*\) confirmed.*/\2/p')
# 5 s of 40 ms blocks: about 125; the client confirms nearly all of them
[ "${sent:-0}" -ge 60 ] && [ "${conf:-0}" -ge $((sent - 10)) ] && [ "${conf:-0}" -le "${sent:-0}" ] \
  && ok "sound: $sent blocks sent, $conf confirmed" || bad "sound: '$line'"
grep -q "^sound mute 1$" "$OUT/input.log" && [ "$(grep '^sound mute' "$OUT/input.log" | tail -1)" = "sound mute 0" ] \
  && ok "this computer muted while the client listened (-m), unmuted after" || bad "mute: $(grep '^sound mute' "$OUT/input.log" | tr '\n' ' ')"
# rdesktop (no -r sound) joins rdpsnd but has no output: no sound, and
# (-v) said so; FreeRDP plays sound by default.
[ "$(grep -c "sound: the client has no sound output" "$OUT/server.log")" -eq 6 ] \
  && ok "no sound for the clients without an output (rdesktop)" || bad "no-output clients: $(grep -c 'no sound output' "$OUT/server.log")"
[ "$(grep -c "sound ended: [0-9]* blocks sent" "$OUT/server.log")" -eq "$(grep -c "sound: to the client" "$OUT/server.log")" ] \
  && ok "one sound summary per session that had sound" || bad "sound summaries: $(grep -c 'blocks sent' "$OUT/server.log") for $(grep -c 'sound: to the client' "$OUT/server.log") sessions"

# TLS (0.2-test14): the server's certificate, and which client got what.
[ -s "$OUT/cert.pem" ] && [ -s "$OUT/key.pem" ] \
  && openssl x509 -in "$OUT/cert.pem" -noout -ext extendedKeyUsage 2>/dev/null | grep -q "TLS Web Server Authentication" \
  && [ "$(openssl x509 -in "$OUT/cert.pem" -noout -modulus)" = "$(openssl rsa -in "$OUT/key.pem" -noout -modulus 2>/dev/null)" ] \
  && ok "a self-signed certificate for server authentication, with its key" || bad "no proper cert.pem/key.pem in Choices"
[ "$(grep -c 'security: TLS (TLSv1.2' "$OUT/server.log")" -eq 7 ] \
  && ok "TLS 1.2 for the 7 sessions that asked (rdesktop, FreeRDP choosing)" || bad "TLS sessions: $(grep -c 'security: TLS' "$OUT/server.log")"
[ "$(grep -c 'security: Standard RDP Security' "$OUT/server.log")" -eq 6 ] \
  && ok "Standard RDP Security for the 6 FreeRDP /sec:rdp sessions" || bad "RDP-security sessions: $(grep -c 'security: Standard' "$OUT/server.log")"
grep -q "Client requested security types.*SSL|HYBRID" "$OUT/XrdpLog" 2>/dev/null || grep -q "SSL|HYBRID" "$OUT/server.log" \
  && ok "FreeRDP offered TLS and NLA; TLS was chosen" || bad "FreeRDP's offer not seen"

# Only TLS (-tls, as Choices' option): a second server on the next port.
$SRV -p $((PORT + 2)) -t 60 -g 2 -v -tls >"$OUT/server-tls.log" 2>&1 & TPID=$!
for i in $(seq 1 30); do grep -q listening "$OUT/server-tls.log" && break; sleep 1; done
timeout 20 xfreerdp3 /v:localhost:$((PORT + 2)) /u:test /p:x /sec:rdp /cert:ignore >"$OUT/freerdp-rc4-refused.log" 2>&1
! grep -q "client up" "$OUT/server-tls.log" && grep -q "refused: it didn't ask for TLS" "$OUT/server-tls.log" \
  && ok "-tls: a client without TLS is refused" || bad "-tls: $(grep -i 'requires\|client up' "$OUT/server-tls.log" | tail -2 | tr '\n' ' ')"
xfreerdp3 /v:localhost:$((PORT + 2)) /u:test /p:x /sec:tls /cert:ignore >"$OUT/freerdp-tls-only.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do grep -q "client up" "$OUT/server-tls.log" && break; sleep 1; done
grep -q "security: TLS" "$OUT/server-tls.log" && grep -q "client up" "$OUT/server-tls.log" \
  && ok "-tls: a TLS client comes in" || bad "-tls: the TLS client didn't come up"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null
# This server wasn't given -sound (Choices' "send this computer's sound to
# the client", off unless chosen: 0.2-test22): FreeRDP asks, gets none.
grep -q "sound: not sent (Choices" "$OUT/server-tls.log" && ! grep -q "sound: to the client" "$OUT/server-tls.log" \
  && ok "without -sound, no sound to the client (the log says why)" || bad "sound without -sound: $(grep sound "$OUT/server-tls.log" | head -2 | tr '\n' ' ')"

# Keep-alive (0.2-test15): FreeRDP over TLS takes heartbeats and answers
# network checks; rdesktop takes neither.
grep -q "keep-alive: heartbeats, and network checks" "$OUT/server.log" && grep -q "network check answered" "$OUT/server.log" \
  && ok "keep-alive: FreeRDP over TLS gets heartbeats and answers network checks" || bad "keep-alive: $(grep 'keep-alive' "$OUT/server.log" | sort | uniq -c | tr '\n' ' ')"
# The message channel has a number of its own (0.2-test15 gave it the
# user channel's: Windows App kept asking to join and never got further).
line=$(cat "$OUT/server.log" "$OUT/XrdpLog" 2>/dev/null | grep "message channel [0-9]* given" | head -1)
m=$(echo "$line" | sed -n 's/.*message channel \([0-9]*\) given.*/\1/p')
u=$(echo "$line" | sed -n 's/.*user channel \([0-9]*\)).*/\1/p')
[ -n "$m" ] && [ -n "$u" ] && [ "$m" -ne "$u" ] && [ "$m" -ne 1003 ] \
  && ok "keep-alive: the message channel ($m) isn't the user channel ($u)" || bad "message channel: '$line'"
[ "$(grep -c "keep-alive: none" "$OUT/server.log")" -ge 5 ] \
  && ok "keep-alive: none for rdesktop and RC4 sessions" || bad "keep-alive none: $(grep -c 'keep-alive: none' "$OUT/server.log")"

# Ends test clients for sure (a frozen FreeRDP doesn't always go on SIGTERM).
stop_clients () {
  local p
  for p in "$@"; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
  sleep 1
  for p in "$@"; do kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
}
# A third server (-silent 800: a client that answers network checks is
# gone after 8 s of silence), for clients that stop answering. SIGSTOP
# freezes a client with its connection still open: as a computer gone to
# sleep, or off the network.
KLOG="$OUT/server-ka.log"
$SRV -p $((PORT + 4)) -t 150 -g 2 -v -silent 800 >"$KLOG" 2>&1 & KPID=$!
for i in $(seq 1 30); do grep -q listening "$KLOG" && break; sleep 1; done
ups () { grep -c "client up" "$KLOG"; }
gones () { grep -c "client gone" "$KLOG"; }
wait_for () {  # count-function target seconds
  for i in $(seq 1 "$3"); do [ "$($1)" -ge "$2" ] && return 0; sleep 1; done; return 1
}
freerdp_ka () { xfreerdp3 /v:localhost:$((PORT + 4)) /u:test /p:x /sec:tls /cert:ignore >"$OUT/$1.log" 2>&1 & }
# 1. A frozen client is dropped.
freerdp_ka ka-a; APID=$!
wait_for ups 1 30
sleep 2; kill -STOP $APID
wait_for gones 1 20
grep -q "client gone (the client stopped answering" "$KLOG" \
  && ok "keep-alive: a client that stops answering is dropped" || bad "frozen client: $(grep 'client gone' "$KLOG" | tail -1)"
stop_clients $APID
# 2. A new connection while a frozen client holds the place: the frozen
#    one is checked, dropped, and the new one goes ahead.
g=$(gones); u=$(ups)
freerdp_ka ka-b; BPID=$!
wait_for ups $((u + 1)) 30
sleep 2; kill -STOP $BPID
freerdp_ka ka-c; CPID=$!
wait_for ups $((u + 2)) 30
grep -q "waiting while the client that's connected is checked" "$KLOG" \
  && grep -q "client gone (a new connection came, and this client didn't answer a network check)" "$KLOG" \
  && grep -q "the waiting connection goes ahead" "$KLOG" && [ "$(ups)" -eq $((u + 2)) ] \
  && ok "keep-alive: a new connection takes the place of a frozen client" \
  || bad "takeover: $(grep -i 'waiting\|client gone\|refused' "$KLOG" | tail -3 | tr '\n' ' ')"
stop_clients $BPID
# 3. A new connection while a live client is there: refused, and the
#    client stays.
g=$(gones)
freerdp_ka ka-d; DPID=$!
for i in $(seq 1 20); do grep -q "the client that's connected is still there" "$KLOG" && break; sleep 1; done
grep -q "refused .*: the client that's connected is still there" "$KLOG" && [ "$(gones)" -eq "$g" ] \
  && ok "keep-alive: a live client keeps its place" || bad "live client: $(grep -i 'refused\|client gone' "$KLOG" | tail -2 | tr '\n' ' ')"
stop_clients $DPID $CPID
# 4. Likewise for rdesktop (no network checks): the waiting connection is
#    refused when its time is up.
wait_for gones $((g + 1)) 15
g=$(gones); u=$(ups)
$RDESKTOP -u test -p x -a 32 -N localhost:$((PORT + 4)) >"$OUT/ka-e.log" 2>&1 & EPID=$!
wait_for ups $((u + 1)) 30
r=$(grep -c "still there" "$KLOG")
freerdp_ka ka-f; FPID=$!
for i in $(seq 1 20); do [ "$(grep -c "still there" "$KLOG")" -gt "$r" ] && break; sleep 1; done
[ "$(grep -c "still there" "$KLOG")" -gt "$r" ] && [ "$(gones)" -eq "$g" ] \
  && ok "keep-alive: an rdesktop client keeps its place too" || bad "rdesktop: $(grep -i 'refused\|client gone' "$KLOG" | tail -2 | tr '\n' ' ')"
stop_clients $FPID $EPID
kill -INT $KPID 2>/dev/null; wait $KPID 2>/dev/null

# A client that stops reading with its connection open (a RISC OS client
# whose program stopped, 0.2-rc1: the server waited for half an hour):
# a fifth server, a 1920 x 1080 screen of fresh noise every second,
# -stall 600. A frozen rdesktop (no network checks) is dropped once
# nothing has gone to it for 6 s; and a new connection takes the place of
# another frozen one at once (the whole screen sent, none of it taken).
SLOG="$OUT/server-stall.log"
env -u RDPS_HOST_STILL RDPS_HOST_SIZE=1920x1080 RDPS_HOST_NOISE=1 \
  $SRV -p $((PORT + 6)) -t 150 -g 2 -v -stall 600 >"$SLOG" 2>&1 & LPID=$!
for i in $(seq 1 30); do grep -q listening "$SLOG" && break; sleep 1; done
sups () { grep -c "client up" "$SLOG"; }
sgones () { grep -c "client gone" "$SLOG"; }
$RDESKTOP -u test -p x -a 32 -N localhost:$((PORT + 6)) >"$OUT/stall-a.log" 2>&1 & SAPID=$!
wait_for sups 1 30
sleep 3; kill -STOP $SAPID
wait_for sgones 1 40
grep -q "client gone (the client stopped taking data" "$SLOG" \
  && ok "a client that stops reading is dropped" || bad "stalled client: $(grep 'client gone' "$SLOG" | tail -1)"
stop_clients $SAPID
$RDESKTOP -u test -p x -a 32 -N localhost:$((PORT + 6)) >"$OUT/stall-b.log" 2>&1 & SBPID=$!
wait_for sups 2 30
kill -STOP $SBPID
$RDESKTOP -u test -p x -a 32 -N localhost:$((PORT + 6)) >"$OUT/stall-c.log" 2>&1 & SCPID=$!
wait_for sups 3 30
# (Not under QEMU: the emulated server makes the whole screen so slowly
# that the frozen client's socket buffers here, megabytes on Linux, keep
# taking it for longer than the check lasts; on RISC OS they're 64 KB.)
if [ "${QEMU:-0}" = 1 ]; then
  echo "skip a new connection takes the place of a client that stopped reading (QEMU: too slow to fill Linux's buffers)"
else
  grep -q "client gone (a new connection came, and this client wasn't taking what was sent to it)" "$SLOG" \
    && [ "$(sups)" -eq 3 ] && ok "a new connection takes the place of a client that stopped reading" \
    || bad "stalled takeover: $(grep -i 'waiting\|client gone\|refused' "$SLOG" | tail -3 | tr '\n' ' ')"
fi
grep -q "send buffer: 64 KB" "$SLOG" && ok "a 64 KB send buffer (not 256: the stack's mbufs)" || bad "send buffer: $(grep 'send buffer' "$SLOG" | tail -1)"
stop_clients $SBPID $SCPID
kill -INT $LPID 2>/dev/null; wait $LPID 2>/dev/null

kill -INT $SPID; wait $SPID; status=$?
[ $status -ne 135 ] && [ $status -ne 132 ] && ok "no alignment or VFPv4 trap (exit $status)" || bad "trapped: exit $status"
# RISC OS restarting (0.2-test24; SIGUSR2 is the module's Service_PreReset):
# the client is shown a page saying so, then the connection goes without a
# Disconnect, so FreeRDP (with /auto-reconnect) connects again by itself to
# the server that starts afterwards, as RDPServer would after the restart.
RLOG="$OUT/server-reset.log"
$SRV -p $((PORT + 8)) -t 60 -g 2 -v >"$RLOG" 2>&1 & RPID=$!
for i in $(seq 1 30); do grep -q listening "$RLOG" && break; sleep 1; done
xfreerdp3 /v:localhost:$((PORT + 8)) /u:test /p:x /cert:ignore /bpp:32 /auto-reconnect \
  /auto-reconnect-max-retries:30 >"$OUT/freerdp-reset.log" 2>&1 & RCPID=$!
for i in $(seq 1 30); do grep -q "client up" "$RLOG" && break; sleep 1; done
sleep 3
# The desktop shut down (SIGUSR1: Service_ShutdownComplete), then a click,
# perhaps on Restart (0.2-test25): the page first, then the click; no
# reset followed here, so after 5 s the client connects again.
kill -USR1 $RPID; sleep 1
xdotool mousemove 300 300 click 1 >/dev/null 2>&1
sleep 2
import -window root "$OUT/shutdown-click.png" 2>/dev/null
grep -q "RISC OS is restarting: the client was told so" "$RLOG" \
  && awk '/RISC OS is restarting: the client was told so/ && !p {p=NR} /click &70|KeyV down &70/ && p && !c {c=NR} END {exit !(p && c)}' "$RLOG" \
  && ok "after the shutdown, a click: the page went, then the click" || bad "shutdown click: $(grep -i 'restart\|click' "$RLOG" | tail -3 | tr '\n' ' ')"
q=$(pixel "$OUT/shutdown-click.png" 590 278)
near "$q" 238,238,238 && ok "and the client shows it" || bad "shutdown click page: panel $q"
for i in $(seq 1 30); do [ "$(grep -c "client up" "$RLOG")" -ge 2 ] && break; sleep 1; done
grep -q "no restart followed the page" "$RLOG" && [ "$(grep -c "client up" "$RLOG")" -ge 2 ] \
  && ok "no restart followed: the client connected again by itself" || bad "after the shutdown click: $(grep -i 'no restart\|client up\|client gone' "$RLOG" | tail -3 | tr '\n' ' ')"
sleep 3
kill -USR2 $RPID; wait $RPID 2>/dev/null
sleep 2
import -window root "$OUT/reset.png" 2>/dev/null
[ "$(grep -c "RISC OS is restarting: the client was told so" "$RLOG")" -ge 2 ] && grep -q "pretend reset" "$RLOG" \
  && ok "restarting: the client was sent the page, then the connection went" || bad "restarting: $(tail -3 "$RLOG" | tr '\n' ' ')"
# (FreeRDP's "Reconnecting" box covers the top left: the background lower down)
p=$(pixel "$OUT/reset.png" 100 400); q=$(pixel "$OUT/reset.png" 590 278)
near "$p" 42,51,69 && near "$q" 238,238,238 \
  && ok "the client shows the restarting page (background $p, panel $q)" || bad "restarting page: background $p, panel $q"
RLOG2="$OUT/server-reset2.log"
$SRV -p $((PORT + 8)) -t 60 -g 2 -v >"$RLOG2" 2>&1 & RPID=$!
for i in $(seq 1 60); do grep -q "client up" "$RLOG2" && break; sleep 1; done
grep -q "client up" "$RLOG2" \
  && ok "and once the server is back, the client connects again by itself" || bad "no reconnect after the restart ($(tail -2 "$OUT/freerdp-reset.log" | tr '\n' ' '))"
stop_clients $RCPID
kill -INT $RPID 2>/dev/null; wait $RPID 2>/dev/null

# Encoding (0.2-test22): solid areas as fills where the client takes them
# (FreeRDP at every depth; rdesktop below 32 bpp: at 32 it takes the
# colour the other way round, so it gets bitmaps); how fast this computer
# encodes, at start-up.
grep -q "updates (client version &[0-9A-F]*): .*solid areas as fills (OpaqueRect)" "$OUT/server.log" \
  && ok "solid areas sent as fills" || bad "no fills: $(grep 'updates:' "$OUT/server.log" | sort | uniq -c | tr '\n' ' ')"
grep -q "updates (client version &00080004): .*bitmaps (at 32 bpp, fills go to RDP 10 clients only)" "$OUT/server.log" \
  && ok "rdesktop at 32 bpp gets bitmaps, not fills" || bad "rdesktop-32 updates: $(grep 'updates:' "$OUT/server.log" | sort | uniq -c | tr '\n' ' ')"
grep "tiles sent in all ([1-9][0-9]* as fills)" "$OUT/server.log" >/dev/null \
  && ok "the figures count the fills" || bad "fills not counted: $(grep 'as fills' "$OUT/server.log" | tail -1)"
[ "$(grep -c "encoding here, a whole screen like its middle rows at [0-9]* bpp: converting [0-9]* ms, compressing [0-9]* ms ([0-9]* KB), MPPC [0-9]* ms ([0-9]* KB)" "$OUT/server.log")" -eq 2 ] \
  && ok "start-up: how fast this computer encodes, at 32 and 16 bpp" || bad "encode figures: $(grep 'encoding here' "$OUT/server.log" | tr '\n' ' ')"
grep -q "stopped" "$OUT/server.log" && ok "server stopped cleanly" || bad "server didn't stop cleanly"
n=$(grep -c "client up" "$OUT/server.log")
[ "$n" -eq 13 ] && ok "13 sessions" || bad "$n sessions came up, not 13"
echo "e2e: $fails failed"
[ $fails -eq 0 ]
