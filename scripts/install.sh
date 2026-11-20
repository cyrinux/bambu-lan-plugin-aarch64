#!/usr/bin/env bash
# Installs the LAN plugins into Bambu Studio's plugin folder.
#
#   scripts/install.sh               install into the flatpak data dir
#   scripts/install.sh --native      install into ~/.config/BambuStudio (non-flatpak build)
#   scripts/install.sh --uninstall   restore the original (x86-64) files
#
# Original files are moved to plugins/x86_64-original/ (never deleted).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Release tarball: libraries sit next to this script. Source tree: dist/.
if [[ -f $HERE/libbambu_networking.so ]]; then LIB_DIR="$HERE"; else LIB_DIR="$HERE/../dist"; fi
LIB_DIR="${BBL_PLUGIN_LIB_DIR:-$LIB_DIR}"
DATA_DIR="$HOME/.var/app/com.bambulab.BambuStudio/config/BambuStudio"
MODE=install
for arg in "$@"; do
  case "$arg" in
    --native) DATA_DIR="$HOME/.config/BambuStudio" ;;
    --uninstall) MODE=uninstall ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done
PLUGINS="$DATA_DIR/plugins"
ORIG="$PLUGINS/x86_64-original"
LIBS=(libbambu_networking.so libBambuSource.so)
OTHER=(liblive555.so libagora_rtc_sdk.so libagora-fdkaac.so)

if flatpak ps --columns=application 2>/dev/null | grep -q com.bambulab.BambuStudio; then
  echo "Bambu Studio is running; close it first." >&2
  exit 1
fi

if [[ $MODE == uninstall ]]; then
  [[ -d $ORIG ]] || { echo "nothing to restore ($ORIG missing)" >&2; exit 1; }
  for dir in "" backup/; do
    [[ -d $ORIG/$dir ]] || continue
    mkdir -p "$PLUGINS/$dir"
    cp -a "$ORIG/$dir"*.so "$PLUGINS/$dir"
  done
  echo "Restored original plugin files from $ORIG"
  exit 0
fi

for lib in "${LIBS[@]}"; do
  [[ -f $LIB_DIR/$lib ]] || { echo "missing $LIB_DIR/$lib (run scripts/build.sh)" >&2; exit 1; }
done
mkdir -p "$PLUGINS"

# Keep the vendor files once (first install only), for --uninstall.
for dir in "" backup/; do
  [[ -d $PLUGINS/$dir ]] || continue
  for f in "${LIBS[@]}" "${OTHER[@]}"; do
    src="$PLUGINS/$dir$f"
    [[ -f $src ]] || continue
    if file -L "$src" | grep -q 'x86-64'; then
      mkdir -p "$ORIG/$dir"
      [[ -f $ORIG/$dir$f ]] || cp -a "$src" "$ORIG/$dir$f"
      rm -f "$src"
    fi
  done
done

# The slicer falls back to plugins/backup/ when the main copy fails its
# version check, so install there too.
for dir in "" backup/; do
  mkdir -p "$PLUGINS/$dir"
  install -m 0644 "${LIBS[@]/#/$LIB_DIR/}" "$PLUGINS/$dir"
done

echo "Installed into $PLUGINS:"
file "$PLUGINS"/*.so | sed 's/^/  /'
echo "Plugin log: bambu_lan_plugin.log in Studio's log directory ($DATA_DIR/log)"
