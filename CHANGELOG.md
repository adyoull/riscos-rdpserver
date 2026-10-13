# Changelog

## RDPServer 0.1-test3 (13 October 2026)

- A desktop program now, not a task window. test2 used about 70% of the
  Pi's time: in a task window, UnixLib's `select()` busy-waits through its
  timeout. Now the server runs from the Wimp poll loop and sleeps in
  `Wimp_PollIdle` until it next needs to look at the screen or the
  network (at most every 2 cs while a client is connected, 25 cs when
  idle). While the client is behind, it waits 2 cs rather than polling.
- Icon bar icon (a different picture while a client is connected); menu
  with Info, Status..., Choices..., Disconnect, Quit. Status shows the
  state, the client, and the last messages. Choices sets the port and the
  user name and password (Save, Clear login), as asked for in test2.
- The screen is read 128 rows per step on the desktop, the scan period is
  10 cs while things change and 25 cs after 2 s without a change, and a
  client gets 10 s to finish connecting (the desktop waits meanwhile).
- Clicks acting as Shift-clicks: the test2 log shows the client sent no
  Shift, so RISC OS itself must have had Shift held. Before each button
  goes down, the server now reads Shift, Ctrl and Alt (OS_Byte 121) and,
  if RISC OS has one held that the client didn't press, releases it
  (KeyV) and says so in the log. A Shift-click from the client keeps its
  Shift.
- `-v` no longer names ordinary keys in the log (only Shift, Ctrl, Alt,
  the locks and mouse buttons), so a password typed remotely isn't kept.
- Messages go to Choices:RDPServer.Log, libxrdp's to XrdpLog. `-cli` runs
  it in a task window as before.

## RDPServer 0.1-test2 (13 October 2026)

- Clicks: the Pi showed a left click acting oddly (a double-click opened
  the file in !Edit, as with Shift held). Input is now paced: moves go at
  once, each button or key change waits 3 cs after the last move and 2 cs
  after the last change, so RISC OS records the button where the pointer
  is. `-v` (on in !Run for test builds) shows every button, key and wheel
  event and what went to KeyV, to find the rest of the problem.
- The scroll wheel: RDP wheel rotation (120 a notch, small steps added
  up) to PointerV 9, as the USB mouse driver reports it.
- A login: `RDPServer -setlogin NAME PASSWORD` (salted SHA-256 in
  Choices:RDPServer.logins), checked before the client sees anything; a
  wrong one is refused and new connections wait 3 s. `-clearlogin`.
- `RDPServer$Clients` is 1 while a client is connected, for programs
  using hardware video overlays (handoff to Reel and EGL).

## RDPServer 0.1-test1 (13 October 2026)

- First test build. xrdp 0.10.6.1's libxrdp with new `os_calls`/`ssl_calls`
  for UnixLib (crypto from Mbed TLS 3.6.7; RC4 written here). Standard RDP
  Security, 2048-bit key made on first run (Choices:RDPServer.rsakeys/ini).
- Patches to libxrdp: the desktop is the RISC OS screen's size (0001);
  salted MACs, which FreeRDP sends by default (0002); no Cache Color Table
  order with the 8 bpp palette, which FreeRDP 3 rejects (0003).
- The screen is read a row at a time with memcpy and compared with the
  last frame in 64x64 tiles (RDPSProbe's findings); changed tiles go as
  bitmap updates in the client's depth. 16 and 32 bpp RISC OS modes.
- Input: OS_Word 21,3 for the mouse; KeyV for buttons (left Select,
  middle Menu, right Adjust) and keys (scancode to low-level key number).
  Everything pressed is released when the client goes or resynchronises.
- Output waits while the client hasn't taken the last lot; a client that
  doesn't finish connecting in 30 s is dropped; a failed accept() doesn't
  stop the listener; a screen mode change ends the session.
- Tested on Linux only: unit tests, plat_riscos against the fake RISC OS,
  and tests/e2e.sh (rdesktop at 8/16/24/32 bpp, FreeRDP at 8/15/16/32,
  input, a stalled client), also as ARM code under qemu-arm-aligntrap.

## RDPSProbe 0.2 (13 October 2026)

- 0.1 on a Pi 4 (1920x1080, 32 bpp) read the screen with memcpy at
  680 MB/s, but compared it with memcmp at only 42 MB/s: small loads from
  the screen are slow. 0.2 times two more ways: 32-bit loads straight from
  the screen, and each row copied out with memcpy then compared in memory
  (the copy kept up to date, as a server would).
- `-k` (on in !Run): presses and releases Left Shift through KeyV
  (OS_CallAVector) and checks OS_Byte 121 sees it go down and up.
  Not yet run on RISC OS.

## RDPSProbe 0.1 (13 October 2026)

- First version: reads the screen mode; times copying the screen to
  memory, memory to memory, comparing the screen with a copy, and a 64x64
  tile scan; with `-p`, moves the mouse with OS_Word 21 and checks the
  pointer follows, then puts it back. Not yet run on RISC OS.
