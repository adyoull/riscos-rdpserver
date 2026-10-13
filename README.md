# riscos-rdpserver

An RDP server for RISC OS, so the RISC OS desktop can be used from the
Remote Desktop clients built into Windows, macOS, iOS, Android and Linux.

**Status: test builds (0.1-test3).** Connects from Windows App on a Pi 4
running RISC OS 5; tested on Linux with rdesktop and FreeRDP. What's here:

- `server/`: **RDPServer 0.1-test3**, built on xrdp's libxrdp. A desktop
  program (icon bar icon, Status and Choices windows). Standard RDP
  Security only (RC4: TLS waits for AcornSSL to work as a server),
  classic RDP bitmaps (interleaved RLE, 32 bpp planar) at 8, 15, 16, 24 and
  32 bpp, changed 64x64 tiles only, mouse, scroll wheel and keyboard, a
  user name and password set in Choices. One client at a time. For your
  own network only.

- `docs/DESIGN.md`: the plan: libxrdp (from xrdp) for the protocol,
  AcornSSL for TLS, the screen read and compared in tiles, input through
  KeyV and OS_Word 21, the VideoCore's H.264 encoder later.
- `probe/`: **RDPSProbe 0.2**, a test program for a Raspberry Pi. It
  measures how fast the screen can be read and compared, and checks that
  the mouse and keyboard can be driven the way a remote client would.

## Building RDPServer

Needs riscos-crossdev (as below), git and network access to github.com,
and for the Linux tests Xvfb, rdesktop, xfreerdp3, ImageMagick and xdotool.

```sh
cd server
tools/fetch.sh                                    # xrdp and Mbed TLS, patched
make RCD_DIR=/path/to/riscos-crossdev             # build/RDPServer-0.1-test3.zip
make RCD_DIR=/path/to/riscos-crossdev host qemu check
tests/e2e.sh                                      # real clients, Linux build
RCD_DIR=/path/to/riscos-crossdev QEMU=1 tests/e2e.sh   # ARM build, alignment traps on
```

## Building RDPSProbe

Needs [riscos-crossdev](https://github.com/adyoull/riscos-crossdev) (a
tagged release, with `make prebuilt` done).

```sh
cd probe
make RCD_DIR=/path/to/riscos-crossdev check   # checks, fake RISC OS and QEMU tests
make RCD_DIR=/path/to/riscos-crossdev         # out/RDPSProbe-0.2.zip
```

On RISC OS, unzip it and double-click `!RDPSProbe`. It needs
SharedUnixLibrary and ARMEABISupport (PackMan). It writes `Report` inside
the application.

## Licence

MIT (see `LICENSE`). The server is built with xrdp's libxrdp and Mbed TLS
(both Apache-2.0), fetched at build time: see `LICENCES.md`.

Parts of this program were written with the help of an AI assistant.
