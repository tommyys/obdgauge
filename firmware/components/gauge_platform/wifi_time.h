// The board's half of setting the clock: WiFi station, a time server, and then
// getting out of the way.
//
// Why this is a task and not a call: joining a network takes seconds and the
// gauge's boot has nothing to spare on the screen. It runs during display
// bring-up, which is dead time anyway, and reports back through a callback.
//
// Why it shuts itself down: WiFi and BLE share one 2.4 GHz radio and, worse,
// one pool of internal RAM. That pool is already fought over by the panel's
// DMA and the BT controller's contiguous 30,720-byte block -- see
// BleTransport::radio_init() for what happens when it loses. Once the clock is
// set, WiFi has no further job on that drive, so it hands both back. main.cpp
// holds the OBD scan until busy() goes false, so the two radios never transmit
// at the same time.
//
// The policy -- order, budgets, retries, what counts as a believable clock --
// is in gauge_core/wifi_plan.h and is host-tested. This file only does what a
// radio is needed for.
#pragma once
#include <cstddef>
#include <cstdint>

#include "wifi_plan.h"

namespace gauge_platform {
namespace wifi_time {

struct Config {
    gauge::WifiPlan::Config plan;
    // What the time server gets, once a network is joined. Short: an answer
    // is one small exchange, and a server that has not replied in this long
    // is not going to.
    int sntp_timeout_ms = 6000;
    // Given the wall clock, once, when a believable one arrives.
    void (*on_time)(uint32_t epoch_s) = nullptr;
    // Given every line worth keeping. main.cpp points this at flight_log, so
    // an unattended run leaves a record -- the whole point, since the sync
    // happens in a car with nothing plugged into it.
    void (*log)(const char* line) = nullptr;
};

// Starts the sync task and returns immediately. `nets` must outlive it, which
// is why main.cpp holds the list in static storage. Doing nothing when n is 0
// is deliberate: a fresh clone has no wifi_creds.h and must still boot.
void start(const gauge::WifiNetwork* nets, std::size_t n, const Config& cfg);

// Block until the sync task has finished and released the radio, or until
// max_ms. Called from app_main, because the memory WiFi holds is memory the
// display and the BT controller cannot start without -- see wifi_time.cpp.
void wait(int max_ms);

// True while the task is still working. False before it starts and once it
// has finished, so main.cpp's `!busy()` gate does not deadlock on a build
// with no networks.
bool busy();

// The clock it found, or 0.
uint32_t epoch();

// One line for the serial log and the WIFI command.
const char* status();

// There is deliberately no "keep WiFi up" flag.
//
// It was designed, and the board killed it on 2026-09-03. Measured: the
// station holds 30,252 bytes of internal RAM while it is up (132,987 free
// before, 102,735 after), and the display's own 14,912-byte contiguous buffer
// then cannot be had -- bsp_display_start() asserts and the gauge boot-loops.
// So WiFi and the panel cannot both hold memory on this board: there is
// nothing to trade off and nothing to measure. The radio comes up early and is
// gone before the screen exists.
//
// Linking the stack in costs about 17 KB of internal RAM permanently, radio or
// no radio (net80211 12,350 + lwip 3,823 + esp_wifi 886, from
// `idf.py size-components`). That alone was enough to stop the console task's
// 12 KB stack being allocatable -- see serial_cmd.h. The WiFi and lwip
// settings in sdkconfig.defaults are all footprint, not throughput.

}  // namespace wifi_time
}  // namespace gauge_platform
