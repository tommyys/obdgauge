// The live car, on its own task.
//
// Everything below this header is the same code the Mac runs (gauge_core's
// Elm327/poll) over the same seam (gauge::ITransport); the only board-specific
// part is which transport it gets. The task exists because connecting takes
// seconds and polling blocks on the car: neither can happen inside the UI loop
// without the gauge freezing, so readings cross over as small POD samples
// through a queue and the UI loop never blocks on the radio.
#pragma once
#include <set>
#include <string>

namespace live {

struct Sample {
    char   key[16];
    float  value;
    double t_s;     // seconds since boot, the timeline trip/score are fed
};

// Starts the connect-and-poll task. Returns immediately; the task retries with
// backoff forever, so a gauge powered up before the adapter wakes still lands.
void start(const char* name_hint);

// True once the car has answered its supported-PID sweep -- i.e. there is a
// real car on the other end and the views can be switched over to it.
bool ready();

// Drains one queued reading. False when there is nothing waiting.
bool next(Sample* out);

// Channel keys the car supports, valid once ready(). Includes "volts", which
// no PID advertises because ATRV is an adapter command rather than a PID.
const std::set<std::string>& keys();

// The car's VIN, "" if it would not say.
const std::string& vin();

// One line for the serial log and, later, the status view.
const char* status();

// Where the link has got to, in the two or three words a banner can hold.
// status() is the sentence; this is the state behind it.
enum class Phase { Idle, Scanning, Connecting, Waking, Live, Lost };
Phase phase();
const char* phase_text();

}  // namespace live
