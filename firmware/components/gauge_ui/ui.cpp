#include "gauge_ui.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "avail.h"
#include "bar.h"
#include "carousel.h"
#include "face.h"
#include "slide.h"
#include "views.h"

namespace gauge_ui {
namespace {

// ---- where text goes, on every view -----------------------------------
// One set of bands, so a view built from a grid lands in the same places as one
// built from rows. They had drifted apart: the grid started higher and ran 18 px
// lower, which on TRIP put the bottom label almost against the make/model
// banner while every other view had a clear margin under it.
constexpr int kTitleY   = -142;  // view name, under the top of the rim
constexpr int kHeroY    =  -60;  // the one big number
constexpr int kUnitY    =  -18;  // its unit, directly beneath
constexpr int kWordY    =   22;  // COLD / READY / the coach verdict
constexpr int kSubY0    =   66;  // first line of sub-text, Rows layout
// A grid cell is a value over its own label, so two rows of them stand about
// 96 px tall against the 62 px of four text lines. Anchoring both layouts at
// the same TOP therefore finished the grid 50 px lower down the panel. TRIP is
// raised 40 px, which leaves it the same clear margin above the make/model
// banner that the rows views have. 48 px was tried first and read as too high.
constexpr int kGridHeroY = kSubY0 - 40;
constexpr int kRowStep  =   24;  // Rows: line to line
constexpr int kCellStep =   52;  // Grid: cell row to cell row
constexpr int kCellGap  =   24;  // Grid: a cell's value to its own label
constexpr int kCellX    =   82;  // Grid: column offset from centre
// A grid with no hero has the whole middle to itself, so its block is centred
// rather than hung below where the hero would have been.
constexpr int kGridNoHeroY = -24;

// 135 positions across a 270-degree sweep: one step is ~2 degrees.
constexpr int kDialSteps = 135;
// One swipe cannot reasonably produce two intended gestures inside this
// window, and LVGL repeats the event every input read (~16ms).
constexpr uint32_t kGestureDebounceMs = 400;

// Views slide, but not by moving LVGL objects. Moving two live 466x466 views
// makes LVGL re-render both of them through a 466x50 partial buffer on every
// frame -- about 52 ms each, so a 240 ms slide was four frames of judder, which
// is why this used to be an instant cut. slide.h renders each view once into a
// buffer and then blits composed frames straight past LVGL, so the per-frame
// cost is the panel's own ~22 ms and the motion is real. The instant cut
// survives as the fallback for when the buffers do not fit.

struct ViewObjs {
    lv_obj_t* root   = nullptr;
    lv_obj_t* title  = nullptr;
    lv_obj_t* hero   = nullptr;
    lv_obj_t* unit   = nullptr;
    lv_obj_t* word   = nullptr;
    lv_obj_t* arc    = nullptr;
    // Tacho only: a black arc under the shutter, two pixels wider, that hides
    // the redline's anti-aliased edges. Invisible on a black face, so it costs
    // the rim no apparent thickness -- see build_view.
    lv_obj_t* arc_mask = nullptr;
    lv_obj_t* rlabel[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* rvalue[4] = {nullptr, nullptr, nullptr, nullptr};
    Face face;                 // tacho, engine and power fill this in
    Bar  bar;                  // electrical only
    bool has_face = false;
    // The "this car cannot drive this view" screen. Everything else in the
    // view is hidden behind it rather than left showing dashes.
    lv_obj_t* na_head = nullptr;
    lv_obj_t* na_note = nullptr;
    lv_obj_t* content = nullptr;   // parent of everything the na screen hides
    bool na_shown = false;
};

const ViewSpec* g_specs = nullptr;
int g_count = 0;
int g_cur = 0;
std::vector<ViewObjs> g_objs;
lv_obj_t* g_parent = nullptr;
gauge::Identity g_id;
lv_obj_t* g_banner = nullptr;
lv_obj_t* g_dots[16] = {nullptr};
lv_obj_t* g_dot_active = nullptr;
char g_banner_base[40] = {0};
bool g_dials_on = true;
int  g_dot_x0 = 0;
int  g_dot_spacing = 18;
int      g_gestures = 0;
uint32_t g_last_gesture_ms = 0;
int      g_presses = 0;
int      g_releases = 0;

int screen_w() { return lv_obj_get_width(g_parent); }

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour,
                   lv_align_t align, int dx, int dy) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, dx, dy);
    return l;
}

ViewObjs build_view(const ViewSpec& spec) {
    ViewObjs v;
    v.root = lv_obj_create(g_parent);
    lv_obj_remove_style_all(v.root);
    lv_obj_set_size(v.root, screen_w(), screen_w());
    lv_obj_set_style_bg_color(v.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v.root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v.root, LV_OBJ_FLAG_SCROLLABLE);

    // Everything the view normally shows hangs off `content`, so the
    // not-available screen is one flag rather than a walk over a dozen objects.
    v.content = lv_obj_create(v.root);
    lv_obj_remove_style_all(v.content);
    lv_obj_set_size(v.content, screen_w(), screen_w());
    lv_obj_center(v.content);
    lv_obj_remove_flag(v.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* c = v.content;

    const Instrument face_k = spec.face;
    const Layout layout_k = spec.layout;
    const bool tacho = face_k == Instrument::TachoDial;
    const bool power = face_k == Instrument::Power;
    v.has_face = tacho || power || face_k == Instrument::Engine;

    // The track, the zones and the heat band go on before the value arc,
    // because the value arc has to cover them as it fills.
    if (v.has_face) {
        face_build_under(c, g_id, tacho ? FaceKind::Tacho
                                        : power ? FaceKind::Power : FaceKind::Engine);
    }

    // The engine face reads by mark position over fixed zones, so it has no
    // fill arc at all; every other dial that has a value gets one.
    const bool wants_arc = spec.dial.value && face_k != Instrument::Engine;
    if (wants_arc) {
        const bool score_ring = face_k == Instrument::ScoreRing;

        // The tacho's shutter has to hide a bright redline arc lying directly
        // under it. At equal size it cannot: both are anti-aliased, so the
        // shutter's edge pixels are part-transparent and the red beneath shows
        // through as a hairline. Making the shutter bigger fixed that but left
        // the rim visibly thicker where it covered than where the heat band
        // showed, with a step at the needle.
        //
        // So the oversized arc is here instead, and it is BLACK -- the face's
        // own colour. It hides the redline's edges and shows nothing itself, so
        // every ring the eye can actually see is kRimPx by kRimWidth. It tracks
        // the shutter's angle exactly and is created first, so the shutter
        // draws on top of it.
        if (tacho) {
            v.arc_mask = lv_arc_create(c);
            lv_obj_set_size(v.arc_mask, kRimShutterPx, kRimShutterPx);
            lv_obj_center(v.arc_mask);
            lv_arc_set_bg_angles(v.arc_mask, 135, 45);
            lv_arc_set_mode(v.arc_mask, LV_ARC_MODE_REVERSE);
            lv_obj_remove_style(v.arc_mask, nullptr, LV_PART_KNOB);
            lv_obj_remove_flag(v.arc_mask, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(v.arc_mask, kRimShutterWidth, LV_PART_MAIN);
            lv_obj_set_style_arc_width(v.arc_mask, kRimShutterWidth, LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(v.arc_mask, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_arc_color(v.arc_mask, lv_color_black(), LV_PART_INDICATOR);
            // Square, and this one matters: a rounded cap is drawn OUTSIDE the
            // angle it is given, and this arc is both black and wider than the
            // shutter, so a rounded end would paint black past the shutter and
            // over the lit band -- a dark line sitting at the needle.
            lv_obj_set_style_arc_rounded(v.arc_mask, false, LV_PART_INDICATOR);
        }

        v.arc = lv_arc_create(c);
        // Every ring on every view is now the same ring; see face.h.
        lv_obj_set_size(v.arc, kRimPx, kRimPx);
        lv_obj_center(v.arc);
        lv_arc_set_bg_angles(v.arc, 135, 45);
        lv_obj_remove_style(v.arc, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(v.arc, LV_OBJ_FLAG_CLICKABLE);
        const int w = kRimWidth;
        lv_obj_set_style_arc_width(v.arc, w, LV_PART_MAIN);
        lv_obj_set_style_arc_width(v.arc, w, LV_PART_INDICATOR);
        if (tacho) {
            // Not a fill but a shutter. face_build_under() has laid the heat
            // band all the way round; this arc covers the part the engine has
            // not reached, and REVERSE makes it retreat from the value to the
            // end of the sweep as rpm climbs. Its length is the only thing on
            // the rim that changes, and lv_arc invalidates just the sector that
            // moved -- which is the whole reason the heat can live out here.
            lv_arc_set_mode(v.arc, LV_ARC_MODE_REVERSE);
            lv_obj_set_style_arc_opa(v.arc, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_arc_color(v.arc, lv_color_hex(0x23262E), LV_PART_INDICATOR);
            // Square for the same reason. The shutter's moving end sits AT the
            // needle, so a rounded cap there reaches two degrees back over the
            // band the engine has already lit -- a dark lip travelling with the
            // needle. The ring's rounded silhouette comes from the band's own
            // ends (face.cpp), which is where it belongs: those never move.
            lv_obj_set_style_arc_rounded(v.arc, false, LV_PART_INDICATOR);
        } else if (power) {
            // The face drew the track, so this arc is the fill alone.
            lv_obj_set_style_arc_opa(v.arc, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_arc_color(v.arc, lv_color_hex(0xCFE0FF), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_arc_color(v.arc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
            // The score ring is recoloured by its own value in update(); the
            // green here is only what it looks like before the first reading.
            lv_obj_set_style_arc_color(v.arc, lv_color_hex(score_ring ? 0x35E06B : 0xFF9500),
                                       LV_PART_INDICATOR);
        }
    }

    // Ticks, numbering and the needle go over the value arc, matching the
    // order the simulator draws them in.
    if (v.has_face) {
        v.face = face_build_over(c, g_id, tacho ? FaceKind::Tacho
                                                : power ? FaceKind::Power : FaceKind::Engine);
    }

    // The tacho has no title: the dial numbering would collide with it, and the
    // page dots already say which view you are on.
    if (spec.title) {
        v.title = mk_label(c, &lv_font_montserrat_20, 0x707070, LV_ALIGN_CENTER, 0, kTitleY);
        lv_label_set_text(v.title, spec.title);
    }

    // Thermals has no hero -- the view IS the comparison between its three
    // temperatures, so promoting one of them would misrepresent it.
    //
    // Faces with a needle carry a boss at the centre, 30 px across, and the
    // unit line sat right on it. The simulator never collides because its hero
    // is 21cqw -- 98 px -- and simply covers the boss; LVGL's largest built-in
    // Montserrat is 48 px, so here the two have to be moved apart instead.
    const int hub_lift = (face_k == Instrument::TachoDial ||
                          face_k == Instrument::Power) ? 24 : 0;
    if (spec.hero) {
        v.hero = mk_label(c, &lv_font_montserrat_48, 0xFFFFFF, LV_ALIGN_CENTER, 0, kHeroY - hub_lift);
        v.unit = mk_label(c, &lv_font_montserrat_20, 0x9A9A9A, LV_ALIGN_CENTER, 0, kUnitY - hub_lift);
        lv_label_set_text(v.unit, spec.hero_unit ? spec.hero_unit : "");
    }

    // Where the state word sits depends on whether a unit is already there.
    // Score and Power have no static unit because their word IS the unit line
    // -- the coach verdict, the peak -- so it moves up into that slot rather
    // than leaving a gap the width of a line above it.
    if (spec.state_word) {
        const int wy = (spec.hero_unit ? kWordY : kUnitY) - hub_lift;
        const lv_font_t* f = spec.hero_unit ? &lv_font_montserrat_28 : &lv_font_montserrat_20;
        v.word = mk_label(c, f, 0x808080, LV_ALIGN_CENTER, 0, wy);
    }

    if (face_k == Instrument::Bar) v.bar = bar_build(c, 34);

    int n = 0;
    while (n < 4 && spec.rows[n].label) ++n;

    if (layout_k == Layout::Grid) {
        // Large value over small label, two to a line. A third cell of three
        // spans both columns, as the simulator's catalyst does: an odd one out
        // centred reads as a total rather than as a lonely column.
        const int y0 = spec.hero ? kGridHeroY : kGridNoHeroY;
        for (int i = 0; i < n; ++i) {
            const bool wide = (n == 3 && i == 2);
            const int x = wide ? 0 : ((i % 2) ? kCellX : -kCellX);
            const int y = y0 + (i / 2) * kCellStep;
            v.rvalue[i] = mk_label(c, &lv_font_montserrat_28, 0xF0F0F0, LV_ALIGN_CENTER, x, y);
            v.rlabel[i] = mk_label(c, &lv_font_montserrat_14, 0x707070, LV_ALIGN_CENTER, x,
                                   y + kCellGap);
            lv_label_set_text(v.rlabel[i], spec.rows[i].label);
        }
    } else {
        // Stacked label/value pairs below the hero, left and right of centre --
        // the original arrangement. Only the SIZE was ever wrong: at 20 px the
        // text was wide enough to reach the dial numbering at radius 143 and
        // collided with it. At 14 px, with the rows drawn a little tighter, the
        // whole block sits inside the empty bottom of the dial: a 270-degree
        // sweep running bottom-left over the top to bottom-right leaves
        // everything between 45 and 135 degrees free.
        for (int i = 0; i < n; ++i) {
            int y = kSubY0 + i * kRowStep;
            // Label and value close enough to read as one phrase. At the old
            // +/-78 and 62 the two were 140 px apart, which at 14 px text left
            // them looking like two unrelated columns.
            v.rlabel[i] = mk_label(c, &lv_font_montserrat_14, 0x606060, LV_ALIGN_CENTER, -40, y);
            v.rvalue[i] = mk_label(c, &lv_font_montserrat_14, 0xD0D0D0, LV_ALIGN_CENTER, 20, y);
            lv_label_set_text(v.rlabel[i], spec.rows[i].label);
        }
    }

    // The not-available screen, built once and hidden. Two lines, because
    // "NO RPM" alone reads as a fault in the gauge; the second line says it is
    // the car that is not reporting, which is a different and calmer message.
    if (spec.avail.head) {
        v.na_head = mk_label(v.root, &lv_font_montserrat_28, 0xC0C0C0, LV_ALIGN_CENTER, 0, -18);
        lv_label_set_text(v.na_head, spec.avail.head);
        v.na_note = mk_label(v.root, &lv_font_montserrat_14, 0x707070, LV_ALIGN_CENTER, 0, 22);
        lv_label_set_long_mode(v.na_note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(v.na_note, 300);
        lv_obj_set_style_text_align(v.na_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(v.na_note, spec.avail.note ? spec.avail.note : "");
        lv_obj_align(v.na_note, LV_ALIGN_CENTER, 0, 22);
        lv_obj_add_flag(v.na_head, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v.na_note, LV_OBJ_FLAG_HIDDEN);
    }
    return v;
}

void place(int index, int x) { lv_obj_set_pos(g_objs[index].root, x, 0); }

int g_flip_target = 0;

// Makes the target view the visible one. Every view sits at (0,0) and only
// visibility changes -- moving the objects instead invalidated the full screen
// TWICE per switch, once for the outgoing view's old area and once for the
// incoming view's, which is ~104 ms of dead time at the measured ~52 ms
// full-frame cost. A view change has to repaint the screen once; not twice.
void flip_now(void*) {
    lv_obj_add_flag(g_objs[static_cast<size_t>(g_cur)].root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_objs[static_cast<size_t>(g_flip_target)].root, LV_OBJ_FLAG_HIDDEN);
    g_cur = g_flip_target;
    // The dot moves in the same breath so that the destination snapshot -- and
    // so the sliding frame -- already shows the right page. It needs no
    // animation of its own now that the view itself is the motion cue.
    if (g_dot_active) lv_obj_set_x(g_dot_active, g_dot_x0 + g_cur * g_dot_spacing - 3);
}

void switch_to(int target, int dir) {
    if (target == g_cur || target < 0 || target >= g_count) return;
    g_flip_target = target;
    // slide_run always performs the flip, so there is exactly one path that
    // changes the view whether or not the animation is available.
    if (slide_ready()) slide_run(dir, flip_now, nullptr);
    else               flip_now(nullptr);
}

void gesture_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;

    // LVGL repeats LV_EVENT_GESTURE for as long as the gesture is active, once
    // per input read, so one swipe would otherwise advance many views.
    //
    // lv_indev_wait_release() is the documented remedy but is unusable here:
    // it suppresses input until a RELEASE is observed, and with the CST9217 in
    // IRQ mode a release is not reliably reported, so the indev latched after
    // the very first gesture and never accepted another -- one gesture in 75
    // seconds of swiping.
    //
    // A time debounce needs no release at all: take the first gesture, ignore
    // the repeats that follow it.
    uint32_t now = lv_tick_get();
    if (g_last_gesture_ms && (now - g_last_gesture_ms) < kGestureDebounceMs) return;
    g_last_gesture_ms = now;

    // gauge::ring_index is host-tested (test_carousel.cpp), so the wrap is not
    // something to wonder about from the board.
    const int step = (dir == LV_DIR_LEFT) ? +1 : -1;
    int target = gauge::ring_index(g_cur, g_count, step);
    switch_to(target, step);
    ++g_gestures;
}

}  // namespace

void init(lv_obj_t* parent, const gauge::Identity& id) {
    g_parent = parent;
    // Presses land here now that the view roots are non-clickable, and LVGL
    // screens are scrollable by default. When LVGL decides a drag is a scroll it
    // suppresses the gesture -- which is why 21 touches produced only 4
    // gestures. Nothing here scrolls, so take the flag off.
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_NONE);
    // The dial face is built from the car's own scale, so the profile has to be
    // known before any view is.
    g_id = id;
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    g_specs = view_table(&g_count);
    g_objs.clear();
    g_objs.reserve(static_cast<size_t>(g_count));
    for (int i = 0; i < g_count; ++i) g_objs.push_back(build_view(g_specs[i]));
    for (int i = 0; i < g_count; ++i) {
        place(i, 0);
        // Presses must NOT land on a view root. switch_to() hides the current
        // view, and hiding the object LVGL is tracking as the active press
        // target leaves the input device wedged -- exactly one gesture was ever
        // delivered, then nothing, however the handler was written. Making the
        // roots non-clickable puts the press on the screen, which never hides.
        lv_obj_remove_flag(g_objs[static_cast<size_t>(i)].root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_objs[static_cast<size_t>(i)].root, LV_OBJ_FLAG_GESTURE_BUBBLE);
        if (i != 0) lv_obj_add_flag(g_objs[static_cast<size_t>(i)].root, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(parent, gesture_cb, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(parent, [](lv_event_t*) { ++g_presses; slide_prepare(); },
                        LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(parent, [](lv_event_t*) { ++g_releases; }, LV_EVENT_RELEASED, nullptr);
    g_cur = 0;

    // Page indicator. The content cut is instant because a full-screen change
    // costs ~52ms on this single-buffered panel, so a sliding view would be
    // about four frames of judder. Instead the *indicator* animates: a 12px dot
    // moving 18px dirties almost nothing, so it is genuinely smooth and still
    // tells you which way you moved and where you are in the ring.
    const int spacing = 18;
    const int x0 = -(g_count - 1) * spacing / 2;
    for (int i = 0; i < g_count && i < 16; ++i) {
        g_dots[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(g_dots[i]);
        lv_obj_set_size(g_dots[i], 6, 6);
        lv_obj_set_style_radius(g_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(g_dots[i], lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_bg_opa(g_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(g_dots[i], LV_ALIGN_CENTER, x0 + i * spacing, 205);
    }
    g_dot_active = lv_obj_create(parent);
    lv_obj_remove_style_all(g_dot_active);
    lv_obj_set_size(g_dot_active, 12, 12);
    lv_obj_set_style_radius(g_dot_active, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot_active, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_bg_opa(g_dot_active, LV_OPA_COVER, 0);
    lv_obj_align(g_dot_active, LV_ALIGN_CENTER, x0, 205);
    lv_obj_update_layout(parent);
    g_dot_x0 = lv_obj_get_x(g_dots[0]);
    g_dot_spacing = spacing;

    // The make/model banner persists across views (SPEC.md section 10), so it
    // lives on the parent rather than inside any one view.
    g_banner = lv_label_create(parent);
    lv_obj_set_style_text_font(g_banner, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_banner, lv_color_hex(0x585858), 0);
    snprintf(g_banner_base, sizeof g_banner_base, "%s", id.label.c_str());
    lv_label_set_text(g_banner, g_banner_base);
    lv_obj_align(g_banner, LV_ALIGN_CENTER, 0, 178);

    // Last, because it measures the screen and wants the PSRAM the boot clip
    // has just given back.
    slide_init(parent);
}

void set_text_if_changed(lv_obj_t* label, const std::string& text) {
    const char* cur = lv_label_get_text(label);
    if (cur && text == cur) return;
    lv_label_set_text(label, text.c_str());
}

void update(const Model& m) {
    if (g_cur < 0 || g_cur >= g_count) return;
    const ViewSpec& s = g_specs[g_cur];
    ViewObjs& v = g_objs[static_cast<size_t>(g_cur)];

    // Can this car drive this view at all? The rule is host-tested in
    // test_avail.cpp; the part that matters here is that it is checked BEFORE
    // anything is formatted, so an unavailable view costs nothing per frame.
    if (v.na_head) {
        const bool want_na = !gauge::view_available(s.avail.needs, m.supported);
        if (want_na != v.na_shown) {
            v.na_shown = want_na;
            if (!want_na) {
                lv_obj_clear_flag(v.content, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(v.na_head, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(v.na_note, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(v.content, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(v.na_head, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(v.na_note, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (v.na_shown) return;
    }

    if (v.hero) set_text_if_changed(v.hero, s.hero(m));

    if (s.state_word && v.word) {
        uint32_t colour = 0x808080;
        std::string w = s.state_word(m, &colour);
        if (w != lv_label_get_text(v.word)) {
            lv_label_set_text(v.word, w.c_str());
            lv_obj_set_style_text_color(v.word, lv_color_hex(colour), 0);
        }
    }

    if (v.arc && s.dial.value) {
        double val = 0.0;
        const bool tacho = s.face == Instrument::TachoDial;
        if (s.dial.value(m, &val)) {
            int lo = static_cast<int>(s.dial.lo(m));
            int hi = static_cast<int>(s.dial.hi(m));
            if (hi > lo) {
                // The dial is 434 px across, so ANY change to its value
                // invalidates almost the whole screen -- which is why the UI sat
                // at the ~19 fps full-screen ceiling while rpm moved every
                // sample. Quantise to kDialSteps positions (~2 degrees of a
                // 270-degree sweep): visually identical, and most frames now
                // leave the dial alone and redraw only the small text areas.
                // Same lesson as the section 11 backdrop: the win is in not
                // touching big objects, not in drawing them faster.
                int steps = kDialSteps;
                int q = static_cast<int>((val - lo) / (hi - lo) * steps + 0.5);
                if (q < 0) q = 0;
                if (q > steps) q = steps;
                lv_arc_set_range(v.arc, 0, steps);
                if (v.arc_mask) lv_arc_set_range(v.arc_mask, 0, steps);
                // The shutter runs backwards on purpose. LVGL's REVERSE mode
                // maps the value from the END of the sweep to the start -- at
                // the minimum it covers nothing, at the maximum it covers
                // everything -- so feeding it rpm directly left the rim fully
                // hot at idle and going dark as the engine picked up, with the
                // needle sweeping the other way. Inverting here puts the edge
                // of the shutter exactly under the needle.
                lv_arc_set_value(v.arc, tacho ? steps - q : q);
                // The mask is the shutter's own shape; it must never lag, or
                // the redline's edge reappears for a frame at the seam.
                if (v.arc_mask) lv_arc_set_value(v.arc_mask, steps - q);
            }
            // The score ring carries its verdict as colour, the way the
            // simulator's does: green from 85, amber from 70, red below.
            if (s.face == Instrument::ScoreRing) {
                const uint32_t col = val >= 85 ? 0x35E06B : val >= 70 ? 0xFFC53D : 0xFF3B30;
                lv_obj_set_style_arc_color(v.arc, lv_color_hex(col), LV_PART_INDICATOR);
            }
            if (g_dials_on) lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        } else if (tacho) {
            // The shutter is not a reading, it is the absence of one: closed
            // over the whole band, so a car that is not reporting rpm shows a
            // cold dial rather than a dial pinned at the redline. Closed is the
            // MAXIMUM here, for the inversion described above.
            lv_arc_set_value(v.arc, kDialSteps);
            if (v.arc_mask) lv_arc_set_value(v.arc_mask, kDialSteps);
            if (g_dials_on) lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            // No reading: hide the dial rather than draw it pinned at zero.
            lv_obj_add_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (v.has_face) face_update(v.face, m);

    if (s.face == Instrument::Bar) {
        auto volts = m.st.get("ctrl_volt");
        if (!volts) volts = m.st.get("volts");
        bar_update(v.bar, volts);
    }

    for (int i = 0; i < 4 && s.rows[i].label; ++i) {
        set_text_if_changed(v.rvalue[i], s.rows[i].value(m));
    }
}

void set_dial_enabled(bool on) {
    if (g_dials_on == on) return;
    g_dials_on = on;
    for (auto& v : g_objs) {
        if (!v.arc) continue;
        if (on) lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_fps(uint32_t fps) {
    if (!g_banner) return;
    char b[64];
    if (fps) snprintf(b, sizeof b, "%s  %u fps", g_banner_base, (unsigned)fps);
    else     snprintf(b, sizeof b, "%s", g_banner_base);
    set_text_if_changed(g_banner, b);
}

int gesture_count() { return g_gestures; }
int press_count() { return g_presses; }
int release_count() { return g_releases; }

int view_count() { return g_count; }
int current_view() { return g_cur; }
const char* current_view_name() {
    if (g_cur < 0 || g_cur >= g_count) return "";
    // The tacho has no title of its own, so name it for the log rather than
    // returning an empty string that reads as a bug.
    const char* t = g_specs[g_cur].title;
    return t ? t : "TACHO";
}

}  // namespace gauge_ui
