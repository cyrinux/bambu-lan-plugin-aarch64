#!/usr/bin/env bash
# Builds and packages a release tarball:
#   dist/bambu-lan-plugin-<version>-studio-<abi>-aarch64.tar.gz (+ .sha256)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
ABI="$(sed -n 's/#define BBL_LAN_PLUGIN_VERSION "\([0-9.]*\)"/\1/p' src/networking/version.hpp)"
ARCH="$(uname -m)"
NAME="bambu-lan-plugin-${VERSION}-studio-${ABI%.*}-${ARCH}"

[[ ${SKIP_BUILD:-0} == 1 ]] || scripts/build.sh

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME"
cp dist/libbambu_networking.so dist/libBambuSource.so scripts/install.sh README.md LICENSE "$STAGE/$NAME/"
( cd "$STAGE/$NAME" && sha256sum libbambu_networking.so libBambuSource.so > SHA256SUMS )
tar -C "$STAGE" --owner=0 --group=0 --sort=name --mtime='2026-01-01' -czf "dist/$NAME.tar.gz" "$NAME"
( cd dist && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
echo "dist/$NAME.tar.gz"
