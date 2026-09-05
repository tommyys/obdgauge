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
// Between the drive's name line and its numbers line, inside one row.
constexpr int kLineGap = 7;
// The hairline under each row is part of the row's own 88 px, so the text has
// 87 to be centred in.
constexpr int kRowRule = 1;

const DrivesSource* g_src = nullptr;

struct Row {
    lv_obj_t* box   = nullptr;
    lv_obj_t* when  = nullptr;   // "29 Aug 11:24"
    lv_obj_t* dur   = nullptr;   // "27 min"
    lv_obj_t* stats = nullptr;   // "12.4 km   3532 rpm   90 km/h"
};

// What a row's three strings were built from. Formatting them costs a
// localtime_r, a strftime and three float conversions, and drives_update()
// runs on EVERY frame -- measured at 2.33 ms of a 15 ms frame, for an answer
// that changes when a drive is folded and at no other time. Compared first,
// formatted only on a difference.
struct RowKey {
    uint32_t id = 0xFFFFFFFFu;
    uint32_t epoch_s = 0;
    uint32_t duration_ms = 0;
    float    km = 0, rpm = 0, kph = 0;
    bool     ready = false, table_ok = false, used = false;
    bool same(const DriveRowInfo& d) const {
        return used && id == d.id && epoch_s == d.epoch_s &&
               duration_ms == d.stats.duration_ms && ready == d.ready &&
               table_ok == d.table_ok && km == d.stats.distance_km &&
               rpm == d.stats.peak_rpm && kph == d.stats.peak_kph;
    }
    void take(const DriveRowInfo& d) {
        id = d.id; epoch_s = d.epoch_s; duration_ms = d.stats.duration_ms;
        km = d.stats.distance_km; rpm = d.stats.peak_rpm; kph = d.stats.peak_kph;
        ready = d.ready; table_ok = d.table_ok; used = true;
    }
};

lv_obj_t* g_list = nullptr;
lv_obj_t* g_note = nullptr;      // the empty/unavailable line
Row       g_rows[kMaxRows];
RowKey    g_keys[kMaxRows];
RowKey    g_card_key;

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


bool drives_scroll_by(int dy) {
    if (!g_list) return false;
    lv_obj_scroll_by(g_list, 0, dy, LV_ANIM_OFF);
    return true;
}

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
    // As wide as its rows and no wider. A scroll invalidates the whole of this
    // object's area every frame, and at 466 wide that was the entire panel --
    // including 120 px of black margin either side of the rows that never has
    // anything in it to move. Nothing looks different; the frame is smaller.
    lv_obj_set_size(g_list, w - 120, w);
    lv_obj_center(g_list);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    // Round panel: the first and last rows would otherwise sit in the corners
    // that the glass does not have.
    lv_obj_set_style_pad_top(g_list, 76, 0);
    lv_obj_set_style_pad_bottom(g_list, 76, 0);
    // Opaque, though the screen behind it is the same black. A transparent
    // object has to be composed over whatever is under it, in every band of
    // every frame of a scroll; an opaque one is drawn straight. Nothing looks
    // different.
    lv_obj_set_style_bg_color(g_list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);

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
        // Opaque for the same reason as the list itself.
        lv_obj_set_style_bg_color(r.box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(r.box, LV_OPA_COVER, 0);

        // Centred in the row, not hung from its top. The two lines were at 6
        // and 38 of an 88 px row, which left 6 px above the text and 32 below
        // it -- so every row looked as though it belonged to the hairline under
        // it rather than sitting between two of them.
        //
        // Measured from the fonts rather than typed in, so the block stays
        // centred if either font ever changes size.
        const int h_top = (int)lv_font_get_line_height(&lv_font_montserrat_20);
        const int h_bot = (int)lv_font_get_line_height(&lv_font_montserrat_14);
        const int y_top = (kRowH - kRowRule - h_top - kLineGap - h_bot) / 2;
        const int y_bot = y_top + h_top + kLineGap;

        r.when  = mk(r.box, &lv_font_montserrat_20, 0xF0F0F0, LV_ALIGN_TOP_LEFT, 0, y_top);
        r.dur   = mk(r.box, &lv_font_montserrat_20, 0x909090, LV_ALIGN_TOP_RIGHT, 0, y_top);
        r.stats = mk(r.box, &lv_font_montserrat_14, 0x9090A0, LV_ALIGN_TOP_LEFT, 0, y_bot);
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
    // First, because it is what lets the board refresh the list at all -- see
    // DrivesSource::watching. Before the early returns below, so a view that
    // has nothing to redraw still counts as being looked at.
    if (g_src && g_src->watching) g_src->watching();

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
        if (g_card_key.same(info)) return;    // nothing to re-format
        g_card_key.take(info);

        char buf[48];
        fmt_when(buf, sizeof buf, info.epoch_s);
        set_text_if_changed(g_card_when, buf);
        fmt_dur(buf, sizeof buf, info.stats.duration_ms);
        set_text_if_changed(g_card_dur, buf);

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
            g_keys[i].used = false;
            continue;
        }
        lv_obj_clear_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        if (g_keys[i].same(d)) continue;      // nothing to re-format
        g_keys[i].take(d);

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
            // No "unfinished" marker. A drive missing its end marker -- the
            // gauge unplugged before the 20 s of silence that closes one --
            // still holds every record it recorded, so the word said nothing
            // about the drive and only invited the question.
            snprintf(buf, sizeof buf, "%.1f km   %.0f rpm   %.0f km/h",
                     d.stats.distance_km, d.stats.peak_rpm, d.stats.peak_kph);
            set_text_if_changed(r.stats, buf);
        }
    }
}

}  // namespace gauge_ui
