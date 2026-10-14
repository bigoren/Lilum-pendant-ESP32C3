// Decoder/encoder for the variable-length ElementConfig wire blob
// (firmware-ble-protocol.md §6.1). The decoder is strict: any length mismatch
// or out-of-range value causes a rejection so callers can NACK the BLE write.
#include "lilum_config.h"

#include <stdint.h>
#include <string.h>

namespace {

inline uint32_t read_u32_le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline bool in_range_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v >= lo && v <= hi;
}

}  // namespace

size_t lilum_config_decode(const uint8_t *buf, size_t buf_len, LilumElementConfig *out)
{
    if (!buf || !out) return 0;
    if (buf_len < 14) return 0;

    LilumElementConfig tmp = {};

    tmp.startup_mode = buf[0];
    if (tmp.startup_mode > LILUM_STARTUP_MANUAL) return 0;

    tmp.startup_brightness = buf[1];
    tmp.min_brightness     = buf[2];
    tmp.max_brightness     = buf[3];
    if (!in_range_u8(tmp.startup_brightness, 20, 100)) return 0;
    if (!in_range_u8(tmp.min_brightness, 20, 100))     return 0;
    if (!in_range_u8(tmp.max_brightness, 20, 100))     return 0;
    if (tmp.max_brightness < tmp.min_brightness)       return 0;

    tmp.music_reactive  = buf[4] ? 1 : 0;
    tmp.beat_reactive   = buf[5] ? 1 : 0;
    tmp.volume_reactive = buf[6] ? 1 : 0;

    tmp.single_press_behavior = buf[7];
    tmp.double_press_behavior = buf[8];
    if (tmp.single_press_behavior > LILUM_SINGLE_ALWAYS_ADVANCE) return 0;
    if (tmp.double_press_behavior > LILUM_DOUBLE_DISABLED)       return 0;

    tmp.flashlight_color      = read_u32_le(buf + 9);
    tmp.flashlight_brightness = buf[13];
    if (!in_range_u8(tmp.flashlight_brightness, 20, 100)) return 0;

    size_t off = 14;

    if (off >= buf_len) return 0;
    tmp.auto_playlist_len = buf[off++];
    if (tmp.auto_playlist_len > LILUM_PLAYLIST_MAX_LEN) return 0;
    if (off + tmp.auto_playlist_len * LILUM_ANIM_TUPLE_BYTES > buf_len) return 0;
    for (uint8_t i = 0; i < tmp.auto_playlist_len; ++i) {
        LilumAnimationTuple &t = tmp.auto_playlist[i];
        t.base     = buf[off + 0];
        t.symmetry = buf[off + 1];
        for (int p = 0; p < 6; ++p) t.params[p] = buf[off + 2 + p];
        off += LILUM_ANIM_TUPLE_BYTES;
    }

    if (off >= buf_len) return 0;
    tmp.manual_playlist_len = buf[off++];
    if (tmp.manual_playlist_len > LILUM_PLAYLIST_MAX_LEN) return 0;
    if (off + tmp.manual_playlist_len * LILUM_ANIM_TUPLE_BYTES > buf_len) return 0;
    for (uint8_t i = 0; i < tmp.manual_playlist_len; ++i) {
        LilumAnimationTuple &t = tmp.manual_playlist[i];
        t.base     = buf[off + 0];
        t.symmetry = buf[off + 1];
        for (int p = 0; p < 6; ++p) t.params[p] = buf[off + 2 + p];
        off += LILUM_ANIM_TUPLE_BYTES;
    }

    *out = tmp;
    return off;
}

size_t lilum_config_encode(const LilumElementConfig *cfg, uint8_t *buf, size_t buf_len)
{
    if (!cfg || !buf) return 0;
    if (cfg->auto_playlist_len > LILUM_PLAYLIST_MAX_LEN)  return 0;
    if (cfg->manual_playlist_len > LILUM_PLAYLIST_MAX_LEN) return 0;

    const size_t need = 14 + 1 + cfg->auto_playlist_len * LILUM_ANIM_TUPLE_BYTES
                              + 1 + cfg->manual_playlist_len * LILUM_ANIM_TUPLE_BYTES;
    if (buf_len < need) return 0;

    buf[0]  = cfg->startup_mode;
    buf[1]  = cfg->startup_brightness;
    buf[2]  = cfg->min_brightness;
    buf[3]  = cfg->max_brightness;
    buf[4]  = cfg->music_reactive ? 1 : 0;
    buf[5]  = cfg->beat_reactive ? 1 : 0;
    buf[6]  = cfg->volume_reactive ? 1 : 0;
    buf[7]  = cfg->single_press_behavior;
    buf[8]  = cfg->double_press_behavior;
    write_u32_le(buf + 9, cfg->flashlight_color);
    buf[13] = cfg->flashlight_brightness;

    size_t off = 14;

    buf[off++] = cfg->auto_playlist_len;
    for (uint8_t i = 0; i < cfg->auto_playlist_len; ++i) {
        const LilumAnimationTuple &t = cfg->auto_playlist[i];
        buf[off + 0] = t.base;
        buf[off + 1] = t.symmetry;
        for (int p = 0; p < 6; ++p) buf[off + 2 + p] = t.params[p];
        off += LILUM_ANIM_TUPLE_BYTES;
    }

    buf[off++] = cfg->manual_playlist_len;
    for (uint8_t i = 0; i < cfg->manual_playlist_len; ++i) {
        const LilumAnimationTuple &t = cfg->manual_playlist[i];
        buf[off + 0] = t.base;
        buf[off + 1] = t.symmetry;
        for (int p = 0; p < 6; ++p) buf[off + 2 + p] = t.params[p];
        off += LILUM_ANIM_TUPLE_BYTES;
    }

    return off;
}
