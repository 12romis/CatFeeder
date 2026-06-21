#pragma once

// ============================================================
// Pin map — same GPIO numbers on both targets (verified safe)
// GPIO2-5 are freely usable on both C3 and S3 DevKitC-1.
// S3 strapping pins to avoid: GPIO0 (BOOT), GPIO3, GPIO45, GPIO46.
// GPIO3 is a strapping pin on S3 but safe to use as OUTPUT after boot.
// ============================================================
#if defined(TARGET_C3)
  #define PIN_STEPPER_IN1   2
  #define PIN_STEPPER_IN2   3
  #define PIN_STEPPER_IN3   4
  #define PIN_STEPPER_IN4   5
  #define PIN_BUTTON        9   // INPUT_PULLUP, button to GND; GPIO wakeup source
  #define PIN_BOOST_EN      10  // HIGH = boost on, LOW = motor rail off
  #define PIN_LED           8   // status LED via 220R to GND
#elif defined(TARGET_S3)
  // S3 DevKitC-1 breadboard test — same GPIO numbers as C3 (matches schematic wiring).
  // GPIO48 = onboard RGB LED on DevKitC-1 — avoid
  // GPIO39–42 = JTAG, GPIO0 = BOOT, GPIO19/20 = USB D±  — all avoided
  #define PIN_STEPPER_IN1   3
  #define PIN_STEPPER_IN2   4
  #define PIN_STEPPER_IN3   5
  #define PIN_STEPPER_IN4   6
  #define PIN_BUTTON        9
  #define PIN_BOOST_EN      10
  #define PIN_LED           8
#else
  #error "Build target not defined. Use -DTARGET_C3 or -DTARGET_S3"
#endif

// ============================================================
// Stepper tunables
// ============================================================
#define STEPS_PER_REV           2048    // 28BYJ-48 full-step, 64:1 gearbox
#define STEPS_PER_PORTION       512     // initial value — calibrate with CAL command
#define STEPPER_RPM             12
// Microseconds per step derived from RPM:
#define STEPPER_STEP_US         ((60UL * 1000000UL) / ((uint32_t)STEPS_PER_REV * STEPPER_RPM))

// Anti-jam: briefly reverse every N forward steps
#define ANTIJAM_CYCLE_EVERY     256     // forward steps between reverse pulses
#define ANTIJAM_REVERSE_STEPS   32      // steps backward per anti-jam pulse

// ============================================================
// Overfeed safety limits
// ============================================================
#define MIN_INTERVAL_SEC        (30 * 60)   // 30 min between portions
#define MAX_PORTIONS_PER_DAY    4

// ============================================================
// BLE
// ============================================================
#define BLE_ENABLED             0           // set to 1 when BLE is needed
#define BLE_DEVICE_NAME         "CatFeeder"
#define BLE_WINDOW_SEC          60          // advertising window after button press

// ============================================================
// Power / button
// ============================================================
#define BOOST_SETTLE_MS         5           // settle time after boost EN before stepping
#define LONG_PRESS_MS           2000        // hold duration to enter sleep without feeding

// Wakeup cause constant for the button — platform-specific.
// C3: GPIO wakeup (esp_deep_sleep_enable_gpio_wakeup).
// S3: EXT1 wakeup (esp_sleep_enable_ext1_wakeup).
#if defined(TARGET_C3)
#define WAKEUP_CAUSE_BUTTON  ESP_SLEEP_WAKEUP_GPIO
#else
#define WAKEUP_CAUSE_BUTTON  ESP_SLEEP_WAKEUP_EXT1
#endif

// ============================================================
// Serial
// ============================================================
#define SERIAL_BAUD             115200
// After any Serial command, stay awake this many seconds so the user can type more.
#define SERIAL_STAY_AWAKE_SEC   60
// Button debounce: presses shorter than this are ignored as noise.
#define DEBOUNCE_MS             50
