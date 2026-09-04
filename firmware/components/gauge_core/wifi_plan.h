// Deciding when to ask the network for the time, with no radio in the room.
//
// The gauge has no real-time clock: 0x51 is named in the board's I2C table but
// the part is not fitted (gauge_ui/clock.h, checked 2026-08-31). So every boot
// starts not knowing the time, and a drive recorded without one is stamped
// "date unknown" for ever -- which is what happened to every drive recorded so
// far. WiFi is how that stops depending on somebody remembering.
//
// The policy of it -- which network to try, how long each one gets, when to
// give up, when to try again -- involves no ESP-IDF and no radio, so it lives
// here and is proven on the Mac (test_wifi_plan.cpp). What is left in
// gauge_platform/wifi_time.cpp is only the part that cannot be tested there.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gauge {

// One network to try. The list is tried in order, so the hotspot goes first:
// it is the one that is present wherever the car is.
struct WifiNetwork {
    const char* ssid;
    const char* pass;
};

// Would this be a believable wall clock for this gauge?
//
// A time server that answers with rubbish must not be allowed to stamp a
// drive. The stamp is permanent and there is nothing to check it against
// afterwards, so a wrong answer is worse than no answer -- "date unknown" is
// at least honest. 2026 is the year the gauge was built, so anything earlier
// is wrong by construction; the upper bound is a decade out, well short of
// where a signed 32-bit epoch runs out in 2038.
bool wifi_epoch_believable(uint32_t epoch_s);

// The order of attempts and the clock they run against. Holds no radio state:
// the caller does the joining and reports back what happened.
class WifiPlan {
  public:
    struct Config {
        // What one network gets before it is written off.
        //
        // 9000, not the 5000 it was. Measured at home on 2026-09-04, at -74
        // dBm: the gauge ASSOCIATES with the router 1.7 s into the attempt and
        // then needs several more seconds for DHCP to hand it an address. At
        // 5 s it was cut off mid-lease about half the time -- "found but gave
        // us no address in time" -- while 2.7 s of the boot budget below sat
        // unused. Associating is quick and getting an address is not, and the
        // old number only allowed for the quick half.
        int per_network_ms = 9000;
        // What the whole boot attempt gets. It exists because this runs inside
        // the gauge's boot and the OBD link waits for it to finish (main.cpp),
        // so that the two radios never transmit at once. An unbounded search
        // would keep the car waiting.
        // 14000, not 11000. The whole sequence at a weak signal is
        // associate + DHCP + two SNTP servers, and a successful boot measured
        // 6.9 s of it with nothing to spare. The 3 s is only ever spent on a
        // boot where no network answers at all, which is the boot that already
        // has nothing to wait for.
        int boot_budget_ms = 14000;
        // False: hand the radio back the moment the clock is set, or the boot
        // budget runs out. True: stay up and keep trying, which is what a
        // hotspot switched on after the engine started needs.
        //
        // Off by default because WiFi holds internal RAM, and internal RAM is
        // what the panel's DMA and the BT controller are already competing
        // for -- exhausting it is what broke the BLE link on 2026-08-28 and
        // what froze the panel before that.
        bool keep_up = false;
        // How long to wait between passes when keep_up is set.
        int retry_period_ms = 60000;
    };

    // Returned by next() in place of an index.
    enum : int {
        kDone = -2,  // nothing more will be attempted; the radio can go
        kWait = -1,  // nothing to do yet; ask again shortly
    };

    WifiPlan(std::size_t network_count, const Config& cfg);

    // The network to try now, or kWait / kDone.
    int next(int64_t now_ms);

    // How long the attempt just handed out is allowed to take. Never longer
    // than what is left of the boot budget, so the last network in a list
    // cannot overrun it.
    int attempt_timeout_ms(int64_t now_ms) const;

    // Report back on the attempt next() handed out.
    void failed(int64_t now_ms);
    void succeeded(int64_t now_ms);

    bool synced() const { return synced_; }

    // False once the caller should shut the radio down and give the memory
    // back. Separate from kDone because with keep_up set the plan stops
    // asking but the link stays up.
    bool radio_wanted(int64_t now_ms) const;

  private:
    int64_t remaining_ms(int64_t now_ms) const;
    bool    pass_over(int64_t now_ms) const;

    std::size_t n_;
    Config      cfg_;
    int         idx_ = 0;
    bool        synced_ = false;
    bool        first_pass_ = true;
    int64_t     pass_start_ms_ = -1;
    int64_t     pass_end_ms_ = -1;
};

}  // namespace gauge
