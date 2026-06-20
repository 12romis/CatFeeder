#include <Arduino.h>
#include <esp_sleep.h>
#include "config.h"
#include "power.h"
#include "motor.h"
#include "dosing.h"
#include "schedule.h"
#if BLE_ENABLED
#include "ble.h"
#endif

void setup() {
    Serial.begin(SERIAL_BAUD);

    powerInit();    // GPIO10 (boost) LOW, LED LOW, button INPUT_PULLUP
    motorInit();    // stepper pins OUTPUT LOW (coils released)
    scheduleInit(); // load schedule + time state first — dosingInit() depends on scheduleNow()
    dosingInit();   // load stepsPerPortion from NVS
#if BLE_ENABLED
    bleInit();      // init NimBLE GATT server (not advertising yet)
#endif

    auto cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI("main", "woke by button");

        // Detect long press: button still held after setup() (~200 ms boot time).
        // Short press → already released → feed.
        // Long press → held LONG_PRESS_MS more → sleep without feeding.
        // Deadline prevents infinite spin on a shorted button contact.
        if (digitalRead(PIN_BUTTON) == LOW) {
            uint32_t holdStart = millis();
            uint32_t deadline  = holdStart + LONG_PRESS_MS + 5000;
            while (digitalRead(PIN_BUTTON) == LOW && millis() < deadline) delay(10);
            if (millis() - holdStart >= LONG_PRESS_MS) {
                ESP_LOGI("main", "long press — going to sleep");
                powerSleep(scheduleNextWakeSec());
                // no return — deep sleep starts inside
            }
        }

        FeedResult r = dosingFeed(FeedMode::BUTTON);
        const char* msg[] = {"OK", "BLOCKED_INTERVAL", "BLOCKED_DAILY", "NO_TIME"};
        ESP_LOGI("main", "button feed: %s", msg[(uint8_t)r]);

    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        // Scheduled feed timer — run schedule check, then go back to sleep
        ESP_LOGI("main", "woke by timer");
        scheduleCheck();

    } else {
        // Cold boot
        ESP_LOGI("main", "cold boot");
        // bleStart();  // open BLE window for initial time sync
    }
}

void loop() {
    dosingProcessSerial();  // handle CAL/SAVE/FEED/STATUS/RESET_DAY
    scheduleCheck();        // fire any due feed entries
    // bleTick();              // close BLE window when BLE_WINDOW_SEC expires
    powerTick();            // sleep when BLE is closed and no work is pending
}
