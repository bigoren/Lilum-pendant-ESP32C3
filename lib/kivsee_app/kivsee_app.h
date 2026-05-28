#pragma once

// Kivsee networked-animation app (linked into the esp32c3_kivsee env only;
// the esp32c3_custom env lib_ignores this library).
//
// Wraps the WiFi / MQTT / time-sync / segment-store / renderer pipeline that
// originated in the esp32-animations project. In the esp32c3_kivsee env this
// is started by app_main when the active runtime mode is kivsee (NVS-backed
// app_mode flag — Phase 4). The task calls kivsee_app_setup() once and
// kivsee_app_loop() repeatedly, mirroring the Arduino setup()/loop() model
// upstream, but driven from an explicit FreeRTOS task so app_main remains
// the single entry point.

#ifdef __cplusplus
extern "C" {
#endif

// One-time bring-up: SPIFFS, thing-name, renderer, segment store, MQTT, WiFi,
// OTA, time sync. Safe to call once from the kivsee task before the loop.
void kivsee_app_setup(void);

// One iteration of the networked-animation main loop: WiFi/MQTT upkeep,
// trigger drain, sequence management, and a render frame. Call repeatedly.
void kivsee_app_loop(void);

#ifdef __cplusplus
}
#endif
