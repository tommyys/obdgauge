#include "wifi_plan.h"

namespace gauge {
namespace {

// 2026-01-01 and 2036-01-01 UTC. Written out rather than computed so that the
// numbers a rejected answer is compared against are readable in a log.
constexpr uint32_t kEpochMin = 1767225600u;
constexpr uint32_t kEpochMax = 2082758400u;

// Stands in for "no cap" on the retry passes. Larger than any budget worth
// expressing and small enough that arithmetic on it cannot wrap.
constexpr int64_t kNoCap = 1LL << 40;

}  // namespace

bool wifi_epoch_believable(uint32_t epoch_s) {
    return epoch_s >= kEpochMin && epoch_s < kEpochMax;
}

WifiPlan::WifiPlan(std::size_t network_count, const Config& cfg)
    : n_(network_count), cfg_(cfg) {}

int64_t WifiPlan::remaining_ms(int64_t now_ms) const {
    // Only the boot pass is capped as a whole. Once keep_up has taken the
    // plan past that pass the gauge is already running, nothing is waiting on
    // the radio, and each network simply gets its own timeout.
    if (!first_pass_) return kNoCap;
    if (pass_start_ms_ < 0) return cfg_.boot_budget_ms;
    return cfg_.boot_budget_ms - (now_ms - pass_start_ms_);
}

bool WifiPlan::pass_over(int64_t now_ms) const {
    return idx_ >= static_cast<int>(n_) || remaining_ms(now_ms) <= 0;
}

int WifiPlan::next(int64_t now_ms) {
    if (synced_ || n_ == 0) return kDone;
    // The budget starts when the plan is first asked, not when it is built:
    // the caller builds it before the task is scheduled, and charging the
    // gap to the boot budget would spend it on nothing.
    if (pass_start_ms_ < 0) pass_start_ms_ = now_ms;

    if (!pass_over(now_ms)) return idx_;

    // Every network was tried, or the budget ran out.
    if (!cfg_.keep_up) return kDone;
    if (pass_end_ms_ < 0) pass_end_ms_ = now_ms;
    if (now_ms - pass_end_ms_ < cfg_.retry_period_ms) return kWait;

    idx_ = 0;
    first_pass_ = false;
    pass_start_ms_ = now_ms;
    pass_end_ms_ = -1;
    return idx_;
}

int WifiPlan::attempt_timeout_ms(int64_t now_ms) const {
    const int64_t left = remaining_ms(now_ms);
    if (left < cfg_.per_network_ms) return static_cast<int>(left);
    return cfg_.per_network_ms;
}

void WifiPlan::failed(int64_t) { ++idx_; }

void WifiPlan::succeeded(int64_t) { synced_ = true; }

bool WifiPlan::radio_wanted(int64_t now_ms) const {
    if (n_ == 0) return false;
    if (cfg_.keep_up) return true;
    if (synced_) return false;
    if (pass_start_ms_ < 0) return true;  // not started yet
    return !pass_over(now_ms);
}

}  // namespace gauge
