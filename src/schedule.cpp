#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include "schedule.h"
#include "dosing.h"
#include "config.h"

// Time tracking — survives deep sleep; wiped on full power-off (handled in init)
RTC_DATA_ATTR static uint32_t rtcEpochBase  = 0;   // epoch at last sync
RTC_DATA_ATTR static uint64_t rtcMicrosBase = 0;   // esp_rtc_get_time_us() at last sync
RTC_DATA_ATTR static bool     timeKnown     = false;

// Bitmask: bit i = entry i was fired today. uint8_t supports up to 8 entries.
static_assert(SCHEDULE_MAX_ENTRIES <= 8, "firedToday bitmask is uint8_t — increase to uint16_t if SCHEDULE_MAX_ENTRIES > 8");
RTC_DATA_ATTR static uint8_t  firedToday    = 0;
RTC_DATA_ATTR static uint32_t firedDay      = 0;   // unix day of firedToday

static FeedTime entries[SCHEDULE_MAX_ENTRIES];
static uint8_t  entryCount = 0;
static Preferences prefs;

static void loadScheduleFromNVS() {
    prefs.begin("schedule", true);
    entryCount = prefs.getUChar("count", 0);
    if (entryCount > SCHEDULE_MAX_ENTRIES) entryCount = SCHEDULE_MAX_ENTRIES;
    prefs.getBytes("entries", entries, entryCount * sizeof(FeedTime));
    prefs.end();
}

void scheduleInit() {
    loadScheduleFromNVS();

    // On cold boot RTC_DATA_ATTR vars are zero — timeKnown=false is the right state.
    // On deep-sleep wake they retain their values from before sleep.
    auto cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // Full power-on: NVS schedule loaded above; time needs BLE sync
        timeKnown  = false;
        firedToday = 0;
    }

    ESP_LOGI("schedule", "entries=%u timeKnown=%d cause=%d", entryCount, timeKnown, cause);
}

void scheduleSetTime(uint32_t epoch) {
    rtcEpochBase  = epoch;
    rtcMicrosBase = esp_rtc_get_time_us();
    timeKnown     = true;
    ESP_LOGI("schedule", "time synced epoch=%lu", (unsigned long)epoch);
}

uint32_t scheduleNow() {
    if (!timeKnown) return 0;
    uint64_t elapsed = esp_rtc_get_time_us() - rtcMicrosBase;
    return rtcEpochBase + (uint32_t)(elapsed / 1000000ULL);
}

bool scheduleTimeKnown() { return timeKnown; }

void scheduleSet(const FeedTime* src, uint8_t count) {
    if (count > SCHEDULE_MAX_ENTRIES) count = SCHEDULE_MAX_ENTRIES;
    entryCount = count;
    memcpy(entries, src, count * sizeof(FeedTime));
    firedToday = 0;

    prefs.begin("schedule", false);
    prefs.putUChar("count", count);
    prefs.putBytes("entries", entries, count * sizeof(FeedTime));
    prefs.end();
}

void scheduleCheck() {
    if (!timeKnown || entryCount == 0) return;

    uint32_t now   = scheduleNow();
    uint32_t today = now / 86400;

    // New day — reset fired mask
    if (today != firedDay) {
        firedToday = 0;
        firedDay   = today;
    }

    // Seconds since midnight (UTC)
    uint32_t secOfDay = now % 86400;
    uint16_t curHHMM  = (uint16_t)((secOfDay / 3600) * 100 + (secOfDay % 3600) / 60);

    for (uint8_t i = 0; i < entryCount; i++) {
        if (firedToday & (1 << i)) continue;
        uint16_t entryHHMM = entries[i].hour * 100 + entries[i].minute;
        if (curHHMM >= entryHHMM) {
            ESP_LOGI("schedule", "firing entry %u (%02u:%02u)", i, entries[i].hour, entries[i].minute);
            FeedResult r = dosingFeed(FeedMode::SCHEDULE);
            if (r == FeedResult::OK) {
                firedToday |= (1 << i);
            } else {
                ESP_LOGW("schedule", "feed skipped, result=%u — will retry next check", (uint8_t)r);
            }
        }
    }
}

uint32_t scheduleNextWakeSec() {
    if (!timeKnown || entryCount == 0) return 0;

    uint32_t now      = scheduleNow();
    uint32_t secOfDay = now % 86400;
    uint32_t today    = now / 86400;

    uint32_t earliest = UINT32_MAX;

    for (uint8_t i = 0; i < entryCount; i++) {
        if (firedToday & (1 << i)) continue;
        uint32_t entrySec = (uint32_t)entries[i].hour * 3600 + entries[i].minute * 60;
        uint32_t delta;
        if (entrySec > secOfDay) {
            delta = entrySec - secOfDay;
        } else {
            // Already past — schedule for tomorrow
            delta = 86400 - secOfDay + entrySec;
        }
        if (delta < earliest) earliest = delta;
    }

    return (earliest == UINT32_MAX) ? 0 : earliest;
}
