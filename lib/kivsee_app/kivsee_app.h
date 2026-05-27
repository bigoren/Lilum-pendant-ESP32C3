#pragma once

// Kivsee networked-animation app (esp32c3_kivsee variant only).
//
// Wraps the WiFi / MQTT / time-sync / segment-store / renderer pipeline that
// originated in the esp32-animations project. The Lilum app_main runs the
// shared boot prefix (power-on gate + sensors), then — in the kivsee variant —
// starts a task that calls kivsee_app_setup() once and kivsee_app_loop()
// repeatedly. This mirrors the Arduino setup()/loop() model used upstream,
// but driven from an explicit FreeRTOS task so the single app_main entry point
// is preserved.

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
