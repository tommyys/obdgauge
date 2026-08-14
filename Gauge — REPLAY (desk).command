#!/bin/bash
# Double-click launcher: replay a recorded drive at the desk. No car needed.
# resolve symlinks so a Desktop alias still finds the project
SRC="${BASH_SOURCE[0]}"
while [ -h "$SRC" ]; do
  DIR="$(cd -P "$(dirname "$SRC")" && pwd)"
  SRC="$(readlink "$SRC")"
  case "$SRC" in /*) ;; *) SRC="$DIR/$SRC" ;; esac
done
cd "$(cd -P "$(dirname "$SRC")" && pwd)" || exit 1

clear 2>/dev/null || true
cat <<'BANNER'
  ┌──────────────────────────────────────────────┐
  │   OBD GAUGE  ·  REPLAY (desk)                │
  └──────────────────────────────────────────────┘

  Plays back a real drive through the gauge, in the
  browser. Nothing to plug in, no car required.

  Picks the capture with the most actual driving in it,
  and opens http://127.0.0.1:8420 automatically.

BANNER

if [ ! -x ".venv/bin/python" ]; then
  echo "  !! Virtualenv missing. Run this once in Terminal:"
  echo "     cd \"$(pwd)\" && python3 -m venv .venv && .venv/bin/pip install bleak"
  echo
  read -n 1 -r -p "  Press any key to close..."
  exit 1
fi

# Free the port if an earlier run is still alive. Match on the listening port,
# not the process name: the venv python resolves to the system framework path,
# so the project name never appears in argv.
PORT_PIDS="$(lsof -ti tcp:8420 2>/dev/null)"
if [ -n "$PORT_PIDS" ]; then
  echo "  (stopping a previous run still holding port 8420)"
  echo "$PORT_PIDS" | xargs kill 2>/dev/null
  sleep 1
fi

# --model names the car on the display: a VIN cannot supply a model name, and
# captures carry no VIN at all, so it is passed in here.
# No --sweep: replay shows the revs the drive actually turned. A synthetic
# sweep made the rev-reactive visuals easier to admire, but it also made replay
# useless for reviewing a drive, which is what replay is for. The flag still
# exists (run.py --sweep) for judging the visuals on an idling capture.
./.venv/bin/python run.py --replay --speed 8 --model "MX-5"

echo
echo "  ── stopped ─────────────────────────────────"
echo "  Replay a specific drive instead:"
echo "    run.py --sessions            list recorded drives"
echo "    run.py --replay last         the most recent one"
echo "    run.py --replay \"logs/<file>\""
echo
read -n 1 -r -p "  Press any key to close this window..."
