#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>
// esp_clk_rtc_time() — IDF 5.x rename of esp_rtc_get_time_us(). Reads RTC slow-clock
// counter in microseconds; runs through deep sleep on both C3 and S3.
#include <esp_private/esp_clk.h>
#include "schedule.h"
#include "dosing.h"
#include "config.h"

static inline uint64_t getRtcUs() { return esp_clk_rtc_time(); }

// Time tracking — survives deep sleep; wiped on full power-off (handled in init)
RTC_DATA_ATTR static uint32_t rtcEpochBase  = 0;   // epoch at last sync
RTC_DATA_ATTR static uint64_t rtcMicrosBase = 0;   // getRtcUs() value at last sync
RTC_DATA_ATTR static bool     timeKnown     = false;

// Bitmask: bit i = entry i was fired today. uint8_t supports up to 8 entries.
static_assert(SCHEDULE_MAX_ENTRIES <= 8, "firedToday bitmask is uint8_t — increase to uint16_t if SCHEDULE_MAX_ENTRIES > 8");
RTC_DATA_ATTR static uint8_t  firedToday    = 0;
RTC_DATA_ATTR static uint32_t firedDay      = 0;   // unix day of firedToday

static FeedTime entries[SCHEDULE_MAX_ENTRIES];
static uint8_t  entryCount = 0;
static Preferences prefs;

static void loadScheduleFromNVS() {
    if (!prefs.begin("schedule", true)) return;  // namespace not created yet — defaults are 0
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
    rtcMicrosBase = getRtcUs();
    timeKnown     = true;

    // Mark schedule entries that are already past as fired so they don't
    // trigger immediately after a cold boot or manual SET_TIME.
    uint32_t today    = epoch / 86400;
    uint32_t secOfDay = epoch % 86400;
    uint16_t curHHMM  = (uint16_t)((secOfDay / 3600) * 100 + (secOfDay % 3600) / 60);

    if (today != firedDay) {
        firedToday = 0;
        firedDay   = today;
    }
    for (uint8_t i = 0; i < entryCount; i++) {
        if (curHHMM >= (uint16_t)(entries[i].hour * 100 + entries[i].minute)) {
            firedToday |= (1 << i);
        }
    }

    ESP_LOGI("schedule", "time synced epoch=%lu curHHMM=%04u pastMarked=0x%02X",
             (unsigned long)epoch, curHHMM, firedToday);
}

uint32_t scheduleNow() {
    if (!timeKnown) return 0;
    uint64_t elapsed = getRtcUs() - rtcMicrosBase;
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
    // Schedule has minute-level precision — no need to check more often than once a minute.
    // nextCheckMs=0 on first call (cold boot or deep-sleep wake) → runs immediately.
    static uint32_t nextCheckMs = 0;
    if (millis() < nextCheckMs) return;
    nextCheckMs = millis() + 60000;

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
            firedToday |= (1 << i);  // mark done regardless — blocked entries are skipped, not retried
            if (r != FeedResult::OK) {
                static const char* const kReasons[] = {"OK", "BLOCKED_INTERVAL", "BLOCKED_DAILY", "NO_TIME"};
                ESP_LOGW("schedule", "entry %u (%02u:%02u) skipped: %s",
                    i, entries[i].hour, entries[i].minute,
                    kReasons[(uint8_t)r < 4 ? (uint8_t)r : 0]);
            }
        }
    }
}

uint8_t scheduleGet(FeedTime* dst) {
    memcpy(dst, entries, entryCount * sizeof(FeedTime));
    return entryCount;
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
