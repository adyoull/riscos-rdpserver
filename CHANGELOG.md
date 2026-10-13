# Changelog

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
