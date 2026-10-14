// Lilum advertising payload codecs.
//
// Two distinct manufacturer-data layouts coexist on the wire and are
// disambiguated by a one-byte "kind" tag at offset 2 (immediately after the
// company id):
//
//   KIND_CONFIG  (0x01) — 41-byte manufacturer-data field broadcast during the
//       120 s configuration window. Body follows protocol §3.1.
//   KIND_RUNTIME (0x02) — compact manufacturer-data field broadcast by a
//       master outside the config window so followers can mirror its current
//       animation state. Layout is firmware-internal (peer-to-peer only).
//
// All multi-byte integers are little-endian; UUIDs are stored in canonical
// big-endian order. Each layout includes the 2-byte company id at offset 0,
// so the buffers passed below are the full manufacturer-data field as handed
// to NimBLE.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lilum_ble_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LILUM_ADV_KIND_CONFIG   ((uint8_t)0x01)
#define LILUM_ADV_KIND_RUNTIME  ((uint8_t)0x02)

// Total bytes of manufacturer-data for the config-window broadcast:
// 2 (company id) + 1 (kind) + 38 (protocol §3.1 payload) = 41 bytes.
#define LILUM_ADV_CONFIG_MFG_BYTES  (2 + 1 + 38)

// Total bytes of manufacturer-data for the runtime master broadcast:
// 2 (company id) + 1 (kind) + 23 (body) = 26 bytes.
#define LILUM_ADV_RUNTIME_MFG_BYTES (2 + 1 + 23)

// ── Config-window payload (protocol §3.1) ─────────────────────────────────
struct LilumConfigAdvPayload {
    uint16_t element_type_id_hash;
    uint8_t  pair_id[16];                  // all-zero if not paired
    uint8_t  group_id[16];                 // all-zero if not group-bound
    uint8_t  firmware_major;
    uint8_t  firmware_minor;
    uint8_t  firmware_patch;
    uint8_t  config_window_remaining_sec;  // 0..120
};

size_t lilum_encode_config_adv(uint8_t *buf,
                               size_t buf_len,
                               const LilumConfigAdvPayload &payload);
bool   lilum_decode_config_adv(const uint8_t *buf,
                               size_t buf_len,
                               LilumConfigAdvPayload *out);

// ── Runtime master payload (firmware-internal) ────────────────────────────
struct LilumRuntimeAdvPayload {
    uint16_t element_type_id_hash;
    uint8_t  session_kind;            // 0 = pair, 1 = group
    uint8_t  session_id[16];          // pair_id or group_id, as appropriate
    uint32_t sync_timestamp_ms;       // monotonic ms since master boot
    uint8_t  animation_index;
    uint8_t  brightness;
};

size_t lilum_encode_runtime_adv(uint8_t *buf,
                                size_t buf_len,
                                const LilumRuntimeAdvPayload &payload);
bool   lilum_decode_runtime_adv(const uint8_t *buf,
                                size_t buf_len,
                                LilumRuntimeAdvPayload *out);

// Returns the kind tag (LILUM_ADV_KIND_CONFIG or LILUM_ADV_KIND_RUNTIME) if
// the buffer's header matches a Lilum payload, or 0 otherwise.
uint8_t lilum_adv_kind(const uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
