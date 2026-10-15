# Licences

RDPServer's own files (everything in this repository) are MIT: see
`LICENSE`.

The server is built with two other projects, which `server/tools/fetch.sh`
fetches at fixed commits into `server/build/upstream` (they are not kept in
this repository):

| Project | Version | Licence | What's used |
|---|---|---|---|
| [xrdp](https://github.com/neutrinolabs/xrdp) | v0.10.6.1 (80c52879) | Apache-2.0 | `libxrdp/` (not `xrdp_surface.c`) and 11 files from `common/`, with the changes in `server/patches/xrdp/` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | v3.6.7 (068ff080) | Apache-2.0 (or GPL-2.0-or-later) | the crypto library, the TLS 1.2 server and X.509 (since 0.3), configured by `server/config/rdps_mbedtls_config.h` |

`server/src/os_calls.c` and `server/src/ssl_calls.c` are new
implementations of xrdp's `common/os_calls.h` and `common/ssl_calls.h`
interfaces. `server/src/server.c` drives a libxrdp session the way xrdp's
`xrdp/xrdp_process.c` does. `server/src/keygen.c` signs the server's key
as xrdp's `keygen/keygen.c` does; the Terminal Services Signing Key's
values are from [MS-RDPBCGR] 5.3.3.1.1.

`server/src/font8x8.c` (the login screen's font) is from
[font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper, based on
Marcel Sondaar's and IBM's VGA fonts: public domain.

The app (`!RDPServer.Licences`) carries this and the Apache-2.0 text.
