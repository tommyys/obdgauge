# Vendored copy of waveshare/esp32_s3_touch_amoled_1_75c 3.0.0

A component in `components/` overrides the registry copy of the same name in
`managed_components/`, which is why this directory exists. Upstream is
`git://github.com/waveshareteam/Waveshare-ESP32-components.git`, path
`bsp/esp32_s3_touch_amoled_1_75c`, commit `010c5fb39ea7f0e996efd1f8e162cc70e569e57f`.

## The only change

`esp32_s3_touch_amoled_1_75c.c`, in `bsp_display_new()`, one line:

    io_config.pclk_hz = 80 * 1000 * 1000;

Upstream builds the panel IO from `CO5300_PANEL_IO_QSPI_CONFIG`, which hardcodes
`pclk_hz = 40 * 1000 * 1000`, and there is no Kconfig knob for it. The SPI clock
divides from an 80 MHz source, so 80 is the only step above 40.

Measured on the board 2026-08-22 (466x466 RGB565, 434 KB/frame): the banded blit
went 32 ms -> 19 ms and the boot splash went from 27 of 31 frames to all 31 in
the same 2.5 s. Live view fps did not change -- those are LVGL-render bound, not
panel bound. Verified visually clean at 80 MHz.

`CHECKSUMS.json` was deleted: it describes the unmodified upstream files.

## Updating

Diff a fresh upstream copy against this directory, confirm the one line above is
the entire delta, then re-apply it. If a future BSP version exposes the pixel
clock as a Kconfig option, delete this directory and set that instead.
