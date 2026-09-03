// Where the header's clock reading comes from.
//
// This board has no real-time clock. The I2C scan names 0x51 as a PCF85063,
// but 0x51 is not on the bus -- the part is not fitted (checked 2026-08-31).
// So the only clock the gauge has is one it was given, kept in NVS.
//
// It gives itself one now: WiFi at boot, before the display even starts
// (SPEC.md s16, gauge_platform/wifi_time.h). Measured 3.6 s on the phone
// hotspot. Failing that, `TIME <epoch>` from the Mac still works, and the
// header shows "--:--" rather than a plausible lie.
//
// There used to be a Clock view here as well -- five wheels, set by thumb in
// the car, the way a phone's alarm does it. It was removed on 2026-09-03 once
// the WiFi sync proved fast enough to make it dead weight. `git log` has it if
// setting the clock by hand is ever wanted back; note that without it a boot
// with no reachable network has no in-car way to set the clock at all.
#pragma once
#include <cstdint>

namespace gauge_ui {

// Where the time comes from. The header knows nothing about NVS or the
// recorder -- gauge_ui may not depend on main. Installed the same way as
// DrivesSource.
struct ClockSource {
    // The wall clock now, or 0 if nobody has ever set it.
    uint32_t (*now)();
};

void clock_set_source(const ClockSource* src);

// The wall clock now, or 0 if nobody has set one. ui.cpp draws it into the
// header on every view.
uint32_t clock_now();

}  // namespace gauge_ui
