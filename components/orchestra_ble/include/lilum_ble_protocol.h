// Lilum BLE protocol constants for the Mandalum pendant firmware.
// Derived from firmware-ble-protocol.md (single source of truth).
#pragma once

#include <stdint.h>

// ── Element identity ──────────────────────────────────────────────────────
// String id reported via Identity char FF11 and used by phone-side OTA repo
// selection. The 16-bit hash (Java String.hashCode() & 0xFFFF) is embedded in
// the manufacturer-specific advertising payload (offset 0).
#define LILUM_ELEMENT_TYPE_ID         "LILUM_MANDALUM_PENDANT_ESP32C3_V1"
#define LILUM_ELEMENT_TYPE_ID_HASH16  ((uint16_t)0x8C26)  // precomputed, see protocol doc §3.2

#define LILUM_HARDWARE_REVISION       "rev-A"
#define LILUM_SERIAL_NUMBER           "MANDA-0001"

#define LILUM_FW_VERSION_MAJOR        1
#define LILUM_FW_VERSION_MINOR        0
#define LILUM_FW_VERSION_PATCH        0

// ── Advertising ───────────────────────────────────────────────────────────
// Manufacturer ID for dev (Bluetooth SIG "no company assigned" sentinel).
#define LILUM_ADV_COMPANY_ID          ((uint16_t)0xFFFF)

// 38-byte manufacturer-specific data layout (protocol §3.1, little-endian).
#define LILUM_ADV_MFG_DATA_LEN        38

// Configuration window length, measured from boot. While this window is open
// the element is connectable; once a phone connects the timer is suspended
// indefinitely.
#define LILUM_CONFIG_WINDOW_SEC       120

// Advertising interval during config window: 100 ms ± a touch of jitter.
#define LILUM_ADV_ITVL_MIN_0_625      160   // 100.0 ms
#define LILUM_ADV_ITVL_MAX_0_625      176   // 110.0 ms

// ── 128-bit Service UUIDs (placeholder set, mirrored on phone side) ───────
// Stored here in the byte order expected by NimBLE's ble_uuid128_t (LSB first).
// Canonical form for human eyes is e.g. 0000FF00-0000-1000-8000-00805F9B34FB.

#define LILUM_UUID128_BYTES(suffix_hi, suffix_lo)                              \
    { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,                          \
      0x00, 0x10, 0x00, 0x00, (suffix_lo), (suffix_hi), 0x00, 0x00 }

// Services (suffix is the 16-bit portion of `0000XXXX-0000-1000-8000-...`)
#define LILUM_SVC_PRIMARY_BYTES   LILUM_UUID128_BYTES(0xFF, 0x00)
#define LILUM_SVC_IDENTITY_BYTES  LILUM_UUID128_BYTES(0xFF, 0x01)
#define LILUM_SVC_BINDING_BYTES   LILUM_UUID128_BYTES(0xFF, 0x02)
#define LILUM_SVC_CONFIG_BYTES    LILUM_UUID128_BYTES(0xFF, 0x03)
#define LILUM_SVC_LIVE_BYTES      LILUM_UUID128_BYTES(0xFF, 0x04)

// Identity chars
#define LILUM_CHR_IDENTITY_TYPE_BYTES   LILUM_UUID128_BYTES(0xFF, 0x11)
#define LILUM_CHR_IDENTITY_FWVER_BYTES  LILUM_UUID128_BYTES(0xFF, 0x12)
#define LILUM_CHR_IDENTITY_HWREV_BYTES  LILUM_UUID128_BYTES(0xFF, 0x13)
#define LILUM_CHR_IDENTITY_SERIAL_BYTES LILUM_UUID128_BYTES(0xFF, 0x14)
// Config-window info (replaces 41-byte mfg-data adv field; readable only
// while the 120 s configuration window is open). Contains the same 41-byte
// payload that used to be broadcast in extended advertising:
//   [0..1]   company id (LE)
//   [2]      kind tag (LILUM_ADV_KIND_CONFIG = 0x01)
//   [3..40]  LilumConfigAdvPayload body (protocol §3.1)
#define LILUM_CHR_IDENTITY_CONFIG_INFO_BYTES LILUM_UUID128_BYTES(0xFF, 0x15)

// Binding chars
#define LILUM_CHR_BINDING_PAIR_ID_BYTES    LILUM_UUID128_BYTES(0xFF, 0x21)
#define LILUM_CHR_BINDING_PAIR_ROLE_BYTES  LILUM_UUID128_BYTES(0xFF, 0x22)
#define LILUM_CHR_BINDING_GROUP_ID_BYTES   LILUM_UUID128_BYTES(0xFF, 0x23)
#define LILUM_CHR_BINDING_GROUP_ROLE_BYTES LILUM_UUID128_BYTES(0xFF, 0x24)
#define LILUM_CHR_BINDING_SLOT_BYTES       LILUM_UUID128_BYTES(0xFF, 0x25)

// Configuration chars
#define LILUM_CHR_CFG_STANDALONE_BYTES LILUM_UUID128_BYTES(0xFF, 0x31)
#define LILUM_CHR_CFG_SHARED_BYTES     LILUM_UUID128_BYTES(0xFF, 0x32)

// Live Command chars
#define LILUM_CHR_LIVE_BRIGHTNESS_BYTES LILUM_UUID128_BYTES(0xFF, 0x41)
#define LILUM_CHR_LIVE_PREVIEW_BYTES    LILUM_UUID128_BYTES(0xFF, 0x42)
#define LILUM_CHR_LIVE_FLASHLIGHT_BYTES LILUM_UUID128_BYTES(0xFF, 0x43)
#define LILUM_CHR_LIVE_APPLY_EXIT_BYTES LILUM_UUID128_BYTES(0xFF, 0x44)

// ── Roles encoded on the wire (Binding service) ───────────────────────────
enum LilumPairRole : uint8_t {
    LILUM_PAIR_ROLE_NONE     = 0,
    LILUM_PAIR_ROLE_MASTER   = 1,
    LILUM_PAIR_ROLE_FOLLOWER = 2,
};

enum LilumGroupRole : uint8_t {
    LILUM_GROUP_ROLE_FOLLOWER = 0,
    LILUM_GROUP_ROLE_MASTER   = 1,
};

// ── Configuration blob (Configuration service FF31/FF32) ──────────────────
// Variable-length wire format, see protocol §6.1. The fixed prefix is 14 B,
// followed by `1 + 8*N1` (auto playlist) and `1 + 8*N2` (manual playlist).
// Worst case = 14 + 1 + 15*8 + 1 + 15*8 = 256 bytes.

#define LILUM_PLAYLIST_MAX_LEN    15
#define LILUM_ANIM_TUPLE_BYTES    8
#define LILUM_CONFIG_BLOB_MAX_LEN (14 + 1 + LILUM_PLAYLIST_MAX_LEN * LILUM_ANIM_TUPLE_BYTES \
                                       + 1 + LILUM_PLAYLIST_MAX_LEN * LILUM_ANIM_TUPLE_BYTES)

enum LilumStartupMode : uint8_t {
    LILUM_STARTUP_AUTO   = 0,
    LILUM_STARTUP_MANUAL = 1,
};

enum LilumSinglePress : uint8_t {
    LILUM_SINGLE_SWITCH_THEN_ADVANCE = 0,
    LILUM_SINGLE_ALWAYS_ADVANCE      = 1,
};

enum LilumDoublePress : uint8_t {
    LILUM_DOUBLE_FLASHLIGHT = 0,
    LILUM_DOUBLE_DISABLED   = 1,
};
