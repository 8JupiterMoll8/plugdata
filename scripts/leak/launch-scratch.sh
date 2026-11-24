#!/usr/bin/env bash
# Host leak 2026-11-24: start ONE test PlugData on a scratch HOME, memory-capped, frame log on.
#
#   launch-scratch.sh <scratch-dir> <patch.pd> [extra env, e.g. LD_PRELOAD=...]
#
# Refuses when any plugdata runs (pgrep -x) or UDP 9000 is held: the owner's app must be CLOSED (never stop it).
# The app runs in a transient systemd scope with MemoryMax (MEM_MAX, default 3G) and no swap, so a leak kills the
# test app, not the laptop. stderr (the PLUGDATA_NVG_FRAME_LOG lines) goes to <scratch-dir>/app.log.
# Prints the PID. BIN overrides the binary (always a full path: a symlink start cannot find its resources).
set -euo pipefail
scratch="$(realpath -m "$1")"; patch="$(realpath "$2")"; shift 2
bin="${BIN:-/home/alphi/Desktop/plugdata/plugdata-p31-build/Plugins/Standalone/plugdata}"
if pgrep -x plugdata >/dev/null; then
  echo "launch-scratch: a plugdata is running; it must be closed first:" >&2
  pgrep -x plugdata -a >&2
  exit 2
fi
if ss -lun | grep -qE '[:.]9000\b'; then
  echo "launch-scratch: UDP 9000 is held (ss -lunp | grep 9000)" >&2
  exit 2
fi
case "$scratch" in /home/alphi|/home/alphi/) echo "launch-scratch: refusing the real HOME" >&2; exit 2;; esac
mkdir -p "$scratch/home"
[ -f /home/alphi/.Xauthority ] && cp /home/alphi/.Xauthority "$scratch/home/.Xauthority"
cd "$scratch"
env HOME="$scratch/home" XAUTHORITY="$scratch/home/.Xauthority" PLUGDATA_NVG_FRAME_LOG=1 PIPEWIRE_QUANTUM=256/48000 "$@" \
  systemd-run --user --scope --quiet -p MemoryMax="${MEM_MAX:-3G}" -p MemorySwapMax=0 \
  /usr/bin/pw-jack "$bin" "$patch" >"$scratch/app.log" 2>&1 &
for _ in $(seq 1 40); do
  sleep 0.5
  pid="$(pgrep -x plugdata -n || true)"
  if [ -n "$pid" ]; then echo "$pid"; exit 0; fi
done
echo "launch-scratch: plugdata did not start, see $scratch/app.log" >&2
exit 1
