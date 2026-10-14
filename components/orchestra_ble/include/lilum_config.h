// Lilum persistent runtime state: binding (where do I belong) + ElementConfig.
// Lives in NVS, can be re-written by the phone over BLE during the
// configuration window.
#pragma once

#include <stdint.h>
#include <string.h>

#include "lilum_ble_protocol.h"

// ── 8-byte animation tuple (protocol §6.2) ────────────────────────────────
struct LilumAnimationTuple {
    uint8_t base;
    uint8_t symmetry;
    uint8_t params[6];
};
static_assert(sizeof(LilumAnimationTuple) == LILUM_ANIM_TUPLE_BYTES,
              "LilumAnimationTuple must be 8 bytes wire-compatible");

// ── Standalone / pair-shared ElementConfig (decoded view of §6.1 blob) ───
struct LilumElementConfig {
    uint8_t startup_mode;            // LilumStartupMode
    uint8_t startup_brightness;      // 20..100
    uint8_t min_brightness;          // 20..100
    uint8_t max_brightness;          // 20..100, >= min
    uint8_t music_reactive;          // 0/1
    uint8_t beat_reactive;           // 0/1
    uint8_t volume_reactive;         // 0/1
    uint8_t single_press_behavior;   // LilumSinglePress
    uint8_t double_press_behavior;   // LilumDoublePress
    uint32_t flashlight_color;       // ARGB little-endian
    uint8_t flashlight_brightness;   // 20..100

    uint8_t auto_playlist_len;
    LilumAnimationTuple auto_playlist[LILUM_PLAYLIST_MAX_LEN];

    uint8_t manual_playlist_len;
    LilumAnimationTuple manual_playlist[LILUM_PLAYLIST_MAX_LEN];
};

// ── Binding: pair / group affiliation ─────────────────────────────────────
struct LilumBinding {
    uint8_t pair_id[16];      // all-zero if not paired
    uint8_t pair_role;        // LilumPairRole
    uint8_t group_id[16];     // all-zero if not in a group
    uint8_t group_role;       // LilumGroupRole
    uint8_t group_slot_index; // 0..5
};

static inline bool lilum_uuid_is_zero(const uint8_t uuid[16])
{
    for (int i = 0; i < 16; ++i) {
        if (uuid[i] != 0) return false;
    }
    return true;
}

static inline void lilum_binding_clear(LilumBinding *b)
{
    memset(b, 0, sizeof(*b));
    b->pair_role  = LILUM_PAIR_ROLE_NONE;
    b->group_role = LILUM_GROUP_ROLE_FOLLOWER;
}

// Default ElementConfig used at first boot (or after NVS wipe).
static inline void lilum_element_config_default(LilumElementConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->startup_mode           = LILUM_STARTUP_AUTO;
    cfg->startup_brightness     = 60;
    cfg->min_brightness         = 20;
    cfg->max_brightness         = 100;
    cfg->single_press_behavior  = LILUM_SINGLE_SWITCH_THEN_ADVANCE;
    cfg->double_press_behavior  = LILUM_DOUBLE_FLASHLIGHT;
    cfg->flashlight_color       = 0xFFFFFFFFu;  // white
    cfg->flashlight_brightness  = 80;
}

// ── Codec for the variable-length wire blob (protocol §6.1) ───────────────
// Returns the number of bytes consumed on success, 0 on error.
size_t lilum_config_decode(const uint8_t *buf, size_t buf_len, LilumElementConfig *out);

// Returns the number of bytes written on success, 0 on error.
size_t lilum_config_encode(const LilumElementConfig *cfg, uint8_t *buf, size_t buf_len);
