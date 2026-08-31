#include "clock.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace gauge_ui {
namespace {

const ClockSource* g_src = nullptr;

// The five wheels, left to right and top to bottom: day, month, year on the
// date row; hour, minute on the time row.
enum Wheel { kDay, kMon, kYear, kHour, kMin, kWheels };
lv_obj_t* g_roll[kWheels] = {nullptr};

lv_obj_t* g_set   = nullptr;
// No title and no status line. The wheels say what the view is, and the live
// clock in the header says what the gauge currently thinks the time is -- on
// this view and on the other six. A caption repeating either was one more
// thing between the two rows a thumb has to reach.

// Years offered. Ten is plenty for a car gauge and keeps the wheel short
// enough to spin end to end; the first is the year this was written, so the
// list never starts in the past.
constexpr int kYear0  = 2026;
constexpr int kYears  = 10;

// The wheels are only seeded from the clock while nobody is touching them.
// Without this the view would drag a half-set wheel back to `now` under the
// finger every frame.
bool g_seeded = false;
// The minute the wheels were last put on. Re-seeding them every frame meant
// five lv_roller_set_selected calls 66 times a second, each of which scrolls a
// label -- the same "a write is a repaint" rule the rest of the UI follows.
// The clock only moves once a minute, so that is how often this needs to.
int32_t g_seeded_min = -1;
// Cleared by any wheel moving. Until then the view is a live clock and keeps
// following the board; after it, the wheels are the driver's and are left
// alone.
bool g_touched = false;
// Set briefly after SET so the note can say what happened rather than going
// straight back to reading like nothing did.
int64_t g_said_ms = 0;

constexpr const char* kMonths =
    "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";

void wheel_changed(lv_event_t*) {
    g_touched = true;
    // Forget which minute the wheels were on. Otherwise, if the clock is ever
    // followed again, the next seed would be skipped as "already there" while
    // the wheels are actually wherever a thumb left them.
    g_seeded_min = -1;
}

// Days in a month, so 31 February becomes 28 or 29 rather than rolling into
// March. The wheel always offers 1-31 -- rebuilding its options every time
// the month wheel moves is allocation on a user's finger, for a case that
// only happens if you go looking for it -- so the clamp lives here instead.
int days_in(int year, int month) {
    static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month != 2) return len[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
}

// A row fades out at its TOP and BOTTOM, the way a phone's picker does -- the
// options roll in from nothing above and leave into nothing below, and the one
// in the middle is the only one at full strength.
//
// It was left and right first. That was wrong, and wrong in a way that read as
// a different fault: dimming the ends of a row makes the outer wheels look
// pulled towards the middle, so five columns that were in fact centred looked
// as though they were not. Fading top and bottom says what is actually true --
// that the middle row is the one that counts -- and leaves the columns alone.
//
// There is no alpha gradient available to do it with: this build has
// LV_USE_DRAW_SW_COMPLEX_GRADIENTS off and LV_GRADIENT_MAX_STOPS at 2, so a
// style gradient can move colour but not opacity, and over a black background
// that is no fade at all. Turning the option on costs code size and draw time
// on every gradient in the firmware, for one decoration.
//
// So it is stacked strips of black at decreasing opacity -- the same trick
// SPEC.md section 4 uses for the tacho's heat band, where a real gradient was
// also unaffordable and segments of a ramp read as smooth at this size. Built
// once and never touched again, so it costs nothing per frame.
constexpr int kFadeSteps = 6;

// `outer_y` is the row's top or bottom edge; `dir` is +1 to walk down from the
// top, -1 to walk up from the bottom. `depth` is how far in the fade reaches,
// and it is always exactly one row of the wheel -- so the fade ends where the
// selection band begins, and there is no strip of full-brightness text
// stranded between the two. That gap was the thing that looked wrong: the
// gradient has to run into the box, not stop short of it.
void mk_fade(lv_obj_t* parent, int cx, int w, int outer_y, int dir, int depth) {
    const int step = (depth + kFadeSteps - 1) / kFadeSteps;
    for (int i = 0; i < kFadeSteps; ++i) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, w, step);
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        // Nearly solid at the outer edge, gone by the inner one. Squared, so
        // the fade holds on longer at the outside and lets go quickly at the
        // inside -- a linear ramp reads as a grey band with an edge on it.
        const double f = 1.0 - static_cast<double>(i) / kFadeSteps;
        lv_obj_set_style_bg_opa(o, static_cast<lv_opa_t>(245.0 * f * f), 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        // Not clickable, so a thumb landing on the fade still reaches the
        // wheel underneath it.
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        const int y = dir > 0 ? outer_y + i * step
                              : outer_y - (i + 1) * step;
        lv_obj_align(o, LV_ALIGN_TOP_MID, cx, y);
    }
}

// `wrap` puts the wheel in LVGL's infinite mode: the option list is repeated
// so 31 rolls round to 1 and 59 rolls round to 00, the way a phone's picker
// does. It is what you want on a wheel that is a cycle -- day, month, hour and
// minute all are. It is NOT what you want on the year, which is a range with a
// first and a last, and a wheel that rolls 2035 back round to 2024 tells you
// the gauge will take a year it will not.
//
// "Infinite" is LVGL's word for three copies of the list, not for no end at
// all: the wheel stops after one full turn either way (LV_CLAMP in
// lv_roller.c, and EXTRA_INF_SIZE decides the copies -- every list here is
// long enough that it lands on the minimum of three). Setting a clock never
// spins one wheel past sixty steps in one direction, so the end is not
// reachable by hand.
//
// lv_roller_get_selected already reports the index within the REAL list in
// this mode, so nothing that reads the wheels -- seed_from, set_clicked --
// changes.
lv_obj_t* mk_roller(lv_obj_t* parent, const char* options, int w, int h, int x,
                    int y, const lv_font_t* font, bool wrap) {
    lv_obj_t* r = lv_roller_create(parent);
    lv_roller_set_options(r, options,
                          wrap ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    // Height set explicitly, not left to the font: the fade strips beside the
    // row have to be exactly as tall as it is, and asking LVGL for a height
    // before the first layout pass answers 0.
    lv_obj_set_size(r, w, h);
    lv_obj_align(r, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_text_font(r, font, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0x6A737F), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, LV_PART_SELECTED);
    // The centre band: the value under it is the one that counts, so it is
    // the only part drawn at full brightness.
    lv_obj_set_style_bg_opa(r, LV_OPA_30, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x49535F), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(0xF0F0F0), LV_PART_SELECTED);
    lv_obj_add_event_cb(r, wheel_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    // The options live on a label inside the roller, and it is that label
    // which lays the text out. Styling the roller alone left the columns
    // ragged -- each wheel's widest option decided where its text started, so
    // "Sep" and "1" did not share a centre line. Widening the label to the
    // wheel and centring it there is what actually lines the five up.
    if (lv_obj_t* lbl = lv_obj_get_child(r, 0)) {
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    }
    return r;
}

// Put the wheels on one moment.
void seed_from(uint32_t epoch_s) {
    const time_t t = static_cast<time_t>(epoch_s);
    struct tm tm_v;
    localtime_r(&t, &tm_v);
    int yi = tm_v.tm_year + 1900 - kYear0;
    if (yi < 0) yi = 0;
    if (yi >= kYears) yi = kYears - 1;
    lv_roller_set_selected(g_roll[kDay],  (uint16_t)(tm_v.tm_mday - 1), LV_ANIM_OFF);
    lv_roller_set_selected(g_roll[kMon],  (uint16_t)tm_v.tm_mon, LV_ANIM_OFF);
    lv_roller_set_selected(g_roll[kYear], (uint16_t)yi, LV_ANIM_OFF);
    lv_roller_set_selected(g_roll[kHour], (uint16_t)tm_v.tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(g_roll[kMin],  (uint16_t)tm_v.tm_min, LV_ANIM_OFF);
}

void set_clicked(lv_event_t*) {
    if (!g_src || !g_src->set) return;
    struct tm tm_v{};
    tm_v.tm_year = kYear0 + (int)lv_roller_get_selected(g_roll[kYear]) - 1900;
    tm_v.tm_mon  = (int)lv_roller_get_selected(g_roll[kMon]);
    tm_v.tm_mday = (int)lv_roller_get_selected(g_roll[kDay]) + 1;
    tm_v.tm_hour = (int)lv_roller_get_selected(g_roll[kHour]);
    tm_v.tm_min  = (int)lv_roller_get_selected(g_roll[kMin]);
    tm_v.tm_sec  = 0;
    tm_v.tm_isdst = -1;
    const int last = days_in(tm_v.tm_year + 1900, tm_v.tm_mon + 1);
    if (tm_v.tm_mday > last) {
        tm_v.tm_mday = last;
        // Move the wheel too, so the screen agrees with what was stored.
        lv_roller_set_selected(g_roll[kDay], (uint16_t)(last - 1), LV_ANIM_ON);
    }
    const time_t t = mktime(&tm_v);
    if (t <= 0) return;
    if (!g_src->set((uint32_t)t)) return;
    // Back to following the clock: the wheels and the board now agree, so
    // there is nothing half-entered left to protect. The confirmation is the
    // clock at the top of the screen jumping to what was just set -- which is
    // the thing being changed, so it is the right thing to watch.
    g_touched = false;
    g_seeded_min = -1;
}

}  // namespace

void clock_set_source(const ClockSource* src) { g_src = src; }

uint32_t clock_now() {
    return (g_src && g_src->now) ? g_src->now() : 0;
}

void clock_build(lv_obj_t* parent) {
    // Days 1-31 and minutes 0-59 are too long to write out by hand, and the
    // strings have to outlive this function -- lv_roller_set_options copies,
    // but only once, so a stack buffer would be read after it is gone on a
    // later rebuild. Static, built once.
    static char days[31 * 3 + 1];
    static char hours[24 * 3 + 1];
    static char mins[60 * 3 + 1];
    static char years[kYears * 5 + 1];
    char* p = days;
    for (int d = 1; d <= 31; ++d) p += std::snprintf(p, 4, d == 1 ? "%d" : "\n%d", d);
    p = hours;
    for (int h = 0; h < 24; ++h) p += std::snprintf(p, 4, h == 0 ? "%02d" : "\n%02d", h);
    p = mins;
    for (int m = 0; m < 60; ++m) p += std::snprintf(p, 4, m == 0 ? "%02d" : "\n%02d", m);
    p = years;
    for (int y = 0; y < kYears; ++y)
        p += std::snprintf(p, 6, y == 0 ? "%d" : "\n%d", kYear0 + y);

    // Two rows, pushed apart. The date sits high and the time low, with the
    // SET button lower still, so each is its own band with clear glass between
    // -- at the first spacing the three ran together and a thumb reaching for
    // the hour wheel was already over the minutes. Both rows stay on the wide
    // part of the circle, where a 466 px round screen actually has width.
    // Tighter than the first pass. The fade at each row's ends now does the
    // separating, so the rows no longer need clear glass between them to read
    // as two -- and closing that up is what lets SET sit higher, out of the
    // narrow bottom of the circle.
    constexpr int kDateY = 80, kTimeY = 196;
    constexpr int kDateH = 96, kTimeH = 108;
    // Everything but the year wraps. See mk_roller on why the year does not.
    g_roll[kDay]  = mk_roller(parent, days,    84, kDateH, -118, kDateY, &lv_font_montserrat_24, true);
    g_roll[kMon]  = mk_roller(parent, kMonths, 96, kDateH,    0, kDateY, &lv_font_montserrat_24, true);
    g_roll[kYear] = mk_roller(parent, years,  108, kDateH,  118, kDateY, &lv_font_montserrat_24, false);
    g_roll[kHour] = mk_roller(parent, hours,  104, kTimeH,  -62, kTimeY, &lv_font_montserrat_28, true);
    g_roll[kMin]  = mk_roller(parent, mins,   104, kTimeH,   62, kTimeY, &lv_font_montserrat_28, true);

    // Over each row, after the wheels so the fade sits on top of them. A row
    // is as wide as its outermost wheel edges, plus a little either side so
    // no seam shows where the strips end.
    constexpr int kDateL = -118 -  84 / 2, kDateR = 118 + 108 / 2;
    constexpr int kTimeL =  -62 - 104 / 2, kTimeR =  62 + 104 / 2;
    constexpr int kDateCx = (kDateL + kDateR) / 2, kDateW = kDateR - kDateL + 8;
    constexpr int kTimeCx = (kTimeL + kTimeR) / 2, kTimeW = kTimeR - kTimeL + 8;
    // A wheel shows three rows, so one row is a third of its height -- which
    // is exactly how deep the fade goes.
    mk_fade(parent, kDateCx, kDateW, kDateY,          +1, kDateH / 3);
    mk_fade(parent, kDateCx, kDateW, kDateY + kDateH, -1, kDateH / 3);
    mk_fade(parent, kTimeCx, kTimeW, kTimeY,          +1, kTimeH / 3);
    mk_fade(parent, kTimeCx, kTimeW, kTimeY + kTimeH, -1, kTimeH / 3);

    g_set = lv_button_create(parent);
    lv_obj_set_size(g_set, 150, 52);
    lv_obj_align(g_set, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_set_style_radius(g_set, 26, 0);
    lv_obj_set_style_bg_color(g_set, lv_color_hex(0x2A3038), 0);
    lv_obj_set_style_border_color(g_set, lv_color_hex(0x5BD97A), 0);
    lv_obj_set_style_border_width(g_set, 1, 0);
    lv_obj_add_event_cb(g_set, set_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(g_set);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x5BD97A), 0);
    lv_label_set_text(lbl, "SET");
    lv_obj_center(lbl);

}

void clock_update() {
    if (!g_src) return;
    const uint32_t now = g_src->now ? g_src->now() : 0;
    const uint32_t floor_s = g_src->floor ? g_src->floor() : 0;

    // Where to start. The clock if it is running -- then the wheels follow it
    // minute by minute and the view is itself a clock. Otherwise the last
    // moment a previous run persisted, which is effectively the end of the
    // last drive, so setting it the next day is a nudge on two wheels rather
    // than five. Never a guess forward.
    //
    // A reboot loses the running clock but keeps that floor, which is why the
    // wheels can sit still while looking set: nobody has told the gauge the
    // time yet this run. The header shows --:-- for exactly that reason.
    const uint32_t seed = now ? now : floor_s;
    const int32_t seed_min = seed ? (int32_t)(seed / 60) : -1;
    if (!g_touched && seed && seed_min != g_seeded_min) {
        g_seeded_min = seed_min;
        seed_from(seed);
        g_seeded = true;
    }
    (void)g_said_ms;
    (void)g_seeded;
}

}  // namespace gauge_ui
