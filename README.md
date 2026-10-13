# riscos-rdpserver

An RDP server for RISC OS, so the RISC OS desktop can be used from the
Remote Desktop clients built into Windows, macOS, iOS, Android and Linux.

**Status: design and measurements.** There is no server yet. What's here:

- `docs/DESIGN.md`: the plan: libxrdp (from xrdp) for the protocol,
  AcornSSL for TLS, the screen read and compared in tiles, input through
  KeyV and OS_Word 21, the VideoCore's H.264 encoder later.
- `probe/`: **RDPSProbe 0.2**, a test program for a Raspberry Pi. It
  measures how fast the screen can be read and compared, and checks that
  the mouse and keyboard can be driven the way a remote client would.

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

MIT (see `LICENSE`). Files copied from other projects will keep their own
licences, listed in `LICENCES.md`.

Parts of this program were written with the help of an AI assistant.
