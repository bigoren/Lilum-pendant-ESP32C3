#include <Arduino.h>
#include <FastLED.h>
#include "esp_log.h"

#include "led_engine.h"
#include "orchestra_shared_config.h"
#include "battery_service.h"
#include "mic_service.h"

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
// Mapping arrays  (operate on the 27 animation LEDs)
// ---------------------------------------------------------------------------
#define NUM_LEDS_MAP1 3
static CRGB ledsMap1[NUM_LEDS_MAP1];

static uint8_t map1Mapping[27] = {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2};
static uint8_t map2brightness[27] = {0,1,0,0,1,0,0,1,0,0,1,1,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1};

// ---------------------------------------------------------------------------
// Global hue counter (derived from the synced time base in led_engine_loop)
// ---------------------------------------------------------------------------
static uint8_t gHueModifier = 0;

// ---------------------------------------------------------------------------
// Animation time base / master-clock correction
// ---------------------------------------------------------------------------
// Beat-driven patterns oscillate off (millis() - g_engine_timebase). On the
// master this stays 0, so animations run on the local clock. On a follower it
// holds (local_millis - master_timestamp), shifting every beat and the hue
// counter onto the master's clock so the whole group stays phase-aligned.
volatile uint32_t g_engine_timebase = 0;
static bool       s_engine_time_synced = false;
// Reject single sync corrections larger than this (corrupt packet / glitch).
#define ENGINE_SYNC_MAX_STEP_MS 3000
// Last master timestamp seen, used to detect a master reboot (its monotonic
// clock resets, so the timestamp jumps backwards).
static uint32_t   s_last_master_time_ms = 0;

// ---------------------------------------------------------------------------
// Music-reactive state
// ---------------------------------------------------------------------------
// On the master these mirror the local microphone; on a follower they hold the
// values received from the master's advertising payload (set via
// led_engine_apply_music). Music-reactive animations read these on followers.
static volatile uint8_t s_music_bpm    = 0;
static volatile uint8_t s_music_volume = 0;

// ---------------------------------------------------------------------------
// Forward declarations – automatic patterns
// ---------------------------------------------------------------------------
static void rainbowMap1();
static void confetti();
static void spinningVortex();
static void portal();
static void wings();
static void sinelon();
static void mSinelon();

// ---------------------------------------------------------------------------
// Forward declarations – manual (single-color) pattern variants
// ---------------------------------------------------------------------------
static void confettiFlowerB();
static void confettiFlowerR();
static void spinningVortexR();
static void spinningVortexB();
static void spinningVortexG();
static void portalR();
static void portalG();
static void portalB();
static void wingsR();
static void wingsG();
static void wingsB();

// ---------------------------------------------------------------------------
// Pattern lists
// ---------------------------------------------------------------------------
typedef void (*PatternFunc)();

static PatternFunc gPatternsAuto[] = { rainbowMap1, confetti, spinningVortex, portal, wings, sinelon };
static constexpr uint8_t NUM_AUTO_PATTERNS = sizeof(gPatternsAuto) / sizeof(gPatternsAuto[0]);

// ---------------------------------------------------------------------------
// Auto-mode pattern cycling
// ---------------------------------------------------------------------------
// While in auto mode, advance to the next auto pattern every
// AUTO_CYCLE_INTERVAL_S seconds. Runs on the master only; followers receive the
// resulting animation index over BLE and stay in sync.
#define AUTO_CYCLE_INTERVAL_S 10
static unsigned long s_last_auto_cycle_ms = 0;

static PatternFunc gPatternsManual[] = {
    rainbowMap1, confetti, sinelon,
    confettiFlowerB, confettiFlowerR,
    spinningVortexB, spinningVortexG, spinningVortexR,
    portalB, portalG, portalR,
    wingsG, wingsR, wingsB
};
static constexpr uint8_t NUM_MANUAL_PATTERNS = sizeof(gPatternsManual) / sizeof(gPatternsManual[0]);

// ---------------------------------------------------------------------------
// Lookup tables for custom beat functions
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Helper beat functions (ported from ATmega sketch)
// ---------------------------------------------------------------------------
static uint8_t tri2beatsin8(uint8_t bpm, uint32_t timeBase, uint8_t phaseOffset) {
    return tri2_lut[(uint8_t)(beat8(bpm, timeBase) + phaseOffset)];
}

static uint8_t beatcubic8(accum88 beats_per_minute, uint8_t lowest = 0,
                           uint8_t highest = 255, uint32_t timebase = 0,
                           uint8_t phase_offset = 0) {
    uint8_t tribeatsin = tri2beatsin8(beats_per_minute, timebase, phase_offset);
    uint8_t rangewidth = highest - lowest;
    uint8_t scaledbeat = scale8(tribeatsin, rangewidth);
    return lowest + scaledbeat;
}

// beatsin8 bound to the shared animation time base so all devices stay in
// phase. Equivalent to FastLED's beatsin8() with timebase = g_engine_timebase.
static inline uint8_t sbeatsin8(accum88 bpm, uint8_t lowest = 0,
                                uint8_t highest = 255, uint8_t phase_offset = 0) {
    return beatsin8(bpm, lowest, highest, (uint32_t)g_engine_timebase, phase_offset);
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
    uint8_t modeHue = sbeatsin8(15, hueBase, hueBase + hueVariance);
    for (uint8_t i = 0; i < NUM_ANIM_LEDS; i++) {
        if (map2brightness[i] == 0) anim[i] = CHSV(modeHue, 255, 255);
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

    uint8_t modeHueDes1 = sbeatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 0);
    uint8_t modeHueDes2 = sbeatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 60);
    uint8_t modeHueDes3 = sbeatsin8(10, gHueModifier, gHueModifier + colorAmplitude, 120);

    const uint8_t OFFS[9] = { 0, 28, 56, 84, 112, 140, 168, 196, 224 };

    for (uint8_t i = 0; i < 9; i++) {
        uint8_t briO = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i]);
        uint8_t briM = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i] + 56);
        uint8_t briI = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i] + 112);
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

    uint8_t modeHueDes1 = sbeatsin8(10, baseColor1, baseColor1 + colorAmplitude, 0);
    uint8_t modeHueDes2 = sbeatsin8(10, baseColor1, baseColor1 + colorAmplitude, 60);
    uint8_t modeHueDes3 = sbeatsin8(10, baseColor1, baseColor1 + colorAmplitude, 120);

    const uint8_t OFFS[9] = { 0, 28, 56, 84, 112, 140, 168, 196, 224 };

    for (uint8_t i = 0; i < 9; i++) {
        uint8_t briO = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i]);
        uint8_t briM = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i] + 56);
        uint8_t briI = sbeatsin8(brightnessBPM, minV, maxV, OFFS[8 - i] + 112);
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

    uint8_t modeHue1 = sbeatsin8(15, gHueModifier, gHueModifier + 50, 0);
    uint8_t modeHue2 = sbeatsin8(15, gHueModifier, gHueModifier + 50, 85);
    uint8_t modeHue3 = sbeatsin8(15, gHueModifier, gHueModifier + 50, 170);

    uint8_t b0 = sbeatsin8(bpm, 0, 255, P0);
    uint8_t b1 = sbeatsin8(bpm, 0, 255, P1);
    uint8_t b2 = sbeatsin8(bpm, 0, 255, P2);

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

    uint8_t modeHue1 = sbeatsin8(15, baseColor, baseColor + amplitudeColor, 0);
    uint8_t modeHue2 = sbeatsin8(15, baseColor, baseColor + amplitudeColor, 85);
    uint8_t modeHue3 = sbeatsin8(15, baseColor, baseColor + amplitudeColor, 170);

    uint8_t b0 = sbeatsin8(bpm, 0, 255, P0);
    uint8_t b1 = sbeatsin8(bpm, 0, 255, P1);
    uint8_t b2 = sbeatsin8(bpm, 0, 255, P2);

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
    uint8_t HueRing1 = sbeatsin8(10, gHueModifier, gHueModifier + colorVariance, 0);
    uint8_t HueRing2 = sbeatsin8(10, gHueModifier, gHueModifier + colorVariance, 60);
    uint8_t HueRing3 = sbeatsin8(10, gHueModifier, gHueModifier + colorVariance, 120);
    uint8_t ringsSaturation = 255;

    int calcPos = beatcubic8(40, 0, 4, (uint32_t)g_engine_timebase);
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

    uint8_t HueRing1 = sbeatsin8(15, baseColor, baseColor + colorVariance, 0);
    uint8_t HueRing2 = sbeatsin8(15, baseColor, baseColor + colorVariance, 60);
    uint8_t HueRing3 = sbeatsin8(15, baseColor, baseColor + colorVariance, 120);
    uint8_t ringsSaturation = sbeatsin8(10, 255 - saturationVariance, 255);

    int calcPos = beatcubic8(40, 0, 4, (uint32_t)g_engine_timebase);
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

// ---- sinelon (FastLED-style dot sweeping side to side) ----
// The 27 animation pixels are grouped into 7 "virtual columns" ordered
// spatially from one side to the other. A single dot sweeps back and forth.
// To create the illusion of the dot travelling through the whole orchestra,
// the dot runs across a virtual strip of SINELON_NUM_VCOLS positions per
// player: positions [pos*7 .. pos*7+6] belong to the device at ORCHESTRA_
// POSITION "pos". The synced beat is identical on every device; each one only
// renders the dot when it falls inside its own 7-position window. The
// positional phase shift is therefore ORCHESTRA_POSITION * 7.
static const int8_t SINELON_VCOLS[7][5] = {
    {  1,  2, -1, -1, -1 },   // vcol_0
    {  0, 10, 11, 12,  3 },   // vcol_1
    { 18, 19, 20, 21, 13 },   // vcol_2
    {  9, 22,  4, -1, -1 },   // vcol_3
    { 26, 25, 24, 23, 14 },   // vcol_4
    {  8, 17, 16, 15,  5 },   // vcol_5
    {  6,  7, -1, -1, -1 },   // vcol_6
};
static constexpr uint8_t SINELON_NUM_VCOLS = 7;

// Total length of the shared virtual strip: 7 columns per player.
static constexpr uint16_t SINELON_TOTAL_VCOLS =
    SINELON_NUM_VCOLS * ORCHESTRA_TOTAL_PLAYERS;
// This device's offset into the virtual strip.
static constexpr uint16_t SINELON_POS_SHIFT =
    ORCHESTRA_POSITION * SINELON_NUM_VCOLS;

static void sinelon() {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 20);

    // Synced dot position across the full virtual strip (0 .. total-1).
    uint8_t vpos = sbeatsin8(40, 0, SINELON_TOTAL_VCOLS - 1);

    // Render only when the dot is inside this device's window.
    int16_t col = (int16_t)vpos - (int16_t)SINELON_POS_SHIFT;
    if (col >= 0 && col < SINELON_NUM_VCOLS) {
        CRGB color = CHSV(gHueModifier, 220, 255);
        for (uint8_t i = 0; i < 5; i++) {
            int8_t p = SINELON_VCOLS[col][i];
            if (p >= 0) anim[p] += color;
        }
    }
    FastLED.setBrightness(workingBrightness);
}

// ---- mSinelon (music-reactive sinelon) ----
// Identical sweeping-dot motion to sinelon(), but driven by music analysis:
//   * the sweep tempo follows the detected BPM
//   * the overall brightness tracks the live volume level
// On the master the values come from the local microphone; on a follower they
// come from the master's advertising payload (see led_engine_apply_music), so
// the whole group reacts to the same audio. The dot still runs across the
// shared virtual strip (7 columns per player) so the hand-off between adjacent
// players stays in phase.
static void mSinelon() {
    fadeToBlackBy(anim, NUM_ANIM_LEDS, 20);

    // Source the music analysis: local mic on the master, master-supplied
    // values on a follower.
    uint16_t bpm;
    uint8_t  vol;
    if (ORCHESTRA_ROLE == OrchestraRole::Master) {
        bpm = mic_get_bpm();
        vol = mic_get_volume();
    } else {
        bpm = s_music_bpm;
        vol = s_music_volume;
    }

    // BPM is 0 until the beat detector has enough data; fall back to a default
    // and clamp to a usable range so beatsin8 always has a sane tempo.
    if (bpm < 40)  bpm = 40;
    if (bpm > 200) bpm = 200;

    // Synced dot position across the full virtual strip (0 .. total-1).
    uint8_t vpos = sbeatsin8((accum88)bpm, 0, SINELON_TOTAL_VCOLS - 1);

    // Render only when the dot is inside this device's window.
    int16_t col = (int16_t)vpos - (int16_t)SINELON_POS_SHIFT;
    if (col >= 0 && col < SINELON_NUM_VCOLS) {
        CRGB color = CHSV(gHueModifier, 220, 255);
        for (uint8_t i = 0; i < 5; i++) {
            int8_t p = SINELON_VCOLS[col][i];
            if (p >= 0) anim[p] += color;
        }
    }

    // Brightness follows the live volume, scaled by the user brightness so it
    // never exceeds the configured ceiling.
    FastLED.setBrightness(scale8(vol, workingBrightness));
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

    s_last_auto_cycle_ms = millis();
}

void led_engine_loop() {
    unsigned long now = millis();

    // Derive the global hue counter from the (optionally master-corrected) time
    // base so every device lands on the same hue. On the master g_engine_timebase
    // is 0, so this is simply local time / 15.
    gHueModifier = (uint8_t)((now - g_engine_timebase) / 15);

    // Auto-mode pattern cycling (master only). Followers receive the new index
    // over BLE, so they must not cycle independently.
    if (ORCHESTRA_ROLE == OrchestraRole::Master && !g_white_mode && !g_manual_mode_active) {
        if (now - s_last_auto_cycle_ms >= (AUTO_CYCLE_INTERVAL_S * 1000UL)) {
            s_last_auto_cycle_ms = now;
            if (NUM_AUTO_PATTERNS > 0) {
                g_animation_mode = (g_animation_mode + 1) % NUM_AUTO_PATTERNS;
            }
        }
    } else {
        // Not actively auto-cycling; keep the timer fresh so the full interval
        // applies once auto mode resumes.
        s_last_auto_cycle_ms = now;
    }

    // Update controllable output
    digitalWrite(OUTPUT_PIN, g_output_state ? HIGH : LOW);

    if (s_led_power_on) {
        if (g_white_mode) {
            // Solid white mode
            fill_solid(anim, NUM_ANIM_LEDS, CRGB::White);
            FastLED.setBrightness(LED_BRIGHTNESS_WHITE);
        } else if (!g_manual_mode_active) {
            // Automatic mode: pattern selected by orchestra config / BLE
            if (g_animation_mode >= NUM_AUTO_PATTERNS) {
                g_animation_mode = 0;
            }
            gPatternsAuto[g_animation_mode]();
        } else {
            // Manual mode: run the selected manual pattern
            if (g_manual_pattern >= NUM_MANUAL_PATTERNS) {
                g_manual_pattern = 0;
            }
            gPatternsManual[g_manual_pattern]();
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

    FastLED.show();
}

uint8_t led_engine_get_num_auto_patterns() {
    return NUM_AUTO_PATTERNS;
}

uint8_t led_engine_get_num_manual_patterns() {
    return NUM_MANUAL_PATTERNS;
}

void led_engine_set_white_mode(bool enable) {
    g_white_mode = enable;
}

bool led_engine_is_white_mode(void) {
    return g_white_mode;
}

// Followers call this with the master's advertised timestamp to align their
// animation clock to the master. The master never calls it, so g_engine_timebase
// stays 0 there and animations run on the local clock.
void led_engine_apply_master_time(uint32_t master_time_ms) {
    // Timebase = local time - master time. Beats compute phase from
    // (millis() - g_engine_timebase), which therefore tracks the master's clock.
    // uint32 modular arithmetic handles either device booting first.
    uint32_t new_timebase = (uint32_t)millis() - master_time_ms;

    // First packet, or the master rebooted (its monotonic clock reset, so the
    // timestamp jumped backwards): snap straight to the new base.
    bool master_restarted = s_engine_time_synced &&
                            (master_time_ms + ENGINE_SYNC_MAX_STEP_MS) < s_last_master_time_ms;
    s_last_master_time_ms = master_time_ms;

    if (!s_engine_time_synced || master_restarted) {
        g_engine_timebase = new_timebase;
        s_engine_time_synced = true;
        return;
    }

    // Normal case: small drift correction. Reject large outliers (corrupt
    // packet / glitch) instead of jolting the animation.
    int32_t delta = (int32_t)(new_timebase - g_engine_timebase);
    if (delta > ENGINE_SYNC_MAX_STEP_MS || delta < -ENGINE_SYNC_MAX_STEP_MS) {
        return;
    }
    g_engine_timebase = new_timebase;
}

// Followers call this with the master's microphone-derived tempo and volume so
// music-reactive animations react to the same audio as the master. The master
// never calls it and reads its local microphone directly instead.
void led_engine_apply_music(uint8_t bpm, uint8_t volume) {
    s_music_bpm    = bpm;
    s_music_volume = volume;
}

