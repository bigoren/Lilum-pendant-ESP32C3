// Lightweight NVS-backed persistence for binding + ElementConfig.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "lilum_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads binding from NVS into `out`; falls back to cleared binding if absent.
void lilum_nvs_load_binding(LilumBinding *out);
void lilum_nvs_save_binding(const LilumBinding *in);

// Loads ElementConfig from NVS into `out`; falls back to defaults if absent.
void lilum_nvs_load_element_config(LilumElementConfig *out);
void lilum_nvs_save_element_config(const LilumElementConfig *in);

#ifdef __cplusplus
}
#endif
