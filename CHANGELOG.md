# Changelog

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
