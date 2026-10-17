#!/usr/bin/env bash
# omarchy/Arch-native DualSense mode switch.
#
#   ds5-mic-setup.sh on      pad always connects on hid-generic: zero-ceremony
#                            mic, no phantom-input window. Steam Input and SDL
#                            still see the pad (they read hidraw directly);
#                            only native-evdev games lose the kernel gamepad.
#   ds5-mic-setup.sh off     restore normal hid-playstation gamepad mode
#   ds5-mic-setup.sh toggle  flip between the two (for the waybar widget)
#   ds5-mic-setup.sh widget  waybar status JSON
#   ds5-mic-setup.sh notify  user-side mako notification of the current mode
#   ds5-mic-setup.sh status  human-readable state
#
# After switching, reconnect the pad (press PS) for it to take effect.
# When installed to /usr/local/bin (root-owned) with the matching sudoers
# rule, the waybar widget can toggle it passwordlessly.
set -euo pipefail

CONF=/etc/modprobe.d/steaminputrev-ds5mic.conf

mic_mode() { [ -f "$CONF" ]; }

pad_state() {   # prints "driver-name" when connected, nothing otherwise
  for h in /sys/class/hidraw/hidraw*; do
    grep -q 054C "$h/device/uevent" 2>/dev/null || continue
    drv=$(readlink "$h/device/driver" 2>/dev/null || true)
    echo "${drv##*/}"
    return
  done
}

need_root() {
  if [ "$(id -u)" -ne 0 ]; then exec sudo "$0" "$@"; fi
}

switch_on() {
  need_root on
  printf 'blacklist hid_playstation\n' > "$CONF"
  # drop the live playstation binding so the module can unload; the pad
  # reconnects on hid-generic at the next PS press
  for h in /sys/class/hidraw/hidraw*; do
    grep -q 054C "$h/device/uevent" 2>/dev/null || continue
    n=$(basename "$(readlink -f "$h/device")")
    echo "$n" > /sys/bus/hid/drivers/playstation/unbind 2>/dev/null || true
  done
  modprobe -r hid_playstation 2>/dev/null || true
}

switch_off() {
  need_root off
  rm -f "$CONF"
  modprobe hid_playstation 2>/dev/null || true
}

case "${1:-}" in
  on)     switch_on;  echo "mic mode on. Press PS on the pad to reconnect." ;;
  off)    switch_off; echo "gamepad mode on. Press PS on the pad to reconnect." ;;
  toggle) if mic_mode; then switch_off; else switch_on; fi ;;
  status)
    if mic_mode; then echo "mode     : mic (hid-generic)"; else echo "mode     : gamepad (hid-playstation)"; fi
    lsmod | grep -q hid_playstation && echo "module   : loaded" || echo "module   : not loaded"
    echo "pad      : $(pad_state || echo disconnected)"
    ;;
  widget)
    drv=$(pad_state || true)
    if mic_mode; then
      cls=mic; icon="🎤"; mode="DualSense mic mode"
      [ -n "$drv" ] && icon="🎤"
    else
      cls=gamepad; icon="🎮"; mode="DualSense gamepad mode"
    fi
    tip="$mode\\nmodule: $(lsmod | grep -q hid_playstation && echo loaded || echo unloaded) · pad: ${drv:-disconnected}"
    tip="$tip\\nclick: toggle · after switching, press PS to reconnect"
    if mic_mode && [ -z "$drv" ]; then tip="$tip\\n(start ds5-mic for the microphone)"; fi
    printf '{"text":"%s","class":"%s","tooltip":"%s"}\n' "$icon" "$cls" "$tip"
    ;;
  notify)
    if mic_mode; then
      notify-send -u normal "DualSense: mic mode" \
        "Pad reconnects on hid-generic — press PS.\\nRun ds5-mic to stream the microphone."
    else
      notify-send -u normal "DualSense: gamepad mode" \
        "hid-playstation restored — press PS to reconnect normally."
    fi
    ;;
  *)
    echo "usage: $0 on|off|toggle|status|widget|notify" >&2
    exit 2
    ;;
esac
