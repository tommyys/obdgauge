// The WiFi time-sync policy, off the board.
//
// What is worth testing here is not "does WiFi work" -- that needs a radio --
// but the rules that decide how long the gauge is willing to sit there trying.
// Those rules protect two things: the OBD link, which waits for this to
// finish, and the drive stamps, which a bad answer would ruin permanently.
#include "check.h"
#include "wifi_plan.h"

using gauge::WifiPlan;
using gauge_test::check;

namespace {

WifiPlan::Config quick() {
    WifiPlan::Config c;
    c.per_network_ms = 4000;
    c.boot_budget_ms = 8000;
    c.keep_up = false;
    c.retry_period_ms = 60000;
    return c;
}

}  // namespace

int main() {
    // --- the list is tried in order, and then AGAIN --------------------------
    // Hotspot first, home second: the order in the list is the order on the
    // air, because the hotspot is the one present wherever the car is.
    //
    // Reaching the end of the list is not the end of the boot. Measured at
    // home on 2026-09-04 at -82 dBm, the home network failed its WPA
    // handshake 3.5 s into a 14 s budget and the gauge gave up with 8 s
    // unused -- the same network having joined cleanly at -66 dBm minutes
    // before. At a marginal signal one failure says almost nothing about the
    // next attempt, so the budget goes on trying again.
    {
        WifiPlan p(2, quick());
        check("first ask offers network 0", p.next(0), 0);
        p.failed(100);
        check("after a failure it offers network 1", p.next(100), 1);
        p.failed(200);
        check("with the list exhausted it starts over", p.next(200), 0);
        check("and the radio is still wanted", p.radio_wanted(200), true);
        p.failed(300);
        check("second lap offers network 1 again", p.next(300), 1);
    }

    // --- but only while an attempt could still finish -----------------------
    // The floor is what stops the radio being held for a try that cannot get
    // anywhere: below min_attempt_ms a network cannot associate, let alone
    // lease an address.
    {
        WifiPlan p(2, quick());          // 8 s budget, 2.5 s floor
        p.next(0);
        p.failed(3000);
        check("network 1 at 3 s", p.next(3000), 1);
        p.failed(6000);
        check("2 s left is not enough for another lap",
              p.next(6000), (int)WifiPlan::kDone);
        check("so the radio goes back", p.radio_wanted(6000), false);
    }

    // --- a success stops everything ----------------------------------------
    {
        WifiPlan p(2, quick());
        check("network 0 offered", p.next(0), 0);
        p.succeeded(500);
        check("synced", p.synced(), true);
        check("nothing else is tried", p.next(500), (int)WifiPlan::kDone);
        check("the radio is handed back", p.radio_wanted(500), false);
    }

    // --- the boot budget outranks the list ---------------------------------
    // Three networks at 4 s each would be 12 s, and the OBD link is waiting.
    // The third must never be reached.
    {
        WifiPlan p(3, quick());
        check("network 0 offered at 0 ms", p.next(0), 0);
        p.failed(4000);
        check("network 1 offered at 4 s", p.next(4000), 1);
        p.failed(8000);
        check("network 2 is never tried", p.next(8000), (int)WifiPlan::kDone);
    }

    // --- an attempt cannot overrun the budget ------------------------------
    // The clamp is what makes the budget a real ceiling rather than a wish:
    // without it a 4 s attempt started at 7 s would run to 11 s.
    {
        WifiPlan p(3, quick());
        p.next(0);
        check("a fresh attempt gets its full 4 s", p.attempt_timeout_ms(0), 4000);
        p.failed(7000);
        p.next(7000);
        check("an attempt at 7 s gets only the last second",
              p.attempt_timeout_ms(7000), 1000);
    }

    // --- keep_up waits, then starts the list again -------------------------
    {
        WifiPlan::Config c = quick();
        c.keep_up = true;
        WifiPlan p(2, c);
        p.next(0);
        p.failed(4000);
        p.next(4000);
        p.failed(8000);
        check("the boot pass is over", p.next(8000), (int)WifiPlan::kWait);
        check("but the radio stays up", p.radio_wanted(8000), true);
        check("still waiting a second later", p.next(9000), (int)WifiPlan::kWait);
        check("a new pass starts after the retry period",
              p.next(8000 + 60000), 0);
        check("and a retry pass is not capped by the boot budget",
              p.attempt_timeout_ms(8000 + 60000), 4000);
    }

    // --- no networks compiled in -------------------------------------------
    // A fresh clone has no wifi_creds.h. It must build, boot, and never bring
    // a radio up looking for a network it was never given.
    {
        WifiPlan p(0, quick());
        check("nothing to do", p.next(0), (int)WifiPlan::kDone);
        check("the radio is never asked for", p.radio_wanted(0), false);
    }

    // --- a believable clock -------------------------------------------------
    // The stamp on a drive is permanent and unverifiable afterwards, so an
    // answer that cannot be true is dropped rather than recorded.
    {
        check("2026-09-03 is believable", gauge::wifi_epoch_believable(1772500000u), true);
        check("the epoch itself is not",   gauge::wifi_epoch_believable(0u), false);
        check("1970 is not",               gauge::wifi_epoch_believable(86400u), false);
        check("2020 is not",               gauge::wifi_epoch_believable(1600000000u), false);
        check("2026-01-01 is the floor",   gauge::wifi_epoch_believable(1767225600u), true);
        check("a second before it is not", gauge::wifi_epoch_believable(1767225599u), false);
        check("2040 is not",               gauge::wifi_epoch_believable(2200000000u), false);
    }

    return gauge_test::check_report();
}
