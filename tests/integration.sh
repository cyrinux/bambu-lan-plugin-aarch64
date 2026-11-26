#!/usr/bin/env bash
# End-to-end test of the built plugins against tests/fake_printer.py.
#
#   BBL_TEST_TARGET=studio|orca   which networking plugin to drive (default studio)
#   BBL_TEST_RUNNER=flatpak       run the probe inside a flatpak runtime (default):
#                                 Bambu Studio's when installed, else BBL_TEST_APP / the SDK
#   BBL_TEST_RUNNER=native        compile the probe with the host compiler, linked to the
#                                 host's OpenSSL/libcurl, and run it there (e.g. a NixOS
#                                 OrcaSlicer: run under
#                                 `nix-shell -p gcc pkg-config openssl curl --run ...`)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/bbl-lan-it.XXXXXX")"
SERIAL=03900A000000001
CODE=12345678
trap 'kill "${FAKE_PID:-0}" 2>/dev/null || true; rm -rf "$WORK"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=$SERIAL" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" 2>/dev/null
python3 -I "$ROOT/tests/fake_printer.py" --cert "$WORK/cert.pem" --key "$WORK/key.pem" \
  --code "$CODE" --serial "$SERIAL" --h264 "$ROOT/tests/fixtures/camera_320x180.h264" --rtsp-port 8322 \
  >"$WORK/fake.log" 2>&1 &
FAKE_PID=$!
sleep 1

TARGET="${BBL_TEST_TARGET:-studio}"
case "$TARGET" in
  studio) PROBE="$ROOT/build/bbl_lan_probe" ;;
  orca) PROBE="$ROOT/build/bbl_lan_probe_orca" ;;
  *) echo "BBL_TEST_TARGET must be studio or orca" >&2; exit 2 ;;
esac

if [[ ${BBL_TEST_RUNNER:-flatpak} == native ]]; then
  # The slicer process already has OpenSSL and libcurl loaded when it dlopen()s
  # the plugin; linking the probe to them reproduces that.
  defs=(); [[ $TARGET == orca ]] && defs=(-DBBL_TARGET_ORCA -I"$ROOT/third_party/orca")
  libs="$(pkg-config --libs openssl libcurl)"
  "${CXX:-g++}" -std=c++17 -O1 "${defs[@]}" -I"$ROOT/third_party/bambu" -I"$ROOT/third_party" \
    "$ROOT/tests/probe.cpp" -o "$WORK/probe" -Wl,--no-as-needed \
    $libs -ldl -lpthread
  echo "running native probe ($TARGET): $(ldd "$WORK/probe" | grep -oE '(libc|libstdc\+\+|libssl|libcurl)\.so[^ ]* => [^ ]*' | tr '\n' ' ')"
  probe() { (cd "$WORK" && BBL_ACCESS_CODE="$1" "$WORK/probe" "$ROOT/dist" "${@:2}"); }
else
  # Bambu Studio's runtime when installed (the real target), otherwise the SDK
  # the plugins were built with (CI).
  if [[ -z ${BBL_TEST_APP:-} ]]; then
    if flatpak info com.bambulab.BambuStudio >/dev/null 2>&1; then BBL_TEST_APP=com.bambulab.BambuStudio
    else BBL_TEST_APP=org.freedesktop.Sdk//26.08; fi
  fi
  echo "running probe ($TARGET) in $BBL_TEST_APP"
  probe() {
    flatpak run --share=network --filesystem="$ROOT" --filesystem="$WORK" --command=sh "$BBL_TEST_APP" -c \
      "cd '$WORK' && BBL_ACCESS_CODE='$1' '$PROBE' '$ROOT/dist' ${*:2}"
  }
fi

fail=0
check() { # name, expected-substring, output
  if grep -q -- "$2" <<<"$3"; then echo "PASS $1"; else echo "FAIL $1"; echo "$3" | sed 's/^/    /'; fail=1; fi
}

check detect   "dev_id=$SERIAL model=N2S"            "$(probe x detect 127.0.0.1 || true)"
check connect  "received 1 report"                    "$(probe "$CODE" connect 127.0.0.1 "$SERIAL" || true)"
check bad_code "on_local_connect status=1 dev=$SERIAL msg=5" "$(probe wrong connect 127.0.0.1 "$SERIAL" || true)"
check camera   "stream: sub_type=1 640x480"           "$(probe "$CODE" camera 127.0.0.1 "$SERIAL" "$WORK/frame.jpg" || true)"
check sdprint    "start_sdcard_print rc=0"            "$(probe "$CODE" sdprint 127.0.0.1 "$SERIAL" benchy.gcode.3mf || true)"
# 192.0.2.1 (TEST-NET-1) is unroutable: connects hang until cancelled.
check cancel     "OK"                                  "$(probe x cancel 192.0.2.1 || true)"
check camera_bad "Bambu_StartStream rc=-1"            "$(probe wrong camera 127.0.0.1 "$SERIAL" "$WORK/frame.jpg" || true)"
# X1-style RTSPS camera: Digest auth, H.264 (sub_type 0 = AVC1), SPS from the SDP.
check camera_rtsp     "stream: sub_type=0 320x180 @15fps" "$(probe "$CODE" camera_rtsp 127.0.0.1:8322 "$WORK/frame.h264" || true)"
check camera_rtsp_key "sample 1: [0-9]* bytes sync"        "$(probe "$CODE" camera_rtsp 127.0.0.1:8322 "$WORK/frame.h264" || true)"
check camera_rtsp_bad "access code refused"                "$(probe wrong camera_rtsp 127.0.0.1:8322 "$WORK/frame.h264" || true)"

# Hybrid mode (printer NOT in LAN-only mode): the fake ignores plaintext and
# answers encrypted; the plugin must detect it (auto), encrypt and still get
# its reports. Serial 094... = H2D family (ftp:// print URL).
echo "=== hybrid section starting ==="
HWORK="$(mktemp -d "${TMPDIR:-/tmp}/bbl-lan-it-hy.XXXXXX")"
trap 'kill "${FAKE_PID:-0}" "${HFAKE_PID:-0}" 2>/dev/null || true; rm -rf "$WORK" "$HWORK"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=$SERIAL" \
  -keyout "$HWORK/key.pem" -out "$HWORK/cert.pem" 2>/dev/null
python3 -I "$ROOT/tests/fake_printer.py" --cert "$HWORK/cert.pem" --key "$HWORK/key.pem" \
  --code "$CODE" --serial "$SERIAL" --h264 "$ROOT/tests/fixtures/camera_320x180.h264" --rtsp-port 8323 \
  --hybrid --mqtt-port 18883 --camera-port 16000 >"$HWORK/fake.log" 2>&1 &
HFAKE_PID=$!
sleep 1
hprobe() { (cd "$HWORK" && BBL_ACCESS_CODE="$1" "${@:2}"); }
if [[ ${BBL_TEST_RUNNER:-flatpak} == native ]]; then
  hprobe() { (cd "$HWORK" && BBL_ACCESS_CODE="$1" "$WORK/probe" "$ROOT/dist" "${@:2}"); }
else
  hprobe() {
    flatpak run --share=network --filesystem="$ROOT" --filesystem="$HWORK" --command=sh "$BBL_TEST_APP" -c \
      "cd '$HWORK' && BBL_ACCESS_CODE='$1' '$PROBE' '$ROOT/dist' ${*:2}"
  }
fi
check hybrid_connect "received 1 report" "$(hprobe "$CODE" connect 127.0.0.1:18883 "$SERIAL" || true)"
check hybrid_sdprint "start_sdcard_print rc=0" "$(hprobe "$CODE" sdprint 127.0.0.1:18883 "$SERIAL" benchy.gcode.3mf || true)"
echo "--- hybrid fake printer log"; cat "$HWORK/fake.log"

echo "--- fake printer log"; cat "$WORK/fake.log"
exit $fail
