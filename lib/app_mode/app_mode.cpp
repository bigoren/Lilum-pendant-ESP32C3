// app_mode: NVS-backed boot mode + runtime switch for the esp32c3_kivsee
// build. See app_mode.h for the contract.

#include "app_mode.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "APP_MODE";

#define NVS_NAMESPACE "app_mode"
#define NVS_KEY_MODE  "mode"

// Live RAM mode, set from NVS at boot via app_mode_get_boot() and updated
// by the switch helpers. Starts as STANDALONE so anything that calls
// app_mode_current() before the first NVS read gets the safe default.
static app_mode_t g_current_mode = APP_MODE_STANDALONE;
static app_mode_start_kivsee_fn g_start_kivsee = NULL;

app_mode_t app_mode_get_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS namespace yet -> default STANDALONE");
        g_current_mode = APP_MODE_STANDALONE;
        return g_current_mode;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s) -> default STANDALONE", esp_err_to_name(err));
        g_current_mode = APP_MODE_STANDALONE;
        return g_current_mode;
    }

    uint8_t raw = (uint8_t)APP_MODE_STANDALONE;
    err = nvs_get_u8(handle, NVS_KEY_MODE, &raw);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no mode key yet -> default STANDALONE");
        g_current_mode = APP_MODE_STANDALONE;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8 failed (%s) -> default STANDALONE", esp_err_to_name(err));
        g_current_mode = APP_MODE_STANDALONE;
    } else {
        g_current_mode = (raw == (uint8_t)APP_MODE_KIVSEE) ? APP_MODE_KIVSEE : APP_MODE_STANDALONE;
        ESP_LOGI(TAG, "boot mode from NVS: %s",
                 g_current_mode == APP_MODE_KIVSEE ? "KIVSEE" : "STANDALONE");
    }
    return g_current_mode;
}

bool app_mode_set_persistent(app_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(RW) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(handle, NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist mode=%d failed: %s", (int)mode, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "persisted mode=%s", mode == APP_MODE_KIVSEE ? "KIVSEE" : "STANDALONE");
    return true;
}

app_mode_t app_mode_current(void)
{
    return g_current_mode;
}

void app_mode_install_start_kivsee(app_mode_start_kivsee_fn start)
{
    g_start_kivsee = start;
}

void app_mode_switch_to_kivsee(void)
{
    if (g_current_mode == APP_MODE_KIVSEE) {
        ESP_LOGI(TAG, "switch_to_kivsee: already in kivsee, ignoring");
        return;
    }
    if (g_start_kivsee == NULL) {
        ESP_LOGE(TAG, "switch_to_kivsee: no start hook installed; cannot switch");
        return;
    }
    ESP_LOGI(TAG, "switching STANDALONE -> KIVSEE (in-place)");
    // Persist first so a crash mid-bringup still wakes up in kivsee mode
    // and re-tries (or falls back via the no-hang paths in kivsee_app).
    app_mode_set_persistent(APP_MODE_KIVSEE);
    g_current_mode = APP_MODE_KIVSEE;
    g_start_kivsee();
}

void app_mode_switch_to_standalone(void)
{
    ESP_LOGI(TAG, "switching to STANDALONE (via reboot)");
    app_mode_set_persistent(APP_MODE_STANDALONE);
    // Brief delay so the log flushes and NVS write fully lands before
    // the reset takes the UART down.
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
