#include <Arduino.h>
#include <esp_sleep.h>
#include "config.h"
#include "power.h"
#include "motor.h"
#include "dosing.h"
#include "schedule.h"
#include "wifi_time.h"
#if BLE_ENABLED
#include "ble.h"
#endif

// ── Button polling while the device is awake ───────────────────────────────
// Deep sleep wakeup handles the button on wake-up; this handles it during the
// active window (Serial config session, BLE window, etc.).

static uint32_t btnPressMs = 0;
static bool     btnHeld    = false;

static void buttonTick() {
    bool low = (digitalRead(PIN_BUTTON) == LOW);

    if (low && !btnHeld) {
        btnPressMs = millis();
        btnHeld    = true;
    } else if (!low && btnHeld) {
        uint32_t held = millis() - btnPressMs;
        btnHeld = false;
        if (held < DEBOUNCE_MS) return;  // noise — ignore

        if (held >= LONG_PRESS_MS) {
            ESP_LOGI("main", "long press while awake — sleep");
            powerSleep(scheduleNextWakeSec());
        } else {
            FeedResult r = dosingFeed(FeedMode::BUTTON);
            const char* msg[] = {"OK", "BLOCKED_INTERVAL", "BLOCKED_DAILY", "NO_TIME"};
            ESP_LOGI("main", "button feed: %s", msg[(uint8_t)r]);
        }
    }
}

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

    // WAKEUP_CAUSE_BUTTON = ESP_SLEEP_WAKEUP_GPIO (C3) or ESP_SLEEP_WAKEUP_EXT1 (S3)
    if (cause == WAKEUP_CAUSE_BUTTON) {
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
        // Cold boot — try NTP sync via Wi-Fi; fall back to SET_TIME via Serial
        ESP_LOGI("main", "cold boot");
        bool synced = wifiSyncTime();
        if (!synced) {
            ESP_LOGW("main", "NTP failed — waiting for SET_TIME or WIFI_SSID/WIFI_PASS");
        }
    }
}

void loop() {
    dosingProcessSerial();  // handle CAL/SAVE/FEED/STATUS/RESET_DAY
    scheduleCheck();        // fire any due feed entries
    buttonTick();           // poll button while awake (deep sleep handles it on wake)
    // bleTick();              // close BLE window when BLE_WINDOW_SEC expires
    powerTick();            // sleep when BLE is closed and no work is pending
}
