#include "button.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The ESP32-S3's BOOT button. Active low, with the chip's own pull-up: the pin
// reads 1 at rest and 0 while the button is down.
#define BTN_GPIO GPIO_NUM_0

// How long the button must be down before a press counts. The mechanical
// bounce on these is tens of microseconds; 40 ms is far past it and still
// below anything a finger can do on purpose.
#define BTN_DEBOUNCE_US 40000

// The hold that means "into demo". Three seconds, as asked.
#define BTN_HOLD_US 3000000

// A press this long is a stuck button or something resting on the gauge, not a
// gesture. It is ignored entirely rather than treated as a hold, because
// restarting the gauge in a loop is the worst thing this file could do.
#define BTN_ABANDON_US 30000000

// The request, in RTC memory, so it survives the restart that carries it out.
//
// RTC_NOINIT_ATTR, and this is the whole trick. RTC_DATA_ATTR was tried first
// and silently did not work: a variable with no initialiser lands in .rtc.bss,
// which the bootloader zeroes on EVERY boot including a software reset, so the
// request was always gone by the time app_main read it -- the splash played and
// demo mode never started. .rtc_noinit is the only section the bootloader
// leaves alone.
//
// The cost of NOINIT is that it holds whatever the RAM contained on a power-on
// reset, so it is guarded twice: a magic word that random contents will not
// match, and the reset reason, which must be a software reset -- the only thing
// that follows esp_restart(). Either check alone would do; both together mean a
// gauge that loses power in the car cannot come up replaying a drive.
#define BTN_MAGIC 0x5B7A1E01u
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_request;

// Queued by the console from its own task; spent by the app loop. volatile
// because two tasks touch it, and a single aligned int is atomic enough on this
// chip for a flag that is only ever set to one of three values.
static volatile int s_queued;

// The press being watched, if any. `us` is when the pin first read low.
static int64_t s_down_us;
static bool    s_down;

void button_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    s_down = false;
    s_down_us = 0;
}

boot_request_t button_boot_request(void) {
    if (s_magic != BTN_MAGIC) return BOOT_NORMAL;
    if (esp_reset_reason() != ESP_RST_SW) return BOOT_NORMAL;
    if (s_request != BOOT_QUICK && s_request != BOOT_DEMO) return BOOT_NORMAL;
    return (boot_request_t)s_request;
}

void button_boot_request_clear(void) {
    s_magic = 0;
    s_request = BOOT_NORMAL;
}

void button_queue_restart(bool demo) { s_queued = demo ? 2 : 1; }

int button_take_queued_restart(void) {
    const int q = s_queued;
    s_queued = 0;
    return q;
}

void button_request_restart(bool demo) {
    s_magic = BTN_MAGIC;
    s_request = demo ? BOOT_DEMO : BOOT_QUICK;
    printf("button: restarting%s\n", demo ? " into demo mode" : " to retry wifi");
    // Flushed by hand: esp_restart() does not drain the USB console, and the
    // line above is the only evidence that a restart was asked for rather than
    // the gauge falling over.
    fflush(stdout);
    // A beat for the console to actually go out, and for a finger that is
    // still on its way off the button. GPIO0 low through a reset is USB
    // download mode -- see button.h.
    //
    // The caller has already put "RELOADING" on the screen, but drawing it is
    // the LVGL task's job and that task needs the display lock the caller was
    // holding. This wait is what lets the frame actually reach the panel: an
    // AMOLED holds its last frame through a software reset, so the message
    // stays up for the whole dark part of the boot -- which is the point.
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

button_state_t button_poll(void) {
    const bool    low = gpio_get_level(BTN_GPIO) == 0;
    const int64_t now = esp_timer_get_time();

    if (low) {
        if (!s_down) {
            s_down = true;
            s_down_us = now;
        }
        const int64_t held = now - s_down_us;
        if (held < BTN_DEBOUNCE_US) return BTN_IDLE;
        if (held > BTN_ABANDON_US) return BTN_IDLE;   // stuck; see below
        return (held >= BTN_HOLD_US) ? BTN_HELD_LONG : BTN_HELD_SHORT;
    }

    if (!s_down) return BTN_IDLE;
    s_down = false;

    const int64_t held = now - s_down_us;
    if (held < BTN_DEBOUNCE_US) return BTN_IDLE;
    if (held > BTN_ABANDON_US) {
        // A stuck button, or something resting on the gauge. Ignored outright
        // rather than treated as a hold: restarting the gauge in a loop is the
        // worst thing this file could do.
        printf("button: ignoring a %llds press -- stuck, not a gesture\n",
               (long long)(held / 1000000));
        return BTN_IDLE;
    }
    return (held >= BTN_HOLD_US) ? BTN_ACT_DEMO : BTN_ACT_RELOAD;
}
