#pragma once
#include <stdint.h>

void powerInit();

// Enable 5V boost converter and wait for rail to stabilize.
void powerBoostOn();

// Disable 5V boost converter (motor rail off).
void powerBoostOff();

void powerLedOn();
void powerLedOff();

// Release coils, kill boost, arm wakeup sources, enter deep sleep.
// wakeSec > 0 also arms the RTC timer for the next scheduled feed.
void powerSleep(uint32_t wakeSec);

// Called from loop(): enters sleep when BLE window is closed and no work is pending.
void powerTick();
