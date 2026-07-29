// ============================================================
// Microgroove - input.cpp
// Snapshot diffing -> immediate / short / long press dispatch.
// All bindings live in keymap.h.
// Sampling gestures:
//   hold AUX (.) 0.5s  -> mic records to the current drum lane
//                         while AUX stays held; release = commit
//   hold SONG (n) 0.5s -> (while playing) resample the mix;
//                         then tap a pad to commit
// ============================================================
#include <M5Cardputer.h>
#include <SD.h>
#include "config.h"
#include "keymap.h"
#include "sequencer.h"
#include "sampler.h"
#include "storage.h"
#include "wavetable.h"
#include "ui.h"
#include "mic_sampler.h"
#include "audio_engine.h"

void inputInit();
void inputUpdate();

#define LONG_PRESS_MS 450
#define HINT_AFTER_MS 140    // progress bar appears after this

// Unload every sample from the RAM pool (SAMPLE page: CL / Z). The .wav files
// stay on the SD. Any drum lane that was playing a sample is detached first so
// the render task never reads freed pool data. Audio is parked during the swap.
static void clearSampleRAM() {
    g_audioPaused = true;
    for (int i = 0; i < 100 && !g_audioParked; i++) vTaskDelay(1);
    g_previewVoice.stop();
    for (int d = 0; d < NUM_DRUM_LANES; d++)
        if (g_drumLanes[d].engine == ENG_SMPL) {
            g_drumLanes[d].smp.stop();
            g_drumLanes[d].sampleSlot = -1;
        }
    samplerClearAll();                 // pool usage -> 0, registry wiped
    g_audioPaused = false;
    uiStatus("RAM CLEARED");
}

// Audition a sample WITHOUT touching the shared pool: decode it into the
// temporary mic/resample scratch buffer and point the preview voice there.
// Previewing another sample just overwrites the scratch, so auditioning never
// fills RAM. Only assigning a sample to a lane (4..- keys) consumes the pool.
static void previewSample(const char* name) {
    if (!g_scratch) { uiStatus("NO PREVIEW BUF"); return; }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", DIR_SAMPLES, name);
    File f = SD.open(path, FILE_READ);
    if (!f) { uiStatus("LOAD FAILED"); return; }

    g_previewVoice.active = false;                 // stop reading scratch during the swap
    uint32_t frames = 0, rate = 0;
    bool ok = wavDecodeToMono16(f, g_scratch, SCRATCH_FRAMES, frames, rate);
    f.close();

    if (ok && frames) {
        g_previewVoice.data = g_scratch;
        g_previewVoice.len  = frames;
        g_previewVoice.pos  = 0;
        g_previewVoice.inc  = (float)rate * INV_SAMPLE_RATE;
        g_previewVoice.gain = 0.9f;
        g_previewVoice.active = true;              // set active last
    } else {
        uiStatus("LOAD FAILED");
    }
}
#define RPT_DELAY_MS  350
#define RPT_RATE_MS    90

// ---------- snapshot of all 56 keys as synthetic codes ----------
struct KeySnap {
    uint8_t codes[16];
    uint8_t n = 0;
    void add(uint8_t kc) { if (n < sizeof(codes)) codes[n++] = kc; }
    bool has(uint8_t kc) const {
        for (uint8_t i = 0; i < n; i++) if (codes[i] == kc) return true;
        return false;
    }
};
static KeySnap s_prev;
uint8_t g_recPadKc = KC_NONE;          // key that ends mic capture on release

// pending short/long holds
struct Hold { uint8_t kc; uint8_t act; uint32_t t0; bool fired; };
static Hold    s_holds[8];
static uint8_t s_nHolds = 0;

// auto-repeat state (one repeating key at a time is plenty)
static uint8_t  s_rptAct  = ACT_NONE;
static uint8_t  s_rptKc   = KC_NONE;
static uint32_t s_rptNext = 0;
static uint16_t s_rptCount = 0;

// ---------- helpers ----------
static bool accentHeld(const KeySnap& s) { return s.has('m'); }

static uint8_t heldPianoCount(const KeySnap& s) {
    uint8_t c = 0;
    for (uint8_t i = 0; i < s.n; i++) if (pianoSemi(s.codes[i]) >= 0) c++;
    return c;
}

static void semiToNote(int8_t semi, uint8_t& note, uint8_t& oct) {
    note = (uint8_t)(semi % 12) + 1;
    oct  = (uint8_t)constrain((int)g_curOctave + semi / 12, 1, 7);
}

// ---------- SOUND page parameter model ----------
#define SYNTH_PARAMS 9
#define DRUM_PARAMS  7

static void adjustSynthParam(SynthTrack& t, uint8_t row, int dir, bool fine) {
    float step = fine ? 0.01f : 0.05f;
    if (row == 8) {                                        // VOICES 1..MAX_POLY
        t.setVoices((uint8_t)constrain((int)t.voices + dir, 1, MAX_POLY));
        return;
    }
    t.forEach([&](SynthVoice& v) {
        switch (row) {
            case 0: v.oscMode = (OscMode)(((int)v.oscMode + dir + OSC_COUNT) % OSC_COUNT); break;
            case 1: if (g_numWavetables)
                        v.wtIndex = (uint8_t)(((int)v.wtIndex + dir + g_numWavetables) % g_numWavetables);
                    break;
            case 2: v.fltCutoff  = constrain(v.fltCutoff  + dir * step, 0.0f, 1.0f); break;
            case 3: v.fltReso    = constrain(v.fltReso    + dir * step, 0.0f, 1.0f); break;
            case 4: v.fltEnvAmt  = constrain(v.fltEnvAmt  + dir * step, 0.0f, 1.0f); break;
            case 5: v.filtDecRate = constrain(v.filtDecRate + dir * (fine ? 0.00002f : 0.0001f),
                                              0.9950f, 0.99995f); break;
            case 6: v.ampDecRate  = constrain(v.ampDecRate  + dir * (fine ? 0.00001f : 0.00005f),
                                              0.9990f, 0.99999f); break;
            case 7: v.volume     = constrain(v.volume     + dir * step, 0.0f, 1.0f); break;
        }
    });
}

static void adjustDrumParam(uint8_t lane, uint8_t row, int dir, bool fine) {
    DrumLane& d = g_drumLanes[lane];
    switch (row) {
        case 0: g_curDrumLane = (uint8_t)((lane + dir + NUM_DRUM_LANES) % NUM_DRUM_LANES); break;
        case 1: d.engine = (DrumEngine)(((int)d.engine + dir + ENG_COUNT) % ENG_COUNT); break;
        case 2:
            if (d.engine == ENG_SMPL) {
                if (g_numSamples)
                    d.sampleSlot = (int8_t)((((d.sampleSlot < 0 ? 0 : d.sampleSlot)) + dir
                                             + g_numSamples) % g_numSamples);
            } else {
                d.type = (uint8_t)(((int)d.type + dir + DT_COUNT) % DT_COUNT);
            }
            break;
        case 3: d.volume = constrain(d.volume + dir * (fine ? 0.01f : 0.05f), 0.0f, 1.0f); break;
        case 4: d.tune   = constrain(d.tune   + dir * (fine ? 0.1f  : 1.0f), -12.0f, 12.0f); break;
        case 5: d.decay  = constrain(d.decay  + dir * (fine ? 0.02f : 0.1f), 0.4f, 2.5f); break;
        case 6: d.chokeGroup = (uint8_t)(((int)d.chokeGroup + dir + 4) % 4); break;
    }
}

// ---------- arrows, per page ----------
static void adjustMotionParam(uint8_t row, int dx) {
    switch (row) {
        case 0: g_motion.enabled = !g_motion.enabled;                 break;  // MOTION on/off
        case 1: g_motion.stutterEnd ^= 1;                             break;  // STU END FWD/BACK
        case 2: g_motion.probTarget = (uint8_t)((g_motion.probTarget + 3 + (dx > 0 ? 1 : -1)) % 3); break;
        case 3: g_motion.probMode ^= 1;                               break;  // RND/BAR
    }
}

static void arrow(uint8_t act, const KeySnap& now) {
    int dx = (act == ACT_LEFT) ? -1 : (act == ACT_RIGHT) ? 1 : 0;
    int dy = (act == ACT_UP)   ? -1 : (act == ACT_DOWN)  ? 1 : 0;

    switch (g_curPage) {
        case PAGE_PATTERN:
            if (dy) {
                if (g_curTrack < NUM_SYNTHS) {
                    // synth rows: down eventually enters drums (top = lane 8, TR order)
                    if (dy > 0) {
                        if (g_curTrack + 1 < NUM_SYNTHS) g_curTrack++;
                        else { g_curTrack = NUM_SYNTHS; g_curDrumLane = NUM_DRUM_LANES - 1; }
                    } else if (g_curTrack > 0) g_curTrack--;
                } else {
                    // drum grid is drawn kick-at-bottom: visual down = lane-1
                    if (dy > 0) { if (g_curDrumLane > 0) g_curDrumLane--; }
                    else {
                        if (g_curDrumLane < NUM_DRUM_LANES - 1) g_curDrumLane++;
                        else g_curTrack = NUM_SYNTHS - 1;      // exit up into synth 3
                    }
                }
            }
            if (dx) g_curStep = (uint8_t)((g_curStep + NUM_STEPS + dx) % NUM_STEPS);
            break;

        case PAGE_SOUND: {
            uint8_t rows = (g_curTrack == NUM_SYNTHS) ? DRUM_PARAMS : SYNTH_PARAMS;
            if (dy) g_soundParam = (uint8_t)((g_soundParam + rows + dy) % rows);
            if (dx) {
                bool fine = accentHeld(now);
                if (g_curTrack == NUM_SYNTHS) adjustDrumParam(g_curDrumLane, g_soundParam, dx, fine);
                else adjustSynthParam(g_synths[g_curTrack], g_soundParam, dx, fine);
            }
            break;
        }
        case PAGE_SAMPLE:
            if (dy < 0 && g_fileSel > 0) g_fileSel--;
            if (dy > 0 && g_fileSel + 1 < g_fileCount) g_fileSel++;
            break;

        case PAGE_SONG:
            if (dy) g_songCursor = (uint8_t)((g_songCursor + SONG_LENGTH + dy * 16) % SONG_LENGTH);
            if (dx) g_songCursor = (uint8_t)((g_songCursor + SONG_LENGTH + dx) % SONG_LENGTH);
            break;

        case PAGE_MOTION:
            if (dy) g_motionParam = (uint8_t)((g_motionParam + MOTION_PARAMS + dy) % MOTION_PARAMS);
            if (dx) adjustMotionParam(g_motionParam, dx);
            break;
        default: break;
    }
    g_needRedraw = true;
}

// ---------- immediate actions (key-down) ----------
static void doImmediate(uint8_t act, const KeySnap& now) {
    switch (act) {
        case ACT_LEFT: case ACT_RIGHT: case ACT_UP: case ACT_DOWN:
            arrow(act, now); break;

        case ACT_SLIDE:
            if (g_curTrack < NUM_SYNTHS) {
                SynthCell& c = g_patterns[g_curPattern].synth[g_curTrack][g_curStep];
                if (!c.empty()) {
                    c.slide = !c.slide;
                    if (c.slide && g_synths[g_curTrack].voices > 1)
                        uiStatus("SLIDE = MONO ONLY");
                    g_needRedraw = true;
                }
            }
            break;

        case ACT_REC:
            g_recEnabled = !g_recEnabled;
            uiStatus(g_recEnabled ? "REC ON" : "REC OFF");
            g_needRedraw = true; break;

        case ACT_ACCENT: break;   // pure modifier
        default: break;
    }
}

// ---------- short actions (release < 450ms) ----------
static void doShort(uint8_t act) {
    // track select
    if (act >= ACT_TRACK1 && act <= ACT_TRACKD) {
        g_curTrack = (uint8_t)(act - ACT_TRACK1);          // 0..2, 3 = drums
        g_needRedraw = true; return;
    }
    // pattern keys: context
    if (act >= ACT_PAT1 && act <= ACT_PAT8) {
        uint8_t k = (uint8_t)(act - ACT_PAT1);
        if (g_curPage == PAGE_SAMPLE) {
            if (g_fileCount) {
                int slot = samplerLoad(g_fileList[g_fileSel]);
                if (slot >= 0) {
                    DrumLane& d = g_drumLanes[k];
                    g_audioPaused = true;                 // swap lane state atomically vs render task
                    for (int i = 0; i < 100 && !g_audioParked; i++) vTaskDelay(1);
                    d.sv.active  = false;                 // silence old synth-drum voice
                    d.smp.stop();                         // silence old sample voice
                    d.engine = ENG_SMPL; d.sampleSlot = (int8_t)slot; d.smp.init();
                    g_audioPaused = false;
                    uiStatus("ASSIGNED");
                } else uiStatus("LOAD FAILED");
                g_needRedraw = true;
            }
        } else if (g_curPage == PAGE_SONG) {
            g_song[g_songCursor] = k;
            g_songCursor = (g_songCursor + 1) % SONG_LENGTH;
            g_needRedraw = true;
        } else {
            g_curPattern = k;
            if (!g_playing && !g_songMode) g_playPattern = k;
            uiStatus("PATTERN"); g_needRedraw = true;
        }
        return;
    }

    switch (act) {
        case ACT_LOAD: {
            char m[24];
            snprintf(m, sizeof(m), "P%u%s hold=LOAD", g_curProject + 1,
                     storageProjectExists(g_curProject) ? "*" : " empty");
            uiStatus(m); break;
        }
        case ACT_SAVE: {
            char m[24];
            snprintf(m, sizeof(m), "P%u hold=SAVE", g_curProject + 1);
            uiStatus(m); break;
        }
        case ACT_BPM_DN:
            g_bpm = (uint16_t)constrain((int)g_bpm - 1, 40, 300);
            g_needRedraw = true; break;
        case ACT_BPM_UP:
            g_bpm = (uint16_t)constrain((int)g_bpm + 1, 40, 300);
            g_needRedraw = true; break;

        case ACT_PAGE:
            g_curPage = (Page)(((int)g_curPage + 1) % PAGE_COUNT);
            if (g_curPage == PAGE_SAMPLE) uiScanSampleDir();
            g_needRedraw = true; break;

        case ACT_PLAY:
            if (g_playing) sequencerStop(); else sequencerStart(false);
            g_needRedraw = true; break;

        case ACT_CLR:
            if (g_curPage == PAGE_SAMPLE)    clearSampleRAM();
            else if (g_curPage == PAGE_SONG) g_song[g_songCursor] = SONG_EMPTY;
            else                             clearCellAtCursor();
            g_needRedraw = true; break;

        case ACT_SONG:
            g_songMode = !g_songMode;
            uiStatus(g_songMode ? "SONG MODE" : "PATTERN MODE");
            g_needRedraw = true; break;

        case ACT_AUX:
            if (g_curPage == PAGE_SAMPLE && g_fileCount) {
                previewSample(g_fileList[g_fileSel]);
                g_needRedraw = true;
            } else if (g_curPage == PAGE_SONG) {
                g_songLoopStart = g_songCursor;
                uiStatus("LOOP SET"); g_needRedraw = true;
            } else {
                uiStatus("hold = MIC SAMPLE");
            }
            break;
        default: break;
    }
}

// ---------- long actions (held 450ms) ----------
static void doLong(uint8_t act) {
    if (act >= ACT_TRACK1 && act <= ACT_TRACK3) {
        uint8_t t = (uint8_t)(act - ACT_TRACK1);
        g_synthMute[t] = !g_synthMute[t];
        uiStatus(g_synthMute[t] ? "MUTED" : "UNMUTED");
        g_needRedraw = true; return;
    }
    if (act == ACT_TRACKD) {
        g_drumMute = !g_drumMute;
        uiStatus(g_drumMute ? "DRUMS MUTED" : "DRUMS ON");
        g_needRedraw = true; return;
    }
    if (act >= ACT_PAT1 && act <= ACT_PAT8) {
        if (g_curPage == PAGE_SAMPLE || g_curPage == PAGE_SONG) return;
        uint8_t k = (uint8_t)(act - ACT_PAT1);
        clonePatternTo(k);
        g_curPattern = k;
        if (!g_playing && !g_songMode) g_playPattern = k;
        char m[16]; snprintf(m, sizeof(m), "CLONED>%u", k + 1);
        uiStatus(m); g_needRedraw = true; return;
    }
    switch (act) {
        case ACT_LOAD:
            uiStatus(storageLoadProject(g_curProject) ? "LOADED" : "LOAD FAILED");
            g_needRedraw = true; break;
        case ACT_SAVE:
            uiStatus(storageSaveProject(g_curProject) ? "SAVED" : "SAVE FAILED");
            break;
        case ACT_PAGE:
            g_curPage = PAGE_PATTERN; g_needRedraw = true; break;
        case ACT_PLAY:
            sequencerStart(true); g_needRedraw = true; break;
        case ACT_CLR:
            if (g_curPage == PAGE_SAMPLE) break;      // Z here = clear RAM (short press), never wipe pattern
            memset(&g_patterns[g_curPattern], 0, sizeof(Pattern));
            uiStatus("PATTERN CLEARED"); g_needRedraw = true; break;
        case ACT_BPM_DN:
            if (g_curPage == PAGE_SONG) {
                if (g_curProject > 0) g_curProject--;
                uiStatus("PROJECT");
            } else if (g_curOctave > 1) g_curOctave--;
            g_needRedraw = true; break;
        case ACT_BPM_UP:
            if (g_curPage == PAGE_SONG) {
                if (g_curProject < NUM_PROJECT_SLOTS - 1) g_curProject++;
                uiStatus("PROJECT");
            } else if (g_curOctave < 7) g_curOctave++;
            g_needRedraw = true; break;

        case ACT_SONG:                        // long SONG = resample the mix
            if (g_playing) resampleArm();
            else uiStatus("PLAY, THEN HOLD");
            break;

        case ACT_AUX:                         // long AUX = mic record
            if (micRecStart(g_curDrumLane)) g_recPadKc = (uint8_t)'.';
            else uiStatus("MIC BUSY");
            break;
        default: break;
    }
}

// ---------- piano / pads (key-down, latency-critical path) ----------
static void doPiano(uint8_t kc, const KeySnap& now) {
    if (g_curPage != PAGE_PATTERN && g_curPage != PAGE_SOUND) return;

    if (g_curTrack == NUM_SYNTHS) {
        int8_t lane = padLane(kc);
        if (lane < 0) return;
        g_curDrumLane = (uint8_t)lane;
        if (resamplePending()) { resampleCommit(lane); return; }
        if (g_curPage == PAGE_SOUND) triggerLaneLive(lane);
        else                         liveDrumHit(lane);
        g_needRedraw = true;
        return;
    }

    int8_t semi = pianoSemi(kc);
    if (semi < 0) return;
    uint8_t note, oct; semiToNote(semi, note, oct);
    bool accent = accentHeld(now);

    if (g_curPage == PAGE_SOUND) {
        g_synths[g_curTrack].noteOn(noteToFreq(note, oct), accent, false);  // audition
    } else {
        bool legato = heldPianoCount(s_prev) >= 1;
        liveSynthNote(g_curTrack, note, oct, accent, legato);
    }
}

// ---------- main entry ----------
void inputInit() {
    s_prev = KeySnap();
    s_nHolds = 0; s_rptAct = ACT_NONE;
    g_holdProg = 0; g_holdLabel[0] = 0;
}

void inputUpdate() {
    M5Cardputer.update();
    uint32_t nowMs = millis();

    // -- CLR held while live-recording = erase at playhead --
    if (g_playing && g_recEnabled && s_prev.has('z')) clearStepAtPlayhead();

    // -- auto-repeat --
    if (s_rptAct != ACT_NONE && s_prev.has(s_rptKc) && nowMs >= s_rptNext) {
        s_rptCount++;
        doImmediate(s_rptAct, s_prev);
        s_rptNext = nowMs + RPT_RATE_MS;
    }

    // -- long-press firing + progress bar (runs even without key changes) --
    float prog = 0.0f; const char* lbl = "";
    for (uint8_t i = 0; i < s_nHolds; i++) {
        Hold& h = s_holds[i];
        uint32_t held = nowMs - h.t0;
        // CLR in erase-mode never fires short/long
        if (h.act == ACT_CLR && g_playing && g_recEnabled) { h.fired = true; continue; }
        if (!h.fired && held >= LONG_PRESS_MS) { h.fired = true; doLong(h.act); }
        if (!h.fired && held >= HINT_AFTER_MS) {
            float p = (float)(held - HINT_AFTER_MS) / (LONG_PRESS_MS - HINT_AFTER_MS);
            if (p > prog) { prog = p; lbl = actLongName(h.act); }
        }
    }
    // chord: LOAD + SAVE held simultaneously = DEMO
    {
        int iLoad = -1, iSave = -1;
        for (uint8_t i = 0; i < s_nHolds; i++) {
            if (s_holds[i].act == ACT_LOAD && !s_holds[i].fired) iLoad = i;
            if (s_holds[i].act == ACT_SAVE && !s_holds[i].fired) iSave = i;
        }
        if (iLoad >= 0 && iSave >= 0) {
            s_holds[iLoad].fired = true;
            s_holds[iSave].fired = true;
            loadDemoPattern();
            uiStatus("DEMO");
            g_needRedraw = true;
            prog = 0; lbl = "";
        }
    }

    // during mic capture the footer bar is the level meter (mic_sampler owns it)
    if (!micRecActive() && prog != g_holdProg) {
        static uint32_t lastAnim = 0;
        g_holdProg = prog;
        strncpy(g_holdLabel, lbl, sizeof(g_holdLabel) - 1);
        g_holdLabel[sizeof(g_holdLabel) - 1] = 0;
        if (nowMs - lastAnim > 40) { lastAnim = nowMs; g_needRedraw = true; }
    }

    if (!M5Cardputer.Keyboard.isChange()) return;

    // -- build snapshot --
    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    KeySnap now;
    if (st.fn)    now.add(KC_FN);
    if (st.shift) now.add(KC_SHIFT);
    if (st.ctrl)  now.add(KC_CTRL);
    if (st.opt)   now.add(KC_OPT);
    if (st.alt)   now.add(KC_ALT);
    if (st.tab)   now.add(KC_TAB);
    if (st.del)   now.add(KC_DEL);
    if (st.enter) now.add(KC_ENTER);
    if (st.space) now.add(KC_SPACE);
    for (auto k : st.word) now.add((uint8_t)normalizeKey(k));

    // -- newly pressed --
    for (uint8_t i = 0; i < now.n; i++) {
        uint8_t kc = now.codes[i];
        if (s_prev.has(kc)) continue;

        // piano/pads take priority over any action bound to the same key
        if (pianoSemi(kc) >= 0 &&
            (g_curPage == PAGE_PATTERN || g_curPage == PAGE_SOUND)) {
            doPiano(kc, now);
            continue;
        }

        uint8_t act = keyAction(kc);
        if (act == ACT_NONE) continue;

        if (actImmediate(act)) {
            doImmediate(act, now);
            if (actRepeats(act)) {
                s_rptAct = act; s_rptKc = kc;
                s_rptNext = nowMs + RPT_DELAY_MS; s_rptCount = 0;
            }
        } else if (s_nHolds < 8) {
            s_holds[s_nHolds++] = { kc, act, nowMs, false };
        }
    }

    // -- mic capture ends when AUX is released --
    if (g_recPadKc != KC_NONE && !now.has(g_recPadKc)) {
        g_recPadKc = KC_NONE;
        micRecStop();
    }

    // -- released --
    for (uint8_t i = 0; i < s_nHolds; ) {
        if (!now.has(s_holds[i].kc)) {
            if (!s_holds[i].fired) doShort(s_holds[i].act);
            s_holds[i] = s_holds[--s_nHolds];
        } else i++;
    }
    if (s_rptAct != ACT_NONE && !now.has(s_rptKc)) { s_rptAct = ACT_NONE; s_rptCount = 0; }

    s_prev = now;
}
