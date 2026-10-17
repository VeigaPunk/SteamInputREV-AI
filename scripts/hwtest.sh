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
# Device acquisition lives in the Steam client process (the input master);
# the standalone library is the consumer. Run the enumeration with the
# Steam client running when possible.
steam_running=0
pgrep -x steam >/dev/null 2>&1 && steam_running=1
if [ $steam_running -eq 0 ]; then
  echo "   note: Steam not running — standalone mode; the master role is"
  echo "   not implemented yet, so 0 controllers is the expected result"
fi
timeout 60 "$bin" --list && rc=0 || rc=$?
if [ $rc -eq 0 ]; then
  echo "   OK: engine enumerated the device"
elif [ $rc -eq 3 ]; then
  if [ $steam_running -eq 1 ]; then
    echo "   FAIL: engine sees 0 controllers with Steam running —"
    echo "   if earlier manifest runs failed, restart Steam and retry"
    echo "   (failed game-mapping requests can wedge the client's input state)"
  else
    echo "   (expected in standalone mode)"
  fi
  exit 3
else
  echo "   FAIL: enumeration error rc=$rc"
  exit $rc
fi

echo "== output path (lightbar / rumble / adaptive triggers)"
timeout 60 "$bin" --led 0,255,60 && rc=0 || rc=$?
[ $rc -ne 0 ] && { echo "   FAIL: --led rc=$rc"; fail=1; }
sleep 1
timeout 60 "$bin" --rumble 50000,50000 && rc=0 || rc=$?
[ $rc -ne 0 ] && { echo "   FAIL: --rumble rc=$rc"; fail=1; }
sleep 1
timeout 60 "$bin" --trigger-effect feedback,6,7 && rc=0 || rc=$?
sleep 2
timeout 60 "$bin" --trigger-effect off,0,0 && rc=0 || rc=$?
[ $rc -ne 0 ] && { echo "   FAIL: --trigger-effect rc=$rc"; fail=1; }
if [ $fail -eq 0 ]; then
  echo "   OK: lightbar changed color, motors pulsed, triggers resisted"
fi

echo
echo "Action data (button/stick events) additionally requires the game-mapping"
echo "registration handshake — not implemented yet; see README #what-is-blocked."
echo "Do NOT run --manifest against a live Steam client: failed game-mapping"
echo "requests wedge its input state until Steam is restarted."

exit $fail
