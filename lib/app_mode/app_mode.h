#pragma once

// NVS-backed runtime mode for the kivsee binary. Asymmetric switching:
// standalone->kivsee is in-place; kivsee->standalone reboots because
// WiFi/SPIFFS teardown on IDF 4.4.7 is fragile.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_STANDALONE = 0,
    APP_MODE_KIVSEE     = 1,
} app_mode_t;

// Returns NVS-persisted mode, or APP_MODE_STANDALONE on absent key / read error.
app_mode_t app_mode_get_boot(void);

// Persists mode with nvs_commit. Does not change live mode or reboot.
bool app_mode_set_persistent(app_mode_t mode);

app_mode_t app_mode_current(void);

// Installed by app_main; called by switch_to_kivsee to launch the task.
typedef void (*app_mode_start_kivsee_fn)(void);
void app_mode_install_start_kivsee(app_mode_start_kivsee_fn start);

void app_mode_switch_to_kivsee(void);
void app_mode_switch_to_standalone(void);  // does not return

#ifdef __cplusplus
}
#endif
