// Whether a view has anything to show on THIS car.
// Ported from mx5gauge/web/index.html applyChannels() -- the simulator is
// authoritative.
#pragma once
#include <set>
#include <string>

namespace gauge {

// `needs` is the view's comma-separated channel list ("coolant,volts").
// `supported` is the set of channels the car reports, or nullptr when the car
// has not been identified yet.
//
// Available unless the list is KNOWN and contains none of `needs`. Two rules
// carry the weight:
//   - OR, not AND. One of four channels is enough; a view that works on a
//     partial car is worth showing.
//   - Unknown (nullptr) means available. During connect there is no list, and
//     blanking every view would look like a fault rather than a handshake.
// An empty `needs` means the view does not depend on the car at all.
bool view_available(const char* needs, const std::set<std::string>* supported);

}  // namespace gauge
