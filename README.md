# Lilum Pendant — ESP32-C3

Firmware for a wearable LED pendant built on a **Seeed XIAO ESP32-C3** (RISC-V,
single-core, 160 MHz, 4 MB flash). The pendant drives a ring of addressable LEDs
with sound- and motion-reactive animations, responds to a button and a tilt
gesture, manages its own battery/charging, and — most distinctively — can
**synchronize its animations wirelessly with other pendants over BLE** as a
master/follower "orchestra."

---

## Table of contents

- [Build & flash](#build--flash)
- [Hardware](#hardware-pin-map)
- [Per-device configuration](#per-device-configuration)
- [Abilities & how to use them](#abilities--how-to-use-them)
  - [LED animations](#1-led-animations)
  - [White / flashlight mode](#2-white--flashlight-mode)
  - [Button controls](#3-button-controls)
  - [Tilt gesture (double-dip)](#4-tilt-gesture-double-dip)
  - [Sound reactivity (microphone)](#5-sound-reactivity-microphone)
  - [BLE animation sync (orchestra)](#6-ble-animation-sync-orchestra)
  - [Battery, charging & power button](#7-battery-charging--power-button)
  - [Serial debug logging](#8-serial-debug-logging)
- [Framework: why ESP-IDF + Arduino?](#framework-why-esp-idf--arduino)
- [Project layout](#project-layout)
- [A note on the sdkconfig files](#a-note-on-the-sdkconfig-files)

---

## Build & flash

The project is built with [PlatformIO](https://platformio.org/) using a hybrid
ESP-IDF + Arduino framework. There is **no custom build script** — the single
build environment, `esp32c3_custom`, is defined in
[platformio.ini](platformio.ini). The platform is pinned to
**`espressif32@6.12.0`**, so the toolchain stays fixed until you bump it
deliberately. Note that because the framework is `espidf, arduino`, PlatformIO
resolves **ESP-IDF 4.4.7** (the version the bundled Arduino-ESP32 core requires)
rather than the 5.5.0 the standalone platform would use.

**In VS Code:** use the PlatformIO toolbar/sidebar buttons directly —
**Build**, **Upload**, **Monitor**. They automatically use the `esp32c3_custom`
environment; nothing extra to configure.

**From the CLI**, the equivalents are:

```bash
# Build
pio run -e esp32c3_custom

# Build, upload, and open the serial monitor
pio run -e esp32c3_custom -t upload -t monitor
```

Serial monitor runs at **115200 baud** with the ESP32 exception decoder enabled.
Console output uses USB-serial-JTAG.

> **First build / fresh clone:** ESP-IDF generates the `sdkconfig` files from
> [sdkconfig.defaults](sdkconfig.defaults) automatically — and regenerates them
> on **every** build. Only `sdkconfig.defaults` is tracked in git; the generated
> snapshots are ignored. See
> [A note on the sdkconfig files](#a-note-on-the-sdkconfig-files).

---

## Hardware (pin map)

| Subsystem        | Interface        | Pins (GPIO)                          | Device                       |
|------------------|------------------|--------------------------------------|------------------------------|
| LED string       | NEOPIXEL (RMT)   | data **10**, string power **21**     | 28× WS2812 (pixel 0 = status)|
| Generic output   | GPIO             | **20**                               | Controllable digital output  |
| IMU              | I2C0 @ 400 kHz   | SDA **6**, SCL **7**, INT1 **5**     | LSM6DS3TR-C accel/gyro (0x6A)|
| Microphone       | I2S0             | BCLK **1**, WS **3**, DIN **4**      | SPH0645 MEMS mic, 44.1 kHz   |
| Battery / power  | I2C0 (shared)    | (shares SDA 6 / SCL 7)               | IP5306 PMIC (0x75)           |
| NTC temperature  | ADC1             | **0**                                | 10 kΩ NTC + 60.4 kΩ pull-up  |
| Button           | GPIO (pull-up)   | **9**                                | Momentary push button        |

> The IMU and battery PMIC share **I2C0**. The IMU is initialised first
> specifically so the bus is up before the battery service talks to the IP5306.

---

## Per-device configuration

### Standalone variant (`esp32c3_custom`)

Each pendant's identity and BLE timing are **compile-time constants** in
[include/orchestra_shared_config.h](include/orchestra_shared_config.h) — edit
these per device before flashing:

| Constant                  | Meaning                                              |
|---------------------------|------------------------------------------------------|
| `ORCHESTRA_ROLE`          | `Master` or `Follower`                               |
| `ORCHESTRA_GROUP_ID`      | Devices in the same group sync together              |
| `ORCHESTRA_DEVICE_ID`     | Unique ID for this device                            |
| `ORCHESTRA_MASTER_ID`     | Which device ID this follower should sync to         |
| `ORCHESTRA_CYCLE_ADV_MS`  | Master advertise window per cycle (default 2000 ms)  |
| `ORCHESTRA_CYCLE_OFF_MS`  | Master quiet window per cycle (default 10000 ms)     |

> A code comment notes the long-term plan is to manage these from a phone app
> and persist them in NVS, replacing the per-build edits.

### Kivsee variant (`esp32c3_kivsee`)

The kivsee build needs WiFi credentials and a per-device **thing name** (used
in MQTT topics and HTTP fetches against the kivsee server).

1. **WiFi credentials** — copy [include/secrets_template.h](include/secrets_template.h)
   to `include/secrets.h` and fill in `SSID` / `WIFI_PASSWORD`. `secrets.h` is
   gitignored.
2. **Thing name** — create `data/thing_info` containing a single line with the
   device's name (e.g. `ring0`). See [data/thing_info.example](data/thing_info.example)
   as a template. `data/` is gitignored except for the example, so each device's
   name stays local. The file is uploaded to the device's SPIFFS partition,
   **separately from the firmware**:

   ```bash
   # Once per device (or whenever you change the thing name):
   pio run -e esp32c3_kivsee -t uploadfs
   ```

   The kivsee task reads `data/thing_info` at boot; without it the task is stuck
   waiting and you'll see no WiFi/MQTT activity in the monitor.
3. **Server IPs** — set in `[env:esp32c3_kivsee]` build flags in
   [platformio.ini](platformio.ini) (`MQTT_BROKER_IP`, `TIME_SERVER_IP`,
   `LED_OBJECT_SERVICE_IP`, `LED_SEQ_SERVICE_IP`).

---

## Abilities & how to use them

### 1. LED animations

Drives 27 animation pixels (pixel 0 is reserved as a status LED) via FastLED.
There are two modes:

- **Auto mode** (default at boot): cycles through 5 patterns —
  `rainbow`, `confetti`, `spinningVortex`, `portal`, `wings`.
- **Manual mode**: holds a single pattern. 13 variants are available, including
  per-color versions (e.g. `portalR/G/B`, `wingsR/G/B`, `spinningVortexR/G/B`).

**How to use:** switch to manual mode and step through patterns with the button
(see [Button controls](#3-button-controls)). In an orchestra, the master's
current pattern is pushed to followers over BLE.

### 2. White / flashlight mode

A solid-white override that ignores animations — useful as a torch. It saves the
current mode/pattern/brightness on entry and restores them on exit.

**How to use:** **double-press** the button, or perform the **double-dip tilt
gesture** (see below). Repeat the same action to exit.

### 3. Button controls

The button on **GPIO 9** is polled every 5 ms with debounce. Actions:

| Action                | Effect                                                              |
|-----------------------|---------------------------------------------------------------------|
| **Short press**       | First press: auto → manual (pattern 0). Then advances manual pattern.|
| **Short press** (white)| Exits white mode.                                                  |
| **Double press**      | Toggles white / flashlight mode.                                    |
| **Long press (hold)** | Ramps brightness: up to max → holds → down to min → holds → repeats.|

Brightness ranges from `LED_BRIGHTNESS_MIN` (40) to `LED_BRIGHTNESS_MAX` (200),
defined in [lib/led_engine/led_engine.h](lib/led_engine/led_engine.h).

### 4. Tilt gesture (double-dip)

The LSM6DS3TR-C accelerometer runs a gesture state machine that detects a
**"double-dip"** motion (two quick tilts). This **toggles white mode**, mirroring
the button double-press.

**How to use:** flick/dip the pendant twice. Gesture recognition can be enabled
or disabled at runtime via `imu_gesture_set_enabled()`. Set `LOG_ENABLE_GESTURE`
in [include/logging_config.h](include/logging_config.h) to watch the state
machine over serial while tuning.

### 5. Sound reactivity (microphone)

An SPH0645 I2S MEMS mic is sampled at 44.1 kHz. The mic service exposes:

- `mic_get_volume()` — smoothed level (0–255)
- `mic_get_beat()` — fires once per detected beat
- `mic_get_bpm()` — estimated tempo

**How to use:** these feed sound-reactive animations. To observe the raw values,
enable `LOG_ENABLE_MIC` in [include/logging_config.h](include/logging_config.h).

### 6. BLE animation sync (orchestra)

Multiple pendants animate in lockstep over BLE (NimBLE), with no pairing. One
device is the **Master**, the rest are **Followers** in the same group.

- The **master** broadcasts a 17-byte advertising payload (group, device, role,
  position, a sync timestamp, and the current animation mode) on a duty cycle
  (e.g. advertise 2 s, quiet 10 s).
- **Followers** scan on the same schedule, lock onto the master's clock, and
  align their animation to the shared timeline.

**How to use:** set `ORCHESTRA_ROLE`, `ORCHESTRA_GROUP_ID`, and the IDs in
[orchestra_shared_config.h](include/orchestra_shared_config.h) for each pendant,
then flash. Sync is automatic on power-up. Enable `LOG_ENABLE_BLE` to watch
advertise/scan/sync status over serial.

### 7. Battery, charging & power button

Battery and power are managed by an IP5306 PMIC over I2C, plus an NTC thermistor
on the ADC for temperature. Capabilities:

- Battery level (≈0/25/50/75/100 %), charging / full / discharging status.
- Temperature monitoring with a safe **0–45 °C** charging range.
- Power-button events: short press, long press, double-click.

**Power-on gate (important):** at boot the LEDs blink while the firmware waits
**up to 4 s** for you to complete a **3 s long-press** on the power button. If
you don't, the firmware drops the IP5306 boost rail and the device powers off.
This prevents accidental power-on in a bag/pocket. Once confirmed, the blink
clears and the rest of the system comes up.

**How to use:** hold the power button ~3 s at power-on to confirm. Enable
`LOG_ENABLE_BATTERY` (on by default) to see level/charge/temperature over serial.

### 8. Serial debug logging

A dedicated print task emits a configurable one-line status to the serial
monitor. Everything is controlled from
[include/logging_config.h](include/logging_config.h):

| Setting               | Purpose                                  | Default |
|-----------------------|------------------------------------------|---------|
| `LOG_INTERVAL_MS`     | How often the line prints                | 1000 ms |
| `LOG_ENABLE_MAIN`     | Role & current animation                 | off     |
| `LOG_ENABLE_BLE`      | Advertise / scan / sync status           | off     |
| `LOG_ENABLE_MIC`      | Volume / BPM / beat                      | off     |
| `LOG_ENABLE_IMU`      | Per-axis acceleration (mg)               | off     |
| `LOG_ENABLE_GESTURE`  | Gesture state machine (for tuning)       | off     |
| `LOG_ENABLE_BATTERY`  | Battery %, charge state, temperature     | **on**  |
| `LOG_ENABLE_BLE_STACK`| Verbose NimBLE / BLE_INIT logs           | off     |

Toggle the sections you care about and rebuild.

---

## Framework: why ESP-IDF + Arduino?

The project declares `framework = espidf, arduino` in
[platformio.ini](platformio.ini). The Arduino layer rides **on top of** ESP-IDF
(ESP-IDF is the real base; Arduino-ESP32 is itself an IDF component). This is a
deliberate split: use Arduino where a library saves real work, and raw ESP-IDF
everywhere precision matters.

**Arduino parts (2 files):**

- [lib/led_engine/led_engine.cpp](lib/led_engine/led_engine.cpp) — uses
  **FastLED**, an Arduino library, plus `pinMode`/`digitalWrite`/`millis()`.
  FastLED is the entire reason Arduino is pulled in; driving WS2812 pixels in
  pure IDF would mean hand-rolling the RMT peripheral.
- [src/main.cpp](src/main.cpp) — calls `initArduino()` to bootstrap the Arduino
  layer, then hands off to ESP-IDF.

**ESP-IDF parts (everything else):** all five components register via
`idf_component_register` and talk directly to IDF drivers:

| Component                                | ESP-IDF APIs used                          |
|------------------------------------------|--------------------------------------------|
| [mic_service](components/mic_service/)   | `driver/i2s.h` (+ direct `I2S0` registers) |
| [battery_service](components/battery_service/) | `driver/i2c.h`, `driver/adc.h`, `esp_adc_cal` |
| [IMU_service](components/IMU_service/)   | `driver/i2c.h`                             |
| [button_service](components/button_service/) | `driver/gpio.h`                        |
| [orchestra_ble](components/orchestra_ble/) | NimBLE (`bt`)                            |

The application skeleton is also IDF-native: the `app_main()` entry point,
FreeRTOS `xTaskCreate` tasks, and `esp_log` (`ESP_LOGI`) throughout — not
Arduino's `setup()`/`loop()`/`Serial`.

**Rule of thumb in this codebase:** *LEDs = Arduino (for FastLED); sensors,
power, radio, and the app skeleton = ESP-IDF.*

---

## Project layout

```
src/main.cpp                  app_main(): boot sequence + LED/print tasks
include/
  orchestra_shared_config.h   per-device identity & BLE timing (edit per device)
  logging_config.h            serial-log toggles
lib/led_engine/               FastLED animations, brightness, white/boot-blink   [Arduino]
components/
  IMU_service/                LSM6DS3TR-C accel + double-dip gesture             [ESP-IDF]
  mic_service/                SPH0645 I2S mic: volume / beat / BPM               [ESP-IDF]
  battery_service/            IP5306 PMIC + NTC temp + power-on gate             [ESP-IDF]
  button_service/             button UX (mode/pattern/brightness/white)          [ESP-IDF]
  orchestra_ble/              NimBLE master/follower animation sync              [ESP-IDF]
platformio.ini                build environment (esp32c3_custom)
sdkconfig.defaults            source-of-truth IDF config
```

### Boot sequence

`app_main()` brings the system up in a deliberate order:

1. NVS init → 2. IMU init (brings up I2C) → 3. Battery/IP5306 config →
4. LED task starts with the **boot-blink overlay** on →
5. **wait for power-on long-press confirmation** (or power off) →
6. BLE → 7. Mic → 8. Button → 9. battery monitor task → 10. print task.

---

## A note on the sdkconfig files

`sdkconfig.defaults` is the **source of truth** — your intentional IDF settings
live here. The per-environment files (`sdkconfig`, `sdkconfig.esp32c3_custom`,
etc.) are **generated by ESP-IDF on every build** by merging `.defaults` with the
installed IDF version's own defaults.

These generated files would otherwise show diffs you didn't author whenever the
PlatformIO `espressif32` platform / IDF version changes — keys get renamed,
obsolete options disappear, and new sections (e.g. camera defaults) appear. To
avoid that churn, **only `sdkconfig.defaults` is tracked in git**; every
generated variant is ignored via [.gitignore](.gitignore):

```gitignore
sdkconfig
sdkconfig.*
!sdkconfig.defaults
```

If you need to change a build setting, edit **`sdkconfig.defaults`** (or adjust
it through `menuconfig`/PlatformIO and copy the intended change into
`.defaults`). Never hand-edit the generated `sdkconfig.<env>` files — they are
overwritten on the next build.
