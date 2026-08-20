#include "ignition.h"

namespace gauge {

IgnitionEvent Ignition::update(double t, const std::string& key, double value) {
    if (key == "volts") {
        volts_ = value;
        return check_off(t);
    }

    // A run_time that went backwards is a fresh engine start. Checked before
    // the resumption edge below so it still fires when we never saw the
    // engine stop - the adapter was down across it, and the PIDs were flowing
    // again before we knew anything had changed.
    bool restarted = (key == "run_time" && run_time_ && value < *run_time_);
    if (key == "run_time") run_time_ = value;

    bool was_off = off_;
    last_pid_t_ = t;
    off_ = false;
    if (restarted || was_off) {
        // Drop the run_time baseline on the way out. The two on-edges see the
        // same restart moments apart - the PIDs answer immediately, then
        // run_time turns up carrying its reset - and firing twice would
        // rotate the recording twice, orphaning a file seconds old.
        run_time_.reset();
        return IgnitionEvent::On;
    }
    return IgnitionEvent::None;
}

// A `volts` reading is the only chance to notice the engine stopped - while
// it is stopped, nothing else is arriving to ask the question.
IgnitionEvent Ignition::check_off(double t) {
    if (off_ || !last_pid_t_) return IgnitionEvent::None;
    if (t - *last_pid_t_ < kOffSilenceS) return IgnitionEvent::None;
    if (!volts_ || *volts_ >= kAlternatorV) return IgnitionEvent::None;
    off_ = true;
    return IgnitionEvent::Off;
}

}  // namespace gauge
