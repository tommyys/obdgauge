#!/bin/sh
# Answers one question: is the C++ core still the same as the simulator?
#
# Three layers, because "the tests pass" is not the same as "the tests would
# notice":
#   1. unit suites          - the ported logic against assertions from tests/
#   2. cross-validation     - every channel and derived value, on real drives
#   3. mutation self-test   - deliberately break the C++ and confirm layer 2
#                             catches it. Without this, "0 divergences" could
#                             just mean the harness is blind.
#
# A mutation is only meaningful if the captures exercise the code it touches.
# Two earlier attempts here were silently unobservable: a coolant ceiling of
# 199 C on drives that peak at 96 C, and an rpm ceiling of 5000 on a drive
# that peaks at 4466. If you add a mutation and it reports NOT CAUGHT, first
# ask whether the data reaches it at all.
#
# Usage: tools/verify_port.sh          (layers 1-2, fast)
#        tools/verify_port.sh --full   (all three; mutates sources, reverts)
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CORE=firmware/components/gauge_core
HOST=firmware/test/host
PY=.venv/bin/python
fail=0

echo "== 1. unit suites =="
( cd "$HOST" && make test >/tmp/vp_units.txt 2>&1 ) || { cat /tmp/vp_units.txt; exit 1; }
grep -c "^all tests passed" /tmp/vp_units.txt | sed 's/^/   suites passing: /'
grep -E "FAILURES" /tmp/vp_units.txt && fail=1 || echo "   no failures"

echo "== 2. cross-validation against the simulator =="
( cd "$HOST" && make build/replay_check >/dev/null 2>&1 )
n=0
for f in logs/*.csv; do
    [ -e "$f" ] || continue
    "$PY" tools/dump_python_states.py "$f" > /tmp/vp_ref.txt
    "$HOST"/build/replay_check "$f" /tmp/vp_ref.txt | sed 's/^/   /' || fail=1
    n=$((n + 1))
done
[ "$n" -gt 0 ] || echo "   WARNING: no captures in logs/ - nothing was cross-checked"

if [ "$1" = "--full" ]; then
    echo "== 3. mutation self-test (would the harness notice?) =="
    if ! git diff --quiet -- "$CORE"; then
        echo "   REFUSING: uncommitted changes in $CORE. Commit or stash first."
        exit 1
    fi
    CAP=$(ls logs/*.csv 2>/dev/null | head -1)
    if [ -z "$CAP" ]; then
        echo "   skipped: no capture to mutate against"
    else
        "$PY" tools/dump_python_states.py "$CAP" > /tmp/vp_ref.txt
        # each line: description | sed expression | file
        cat <<'MUT' > /tmp/vp_mutations
harsh-brake threshold|s/kHarshBrake  = -3.0/kHarshBrake  = -2.0/|metrics.h
fuel price|s/kFuelPriceRm = 2.05/kFuelPriceRm = 2.06/|metrics.h
eco rpm band|s/kEcoRpmLo    = 1200/kEcoRpmLo    = 1210/|metrics.h
smoothness slope|s/rate \* 8.0/rate * 8.001/|metrics.cpp
power_kw pi literal|s/2 \* 3.14159/2 * M_PI/|state.cpp
rpm plausible bound|s/{"rpm", {0.0, 9000.0}}/{"rpm", {0.0, 1000.0}}/|state.cpp
MUT
        while IFS='|' read -r desc expr file; do
            sed -i '' "$expr" "$CORE/$file"
            if ! git diff --quiet -- "$CORE"; then
                if ( cd "$HOST" && make -B build/replay_check >/dev/null 2>&1 ); then
                    if "$HOST"/build/replay_check "$CAP" /tmp/vp_ref.txt >/dev/null 2>&1; then
                        echo "   NOT CAUGHT: $desc  <-- the harness is blind here"
                        fail=1
                    else
                        echo "   caught: $desc"
                    fi
                else
                    echo "   BUILD FAILED for mutation: $desc"
                    fail=1
                fi
            else
                echo "   SKIPPED (pattern no longer matches): $desc"
                fail=1
            fi
            git checkout -- "$CORE"
        done < /tmp/vp_mutations
        ( cd "$HOST" && make build/replay_check >/dev/null 2>&1 )
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PORT VERIFIED"
else
    echo "PORT NOT VERIFIED - see above"
fi
echo
cat <<'NOTE'
Covered: pid decode, ELM327 parsing, poll ordering, VehicleState + plausibility,
Trip, DrivingScore, VIN/profiles, ignition - and, on real drives, every channel
and every derived value the simulator holds.

NOT covered, and cannot be by this harness:
  - the UI. The nine views are not ported yet; the web UI is DOM/CSS and the
    LVGL screens will be new code.
  - vehicle identity end to end. Captures carry no VIN, so identify() is
    covered by unit tests only.
  - the BLE transport. Only the ELM327 conversation above it is tested, against
    a fake. The real link is Phase 0 Task 14.
  - ignition on real drives is covered by a fixture test, not by replay_check
    (the simulator only runs ignition when recording).
  - recorder / library / brc / server, which are deliberately not ported.
  - the plausibility gate. On these captures 'rejected' stays 0 from start to
    finish: no reading is ever implausible, so replay never exercises the gate.
    It is covered by test_state only. A range bound can therefore be wrong
    without any drive noticing.
NOTE
