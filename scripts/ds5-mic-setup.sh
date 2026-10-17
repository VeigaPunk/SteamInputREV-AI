#!/usr/bin/env bash
# omarchy/Arch-native mic mode for the DualSense.
#
#   ds5-mic-setup.sh on    pad always connects on hid-generic: zero-ceremony
#                          mic, no phantom-input window. Steam Input and SDL
#                          still see the pad (they read hidraw directly);
#                          only native-evdev games lose the kernel gamepad.
#   ds5-mic-setup.sh off   restore normal hid-playstation gamepad mode
#   ds5-mic-setup.sh status
#
# After switching, reconnect the pad (press PS) for it to take effect.
set -euo pipefail

CONF=/etc/modprobe.d/steaminputrev-ds5mic.conf

need_root() {
  if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
  fi
}

status() {
  if lsmod | grep -q hid_playstation; then
    echo "module   : loaded (gamepad mode)"
  else
    echo "module   : not loaded"
  fi
  [ -f "$CONF" ] && echo "config   : blacklist present (mic mode)" \
                 || echo "config   : no blacklist (gamepad mode)"
  for h in /sys/class/hidraw/hidraw*; do
    grep -q 054C "$h/device/uevent" 2>/dev/null || continue
    drv=$(readlink "$h/device/driver" 2>/dev/null || true)
    echo "pad      : $(basename "$h") driver=${drv##*/}"
  done
}

case "${1:-}" in
  on)
    need_root "$@"
    printf 'blacklist hid_playstation\n' > "$CONF"
    modprobe -r hid_playstation 2>/dev/null || true
    echo "mic mode on. Press PS on the pad to reconnect it on hid-generic."
    ;;
  off)
    need_root "$@"
    rm -f "$CONF"
    modprobe hid_playstation 2>/dev/null || true
    echo "gamepad mode on. Press PS on the pad to reconnect normally."
    ;;
  status) status ;;
  *)
    echo "usage: $0 on|off|status" >&2
    exit 2
    ;;
esac
