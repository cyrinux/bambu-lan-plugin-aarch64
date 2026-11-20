#!/usr/bin/env bash
# End-to-end test of the built plugins against tests/fake_printer.py, run
# inside Bambu Studio's own flatpak runtime (the environment that loads them).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/bbl-lan-it.XXXXXX")"
SERIAL=03900A000000001
CODE=12345678
trap 'kill "${FAKE_PID:-0}" 2>/dev/null || true; rm -rf "$WORK"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=$SERIAL" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" 2>/dev/null
python3 -I "$ROOT/tests/fake_printer.py" --cert "$WORK/cert.pem" --key "$WORK/key.pem" \
  --code "$CODE" --serial "$SERIAL" >"$WORK/fake.log" 2>&1 &
FAKE_PID=$!
sleep 1

# Run the probe in Bambu Studio's runtime when installed (the real target),
# otherwise in the SDK the plugins were built with (CI).
if [[ -z ${BBL_TEST_APP:-} ]]; then
  if flatpak info com.bambulab.BambuStudio >/dev/null 2>&1; then BBL_TEST_APP=com.bambulab.BambuStudio
  else BBL_TEST_APP=org.freedesktop.Sdk//26.08; fi
fi
echo "running probe in $BBL_TEST_APP"
probe() {
  flatpak run --share=network --filesystem="$ROOT" --filesystem="$WORK" --command=sh "$BBL_TEST_APP" -c \
    "cd '$WORK' && BBL_ACCESS_CODE='$1' '$ROOT/build/bbl_lan_probe' '$ROOT/dist' ${*:2}"
}

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

echo "--- fake printer log"; cat "$WORK/fake.log"
exit $fail
