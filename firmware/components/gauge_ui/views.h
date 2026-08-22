// The view table. One place that says what each view shows, so adding a view
// is data rather than a new layout.
#pragma once
#include <string>
#include "gauge_ui.h"

namespace gauge_ui {

// A row of small label/value text under the hero number.
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

struct ViewSpec {
    const char* title;
    std::string (*hero)(const Model&);     // "--" when the channel is absent
    const char* hero_unit;
    // Colour + word under the hero, e.g. COLD/WARMING/READY. Empty = nothing.
    std::string (*state_word)(const Model&, uint32_t* colour);
    Dial dial;
    Row  rows[4];
};

const ViewSpec* view_table(int* count);

}  // namespace gauge_ui
