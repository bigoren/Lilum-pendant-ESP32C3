#include "lilum_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char *kTag       = "LILUM_NVS";
constexpr const char *kNamespace = "lilum";
constexpr const char *kKeyBind   = "binding";
constexpr const char *kKeyCfg    = "elemcfg";
}  // namespace

extern "C" void lilum_nvs_load_binding(LilumBinding *out)
{
    lilum_binding_clear(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(kTag, "binding: namespace not present yet (rc=%d), using defaults", err);
        return;
    }

    size_t needed = sizeof(LilumBinding);
    LilumBinding tmp;
    err = nvs_get_blob(h, kKeyBind, &tmp, &needed);
    if (err == ESP_OK && needed == sizeof(LilumBinding)) {
        *out = tmp;
        ESP_LOGI(kTag, "binding loaded");
    } else {
        ESP_LOGI(kTag, "binding blob missing/invalid (rc=%d), using defaults", err);
    }
    nvs_close(h);
}

extern "C" void lilum_nvs_save_binding(const LilumBinding *in)
{
    if (!in) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "binding: nvs_open failed: %d", err);
        return;
    }
    err = nvs_set_blob(h, kKeyBind, in, sizeof(*in));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "binding save failed: %d", err);
    } else {
        ESP_LOGI(kTag, "binding saved");
    }
    nvs_close(h);
}

extern "C" void lilum_nvs_load_element_config(LilumElementConfig *out)
{
    lilum_element_config_default(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(kTag, "elemcfg: namespace not present yet (rc=%d), using defaults", err);
        return;
    }

    size_t needed = sizeof(LilumElementConfig);
    LilumElementConfig tmp;
    err = nvs_get_blob(h, kKeyCfg, &tmp, &needed);
    if (err == ESP_OK && needed == sizeof(LilumElementConfig)) {
        *out = tmp;
        ESP_LOGI(kTag, "elemcfg loaded");
    } else {
        ESP_LOGI(kTag, "elemcfg blob missing/invalid (rc=%d), using defaults", err);
    }
    nvs_close(h);
}

extern "C" void lilum_nvs_save_element_config(const LilumElementConfig *in)
{
    if (!in) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "elemcfg: nvs_open failed: %d", err);
        return;
    }
    err = nvs_set_blob(h, kKeyCfg, in, sizeof(*in));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "elemcfg save failed: %d", err);
    } else {
        ESP_LOGI(kTag, "elemcfg saved");
    }
    nvs_close(h);
}
