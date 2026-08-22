#include "gauge_ui.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "views.h"

namespace gauge_ui {
namespace {

constexpr int kSwipePx = 55;      // shorter than this is a tap, not a swipe
constexpr int kSlideMs = 220;

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
bool g_was_pressed = false;
int  g_press_x = 0;
bool g_sliding = false;

int screen_w() { return lv_obj_get_width(g_parent); }

void slide_done(lv_anim_t*) { g_sliding = false; }

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

// Section 6: each view is placed at its shortest signed distance from the
// current one, so the last-to-first step is one slide rather than a rewind of
// the whole strip.
int signed_distance(int from, int to, int n) {
    int d = to - from;
    while (d >  n / 2) d -= n;
    while (d < -n / 2) d += n;
    return d;
}

void animate_to(int target) {
    if (g_sliding || target == g_cur) return;
    int n = g_count;
    int dir = signed_distance(g_cur, target, n) > 0 ? 1 : -1;
    int w = screen_w();

    place(target, dir * w);
    lv_obj_clear_flag(g_objs[target].root, LV_OBJ_FLAG_HIDDEN);

    g_sliding = true;
    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, g_objs[g_cur].root);
    lv_anim_set_values(&out, 0, -dir * w);
    lv_anim_set_duration(&out, kSlideMs);
    lv_anim_set_exec_cb(&out, [](void* obj, int32_t x) {
        lv_obj_set_pos(static_cast<lv_obj_t*>(obj), x, 0); });
    lv_anim_start(&out);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, g_objs[target].root);
    lv_anim_set_values(&in, dir * w, 0);
    lv_anim_set_duration(&in, kSlideMs);
    lv_anim_set_exec_cb(&in, [](void* obj, int32_t x) {
        lv_obj_set_pos(static_cast<lv_obj_t*>(obj), x, 0); });
    lv_anim_set_completed_cb(&in, slide_done);
    lv_anim_start(&in);

    g_cur = target;
}

}  // namespace

void init(lv_obj_t* parent, const gauge::Identity& id) {
    g_parent = parent;
    g_specs = view_table(&g_count);
    g_objs.clear();
    g_objs.reserve(static_cast<size_t>(g_count));
    for (int i = 0; i < g_count; ++i) g_objs.push_back(build_view(g_specs[i]));
    for (int i = 0; i < g_count; ++i) place(i, i == 0 ? 0 : screen_w());
    g_cur = 0;

    // The make/model banner persists across views (SPEC.md section 10), so it
    // lives on the parent rather than inside any one view.
    g_banner = lv_label_create(parent);
    lv_obj_set_style_text_font(g_banner, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_banner, lv_color_hex(0x585858), 0);
    lv_label_set_text(g_banner, id.label.c_str());
    lv_obj_align(g_banner, LV_ALIGN_CENTER, 0, 178);
}

void update(const Model& m) {
    if (g_cur < 0 || g_cur >= g_count) return;
    const ViewSpec& s = g_specs[g_cur];
    ViewObjs& v = g_objs[static_cast<size_t>(g_cur)];

    lv_label_set_text(v.hero, s.hero(m).c_str());

    if (s.state_word) {
        uint32_t colour = 0x808080;
        std::string w = s.state_word(m, &colour);
        lv_label_set_text(v.word, w.c_str());
        lv_obj_set_style_text_color(v.word, lv_color_hex(colour), 0);
    }

    if (v.arc && s.dial.value) {
        double val = 0.0;
        if (s.dial.value(m, &val)) {
            int lo = static_cast<int>(s.dial.lo(m));
            int hi = static_cast<int>(s.dial.hi(m));
            if (hi > lo) {
                lv_arc_set_range(v.arc, lo, hi);
                lv_arc_set_value(v.arc, static_cast<int>(val));
            }
            lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            // No reading: hide the dial rather than draw it pinned at zero.
            lv_obj_add_flag(v.arc, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 4 && s.rows[i].label; ++i) {
        lv_label_set_text(v.rvalue[i], s.rows[i].value(m).c_str());
    }
}

void handle_touch(lv_indev_t* indev) {
    if (!indev) return;
    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p{};
    lv_indev_get_point(indev, &p);

    if (pressed && !g_was_pressed) {
        g_press_x = p.x;
    } else if (!pressed && g_was_pressed) {
        int dx = p.x - g_press_x;
        if (dx <= -kSwipePx)      animate_to((g_cur + 1) % g_count);
        else if (dx >= kSwipePx)  animate_to((g_cur - 1 + g_count) % g_count);
    }
    g_was_pressed = pressed;
}

int view_count() { return g_count; }
int current_view() { return g_cur; }
const char* current_view_name() {
    return (g_cur >= 0 && g_cur < g_count) ? g_specs[g_cur].title : "";
}

}  // namespace gauge_ui
