#!/bin/bash
# RDPServer: end-to-end test of the module (build/module/RDPServer,ffa) with
# real RDP clients. The module runs in the Unicorn emulator with a pretend
# RISC OS around it (tests/modserver.py, tests/modemu.py): loaded at an
# address it wasn't linked for, run from the ticker's callbacks, its
# sockets real ones.
#
# What's checked beyond tests/e2e.sh: while a Wimp error box is open (no
# callbacks, only the box's OS_Byte &81 through ByteV), the client still
# sees the screen change and can click the box away; Disconnect drops a
# client at once, mid-session and mid-handshake; nothing waits.
#
#   tests/e2e_module.sh       (from the server folder, after make module host)
# Needs: Xvfb, rdesktop, xfreerdp3, ImageMagick, xdotool, python3 unicorn.
set -u
cd "$(dirname "$0")/.."
OUT=$PWD/build/tests/e2e-module
PORT=${PORT:-33898}
DISP=${DISP:-:96}
rm -rf "$OUT"; mkdir -p "$OUT/keys"
fails=0
ok () { echo "ok   $1"; }
bad () { echo "FAIL $1"; fails=$((fails + 1)); }
export DISPLAY=$DISP

# A key and a login (test / x) from the host build.
RDPS_CHOICES="$OUT/keys" RDPS_RESOURCES='app/!RDPServer' build/host/rdpserver -setlogin test x >/dev/null 2>&1
RDPS_CHOICES="$OUT/keys" RDPS_RESOURCES='app/!RDPServer' timeout 120 build/host/rdpserver -p $((PORT + 1)) -t 1 >/dev/null 2>&1
[ -f "$OUT/keys/rsakeys.ini" ] || { bad "no key"; exit 1; }

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 & XPID=$!
sleep 1
# (no alignment check here: it makes each memory access a Python call, and
# the RSA step then takes minutes; tests/test_module.py has it)
EMU_ALIGN=0 python3 tests/modserver.py build/module/RDPServer,ffa "$OUT/keys" $PORT "$OUT" 900 & SPID=$!
trap 'kill $SPID $XPID 2>/dev/null' EXIT
for i in $(seq 1 30); do grep -q "listening on port" "$OUT/server.log" 2>/dev/null && break; sleep 1; done
grep -q "listening on port $PORT" "$OUT/server.log" && ok "the module is listening" || { bad "the module didn't start"; cat "$OUT/server.log"; exit 1; }
ctl () { echo "$1" >>"$OUT/ctl"; }
ups () { grep -c "client up" "$OUT/server.log"; }
gones () { grep -c "client gone" "$OUT/server.log"; }

pixel () { convert "$1" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info:; }
near () {
  local IFS=,; set -- $1 $2
  for d in $(( $1 - $4 )) $(( $2 - $5 )) $(( $3 - $6 )); do
    [ ${d#-} -le 12 ] || return 1
  done
}
shot () { import -window root "$OUT/$1.png" 2>/dev/null; }
expect () {  # shot x,y r,g,b what
  local p; p=$(pixel "$OUT/$1.png" ${2%,*} ${2#*,})
  near "$p" "$3" && ok "$1: $4 at $2" || bad "$1: $4 at $2 is $p"
}
wait_for () {  # count-command target seconds
  for i in $(seq 1 $(( $3 * 4 ))); do [ "$($1)" -ge "$2" ] && return 0; sleep 0.25; done
  return 1
}
quarters () {
  expect "$1" 100,200 255,0,0 red; expect "$1" 500,100 0,255,0 green
  expect "$1" 100,300 0,0,255 blue; expect "$1" 500,400 128,128,128 grey
  expect "$1" 30,90 255,255,255 white-square
}

# --- 1: a client, its picture and input --------------------------------
rdesktop -u test -p x -a 32 -N localhost:$PORT >"$OUT/rdesktop-32.log" 2>&1 & CPID=$!
wait_for ups 1 60 && ok "rdesktop (32 bpp) connected" || bad "rdesktop didn't come up"
sleep 3; shot rdesktop-32; quarters rdesktop-32
grep -q "^clients 1$" "$OUT/input.log" && ok "RDPServer\$Clients set" || bad "RDPServer\$Clients not set"
grep -A1 "^clients 1$" "$OUT/input.log" | grep -q "^overlay off$" && ok "EGL\$Overlay off while a client is connected" || bad "EGL\$Overlay not off with a client"
: >"$OUT/input.log"
xdotool mousemove 200 150 click 1 >/dev/null 2>&1; sleep 1
xdotool key a >/dev/null 2>&1; sleep 1
grep -q "^mouse 200 150$" "$OUT/input.log" && ok "mouse moved to 200,150" || bad "mouse: $(head -3 "$OUT/input.log" | tr '\n' ' ')"
grep -q "^key 70 down$" "$OUT/input.log" && grep -q "^key 70 up$" "$OUT/input.log" \
  && ok "click: Select down and up" || bad "click not seen"
grep -q "^key 3C down$" "$OUT/input.log" && ok "A key: &3C" || bad "A key not seen"

# --- 2: an error box opens on RISC OS ------------------------------------
ctl errorbox
sleep 3; shot errorbox
expect errorbox 320,240 255,0,255 "the error box (sent while it's open)"
expect errorbox 100,300 0,0,255 "the rest of the screen"
xdotool mousemove 320 240 click 1 >/dev/null 2>&1
for i in $(seq 1 20); do grep -q "error box closed by a click" "$OUT/server.log" && break; sleep 0.5; done
grep -q "error box closed by a click from the client" "$OUT/server.log" \
  && ok "the client's click reached RISC OS while the box was open" || bad "the click never arrived"
sleep 3; shot after-box
expect after-box 320,240 128,128,128 "the box gone again"
quarters after-box

# --- 3: Disconnect, mid-session ------------------------------------------
g=$(gones)
ctl disconnect
wait_for gones $((g + 1)) 5 && ok "Disconnect: the client is dropped at once" || bad "Disconnect didn't drop the client"
grep -q "client gone (disconnected from this computer)" "$OUT/server.log" && ok "and the log says why" || bad "no reason logged"
for i in $(seq 1 20); do kill -0 $CPID 2>/dev/null || break; sleep 0.5; done
kill -0 $CPID 2>/dev/null && { bad "rdesktop still connected"; kill $CPID; } || ok "rdesktop saw the connection close"
! grep -q 'auto-reconnect: "test" is back' "$OUT/server.log" && ok "and didn't connect again by itself (Disconnect is on purpose)" || bad "the client came back after Disconnect"
wait $CPID 2>/dev/null
grep -q "^clients 0$" "$OUT/input.log" && ok "RDPServer\$Clients back to 0" || bad "RDPServer\$Clients not cleared"
grep -A1 "^clients 0$" "$OUT/input.log" | grep -q "^overlay (unset)$" && ok "EGL\$Overlay unset again, as it was" || bad "EGL\$Overlay not put back"

# --- 4: Disconnect, mid-handshake ----------------------------------------
exec 3<>/dev/tcp/127.0.0.1/$PORT; printf '\x03' >&3
sleep 1.5
ctl status
sleep 0.5
grep "emu: status" "$OUT/server.log" | tail -1 | grep -q "connecting 1" && ok "a silent client is mid-handshake" || bad "not connecting: $(grep 'emu: status' "$OUT/server.log" | tail -1)"
g=$(gones)
ctl disconnect
wait_for gones $((g + 1)) 3 && ok "Disconnect mid-handshake drops it at once" || bad "Disconnect mid-handshake didn't"
exec 3>&-

# --- 5: the next client is fine ------------------------------------------
sleep 1
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:16 >"$OUT/freerdp-16.log" 2>&1 & CPID=$!
u=$(ups)
wait_for ups $((u + 1)) 60 && ok "FreeRDP (16 bpp) connected" || bad "FreeRDP didn't come up"
sleep 3; shot freerdp-16; quarters freerdp-16
# an error box while FreeRDP watches
ctl errorbox
sleep 3; shot freerdp-errorbox
expect freerdp-errorbox 320,240 255,0,255 "the error box"
xdotool mousemove 330 250 click 1 >/dev/null 2>&1
for i in $(seq 1 20); do [ "$(grep -c "error box closed by a click" "$OUT/server.log")" -ge 2 ] && break; sleep 0.5; done
[ "$(grep -c "error box closed by a click" "$OUT/server.log")" -ge 2 ] && ok "FreeRDP's click closed it" || bad "FreeRDP's click didn't arrive"
# another screen bank shown (double-buffering): not a mode change
g=$(gones)
ctl bank
sleep 3
[ "$(gones)" -eq "$g" ] && ! grep -q "desktop is now" "$OUT/server.log" \
  && ok "a screen bank swap isn't taken for a mode change" || bad "a bank swap ended or resized the session"
# output switched to a sprite in some program's turn (a runaway program,
# say), for many reads: the sprite's mode isn't the screen's
ctl "sprite 40"
sleep 8; shot freerdp-sprite
! grep -q "screen mode changed" "$OUT/server.log" && [ "$(gones)" -eq "$g" ] \
  && ok "output redirected to a sprite isn't taken for a mode change" || bad "redirected output: $(grep 'mode changed' "$OUT/server.log" | tail -1)"
expect freerdp-sprite 100,400 0,0,255 "the screen still sent while output is redirected: blue"
# a mode change while FreeRDP watches: the client is resized, carries on
ctl "mode 800x600"
for i in $(seq 1 30); do grep -q "desktop is now 800 x 600" "$OUT/server.log" && break; sleep 0.5; done
sleep 4; shot freerdp-resized
grep -q "the client's desktop is now 800 x 600" "$OUT/server.log" && ok "mode change: the client resized to 800 x 600" || bad "no resize"
expect freerdp-resized 700,550 128,128,128 "the new screen's grey"
expect freerdp-resized 100,400 0,0,255 "blue"
[ "$(gones)" -eq "$g" ] && ok "the session carried on through the mode change" || bad "the session ended at the mode change"
ctl "mode 640x480"
for i in $(seq 1 30); do grep -q "desktop is now 640 x 480" "$OUT/server.log" && break; sleep 0.5; done
grep -q "the client's desktop is now 640 x 480" "$OUT/server.log" && ok "and back" || bad "no resize back"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null

# --- 5b: auto-reconnect ----------------------------------------------------
# The server thread thrown away (as when Alt-Break stops a program in the
# middle of the server's turn): the module starts the server again and cuts
# the connection; FreeRDP with /auto-reconnect connects again by itself,
# with the auto-reconnect cookie the server gave it.
sleep 2
u=$(ups)
backs () { grep -c 'auto-reconnect: "test" is back' "$OUT/server.log"; }
losts () { grep -c "the server was lost" "$OUT/server.log"; }
b=$(backs); l=$(losts)
xfreerdp3 /v:localhost:$PORT /u:test /p:x /sec:rdp /cert:ignore /bpp:32 /auto-reconnect \
  /auto-reconnect-max-retries:10 >"$OUT/freerdp-arc.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do [ "$(ups)" -gt "$u" ] && break; sleep 1; done
sleep 2
ctl unwind
for i in $(seq 1 40); do [ "$(backs)" -gt "$b" ] && break; sleep 1; done
[ "$(losts)" -gt "$l" ] && ok "the thread thrown away: the module started the server again" || bad "no restart after the thread was thrown away"
[ "$(backs)" -gt "$b" ] && [ "$(ups)" -ge $((u + 2)) ] \
  && ok "FreeRDP connected again by itself, with its auto-reconnect cookie" \
  || bad "no auto-reconnect ($(tail -3 "$OUT/freerdp-arc.log" | tr '\n' ' '))"
sleep 3; shot freerdp-arc
expect freerdp-arc 100,400 0,0,255 "after reconnecting: blue"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null

# --- 6: a wrong password --------------------------------------------------
sleep 2
u=$(ups)
rdesktop -u test -p wrong -a 32 -N localhost:$PORT >"$OUT/wrongpw.log" 2>&1 & CPID=$!
for i in $(seq 1 30); do grep -q "login refused" "$OUT/server.log" && break; sleep 1; done
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
grep -q 'login refused for "test"' "$OUT/server.log" && [ "$(ups)" -eq "$u" ] && ok "a wrong password is refused" || bad "a wrong password wasn't refused"

ctl stop
wait $SPID; st=$?
[ $st -eq 0 ] && ok "the emulated module ran without a fault" || { bad "modserver exit $st"; grep "FATAL\|last SWIs" "$OUT/server.log"; }
grep -q "emu: finalised: ok" "$OUT/server.log" && ok "the module was killed cleanly" || bad "finalisation: $(grep 'finalised' "$OUT/server.log")"
grep "longest turn" "$OUT/server.log"
echo "e2e (module): $fails failed"
[ $fails -eq 0 ]
