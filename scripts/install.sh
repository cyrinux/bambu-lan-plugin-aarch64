#!/usr/bin/env bash
# Installs the LAN plugins into a slicer's plugin folder.
#
#   install.sh                   Bambu Studio flatpak (~/.var/app/com.bambulab.BambuStudio/...)
#   install.sh --native          Bambu Studio, non-flatpak (~/.config/BambuStudio)
#   install.sh --orca            OrcaSlicer, non-flatpak (~/.config/OrcaSlicer)
#   install.sh --orca-flatpak    OrcaSlicer flatpak (~/.var/app/io.github.softfever.OrcaSlicer/...)
#   install.sh [target] --uninstall   restore the original (x86-64) files
#
# Original x86-64 files are moved to plugins/x86_64-original/ (never deleted).
# For OrcaSlicer the networking plugin is installed as
#   libbambu_networking_<version>.so     what Orca loads: after loading a plugin it
#                                        saves its get_version() as the configured version
#   libbambu_networking_<base>-lan.so    the same file, listed as "<base>-lan" in
#                                        Preferences > Network plug-in version
# and selected in OrcaSlicer.conf (app.network_plugin_version = <version>,
# app.installed_networking = true; --no-config to skip).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Release tarball: libraries sit next to this script. Source tree: dist/.
if [[ -f $HERE/libbambu_networking.so ]]; then LIB_DIR="$HERE"; else LIB_DIR="$HERE/../dist"; fi
LIB_DIR="${BBL_PLUGIN_LIB_DIR:-$LIB_DIR}"

SLICER=studio
DATA_DIR="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
FLATPAK_APP=com.bambulab.BambuStudio
MODE=install
SET_CONFIG=1
TARGETS=0
for arg in "$@"; do
  case "$arg" in --native|--orca|--orca-flatpak) TARGETS=$((TARGETS + 1)) ;; esac
done
(( TARGETS <= 1 )) || { echo "choose one of --native, --orca, --orca-flatpak" >&2; exit 2; }
for arg in "$@"; do
  case "$arg" in
    --native) DATA_DIR="$HOME/.config/BambuStudio"; FLATPAK_APP= ;;
    --orca) SLICER=orca; DATA_DIR="$HOME/.config/OrcaSlicer"; FLATPAK_APP= ;;
    --orca-flatpak)
      SLICER=orca; FLATPAK_APP=io.github.softfever.OrcaSlicer
      DATA_DIR="$HOME/.var/app/$FLATPAK_APP/config/OrcaSlicer" ;;
    --uninstall) MODE=uninstall ;;
    --no-config) SET_CONFIG=0 ;;
    -h|--help) sed -n "2,17p" "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done
PLUGINS="$DATA_DIR/plugins"
ORIG="$PLUGINS/x86_64-original"

# ELF e_machine 62 = x86-64 (no dependency on file(1)).
is_x86_64() { [[ "$(od -An -j18 -N2 -tu2 "$1" 2>/dev/null | tr -d ' ')" == 62 ]]; }

running() {
  if [[ $SLICER == orca ]]; then
    pgrep -x 'orca-slicer|\.orca-slicer-w.*|OrcaSlicer|orca_slicer' >/dev/null 2>&1 ||
      flatpak ps --columns=application 2>/dev/null | grep -q io.github.softfever.OrcaSlicer
  else
    pgrep -x 'bambu-studio|\.bambu-studio-.*|BambuStudio|bambu_studio' >/dev/null 2>&1 ||
      flatpak ps --columns=application 2>/dev/null | grep -q com.bambulab.BambuStudio
  fi
}
if running; then
  echo "The slicer is running; close it first." >&2
  exit 1
fi

# OrcaSlicer: Bambu plugin base version Orca lists ours under, and the version
# our plugin reports.
orca_base() {
  if [[ -f $LIB_DIR/COMPAT ]]; then sed -n 's/^orca_plugin_base=//p' "$LIB_DIR/COMPAT"
  else sed -n 's/^plugin_base=//p' "$HERE/../third_party/orca/UPSTREAM"; fi
}
orca_version() {
  if [[ -f $LIB_DIR/COMPAT ]]; then sed -n 's/^orca_plugin_version=//p' "$LIB_DIR/COMPAT"
  else sed -n 's/#define BBL_ORCA_PLUGIN_VERSION "\([^"]*\)".*/\1/p' "$HERE/../src/networking/version.hpp"; fi
}

# Sets or restores app.network_plugin_version / app.installed_networking in
# OrcaSlicer.conf (JSON). The previous values are kept in $ORIG/orca-config.json.
orca_config() {  # <new value> | --restore
  local conf="$DATA_DIR/OrcaSlicer.conf" saved="$ORIG/orca-config.json"
  [[ $SET_CONFIG == 1 && -f $conf ]] || return 0
  command -v python3 >/dev/null || { echo "python3 missing: select '$1' in Preferences instead"; return 0; }
  python3 -I - "$conf" "$saved" "$1" <<'PY'
import json, os, sys
conf, saved, value = sys.argv[1:]
with open(conf) as f:
    data = json.load(f)
app = data.setdefault("app", {})
keys = ("network_plugin_version", "installed_networking")
if value == "--restore":
    if not os.path.exists(saved):
        sys.exit(0)
    old = json.load(open(saved))
    for k in keys:
        if k in old:
            app[k] = old[k]
        else:
            app.pop(k, None)
    os.remove(saved)
    value = app.get("network_plugin_version", "")
else:
    if not os.path.exists(saved):
        os.makedirs(os.path.dirname(saved), exist_ok=True)
        json.dump({k: app[k] for k in keys if k in app}, open(saved, "w"))
    app["network_plugin_version"] = value
    app["installed_networking"] = True
tmp = conf + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f, indent="\t")
os.replace(tmp, conf)
print(f"OrcaSlicer.conf: network_plugin_version = {value!r}")
PY
}

# Our OrcaSlicer files, also from older releases (other base/version).
remove_orca_files() {
  local dir
  for dir in "" backup/; do
    rm -f "$PLUGINS/$dir"libbambu_networking_*-lan.so "$PLUGINS/$dir"libbambu_networking_*.99.so
  done
}

if [[ $MODE == uninstall ]]; then
  if [[ $SLICER == orca ]]; then
    remove_orca_files
    for dir in "" backup/; do  # our camera plugin (originals come back below)
      f="$PLUGINS/${dir}libBambuSource.so"
      if [[ -f $f ]] && ! is_x86_64 "$f"; then rm -f "$f"; fi
    done
    orca_config --restore
  fi
  [[ -d $ORIG ]] || { echo "no original files to restore ($ORIG missing)"; exit 0; }
  for dir in "" backup/; do
    [[ -d $ORIG/$dir ]] || continue
    mkdir -p "$PLUGINS/$dir"
    cp -a "$ORIG/$dir"*.so "$PLUGINS/$dir" 2>/dev/null || true
  done
  echo "Restored original plugin files from $ORIG"
  exit 0
fi

if [[ $SLICER == orca ]]; then
  BASE="$(orca_base)"; ORCA_VERSION="$(orca_version)"
  [[ -n $BASE && -n $ORCA_VERSION ]] || { echo "cannot read the OrcaSlicer plugin versions" >&2; exit 1; }
  NET_SRC=libbambu_networking_orca.so
  NET_DST=("libbambu_networking_${ORCA_VERSION}.so" "libbambu_networking_${BASE}-lan.so")
else
  NET_SRC=libbambu_networking.so
  NET_DST=(libbambu_networking.so)
fi
for lib in "$NET_SRC" libBambuSource.so; do
  [[ -f $LIB_DIR/$lib ]] || { echo "missing $LIB_DIR/$lib (run scripts/build.sh)" >&2; exit 1; }
done
mkdir -p "$PLUGINS"

# Keep Bambu's x86-64 files once (first install only), for --uninstall. They
# cannot load on aarch64, and Studio would pick them over ours.
for dir in "" backup/; do
  [[ -d $PLUGINS/$dir ]] || continue
  for src in "$PLUGINS/$dir"*.so; do
    [[ -f $src ]] || continue
    if is_x86_64 "$src"; then
      mkdir -p "$ORIG/$dir"
      [[ -f $ORIG/$dir${src##*/} ]] || cp -a "$src" "$ORIG/$dir${src##*/}"
      rm -f "$src"
    fi
  done
done

[[ $SLICER == orca ]] && remove_orca_files

# The slicer falls back to plugins/backup/ when the main copy fails to load,
# so install there too.
for dir in "" backup/; do
  mkdir -p "$PLUGINS/$dir"
  for dst in "${NET_DST[@]}"; do install -m 0644 "$LIB_DIR/$NET_SRC" "$PLUGINS/$dir$dst"; done
  install -m 0644 "$LIB_DIR/libBambuSource.so" "$PLUGINS/${dir}libBambuSource.so"
done
[[ $SLICER == orca ]] && orca_config "$ORCA_VERSION"

echo "Installed into $PLUGINS:"
for f in "$PLUGINS"/*.so; do echo "  ${f##*/}"; done
echo "Plugin log: bambu_lan_plugin.log in the slicer's log directory ($DATA_DIR/log)"
if [[ $SLICER == orca ]]; then
  echo "OrcaSlicer: restart it after installing; Preferences shows the plugin as '${BASE}-lan'."
  echo "Do not pick another network plug-in version or click 'download': that fetches Bambu's x86-64 build."
fi
