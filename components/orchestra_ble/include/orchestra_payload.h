#pragma once

#include <stddef.h>
#include <stdint.h>

#include "orchestra_config.h"

struct OrchestraAdvPayload {
    uint16_t group_id;
    uint16_t device_id;
    OrchestraRole role;
    int16_t position;
    uint16_t master_id;
    uint32_t sync_timestamp_ms;
    uint8_t animation_mode;
    uint8_t manual_mode_active;  // Non-zero when the master is in manual single-pattern mode.
    uint8_t manual_pattern;      // Selected manual-pattern index (valid when manual_mode_active).
    uint8_t music_bpm;           // Beat tempo from the master's microphone (0 if unknown).
    uint8_t music_volume;        // Live volume level (0-255) from the master's microphone.
};

static constexpr size_t ORCHESTRA_ADV_PAYLOAD_LEN = 21;

size_t orchestra_encode_adv_payload(uint8_t *buffer, size_t buffer_len, const OrchestraAdvPayload &payload);
bool orchestra_decode_adv_payload(const uint8_t *buffer, size_t buffer_len, OrchestraAdvPayload *out_payload);
