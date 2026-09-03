#include "clock.h"

namespace gauge_ui {
namespace {

const ClockSource* g_src = nullptr;

}  // namespace

void clock_set_source(const ClockSource* src) { g_src = src; }

uint32_t clock_now() {
    return (g_src && g_src->now) ? g_src->now() : 0;
}

}  // namespace gauge_ui
