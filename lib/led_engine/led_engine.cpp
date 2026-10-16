#include <Arduino.h>
#include <FastLED.h>
#include "esp_log.h"

#include "led_engine.h"
#include "orchestra_shared_config.h"
#include "battery_service.h"

// ---------------------------------------------------------------------------
// Hardware layout
// ---------------------------------------------------------------------------
// Total physical LEDs on the data line: 1 status + 27 animation
#define TOTAL_LEDS 28
#define DATA_PIN 10

// Status LED is index 0; animation LEDs are indices 1..27
#define STATUS_LED_INDEX 0
#define ANIM_LED_START   1
#define NUM_ANIM_LEDS    27

// Controllable digital output on GPIO 20
#define OUTPUT_PIN 20
#define LED_POWER_PIN 21    // GPIO21 controls power to LED string (not pixel 0)
bool g_output_state = true; // starts HIGH

// Minimum time (ms) the LED-power state must be held before it can transition
#define LED_POWER_HOLD_MS  5000
static bool          s_led_power_on        = true;  // committed power state
static unsigned long s_led_power_change_ms = 0;      // millis() of last transition

uint8_t g_animation_mode = ORCHESTRA_DEFAULT_ANIMATION_MODE;
bool    g_manual_mode_active = false;
uint8_t g_manual_pattern = 0;

CRGB leds[TOTAL_LEDS];

// Convenience pointer into the animation portion of the strip
static CRGB *anim = &leds[ANIM_LED_START];

#ifdef LILUM_KIVSEE
// Double-buffer: renderer always writes to s_render_buf, LED task always reads
// from s_display_buf.  led_engine_anim_commit() swaps the pointers atomically so
// the two tasks never touch the same buffer simultaneously.
static CRGB  s_buf_a[NUM_ANIM_LEDS];
static CRGB  s_buf_b[NUM_ANIM_LEDS];
static CRGB *volatile s_render_buf  = s_buf_a;   // renderer writes here
static CRGB *volatile s_display_buf = s_buf_b;   // LED task reads here
static volatile bool  s_anim_shadow_ready = false;
#endif

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------
uint8_t workingBrightness      = 60;
static const uint8_t ZERO_CLAMP = 40;

// ---------------------------------------------------------------------------
// White-light mode
// ---------------------------------------------------------------------------
static bool g_white_mode = false;

// ---------------------------------------------------------------------------
// Auto-mode pattern cycling
// ---------------------------------------------------------------------------
// While in auto mode, advance to the next auto pattern every
// AUTO_CYCLE_INTERVAL_MS. Paused in white/manual mode and while the kivsee
// renderer owns the LEDs.
#define AUTO_CYCLE_INTERVAL_MS 30000
static unsigned long s_last_auto_cycle_ms = 0;

#ifdef LILUM_KIVSEE
static bool g_connecting_blink_active = false;
#define CONNECTING_BLINK_PERIOD_MS  2000
// Main-LED connecting indicator: one blue dot stepping around the outer ring.
#define CONNECTING_DOT_RING_LEDS    9     // anim[0..8] = outer ring
#define CONNECTING_DOT_STEP_MS      150   // full lap ≈ 1.35 s
#define CONNECTING_DOT_LEVEL        128   // blue only, further scaled by workingBrightness

// Connected-idle indicator: the same dot in green, slower, fading pixel to pixel.
#define IDLE_DOT_STEP_MS            1000  // per-pixel crossfade; full lap ≈ 9 s

// Kivsee idle: no animation/trigger is playing. Gates the ring dot (blue while
// connecting, green once connected) so it never draws over an animation.
static bool g_idle_blink_active = false;

static led_src_t g_led_source = LED_SRC_FASTLED;
#endif

// ---------------------------------------------------------------------------
// Mapping arrays  (operate on the 27 animation LEDs)
// ---------------------------------------------------------------------------
#define NUM_LEDS_MAP1 3
#define NUM_LEDS_MAP3 4  // indices 0..3
static CRGB ledsMap1[NUM_LEDS_MAP1];
static CRGB ledsMap3[NUM_LEDS_MAP3];

static uint8_t map1Mapping[27] = {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2};
static uint8_t map2brightness[27] = {0,1,0,0,1,0,0,1,0,0,1,1,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1};
static uint8_t map3Mapping[27] = {0,1,0,0,1,0,0,1,0,0,2,1,0,2,1,0,2,1,3,2,1,3,2,1,3,2,1};

// ---------------------------------------------------------------------------
// Global hue counter (incremented from led_engine_loop)
// ---------------------------------------------------------------------------
static uint8_t gHueModifier = 0;
static unsigned long lastHueModTime = 0;

// ---------------------------------------------------------------------------
// Forward declarations – patterns
// ---------------------------------------------------------------------------
static void rainbowMap1();
static void confetti();
static void confettiFlowerB();
static void confettiFlowerR();
static void spinningVortex();
static void spinningVortexB();
static void spinningVortexG();
static void spinningVortexR();
static void portal();
static void portalB();
static void portalG();
static void portalR();
static void wings();
static void wingsG();
static void wingsR();
static void wingsB();
static void beacon();
static void sinelonUneven();
static void curtainCenter();
static void curtainEdges();
static void heartbeat();
static void sweep();
static void flower();

// ---------------------------------------------------------------------------
// Pattern list
// ---------------------------------------------------------------------------
// One list serves both modes: auto mode advances through it every
// AUTO_CYCLE_INTERVAL_MS, and in manual mode each short button press steps to
// the next entry. The name is logged on every change.
struct Pattern {
    void (*fn)();
    const char *name;
};

static const Pattern gPatterns[] = {
    { rainbowMap1,     "rainbowMap1" },
    { confetti,        "confetti" },
    { confettiFlowerB, "confettiFlowerB" },
    { confettiFlowerR, "confettiFlowerR" },
    { spinningVortex,  "spinningVortex" },
    { spinningVortexB, "spinningVortexB" },
    { spinningVortexG, "spinningVortexG" },
    { spinningVortexR, "spinningVortexR" },
    { portal,          "portal" },
    { portalB,         "portalB" },
    { portalG,         "portalG" },
    { portalR,         "portalR" },
    { wings,           "wings" },
    { wingsG,          "wingsG" },
    { wingsR,          "wingsR" },
    { wingsB,          "wingsB" },
    { beacon,          "beacon" },
    { sinelonUneven,   "sinelonUneven" },
    { curtainCenter,   "curtainCenter" },
    { curtainEdges,    "curtainEdges" },
    { heartbeat,       "heartbeat" },
    { sweep,           "sweep" },
    { flower,          "flower" },
};
static constexpr uint8_t NUM_PATTERNS = sizeof(gPatterns) / sizeof(gPatterns[0]);

// ---------------------------------------------------------------------------
// Lookup tables for custom beat functions
// ---------------------------------------------------------------------------
static const uint8_t tri1_lut[256] = {
    0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 5, 6, 6, 8, 9,
    10, 11, 12, 14, 15, 17, 18, 20, 22, 23, 25, 27, 29, 31, 33, 35,
    38, 40, 42, 45, 47, 49, 52, 54, 57, 60, 62, 65, 68, 71, 73, 76,
    79, 82, 85, 88, 91, 94, 97, 100, 103, 106, 109, 113, 116, 119, 122, 125,
    128, 131, 135, 138, 141, 144, 147, 150, 153, 156, 159, 162, 165, 168, 171, 174,
    177, 180, 183, 186, 189, 191, 194, 197, 199, 202, 204, 207, 209, 212, 214, 216,
    218, 221, 223, 225, 227, 229, 231, 232, 234, 236, 238, 239, 241, 242, 243, 245,
    246, 247, 248, 249, 250, 251, 252, 252, 253, 253, 254, 254, 255, 255, 255, 255,
    255, 255, 255, 255, 254, 254, 253, 253, 252, 252, 251, 250, 249, 248, 247, 246,
    245, 243, 242, 241, 239, 238, 236, 234, 232, 231, 229, 227, 225, 223, 221, 218,
    216, 214, 212, 209, 207, 204, 202, 199, 197, 194, 191, 189, 186, 183, 180, 177,
    174, 171, 168, 165, 162, 159, 156, 153, 150, 147, 144, 141, 138, 135, 131, 128,
    125, 122, 119, 116, 113, 109, 106, 103, 100, 97, 94, 91, 88, 85, 82, 79,
    76, 73, 71, 68, 65, 62, 60, 57, 54, 52, 49, 47, 45, 42, 40, 38,
    35, 33, 31, 29, 27, 25, 23, 22, 20, 18, 17, 15, 14, 12, 11, 10,
    9, 8, 6, 6, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 0, 0
};

static const uint8_t tri2_lut[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2,
    3, 4, 5, 6, 8, 9, 11, 13, 16, 18, 20, 23, 26, 29, 32, 35,
    38, 42, 45, 49, 53, 56, 60, 64, 69, 73, 77, 81, 86, 90, 95, 99,
    104, 109, 113, 118, 123, 128, 132, 137, 142, 146, 151, 156, 160, 165, 169, 174,
    178, 182, 186, 191, 195, 199, 202, 206, 210, 213, 217, 220, 223, 226, 229, 232,
    235, 237, 239, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 254, 255, 255,
    255, 255, 254, 254, 253, 252, 251, 250, 249, 247, 246, 244, 242, 239, 237, 235,
    232, 229, 226, 223, 220, 217, 213, 210, 206, 202, 199, 195, 191, 186, 182, 178,
    174, 169, 165, 160, 156, 151, 146, 142, 137, 132, 128, 123, 118, 113, 109, 104,
    99, 95, 90, 86, 81, 77, 73, 69, 64, 60, 56, 53, 49, 45, 42, 38,
    35, 32, 29, 26, 23, 20, 18, 16, 13, 11, 9, 8, 6, 5, 4, 3,
    2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t tri3_lut[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 3, 4, 6, 8, 10, 13, 16, 19, 23, 26, 30, 34, 39, 43,
    48, 53, 58, 64, 69, 75, 81, 87, 93, 99, 105, 111, 117, 124, 130, 136,
    142, 149, 155, 161, 167, 173, 179, 184, 190, 195, 201, 206, 210, 215, 220, 224,
    228, 232, 235, 238, 241, 244, 246, 249, 250, 252, 253, 254, 255, 255, 255, 255,
    254, 253, 252, 250, 249, 246, 244, 241, 238, 235, 232, 228, 224, 220, 215, 210,
    206, 201, 195, 190, 184, 179, 173, 167, 161, 155, 149, 142, 136, 130, 124, 117,
    111, 105, 99, 93, 87, 81, 75, 69, 64, 58, 53, 48, 43, 39, 34, 30,
    26, 23, 19, 16, 13, 10, 8, 6, 4, 3, 2, 1, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// ---------------------------------------------------------------------------
// Helper beat functions (ported from ATmega sketch)
// ---------------------------------------------------------------------------
static uint8_t tri1beatsin8(uint8_t bpm) {
    return tri1_lut[beat8(bpm)];
}

static uint8_t tri2beatsin8(uint8_t bpm, uint8_t timeBase, uint8_t phaseOffset) {
    return tri2_lut[(uint8_t)(beat8(bpm, timeBase) + phaseOffset)];
}

static uint8_t tri3beatsin8(uint8_t bpm) {
    return tri3_lut[beat8(bpm)];
}

static uint8_t beatcubic8(accum88 beats_per_minute, uint8_t lowest = 0,
                           uint8_t highest = 255, uint32_t timebase = 0,
                           uint8_t phase_offset = 0) {
    uint8_t tribeatsin = tri2beatsin8(beats_per_minute, timebase, phase_offset);
    uint8_t rangewidth = highest - lowest;
    uint8_t scaledbeat = scale8(tribeatsin, rangewidth);
    return lowest + scaledbeat;
}

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------
static void map1Mapper() {
    for (uint8_t i = 0; i < NUM_ANIM_LEDS; i++) {
        anim[i] = ledsMap1[map1Mapping[i]];
    }
}

static void map2Mapper(uint8_t hueBase, uint8_t hueVariance) {
    uint8_t modeHue = beatsin8(15, hueBase, hueBase + hueVariance);
    for (uint8_t i = 0; i < NUM_ANIM_LEDS; i++) {
        if (map2brightness[i] == 0) anim[i] = CHSV(modeHue, 255, 255);
    }
}

static void map3Mapper(CRGB ledsForMapping[]) {
    for (uint8_t i = 0; i < NUM_ANIM_LEDS; i++) {
        ledsForMapping[i] = ledsMap3[map3Mapping[i]];
    }
}

static void ledsRotatorCW(CRGB ledsForRotation[], uint8_t sizeOfLedsForRotation,
                           uint8_t numberOfRotations) {
    uint8_t innerNumberOfRotations = numberOfRotations % 9;
    if (innerNumberOfRotations == 0) return;
    CRGB tempLeds[27];
    for (uint8_t j = 1; j <= numberOfRotations; j++) {
        for (uint8_t i = 0; i < sizeOfLedsForRotation; i++) {
            tempLeds[i] = ledsForRotation[i];
        }
        ledsForRotation[0]  = tempLeds[8];
        ledsForRotation[9]  = tempLeds[17];
        ledsForRotation[18] = tempLeds[26];
        for (uint8_t i = 1; i < 9; i++) {
            ledsForRotation[i]      = tempLeds[i - 1];
            ledsForRotation[i + 9]  = tempLeds[i + 8];
            ledsForRotation[i + 18] = tempLeds[i + 17];
        }
    }
}

// ---------------------------------------------------------------------------
// Animation implementations
// ---------------------------------------------------------------------------

// ---- rainbowMap1 ----
static void rainbowMap1() {
    fill_rainbow(ledsMap1, NUM_LEDS_MAP1, gHueModifier, 30);
    map1Mapper();
    FastLED.setBrightness(workingBrightness / 2);
}

// ---- confetti (rainbow hue) ----
static void confetti() {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 12);
    int pos = random8(NUM_ANIM_LEDS);
    anim[pos] += CHSV(gHueModifier + random8(64), 200, 255);
    FastLED.setBrightness(workingBrightness);
}

// ---- confettiFlower ----
static void confettiFlower(uint8_t hueBase, uint8_t hueVariance,
                            uint8_t edgeHueBase, uint8_t edgeHueVariance) {
    uint8_t saturationVariance = 30;
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 3);
    int pos = random8(NUM_ANIM_LEDS);
    EVERY_N_MILLISECONDS(20) {
        anim[pos] += CHSV(hueBase + random8(hueVariance),
                          180 + random8(saturationVariance), 255);
    }
    map2Mapper(edgeHueBase, edgeHueVariance);
    FastLED.setBrightness(workingBrightness);
}

static void confettiFlowerB() { confettiFlower(150, 50, 10, 20); }
static void confettiFlowerR() { confettiFlower(230, 30, 100, 50); }

// ---- spinningVortex (rainbow) ----
static void spinningVortex() {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 20);

    const uint8_t brightnessBPM = 60;
    const uint8_t minV = 0;
    const uint8_t maxV = 200;
    uint8_t colorAmplitude = 50;

    uint8_t modeHueDes1 = beatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 0, 0);
    uint8_t modeHueDes2 = beatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 0, 60);
    uint8_t modeHueDes3 = beatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 0, 120);

    const uint8_t OFFS[9] = { 0, 28, 56, 84, 112, 140, 168, 196, 224 };

    for (uint8_t i = 0; i < 9; i++) {
        uint8_t briO = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i]);
        uint8_t briM = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i] + 56);
        uint8_t briI = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i] + 112);
        if (briO < ZERO_CLAMP) briO = 0;
        if (briM < ZERO_CLAMP) briM = 0;
        if (briI < ZERO_CLAMP) briI = 0;
        anim[i]     = CHSV(modeHueDes1, 255, briO);
        anim[i + 9] = CHSV(modeHueDes2, 255, briM);
        if ((i + 19) != 27)
            anim[i + 19] = CHSV(modeHueDes3, 255, briI);
        else
            anim[18] = CHSV(modeHueDes3, 255, briI);
    }
    FastLED.setBrightness(workingBrightness);
}

// ---- spinningVortex (single color) ----
static void spinningVortexColor(int baseColor1) {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 20);

    const uint8_t brightnessBPM = 60;
    const uint8_t minV = 0;
    const uint8_t maxV = 200;
    uint8_t colorAmplitude = 50;

    uint8_t modeHueDes1 = beatsin8(10, baseColor1, baseColor1 + colorAmplitude, 0, 0);
    uint8_t modeHueDes2 = beatsin8(10, baseColor1, baseColor1 + colorAmplitude, 0, 60);
    uint8_t modeHueDes3 = beatsin8(10, baseColor1, baseColor1 + colorAmplitude, 0, 120);

    const uint8_t OFFS[9] = { 0, 28, 56, 84, 112, 140, 168, 196, 224 };

    for (uint8_t i = 0; i < 9; i++) {
        uint8_t briO = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i]);
        uint8_t briM = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i] + 56);
        uint8_t briI = beatsin8(brightnessBPM, minV, maxV, 0, OFFS[8 - i] + 112);
        if (briO < ZERO_CLAMP) briO = 0;
        if (briM < ZERO_CLAMP) briM = 0;
        if (briI < ZERO_CLAMP) briI = 0;
        anim[i]     = CHSV(modeHueDes1, 255, briO);
        anim[i + 9] = CHSV(modeHueDes2, 255, briM);
        if ((i + 19) != 27)
            anim[i + 19] = CHSV(modeHueDes3, 255, briI);
        else
            anim[18] = CHSV(modeHueDes3, 255, briI);
    }
    FastLED.setBrightness(workingBrightness);
}

static void spinningVortexR() { spinningVortexColor(240); }
static void spinningVortexB() { spinningVortexColor(150); }
static void spinningVortexG() { spinningVortexColor(70); }

// ---- portal (rainbow) ----
static void portal() {
    const uint16_t cycleTime = 1700;
    const uint8_t bpm = (uint8_t)(60000UL / cycleTime);
    const uint8_t segLen = NUM_ANIM_LEDS / 3;

    const uint8_t P0 = 0;
    const uint8_t P1 = 85;
    const uint8_t P2 = 170;

    uint8_t modeHue1 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 0);
    uint8_t modeHue2 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 85);
    uint8_t modeHue3 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 170);

    uint8_t b0 = beatsin8(bpm, 0, 255, 0, P0);
    uint8_t b1 = beatsin8(bpm, 0, 255, 0, P1);
    uint8_t b2 = beatsin8(bpm, 0, 255, 0, P2);

    if (b0 < ZERO_CLAMP) b0 = 0;
    if (b1 < ZERO_CLAMP) b1 = 0;
    if (b2 < ZERO_CLAMP) b2 = 0;

    fill_solid(&anim[0],          segLen, CHSV(modeHue1, 255, b0));
    fill_solid(&anim[segLen],     segLen, CHSV(modeHue2, 255, b1));
    fill_solid(&anim[segLen * 2], segLen, CHSV(modeHue3, 255, b2));
    FastLED.setBrightness(workingBrightness);
}

// ---- portal (single color) ----
static void portalColor(int baseColor, int amplitudeColor) {
    const uint16_t cycleTime = 1700;
    const uint8_t bpm = (uint8_t)(60000UL / cycleTime);
    const uint8_t segLen = NUM_ANIM_LEDS / 3;

    const uint8_t P0 = 0;
    const uint8_t P1 = 85;
    const uint8_t P2 = 170;

    uint8_t modeHue1 = beatsin8(15, baseColor, baseColor + amplitudeColor, 0, 0);
    uint8_t modeHue2 = beatsin8(15, baseColor, baseColor + amplitudeColor, 0, 85);
    uint8_t modeHue3 = beatsin8(15, baseColor, baseColor + amplitudeColor, 0, 170);

    uint8_t b0 = beatsin8(bpm, 0, 255, 0, P0);
    uint8_t b1 = beatsin8(bpm, 0, 255, 0, P1);
    uint8_t b2 = beatsin8(bpm, 0, 255, 0, P2);

    if (b0 < ZERO_CLAMP) b0 = 0;
    if (b1 < ZERO_CLAMP) b1 = 0;
    if (b2 < ZERO_CLAMP) b2 = 0;

    fill_solid(&anim[0],          segLen, CHSV(modeHue1, 255, b0));
    fill_solid(&anim[segLen],     segLen, CHSV(modeHue2, 255, b1));
    fill_solid(&anim[segLen * 2], segLen, CHSV(modeHue3, 255, b2));
    FastLED.setBrightness(workingBrightness);
}

static void portalR() { portalColor(253, 37); }
static void portalG() { portalColor(70, 50); }
static void portalB() { portalColor(170, 60); }

// ---- wings (rainbow) ----
static void wings() {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 10);

    uint8_t colorVariance = 50;
    uint8_t HueRing1 = beatsin8(10, gHueModifier, gHueModifier + colorVariance, 0, 0);
    uint8_t HueRing2 = beatsin8(10, gHueModifier, gHueModifier + colorVariance, 0, 60);
    uint8_t HueRing3 = beatsin8(10, gHueModifier, gHueModifier + colorVariance, 0, 120);
    uint8_t ringsSaturation = 255;

    int calcPos = beatcubic8(40, 0, 4);
    int pos11 = 4 - calcPos;
    anim[pos11] = CHSV(HueRing1, ringsSaturation, 255);
    int pos12 = 4 + calcPos;
    if (pos12 != 4) anim[pos12] = CHSV(HueRing1, ringsSaturation, 255);
    int pos21 = 13 - calcPos;
    anim[pos21] = CHSV(HueRing2, ringsSaturation, 255);
    int pos22 = 27 - pos21;
    if (pos22 != 18) anim[pos22] = CHSV(HueRing2, ringsSaturation, 255);
    int pos31 = 22 - calcPos;
    anim[pos31] = CHSV(HueRing3, ringsSaturation, 255);
    int pos32 = 44 - pos31;
    if (pos32 != 22) anim[pos32] = CHSV(HueRing3, ringsSaturation, 255);
    FastLED.setBrightness(workingBrightness);
}

// ---- wings (single color) ----
static void wingsColor(uint8_t baseColor, uint8_t colorVariance, uint8_t saturationVariance) {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 10);

    uint8_t HueRing1 = beatsin8(15, baseColor, baseColor + colorVariance, 0, 0);
    uint8_t HueRing2 = beatsin8(15, baseColor, baseColor + colorVariance, 0, 60);
    uint8_t HueRing3 = beatsin8(15, baseColor, baseColor + colorVariance, 0, 120);
    uint8_t ringsSaturation = beatsin8(10, 255 - saturationVariance, 255);

    int calcPos = beatcubic8(40, 0, 4);
    int pos11 = 4 - calcPos;
    anim[pos11] = CHSV(HueRing1, ringsSaturation, 255);
    int pos12 = 4 + calcPos;
    if (pos12 != 4) anim[pos12] = CHSV(HueRing1, ringsSaturation, 255);
    int pos21 = 13 - calcPos;
    anim[pos21] = CHSV(HueRing2, ringsSaturation, 255);
    int pos22 = 27 - pos21;
    if (pos22 != 18) anim[pos22] = CHSV(HueRing2, ringsSaturation, 255);
    int pos31 = 22 - calcPos;
    anim[pos31] = CHSV(HueRing3, ringsSaturation, 255);
    int pos32 = 44 - pos31;
    if (pos32 != 22) anim[pos32] = CHSV(HueRing3, ringsSaturation, 255);
    FastLED.setBrightness(workingBrightness);
}

static void wingsR() { wingsColor(250, 40, 0); }
static void wingsG() { wingsColor(100, 60, 60); }
static void wingsB() { wingsColor(175, 60, 100); }

// ---- flower ----
// Four colors in a petal layout (map3) that rotates one step around the rings
// every FLOWER_STEP_MS, cross-fading between positions.
static void flower() {
    const uint8_t P0 = 0, P1 = 85, P2 = 170;

    uint8_t modeHueBase0 = 120;
    uint8_t modeHueBase1 = 0;
    uint8_t modeHueBase2 = 230;
    uint8_t modeHueBase3 = 20;
    uint8_t modeHueVariance = 50;

    uint8_t modeHue1 = beatsin8(15, modeHueBase1, modeHueBase1 + modeHueVariance, 0, P0);
    uint8_t modeHue2 = beatsin8(15, modeHueBase2, modeHueBase2 + modeHueVariance, 0, P1);
    uint8_t modeHue3 = beatsin8(15, modeHueBase3, modeHueBase3 + modeHueVariance, 0, P2);

    ledsMap3[0] = CHSV(modeHueBase0, 255, 255);
    ledsMap3[1] = CHSV(modeHue1, 255, 255);
    ledsMap3[2] = CHSV(modeHue2, 255, 255);
    ledsMap3[3] = CHSV(modeHue3, 255, 255);

    // Rotate one step every FLOWER_STEP_MS: hold the current position, then
    // cross-fade into the next one instead of jumping.
    const uint16_t FLOWER_STEP_MS = 1000;
    const uint16_t FLOWER_HOLD_MS = 300;
    uint32_t now   = millis();
    uint8_t  step  = (now / FLOWER_STEP_MS) % 9;
    uint16_t phase = now % FLOWER_STEP_MS;
    uint8_t  t = (phase < FLOWER_HOLD_MS)
                     ? 0
                     : (uint32_t)(phase - FLOWER_HOLD_MS) * 255 / (FLOWER_STEP_MS - FLOWER_HOLD_MS);

    CRGB from[NUM_ANIM_LEDS], to[NUM_ANIM_LEDS];
    map3Mapper(from);
    memcpy(to, from, sizeof(from));
    ledsRotatorCW(from, NUM_ANIM_LEDS, step);
    ledsRotatorCW(to, NUM_ANIM_LEDS, step + 1);
    uint8_t eased = ease8InOutQuad(t);
    for (uint8_t i = 0; i < NUM_ANIM_LEDS; i++) {
        anim[i] = blend(from[i], to[i], eased);
    }
    FastLED.setBrightness(workingBrightness);
}

// ---- beacon ----
// The three segments pulse with offset triangle waves (ported from the ATmega
// sketch).
static void beacon() {
    const uint8_t bpm = 30;
    const uint8_t segLen = NUM_ANIM_LEDS / 3;

    uint8_t modeHue1 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 0);
    uint8_t modeHue2 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 85);
    uint8_t modeHue3 = beatsin8(15, gHueModifier, gHueModifier + 50, 0, 170);

    uint8_t b2 = tri1beatsin8(bpm);
    uint8_t b1 = tri2beatsin8(bpm, 0, 0);
    uint8_t b0 = tri3beatsin8(bpm);

    if (b0 < ZERO_CLAMP) b0 = 0;
    if (b1 < ZERO_CLAMP) b1 = 0;
    if (b2 < (2 * ZERO_CLAMP)) b2 = 2 * ZERO_CLAMP;

    fill_solid(&anim[0],          segLen, CHSV(modeHue1, 255, b0));
    fill_solid(&anim[segLen],     segLen, CHSV(modeHue2, 255, b1));
    fill_solid(&anim[segLen * 2], segLen, CHSV(modeHue3, 255, b2));
    FastLED.setBrightness(workingBrightness);
}

// ---- column-based patterns (sinelonUneven, sweep, curtains, heartbeat) ----
// The 27 animation pixels grouped into 7 virtual columns, ordered from one
// side of the pendant to the other (column layout from upstream's sinelon).
// -1 = unused slot.
static const int8_t SINELON_VCOLS[7][5] = {
    {  1,  2, -1, -1, -1 },
    {  0, 10, 11, 12,  3 },
    { 18, 19, 20, 21, 13 },
    {  9, 22,  4, -1, -1 },
    { 26, 25, 24, 23, 14 },
    {  8, 17, 16, 15,  5 },
    {  6,  7, -1, -1, -1 },
};

static void sinelonDrawCol(uint8_t col) {
    CRGB color = CHSV(gHueModifier, 220, 255);
    for (uint8_t i = 0; i < 5; i++) {
        int8_t p = SINELON_VCOLS[col][i];
        if (p >= 0) anim[p] += color;
    }
}

// One fast pass from side to side in `sweep_ms`, then nothing new is drawn
// until `period_ms` while the trail fades out slowly (`fade` per frame), then
// the next pass goes back the other way.
static void sinelonSweep(uint16_t sweep_ms, uint16_t period_ms, uint8_t fade) {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, fade);
    uint32_t now   = millis();
    uint32_t phase = now % period_ms;
    if (phase < sweep_ms) {
        uint8_t col = phase * 7 / sweep_ms;              // 0..6
        if ((now / period_ms) & 1) col = 6 - col;        // alternate direction
        sinelonDrawCol(col);
    }
    FastLED.setBrightness(workingBrightness);
}

// Back-and-forth with uneven legs: one direction takes `fast_pct` of the
// motion time (one sweep cycle at `bpm`), the return takes the rest. Each leg
// eases in/out like a sine sweep and has its own trail fade (lower = longer
// tail). After the slow leg nothing is drawn for `pause_ms`, so its tail fades
// out completely before the next fast pass starts.
static void sinelonAsym(uint8_t bpm, uint8_t fast_pct, uint8_t fade_fast, uint8_t fade_slow,
                        uint16_t pause_ms) {
    uint32_t motion = 60000UL / bpm;
    uint32_t fast   = motion * fast_pct / 100;
    uint32_t phase  = millis() % (motion + pause_ms);
    fadeToBlackBy(anim, NUM_ANIM_LEDS, phase < fast ? fade_fast : fade_slow);
    if (phase < motion) {
        uint8_t t;
        bool    back;
        if (phase < fast) { t = phase * 255 / fast;                     back = false; }
        else              { t = (phase - fast) * 255 / (motion - fast); back = true;  }
        uint8_t col = ((uint16_t)ease8InOutQuad(t) * 6 + 127) / 255;   // 0..6
        if (back) col = 6 - col;
        sinelonDrawCol(col);
    }
    FastLED.setBrightness(workingBrightness);
}
// 0.36 s fast pass (short trail), 0.84 s slow return (long trail), 0.72 s dark.
static void sinelonUneven() { sinelonAsym(50, 30, 60, 20, 720); }
// 0.3 s pass every 3 s, alternating direction, with a ~3 s fade.
static void sweep()         { sinelonSweep(300, 3000, 4); }

// ---- curtain ----
// Quick open then close of a lit area across the 7 columns, then a slow fade
// to black. `from_center`: the area grows from the middle column out to both
// sides and shrinks back to the middle; otherwise it closes in from both
// edges and reopens back to the edges. Columns the curtain leaves behind
// fade with `fade_move`; after it closes everything fades with `fade_end`.
static void curtain(bool from_center, uint16_t open_ms, uint16_t close_ms, uint16_t pause_ms,
                    uint8_t fade_move, uint8_t fade_end) {
    uint32_t phase = millis() % (open_ms + close_ms + pause_ms);
    bool moving = phase < (uint32_t)open_ms + close_ms;
    fadeToBlackBy(anim, NUM_ANIM_LEDS, moving ? fade_move : fade_end);
    if (!moving) {
        FastLED.setBrightness(workingBrightness);
        return;
    }
    uint8_t t = (phase < open_ms) ? phase * 255 / open_ms
                                  : 255 - (phase - open_ms) * 255 / close_ms;
    uint8_t reach = ((uint16_t)ease8InOutQuad(t) * 3 + 127) / 255;  // 0..3 columns
    CRGB color = CHSV(gHueModifier, 220, 255);
    for (uint8_t col = 0; col < 7; col++) {
        uint8_t dist = (col > 3) ? col - 3 : 3 - col;               // 0 = middle, 3 = edge
        bool lit = from_center ? (dist <= reach) : (dist >= 3 - reach);
        if (!lit) continue;
        for (uint8_t i = 0; i < 5; i++) {
            int8_t p = SINELON_VCOLS[col][i];
            if (p >= 0) anim[p] = color;
        }
    }
    FastLED.setBrightness(workingBrightness);
}
static void curtainCenter() { curtain(true,  250, 350, 1200, 40, 12); }
static void curtainEdges()  { curtain(false, 250, 350, 1200, 40, 12); }

// ---- heartbeat ----
// Two quick center-out curtain pulses: a small dim one, a gap that fades to
// near black, then a big full-width one, followed by a slow fade during the
// rest of the beat.
struct HeartPulse { uint16_t start_ms, open_ms, close_ms; uint8_t max_reach, value; };

static void heartbeat() {
    static const HeartPulse PULSES[2] = {
        {   0,  99, 154, 1, 150 },   // small, dim: middle three columns only
        { 418, 121, 220, 3, 255 },   // big: full width, full brightness
    };
    const uint16_t BEAT_MS = 1430;   // ≈42 beats per minute
    uint32_t phase = millis() % BEAT_MS;

    bool in_pulse = false;
    for (const HeartPulse &hp : PULSES) {
        if (phase < hp.start_ms || phase >= (uint32_t)hp.start_ms + hp.open_ms + hp.close_ms) continue;
        in_pulse = true;
        uint32_t p = phase - hp.start_ms;
        uint8_t t = (p < hp.open_ms) ? p * 255 / hp.open_ms
                                     : 255 - (p - hp.open_ms) * 255 / hp.close_ms;
        uint8_t reach = ((uint16_t)ease8InOutQuad(t) * hp.max_reach + 127) / 255;
        fadeToBlackBy(anim, NUM_ANIM_LEDS, 40);
        CRGB color = CHSV(gHueModifier, 220, hp.value);
        for (uint8_t col = 0; col < 7; col++) {
            uint8_t dist = (col > 3) ? col - 3 : 3 - col;
            if (dist > reach) continue;
            for (uint8_t i = 0; i < 5; i++) {
                int8_t px = SINELON_VCOLS[col][i];
                if (px >= 0) anim[px] = color;
            }
        }
    }
    if (!in_pulse) {
        // Between the pulses fade fast so they read as two separate beats;
        // after the big one fade slowly through the rest of the beat.
        fadeToBlackBy(anim, NUM_ANIM_LEDS, phase < PULSES[1].start_ms ? 60 : 14);
    }
    FastLED.setBrightness(workingBrightness);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void led_engine_setup() {

    pinMode(OUTPUT_PIN, OUTPUT);
    digitalWrite(OUTPUT_PIN, g_output_state ? HIGH : LOW);

    pinMode(LED_POWER_PIN, OUTPUT);
    digitalWrite(LED_POWER_PIN, HIGH);  // LED string power on by default

    FastLED.addLeds<NEOPIXEL, DATA_PIN>(leds, TOTAL_LEDS);
    FastLED.setBrightness(workingBrightness);

    // Default status LED to off
    leds[STATUS_LED_INDEX] = CRGB::Black;

    lastHueModTime = millis();
    s_last_auto_cycle_ms = millis();
}

void led_engine_loop() {
    unsigned long now = millis();

    // Advance hue counter
    if (now - lastHueModTime >= 15) {
        gHueModifier++;
        lastHueModTime = now;
    }

    // Auto-mode pattern cycling.
    bool auto_cycling = !g_white_mode && !g_manual_mode_active;
#ifdef LILUM_KIVSEE
    auto_cycling = auto_cycling && g_led_source != LED_SRC_KIVSEE;
#endif
    if (!auto_cycling || now - s_last_auto_cycle_ms >= AUTO_CYCLE_INTERVAL_MS) {
        // When paused, keep the timer fresh so the full interval applies once
        // auto mode resumes.
        if (auto_cycling) {
            g_animation_mode = (g_animation_mode + 1) % NUM_PATTERNS;
        }
        s_last_auto_cycle_ms = now;
    }

    // Update controllable output
    digitalWrite(OUTPUT_PIN, g_output_state ? HIGH : LOW);

    if (s_led_power_on) {
#ifdef LILUM_KIVSEE
        if (g_led_source == LED_SRC_KIVSEE) {
            // Snap display buffer only when renderer committed a complete frame.
            // s_display_buf is the renderer's previous render_buf — renderer is
            // now writing to the old display buf, so these two never overlap.
            if (s_anim_shadow_ready) {
                memcpy(anim, s_display_buf, sizeof(CRGB) * NUM_ANIM_LEDS);
                s_anim_shadow_ready = false;
            }
            if (g_white_mode) {
                fill_solid(anim, NUM_ANIM_LEDS, CRGB::White);
                FastLED.setBrightness(LED_BRIGHTNESS_WHITE);
            } else {
                // Connecting and nothing playing: one dim blue dot orbiting the
                // outer ring, so the state is visible when the status pixel is
                // hidden. A single lit pixel keeps the current draw minimal.
                // Connected and nothing playing: the same dot in green, slower.
                if (g_idle_blink_active) {
                    fill_solid(anim, NUM_ANIM_LEDS, CRGB::Black);
                    if (g_connecting_blink_active) {
                        anim[(now / CONNECTING_DOT_STEP_MS) % CONNECTING_DOT_RING_LEDS] =
                            CRGB(0, 0, CONNECTING_DOT_LEVEL);
                    } else {
                        // Crossfade each pixel into the next instead of stepping.
                        uint32_t pos  = now / IDLE_DOT_STEP_MS;
                        uint8_t  frac = (uint8_t)((now % IDLE_DOT_STEP_MS) * 255 / IDLE_DOT_STEP_MS);
                        anim[pos % CONNECTING_DOT_RING_LEDS] =
                            CRGB(0, scale8(CONNECTING_DOT_LEVEL, 255 - frac), 0);
                        anim[(pos + 1) % CONNECTING_DOT_RING_LEDS] =
                            CRGB(0, scale8(CONNECTING_DOT_LEVEL, frac), 0);
                    }
                }
                FastLED.setBrightness(workingBrightness);
            }
        } else
#endif
        if (g_white_mode) {
            // Solid white mode
            fill_solid(anim, NUM_ANIM_LEDS, CRGB::White);
            FastLED.setBrightness(LED_BRIGHTNESS_WHITE);
        } else {
            // Auto mode: pattern selected by the auto-cycle / BLE.
            // Manual mode: pattern selected by the button.
            uint8_t &idx = g_manual_mode_active ? g_manual_pattern : g_animation_mode;
            if (idx >= NUM_PATTERNS) {
                idx = 0;
            }
            static int16_t s_logged        = -1;
            static bool    s_logged_manual = false;
            if (idx != s_logged || g_manual_mode_active != s_logged_manual) {
                s_logged        = idx;
                s_logged_manual = g_manual_mode_active;
                ESP_LOGI("ANIM", "%s pattern %u/%u: %s",
                         g_manual_mode_active ? "Manual" : "Auto",
                         idx + 1, NUM_PATTERNS, gPatterns[idx].name);
            }
            gPatterns[idx].fn();
        }
    } else {
        // Power is off: always keep animation LEDs black
        fill_solid(anim, NUM_ANIM_LEDS, CRGB::Black);
    }

    // ── Battery status: indication pixel + LED-power with hold time ──
    {
        battery_charge_status_t cs = battery_get_charge_status();

        // Desired power state for animation LEDs (GPIO21)
        bool want_power_on;
        switch (cs) {
        case BAT_CHG_TEMP_INHIBITED:
        case BAT_CHG_TEMP_CRITICAL:
            want_power_on = false;
            break;
        default:
            want_power_on = true;
            break;
        }

        // Hold: only allow a transition after LED_POWER_HOLD_MS in the current state
        if (want_power_on != s_led_power_on) {
            if (now - s_led_power_change_ms >= LED_POWER_HOLD_MS) {
                s_led_power_on = want_power_on;
                s_led_power_change_ms = now;
            }
        }

        // Apply committed power state to animation LEDs
        digitalWrite(LED_POWER_PIN, s_led_power_on ? HIGH : LOW);
        if (!s_led_power_on) {
            fill_solid(anim, NUM_ANIM_LEDS, CRGB::Black);
        }

        // Status pixel (pixel 0) always updates immediately
        switch (cs) {
        case BAT_CHG_CHARGING_OK:
            leds[STATUS_LED_INDEX] = CRGB::Red;
            break;
        case BAT_CHG_TEMP_INHIBITED:
        case BAT_CHG_TEMP_CRITICAL: {
            bool on = ((now / 500) & 1) == 0;
            leds[STATUS_LED_INDEX] = on ? CRGB::Red : CRGB::Black;
            break;
        }
        case BAT_CHG_COMPLETE:
            leds[STATUS_LED_INDEX] = CRGB::Green;
            break;
        default:
            leds[STATUS_LED_INDEX] = CRGB::Black;
            break;
        }
    }

#ifdef LILUM_KIVSEE
    // WiFi connecting: blink status pixel blue — distinct from the red charge
    // states.
    if (g_connecting_blink_active) {
        bool on = ((now / (CONNECTING_BLINK_PERIOD_MS / 2)) & 1) == 0;
        leds[STATUS_LED_INDEX] = on ? CRGB::Blue : CRGB::Black;
    }
#endif

    FastLED.show();
}

uint8_t led_engine_get_num_auto_patterns() {
    return NUM_PATTERNS;
}

uint8_t led_engine_get_num_manual_patterns() {
    return NUM_PATTERNS;
}

void led_engine_set_white_mode(bool enable) {
    g_white_mode = enable;
}

bool led_engine_is_white_mode(void) {
    return g_white_mode;
}

#ifdef LILUM_KIVSEE
void led_engine_set_source(led_src_t src) {
    // Kivsee content is tuned for the rings, so start brighter than standalone.
    // RAM only: every kivsee boot or switch starts again at LED_BRIGHTNESS_KIVSEE.
    if (src == LED_SRC_KIVSEE && g_led_source != LED_SRC_KIVSEE) {
        workingBrightness = LED_BRIGHTNESS_KIVSEE;
    }
    g_led_source = src;
}

led_src_t led_engine_get_source(void) {
    return g_led_source;
}

struct CRGB *led_engine_anim_buffer(void) {
    return s_render_buf;
}

void led_engine_anim_commit(void) {
    // Swap render and display pointers so the LED task reads the just-completed
    // frame while the renderer writes into the (now-free) old display buffer.
    CRGB *tmp    = s_display_buf;
    s_display_buf = s_render_buf;
    s_render_buf  = tmp;
    s_anim_shadow_ready = true;
}

int led_engine_num_anim_leds(void) {
    return NUM_ANIM_LEDS;
}

void led_engine_set_connecting_blink(bool enable) {
    g_connecting_blink_active = enable;
}

void led_engine_set_idle_blink(bool enable) {
    g_idle_blink_active = enable;
}
#endif
