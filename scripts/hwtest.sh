#!/usr/bin/env bash
# Hardware acceptance test: with a DualSense connected (USB or the machine's
# Bluetooth), this answers the project's open question — does the standalone
# engine acquire the device and deliver action data?
#
# Run with Steam STOPPED for the standalone verdict:
#   steam -shutdown   # graceful, or quit from the tray
#   scripts/hwtest.sh
set -uo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
bin="${STEAMINPUT_HARNESS:-$here/steaminput-harness}"
[ -x "$bin" ] || { echo "harness not built: run 'make' in $here" >&2; exit 2; }

fail=0

echo "== udev rules"
if ls /usr/lib/udev/rules.d/*steam*input* >/dev/null 2>&1; then
  echo "   OK: steam-devices rules installed"
else
  echo "   MISSING: install Valve's steam-devices rules or controller HID access"
  echo "   will be denied for the engine. See README #udev."
  fail=1
fi

echo "== kernel sees a controller?"
if grep -qiE 'vendor=054c' /proc/bus/input/devices 2>/dev/null; then
  echo "   OK: Sony device present in /proc/bus/input/devices"
else
  echo "   WARN: no Sony (054c) device visible — connect/pair the DualSense first"
fi

echo "== engine enumeration"
timeout 60 "$bin" --list && rc=0 || rc=$?
if [ $rc -eq 0 ]; then
  echo "   OK: engine enumerated the device"
elif [ $rc -eq 3 ]; then
  echo "   FAIL: engine sees 0 controllers (permissions? udev? Steam stopped?)"
  exit 3
else
  echo "   FAIL: enumeration error rc=$rc"
  exit $rc
fi

echo "== action data (press buttons/move sticks for 15s)"
timeout 30 "$bin" --manifest "$here/manifests/dualsense_probe.vdf" --poll 15
rc=$?
if [ $rc -eq 0 ]; then
  echo "   OK: monitor completed"
else
  echo "   monitor rc=$rc (manifest/game-mapping path — see README #status)"
  fail=1
fi

exit $fail
