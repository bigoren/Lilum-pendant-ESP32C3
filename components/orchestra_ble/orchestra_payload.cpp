#include "orchestra_payload.h"

#include <string.h>

namespace {

inline void write_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline uint16_t read_u16_le(const uint8_t *src)
{
    return static_cast<uint16_t>(src[0]) |
           (static_cast<uint16_t>(src[1]) << 8);
}

inline void write_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline uint32_t read_u32_le(const uint8_t *src)
{
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline size_t write_header(uint8_t *buf, uint8_t kind)
{
    write_u16_le(buf, LILUM_ADV_COMPANY_ID);
    buf[2] = kind;
    return 3;
}

inline bool check_header(const uint8_t *buf, size_t buf_len, uint8_t expected_kind)
{
    if (buf_len < 3) return false;
    if (read_u16_le(buf) != LILUM_ADV_COMPANY_ID) return false;
    return buf[2] == expected_kind;
}

}  // namespace

size_t lilum_encode_config_adv(uint8_t *buf,
                               size_t buf_len,
                               const LilumConfigAdvPayload &payload)
{
    if (!buf || buf_len < LILUM_ADV_CONFIG_MFG_BYTES) {
        return 0;
    }

    size_t off = write_header(buf, LILUM_ADV_KIND_CONFIG);

    write_u16_le(buf + off, payload.element_type_id_hash);
    off += 2;

    memcpy(buf + off, payload.pair_id, 16);
    off += 16;

    memcpy(buf + off, payload.group_id, 16);
    off += 16;

    buf[off++] = payload.firmware_major;
    buf[off++] = payload.firmware_minor;
    buf[off++] = payload.firmware_patch;
    buf[off++] = payload.config_window_remaining_sec;

    return off;
}

bool lilum_decode_config_adv(const uint8_t *buf,
                             size_t buf_len,
                             LilumConfigAdvPayload *out)
{
    if (!buf || !out) return false;
    if (!check_header(buf, buf_len, LILUM_ADV_KIND_CONFIG)) return false;
    if (buf_len < LILUM_ADV_CONFIG_MFG_BYTES) return false;

    size_t off = 3;
    out->element_type_id_hash = read_u16_le(buf + off);
    off += 2;
    memcpy(out->pair_id, buf + off, 16);
    off += 16;
    memcpy(out->group_id, buf + off, 16);
    off += 16;
    out->firmware_major = buf[off++];
    out->firmware_minor = buf[off++];
    out->firmware_patch = buf[off++];
    out->config_window_remaining_sec = buf[off++];
    return true;
}

size_t lilum_encode_runtime_adv(uint8_t *buf,
                                size_t buf_len,
                                const LilumRuntimeAdvPayload &payload)
{
    if (!buf || buf_len < LILUM_ADV_RUNTIME_MFG_BYTES) {
        return 0;
    }

    size_t off = write_header(buf, LILUM_ADV_KIND_RUNTIME);

    write_u16_le(buf + off, payload.element_type_id_hash);
    off += 2;

    buf[off++] = payload.session_kind;

    memcpy(buf + off, payload.session_id, 16);
    off += 16;

    write_u32_le(buf + off, payload.sync_timestamp_ms);
    off += 4;

    buf[off++] = payload.animation_index;
    buf[off++] = payload.brightness;

    return off;
}

bool lilum_decode_runtime_adv(const uint8_t *buf,
                              size_t buf_len,
                              LilumRuntimeAdvPayload *out)
{
    if (!buf || !out) return false;
    if (!check_header(buf, buf_len, LILUM_ADV_KIND_RUNTIME)) return false;
    if (buf_len < LILUM_ADV_RUNTIME_MFG_BYTES) return false;

    size_t off = 3;
    out->element_type_id_hash = read_u16_le(buf + off);
    off += 2;
    out->session_kind = buf[off++];
    memcpy(out->session_id, buf + off, 16);
    off += 16;
    out->sync_timestamp_ms = read_u32_le(buf + off);
    off += 4;
    out->animation_index = buf[off++];
    out->brightness = buf[off++];
    return true;
}

uint8_t lilum_adv_kind(const uint8_t *buf, size_t buf_len)
{
    if (buf_len < 3) return 0;
    if (read_u16_le(buf) != LILUM_ADV_COMPANY_ID) return 0;
    const uint8_t k = buf[2];
    if (k == LILUM_ADV_KIND_CONFIG || k == LILUM_ADV_KIND_RUNTIME) {
        return k;
    }
    return 0;
}
