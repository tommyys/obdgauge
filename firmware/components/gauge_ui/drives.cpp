#include "drives.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace gauge_ui {
namespace {

// Rows built up front and reused. The ring can hold more drives than this,
// but a list is read, not paged through for ever -- and every row is LVGL
// objects held for the life of the run, which is memory the panel wants more
// (SPEC.md section 3: nothing in the draw path may allocate, so rows are not
// created and destroyed as the list changes).
constexpr int kMaxRows = 12;
constexpr int kRowH    = 88;

const DrivesSource* g_src = nullptr;

struct Row {
    lv_obj_t* box   = nullptr;
    lv_obj_t* when  = nullptr;   // "29 Aug 11:24"
    lv_obj_t* dur   = nullptr;   // "27 min"
    lv_obj_t* stats = nullptr;   // "12.4 km   3532 rpm   90 km/h"
};

lv_obj_t* g_list = nullptr;
lv_obj_t* g_note = nullptr;      // the empty/unavailable line
Row       g_rows[kMaxRows];

// The card: the same four numbers, big. -1 when the list is showing.
int       g_open = -1;
lv_obj_t* g_card = nullptr;
lv_obj_t* g_card_when = nullptr;
lv_obj_t* g_card_dur = nullptr;
lv_obj_t* g_card_dist = nullptr;
lv_obj_t* g_card_rpm = nullptr;
lv_obj_t* g_card_speed = nullptr;

lv_obj_t* mk(lv_obj_t* parent, const lv_font_t* font, uint32_t colour,
             lv_align_t align, int dx, int dy) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, dx, dy);
    return l;
}

void set_text_if_changed(lv_obj_t* l, const char* s) {
    if (!l) return;
    const char* cur = lv_label_get_text(l);
    if (cur && !strcmp(cur, s)) return;      // a redraw LVGL does not have to do
    lv_label_set_text(l, s);
}

// "29 Aug 11:24", or the honest answer when no clock was ever set. A drive
// recorded with epoch 0 is not dated later (SPEC.md section 15) -- saying so
// beats inventing a date that would then be replayed as fact.
void fmt_when(char* out, size_t n, uint32_t epoch_s) {
    if (!epoch_s) { snprintf(out, n, "date unknown"); return; }
    const time_t t = (time_t)epoch_s;
    struct tm tm_v;
    localtime_r(&t, &tm_v);
    strftime(out, n, "%d %b %H:%M", &tm_v);
}

void fmt_dur(char* out, size_t n, uint32_t ms) {
    const uint32_t s = ms / 1000;
    if (s < 60)   { snprintf(out, n, "%u s", (unsigned)s); return; }
    if (s < 3600) { snprintf(out, n, "%u min", (unsigned)(s / 60)); return; }
    snprintf(out, n, "%uh %02um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
}

void row_clicked(lv_event_t* e) {
    g_open = (int)(intptr_t)lv_event_get_user_data(e);
}

void card_clicked(lv_event_t*) { g_open = -1; }

}  // namespace

void drives_set_source(const DrivesSource* src) { g_src = src; }

void drives_build(lv_obj_t* parent) {
    // The panel's width, not the parent's. drives_build runs while the
    // carousel is still being built, before LVGL has laid anything out, so
    // lv_obj_get_width(parent) answers 0 -- and every row then came out
    // 120 pixels narrower than nothing, which draws as an empty screen with
    // the right text in it. The other views never hit this because they place
    // everything by alignment rather than by measured size.
    const int w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());

    // The one place in this firmware that scrolls. ui.cpp takes
    // LV_OBJ_FLAG_SCROLLABLE off everything on purpose: when LVGL decides a
    // drag is a scroll it swallows the gesture, and the carousel's swipe IS
    // that gesture. Restricted to the vertical axis, a drag up and down moves
    // the list while a drag left and right is left alone to change view.
    g_list = lv_obj_create(parent);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_size(g_list, w, w);
    lv_obj_center(g_list);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    // Round panel: the first and last rows would otherwise sit in the corners
    // that the glass does not have.
    lv_obj_set_style_pad_top(g_list, 76, 0);
    lv_obj_set_style_pad_bottom(g_list, 76, 0);

    for (int i = 0; i < kMaxRows; ++i) {
        Row& r = g_rows[i];
        r.box = lv_obj_create(g_list);
        lv_obj_remove_style_all(r.box);
        lv_obj_set_size(r.box, w - 120, kRowH);
        lv_obj_align(r.box, LV_ALIGN_TOP_MID, 0, 76 + i * kRowH);
        lv_obj_remove_flag(r.box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r.box, row_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        // A hairline under each row: without it the two text lines of one
        // drive read as one paragraph with the drive below it.
        lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r.box, 1, 0);
        lv_obj_set_style_border_color(r.box, lv_color_hex(0x303030), 0);
        lv_obj_set_style_border_opa(r.box, LV_OPA_COVER, 0);

        r.when  = mk(r.box, &lv_font_montserrat_20, 0xF0F0F0, LV_ALIGN_TOP_LEFT, 0, 6);
        r.dur   = mk(r.box, &lv_font_montserrat_20, 0x909090, LV_ALIGN_TOP_RIGHT, 0, 6);
        r.stats = mk(r.box, &lv_font_montserrat_14, 0x9090A0, LV_ALIGN_TOP_LEFT, 0, 38);
        lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    }

    g_note = mk(parent, &lv_font_montserrat_20, 0x808080, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(g_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_note, 300);
    lv_obj_set_style_text_align(g_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(g_note, LV_OBJ_FLAG_HIDDEN);

    // The card. Same four numbers, one screen, tap anywhere to go back.
    g_card = lv_obj_create(parent);
    lv_obj_remove_style_all(g_card);
    lv_obj_set_size(g_card, w, w);
    lv_obj_center(g_card);
    lv_obj_remove_flag(g_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_card, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_card, card_clicked, LV_EVENT_CLICKED, nullptr);
    g_card_when  = mk(g_card, &lv_font_montserrat_20, 0xB0B0B0, LV_ALIGN_CENTER, 0, -122);
    g_card_dur   = mk(g_card, &lv_font_montserrat_14, 0x808080, LV_ALIGN_CENTER, 0, -94);
    g_card_dist  = mk(g_card, &lv_font_montserrat_48, 0xF0F0F0, LV_ALIGN_CENTER, 0, -40);
    g_card_rpm   = mk(g_card, &lv_font_montserrat_28, 0xE0E0E0, LV_ALIGN_CENTER, 0, 40);
    g_card_speed = mk(g_card, &lv_font_montserrat_28, 0xE0E0E0, LV_ALIGN_CENTER, 0, 96);
    lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);
}

void drives_update() {
    if (!g_list) return;

    const int n = g_src ? g_src->count() : 0;


    // The card, when one is open. Its row may have rolled off the ring while
    // it was open -- the list is the authority, so fall back to it rather than
    // keep showing a drive the board no longer holds.
    DriveRowInfo info;
    const bool card_ok = g_open >= 0 && g_src && g_src->row(g_open, &info);
    if (g_open >= 0 && !card_ok) g_open = -1;

    if (g_open >= 0) {
        lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_note, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_card, LV_OBJ_FLAG_HIDDEN);

        char buf[48];
        fmt_when(buf, sizeof buf, info.epoch_s);
        set_text_if_changed(g_card_when, buf);
        fmt_dur(buf, sizeof buf, info.stats.duration_ms);
        if (!info.complete) {
            char note[64];
            snprintf(note, sizeof note, "%s  (unfinished)", buf);
            set_text_if_changed(g_card_dur, note);
        } else {
            set_text_if_changed(g_card_dur, buf);
        }

        if (!info.table_ok) {
            set_text_if_changed(g_card_dist, "--");
            set_text_if_changed(g_card_rpm, "recorded by other firmware");
            set_text_if_changed(g_card_speed, "");
        } else if (!info.ready) {
            set_text_if_changed(g_card_dist, "...");
            set_text_if_changed(g_card_rpm, "reading");
            set_text_if_changed(g_card_speed, "");
        } else {
            snprintf(buf, sizeof buf, "%.1f km", info.stats.distance_km);
            set_text_if_changed(g_card_dist, buf);
            snprintf(buf, sizeof buf, "%.0f rpm peak", info.stats.peak_rpm);
            set_text_if_changed(g_card_rpm, buf);
            snprintf(buf, sizeof buf, "%.0f km/h peak", info.stats.peak_kph);
            set_text_if_changed(g_card_speed, buf);
        }
        return;
    }

    lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_list, LV_OBJ_FLAG_HIDDEN);

    if (n <= 0) {
        const char* why = g_src && g_src->empty_note() ? g_src->empty_note()
                                                       : "no drives recorded yet";
        set_text_if_changed(g_note, why);
        lv_obj_clear_flag(g_note, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < kMaxRows; ++i) lv_obj_add_flag(g_rows[i].box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(g_note, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < kMaxRows; ++i) {
        Row& r = g_rows[i];
        DriveRowInfo d;
        if (i >= n || !g_src->row(i, &d)) {
            lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(r.box, LV_OBJ_FLAG_HIDDEN);

        char buf[64];
        fmt_when(buf, sizeof buf, d.epoch_s);
        set_text_if_changed(r.when, buf);
        fmt_dur(buf, sizeof buf, d.stats.duration_ms);
        set_text_if_changed(r.dur, buf);

        if (!d.table_ok) {
            set_text_if_changed(r.stats, "recorded by other firmware");
        } else if (!d.ready) {
            set_text_if_changed(r.stats, "reading...");
        } else {
            snprintf(buf, sizeof buf, "%.1f km   %.0f rpm   %.0f km/h%s",
                     d.stats.distance_km, d.stats.peak_rpm, d.stats.peak_kph,
                     d.complete ? "" : "   unfinished");
            set_text_if_changed(r.stats, buf);
        }
    }
}

}  // namespace gauge_ui
