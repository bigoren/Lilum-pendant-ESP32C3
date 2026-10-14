#include <Arduino.h>
#include <SPIFFS.h>
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_engine.h"
#ifndef LILUM_KIVSEE
#include "simple_BLE.h"
#else
#include "kivsee_app.h"
#include "app_mode.h"
#endif
#include "button_service.h"
#include "mic_service.h"
#include "IMU_service.h"
#include "battery_service.h"
#include "orchestra_shared_config.h"
#include "logging_config.h"
#include "nvs_flash.h"

static const char *TAG = "MAIN";

uint8_t animationNumber = 0;

// Gesture → white-mode toggle (mirrors button double-tap).
static bool     s_gesture_saved_manual_mode    = false;
static uint8_t  s_gesture_saved_manual_pattern = 0;
static uint8_t  s_gesture_saved_brightness     = LED_BRIGHTNESS_MAX;

static void gesture_enter_white_mode(void)
{
    s_gesture_saved_manual_mode    = g_manual_mode_active;
    s_gesture_saved_manual_pattern = g_manual_pattern;
    s_gesture_saved_brightness     = workingBrightness;
    led_engine_set_white_mode(true);
}

static void gesture_exit_white_mode(void)
{
    led_engine_set_white_mode(false);
    g_manual_mode_active = s_gesture_saved_manual_mode;
    g_manual_pattern     = s_gesture_saved_manual_pattern;
    workingBrightness    = s_gesture_saved_brightness;
}

void ledTask(void *pvParameter) {
    ESP_LOGI(TAG, "LED Task started");
    TickType_t xLastWakeTime = xTaskGetTickCount();

    led_engine_setup();
    while (1) {
        if (imu_gesture_double_dip()) {
            if (led_engine_is_white_mode()) {
                gesture_exit_white_mode();
            } else {
                gesture_enter_white_mode();
            }
        }
        led_engine_loop();
        xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(16));
    }
}

#ifdef LILUM_KIVSEE
static void kivseeTask(void *pvParameter) {
    ESP_LOGI(TAG, "Kivsee Task started");
    kivsee_app_setup();
    while (1) {
        kivsee_app_loop();
    }
}

// app_mode calls this for the in-place STANDALONE → KIVSEE switch.
static void start_kivsee_task(void) {
    ESP_LOGI(TAG, "Launching Kivsee Task (in-place)");
    led_engine_set_source(LED_SRC_KIVSEE);
    xTaskCreate(kivseeTask, "Kivsee Task", 8192, NULL, 1, NULL);
}
#endif

void printTask(void *pvParameter) {
    ESP_LOGI(TAG, "Print Task started");
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        char buf[256];
        int pos = 0;

        if (LOG_ENABLE_MAIN) {
#ifndef LILUM_KIVSEE
            const char *role_str = ble_in_config_window() ? "CfgWin"
                                   : ble_runtime_is_master()   ? "Master"
                                   : ble_runtime_is_follower() ? "Follower"
                                                               : "Idle";
#else
            const char *role_str = app_mode_current() == APP_MODE_KIVSEE ? "Kivsee"
                                                                         : "Standalone";
#endif
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s | Anim: %u",
                            role_str, g_animation_mode);
        }

#ifndef LILUM_KIVSEE
        if (LOG_ENABLE_BLE) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            " | Adv:%s Scan:%s Conn:%s Sync:%s CfgWin:%us",
                            ble_is_currently_advertising() ? "Y" : "N",
                            ble_is_currently_scanning() ? "Y" : "N",
                            ble_is_connected() ? "Y" : "N",
                            ble_is_follower_synced() ? "Y" : "N",
                            (unsigned)ble_config_window_remaining_sec());
        }
#endif

        if (LOG_ENABLE_MIC) {
            uint8_t vol = mic_get_volume();
            bool beat = mic_get_beat();
            uint16_t bpm = mic_get_bpm();
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | Vol: %3u | BPM: %3u%s",
                            vol, bpm, beat ? " <BEAT>" : "");
        }

        if (LOG_ENABLE_IMU) {
            float ax, ay, az;
            imu_get_accel_mg(&ax, &ay, &az);
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | XX:%+7.0f YY:%+7.0f ZZ:%+7.0f mg",
                            ax, ay, az);
        }

        if (LOG_ENABLE_GESTURE) {
            bool dip = imu_gesture_double_dip();
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | G:%-7s%s",
                            imu_gesture_state_str(), dip ? " <<DOUBLE DIP>>" : "");
        }

        if (battery_key_short_press()) {
            ESP_LOGI(TAG, "IP5306 short press → dumping battery drain log");
            battery_drain_log_dump();
        }

        if (LOG_ENABLE_BATTERY) {
            float temp = battery_get_temperature();
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            " | Bat:%3d%% %s T:%.1f°C%s",
                            battery_get_level(),
                            battery_is_full() ? "FULL" : (battery_is_charging() ? "CHG" : "DIS"),
                            temp,
                            battery_temp_in_range() ? "" : " !TEMP");
        }

        ESP_LOGI(TAG, "%s", buf);
        xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(LOG_INTERVAL_MS));
    }
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(100));  // Give serial a moment to initialize
    ESP_LOGI(TAG, ">>> APP MAIN START <<<");

    ESP_LOGI(TAG, "Calling initArduino...");
    initArduino();
    ESP_LOGI(TAG, "initArduino done");

    int rc;

    // NVS must be ready before BLE.
    ESP_LOGI(TAG, "Initializing NVS");
    rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rc);
    ESP_LOGI(TAG, "NVS initialized");

    ESP_LOGI(TAG, "Starting IMU Service (brings up I2C)");
    imu_service_init();

    ESP_LOGI(TAG, "Initialising Battery Service (IP5306)");
    battery_service_init();

    ESP_LOGI(TAG, "Starting LED Task");
    xTaskCreate(ledTask, // Task function
         "LED Task", // Name of the task
        4096, // Stack size in bytes
        &animationNumber, // Pass pointer to global variable
        1, // Task priority
        NULL // Task handle
    );

#ifndef LILUM_KIVSEE
    ESP_LOGI(TAG, "Calling ble_init...");
    ble_init();
    ble_set_logging(false);
    if (!LOG_ENABLE_BLE_STACK) {
        esp_log_level_set("NimBLE", ESP_LOG_WARN);
        esp_log_level_set("BLE_INIT", ESP_LOG_WARN);
    }
    ESP_LOGI(TAG, "ble_init completed");
#else
    app_mode_install_start_kivsee(start_kivsee_task);
    const bool kivsee_boot = (app_mode_get_boot() == APP_MODE_KIVSEE);
    if (kivsee_boot) {
        ESP_LOGI(TAG, "Boot mode: KIVSEE");
        led_engine_set_source(LED_SRC_KIVSEE);
        xTaskCreate(kivseeTask, "Kivsee Task", 8192, NULL, 1, NULL);
    } else {
        ESP_LOGI(TAG, "Boot mode: STANDALONE");
    }
#endif

    // Mic is only useful in standalone mode (beat-sync animations).
    // Skipping it in kivsee mode saves ~1 mA and removes the I2S APB lock,
    // which would otherwise block tickless light sleep.
#ifdef LILUM_KIVSEE
    if (!kivsee_boot) {
        ESP_LOGI(TAG, "Starting Mic Service");
        mic_service_init();
    } else {
        ESP_LOGI(TAG, "Mic Service skipped (kivsee mode)");
    }
#else
    ESP_LOGI(TAG, "Starting Mic Service");
    mic_service_init();
#endif

    ESP_LOGI(TAG, "Starting Button Service");
    button_service_init();

    if (!SPIFFS.begin(true)) {
        ESP_LOGE(TAG, "SPIFFS mount failed — battery drain log unavailable");
    }
    battery_drain_log_dump();
    battery_drain_log_boot();

    ESP_LOGI(TAG, "Starting Battery monitor task");
    battery_service_start();

    ESP_LOGI(TAG, "Starting Print Task");
    xTaskCreate(printTask, // Task function
         "Print Task", // Name of the task
        3072, // Stack size in bytes
        NULL, // Parameter to pass to the task
        1, // Task priority
        NULL // Task handle
    );
}

