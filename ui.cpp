// ============================================================
// Microgroove - ui.cpp
// 240x135, sprite double-buffered. 5 pages.
// Drum grid drawn TR-style: kick (lane 1) at the bottom.
// Footer doubles as long-press progress bar / mic level meter.
// ============================================================
#include <M5Cardputer.h>
#include <SD.h>
#include "config.h"
#include "ui.h"
#include "sequencer.h"
#include "sampler.h"
#include "storage.h"
#include "wavetable.h"
#include "audio_engine.h"

Page    g_curPage    = PAGE_PATTERN;
bool    g_needRedraw = true;
uint8_t g_soundParam = 0;
uint8_t g_songCursor = 0;
float   g_holdProg   = 0.0f;
char    g_holdLabel[16] = "";

char    g_fileList[BROWSER_MAX][SAMPLE_NAME_LEN];
uint8_t g_fileCount = 0;
uint8_t g_fileSel   = 0;

static M5Canvas canvas(&M5Cardputer.Display);
static char     s_status[24] = "";
static uint32_t s_statusUntil = 0;

static const char* noteNames[] = {"--","C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};
static const uint16_t trackCols[4] = { COL_SYNTH1, COL_SYNTH2, COL_SYNTH3, COL_DRUMS };
static const char* engNames[]  = {"808","909","SMP"};
static const char* type808[]   = {"KICK","SNARE","CHAT","CLAP"};
static const char* type909[]   = {"KICK","SNARE","CHAT","OHAT"};
static const char* oscNames[]  = {"SAW","SQR","TRI","SIN","WT"};

void uiInit() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    canvas.setTextFont(1);
    canvas.setTextSize(1);
}

void uiStatus(const char* msg) {
    strncpy(s_status, msg, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    s_statusUntil = millis() + 1500;
    g_needRedraw = true;
}

void uiScanSampleDir() {
    g_fileCount = 0;
    File dir = SD.open(DIR_SAMPLES);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_fileCount < BROWSER_MAX) {
        String nm = f.name();
        int slash = nm.lastIndexOf('/');            // some core versions return full path
        if (slash >= 0) nm = nm.substring(slash + 1);
        if (!f.isDirectory() && (nm.endsWith(".wav") || nm.endsWith(".WAV"))) {
            strncpy(g_fileList[g_fileCount], nm.c_str(), SAMPLE_NAME_LEN - 1);
            g_fileList[g_fileCount][SAMPLE_NAME_LEN - 1] = 0;
            g_fileCount++;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    if (g_fileSel >= g_fileCount) g_fileSel = 0;
}

// ---------- header / footer ----------
static void drawHeader() {
    canvas.fillRect(0, 0, SCREEN_W, 11, COL_GRID);
    canvas.setTextColor(COL_TEXT);
    canvas.setCursor(2, 2);
    static const char* pageNames[] = {"PATTERN","SOUND","SAMPLE","SONG","HELP"};
    canvas.print(pageNames[g_curPage]);

    canvas.setCursor(60, 2);
    canvas.printf("BPM%3u", g_bpm);

    canvas.setCursor(104, 2);
    canvas.printf("PT%u", g_curPattern + 1);
    if (g_songMode) { canvas.setTextColor(COL_ACCENT); canvas.print(" SNG"); }

    canvas.setTextColor(COL_DIM);
    canvas.setCursor(156, 2);
    canvas.printf("P%u O%u", g_curProject + 1, g_curOctave);

    // transport
    if (g_playing) { canvas.setTextColor(COL_SYNTH2); canvas.setCursor(206, 2); canvas.print(">"); }
    if (g_recEnabled) canvas.fillCircle(222, 5, 3, COL_REC);
}

static void drawFooter(const char* hint) {
    // long-press progress / mic level meter takes over the footer
    if (g_holdProg > 0.001f) {
        int w = (int)((SCREEN_W - 4) * (g_holdProg > 1.0f ? 1.0f : g_holdProg));
        canvas.drawRect(1, SCREEN_H - 10, SCREEN_W - 2, 9, COL_GRID);
        canvas.fillRect(2, SCREEN_H - 9, w, 7, COL_ACCENT);
        if (g_holdLabel[0]) {
            canvas.setTextColor(COL_TEXT);
            canvas.setCursor(4, SCREEN_H - 9);
            canvas.print(g_holdLabel);
        }
        return;
    }
    canvas.setTextColor(COL_DIM);
    canvas.setCursor(2, SCREEN_H - 9);
    if (millis() < s_statusUntil) {
        canvas.setTextColor(COL_ACCENT);
        canvas.print(s_status);
    } else {
        canvas.print(hint);
    }
}

// ---------- PATTERN page ----------
static void drawPatternPage() {
    const int gx = 14, gw = 14;          // grid origin x, cell width
    int y = 14;

    // 3 synth rows (14 px tall)
    for (int t = 0; t < NUM_SYNTHS; t++) {
        canvas.setTextColor(g_synthMute[t] ? COL_GRID : trackCols[t]);
        canvas.setCursor(2, y + 3);
        canvas.printf("%d", t + 1);

        for (int st = 0; st < NUM_STEPS; st++) {
            int x = gx + st * gw;
            const SynthCell& c = g_patterns[g_curPattern].synth[t][st];
            uint16_t bg = COL_BG;
            if (g_playing && g_playPattern == g_curPattern &&
                st == (g_playStep + NUM_STEPS - 1) % NUM_STEPS) bg = 0x39E7;
            if (c.accent && !c.empty()) bg = 0x6180;   // dark orange
            canvas.fillRect(x, y, gw - 1, 12, bg);
            canvas.drawRect(x, y, gw - 1, 12, (st % 4 == 0) ? COL_DIM : COL_GRID);

            if (!c.empty()) {
                canvas.setTextColor(g_synthMute[t] ? COL_DIM : trackCols[t]);
                canvas.setCursor(x + 1, y + 2);
                canvas.print(noteNames[c.note[0]]);
                // chord: one dot per extra note, top-right of the cell
                for (int nn = 1; nn < MAX_POLY; nn++)
                    if (c.note[nn] != NOTE_EMPTY)
                        canvas.fillRect(x + gw - 3, y + 1 + (nn - 1) * 3, 2, 2,
                                        g_synthMute[t] ? COL_DIM : trackCols[t]);
                if (c.slide) canvas.drawFastHLine(x + 1, y + 10, gw - 3, COL_ACCENT);
            }
            if (t == g_curTrack && st == g_curStep)
                canvas.drawRect(x - 1, y - 1, gw + 1, 14, COL_CURSOR);
        }
        y += 15;
    }

    // 8 drum lanes, TR-style: lane 8 on top, kick (lane 1) at the bottom
    y += 1;
    for (int row = 0; row < NUM_DRUM_LANES; row++) {
        int l = NUM_DRUM_LANES - 1 - row;             // visual row -> lane
        DrumLane& d = g_drumLanes[l];
        bool sel = (g_curTrack == NUM_SYNTHS && g_curDrumLane == l);
        canvas.setTextColor(g_drumMute ? COL_GRID : (sel ? COL_TEXT : COL_DRUMS));
        canvas.setCursor(2, y);
        canvas.printf("%d", l + 1);

        for (int st = 0; st < NUM_STEPS; st++) {
            int x = gx + st * gw;
            bool hit = g_patterns[g_curPattern].drums[st] & (1 << l);
            uint16_t col = hit ? (g_drumMute ? COL_DIM : COL_DRUMS) : COL_GRID;
            if (g_playing && g_playPattern == g_curPattern &&
                st == (g_playStep + NUM_STEPS - 1) % NUM_STEPS && hit) col = COL_TEXT;
            canvas.fillRect(x + 3, y + 1, gw - 7, 5, col);
            if (sel && st == g_curStep)
                canvas.drawRect(x + 1, y - 1, gw - 3, 9, COL_CURSOR);
        }
        // engine tag
        canvas.setTextColor(COL_DIM);
        canvas.setCursor(gx + NUM_STEPS * gw + 2, y);
        canvas.print(d.engine == ENG_SMPL ? "S" : (d.engine == ENG_909 ? "9" : "8"));
        y += 8;
    }
}

// ---------- SOUND page ----------
static void drawBar(int x, int y, float v01) {
    canvas.drawRect(x, y, 62, 7, COL_GRID);
    canvas.fillRect(x + 1, y + 1, (int)(60 * v01), 5, COL_SYNTH1);
}

static void drawSoundPage() {
    bool onDrums = (g_curTrack == NUM_SYNTHS);
    canvas.setTextColor(trackCols[g_curTrack]);
    canvas.setCursor(2, 14);
    if (onDrums) {
        DrumLane& d = g_drumLanes[g_curDrumLane];
        canvas.printf("DRUM LANE %d  [%s]", g_curDrumLane + 1, engNames[d.engine]);
    } else {
        canvas.printf("SYNTH %d", g_curTrack + 1);
    }

    // scope
    canvas.drawRect(150, 24, 88, 44, COL_GRID);
    int prevY = 46;
    for (int i = 0; i < 86 && i * 2 < g_scopeIdx; i++) {
        int yy = 46 - (int)(g_scopeBuf[i * 2] * 20.0f);
        yy = constrain(yy, 25, 66);
        canvas.drawLine(151 + i - 1, prevY, 151 + i, yy, COL_SYNTH2);
        prevY = yy;
    }
    g_scopeIdx = 0;

    int y = 26;
    char val[24];

    if (!onDrums) {
        SynthTrack& trk = g_synths[g_curTrack];
        SynthVoice& v = trk.v[0];
        const char* names[] = {"OSC","WTABLE","CUTOFF","RESO","ENV AMT","FLT DEC","AMP DEC","VOLUME","VOICES"};
        for (int r = 0; r < 9; r++) {
            bool sel = (g_soundParam == r);
            canvas.setTextColor(sel ? COL_TEXT : COL_DIM);
            canvas.setCursor(2, y);
            canvas.printf("%s%-8s", sel ? ">" : " ", names[r]);
            switch (r) {
                case 0: canvas.print(oscNames[v.oscMode]); break;
                case 1: canvas.print(g_wtNames[v.wtIndex]);
                        if (v.oscMode != OSC_WT) { canvas.setTextColor(COL_GRID); canvas.print(" (off)"); }
                        break;
                case 2: drawBar(64, y, v.fltCutoff);  break;
                case 3: drawBar(64, y, v.fltReso);    break;
                case 4: drawBar(64, y, v.fltEnvAmt);  break;
                case 5: drawBar(64, y, (v.filtDecRate - 0.995f) / 0.00495f); break;
                case 6: drawBar(64, y, (v.ampDecRate  - 0.999f) / 0.00099f); break;
                case 7: drawBar(64, y, v.volume);     break;
                case 8: canvas.printf("%u %s", trk.voices,
                                      trk.voices > 1 ? "POLY" : "MONO(303)");
                        break;
            }
            y += 11;    // 9 rows must clear the footer bar
        }
    } else {
        DrumLane& d = g_drumLanes[g_curDrumLane];
        const char* names[] = {"LANE","ENGINE","TYPE","VOLUME","TUNE","DECAY","CHOKE"};
        for (int r = 0; r < 7; r++) {
            bool sel = (g_soundParam == r);
            canvas.setTextColor(sel ? COL_TEXT : COL_DIM);
            canvas.setCursor(2, y);
            canvas.printf("%s%-8s", sel ? ">" : " ", names[r]);
            switch (r) {
                case 0: canvas.printf("%d", g_curDrumLane + 1); break;
                case 1: canvas.print(engNames[d.engine]); break;
                case 2:
                    if (d.engine == ENG_SMPL) {
                        if (d.sampleSlot >= 0 && d.sampleSlot < g_numSamples)
                            canvas.print(g_samples[d.sampleSlot].name);
                        else { canvas.setTextColor(COL_GRID); canvas.print("(no sample)"); }
                    } else {
                        canvas.print(d.engine == ENG_909 ? type909[d.type] : type808[d.type]);
                    }
                    break;
                case 3: drawBar(64, y, d.volume); break;
                case 4: snprintf(val, sizeof(val), "%+.1f st", d.tune); canvas.print(val); break;
                case 5: snprintf(val, sizeof(val), "%.2fx", d.decay);   canvas.print(val); break;
                case 6: if (d.chokeGroup) canvas.printf("GRP %u", d.chokeGroup);
                        else canvas.print("OFF");
                        break;
            }
            y += 12;
        }
    }
}

// ---------- SAMPLE page ----------
static void drawSamplePage() {
    canvas.setTextColor(COL_TEXT);
    canvas.setCursor(2, 14);
    canvas.printf("SD SAMPLES (%u)", g_fileCount);

    // pool usage
    float use = g_poolCapacity ? (float)g_poolUsed / g_poolCapacity : 0;
    canvas.setCursor(130, 14);
    canvas.setTextColor(COL_DIM);
    canvas.printf("RAM %2d%%", (int)(use * 100));
    canvas.drawRect(180, 13, 58, 8, COL_GRID);
    canvas.fillRect(181, 14, (int)(56 * use), 6, use > 0.9f ? COL_REC : COL_SYNTH2);

    if (g_fileCount == 0) {
        canvas.setTextColor(COL_DIM);
        canvas.setCursor(2, 40);
        canvas.print("Put .wav files in");
        canvas.setCursor(2, 52);
        canvas.print(DIR_SAMPLES);
        canvas.setCursor(2, 70);
        canvas.print("or hold AUX to mic-sample");
        return;
    }

    // scrolling list, 8 rows
    int first = (g_fileSel > 3) ? g_fileSel - 3 : 0;
    if (first + 8 > g_fileCount) first = max(0, (int)g_fileCount - 8);
    int y = 26;
    for (int i = first; i < first + 8 && i < g_fileCount; i++) {
        bool sel = (i == g_fileSel);
        bool loaded = samplerFindByName(g_fileList[i]) >= 0;
        canvas.setTextColor(sel ? COL_TEXT : COL_DIM);
        canvas.setCursor(2, y);
        canvas.printf("%s%s", sel ? ">" : " ", g_fileList[i]);
        if (loaded) { canvas.setTextColor(COL_SYNTH2); canvas.setCursor(210, y); canvas.print("RAM"); }
        y += 12;
    }
}

// ---------- SONG page ----------
static void drawSongPage() {
    canvas.setTextColor(COL_TEXT);
    canvas.setCursor(2, 14);
    canvas.printf("SONG %s", g_songMode ? "[ON]" : "[off]");
    canvas.setTextColor(COL_DIM);
    canvas.setCursor(120, 14);
    canvas.printf("PROJECT P%u %s", g_curProject + 1,
                  storageProjectExists(g_curProject) ? "*" : "");

    const int gx = 8, gy = 28, cw = 14, ch = 16;
    for (int i = 0; i < SONG_LENGTH; i++) {
        int x = gx + (i % 16) * cw;
        int y = gy + (i / 16) * ch;
        bool isCursor = (i == g_songCursor);
        bool isPlay   = (g_songMode && g_playing && i == g_songPos);

        canvas.drawRect(x, y, cw - 1, ch - 2, isPlay ? COL_TEXT : COL_GRID);
        if (i == g_songLoopStart)
            canvas.drawFastVLine(x, y, ch - 2, COL_ACCENT);

        if (g_song[i] != SONG_EMPTY) {
            canvas.setTextColor(isPlay ? COL_TEXT : COL_SYNTH1);
            canvas.setCursor(x + 4, y + 4);
            canvas.printf("%u", g_song[i] + 1);
        }
        if (isCursor) canvas.drawRect(x - 1, y - 1, cw + 1, ch, COL_CURSOR);
    }
}

// ---------- HELP page ----------
static void drawHelpPage() {
    static const char* lines[] = {
        "spc play/stop (hold=from top)",
        "ctl page  / rec  z clr  xcvb=arrows",
        "` 1 2 3 tracks (hold=mute)",
        "4..- patterns (hold=clone)",
        "= load  del save  (both=demo)",
        "opt/alt bpm-+ (hold=octave/prj)",
        "notes: home row + q w r t y ...",
        "hold m=accent  , slide  n song",
        "hold . 0.5s = MIC SAMPLE lane",
        "hold n while playing = RESAMPLE",
    };
    int y = 16;
    canvas.setTextColor(COL_DIM);
    for (auto l : lines) { canvas.setCursor(2, y); canvas.print(l); y += 11; }
}

// ---------- draw dispatch ----------
static void drawMotionPage() {
    canvas.setTextColor(COL_TEXT);
    canvas.setCursor(2, 14);
    canvas.print("MOTION (IMU)");
    canvas.setTextColor(g_motion.enabled ? COL_ACCENT : COL_DIM);
    canvas.setCursor(150, 14);
    canvas.print(g_motion.enabled ? "ACTIVE" : "OFF");

    const char* names[MOTION_PARAMS] = { "MOTION", "STUTTER", "THIN TRACKS", "THIN MODE" };
    const char* tgt[3] = { "ALL", "DRUMS", "SYNTHS" };
    int y = 32;
    for (int r = 0; r < MOTION_PARAMS; r++) {
        bool sel = (g_motionParam == r);
        canvas.setTextColor(sel ? COL_TEXT : COL_DIM);
        canvas.setCursor(2, y);
        canvas.printf("%s%-12s", sel ? ">" : " ", names[r]);
        canvas.setCursor(120, y);
        switch (r) {
            case 0: canvas.print(g_motion.enabled ? "ON"  : "OFF");                    break;
            case 1: canvas.print(g_motion.stutterEnd == 0 ? "AWAY" : "TOWARD YOU"); break;
            case 2: canvas.print(tgt[g_motion.probTarget % 3]);                        break;
            case 3: canvas.print(g_motion.probMode == 0 ? "RANDOM" : "LOCKED");        break;
        }
        y += 18;
    }
    canvas.setTextColor(COL_DIM);
    canvas.setCursor(2, y + 4);
    canvas.print("cutoff: tilt left / right");
}

void uiDraw() {
    canvas.fillSprite(COL_BG);
    drawHeader();
    switch (g_curPage) {
        case PAGE_PATTERN: drawPatternPage();
            drawFooter("ctl:page /:rec spc:play"); break;
        case PAGE_SOUND:   drawSoundPage();
            drawFooter("^ v row  < > adjust  m=fine"); break;
        case PAGE_SAMPLE:  drawSamplePage();
            drawFooter(".:preview  4..-:assign lane"); break;
        case PAGE_SONG:    drawSongPage();
            drawFooter("4..-:set z:clr .:loop n:mode"); break;
        case PAGE_MOTION:  drawMotionPage();
            drawFooter("^ v row   < > change"); break;
        default:           drawHelpPage();
            drawFooter("MICROGROOVE"); break;
    }
    canvas.pushSprite(0, 0);
}

void uiSplash() {
    canvas.fillSprite(COL_BG);
    canvas.setTextSize(2);
    canvas.setTextColor(COL_SYNTH1); canvas.setCursor(15, 16); canvas.print("MICRO");
    canvas.setTextColor(COL_DRUMS);  canvas.setCursor(78, 16); canvas.print("GROOVE");
    canvas.setTextSize(1);
    canvas.setTextColor(COL_DIM);
    canvas.setCursor(15, 40);  canvas.print("lebiro.studio");
    canvas.setCursor(15, 60);  canvas.print("3 acid synths + 8 drum lanes");
    canvas.setCursor(15, 72);  canvas.print("808 / 909 / SD samples / mic");
    canvas.setCursor(15, 84);  canvas.print("wavetables / song mode");
    canvas.setTextColor(COL_ACCENT);
    canvas.setCursor(15, 110); canvas.print("Press any key...");
    canvas.pushSprite(0, 0);
}
