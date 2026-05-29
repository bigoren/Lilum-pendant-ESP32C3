#include <brightness.h>

#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "esp_log.h"

static const char *TAG = "KIVSEE_BR";

bool handleGlobalBrightnessMessage(const byte *payload, unsigned int length, float *new_global_brightness)
{
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        ESP_LOGE(TAG, "deserializeJson() failed: %s", error.f_str());
        return false;
    }

    *new_global_brightness = doc["global_brightness"].as<float>();

    if ((*new_global_brightness < 0.0) || (*new_global_brightness > 1.0))
    {
        ESP_LOGW(TAG, "brightness out of range 0..1: %.3f", *new_global_brightness);
        return false;
    }

    return true;
}