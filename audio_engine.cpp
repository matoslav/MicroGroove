// ============================================================
// CardputerGroovebox - audio_engine.cpp
// Render task derived from qwertyuu/Cardputer-Adv-Tracker (MIT),
// extended with 8 drum lanes + sample voices.
// ============================================================
#include "audio_engine.h"
#include "sequencer.h"
#include "sampler.h"
#include <M5Cardputer.h>
#include "mic_sampler.h"

float g_scopeBuf[SCREEN_W];
volatile int g_scopeIdx = 0;

volatile bool g_audioPaused = false;
volatile bool g_audioParked = false;

volatile float g_motionCut = 0.0f;
volatile int   g_motionTrack = -1;

static int16_t s_bufA[AUDIO_BUF_LEN];
static int16_t s_bufB[AUDIO_BUF_LEN];
static TaskHandle_t s_task = nullptr;

static void audioTask(void*) {
    int16_t* buffers[2] = { s_bufA, s_bufB };
    int cur = 0;

    while (true) {
        if (g_audioPaused) { g_audioParked = true; vTaskDelay(2); continue; }
        g_audioParked = false;
        int16_t* buf = buffers[cur];

        for (int i = 0; i < AUDIO_BUF_LEN; i++) {
            float mix = 0.0f;

            for (int s = 0; s < NUM_SYNTHS; s++)
                if (!g_synthMute[s])
                    mix += g_synths[s].render(s == g_motionTrack ? g_motionCut : 0.0f);

            if (!g_drumMute)
                for (int d = 0; d < NUM_DRUM_LANES; d++)
                    mix += g_drumLanes[d].render() * 0.6f;

            mix += g_previewVoice.render();

            // soft clip
            if (mix > 1.0f) mix = 1.0f; else if (mix < -1.0f) mix = -1.0f;
            buf[i] = (int16_t)(mix * 12000.0f);

            if (g_rsmpRemain) {                       // resample tap
                g_scratch[g_scratchWr++] = buf[i];
                g_rsmpRemain--;
            }

            if ((i & 7) == 0 && g_scopeIdx < SCREEN_W)
                g_scopeBuf[g_scopeIdx++] = mix;
        }

        while (!M5Cardputer.Speaker.playRaw(buf, AUDIO_BUF_LEN, SAMPLE_RATE, false, 1, 0))
            vTaskDelay(1);

        cur ^= 1;
    }
}

void audioEngineStart() {
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 1, &s_task, 0);
}
