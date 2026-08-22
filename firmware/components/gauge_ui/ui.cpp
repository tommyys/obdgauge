#include "gauge_ui.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "carousel.h"
#include "views.h"

namespace gauge_ui {
namespace {

// 135 positions across a 270-degree sweep: one step is ~2 degrees.
constexpr int kDialSteps = 135;
// One swipe cannot reasonably produce two intended gestures inside this
// window, and LVGL repeats the event every input read (~16ms).
constexpr uint32_t kGestureDebounceMs = 400;
constexpr int kSwipePx = 55;      // shorter than this is a tap, not a swipe

// Views switch instantly rather than sliding. A slide moves the whole
// 466x466 view, which dirties the entire screen; at the measured ~52 ms
// full-screen refresh a 220 ms slide is about four frames and reads as a
// stutter rather than motion. An instant cut is honest about the hardware
// and, on a gauge you glance at, arguably better anyway. Section 6's
// shortest-signed-distance rule is kept below because it still decides which
// view you land on when wrapping.

struct ViewObjs {
    lv_obj_t* root   = nullptr;
    lv_obj_t* title  = nullptr;
    lv_obj_t* hero   = nullptr;
    lv_obj_t* unit   = nullptr;
    lv_obj_t* word   = nullptr;
    lv_obj_t* arc    = nullptr;
    lv_obj_t* rlabel[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* rvalue[4] = {nullptr, nullptr, nullptr, nullptr};
};

const ViewSpec* g_specs = nullptr;
int g_count = 0;
int g_cur = 0;
std::vector<ViewObjs> g_objs;
lv_obj_t* g_parent = nullptr;
lv_obj_t* g_banner = nullptr;
lv_obj_t* g_dots[16] = {nullptr};
lv_obj_t* g_dot_active = nullptr;
char g_banner_base[40] = {0};
bool g_dials_on = true;
int  g_dot_x0 = 0;
int  g_dot_spacing = 18;
bool g_was_pressed = false;
int  g_press_x = 0;
bool g_swipe_done = false;
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

    if (spec.dial.value) {
        v.arc = lv_arc_create(v.root);
        lv_obj_set_size(v.arc, 434, 434);
        lv_obj_center(v.arc);
        lv_arc_set_bg_angles(v.arc, 135, 45);
        lv_obj_remove_style(v.arc, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(v.arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(v.arc, 14, LV_PART_MAIN);
        lv_obj_set_style_arc_width(v.arc, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(v.arc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
        lv_obj_set_style_arc_color(v.arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);
    }

    v.title = mk_label(v.root, &lv_font_montserrat_20, 0x707070, LV_ALIGN_CENTER, 0, -142);
    lv_label_set_text(v.title, spec.title);

    v.hero = mk_label(v.root, &lv_font_montserrat_48, 0xFFFFFF, LV_ALIGN_CENTER, 0, -60);
    v.unit = mk_label(v.root, &lv_font_montserrat_20, 0x9A9A9A, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(v.unit, spec.hero_unit ? spec.hero_unit : "");
    v.word = mk_label(v.root, &lv_font_montserrat_28, 0x808080, LV_ALIGN_CENTER, 0, 22);

    // Rows sit as label/value pairs below the hero, left and right of centre.
    for (int i = 0; i < 4 && spec.rows[i].label; ++i) {
        int y = 66 + i * 27;
        v.rlabel[i] = mk_label(v.root, &lv_font_montserrat_20, 0x606060, LV_ALIGN_CENTER, -78, y);
        v.rvalue[i] = mk_label(v.root, &lv_font_montserrat_20, 0xD0D0D0, LV_ALIGN_CENTER, 62, y);
        lv_label_set_text(v.rlabel[i], spec.rows[i].label);
    }
    return v;
}

void place(int index, int x) { lv_obj_set_pos(g_objs[index].root, x, 0); }

void switch_to(int target) {
    if (target == g_cur || target < 0 || target >= g_count) return;
    // Every view sits at (0,0); only visibility changes. Moving objects instead
    // invalidated the full screen TWICE per switch -- once for the outgoing
    // view's old area, once for the incoming view's -- which at the measured
    // ~52 ms full-frame cost is ~104 ms of dead time on every swipe. A view
    // change has to repaint the whole screen once; it must not pay for it twice.
    lv_obj_add_flag(g_objs[static_cast<size_t>(g_cur)].root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_objs[static_cast<size_t>(target)].root, LV_OBJ_FLAG_HIDDEN);
    g_cur = target;

    if (g_dot_active) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_dot_active);
        lv_anim_set_values(&a, lv_obj_get_x(g_dot_active),
                           lv_obj_get_x(g_dots[0]) + target * g_dot_spacing - 3);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, [](void* obj, int32_t x) {
            lv_obj_set_x(static_cast<lv_obj_t*>(obj), x); });
        lv_anim_start(&a);
    }
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
    int target = gauge::ring_index(g_cur, g_count, dir == LV_DIR_LEFT ? +1 : -1);
    int before = g_cur;
    switch_to(target);
    ++g_gestures;
    (void)before;
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
    lv_obj_add_event_cb(parent, [](lv_event_t*) { ++g_presses; }, LV_EVENT_PRESSED, nullptr);
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
    g_dot_x0 = x0;
    g_dot_spacing = spacing;

    // The make/model banner persists across views (SPEC.md section 10), so it
    // lives on the parent rather than inside any one view.
    g_banner = lv_label_create(parent);
    lv_obj_set_style_text_font(g_banner, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_banner, lv_color_hex(0x585858), 0);
    snprintf(g_banner_base, sizeof g_banner_base, "%s", id.label.c_str());
    lv_label_set_text(g_banner, g_banner_base);
    lv_obj_align(g_banner, LV_ALIGN_CENTER, 0, 178);
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

    set_text_if_changed(v.hero, s.hero(m));

    if (s.state_word) {
        uint32_t colour = 0x808080;
        std::string w = s.state_word(m, &colour);
        if (w != lv_label_get_text(v.word)) {
            lv_label_set_text(v.word, w.c_str());
            lv_obj_set_style_text_color(v.word, lv_color_hex(colour), 0);
        }
    }

    if (v.arc && s.dial.value) {
        double val = 0.0;
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
                lv_arc_set_value(v.arc, q);
            }
            if (g_dials_on) lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            // No reading: hide the dial rather than draw it pinned at zero.
            lv_obj_add_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        }
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
    return (g_cur >= 0 && g_cur < g_count) ? g_specs[g_cur].title : "";
}

}  // namespace gauge_ui
