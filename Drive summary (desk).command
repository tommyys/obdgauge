#!/bin/bash
# Double-click launcher: open the drive summary page. No car needed.
# Same server as "Gauge — REPLAY (desk)", but it opens /debrief.html instead
# of the gauge, so reading a drive is one click.
# resolve symlinks so a Desktop alias still finds the project
SRC="${BASH_SOURCE[0]}"
while [ -h "$SRC" ]; do
  DIR="$(cd -P "$(dirname "$SRC")" && pwd)"
  SRC="$(readlink "$SRC")"
  case "$SRC" in /*) ;; *) SRC="$DIR/$SRC" ;; esac
done
cd "$(cd -P "$(dirname "$SRC")" && pwd)" || exit 1

URL="http://127.0.0.1:8420/debrief.html"

clear 2>/dev/null || true
cat <<'BANNER'
  ┌──────────────────────────────────────────────┐
  │   OBD GAUGE  ·  DRIVE SUMMARY (desk)         │
  └──────────────────────────────────────────────┘

  Opens the drive summary in the browser. Pick a
  drive from the list at the top of the page.

  Keep this window open while you read it.

BANNER

if [ ! -x ".venv/bin/python" ]; then
  echo "  !! Virtualenv missing. Run this once in Terminal:"
  echo "     cd \"$(pwd)\" && python3 -m venv .venv && .venv/bin/pip install bleak"
  echo
  read -n 1 -r -p "  Press any key to close..."
  exit 1
fi

# Free the port if an earlier run is still alive (see the REPLAY launcher).
PORT_PIDS="$(lsof -ti tcp:8420 2>/dev/null)"
if [ -n "$PORT_PIDS" ]; then
  echo "  (stopping a previous run still holding port 8420)"
  echo "$PORT_PIDS" | xargs kill 2>/dev/null
  sleep 1
fi

# Open the summary once the server answers, not before.
(
  for _ in $(seq 1 30); do
    if curl -s -o /dev/null "$URL"; then open "$URL"; exit 0; fi
    sleep 1
  done
  echo "  !! the server did not start in 30 s -- see the error above"
) &

./.venv/bin/python run.py --replay --speed 8 --model "MX-5" --no-browser

echo
echo "  ── stopped ─────────────────────────────────"
read -n 1 -r -p "  Press any key to close this window..."
