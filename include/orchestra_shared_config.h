#pragma once

#include <stdint.h>

enum class OrchestraRole : uint8_t {
    Follower = 0,
    Master = 1,
};

// Orchestra node configuration.
// Update these values per device before flashing. In a future revision these
// will be managed from a phone app and persisted in non-volatile storage.
static constexpr uint16_t ORCHESTRA_GROUP_ID = 1;
static constexpr uint16_t ORCHESTRA_DEVICE_ID = 2;
static constexpr OrchestraRole ORCHESTRA_ROLE = OrchestraRole::Master;  // Set to Master for the device that will control the animation and BLE advertising.

// Total number of players (devices) in the orchestra, including the master.
static constexpr uint16_t ORCHESTRA_TOTAL_PLAYERS = 1;

// Position of this device within the orchestra, used as a spatial phase
// reference for multi-element animations. The master is always position 0 (the
// first position); each follower must be assigned a unique position in the
// range 1 .. ORCHESTRA_TOTAL_PLAYERS - 1.
static constexpr uint16_t ORCHESTRA_POSITION =
    (ORCHESTRA_ROLE == OrchestraRole::Master) ? 0 : 2;
static constexpr uint16_t ORCHESTRA_MASTER_ID = 2;

// BLE timing plan (milliseconds).
// Master: advertise connectable for ORCHESTRA_STARTUP_WINDOW_MS so a phone can
//   connect via GATT. If no connection is made in that window, switch to
//   non-connectable advertising and keep broadcasting state indefinitely.
// Follower: scan continuously (indefinitely) and is not connectable; it syncs
//   to the master's timestamp from the received advertisements.
static constexpr uint32_t ORCHESTRA_STARTUP_WINDOW_MS = 60000;

// Shared behavior defaults.
static constexpr uint8_t ORCHESTRA_DEFAULT_ANIMATION_MODE = 0;

static inline const char *orchestra_role_to_string(OrchestraRole role)
{
    return role == OrchestraRole::Master ? "master" : "follower";
}