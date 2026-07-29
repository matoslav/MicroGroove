// ============================================================
// Microgroove — a pocket groovebox for the M5Stack Cardputer-ADV
// by lebiro.studio
//   - 3 synth tracks (saw/sqr/tri/sin + wavetables, 303-style resonant
//     filter, accent, slide); each track switchable 1-3 voices
//     (mono 303 or polyphonic chords)
//   - 8 drum lanes: 808 synth / 909 synth / SD samples, choke groups
//   - 16-step patterns x8, song chaining, live record with quantize
//   - live mic sampling + engine resampling to microSD
//   - project save/load to microSD (GBX v2; loads v1 transparently)
//
// Portions of the synth voice, 808 drums, and audio task are derived
// from qwertyuu/Cardputer-Adv-Tracker (MIT License) - see LICENSE.
// ============================================================
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

#include "config.h"
#include "sequencer.h"
#include "sampler.h"
#include "wavetable.h"
#include "storage.h"
#include "audio_engine.h"
#include "mic_sampler.h"
#include "ui.h"

// ===================== MOTION / IMU TUNING =========================
// All the "feel" knobs for the tilt controls live here. Angles are in
// degrees of tilt away from the flat/neutral position learned at boot.
//
// Cutoff  (LEFT / RIGHT tilt):
#define MOT_CUT_DEADZONE    7.0f    // within +/- this angle, keep the SOUND-page cutoff
#define MOT_CUT_SPAN       33.0f    // degrees from deadzone edge to fully open / shut
#define MOT_CUT_SIGN      (+1.0f)   // set to -1.0f to reverse which side opens the filter
//
// Stutter bands — tilt toward the STUTTER side (degrees from neutral):
#define MOT_STUTTER_DEADZONE  20.0f   // within +/- this: no stutter
#define MOT_STUTTER_BAND2     35.0f   // reach level 2 here (1/16)
#define MOT_STUTTER_BAND3     50.0f   // reach level 3 here (1/32)
//
// Probability bands — tilt toward the PROBABILITY side (degrees from neutral):
#define MOT_PROB_DEADZONE     20.0f   // within +/- this: no thinning
#define MOT_PROB_BAND2        35.0f   // reach 70% here
#define MOT_PROB_BAND3        50.0f   // reach 50% here
//
// Front/back axis orientation. The BMI270 sign can differ between units: if
// "TOWARD YOU" and "AWAY" come out swapped for you, flip this (1 <-> 0).
#define MOT_PITCH_INVERT      1       // 1 = invert the front/back axis
//
// Response feel:
#define MOT_SMOOTH         0.15f    // tilt smoothing 0..1 (lower = smoother / slower)
#define MOT_CAL_FRAMES     30       // frames averaged at boot to learn "flat"
// ===================================================================

void inputInit();
void inputUpdate();

static bool s_sdOk = false;

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    uiInit();

    // Speaker / codec
    auto spk = M5Cardputer.Speaker.config();
    spk.sample_rate   = SAMPLE_RATE;
    spk.task_priority = 3;
    spk.dma_buf_count = 4;
    spk.dma_buf_len   = AUDIO_BUF_LEN;
    M5Cardputer.Speaker.config(spk);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(200);

    // SD (Cardputer-ADV pinout)
    SPI.begin(SD_SPI_CLK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    s_sdOk = SD.begin(SD_SPI_CS_PIN, SPI, 25000000);

    // Modules
    micSamplerInit();                // scratch first, then sample pool
    wavetableInitBuiltins();
    if (s_sdOk) {
        samplerInit();               // also creates /groovebox dirs
        wavetableLoadUserFromSD();
    }
    sequencerInit();
    // Boot into the last project you saved; if there's no marker or it won't
    // load, sequencerInit already left an empty pattern (no demo).
    int lastSlot = s_sdOk ? storageLastSlot() : -1;
    if (lastSlot >= 0 && storageLoadProject((uint8_t)lastSlot))
        g_curProject = (uint8_t)lastSlot;

    uiSplash();
    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
        if (M5Cardputer.BtnA.wasPressed()) break;
        delay(30);
    }

    inputInit();
    audioEngineStart();              // render task on core 0

    if (!s_sdOk) uiStatus("NO SD CARD");
    g_needRedraw = true;
}

// --- Stage 1 motion control: tilt FORWARD/BACK (pitch) -> filter cutoff of the
// selected synth track. Neutral is captured at boot (rest flat on the desk).
// Inside a deadzone the track keeps its SOUND-page cutoff; past it, tilt acts as
// an absolute filter control: forward opens toward fully open, back closes toward
// shut. Tuned from measured device: usable pitch ~ +/-40 deg, rock-stable at rest.
static void motionUpdate() {
    static bool  smInit = false;
    static float ax = 0, ay = 0, az = 1;
    static bool  calDone = false;
    static float pSum = 0, bSum = 0, p0 = 0, b0 = 0;
    static int   calN = 0;

    if (!M5.Imu.isEnabled()) { g_motionTrack = -1; g_motionCut = 0.0f; g_stutterRate = 0; g_stepProb = 100; return; }

    float rx, ry, rz;
    M5.Imu.update();
    M5.Imu.getAccel(&rx, &ry, &rz);
    if (!smInit) { ax = rx; ay = ry; az = rz; smInit = true; }
    const float k = MOT_SMOOTH;                    // one-pole smoothing
    ax += (rx - ax) * k; ay += (ry - ay) * k; az += (rz - az) * k;

    // Two orthogonal tilt axes:
    //   cut = LEFT/RIGHT -> filter cutoff (unchanged, you liked it)
    //   stu = FRONT/BACK -> stutter / beat-repeat
    float cut = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
    float stu = atan2f(ay, az) * 57.2958f;

    if (!calDone) {                               // learn both resting neutrals (~30 frames)
        pSum += cut; bSum += stu; calN++;
        if (calN >= MOT_CAL_FRAMES) { p0 = pSum / calN; b0 = bSum / calN; calDone = true; }
        g_motionTrack = -1; g_motionCut = 0.0f; g_stutterRate = 0; g_stepProb = 100;
        return;
    }

    if (!g_motion.enabled) {                      // MOTION page master switch
        g_motionTrack = -1; g_motionCut = 0.0f; g_stutterRate = 0; g_stepProb = 100;
        return;
    }

    // --- cutoff (left/right): unchanged behaviour ---
    const float C_SIGN = MOT_CUT_SIGN;
    const float C_DZ   = MOT_CUT_DEADZONE;
    const float C_SPAN = MOT_CUT_SPAN;
    float dc = (cut - p0) * C_SIGN;
    float pos = 0.0f;
    float ac = fabsf(dc) - C_DZ;
    if (ac > 0.0f) {
        if (ac > C_SPAN) ac = C_SPAN;
        pos = ac / C_SPAN;
        if (dc < 0.0f) pos = -pos;
    }
    g_motionCut   = pos;
    g_motionTrack = (g_curTrack < NUM_SYNTHS) ? (int)g_curTrack : -1;

    // --- stutter vs step-probability on the front/back axis ---
    // STU END picks which tilt end is the stutter; the opposite end thins the steps.
    float axis = (stu - b0) * (MOT_PITCH_INVERT ? -1.0f : +1.0f);   // correct this unit's axis sign
    float sign = (g_motion.stutterEnd == 0) ? +1.0f : -1.0f;        // 0 = AWAY, 1 = TOWARD YOU
    float sa = axis * sign;                      // >0 = stutter end, <0 = probability end

    uint8_t rate = 0;
    if      (sa > MOT_STUTTER_BAND3)    rate = 3;   // 1/32
    else if (sa > MOT_STUTTER_BAND2)    rate = 2;   // 1/16
    else if (sa > MOT_STUTTER_DEADZONE) rate = 1;   // 1/8
    g_stutterRate = rate;

    uint8_t prob = 100;
    float pb = -sa;                              // opposite end -> thinning
    if      (pb > MOT_PROB_BAND3)    prob = 50;
    else if (pb > MOT_PROB_BAND2)    prob = 70;
    else if (pb > MOT_PROB_DEADZONE) prob = 90;
    g_stepProb = prob;
}

void loop() {
    inputUpdate();
    micSamplerUpdate();
    motionUpdate();
    sequencerTick();

    if (g_needRedraw) {
        uiDraw();
        g_needRedraw = false;
    }

    // keep the scope alive on the sound page
    if (g_curPage == PAGE_SOUND && g_playing) {
        static uint32_t last = 0;
        if (millis() - last > 60) { last = millis(); uiDraw(); }
    }

    delay(4);
}
