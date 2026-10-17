#!/usr/bin/env bash
# Installs steaminput-harness on omarchy/Arch (or any Linux):
#   1. latest release binary from GitHub if compatible, else
#   2. builds from source with g++
set -euo pipefail

repo="VeigaPunk/SteamInputREV-AI"
here="$(cd "$(dirname "$0")" && pwd)"
dest="${DEST:-$HOME/.local/bin}"
mkdir -p "$dest"

try_release() {
  command -v curl >/dev/null || return 1
  local url arch
  case "$(uname -m)" in
    x86_64) arch=x86_64 ;;
    *) return 1 ;;
  esac
  url="$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" \
    | grep -oE "https://[^\"]+/steaminput-harness-linux-$arch\.tar\.gz" | head -1)" || return 1
  [ -n "$url" ] || return 1
  echo "downloading $url"
  local tmp; tmp="$(mktemp -d)"
  curl -fSL "$url" | tar xz -C "$tmp" || { rm -rf "$tmp"; return 1; }
  install -m755 "$tmp/steaminput-harness" "$dest/steaminput-harness"
  rm -rf "$tmp"
}

try_source() {
  command -v g++ >/dev/null || { echo "g++ not found (pacman -S base-devel)" >&2; return 1; }
  make -C "$here"
  install -m755 "$here/steaminput-harness" "$dest/steaminput-harness"
}

try_release || try_source
echo "installed: $dest/steaminput-harness"

if ! ls /usr/lib/udev/rules.d/*steam*input* >/dev/null 2>&1; then
  cat >&2 <<'EOF'
warning: Valve steam-devices udev rules not found.
Without them the engine cannot open controller HID nodes.
Arch:  sudo pacman -S steam     (ships 60-steam-input.rules)
Other: https://github.com/ValveSoftware/steam-devices
EOF
fi
