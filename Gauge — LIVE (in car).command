#!/bin/bash
# Double-click launcher: connect to the vLinker in the car over BLE.
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
  │   MX-5 GAUGE  ·  LIVE                        │
  └──────────────────────────────────────────────┘

  Before continuing:
    1. Engine running
    2. vLinker plugged into the OBD port
    3. Car Scanner FORCE-QUIT on your phone
       (BLE allows only one connection at a time —
        if the phone holds it, the Mac cannot connect)
    4. vLinker NOT paired in System Settings > Bluetooth
       (if it is listed there, click the (i) and "Forget This
        Device" — macOS managing the link is the #1 cause of
        constant disconnects)

  If this window closes instantly with no output, macOS has
  denied Bluetooth access: System Settings > Privacy & Security
  > Bluetooth, and enable Terminal.

BANNER

read -n 1 -r -p "  Press any key to connect (ctrl-c to cancel)..."
echo
echo

if [ ! -x ".venv/bin/python" ]; then
  echo "  !! Virtualenv missing. Run this once in Terminal:"
  echo "     cd \"$(pwd)\" && python3 -m venv .venv && .venv/bin/pip install bleak"
  echo
  read -n 1 -r -p "  Press any key to close..."
  exit 1
fi

# Free the port if an earlier run is still alive.
# Match on the listening port, not the process name: the venv python resolves
# to the system framework path, so the project name never appears in argv.
PORT_PIDS="$(lsof -ti tcp:8420 2>/dev/null)"
if [ -n "$PORT_PIDS" ]; then
  echo "  (stopping a previous run still holding port 8420)"
  echo "$PORT_PIDS" | xargs kill 2>/dev/null
  sleep 1
fi

./.venv/bin/python run.py --live -v

echo
echo "  ── stopped ─────────────────────────────────"
echo "  If it could not find the adapter, check the device list above,"
echo "  then re-run with:  run.py --live -v --name <part-of-the-name>"
echo
read -n 1 -r -p "  Press any key to close this window..."
