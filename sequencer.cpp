// ============================================================
// CardputerGroovebox - sequencer.cpp
// ============================================================
#include "sequencer.h"
#include <Arduino.h>
#include <string.h>

Pattern    g_patterns[NUM_PATTERNS];
uint8_t    g_song[SONG_LENGTH];
uint8_t    g_songLoopStart = 0;

SynthTrack g_synths[NUM_SYNTHS];
DrumLane   g_drumLanes[NUM_DRUM_LANES];
volatile bool g_synthMute[NUM_SYNTHS] = {false, false, false};
volatile bool g_drumMute = false;

volatile bool g_playing = false;
bool       g_recEnabled = false;
bool       g_songMode   = false;
uint8_t    g_playStep    = 0;
uint8_t    g_playPattern = 0;
uint8_t    g_songPos     = 0;
uint16_t   g_bpm         = 128;
uint8_t    g_stutterRate = 0;      // 0 = off, 1 = 1/8, 2 = 1/16, 3 = 1/32 (from tilt)
uint8_t    g_stepProb    = 100;    // % chance a normal step fires (100 = always); tilt-back thins

MotionCfg  g_motion = { true, 1, 1, 0 };   // enabled, stutter=TOWARD YOU, thin=DRUMS, mode=RANDOM
uint8_t    g_motionParam = 0;

uint8_t    g_curPattern  = 0;
uint8_t    g_curTrack    = 0;
uint8_t    g_curStep     = 0;
uint8_t    g_curDrumLane = 0;
uint8_t    g_curOctave   = 4;

extern bool g_needRedraw;   // owned by ui.cpp

static uint32_t s_stepUs      = 0;
static uint32_t s_lastStepUs  = 0;
static uint32_t s_stepPeriod() { return 60000000UL / g_bpm / 4; }   // 16th notes

void sequencerInit() {
    memset(g_patterns, 0, sizeof(g_patterns));
    memset(g_song, SONG_EMPTY, sizeof(g_song));

    for (int s = 0; s < NUM_SYNTHS; s++) g_synths[s].init();
    g_synths[0].forEach([](SynthVoice& v){ v.oscMode = OSC_SAW; v.fltCutoff = 0.30f;
                                           v.fltReso = 0.45f;   v.fltEnvAmt = 0.35f; });
    g_synths[1].forEach([](SynthVoice& v){ v.oscMode = OSC_SQR; v.fltCutoff = 0.45f;
                                           v.volume  = 0.6f; });
    g_synths[2].forEach([](SynthVoice& v){ v.oscMode = OSC_WT;  v.wtIndex = 0;
                                           v.fltCutoff = 0.55f; v.volume = 0.5f; });
    g_synths[2].setVoices(3);          // track 3 ships polyphonic (chords/pads)

    // lanes 0-3: 808 KICK SNARE CHAT CLAP | lanes 4-7: 909 KICK SNARE CHAT OHAT
    g_drumLanes[0].init(ENG_808, DT_KICK);
    g_drumLanes[1].init(ENG_808, DT_SNARE);
    g_drumLanes[2].init(ENG_808, DT_HAT_C);
    g_drumLanes[3].init(ENG_808, DT_HAT_O_OR_CLAP);
    g_drumLanes[4].init(ENG_909, DT_KICK);
    g_drumLanes[5].init(ENG_909, DT_SNARE);
    g_drumLanes[6].init(ENG_909, DT_HAT_C);
    g_drumLanes[7].init(ENG_909, DT_HAT_O_OR_CLAP);
    // 909 hats choke each other by default
    g_drumLanes[6].chokeGroup = 1;
    g_drumLanes[7].chokeGroup = 1;
}

// ---------- triggering ----------
static void triggerLane(uint8_t lane) {
    if (lane >= NUM_DRUM_LANES) return;
    uint8_t grp = g_drumLanes[lane].chokeGroup;
    if (grp != 0) {
        for (int i = 0; i < NUM_DRUM_LANES; i++)
            if (i != lane && g_drumLanes[i].chokeGroup == grp)
                g_drumLanes[i].choke();
    }
    g_drumLanes[lane].trigger();
}

static void triggerStep(uint8_t step, uint8_t pat, bool dropSynth = false, bool dropDrum = false) {
    Pattern& p = g_patterns[pat];

    if (!dropSynth)
    for (int s = 0; s < NUM_SYNTHS; s++) {
        if (g_synthMute[s]) continue;
        const SynthCell& c = p.synth[s][step];
        bool mono = (g_synths[s].voices <= 1);
        for (int i = 0; i < (mono ? 1 : MAX_POLY); i++)
            if (c.note[i] != NOTE_EMPTY)
                g_synths[s].noteOn(noteToFreq(c.note[i], c.oct[i]),
                                   c.accent, mono && c.slide);
    }

    if (!dropDrum && !g_drumMute) {
        uint8_t mask = p.drums[step];
        for (int l = 0; l < NUM_DRUM_LANES; l++)
            if (mask & (1 << l)) triggerLane(l);
    }
}

// ---------- transport ----------
void sequencerStart(bool fromTop) {
    if (fromTop) {
        g_playStep = 0;
        if (g_songMode) {
            g_songPos = g_songLoopStart;
            if (g_song[g_songPos] != SONG_EMPTY) g_playPattern = g_song[g_songPos];
        }
    }
    if (!g_songMode) g_playPattern = g_curPattern;
    s_lastStepUs = micros() - s_stepPeriod();   // fire step immediately
    g_playing = true;
}

void sequencerStop() {
    g_playing = false;
    g_playStep = 0;
}

static void songAdvance() {
    // find next non-empty slot; wrap to loop start
    uint8_t start = g_songPos;
    for (int i = 0; i < SONG_LENGTH; i++) {
        g_songPos = (g_songPos + 1) % SONG_LENGTH;
        if (g_songPos == 0 && start != SONG_LENGTH - 1) g_songPos = g_songLoopStart;
        if (g_song[g_songPos] != SONG_EMPTY) { g_playPattern = g_song[g_songPos]; return; }
        if (g_songPos == start) break;   // nothing found
    }
    // fallback: stay on current
}

// A step is worth repeating if any un-muted track actually sounds on it.
static bool stepSounds(const Pattern& p, uint8_t step) {
    for (int s = 0; s < NUM_SYNTHS; s++)
        if (!g_synthMute[s] && !p.synth[s][step].empty()) return true;
    if (!g_drumMute && p.drums[step]) return true;
    return false;
}

// Probability gate for normal playback (tilt thinning). true = let the step fire.
static bool stepKeeps(uint8_t step) {
    if (g_stepProb >= 100) return true;
    if (g_motion.probMode == 1) {                 // BAR: deterministic per step index
        uint32_t h = step * 2654435761u;          // cheap hash -> stable 0..99 per step
        return (uint8_t)((h >> 24) % 100) < g_stepProb;
    }
    static bool seeded = false;                    // RND: fresh roll each step
    if (!seeded) { randomSeed(micros()); seeded = true; }
    return (long)random(100) < (long)g_stepProb;
}

// Fire a normal (non-stutter) step, thinning only the targeted tracks.
static void triggerNormal(uint8_t step, uint8_t pat) {
    bool drop = !stepKeeps(step);
    bool dropSyn = drop && (g_motion.probTarget == 0 || g_motion.probTarget == 2);
    bool dropDrm = drop && (g_motion.probTarget == 0 || g_motion.probTarget == 1);
    triggerStep(step, pat, dropSyn, dropDrm);
}

void sequencerTick() {
    if (!g_playing) return;

    // Stutter release behaviour:
    //   true  = "to tempo"  : the groove keeps running underneath (silent) while you
    //                         stutter, so on release it resumes where it would have been.
    //   false = "freeze"    : hold on the slice; on release continue from that point.
    const bool STUTTER_TO_TEMPO = true;

    static uint8_t  s_stutterStep   = 0;
    static uint8_t  s_stutterPat    = 0;
    static bool     s_wasStuttering = false;
    static uint32_t s_lastStutterUs = 0;

    uint8_t r = g_stutterRate;                       // 0 off, 1=1/8, 2=1/16, 3=1/32
    bool stuttering = (r != 0);

    if (stuttering && !s_wasStuttering) {            // just engaged: latch a sounding step
        Pattern& p = g_patterns[g_playPattern];
        uint8_t st = (g_playStep == 0) ? (NUM_STEPS - 1) : (g_playStep - 1);  // last played
        for (int i = 0; i < NUM_STEPS; i++) {        // walk back until one actually sounds
            if (stepSounds(p, st)) break;
            st = (st == 0) ? (NUM_STEPS - 1) : (st - 1);
        }
        s_stutterStep   = st;
        s_stutterPat    = g_playPattern;             // latch pattern too (stable across song changes)
        s_lastStutterUs = s_lastStepUs;              // phase-align to the grid: no stumble on entry
    }
    s_wasStuttering = stuttering;

    uint32_t now = micros();
    uint32_t sp  = s_stepPeriod();                   // stutter repeat period
    if      (r == 1) sp *= 2;                        // 1/8
    else if (r == 3) sp /= 2;                        // 1/32

    if (STUTTER_TO_TEMPO) {
        // (1) real song clock: always advances on the 1/16 grid; audible only when
        //     not stuttering, so the timeline keeps its place underneath.
        if (now - s_lastStepUs >= s_stepPeriod()) {
            s_lastStepUs += s_stepPeriod();
            if (!stuttering) triggerNormal(g_playStep, g_playPattern);
            g_playStep++;
            if (g_playStep >= NUM_STEPS) {
                g_playStep = 0;
                if (g_songMode) songAdvance();
                else            g_playPattern = g_curPattern;
            }
            g_needRedraw = true;
        }
        // (2) stutter sub-clock: repeat the latched slice at the chosen rate.
        if (stuttering && now - s_lastStutterUs >= sp) {
            s_lastStutterUs += sp;
            triggerStep(s_stutterStep, s_stutterPat);
        }
    } else {
        // freeze variant: hold the slice, don't advance the timeline.
        uint32_t period = stuttering ? sp : s_stepPeriod();
        if (now - s_lastStepUs < period) return;
        s_lastStepUs += period;
        if (stuttering) {
            triggerStep(s_stutterStep, s_stutterPat);
        } else {
            triggerNormal(g_playStep, g_playPattern);
            g_playStep++;
            if (g_playStep >= NUM_STEPS) {
                g_playStep = 0;
                if (g_songMode) songAdvance();
                else            g_playPattern = g_curPattern;
            }
        }
        g_needRedraw = true;
    }
}

// ---------- live input ----------
// quantize: current step if within first half, else next step
static uint8_t quantizedStep() {
    uint32_t elapsed = micros() - s_lastStepUs;
    uint8_t  step    = g_playStep == 0 ? NUM_STEPS - 1 : g_playStep - 1;  // step just triggered
    if (elapsed > s_stepPeriod() / 2) step = g_playStep;                   // round up
    return step % NUM_STEPS;
}

void liveSynthNote(uint8_t track, uint8_t note, uint8_t octave, bool accent, bool legato) {
    if (track >= NUM_SYNTHS) return;
    bool poly = (g_synths[track].voices > 1);
    // poly: legato means "chord", not slide
    g_synths[track].noteOn(noteToFreq(note, octave), accent, !poly && legato);

    if (g_recEnabled) {
        if (g_playing) {
            uint8_t st = quantizedStep();
            SynthCell& c = g_patterns[g_playPattern].synth[track][st];
            if (poly && legato && !c.empty()) { c.add(note, octave); if (accent) c.accent = true; }
            else c.setMono(note, octave, accent, !poly && legato);
        } else {
            // step-write: keys held together stack a chord on the same step
            if (poly && legato) {
                uint8_t prev = (uint8_t)((g_curStep + NUM_STEPS - 1) % NUM_STEPS);
                SynthCell& c = g_patterns[g_curPattern].synth[track][prev];
                if (!c.empty()) { c.add(note, octave); if (accent) c.accent = true;
                                  g_needRedraw = true; return; }
            }
            g_patterns[g_curPattern].synth[track][g_curStep].setMono(note, octave, accent, false);
            g_curStep = (g_curStep + 1) % NUM_STEPS;   // step-write advance
        }
        g_needRedraw = true;
    }
}

void liveDrumHit(uint8_t lane) {
    triggerLane(lane);
    if (g_recEnabled) {
        if (g_playing) {
            g_patterns[g_playPattern].drums[quantizedStep()] |= (1 << lane);
        } else {
            g_patterns[g_curPattern].drums[g_curStep] |= (1 << lane);
            g_curStep = (g_curStep + 1) % NUM_STEPS;
        }
        g_needRedraw = true;
    }
}

// ---------- helpers for the one-key layout / sampling ----------
void clonePatternTo(uint8_t dst) {
    if (dst >= NUM_PATTERNS) return;
    if (dst != g_curPattern) g_patterns[dst] = g_patterns[g_curPattern];
}

void triggerLaneLive(uint8_t lane) {
    triggerLane(lane);
}

// ---------- step editing ----------
void toggleDrumAtCursor(uint8_t lane) {
    g_patterns[g_curPattern].drums[g_curStep] ^= (1 << lane);
    g_needRedraw = true;
}

void writeSynthAtCursor(uint8_t note, uint8_t octave, bool accent) {
    if (g_curTrack >= NUM_SYNTHS) return;
    g_patterns[g_curPattern].synth[g_curTrack][g_curStep].setMono(note, octave, accent, false);
    g_curStep = (g_curStep + 1) % NUM_STEPS;
    g_needRedraw = true;
}

void clearCellAtCursor() {
    if (g_curTrack < NUM_SYNTHS)
        g_patterns[g_curPattern].synth[g_curTrack][g_curStep].clear(g_curOctave);
    else
        g_patterns[g_curPattern].drums[g_curStep] = 0;
    g_needRedraw = true;
}

void clearStepAtPlayhead() {
    uint8_t st = g_playStep == 0 ? NUM_STEPS - 1 : g_playStep - 1;
    if (g_curTrack < NUM_SYNTHS)
        g_patterns[g_playPattern].synth[g_curTrack][st].clear(g_curOctave);
    else
        g_patterns[g_playPattern].drums[st] = 0;
    g_needRedraw = true;
}

// ---------- demo ----------
void loadDemoPattern() {
    Pattern& p = g_patterns[g_curPattern];
    memset(&p, 0, sizeof(p));

    // acid bassline on synth 1
    const uint8_t bl[NUM_STEPS]  = {1,0,1,0, 4,0,1,0, 1,0,8,0, 6,0,4,0};
    const bool    acc[NUM_STEPS] = {1,0,0,0, 1,0,0,0, 0,0,1,0, 0,0,1,0};
    const bool    sld[NUM_STEPS] = {0,0,0,0, 0,0,1,0, 0,0,0,0, 1,0,0,0};
    for (int i = 0; i < NUM_STEPS; i++)
        if (bl[i]) p.synth[0][i].setMono(bl[i], 2, acc[i], sld[i]);

    // poly chord stabs on synth 3 (Am: A-C-E), showing off VOICES=3
    p.synth[2][0].setMono(10, 3, false, false);            // A3
    p.synth[2][0].add(1, 4); p.synth[2][0].add(5, 4);      // + C4 E4
    p.synth[2][8] = p.synth[2][0];

    // 909 kick 4-on-floor (lane 4), 808 clap on 2&4 (lane 3), 909 hats offbeat (lane 6)
    for (int i = 0; i < NUM_STEPS; i += 4)  p.drums[i] |= (1 << 4);
    p.drums[4] |= (1 << 3); p.drums[12] |= (1 << 3);
    for (int i = 2; i < NUM_STEPS; i += 4)  p.drums[i] |= (1 << 6);
    for (int i = 0; i < NUM_STEPS; i += 2)  p.drums[i] |= (1 << 2);
}
