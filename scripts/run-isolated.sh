#!/usr/bin/env bash
# Clean-room runner: executes the harness with a private /dev/shm and no
# network, reproducing "Steam stopped, first boot" conditions for the input
# engine while the real Steam client keeps running outside the namespace.
#
# Effects:
#   - fresh empty /dev/shm  -> the engine owns every segment it creates,
#     exactly as it would with no Steam running
#   - no network            -> steamclient's crashhandler cannot upload
#     minidumps to Valve when internal asserts fire
#
# Requires unprivileged user namespaces (default on Arch/omarchy kernels).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
bin="${STEAMINPUT_HARNESS:-$here/steaminput-harness}"
[ -x "$bin" ] || { echo "harness not built: run 'make' in $here" >&2; exit 2; }

if ! unshare -rmn true 2>/dev/null; then
  echo "unprivileged user namespaces unavailable; running without isolation" >&2
  exec "$bin" "$@"
fi

exec unshare -rmn sh -c "
  mount -t tmpfs tmpfs /dev/shm || exit 2
  exec '$bin' '$@'
"
