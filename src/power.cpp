#include <Arduino.h>
#include <esp_sleep.h>
#include "power.h"
#include "motor.h"
#include "schedule.h"
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
    // esp_deep_sleep_enable_gpio_wakeup works on both C3 and S3.
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);

    // Timer wakeup for next scheduled feed
    if (wakeSec > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wakeSec * 1000000ULL);
    }

    esp_deep_sleep_start();
    // execution never returns here
}

void powerTick() {
    // Stay awake until time is known — allows SET_TIME / SCHED via Serial on cold boot.
    if (!scheduleTimeKnown()) return;

#if BLE_ENABLED
    if (bleIsActive()) return;
#endif

    uint32_t nextSec = scheduleNextWakeSec();
    powerSleep(nextSec);
}
