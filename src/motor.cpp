#include <Arduino.h>
#include "motor.h"
#include "config.h"

// Double-coil full-step sequence for 28BYJ-48 (higher torque than wave-drive).
// Indexed by (currentPhase & 3).
static const uint8_t kStepTable[4][4] = {
    {1, 1, 0, 0},  // IN1 IN2 IN3 IN4
    {0, 1, 1, 0},
    {0, 0, 1, 1},
    {1, 0, 0, 1},
};

static const uint8_t kPins[4] = {
    PIN_STEPPER_IN1, PIN_STEPPER_IN2, PIN_STEPPER_IN3, PIN_STEPPER_IN4
};

static int8_t phase = 0;  // current step phase 0–3

static void applyPhase() {
    const uint8_t* row = kStepTable[phase & 3];
    for (int i = 0; i < 4; i++) digitalWrite(kPins[i], row[i]);
}

static void singleStep(int8_t dir) {
    phase = (phase + dir + 4) % 4;
    applyPhase();
    delayMicroseconds(STEPPER_STEP_US);
}

void motorInit() {
    for (int i = 0; i < 4; i++) {
        pinMode(kPins[i], OUTPUT);
        digitalWrite(kPins[i], LOW);
    }
}

void motorRelease() {
    for (int i = 0; i < 4; i++) digitalWrite(kPins[i], LOW);
}

void motorStep(int32_t steps, bool antiJam) {
    if (steps == 0) return;

    int8_t dir = (steps > 0) ? 1 : -1;
    int32_t remaining = (steps > 0) ? steps : -steps;
    int32_t sinceJam = 0;

    while (remaining-- > 0) {
        singleStep(dir);
        if (antiJam && dir == 1) {
            if (++sinceJam >= ANTIJAM_CYCLE_EVERY) {
                for (int r = 0; r < ANTIJAM_REVERSE_STEPS; r++) singleStep(-1);
                for (int r = 0; r < ANTIJAM_REVERSE_STEPS; r++) singleStep(1);
                sinceJam = 0;
            }
        }
    }
    motorRelease();
}
