#include <Arduino.h>
#include <esp_sleep.h>
#if !defined(TARGET_C3)
#include <driver/rtc_io.h>  // rtc_gpio_pullup_en — needed for EXT1 wakeup on S3
#endif
#include "power.h"
#include "motor.h"
#include "schedule.h"
#include "dosing.h"
#include "config.h"
#if BLE_ENABLED
#include "ble.h"
#endif

void powerInit() {
    pinMode(PIN_BOOST_EN, OUTPUT);
    powerBoostOff();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

void powerBoostOn() {
    digitalWrite(PIN_BOOST_EN, HIGH);
    delay(BOOST_SETTLE_MS);
}

void powerBoostOff() {
    digitalWrite(PIN_BOOST_EN, LOW);
}

void powerLedOn()  { digitalWrite(PIN_LED, HIGH); }
void powerLedOff() { digitalWrite(PIN_LED, LOW);  }

void powerSleep(uint32_t wakeSec) {
    ESP_LOGI("power", "entering deep sleep, timerSec=%lu", (unsigned long)wakeSec);

    motorRelease();
    powerBoostOff();
    digitalWrite(PIN_LED, LOW);

    // Wait for button release before sleeping: if still held, the GPIO wakeup
    // would fire immediately after esp_deep_sleep_start().
    // Timeout prevents infinite spin on a shorted button contact.
    uint32_t releaseDeadline = millis() + 10000;
    while (digitalRead(PIN_BUTTON) == LOW && millis() < releaseDeadline) delay(10);
    delay(50);  // extra settle — RC filter on button line

    // GPIO wakeup — button active LOW.
    // C3: GPIO wakeup API automatically maintains pullup during deep sleep.
    // S3: EXT1 wakeup is used; Arduino INPUT_PULLUP is NOT maintained in the RTC
    // domain during deep sleep — must call rtc_gpio_pullup_en() explicitly, otherwise
    // the pin floats LOW and triggers an immediate false wakeup on every sleep entry.
#if defined(TARGET_C3)
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
#endif

    // Timer wakeup for next scheduled feed
    if (wakeSec > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wakeSec * 1000000ULL);
    }

    esp_deep_sleep_start();
    // execution never returns here
}

void powerTick() {
    // Stay awake while Serial config session is active (SERIAL_STAY_AWAKE_SEC after last command).
    // Also stays awake when time is unknown — allows SET_TIME on cold boot before first sleep.
    if (!scheduleTimeKnown() || dosingIsSerialActive()) return;

#if BLE_ENABLED
    if (bleIsActive()) return;
#endif

    uint32_t nextSec = scheduleNextWakeSec();
    powerSleep(nextSec);
}
