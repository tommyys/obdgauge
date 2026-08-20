// Phase 0 bring-up entry point. Proves the toolchain, the partition table and
// that gauge_core - the code the host tests exercise - links and runs on the
// board unchanged.
//
// The summary reprints on a loop rather than once at boot: a banner printed
// only at startup is easy to miss, and on the USB-Serial/JTAG port the reset
// needed to see it can drop the chip into download mode instead.
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "parse.h"
#include "pid.h"
#include "poll.h"
#include "state.h"
#include "metrics.h"
#include "vehicle.h"
#include "version.h"

extern "C" void app_main(void) {
    int pass = 0, fail = 0;
    auto expect = [&](const char* what, bool ok, const char* got) {
        if (ok) ++pass; else ++fail;
        printf("  %-34s %s  %s\n", what, ok ? "ok  " : "FAIL", got);
    };

    for (;;) {
        pass = fail = 0;
        printf("\n=== mx5-gauge core %s on ESP32-S3 ===\n", gauge::core_version());

        char buf[48];
        auto rpm = gauge::dec_rpm(gauge::Bytes{0x1A, 0xF8});
        snprintf(buf, sizeof buf, "%.1f", rpm ? *rpm : -1.0);
        expect("dec_rpm(1A F8) == 1726.0", rpm && *rpm == 1726.0, buf);

        auto coolant = gauge::dec_temp(gauge::Bytes{0x5A});
        snprintf(buf, sizeof buf, "%d", coolant ? *coolant : -999);
        expect("dec_temp(5A) == 50", coolant && *coolant == 50, buf);

        auto payload = gauge::parse_mode01("7E8 03 41 0C 1A F8", 0x0C);
        bool ok_parse = payload && payload->size() == 2 &&
                        (*payload)[0] == 0x1A && (*payload)[1] == 0xF8;
        expect("parse_mode01 strips header noise", ok_parse, ok_parse ? "1A F8" : "?");

        auto cyc = gauge::build_poll_cycle({0x0C, 0x0D, 0x05});
        snprintf(buf, sizeof buf, "%d", static_cast<int>(cyc.size()));
        expect("build_poll_cycle len == 3", cyc.size() == 3, buf);

        // The plausibility gate, which replay never exercises on clean logs.
        gauge::VehicleState st;
        st.set("coolant", 88.0);
        st.set("coolant", 900.0);
        auto c = st.get("coolant");
        snprintf(buf, sizeof buf, "%.1f rej=%d", c ? *c : -1.0, st.rejected());
        expect("implausible reading rejected", c && *c == 88.0 && st.rejected() == 1, buf);

        // Economy, in the km/L the display quotes.
        auto kmpl = gauge::km_per_l(20.0);
        snprintf(buf, sizeof buf, "%.2f", kmpl ? *kmpl : -1.0);
        expect("km_per_l(20) == 5.0", kmpl && *kmpl == 5.0, buf);

        auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
        expect("VIN -> MAZDA MX-5", id.label == "MAZDA MX-5", id.label.c_str());
        snprintf(buf, sizeof buf, "%.0f", id.rpm_red);
        expect("MX-5 profile redline == 7000", id.rpm_red == 7000.0, buf);

        printf("=== %d passed, %d failed ===\n", pass, fail);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
