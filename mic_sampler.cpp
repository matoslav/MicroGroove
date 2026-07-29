// ============================================================
// Microgroove [BRANCH: live-sampling] - mic_sampler.cpp
// ============================================================
#include "mic_sampler.h"
#include "sampler.h"
#include "sequencer.h"
#include "audio_engine.h"
#include "ui.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

int16_t*          g_scratch    = nullptr;
volatile uint32_t g_rsmpRemain = 0;
volatile uint32_t g_scratchWr  = 0;

static bool     s_recActive   = false;
static uint8_t  s_recLane     = 0;
static bool     s_rsmpPending = false;
static uint32_t s_rsmpFrames  = 0;
static uint8_t  s_micCount    = 0;      // MIC01.. name counter
static uint8_t  s_rsmpCount   = 0;

// ping-pong chunks queued into M5Unified's mic driver
#define CHUNK 256
static int16_t  s_chunk[2][CHUNK];
static int      s_curChunk = 0;
static float    s_level    = 0.0f;

bool micSamplerInit() {
    g_scratch = (int16_t*)heap_caps_malloc(SCRATCH_FRAMES * sizeof(int16_t),
                                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return g_scratch != nullptr;
}

// ---------- auto trim ----------
static void trimScratch(uint32_t total, uint32_t rate,
                        uint32_t& start, uint32_t& len) {
    uint32_t pre = (rate * TRIM_PREROLL_MS) / 1000;
    uint32_t a = 0, b = total;
    while (a < total && (uint16_t)abs(g_scratch[a]) < TRIM_THRESHOLD) a++;
    while (b > a     && (uint16_t)abs(g_scratch[b - 1]) < TRIM_THRESHOLD) b--;
    if (a >= b) { start = 0; len = 0; return; }          // silence
    start = (a > pre) ? a - pre : 0;
    len   = b - start;
}

// ---------- WAV write + pool commit ----------
static bool writeWav(const char* name, uint32_t start, uint32_t frames, uint32_t rate) {
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", DIR_SAMPLES, name);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    uint32_t dataBytes = frames * 2;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                     16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0,
                     'd','a','t','a',0,0,0,0};
    uint32_t riff = 36 + dataBytes, brate = rate * 2;
    memcpy(h + 4,  &riff,  4);
    memcpy(h + 24, &rate,  4);
    memcpy(h + 28, &brate, 4);
    memcpy(h + 40, &dataBytes, 4);
    bool ok = f.write(h, 44) == 44 &&
              f.write((uint8_t*)(g_scratch + start), dataBytes) == dataBytes;
    f.close();
    return ok;
}

// commit scratch[start..start+frames] to a lane; SD-first, RAM fallback
static bool commitToLane(uint8_t lane, const char* base, uint8_t& counter,
                         uint32_t start, uint32_t frames, uint32_t rate) {
    if (frames < 64) { uiStatus("TOO QUIET"); return false; }
    // Find the next free NNxx.wav on the SD so a reboot (which resets `counter`)
    // never overwrites recordings from an earlier session.
    char name[SAMPLE_NAME_LEN];
    char probe[80];
    do {
        snprintf(name, sizeof(name), "%s%02u.wav", base, (unsigned)(++counter));
        snprintf(probe, sizeof(probe), "%s/%s", DIR_SAMPLES, name);
    } while (SD.exists(probe) && counter < 99);

    // Normalize: quiet mic captures otherwise become quiet samples. Amplify the
    // peak toward ~-1 dBFS, only ever boosting (never attenuating a hot resample)
    // and with a ceiling so a near-silent take doesn't turn its hiss into a roar.
    {
        int32_t pk = 0;
        for (uint32_t i = 0; i < frames; i++) {
            int32_t a = g_scratch[start + i]; if (a < 0) a = -a;
            if (a > pk) pk = a;
        }
        if (pk > 0) {
            float g = (0.9f * 32767.0f) / (float)pk;
            if (g > 8.0f) g = 8.0f;                 // +18 dB ceiling
            if (g > 1.0f) {
                for (uint32_t i = 0; i < frames; i++) {
                    int32_t v = (int32_t)(g_scratch[start + i] * g);
                    if      (v >  32767) v =  32767;
                    else if (v < -32768) v = -32768;
                    g_scratch[start + i] = (int16_t)v;
                }
            }
        }
    }

    int slot = -1;
    if (writeWav(name, start, frames, rate)) {
        slot = samplerLoad(name);                       // reuses pool logic
    }
    if (slot < 0) {                                     // RAM-only fallback
        if (g_numSamples >= MAX_SAMPLES ||
            g_poolUsed + frames > g_poolCapacity) { uiStatus("POOL FULL"); return false; }
        memcpy(g_samplePool + g_poolUsed, g_scratch + start, frames * 2);
        SampleInfo& s = g_samples[g_numSamples];
        strncpy(s.name, name, SAMPLE_NAME_LEN - 1);
        s.name[SAMPLE_NAME_LEN - 1] = 0;
        s.offset = g_poolUsed; s.length = frames; s.rate = rate; s.used = true;
        g_poolUsed += frames;
        slot = g_numSamples++;
    }
    DrumLane& d = g_drumLanes[lane];
    d.engine = ENG_SMPL; d.sampleSlot = (int8_t)slot; d.smp.init();
    d.smp.trigger(slot, 1.0f, d.volume);                // instant audition
    return true;
}

// ---------- mic recording ----------
bool micRecStart(uint8_t lane) {
    if (s_recActive || !g_scratch) return false;
    sequencerStop();
    g_previewVoice.stop();              // preview shares the scratch buffer we're about to fill
    g_audioPaused = true;               // stop the render task touching the codec first
    for (int i = 0; i < 100 && !g_audioParked; i++) vTaskDelay(1);
    M5Cardputer.Speaker.end();          // ES8311: avoid duplex contention (verify on hw)
    M5Cardputer.Mic.begin();
    s_recLane = lane; s_recActive = true;
    g_scratchWr = 0; s_curChunk = 0; s_level = 0;
    M5Cardputer.Mic.record(s_chunk[0], CHUNK, MIC_RATE);
    M5Cardputer.Mic.record(s_chunk[1], CHUNK, MIC_RATE);
    uiStatus("SAMPLING...");
    return true;
}

void micSamplerUpdate() {
    if (s_recActive) {
        // a chunk finished when the driver moved to the queued one
        while (!M5Cardputer.Mic.isRecording() ||
               M5Cardputer.Mic.isRecording() == 1 /*one buf left*/) {
            int16_t* done = s_chunk[s_curChunk];
            uint32_t room = SCRATCH_FRAMES - g_scratchWr;
            uint32_t n = (room < CHUNK) ? room : CHUNK;
            if (n) {
                memcpy(g_scratch + g_scratchWr, done, n * 2);
                g_scratchWr += n;
                int16_t pk = 0;
                for (uint32_t i = 0; i < n; i++) { int16_t a = abs(done[i]); if (a > pk) pk = a; }
                s_level = s_level * 0.6f + (pk / 32768.0f) * 0.4f;
            }
            if (g_scratchWr >= SCRATCH_FRAMES) { micRecStop(); return; }
            M5Cardputer.Mic.record(done, CHUNK, MIC_RATE);   // re-queue
            s_curChunk ^= 1;
            if (M5Cardputer.Mic.isRecording() >= 2) break;
        }
        // level meter via the hold-progress footer bar
        g_holdProg = s_level > 1.0f ? 1.0f : s_level;
        strncpy(g_holdLabel, "SAMPLING", sizeof(g_holdLabel));
        static uint32_t t = 0;
        if (millis() - t > 50) { t = millis(); g_needRedraw = true; }
    }
    if (s_rsmpFrames && g_rsmpRemain == 0) {             // resample finished
        s_rsmpFrames = 0; s_rsmpPending = true;
        uiStatus("RSMP: TAP A PAD");
        g_needRedraw = true;
    }
}

void micRecStop() {
    if (!s_recActive) return;
    s_recActive = false;
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.begin();
    g_holdProg = 0; g_holdLabel[0] = 0;

    uint32_t start, len;
    trimScratch(g_scratchWr, MIC_RATE, start, len);
    if (commitToLane(s_recLane, "MIC", s_micCount, start, len, MIC_RATE))
        uiStatus("SAMPLED!");
    g_audioPaused = false;              // codec + sample pool are stable now, resume audio
    g_needRedraw = true;
}

bool micRecActive() { return s_recActive; }

// ---------- resampling (engine mix -> scratch, tap in audio task) ----------
void resampleArm() {
    if (s_recActive || !g_scratch || g_rsmpRemain) return;
    g_scratchWr = 0;
    s_rsmpFrames = SCRATCH_FRAMES;
    g_rsmpRemain = SCRATCH_FRAMES;      // audio task decrements
    uiStatus("RESAMPLING...");
}

bool resamplePending() { return s_rsmpPending; }

void resampleCommit(uint8_t lane) {
    s_rsmpPending = false;
    uint32_t start, len;
    trimScratch(g_scratchWr, SAMPLE_RATE, start, len);
    if (commitToLane(lane, "RSM", s_rsmpCount, start, len, SAMPLE_RATE))
        uiStatus("RESAMPLED!");
    g_needRedraw = true;
}
