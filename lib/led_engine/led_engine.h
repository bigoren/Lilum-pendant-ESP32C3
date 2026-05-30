#pragma once

#include <stdint.h>
#include <stdbool.h>

// Brightness limits
#define LED_BRIGHTNESS_MIN   40
#define LED_BRIGHTNESS_MAX   200
#define LED_BRIGHTNESS_WHITE 120   // brightness used in white-light mode
#define LED_BRIGHTNESS_HOLD_MS 300 // hold duration at min/max during long-press ramp
#define LED_BRIGHTNESS_STEP_MS 30  // interval between brightness increments

#ifdef __cplusplus
extern "C" {
#endif

void led_engine_setup(void);
void led_engine_loop(void);
uint8_t led_engine_get_num_auto_patterns(void);
uint8_t led_engine_get_num_manual_patterns(void);

/// Enter solid-white mode (overrides auto/manual animations).
void led_engine_set_white_mode(bool enable);

/// Returns true when white mode is active.
bool led_engine_is_white_mode(void);

/// Enable a brightness-based blink overlay used during the power-on confirm
/// window. While enabled, the existing animations still run; only the global
/// FastLED brightness is gated on/off in a square wave so the visible effect
/// is the LEDs blinking. Disable when the system has confirmed power-on so
/// animations are shown at their normal brightness.
void led_engine_set_boot_blink(bool enable);

#ifdef LILUM_KIVSEE
// Kivsee env exposes the FastLED buffer so the networked Renderer can fill
// it directly; LED_SRC_KIVSEE skips pattern selection.
typedef enum {
    LED_SRC_FASTLED = 0,
    LED_SRC_KIVSEE  = 1,
} led_src_t;

void      led_engine_set_source(led_src_t src);
led_src_t led_engine_get_source(void);

struct CRGB;
struct CRGB *led_engine_anim_buffer(void);
void         led_engine_anim_commit(void);
int          led_engine_num_anim_leds(void);

// 1 s on / 1 s off brightness gating; signals "trying to connect" visually.
void led_engine_set_connecting_blink(bool enable);

// Kivsee idle indicator: slow green status-pixel blink (1 s on / 1 s off) when
// connected but no animation/trigger is playing. Yields to the charge states.
void led_engine_set_idle_blink(bool enable);
#endif

#ifdef __cplusplus
}
#endif

extern uint8_t g_animation_mode;       // current auto-pattern index (also set by BLE)
extern bool    g_manual_mode_active;   // true = manual single-pattern mode
extern uint8_t g_manual_pattern;       // current manual-pattern index
extern bool    g_output_state;         // GPIO 20 output state (true = HIGH)
extern uint8_t workingBrightness;      // current LED brightness
