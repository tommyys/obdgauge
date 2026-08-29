// The view table. One place that says what each view shows, so adding a view
// is data rather than a new layout.
#pragma once
#include <string>
#include "gauge_ui.h"

namespace gauge_ui {

// A row of small label/value text under the hero, or one cell of a grid --
// which of the two is decided by the view's Layout, not by the row itself.
struct Row {
    const char* label;
    std::string (*value)(const Model&);
};

// An optional dial around the rim: absent `value` means the view draws none.
struct Dial {
    double (*lo)(const Model&);
    double (*hi)(const Model&);
    std::string (*unused)(const Model&);   // reserved, keeps the struct POD-ish
    bool (*value)(const Model&, double* out);
};

// What the view draws to show its reading as a shape rather than a number.
// Named Instrument, not Face, because face.h already owns Face -- that one is
// the built objects, this one is the choice between them.
enum class Instrument {
    None,        // no dial at all -- ECONOMY, TRIP, THERMALS
    RimArc,      // plain progress arc on the 434 px rim
    TachoDial,   // heat band, ticks and numbering per 1000 rpm, needle
    Engine,      // a cold-to-hot gradient rim and a white mark, no fill
    Power,       // ticks and kW numbering, fill arc, amber peak mark, needle
    ScoreRing,   // a rim arc like the tacho's, but coloured by its own value
};

// How the rows are arranged.
enum class Layout {
    Rows,   // small label/value pairs stacked under the hero
    Grid,   // 2x2 of large value-over-label cells; a 3rd-of-3 spans both columns
    // A list of recorded drives, built and filled by gauge_ui/drives.cpp
    // rather than from `rows` -- its content is what the flash ring holds,
    // which is not a function of the Model the other views render from.
    Drives,
};

// What the view says when the car cannot drive it at all. `needs` is a
// comma-separated channel list; see gauge::view_available for the rule.
struct Avail {
    const char* needs;
    const char* head;   // "NO RPM"
    const char* note;   // "this car is not reporting engine speed"
};

struct ViewSpec {
    const char* title;
    Instrument face;
    Layout layout;
    Avail  avail;
    std::string (*hero)(const Model&);     // "--" when the channel is absent;
                                           // null when the view has no hero
    const char* hero_unit;
    // Colour + word under the hero, e.g. COLD/WARMING/READY. Empty = nothing.
    std::string (*state_word)(const Model&, uint32_t* colour);
    Dial dial;
    Row  rows[4];
};

const ViewSpec* view_table(int* count);

}  // namespace gauge_ui
