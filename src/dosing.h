#pragma once
#include <stdint.h>

enum class FeedResult : uint8_t {
    OK               = 0,
    BLOCKED_INTERVAL = 1,
    BLOCKED_DAILY    = 2,
    NO_TIME          = 3,
};

enum class FeedMode : uint8_t {
    SCHEDULE = 0,  // all limits: interval + daily quota
    BUTTON   = 1,  // interval limit only — no daily quota check
    CALIBRATE = 2, // no limits (CAL command only)
};

void dosingInit();

// Dispense one portion according to mode limits.
FeedResult dosingFeed(FeedMode mode = FeedMode::SCHEDULE);

// Run exactly N steps without updating portionsToday (for calibration).
void dosingRunSteps(int32_t steps);

// Save current calibration step count to NVS.
void dosingSaveSteps(uint16_t steps);

uint16_t dosingGetSteps();
uint8_t  dosingGetPortionsToday();

// Call from loop() to process Serial commands (CAL/SAVE/FEED/STATUS/RESET_DAY).
void dosingProcessSerial();
