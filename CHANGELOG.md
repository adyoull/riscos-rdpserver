# Changelog

## RDPServer 0.2-test3 (13 October 2026)

- A session ended with "client gone (the screen mode changed)" when the
  mode hadn't changed: a program that double-buffers (it swaps screen
  banks with OS_Byte 113, as Reel does) moves the address of the
  displayed screen, and 0.2-test2 took any new address for a mode
  change. Now only a change of size, colours or line length counts; a
  bank swap is followed, and the bank that's shown is what's sent.
- A real mode change no longer ends the session: the client's desktop is
  resized to the new mode ([MS-RDPBCGR] deactivation-reactivation) and
  the whole screen is sent again. The server waits for the mode to
  settle first. A client that didn't say it can be resized, or one that
  doesn't come back within 30 seconds, is disconnected, as before.
- The log's "longest step" no longer counts time the server spent
  waiting for other programs (a step that waits for the client yields),
  so it shows the server's own work: 0.2-test2 showed 310 cs for the
  key exchange.
- Video shows on the client: hardware overlays (VideoOverlay) never
  reach screen memory, so the client saw a black area where Reel's
  video or an EGL window was. While a client is connected the server
  sets EGL$Overlay to "off", which Reel and riscos-mesa's EGL read at
  every frame: they draw ordinarily instead. What EGL$Overlay was
  before is put back when the client goes (a string literally, a
  number or macro as it was; unset again if it was unset), unless it
  was changed meanwhile; a value it can't read, or a code variable, is
  left alone.
- The module is 0.04 (!Run's RMEnsure asks for it, so test2's 0.03 is
  replaced).


## RDPServer 0.2-test2 (13 October 2026)

- 0.2-test1 crashed on the Pi as soon as the front end started the
  server: "EMT trap" in RDPServer, the abort inside the Internet module
  (`*Where`: Internet +&4A8). The server thread ran in SVC mode on its
  own stack, and C modules such as Internet find their static data from
  the base of the SVC stack: a SWI (or an interrupt handled by a C
  module) with sp anywhere else reads rubbish. The thread now runs in USR
  mode on its own stack, so its SWIs and interrupts use the SVC stack as
  any program's do; it goes back to SVC mode with OS_EnterOS, and the
  user sp and lr of whatever task was current are put back.
- The emulator test now refuses a SWI made in SVC mode with sp off the
  (8 KB) SVC stack, checks that the current task's user sp and lr come
  back unchanged, calls the front end's entries in USR mode, and traps
  unaligned accesses as RISC OS does. 0.2-test1 fails it.
- The module is 0.03 (!Run's RMEnsure asks for it, so a 0.02 left loaded
  by test1 is replaced).


## RDPServer 0.2-test1 (13 October 2026)

- **The server is now a module** (`RDPSMod`, title RDPServer), so a
  session no longer freezes while a RISC OS error box is open. The Wimp
  stops multitasking while `Wimp_ReportError` waits (its loop, "pollit"
  in the Wimp's s/Wimp07), so the old Wimp task got no time at all: the
  client kept its last picture and couldn't click the box. The module
  runs from a transient callback (an OS_CallEvery ticker every 2 cs asks
  for one) and, while an error box is open (Service_WimpReportError),
  from ByteV: the box's loop calls OS_Byte &81 with R1 = R2 = 0 on every
  pass. The client sees the box and its click goes through.
- The server runs on a stack of its own (one coroutine, src/coro_arm.s).
  Wherever it used to wait it now lets other programs run: libxrdp's
  connection sequence (which held the desktop for up to 10 s), a full
  send buffer, and the first picture (sent in slices of 2 cs).
- **Disconnect drops the client at once**, also one that is still
  connecting: the front end shuts the socket down and the server ends the
  session at its next turn.
- No SWIs, *commands or service call numbers to allocate: the front end
  finds the module with OS_Module 18 and calls the table after its header
  (as UnixLib's PThreadTicker is used). The front end attaches; the module
  won't be killed while its front end runs, stops serving if the front end
  goes (Service_WimpCloseDown), and is removed when RDPServer quits.
- The front end (the icon bar icon, Status, Choices) gives the module the
  files libxrdp reads (xrdp.ini, rsakeys.ini, logins), so the module never
  uses FileSwitch. Its WimpSlot drops from 24000K to 8000K: the copy of
  the screen is in the module's dynamic area, "RDPServer".
- `-cli` (the task window version) is gone on RISC OS.
- Tests: the module in the Unicorn emulator with a pretend RISC OS
  around it (tests/modemu.py): loaded at an address it wasn't linked for,
  RMReInit, the entries, an error box (ByteV only, no callbacks),
  finalisation (52 checks); and with real rdesktop and FreeRDP clients
  (tests/e2e_module.sh: the error box reaches the client and its click
  closes it; Disconnect mid-session and mid-handshake). The front end's
  calls against a pretend module (29 checks), the module's printf/scanf
  against the C library's (28). Not yet run on RISC OS.


## RDPServer 0.1-test9 (13 October 2026)

- The held-button watchdog (test7, test8) is gone: Folio 0.0.1-test32
  fixed the window drag that needed it, and it could cut short a slow
  program's drag. Clicks are as in test6 (the release by ticker event);
  drags and held buttons go down and up as the client sends them.
- Kept from test8: `-v` logs waits of 25 cs or more between the server's
  turns, and the longest wait every 5 s.

## RDPServer 0.1-test8 (13 October 2026)

- test7's log: dragging Folio's document window, the watchdog let go
  eight times (the server had no turn for half a second while the button
  was held); Bookshelf and other windows never. The watchdog now waits
  1.5 s, so a program that's only slow keeps its drag.
- `-v` logs each wait of 25 cs or more between the server's turns while a
  client is connected ("no turn for N cs", noting a held button), and the
  5-second line gives the longest wait. That shows how long a program
  held the desktop.

## RDPServer 0.1-test7 (13 October 2026)

- Dragging !Folio's windows by the title bar still froze the desktop
  (other windows were fine). Folio must run its own loop while the button
  is down, without letting other programs run, so the server can't send
  the button-up or the moves until it's let go. A held button now has a
  watchdog: a ticker event lets go of it if the server hasn't run for
  half a second, so such a program stops waiting, and the log says so.
  Dragging those windows from the client still won't follow the pointer.

## RDPServer 0.1-test6 (13 October 2026)

- Double-clicks work (test5 on the Pi), but a click on some title bars
  (!Folio's) left the button down: the window followed the pointer and
  the desktop stopped until the next real click. test5's ticker event only
  added a callback to do the release, and callbacks wait for a return to
  user mode, which never comes while the Wimp's own loop runs. Now the
  ticker event calls KeyV itself, as mouse and keyboard drivers do from
  their interrupt handlers.

## RDPServer 0.1-test5 (13 October 2026)

- test4 broke clicks altogether: pressing and releasing the button in the
  same moment, without letting the desktop run in between, isn't seen as
  a click. Now a click is: the button goes down, the desktop runs (the
  Wimp sees it down), and a ticker event lets go of it 6 cs later, from a
  small routine in the RMA (src/clickup.s), so the release happens even
  while the Filer is busy checking for double-click-and-hold. After a
  click the server goes straight back to the desktop.
- Only a clean click goes that way: a down, small moves, its up. A drag
  that arrives all at once, or other keys in between, go one by one.
- Tests: the RMA routines run in an ARM emulator (Unicorn, through
  riscos-crossdev's emu/): registers kept, the right KeyV call, the
  pending word cleared.

## RDPServer 0.1-test4 (13 October 2026)

- Double-clicks open applications as folders (and files in !Edit): found.
  test3's log shows RISC OS never had Shift held. The Filer, given a
  double-click, loops reading the mouse without letting other programs
  run, and if the button is still down after a delay takes it as
  "double-click and hold", which it treats as Shift (Filer s/Clicks). The
  server pressed the button, then let the Filer run before it could let
  go. Now a button-down waits for its button-up from the client, and both
  go to RISC OS together, the gap between them waited out inside the
  server. A button held longer than a quarter of a second, or dragged,
  goes down on its own as before.
- The pointer on RISC OS crawling after the client's: each step read
  only part of one message from the client (xrdp's reader takes a PDU in
  stages), and on the desktop a step comes about every 2 cs. Now each step
  reads everything waiting (up to 3 cs), and moves in a row are merged, so
  the pointer jumps to where the client's is.
- `-v` adds a line every 5 s: steps, reads, scans, the longest step.
- `-g` (tests): a pause after each step, as on the desktop; the Linux
  tests now run that way, with a 150-move sweep that must land at once.

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
