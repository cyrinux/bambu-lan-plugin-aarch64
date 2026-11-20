#!/usr/bin/env bash
# Builds and packages a release tarball:
#   dist/bambu-lan-plugin-<version>-<arch>.tar.gz (+ .sha256)
# containing the Bambu Studio and OrcaSlicer networking plugins, the shared
# camera plugin, install.sh and a COMPAT file naming the slicer versions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
ARCH="$(uname -m)"
NAME="bambu-lan-plugin-${VERSION}-${ARCH}"
LIBS=(libbambu_networking.so libbambu_networking_orca.so libBambuSource.so)

[[ ${SKIP_BUILD:-0} == 1 ]] || scripts/build.sh

macro() { sed -n "s/#define $1 \"\([^\"]*\)\".*/\1/p" src/networking/version.hpp; }
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME"
cp "${LIBS[@]/#/dist/}" scripts/install.sh README.md LICENSE "$STAGE/$NAME/"
{
  printf 'plugin=%s\n' "$VERSION"
  printf 'studio_tag=%s\nstudio_plugin_version=%s\n' "$(sed -n 's/^tag=//p' third_party/bambu/UPSTREAM)" "$(macro BBL_STUDIO_PLUGIN_VERSION)"
  printf 'orca_tag=%s\norca_plugin_base=%s\norca_plugin_version=%s\n' \
    "$(sed -n 's/^tag=//p' third_party/orca/UPSTREAM)" "$(sed -n 's/^plugin_base=//p' third_party/orca/UPSTREAM)" \
    "$(macro BBL_ORCA_PLUGIN_VERSION)"
} > "$STAGE/$NAME/COMPAT"
( cd "$STAGE/$NAME" && sha256sum "${LIBS[@]}" > SHA256SUMS )
tar -C "$STAGE" --owner=0 --group=0 --sort=name --mtime='2026-01-01' -czf "dist/$NAME.tar.gz" "$NAME"
( cd dist && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
echo "dist/$NAME.tar.gz"
