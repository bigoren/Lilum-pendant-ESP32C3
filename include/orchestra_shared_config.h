// Shared defaults used across firmware components.
//
// Pair / group roles and the BLE timing plan that used to live here have
// moved to the Lilum BLE stack and are now persisted in NVS; runtime role
// selection happens in components/orchestra_ble.
#pragma once

#include <stdint.h>

// Index of the default automatic-pattern entry used by led_engine on boot.
static constexpr uint8_t ORCHESTRA_DEFAULT_ANIMATION_MODE = 0;
