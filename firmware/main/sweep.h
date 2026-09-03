// A bench sweep of the dial: rpm driven from a triangle wave instead of from
// the car or the replay.
//
// Exists because the tacho cannot be checked by reading a log. The heat band,
// the shutter that uncovers it, the redline and the needle all have to line up
// at every point of a 270-degree arc, and the only way to see that is to walk
// the needle from one end to the other while somebody watches the glass.
#pragma once

// Run a sweep for `seconds`, from `lo` to `hi` rpm and back, repeating.
void sweep_start(double seconds, double lo, double hi);

// The rpm to draw right now. False when no sweep is running, in which case the
// car or the replay has the dial as usual.
bool sweep_rpm(double* out);

// The same triangle, for the trip view's economy ring: it colours off a
// number that takes a real drive to move, so there is otherwise no way to see
// the ramp between red and green without driving to each end of it.
// The bench replay. The gauge no longer plays a recorded drive at power-up --
// a gauge showing a drive that is not happening is a gauge lying -- so this is
// how a desk demo is asked for. Latches: once asked, the replay runs until the
// car takes over or the board is restarted.
void demo_request(void);
bool demo_wanted(void);

void sweep_econ_start(double seconds, double lo, double hi);
bool sweep_econ(double* out);

// The same triangle again, for the engine view's coolant. Same reason as the
// economy ring: the scale runs 40-120 C and a real engine takes a drive to
// cross it, so the only way to see the whole blue-green-red ramp and the
// segments filling is to walk the temperature end to end on the bench.
void sweep_temp_start(double seconds, double lo, double hi);
bool sweep_temp(double* out);

// And once more for the power dial's kW. Power is derived from rpm, load and
// air mass, so a sweep of rpm alone does not walk it; the only other way to
// see the whole blue-cyan-white ramp is to use all of the engine on a road.
void sweep_kw_start(double seconds, double lo, double hi);
bool sweep_kw(double* out);

// Cancels all four at once.
//
// The button's demo mode starts every sweep for half an hour, and a sweep
// OVERRIDES whatever the dial was being fed -- so a gauge left in demo and then
// plugged into the car would spend that half hour lying about the engine. The
// app loop calls this the moment the real car takes the views over, which is
// the rule the replay already follows.
void sweep_stop_all(void);
