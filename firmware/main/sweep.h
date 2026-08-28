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
