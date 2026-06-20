---
name: cat-feeder-firmware
description: Context and engineering rules for writing ESP32-C3 firmware for an automatic BLE cat feeder with an auger-screw dosing mechanism driven by a 28BYJ-48 stepper. Use whenever generating, reviewing, or refactoring firmware for this project — dosing logic, BLE GATT server, deep-sleep/wake handling, scheduling, or power management.
---

# Cat Feeder Firmware — Project Context

Automatic cat feeder. An ESP32-C3 drives a 28BYJ-48 stepper through a ULN2003
module turning a 3D-printed auger screw that dispenses dry food. Two feeding modes:
(1) time-based schedule, (2) on-demand portion triggered from a phone app over BLE.
The device runs from an 18650 cell and must be power-frugal (deep sleep between events).

The agent writing code for this project MUST read and honor everything below before
producing firmware. Treat the hardware pin map and the safety rules as non-negotiable.

---

## 1. Target & toolchain

- **MCU:** ESP32-C3 SuperMini (RISC-V single core, BLE 5.0, Wi-Fi unused).
- **Framework:** Arduino-ESP32 under **PlatformIO** (the developer uses PlatformIO on
  Kubuntu with USB Serial/JTAG debugging — pioarduino flavor acceptable).
- **BLE stack:** **NimBLE-Arduino** (NOT the default Bluedroid — NimBLE uses far less
  RAM/flash and is the right choice on a C3).
- **Language:** C++ (Arduino style), readable, commented. Non-blocking patterns
  preferred; avoid long `delay()` in the main path.
- Developer is fluent in embedded C/C++, FreeRTOS, PlatformIO. Do NOT over-explain
  basics. Be concise and technically precise. Communicate in Ukrainian when chatting,
  code comments may be Ukrainian or English (developer reads both).

## 2. Hardware pin map (authoritative — do not change without being asked)

| ESP32-C3 GPIO | Connects to | Notes |
|---|---|---|
| GPIO2 | ULN2003 IN1 | stepper phase |
| GPIO3 | ULN2003 IN2 | stepper phase |
| GPIO4 | ULN2003 IN3 | stepper phase |
| GPIO5 | ULN2003 IN4 | stepper phase |
| GPIO9 | Push button | wake source; INPUT_PULLUP, button to GND |
| GPIO10 | Boost converter EN | drive HIGH to enable 5V boost, LOW to kill motor rail in sleep |
| GPIO8 | Status LED (via 220Ω) | optional indicator |
| 5V / VIN | +5V from boost / battery | board LDO makes 3.3V |
| 3V3 | unused (no-connect) | internal LDO output |

- Button is the ONLY wake source. On ESP32-C3 deep-sleep wake uses GPIO with RTC
  capability — verify GPIO9 supports `esp_deep_sleep_enable_gpio_wakeup` (C3 uses
  GPIO wakeup, not ext0/ext1 like classic ESP32). If GPIO9 is a strapping/BOOT pin
  on this board variant, flag it and propose an alternative RTC-capable GPIO.

## 3. Power architecture (drives the sleep strategy)

```
18650 (3.0–4.2V) → TP4056 (charge + protect) → OUT+ splits:
   (a) → ESP32-C3 5V/VIN pin directly  (LDO makes 3.3V; C3 tolerates 3.0–4.2V on VIN)
   (b) → AP3602A boost VIN → VOUT 5V → ULN2003 V+ and 28BYJ-48 motor
```

- ESP32 is always powered from the battery (cannot be hard-cut, it must run BLE/RTC).
- The **motor rail (boost) is switchable** via GPIO10 → boost EN.
- **Autonomy is dominated by quiescent current, not battery capacity.** In deep sleep
  the C3 draws ~tens of µA; an always-on BLE advertise draws ~100 mA. The whole design
  intent is: sleep most of the time, wake briefly.

## 4. Core firmware behavior (functional spec)

### 4.1 Dosing — BY STEPS, never by time
- Portion size is defined in **stepper steps**, e.g. `STEPS_PER_PORTION`, calibrated by
  weighing dispensed food. NEVER dose by "run motor for N milliseconds" — speed drifts
  with battery voltage and load, ruining repeatability.
- 28BYJ-48: 2048 steps/rev (full-step, 64:1 gearbox). Keep RPM modest (~12) so the
  auger doesn't stall under load.
- **Anti-jam reverse:** during a dose, periodically step backward a small amount then
  forward to break food bridging in the auger. Make reverse-steps and cycle count
  constants.

### 4.2 De-energize coils after every move (CRITICAL)
- After any dose, set IN1–IN4 LOW. A stationary 28BYJ-48 with a coil energized heats up
  and wastes current — this can dominate the whole power budget. Always release coils.

### 4.3 Boost EN management around sleep (CRITICAL)
- Before entering deep sleep: release coils, then drive GPIO10 LOW (kill motor rail).
- On wake / before a dose: drive GPIO10 HIGH, allow a few ms for boost to stabilize,
  then move the motor.

### 4.4 Two feeding modes
- **Schedule:** feed at configured times (hour:minute). On real hardware there is no RTC
  chip — time is held by the ESP32 and **synced from the phone over BLE on connect**
  (epoch/Unix time written by the app). If the device resets while the phone is away,
  it does not know the wall-clock time until the next BLE sync — handle this gracefully
  (don't fire stale/duplicate feeds; mark schedule entries done per day).
- **Manual:** button press OR BLE command → one portion (subject to safety limits 4.5).

### 4.5 Overfeed protection (safety — must be enforced for BOTH modes)
- Minimum interval between portions (`MIN_INTERVAL`).
- Maximum portions per day (`MAX_PORTIONS_PER_DAY`).
- A blocked attempt must NOT dispense and should report a reason over BLE / serial.

### 4.6 Wake / sleep flow
- Wake sources: button (GPIO9), and a timer for the next scheduled feed.
- Pattern: wake → (if button) open a short BLE window (e.g. 60 s) for app settings →
  perform any due feed → release coils → boost EN LOW → compute next wake → deep sleep.
- BLE is only available while awake. Accept that an instant app command is not possible
  during deep sleep; the button opens the BLE window. Document this tradeoff in code.

## 5. BLE GATT design (NimBLE)

- Role: GATT **server** (peripheral). Phone is the client.
- Suggested single custom service with these characteristics (use 128-bit UUIDs):
  - **Command** (write): opcodes — `FEED_NOW`, `SET_SCHEDULE`, `SET_TIME` (epoch),
    `SET_PORTION_STEPS` (calibration), `GET_STATUS`.
  - **Status/Notify** (read+notify): last result, portions today, next feed time,
    battery/raw VIN if measured, error/blocked reasons.
- Keep the protocol small and binary or compact JSON; document the byte/field layout in
  comments. Validate every incoming write (length, range) before acting.
- On connect: expect a `SET_TIME` sync; until time is known, schedule mode is "pending".

## 6. Code structure expectations

- Separate concerns into modules/files: `motor` (stepper + anti-jam + release),
  `dosing` (portion logic + safety limits), `schedule`, `ble` (GATT server),
  `power` (boost EN + deep sleep entry/exit), `config` (pins + tunables in one header).
- All tunables (pins, STEPS_PER_PORTION, RPM, MIN_INTERVAL, MAX_PORTIONS_PER_DAY,
  anti-jam params, BLE window length) live in a single `config.h` — no magic numbers
  scattered in logic.
- Persist calibration + schedule + portionsToday across deep sleep. Deep sleep wipes
  RAM except RTC memory → use `RTC_DATA_ATTR` for small state, or NVS (Preferences)
  for settings that must survive full power loss.
- Provide a clean build that compiles under PlatformIO with the NimBLE dependency
  declared in `platformio.ini`.

## 7. Hard rules (the agent must not violate)

1. Dose by steps, never by time.
2. Always release stepper coils after moving.
3. Kill boost EN (GPIO10 LOW) before deep sleep; raise it before driving the motor.
4. Enforce overfeed limits on every dispense path (button, BLE, schedule).
5. Do not hard-cut ESP32 power; only the motor rail is switchable.
6. Use NimBLE, not Bluedroid.
7. Keep 28BYJ-48 at 5V — never 12V (would destroy the motor).
8. Validate all BLE inputs before acting.
9. Keep blocking time minimal; never `delay()` for seconds in the loop.
10. Match the pin map in section 2 exactly.

## 8. Known constraints / gotchas

- No RTC chip: wall-clock relies on BLE time sync; handle "time unknown after reset".
- 28BYJ-48 is weak (~34 mN·m); auger is Ø24 screw in a Ø25 tube, ~60 mm, horizontal,
  pellet size 8–9 mm. If stalls occur, the fix is mechanical/anti-jam, not more current.
- ULN2003 module already wires COM to its V+ (flyback diodes handled on-board) — no
  extra diode needed in firmware reasoning.
- Brown-out risk when the motor starts and pulls through the boost from the same cell;
  a 1000µF bulk cap buffers it. Firmware should still raise boost EN and settle before
  stepping to reduce the transient.

---

## 9. Repository layout

```
CatFeeder/                      ← root = PlatformIO project (open this in VS Code)
├── SKILL.md                    ← this file (agent context + rules)
├── CLAUDE.md                   ← auto-loaded by Claude Code; sources this file
├── platformio.ini              ← PlatformIO build config (envs: c3, s3)
├── include/
│   └── config.h                ← ALL tunables and pin map here
├── src/
│   ├── main.cpp
│   ├── motor.cpp / motor.h
│   ├── dosing.cpp / dosing.h
│   ├── schedule.cpp / schedule.h
│   ├── ble.cpp / ble.h
│   └── power.cpp / power.h
├── schema/
│   ├── CatFeederSchematic.fzz  ← Fritzing (legacy/illustration)
│   └── CatFeeder/              ← KiCad 9.0 project (authoritative schematic)
│       ├── CatFeeder.kicad_sch ← full schematic (source of truth for connections)
│       ├── CatFeeder.kicad_pcb ← PCB layout
│       ├── cat_feeder.csv      ← BOM
│       ├── cat_feeder.pdf      ← rendered schematic PDF
│       └── cat_feeder.net      ← netlist
├── .pio/                       ← build artifacts (git-ignored)
└── .vscode/                    ← auto-generated by PlatformIO IDE (git-ignored)
```

**Hardware stage:** currently breadboard; later all boards and components will sit in
3D-printed slots and be connected by wires. PCB layout in KiCad is prepared but not yet
manufactured.

## 10. PlatformIO build setup (expected platformio.ini)

```ini
[env:esp32-c3-supermini]
platform    = espressif32
board       = esp32-c3-devkitm-1   ; closest upstream board; override pins in config.h
framework   = arduino
monitor_speed = 115200
upload_speed  = 921600

lib_deps =
    h2zero/NimBLE-Arduino @ ^1.4.3  ; use latest stable 1.x; check for breaking changes in 2.x

build_flags =
    -DCORE_DEBUG_LEVEL=3             ; verbose log in debug builds; set to 0 for release
    -DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL=1

; USB Serial/JTAG on ESP32-C3 — no extra UART chip needed
upload_protocol = esptool
monitor_port    = /dev/ttyACM0      ; C3 SuperMini shows up as ACM, not USB0
```

**Note:** ESP32-C3 SuperMini uses native USB-serial (CDC over USB), so the port is
`/dev/ttyACMx`, not `/dev/ttyUSBx`. To flash after deep sleep you may need to hold BOOT
(GPIO9) while pressing RESET to enter download mode.

## 11. Calibration via Serial console

`STEPS_PER_PORTION` must be found empirically by weighing dispensed food. No mobile app
is planned in the near term; calibration is done over the USB Serial monitor in PlatformIO.

**Calibration flow (Serial command `CAL <steps>`):**

1. User sends `CAL <steps>` (e.g. `CAL 512`) over Serial.
2. Firmware runs exactly `<steps>` steps (bypasses overfeed guard — calibration mode only).
3. User weighs the dispensed food and repeats until the desired gram-per-portion is reached.
4. User sends `SAVE` to commit the chosen step count to NVS as `stepsPerPortion`.
5. Firmware echoes back the saved value.

**Implementation notes:**
- Parse Serial input in `loop()` — non-blocking, read until `\n`.
- `CAL` mode flag should prevent the step count from updating `portionsToday` or
  triggering the overfeed limiter.
- Other Serial commands worth supporting: `STATUS` (dump current state), `FEED` (manual
  portion, respects limits), `RESET_DAY` (zero portionsToday for testing).

## 12. BLE GATT — minimal working set

No mobile app is planned now (may appear later in a separate repository). The BLE GATT
server should still be implemented so the device is future-ready. Keep characteristics
generic: **Command** (write) and **Status** (read + notify). The exact binary protocol
can be defined when the app is started. For now, BLE is useful for:
- `FEED_NOW` command from any BLE scanner app (e.g. nRF Connect) during development.
- `SET_TIME` to sync epoch for schedule testing without a custom app.

## 13. Debugging & serial output

- Use `Serial.begin(115200)` in `setup()` — USB CDC on C3 is stable.
- Use `ESP_LOGx` macros (LOGD/LOGI/LOGE) with a module tag, e.g. `ESP_LOGI("motor", "steps=%d", steps)` — controlled by `CORE_DEBUG_LEVEL` build flag, zero overhead in release.
- During development keep `CORE_DEBUG_LEVEL=3`; set to `0` for battery-life measurements.
- To measure actual deep-sleep current: disconnect USB, power from bench supply through a shunt; expect ~50–100 µA (C3 + quiescent LDO). If it's mA-range, a coil is still energised or boost EN is HIGH.

## 14. State persistence across deep sleep

| Data | Size | Storage | Survives power loss? |
|------|------|---------|----------------------|
| `portionsToday`, `lastFeedTime`, `timeKnown` flag | tiny | `RTC_DATA_ATTR` | no (only deep sleep) |
| schedule (times), `stepsPerPortion`, `minInterval`, `maxPortions` | ~dozens bytes | NVS (`Preferences`) | yes |
| wall-clock epoch at last sync | uint32 | `RTC_DATA_ATTR` + NVS | NVS copy for cold boot |

On cold boot (power-on, not deep-sleep wake): restore settings from NVS; `timeKnown = false`
until next BLE `SET_TIME`. Schedule feeds are deferred until time is synced.
