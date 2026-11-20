#!/usr/bin/env bash
# Tracks Bambu Studio and OrcaSlicer releases. See docs/updating.md.
#
#   scripts/upstream.sh check             pinned vs latest tags, installed slicers, runtime vs build SDK
#   scripts/upstream.sh sync <tag>        Bambu Studio: vendor that tag's plugin ABI headers into
#                                         third_party/bambu, regenerate the typedefs, bump the version
#   scripts/upstream.sh sync-orca <tag>   same for OrcaSlicer into third_party/orca
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STUDIO_REPO="bambulab/BambuStudio"
ORCA_REPO="OrcaSlicer/OrcaSlicer"
STUDIO_DIR="$ROOT/third_party/bambu"
ORCA_DIR="$ROOT/third_party/orca"
VERSION_HPP="$ROOT/src/networking/version.hpp"
APP="com.bambulab.BambuStudio"
SDK="${BBL_SDK:-org.freedesktop.Sdk//26.08}"

pinned() { sed -n "s/^$2=//p" "$1/UPSTREAM"; }
plugin_version() { sed -n "s/#define $1 \"\([^\"]*\)\".*/\1/p" "$VERSION_HPP"; }

latest_tag() {  # <repo> <glob>
  git ls-remote --tags --refs "https://github.com/$1.git" "$2" |
    sed 's|.*refs/tags/||' | grep -vE 'alpha|beta|rc|dev' | sort -V | tail -1
}

fetch() {  # <repo> <tag> <dest dir> <path>...
  local repo="$1" tag="$2" dest="$3"; shift 3
  for f in "$@"; do
    curl -fsSL "https://raw.githubusercontent.com/$repo/$tag/$f" -o "$dest/$(basename "$f")"
  done
}

# Function-pointer typedefs (some span several lines) → <out> header used by
# abi_check.hpp to static_assert our exported signatures.
gen_abi() {  # <source header> <out> <origin note> <namespace>
  python3 -I - "$@" <<'PY'
import re, sys
src, out, origin, ns = sys.argv[1:]
defs = re.findall(r'typedef[^;]*?\(\*func_\w+\)\s*\([^;]*?\);', open(src).read(), re.S)
with open(out, 'w') as f:
    f.write(f"// Extracted from {origin} (AGPL-3.0)\n")
    f.write("// by scripts/upstream.sh. Used to static_assert our exported signatures.\n")
    f.write('#pragma once\n#include "bambu_networking.hpp"\n')
    f.write("namespace Slic3r { class BBLModelTask; using OnGetSubTaskFn = std::function<void(BBLModelTask*)>; }\n")
    f.write(f"namespace bbl_abi {{\nusing namespace {ns};\nusing Slic3r::BBLModelTask; using Slic3r::OnGetSubTaskFn;\n")
    f.write("\n".join(" ".join(d.split()) for d in defs) + "\n}\n")
print(f"{len(defs)} typedefs")
PY
}

set_version() {  # <macro> <value>
  sed -i "s/#define $1 \".*\"/#define $1 \"$2\"/" "$VERSION_HPP"
}

# glibc / libstdc++ / OpenSSL / curl as seen inside a flatpak app or runtime.
abi_fingerprint() {
  flatpak run --command=sh "$1" -c '
    printf "glibc     %s\n" "$(ldd --version 2>/dev/null | head -1 | grep -o "[0-9.]*$")"
    for l in libstdc++.so.6 libssl.so.3 libcurl.so.4; do
      f=$(ls -1 /usr/lib/*/$l.* /usr/lib/$l.* /app/lib/$l.* 2>/dev/null | sort -V | tail -1)
      printf "%-9s %s\n" "${l%%.so*}" "${f##*/}"
    done' 2>/dev/null
}

cmd_check() {
  echo "== Bambu Studio"
  echo "pinned tag          : $(pinned "$STUDIO_DIR" tag)"
  echo "latest tag          : $(latest_tag "$STUDIO_REPO" 'v0*')"
  if flatpak info "$APP" >/dev/null 2>&1; then
    local bin ver
    bin="$(flatpak info --show-location "$APP")/files/bin/bambu-studio"
    ver="$(strings "$bin" | grep -m1 -E '^0[0-9]\.[0-9]{2}\.[0-9]{2}\.[0-9]{2}$' || true)"
    echo "installed flatpak   : $(flatpak info "$APP" | sed -n 's/^ *Version: //p') (SLIC3R_VERSION $ver)"
    echo "flatpak runtime     : $(flatpak info --show-metadata "$APP" | sed -n 's/^runtime=//p')"
  fi
  echo "plugin reports      : $(plugin_version BBL_STUDIO_PLUGIN_VERSION)"
  echo
  echo "== OrcaSlicer"
  echo "pinned tag          : $(pinned "$ORCA_DIR" tag) (plugin base $(pinned "$ORCA_DIR" plugin_base))"
  echo "latest tag          : $(latest_tag "$ORCA_REPO" 'v2*')"
  if command -v orca-slicer >/dev/null; then
    echo "installed           : $(readlink -f "$(command -v orca-slicer)")"
  fi
  echo "plugin reports      : $(plugin_version BBL_ORCA_PLUGIN_VERSION)"
  echo
  if flatpak info "$APP" >/dev/null 2>&1 && flatpak info "$SDK" >/dev/null 2>&1; then
    diff <(abi_fingerprint "$APP") <(abi_fingerprint "$SDK") \
      --label "runtime of $APP" --label "build SDK $SDK" -u &&
      echo "Studio runtime and build SDK match (glibc, libstdc++, OpenSSL, curl)"
  else
    echo "install $APP and $SDK to compare runtime and build SDK"
  fi
}

cmd_sync() {
  local tag="${1:?usage: upstream.sh sync <tag, e.g. v02.10.00.50>}"
  local tmp; tmp="$(mktemp -d)"; trap "rm -rf '$tmp'" EXIT
  fetch "$STUDIO_REPO" "$tag" "$tmp" \
    src/slic3r/Utils/bambu_networking.hpp src/slic3r/Utils/NetworkAgent.hpp \
    src/slic3r/GUI/Printer/BambuTunnel.h src/slic3r/Utils/FileTransferUtils.hpp version.inc
  cp "$tmp/bambu_networking.hpp" "$tmp/BambuTunnel.h" "$STUDIO_DIR/"
  cp "$tmp/FileTransferUtils.hpp" "$STUDIO_DIR/FileTransferUtils.hpp.ref"
  gen_abi "$tmp/NetworkAgent.hpp" "$STUDIO_DIR/network_agent_abi.hpp" \
    "BambuStudio $tag src/slic3r/Utils/NetworkAgent.hpp" BBL

  local slic3r
  slic3r="$(sed -n 's/set(SLIC3R_VERSION "\(.*\)")/\1/p' "$tmp/version.inc")"
  [[ $slic3r =~ ^[0-9]{2}\.[0-9]{2}\.[0-9]{2} ]] || { echo "cannot read SLIC3R_VERSION from version.inc" >&2; return 1; }
  set_version BBL_STUDIO_PLUGIN_VERSION "${slic3r:0:8}.99"
  printf 'repo=%s\ntag=%s\nslic3r_version=%s\n' "$STUDIO_REPO" "$tag" "$slic3r" > "$STUDIO_DIR/UPSTREAM"
  echo "synced Studio $tag (SLIC3R_VERSION $slic3r), plugin version now ${slic3r:0:8}.99"
  git -C "$ROOT" diff --stat -- third_party/bambu "$VERSION_HPP"
}

cmd_sync_orca() {
  local tag="${1:?usage: upstream.sh sync-orca <tag, e.g. v2.4.2>}"
  local tmp; tmp="$(mktemp -d)"; trap "rm -rf '$tmp'" EXIT
  fetch "$ORCA_REPO" "$tag" "$tmp" \
    src/slic3r/Utils/bambu_networking.hpp src/slic3r/Utils/BBLNetworkPlugin.hpp \
    src/libslic3r/AppConfig.hpp
  mkdir -p "$ORCA_DIR"

  # Orca's header lives in namespace Slic3r and includes libslic3r/AppConfig.hpp
  # only for BAMBU_NETWORK_AGENT_VERSION_LEGACY: inline that define, and alias
  # the namespace so our code can keep writing BBL::PrintParams.
  local legacy
  legacy="$(grep -o 'BAMBU_NETWORK_AGENT_VERSION_LEGACY "[0-9.]*"' "$tmp/AppConfig.hpp" | tail -1 | cut -d' ' -f2)"
  {
    printf '// OrcaSlicer %s src/slic3r/Utils/bambu_networking.hpp (AGPL-3.0), adapted by\n' "$tag"
    printf '// scripts/upstream.sh: AppConfig.hpp include replaced, BBL namespace alias added.\n'
    sed "s|#include \"libslic3r/AppConfig.hpp\"|#ifndef BAMBU_NETWORK_AGENT_VERSION_LEGACY\n#define BAMBU_NETWORK_AGENT_VERSION_LEGACY $legacy\n#endif|" \
      "$tmp/bambu_networking.hpp"
    printf '\nnamespace BBL { using namespace Slic3r; }\n'
  } > "$ORCA_DIR/bambu_networking.hpp"
  gen_abi "$tmp/BBLNetworkPlugin.hpp" "$ORCA_DIR/network_agent_abi.hpp" \
    "OrcaSlicer $tag src/slic3r/Utils/BBLNetworkPlugin.hpp" Slic3r

  # Newest Bambu plugin version Orca offers ("is_latest" entry): our file is
  # installed as libbambu_networking_<base>-lan.so and reports <base[0:8]>.99.
  local base
  base="$(grep -m1 -E '\{"[0-9.]+", *"[^"]*", *nullptr, *true' "$tmp/bambu_networking.hpp" | grep -o '[0-9][0-9.]*' | head -1)"
  [[ -n $base ]] || { echo "could not find the is_latest entry of AVAILABLE_NETWORK_VERSIONS" >&2; return 1; }
  set_version BBL_ORCA_PLUGIN_VERSION "${base:0:8}.99"
  printf 'repo=%s\ntag=%s\nplugin_base=%s\n' "$ORCA_REPO" "$tag" "$base" > "$ORCA_DIR/UPSTREAM"
  echo "synced Orca $tag (plugin base $base), plugin version now ${base:0:8}.99"
  git -C "$ROOT" diff --stat -- third_party/orca "$VERSION_HPP" || true
}

case "${1:-}" in
  check) cmd_check ;;
  sync) shift; cmd_sync "$@" ;;
  sync-orca) shift; cmd_sync_orca "$@" ;;
  *) echo "usage: $0 check | sync <studio tag> | sync-orca <orca tag>" >&2; exit 2 ;;
esac
