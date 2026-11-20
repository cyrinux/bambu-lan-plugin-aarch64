#!/usr/bin/env bash
# Builds the plugins inside the freedesktop 26.08 SDK, which matches the
# glibc / libstdc++ / OpenSSL / libcurl of Bambu Studio's flatpak runtime
# (org.gnome.Platform 51 is built on freedesktop 26.08).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${BBL_SDK:-org.freedesktop.Sdk//26.08}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

flatpak run --filesystem="$ROOT" --share=network --command=bash "$SDK" -c "
  set -euo pipefail
  cd '$ROOT'
  cmake -S . -B build -DCMAKE_BUILD_TYPE=$BUILD_TYPE >/dev/null
  cmake --build build -j\$(nproc)
  ctest --test-dir build --output-on-failure
  mkdir -p dist
  cp build/libbambu_networking.so build/libbambu_networking_orca.so build/libBambuSource.so dist/
  strip --strip-unneeded dist/*.so
"
echo "Built: $ROOT/dist/{libbambu_networking.so (Studio), libbambu_networking_orca.so (Orca), libBambuSource.so}"
