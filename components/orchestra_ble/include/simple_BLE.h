// Public API for the Lilum BLE stack on this element.
//
// Boot sequence (driven from main.cpp):
//   1. ble_init() initializes NimBLE and starts the configuration window.
//      The element advertises connectable extended advertisements for
//      LILUM_CONFIG_WINDOW_SEC, accepting at most one phone connection at a
//      time. The window timer is paused while a phone is connected.
//   2. The window ends when:
//        - the 120 s timer expires without any connection (Option C, PRD §4.4)
//        - the phone writes "apply and exit" via the Live Command service
//        - a phone disconnects mid-session for any reason
//      After exit, the element transitions to its persisted runtime duty
//      (master advertises, follower scans, standalone is BLE-idle) and is
//      no longer connectable until the next power-cycle.
//
// All "power-saving" cyclic advertising / scanning has been deliberately
// removed: while in runtime master, the radio advertises continuously; while
// in runtime follower, the radio scans continuously.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_init(void);
void ble_set_logging(bool enabled);

// Status helpers exposed for the diagnostics print task.
bool     ble_is_currently_advertising(void);
bool     ble_is_currently_scanning(void);
bool     ble_is_connected(void);
bool     ble_in_config_window(void);
uint8_t  ble_config_window_remaining_sec(void);
bool     ble_runtime_is_master(void);
bool     ble_runtime_is_follower(void);
bool     ble_is_follower_synced(void);

// Returns the effective runtime brightness commanded by the most recent
// setLiveBrightness write while a phone is connected, or 0 if no live override
// is active. The LED engine should consult this when non-zero.
uint8_t  ble_live_brightness_override(void);

#ifdef __cplusplus
}
#endif
