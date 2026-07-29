// ============================================================
// CardputerGroovebox - storage.cpp
// ============================================================
#include "storage.h"
#include "sequencer.h"
#include "sampler.h"
#include "wavetable.h"
#include <SD.h>
#include <string.h>

uint8_t g_curProject = 0;

#define GBX_MAGIC   0x31584247u   // "GBX1"
#define GBX_VERSION 3             // v3: + motion/IMU settings block (v2/v1 still load)

// ---- serialized structures (POD, packed layout kept stable) ----
struct __attribute__((packed)) SaveSynth {
    uint8_t oscMode, wtIndex;
    float   cutoff, reso, envAmt, ampDec, filtDec, volume;
};

struct __attribute__((packed)) SaveDrumLane {
    uint8_t engine, type, chokeGroup;
    float   volume, tune, decay;
    char    sampleName[SAMPLE_NAME_LEN];   // empty = none
};

// ---------- v1 (legacy, mono cells) ----------
struct __attribute__((packed)) SaveCellV1 {
    uint8_t note, octave, flags;   // bit0 accent, bit1 slide
};
struct __attribute__((packed)) ProjectFileV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t bpm;
    uint8_t  songLoopStart;
    uint8_t  song[SONG_LENGTH];
    SaveSynth    synths[NUM_SYNTHS];
    SaveDrumLane lanes[NUM_DRUM_LANES];
    SaveCellV1   cells[NUM_PATTERNS][NUM_SYNTHS][NUM_STEPS];
    uint8_t      drums[NUM_PATTERNS][NUM_STEPS];
};

// ---------- v2 (poly cells + voices) ----------
struct __attribute__((packed)) SaveCell {
    uint8_t note[MAX_POLY];
    uint8_t oct [MAX_POLY];
    uint8_t flags;                 // bit0 accent, bit1 slide
};
struct __attribute__((packed)) ProjectFile {
    uint32_t magic;
    uint16_t version;
    uint16_t bpm;
    uint8_t  songLoopStart;
    uint8_t  song[SONG_LENGTH];
    uint8_t  voices[NUM_SYNTHS];   // 1..MAX_POLY per synth track
    SaveSynth    synths[NUM_SYNTHS];
    SaveDrumLane lanes[NUM_DRUM_LANES];
    SaveCell     cells[NUM_PATTERNS][NUM_SYNTHS][NUM_STEPS];
    uint8_t      drums[NUM_PATTERNS][NUM_STEPS];
};

static void slotPath(uint8_t slot, char* out, size_t n) {
    snprintf(out, n, "%s/P%u.gbx", DIR_PROJECTS, (unsigned)(slot + 1));
}

// v3 appends this block right after the v2 body. reserved[] lets us add more
// motion settings later without another version bump (0 = default on old v3 files).
struct __attribute__((packed)) SaveMotion {
    uint8_t enabled, stutterEnd, probTarget, probMode;
    uint8_t reserved[12];
};

#define LAST_SLOT_FILE  DIR_ROOT "/last.dat"

static void writeLastSlot(uint8_t slot) {
    SD.remove(LAST_SLOT_FILE);
    File f = SD.open(LAST_SLOT_FILE, FILE_WRITE);
    if (!f) return;
    f.write(&slot, 1);
    f.close();
}

int storageLastSlot() {
    File f = SD.open(LAST_SLOT_FILE, FILE_READ);
    if (!f) return -1;
    int b = f.read();
    f.close();
    return (b >= 0 && b < NUM_PROJECT_SLOTS) ? b : -1;
}

bool storageProjectExists(uint8_t slot) {
    char path[64]; slotPath(slot, path, sizeof(path));
    return SD.exists(path);
}

bool storageSaveProject(uint8_t slot) {
    static ProjectFile pf;   // static: too big for stack
    memset(&pf, 0, sizeof(pf));

    pf.magic = GBX_MAGIC;
    pf.version = GBX_VERSION;
    pf.bpm = g_bpm;
    pf.songLoopStart = g_songLoopStart;
    memcpy(pf.song, g_song, SONG_LENGTH);

    for (int s = 0; s < NUM_SYNTHS; s++) {
        SynthVoice& v = g_synths[s].v[0];
        pf.voices[s] = g_synths[s].voices;
        pf.synths[s] = { (uint8_t)v.oscMode, v.wtIndex,
                         v.fltCutoff, v.fltReso, v.fltEnvAmt,
                         v.ampDecRate, v.filtDecRate, v.volume };
    }
    for (int l = 0; l < NUM_DRUM_LANES; l++) {
        DrumLane& d = g_drumLanes[l];
        SaveDrumLane& o = pf.lanes[l];
        o.engine = d.engine; o.type = d.type; o.chokeGroup = d.chokeGroup;
        o.volume = d.volume; o.tune = d.tune; o.decay = d.decay;
        if (d.engine == ENG_SMPL && d.sampleSlot >= 0 && d.sampleSlot < g_numSamples)
            strncpy(o.sampleName, g_samples[d.sampleSlot].name, SAMPLE_NAME_LEN - 1);
    }
    for (int p = 0; p < NUM_PATTERNS; p++) {
        for (int s = 0; s < NUM_SYNTHS; s++)
            for (int st = 0; st < NUM_STEPS; st++) {
                const SynthCell& c = g_patterns[p].synth[s][st];
                SaveCell& o = pf.cells[p][s][st];
                for (int i = 0; i < MAX_POLY; i++) { o.note[i] = c.note[i]; o.oct[i] = c.oct[i]; }
                o.flags = (uint8_t)((c.accent ? 1 : 0) | (c.slide ? 2 : 0));
            }
        memcpy(pf.drums[p], g_patterns[p].drums, NUM_STEPS);
    }

    char path[64]; slotPath(slot, path, sizeof(path));
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write((uint8_t*)&pf, sizeof(pf));

    SaveMotion sm = {};
    sm.enabled    = g_motion.enabled ? 1 : 0;
    sm.stutterEnd = g_motion.stutterEnd;
    sm.probTarget = g_motion.probTarget;
    sm.probMode   = g_motion.probMode;
    written += f.write((uint8_t*)&sm, sizeof(sm));
    f.close();
    bool ok = (written == sizeof(pf) + sizeof(sm));
    if (ok) writeLastSlot(slot);        // remember it for the next boot
    return ok;
}

// shared param/lane/song apply used by both format loaders
static void applySynth(int s, const SaveSynth& in, uint8_t voices) {
    SynthTrack& t = g_synths[s];
    t.forEach([&](SynthVoice& v) {
        v.oscMode     = (OscMode)(in.oscMode < OSC_COUNT ? in.oscMode : OSC_SAW);
        v.wtIndex     = (in.wtIndex < g_numWavetables) ? in.wtIndex : 0;
        v.fltCutoff   = in.cutoff;
        v.fltReso     = in.reso;
        v.fltEnvAmt   = in.envAmt;
        v.ampDecRate  = in.ampDec;
        v.filtDecRate = in.filtDec;
        v.volume      = in.volume;
        v.active      = false;
    });
    t.setVoices(voices);
}

static void applyLane(int l, const SaveDrumLane& o) {
    DrumLane& d = g_drumLanes[l];
    d.engine = (DrumEngine)(o.engine < ENG_COUNT ? o.engine : ENG_808);
    d.type   = (o.type < DT_COUNT) ? o.type : DT_KICK;
    d.chokeGroup = o.chokeGroup;
    d.volume = o.volume; d.tune = o.tune; d.decay = o.decay;
    d.sampleSlot = -1;
    d.sv.init(); d.smp.init();
    if (d.engine == ENG_SMPL && o.sampleName[0])
        d.sampleSlot = (int8_t)samplerLoad(o.sampleName);   // -1 if missing
}

bool storageLoadProject(uint8_t slot) {
    char path[64]; slotPath(slot, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    // header peek: magic + version decide the format
    uint8_t head[8];
    if (f.read(head, 8) != 8) { f.close(); return false; }
    uint32_t magic;  memcpy(&magic, head, 4);
    uint16_t version; memcpy(&version, head + 4, 2);
    if (magic != GBX_MAGIC || version < 1 || version > 3) { f.close(); return false; }
    f.seek(0);

    bool wasPlaying = g_playing;
    g_playing = false;   // pause audio triggering while we swap data
    bool ok = false;

    samplerClearAll();

    if (version >= 2) {                // v2 and v3 share the body; v3 adds a motion block
        static ProjectFile pf;
        size_t got = f.read((uint8_t*)&pf, sizeof(pf));

        MotionCfg m = { true, 1, 1, 0 };            // defaults (used for v2, or if block missing)
        if (got == sizeof(pf) && version >= 3) {
            SaveMotion sm;
            if (f.read((uint8_t*)&sm, sizeof(sm)) == sizeof(sm)) {
                m.enabled    = (sm.enabled != 0);
                m.stutterEnd = (uint8_t)(sm.stutterEnd & 1);
                m.probTarget = (uint8_t)(sm.probTarget % 3);
                m.probMode   = (uint8_t)(sm.probMode & 1);
            }
        }
        f.close();
        if (got == sizeof(pf)) {
            g_bpm = pf.bpm;
            g_songLoopStart = pf.songLoopStart;
            memcpy(g_song, pf.song, SONG_LENGTH);
            for (int s = 0; s < NUM_SYNTHS; s++) applySynth(s, pf.synths[s], pf.voices[s]);
            g_motion = m;                          // v3: loaded block; v2: defaults
            for (int l = 0; l < NUM_DRUM_LANES; l++) applyLane(l, pf.lanes[l]);
            for (int p = 0; p < NUM_PATTERNS; p++) {
                for (int s = 0; s < NUM_SYNTHS; s++)
                    for (int st = 0; st < NUM_STEPS; st++) {
                        const SaveCell& c = pf.cells[p][s][st];
                        SynthCell& o = g_patterns[p].synth[s][st];
                        for (int i = 0; i < MAX_POLY; i++) { o.note[i] = c.note[i]; o.oct[i] = c.oct[i]; }
                        o.accent = c.flags & 1; o.slide = c.flags & 2;
                    }
                memcpy(g_patterns[p].drums, pf.drums[p], NUM_STEPS);
            }
            ok = true;
        }
    } else {              // v1: legacy mono cells, migrate on load
        static ProjectFileV1 pf;
        size_t got = f.read((uint8_t*)&pf, sizeof(pf));
        f.close();
        if (got == sizeof(pf)) {
            g_bpm = pf.bpm;
            g_songLoopStart = pf.songLoopStart;
            memcpy(g_song, pf.song, SONG_LENGTH);
            for (int s = 0; s < NUM_SYNTHS; s++) applySynth(s, pf.synths[s], 1);
            g_motion = { true, 1, 1, 0 };          // v1 has no motion block: defaults
            for (int l = 0; l < NUM_DRUM_LANES; l++) applyLane(l, pf.lanes[l]);
            for (int p = 0; p < NUM_PATTERNS; p++) {
                for (int s = 0; s < NUM_SYNTHS; s++)
                    for (int st = 0; st < NUM_STEPS; st++) {
                        const SaveCellV1& c = pf.cells[p][s][st];
                        g_patterns[p].synth[s][st].setMono(c.note, c.octave,
                                                           c.flags & 1, c.flags & 2);
                    }
                memcpy(g_patterns[p].drums, pf.drums[p], NUM_STEPS);
            }
            ok = true;
        }
    }

    g_playing = wasPlaying && ok;
    return ok;
}
