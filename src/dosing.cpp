#include <Arduino.h>
#include <Preferences.h>
#include "dosing.h"
#include "motor.h"
#include "power.h"
#include "schedule.h"
#include "config.h"

static Preferences prefs;
static uint16_t stepsPerPortion = STEPS_PER_PORTION;

RTC_DATA_ATTR static uint8_t  portionsToday = 0;
RTC_DATA_ATTR static uint32_t lastFeedEpoch = 0;
RTC_DATA_ATTR static uint32_t lastFeedDay   = 0;  // unix day of last feed

void dosingInit() {
    prefs.begin("dosing", true);  // read-only
    stepsPerPortion = prefs.getUShort("steps", STEPS_PER_PORTION);
    prefs.end();

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

        // Guard against unsigned underflow when SET_TIME moves clock backward
        if (lastFeedEpoch != 0 && now >= lastFeedEpoch &&
            (now - lastFeedEpoch) < MIN_INTERVAL_SEC)
            return FeedResult::BLOCKED_INTERVAL;

        // Daily quota applies to scheduled feeds only — button is always allowed
        if (mode == FeedMode::SCHEDULE && portionsToday >= MAX_PORTIONS_PER_DAY)
            return FeedResult::BLOCKED_DAILY;
    }

    powerBoostOn();
    motorStep(stepsPerPortion, /*antiJam=*/true);
    powerBoostOff();

    if (mode != FeedMode::CALIBRATE) {
        portionsToday++;
        lastFeedEpoch = now;  // same value checked above — no second scheduleNow() call
    }

    ESP_LOGI("dosing", "fed %u steps, mode=%u, today=%u", stepsPerPortion, (uint8_t)mode, portionsToday);
    return FeedResult::OK;
}

void dosingRunSteps(int32_t steps) {
    powerBoostOn();
    motorStep(steps, /*antiJam=*/false);
    powerBoostOff();
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
static uint8_t serialPos = 0;
static uint16_t calSteps = 0;  // last CAL value, waiting for SAVE

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
        Serial.printf("stepsPerPortion=%u  portionsToday=%u  lastFeedEpoch=%lu  now=%lu\n",
            stepsPerPortion, portionsToday, (unsigned long)lastFeedEpoch,
            (unsigned long)scheduleNow());

    } else if (strcmp(line, "RESET_DAY") == 0) {
        portionsToday = 0;
        lastFeedEpoch = 0;
        Serial.println("Daily counters reset.");

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
    }
}

void dosingProcessSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                processLine(serialBuf);
                serialPos = 0;
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }
}
