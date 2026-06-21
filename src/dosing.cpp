#include <Arduino.h>
#include <Preferences.h>
#include "dosing.h"
#include "motor.h"
#include "power.h"
#include "schedule.h"
#include "wifi_time.h"
#include "config.h"

static Preferences prefs;
static uint16_t stepsPerPortion = STEPS_PER_PORTION;

RTC_DATA_ATTR static uint8_t  portionsToday = 0;
RTC_DATA_ATTR static uint32_t lastFeedEpoch = 0;
RTC_DATA_ATTR static uint32_t lastFeedDay   = 0;  // unix day of last feed

void dosingInit() {
    if (prefs.begin("dosing", true)) {
        stepsPerPortion = prefs.getUShort("steps", STEPS_PER_PORTION);
        prefs.end();
    }

    // Reset daily counter if it's a new day (requires time to be known)
    uint32_t now = scheduleNow();
    if (now && (now / 86400) != lastFeedDay) {
        portionsToday = 0;
        lastFeedDay   = now / 86400;
    }
    ESP_LOGI("dosing", "stepsPerPortion=%u portionsToday=%u", stepsPerPortion, portionsToday);
}

uint16_t dosingGetSteps()         { return stepsPerPortion; }
uint8_t  dosingGetPortionsToday() { return portionsToday; }

FeedResult dosingFeed(FeedMode mode) {
    uint32_t now = 0;

    if (mode != FeedMode::CALIBRATE) {
        now = scheduleNow();  // captured once — reused for lastFeedEpoch stamp below

        if (!now) return FeedResult::NO_TIME;

        // Reset daily counter at midnight
        uint32_t today = now / 86400;
        if (today != lastFeedDay) {
            portionsToday = 0;
            lastFeedDay   = today;
        }

        // Both safety limits apply to scheduled feeds only.
        // Button and serial FEED are unrestricted (besides requiring time to be known).
        if (mode == FeedMode::SCHEDULE) {
            if (lastFeedEpoch != 0 && now >= lastFeedEpoch &&
                (now - lastFeedEpoch) < MIN_INTERVAL_SEC)
                return FeedResult::BLOCKED_INTERVAL;
            if (portionsToday >= MAX_PORTIONS_PER_DAY)
                return FeedResult::BLOCKED_DAILY;
        }
    }

    powerLedOn();
    powerBoostOn();
    motorStep(stepsPerPortion, /*antiJam=*/true);
    powerBoostOff();
    powerLedOff();

    if (mode != FeedMode::CALIBRATE) {
        portionsToday++;
        lastFeedEpoch = now;  // same value checked above — no second scheduleNow() call
    }

    ESP_LOGI("dosing", "fed %u steps, mode=%u, today=%u", stepsPerPortion, (uint8_t)mode, portionsToday);
    return FeedResult::OK;
}

void dosingRunSteps(int32_t steps) {
    powerLedOn();
    powerBoostOn();
    motorStep(steps, /*antiJam=*/false);
    powerBoostOff();
    powerLedOff();
}

void dosingSaveSteps(uint16_t steps) {
    stepsPerPortion = steps;
    prefs.begin("dosing", false);
    prefs.putUShort("steps", steps);
    prefs.end();
    ESP_LOGI("dosing", "saved stepsPerPortion=%u", steps);
}

// ── Serial command parser ──────────────────────────────────────────────────

static char  serialBuf[64];
static uint8_t  serialPos  = 0;
static uint16_t calSteps   = 0;       // last CAL value, waiting for SAVE
static uint32_t lastSerialMs = 0;     // millis() of last received command

bool dosingIsSerialActive() {
    return (millis() - lastSerialMs) < (uint32_t)SERIAL_STAY_AWAKE_SEC * 1000;
}

static void processLine(const char* line) {
    if (strncmp(line, "CAL ", 4) == 0) {
        calSteps = (uint16_t)atoi(line + 4);
        Serial.printf("CAL %u steps (send SAVE to commit, or another CAL)\n", calSteps);
        dosingRunSteps(calSteps);

    } else if (strcmp(line, "SAVE") == 0) {
        if (calSteps == 0) {
            Serial.println("No CAL value pending.");
            return;
        }
        dosingSaveSteps(calSteps);
        Serial.printf("Saved stepsPerPortion = %u\n", calSteps);
        calSteps = 0;

    } else if (strcmp(line, "FEED") == 0) {
        FeedResult r = dosingFeed(FeedMode::BUTTON);
        const char* msg[] = {"OK", "BLOCKED_INTERVAL", "BLOCKED_DAILY", "NO_TIME"};
        Serial.printf("FEED: %s\n", msg[(uint8_t)r]);

    } else if (strcmp(line, "STATUS") == 0) {
        uint32_t now = scheduleNow();
        Serial.printf("time:          %s (epoch=%lu)\n",
            scheduleTimeKnown() ? "known" : "UNKNOWN", (unsigned long)now);
        Serial.printf("stepsPerPortion: %u\n", stepsPerPortion);
        Serial.printf("portionsToday:   %u / %u\n", portionsToday, MAX_PORTIONS_PER_DAY);
        Serial.printf("lastFeedEpoch:   %lu\n", (unsigned long)lastFeedEpoch);

        FeedTime sched[SCHEDULE_MAX_ENTRIES];
        uint8_t count = scheduleGet(sched);
        if (count == 0) {
            Serial.println("schedule:      (none)");
        } else {
            Serial.printf("schedule:      %u entr%s\n", count, count == 1 ? "y" : "ies");
            for (uint8_t i = 0; i < count; i++)
                Serial.printf("  [%u] %02u:%02u UTC\n", i, sched[i].hour, sched[i].minute);
        }

        uint32_t nextSec = scheduleNextWakeSec();
        if (nextSec > 0)
            Serial.printf("next feed in:  %luh %02lum\n",
                (unsigned long)(nextSec / 3600), (unsigned long)((nextSec % 3600) / 60));
        else
            Serial.println("next feed in:  (no schedule / time unknown)");

        String wifiSsid = wifiGetSsid();
        Serial.printf("wifi SSID:     %s\n", wifiSsid.isEmpty() ? "(not configured)" : wifiSsid.c_str());

    } else if (strcmp(line, "RESET_DAY") == 0) {
        portionsToday = 0;
        lastFeedEpoch = 0;
        Serial.println("Daily counters reset.");

    } else if (strncmp(line, "WIFI_SSID ", 10) == 0) {
        wifiSetSsid(line + 10);
        Serial.printf("Wi-Fi SSID saved: %s\n", line + 10);

    } else if (strncmp(line, "WIFI_PASS ", 10) == 0) {
        wifiSetPass(line + 10);
        Serial.println("Wi-Fi password saved.");

    } else if (strcmp(line, "WIFI_SYNC") == 0) {
        Serial.println("Syncing time via Wi-Fi NTP...");
        bool ok = wifiSyncTime();
        Serial.printf("Wi-Fi NTP: %s\n", ok ? "OK" : "FAILED");

    } else if (strncmp(line, "SET_TIME ", 9) == 0) {
        // Testing without BLE: SET_TIME <unix_epoch>
        // Get current epoch: date +%s  (Linux/Mac terminal)
        uint32_t epoch = (uint32_t)strtoul(line + 9, nullptr, 10);
        scheduleSetTime(epoch);
        Serial.printf("Time set: epoch=%lu\n", (unsigned long)epoch);

    } else if (strncmp(line, "SCHED ", 6) == 0) {
        // Set feeding schedule: SCHED HH:MM [HH:MM ...]  (up to 8 entries, UTC)
        // Example: SCHED 08:00 18:30
        FeedTime times[SCHEDULE_MAX_ENTRIES];
        uint8_t count = 0;
        const char* p = line + 6;
        while (*p && count < SCHEDULE_MAX_ENTRIES) {
            int h = -1, m = -1;
            if (sscanf(p, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                times[count++] = {(uint8_t)h, (uint8_t)m};
            }
            p = strchr(p, ' ');
            if (!p) break;
            p++;
        }
        if (count > 0) {
            scheduleSet(times, count);
            Serial.printf("Schedule saved: %u entries\n", count);
            for (uint8_t i = 0; i < count; i++)
                Serial.printf("  [%u] %02u:%02u\n", i, times[i].hour, times[i].minute);
        } else {
            Serial.println("Usage: SCHED HH:MM [HH:MM ...]  e.g. SCHED 08:00 18:30");
        }

    } else if (strcmp(line, "SCHED?") == 0) {
        // Show current schedule from NVS via scheduleSet — read it back via a status print
        Serial.println("Use STATUS to see next feed, re-send SCHED to overwrite.");

    } else {
        Serial.println("Commands:");
        Serial.println("  CAL <steps>          run motor N steps (calibration)");
        Serial.println("  SAVE                 save CAL steps to NVS");
        Serial.println("  FEED                 dispense one portion (limits apply)");
        Serial.println("  STATUS               show state");
        Serial.println("  RESET_DAY            zero daily counters");
        Serial.println("  SET_TIME <epoch>     set wall clock (unix seconds)");
        Serial.println("  SCHED HH:MM [...]    set schedule, UTC, up to 8 entries");
        Serial.println("  WIFI_SSID <ssid>     save Wi-Fi network name to flash");
        Serial.println("  WIFI_PASS <pass>     save Wi-Fi password to flash");
        Serial.println("  WIFI_SYNC            sync time via NTP now");
    }
}

void dosingProcessSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                lastSerialMs = millis();  // reset stay-awake timer on every command
                processLine(serialBuf);
                serialPos = 0;
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }
}
