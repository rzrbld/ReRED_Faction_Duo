#!/bin/sh
# SSH test helper (development only): runs the real launcher for N seconds
# while muOS's frontend UI and its loop are paused (SIGSTOP), then resumes them.
# Usage: dev-run.sh [seconds]   (env RERED_* is passed through)
PORTS_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")      # <card>/ports
CARD=$(dirname "$PORTS_DIR")
LAUNCHER="$CARD/ROMS/Ports/ReRed Faction II.sh"
# Never run on top of someone using the handheld: the launcher or the game
# already running means a person started it from the frontend.
if pgrep -f '[r]ered-launcher' >/dev/null || pgrep -f '[r]ered.aarch64 data' >/dev/null; then
  echo "dev-run: ReRed Faction II is already running on this device; not starting a test" >&2
  exit 75
fi
PAUSED=
for p in $(pgrep -f '^/opt/muos/frontend/muxfrontend') $(pgrep -f '^/bin/sh /opt/muos/script/mux/frontend.sh'); do
  kill -STOP "$p" 2>/dev/null && PAUSED="$PAUSED $p"
done
trap 'for p in $PAUSED; do kill -CONT "$p" 2>/dev/null; done' EXIT
trap 'exit 130' INT TERM HUP
RERED_AUTOSTART=1 RERED_TIMEOUT=${1:-60} bash "$LAUNCHER"
