// The learned mounting angle, kept across key cycles.
//
// gauge_core/gforce.cpp works out which way the gauge points from the drive
// itself, and that takes a few minutes of accelerating and braking. A bracket
// does not move between drives, so that cost should be paid once rather than
// every time the key is turned -- otherwise the g view spends the first six
// minutes of every drive saying LEARNING.
//
// Learning continues from the restored answer rather than stopping at it, so
// a mount that really was moved corrects itself instead of being trusted for
// ever. Restoring seeds the evidence as well as the direction, so one bumpy
// minute cannot outvote a whole drive that already agreed.
#pragma once
#include "gforce.h"

// Reads NVS into `g`. Does nothing when nothing was ever saved, or when what
// was saved does not look like a pair of unit vectors.
void mount_cache_load(gauge::GForce& g);

// Writes `g`'s axes to NVS, if it has solved them. Cheap enough to call at
// the end of a drive; must not be called every frame -- NVS is flash.
void mount_cache_save(const gauge::GForce& g);
