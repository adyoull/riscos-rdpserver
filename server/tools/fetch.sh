#!/bin/sh
# RDPServer: fetches the two upstream sources at fixed commits into
# build/upstream (not kept in git), checks the commits, and makes
# build/xrdp: the part of xrdp the server uses, with patches/xrdp applied.
#   tools/fetch.sh            (from the server folder)
set -eu
cd "$(dirname "$0")/.."
UP=build/upstream
XRDP_TAG=v0.10.6.1
XRDP_COMMIT=80c52879fead585d6347df5b3c587718adf8f5a5
MBEDTLS_TAG=v3.6.7
MBEDTLS_COMMIT=068ff080b369adfac81509f9b57b2afabaf82dc5
mkdir -p "$UP"

get () {   # name url tag commit
  if [ ! -d "$UP/$1/.git" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 -b "$3" "$2" "$UP/$1"
  fi
  got=$(git -C "$UP/$1" rev-parse HEAD)
  if [ "$got" != "$4" ]; then
    echo "fetch.sh: $1 is at $got, not $4" >&2
    exit 1
  fi
}
get xrdp https://github.com/neutrinolabs/xrdp "$XRDP_TAG" "$XRDP_COMMIT"
get mbedtls https://github.com/Mbed-TLS/mbedtls "$MBEDTLS_TAG" "$MBEDTLS_COMMIT"

# The xrdp files the server builds: libxrdp (but the RemoteFX surface code)
# and the common files it uses. os_calls.c and ssl_calls.c are replaced by
# src/os_calls.c and src/ssl_calls.c.
rm -rf build/xrdp
mkdir -p build/xrdp/libxrdp build/xrdp/common
cp "$UP"/xrdp/libxrdp/*.c "$UP"/xrdp/libxrdp/*.h build/xrdp/libxrdp/
rm build/xrdp/libxrdp/xrdp_surface.c
for f in parse list list16 log trans string_calls file fifo pixman-region16 base64 guid; do
  cp "$UP/xrdp/common/$f.c" build/xrdp/common/
done
cp "$UP"/xrdp/common/pixman-region.c "$UP"/xrdp/common/*.h build/xrdp/common/
cp "$UP/xrdp/COPYING" build/xrdp/COPYING
for p in patches/xrdp/*.patch; do
  [ -e "$p" ] || continue
  patch -s -d build/xrdp -p1 < "$p"
done
echo "fetch.sh: xrdp $XRDP_TAG and mbedTLS $MBEDTLS_TAG ready in build/"
