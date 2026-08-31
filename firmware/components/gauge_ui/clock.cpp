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

lv_obj_t* g_title = nullptr;
lv_obj_t* g_set   = nullptr;
lv_obj_t* g_note  = nullptr;   // where the wheels started, or what just happened

// Years offered. Ten is plenty for a car gauge and keeps the wheel short
// enough to spin end to end; the first is the year this was written, so the
// list never starts in the past.
constexpr int kYear0  = 2026;
constexpr int kYears  = 10;

// The wheels are only seeded from the clock while nobody is touching them.
// Without this the view would drag a half-set wheel back to `now` under the
// finger every frame.
bool g_seeded = false;
// Cleared by any wheel moving. Until then the view is a live clock and keeps
// following the board; after it, the wheels are the driver's and are left
// alone.
bool g_touched = false;
// Set briefly after SET so the note can say what happened rather than going
// straight back to reading like nothing did.
int64_t g_said_ms = 0;

constexpr const char* kMonths =
    "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";

void wheel_changed(lv_event_t*) { g_touched = true; }

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

lv_obj_t* mk_roller(lv_obj_t* parent, const char* options, int w, int x, int y,
                    const lv_font_t* font) {
    lv_obj_t* r = lv_roller_create(parent);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_align(r, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_text_font(r, font, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0x6A737F), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    // The centre band: the value under it is the one that counts, so it is
    // the only part drawn at full brightness.
    lv_obj_set_style_bg_opa(r, LV_OPA_20, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2A3038), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(0xF0F0F0), LV_PART_SELECTED);
    lv_obj_add_event_cb(r, wheel_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    return r;
}

void set_note(const char* s) {
    if (!g_note) return;
    const char* cur = lv_label_get_text(g_note);
    if (cur && !strcmp(cur, s)) return;
    lv_label_set_text(g_note, s);
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
    if (t <= 0) { set_note("could not read that date"); return; }
    char buf[64];
    if (g_src->set((uint32_t)t)) {
        strftime(buf, sizeof buf, "clock set to %d %b %H:%M", &tm_v);
        // Back to following the board: the wheels and the clock now agree, so
        // there is nothing half-entered left to protect.
        g_touched = false;
    } else {
        std::snprintf(buf, sizeof buf, "could not store the clock");
    }
    set_note(buf);
    g_said_ms = (int64_t)lv_tick_get();
}

}  // namespace

void clock_set_source(const ClockSource* src) { g_src = src; }

void clock_build(lv_obj_t* parent) {
    g_title = lv_label_create(parent);
    lv_obj_set_style_text_font(g_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_title, lv_color_hex(0x808080), 0);
    lv_label_set_text(g_title, "SET CLOCK");
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 62);

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

    // Two rows. The date row sits above the middle and the time row below it,
    // so both are on the wide part of a round screen rather than out where the
    // circle takes the corners away.
    constexpr int kDateY = 108, kTimeY = 224;
    g_roll[kDay]  = mk_roller(parent, days,   84, -118, kDateY, &lv_font_montserrat_24);
    g_roll[kMon]  = mk_roller(parent, kMonths, 96,    0, kDateY, &lv_font_montserrat_24);
    g_roll[kYear] = mk_roller(parent, years, 108,  118, kDateY, &lv_font_montserrat_24);
    g_roll[kHour] = mk_roller(parent, hours,  104,  -62, kTimeY, &lv_font_montserrat_28);
    g_roll[kMin]  = mk_roller(parent, mins,   104,   62, kTimeY, &lv_font_montserrat_28);

    g_set = lv_button_create(parent);
    lv_obj_set_size(g_set, 150, 52);
    lv_obj_align(g_set, LV_ALIGN_TOP_MID, 0, 336);
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

    g_note = lv_label_create(parent);
    lv_obj_set_style_text_font(g_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_note, lv_color_hex(0x6A737F), 0);
    lv_label_set_text(g_note, "");
    lv_obj_align(g_note, LV_ALIGN_TOP_MID, 0, 398);
}

void clock_update() {
    if (!g_src) return;
    const uint32_t now = g_src->now ? g_src->now() : 0;
    const uint32_t floor_s = g_src->floor ? g_src->floor() : 0;

    // Where to start. The clock if it is running -- then this view is also a
    // live clock, and the wheels sit on the current minute. Otherwise the last
    // moment a previous run persisted, which is effectively the end of the
    // last drive, so a drive a day later is a nudge on two wheels rather than
    // five. Never a guess forward.
    const uint32_t seed = now ? now : floor_s;
    if (!g_touched && seed) {
        seed_from(seed);
        g_seeded = true;
    }

    // The note. What happened, for a few seconds after SET; otherwise where
    // the wheels came from, which is the thing that makes the offset readable.
    if (g_said_ms && (int64_t)lv_tick_get() - g_said_ms < 4000) return;
    g_said_ms = 0;
    char buf[64];
    if (!seed) {
        set_note("no clock yet -- set one and drives get dated");
    } else if (now) {
        const time_t t = (time_t)now;
        struct tm tm_v;
        localtime_r(&t, &tm_v);
        strftime(buf, sizeof buf, "now %d %b %H:%M", &tm_v);
        set_note(buf);
    } else {
        const time_t t = (time_t)floor_s;
        struct tm tm_v;
        localtime_r(&t, &tm_v);
        strftime(buf, sizeof buf, "last drive %d %b %H:%M", &tm_v);
        set_note(buf);
    }
    (void)g_seeded;
}

}  // namespace gauge_ui
