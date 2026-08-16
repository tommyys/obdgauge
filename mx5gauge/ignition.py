"""Ignition on/off detection from the sample stream.

A drive ends when the engine stops, not when the gauge process does. Two
signals in the stream say so, and the car already reports both.

**Stopping** shows up as every PID going silent while `volts` keeps arriving.
The adapter is powered from the OBD port, which stays live with the ignition
off, so the link survives the engine — on a recorded stop it kept answering
ATRV every ~28.5 s. The value is the tell: 12.4-12.6 V parked against
13.9-14.0 V running. That gap is the alternator.

**Starting** shows up as `run_time` (seconds since engine start) going
backwards. Any decrease is unambiguously a new start.

The distinction that matters most is the one this makes on purpose: silence
with *no* `volts` either is a dropped BLE link or a wedged adapter, which
`sources.py` already handles by reconnecting. Reading that as an ignition
event would chop a single drive into a file per dropout, so the off-edge
demands positive evidence rather than merely noticing the quiet.

Pure Python over a stream of samples — no I/O, no clock of its own — so it is
testable on the host and ports directly to the firmware, same as `metrics`.
"""

# No non-`volts` sample for this long is the engine being off, not a stalled
# poll: it is roughly 100 missed fast-PID replies.
OFF_SILENCE_S = 8.0

# Below this the alternator is not turning. Midway between the 12.5 V a parked
# battery rests at and the 13.9 V a running one sits at.
ALTERNATOR_V = 13.0


class Ignition(object):
    """Tracks whether the engine is running, from the readings alone.

    Feed it every sample. It returns 'off' or 'on' on the transition and None
    the rest of the time, so a caller can act on the edge without tracking
    state of its own.
    """

    def __init__(self):
        self.off = False
        self._last_pid_t = None    # last non-`volts` reading, and when
        self._volts = None         # last battery reading
        self._run_time = None      # last seconds-since-start

    def update(self, t, key, value):
        """Feed one reading. Returns 'off', 'on', or None."""
        if key == 'volts':
            self._volts = value
            return self._check_off(t)

        # A run_time that went backwards is a fresh engine start. Checked
        # before the resumption edge below so it still fires when we never saw
        # the engine stop — the adapter or the gauge was down across the stop,
        # and the PIDs were flowing again before we knew anything had changed.
        restarted = (key == 'run_time' and self._run_time is not None
                     and value < self._run_time)
        if key == 'run_time':
            self._run_time = value

        was_off = self.off
        self._last_pid_t = t
        self.off = False
        if restarted or was_off:
            # Drop the run_time baseline on the way out. The two on-edges see
            # the same restart moments apart — the PIDs answer immediately,
            # then run_time turns up carrying its reset — and firing twice
            # would rotate the recording twice, orphaning a file seconds old.
            # Whichever edge got here first, the other is now disarmed until
            # run_time re-establishes a baseline.
            self._run_time = None
            return 'on'
        # The engine answering at all, after we had called it stopped, is it
        # running again. This fires on the first reply of the new drive, so
        # nothing of the warm-up is lost to a slow-polled confirmation.
        if was_off:
            return 'on'
        return None

    def _check_off(self, t):
        """A `volts` reading is the only chance to notice the engine stopped —
        while it is stopped, nothing else is arriving to ask the question."""
        if self.off or self._last_pid_t is None:
            return None
        if t - self._last_pid_t < OFF_SILENCE_S:
            return None
        if self._volts is None or self._volts >= ALTERNATOR_V:
            return None
        self.off = True
        return 'off'
