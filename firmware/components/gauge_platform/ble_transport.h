// The board's half of the ELM327 link: NimBLE bytes behind gauge::ITransport.
//
// gauge_core's Elm327 does not know what a radio is -- it writes strings and
// reads replies up to the '>' prompt (transport.h). The Mac fills that seam
// with bleak (mx5gauge/sources.py); this fills it with a NimBLE GATT client, so
// the handshake proven by the host tests is byte-for-byte the one that runs in
// the car.
//
// The adapter is a Nordic-UART-style device: one characteristic to write
// commands into, one that notifies replies back. That is what the vLinker
// exposes and what sources.py has always talked to.
#pragma once
#include <string>
#include "transport.h"

namespace gauge_platform {

class BleTransport : public gauge::ITransport {
  public:
    // Brings up the controller, scans for an adapter whose advertised name
    // contains `name_hint` (case-insensitive), connects, and subscribes to its
    // notify characteristic. Blocks until that whole chain is up, or until
    // timeout_ms elapses. Safe to call again after a failure -- the radio is
    // only initialised once.
    bool connect(const char* name_hint, int timeout_ms = 20000);

    // Brings up the controller and the NimBLE host WITHOUT scanning, and
    // returns whether it worked. Call this from app_main before the display
    // starts; connect() will then find the radio already up.
    //
    // It exists purely because of *when* the controller allocates. It asks the
    // internal heap for one contiguous 30,720-byte block, and internal RAM is
    // the only place it can come from. By the time the old code called
    // connect() -- 10 s in, after bsp_display_start() and after the recorder's
    // two task stacks (12 KB + 8 KB) -- the heap had 33,059 bytes free but its
    // largest hole was 21,504, so the allocation failed with the heap looking
    // healthy. Worse, the controller's failure path asserts ("BLE assert
    // emi.c 164") and reboots the gauge, so the gauge boot-looped rather than
    // simply running without a radio. Measured on the board 2026-08-28; the
    // `ui:` line prints both the total and the largest block for exactly this
    // reason.
    //
    // Called at the top of app_main the heap is still whole, the block is
    // there, and everything else fits around it. Safe to call more than once.
    static bool radio_init();

    // False once the peer drops the link. The poll loop watches this rather
    // than inferring a disconnect from read timeouts, which a merely slow car
    // also produces.
    bool connected() const;

    // Drops the link and waits for it to actually close. Required before any
    // retry: while we hold the adapter it does not advertise, so a scan cannot
    // find the very device we are still connected to.
    void disconnect();

    // Advertised name of the peer we settled on, "" when not connected. Logged
    // so a wrong-adapter connection is visible rather than mysterious.
    const char* peer_name() const;

    // gauge::ITransport
    bool write(const std::string& text) override;
    std::string read(int timeout_ms) override;
    void delay_ms(int ms) override;
};

}  // namespace gauge_platform
