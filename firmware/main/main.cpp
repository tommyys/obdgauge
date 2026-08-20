// Phase 0 bring-up entry point. Proves the toolchain, the partition table and
// that gauge_core - the code the host tests exercise - links and runs on the
// board unchanged.
#include <cstdio>
#include "version.h"
#include "pid.h"
#include "poll.h"

extern "C" void app_main(void) {
    printf("\n=== mx5-gauge core %s ===\n", gauge::core_version());

    // Run a couple of the host-tested decoders on the board, so the first
    // flash proves the ported logic actually executes on the ESP32-S3 and not
    // merely on the Mac.
    auto rpm = gauge::dec_rpm(gauge::Bytes{0x1A, 0xF8});
    auto coolant = gauge::dec_temp(gauge::Bytes{0x5A});
    printf("dec_rpm(1A F8)  = %.1f   (expect 1726.0)\n", rpm ? *rpm : -1.0);
    printf("dec_temp(5A)    = %d     (expect 50)\n", coolant ? *coolant : -999);

    auto cyc = gauge::build_poll_cycle({0x0C, 0x0D, 0x05});
    printf("poll cycle len  = %d     (expect 3)\n", static_cast<int>(cyc.size()));
    printf("=== core ok ===\n");
}
