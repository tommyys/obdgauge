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

// Creates the task and its queue, and leaves the task parked.
//
// Called during boot's early window, and this is load-bearing. The task's stack
// is 6 KB of INTERNAL RAM, and by the time the views are up there is not that
// much left: linking WiFi and lwip costs about 28 KB permanently, which took
// the free internal heap at the old creation point from 30,331 bytes down to
// 3,523. xTaskCreate then failed, its result was not checked, and the gauge ran
// perfectly at 66 fps while never once looking for the car -- no OBD link at
// all, and not one line in the log to say so (2026-09-04).
//
// So the stack is claimed where the BT controller and the blit band claim
// theirs: after WiFi has handed the radio back and before the display starts
// allocating, when the internal heap is still whole (137 KB free). The task
// then sleeps until start() wakes it, because WHEN scanning begins is a
// separate question with its own answer -- see start().
void reserve();

// Wakes the parked task and lets it start scanning. Returns immediately; it
// retries with backoff forever, so a gauge powered up before the adapter wakes
// still lands.
//
// Deliberately later than reserve(): bringing the radio up while LVGL is
// drawing whole screens made the panel fail draws for the first 13 seconds,
// which is a visibly torn gauge. Claiming memory early and scanning late is not
// a contradiction -- they are two different costs at two different moments.
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

}  // namespace live
