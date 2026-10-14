# riscos-rdpserver

An RDP server for RISC OS, so the RISC OS desktop can be used from the
Remote Desktop clients built into Windows, macOS, iOS, Android and Linux.

**Status: release candidate (0.2-rc1).** Runs on a Raspberry Pi 4 with
RISC OS 5, used from Windows App on a Mac; also tested from rdesktop and
FreeRDP on Linux.
What's here:

- `server/`: **RDPServer 0.2-rc1**, built on xrdp's libxrdp. The server
  is a relocatable module (`src/mod`), so it keeps going while the desktop
  doesn't multitask (a Wimp error box, a program holding the machine, or
  one stopped with Alt-Break: the server restarts and clients reconnect
  by themselves); a desktop front end (icon bar icon, Status and Choices
  windows) runs it. Standard RDP Security only (RC4: TLS waits for
  AcornSSL to work as a server or use mbedTLS directly), classic RDP bitmaps (interleaved RLE,
  32 bpp planar) at 8, 15, 16, 24 and 32 bpp, changed 64x64 tiles only,
  mouse, scroll wheel and keyboard, sound to the client (RDPSND, PCM),
  a user name and password set in Choices. One client at a time (RISC OS
  has one desktop and one user). **For
  your own network only.** Video and 3D in a hardware overlay (Reel,
  EGL/Mesa programs) show as a black window on the client.

- `probe/`: **RDPSProbe 0.2**, a test program for a Raspberry Pi. It
  measures how fast the screen can be read and compared, and checks that
  the mouse and keyboard can be driven the way a remote client would.

## Building RDPServer

Needs riscos-crossdev (as below), git and network access to github.com,
and for the Linux tests Xvfb, rdesktop, xfreerdp3, ImageMagick, xdotool and
the Python modules unicorn and pyelftools (pyelftools also for the module's
link: tools/modlink.py).

```sh
cd server
tools/fetch.sh                                    # xrdp and Mbed TLS, patched
make RCD_DIR=/path/to/riscos-crossdev             # build/RDPServer-0.2-rc1.zip
make RCD_DIR=/path/to/riscos-crossdev host qemu module check
tests/e2e.sh                                      # real clients, Linux build
RCD_DIR=/path/to/riscos-crossdev QEMU=1 tests/e2e.sh   # ARM build, alignment traps on
tests/e2e_module.sh                               # real clients, the module emulated
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
