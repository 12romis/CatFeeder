#pragma once
#include <stdint.h>

#define SCHEDULE_MAX_ENTRIES 8

struct FeedTime {
    uint8_t hour;
    uint8_t minute;
};

void scheduleInit();

// Set wall-clock time from BLE or other source. Call whenever epoch is known.
void scheduleSetTime(uint32_t epoch);

// Current unix epoch, derived from RTC timer + last sync. Returns 0 if unknown.
uint32_t scheduleNow();

bool scheduleTimeKnown();

// Set the schedule (array of FeedTime, up to SCHEDULE_MAX_ENTRIES). Persisted in NVS.
void scheduleSet(const FeedTime* entries, uint8_t count);

// Called from loop(): fires dosingFeed() for any due entry. Marks entries done for today.
void scheduleCheck();

// Seconds until the next scheduled feed (for deep-sleep timer). 0 = no schedule.
uint32_t scheduleNextWakeSec();
