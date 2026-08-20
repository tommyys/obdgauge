// Ignition on/off detection from the sample stream.
// Ported from mx5gauge/ignition.py. On the board this drives deep-sleep;
// in the simulator it rotates the recording.
//
// Stopping shows up as every PID going silent while `volts` keeps arriving:
// the adapter is powered from the OBD port, which stays live with the
// ignition off. The value is the tell - 12.4-12.6 V parked against
// 13.9-14.0 V running. That gap is the alternator.
//
// Starting shows up as `run_time` going backwards. Any decrease is a start.
//
// Silence with no `volts` either is a dropped BLE link, not an ignition
// event: the off-edge demands positive evidence rather than merely noticing
// the quiet, or one dropout would chop a drive into a file per gap.
#pragma once
#include <optional>
#include <string>

namespace gauge {

// No non-`volts` sample for this long is the engine being off, not a stalled
// poll: roughly 100 missed fast-PID replies.
inline constexpr double kOffSilenceS = 8.0;
// Below this the alternator is not turning. Midway between the 12.5 V a
// parked battery rests at and the 13.9 V a running one sits at.
inline constexpr double kAlternatorV = 13.0;

enum class IgnitionEvent { None, On, Off };

class Ignition {
  public:
    // Feed one reading. Returns the transition, or None the rest of the time,
    // so a caller can act on the edge without tracking state of its own.
    IgnitionEvent update(double t, const std::string& key, double value);

    bool off() const { return off_; }

  private:
    IgnitionEvent check_off(double t);

    bool off_ = false;
    std::optional<double> last_pid_t_;   // last non-`volts` reading, and when
    std::optional<double> volts_;        // last battery reading
    std::optional<double> run_time_;     // last seconds-since-start
};

}  // namespace gauge
