#pragma once

// Runtime mode selection for the esp32c3_kivsee build (this library is
// lib_ignore'd by esp32c3_custom). The kivsee binary contains BOTH the
// Lilum-standalone runtime mode (no radio; FastLED patterns + button
// gestures) and the networked kivsee mode (WiFi/MQTT/renderer). Which one
// runs is read from NVS at boot and may be toggled at runtime by the
// triple-short-press gesture (Phase 6).
//
// Switching is asymmetric (see plan inherited-popping-marshmallow.md):
//   standalone -> kivsee : in-place, no reboot (bring up WiFi/MQTT and
//                          flip led_engine source = LED_SRC_KIVSEE).
//   kivsee     -> standalone : persist new mode and esp_restart(). WiFi
//                              and SPIFFS teardown on IDF 4.4.7 is
//                              fragile; a reboot is the robust path.
//
// Default on a fresh flash (no NVS value): APP_MODE_STANDALONE.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_STANDALONE = 0,
    APP_MODE_KIVSEE     = 1,
} app_mode_t;

// Returns the mode persisted in NVS. If the key is absent or NVS read
// fails for any reason, returns APP_MODE_STANDALONE (safe default — the
// standalone mode has no network dependency and cannot hang).
app_mode_t app_mode_get_boot(void);

// Writes the given mode to NVS (synchronously, with nvs_commit). Does not
// change the live RAM mode and does not reboot. Returns true on success.
bool app_mode_set_persistent(app_mode_t mode);

// Returns the mode currently active in RAM (independent of NVS). The
// startup sequence sets this from app_mode_get_boot(); the switch helpers
// update it. Used by handlers (button_service, kivsee_app) to gate
// runtime behavior.
app_mode_t app_mode_current(void);

// Hook installed by app_main so the in-place switch can launch the
// kivsee task without depending on the static task function in main.cpp.
// Call once during startup, before any switch_to_kivsee call.
typedef void (*app_mode_start_kivsee_fn)(void);
void app_mode_install_start_kivsee(app_mode_start_kivsee_fn start);

// In-place transition from standalone -> kivsee. Persists the mode,
// updates the live RAM mode, and calls the installed start-kivsee hook
// (which is responsible for xTaskCreate of the kivsee task and flipping
// led_engine_set_source). No-op if already in kivsee mode.
void app_mode_switch_to_kivsee(void);

// Persists APP_MODE_STANDALONE and calls esp_restart(). Does not return.
// Used both as the user-triggered switch and as the no-hang fallback
// when kivsee cannot come up (missing thing_info or WiFi timeout).
void app_mode_switch_to_standalone(void);

#ifdef __cplusplus
}
#endif
