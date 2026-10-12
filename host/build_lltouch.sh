#!/usr/bin/env bash
# Cross-build host/prebuilt/lltouch.so (aarch64 glibc, symbol versions kept at GLIBC_2.17).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
docker run --rm -v "$PWD/host:/src:ro" -v "$PWD/build:/out" debian:bullseye bash -c '
    apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        gcc-aarch64-linux-gnu libc6-dev-arm64-cross >/dev/null &&
    aarch64-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -o /out/lltouch.so /src/lltouch.c -ldl -lpthread &&
    aarch64-linux-gnu-strip /out/lltouch.so && chown '"$(id -u):$(id -g)"' /out/lltouch.so'
cp build/lltouch.so host/prebuilt/lltouch.so
