#pragma once
#include <stdint.h>

void motorInit();

// Move stepper. Positive = forward (dispense), negative = reverse.
// antiJam=true inserts periodic reverse pulses (use during a dose).
// Releases coils automatically when done.
void motorStep(int32_t steps, bool antiJam = false);

// Force-release all coils (call before sleep even if not stepping).
void motorRelease();
