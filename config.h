// ============================================================
// CardputerGroovebox - config.h
// Global constants, pins, shared enums
// Target: M5Stack Cardputer-ADV (ESP32-S3FN8)
// ============================================================
#pragma once
#include <stdint.h>

// ---------- Audio ----------
#define SAMPLE_RATE      22050
#define AUDIO_BUF_LEN    256
#define INV_SAMPLE_RATE  (1.0f / (float)SAMPLE_RATE)
#define TWO_PI_F         6.28318530718f

// ---------- Tracks / sequencer ----------
#define NUM_SYNTHS       3
#define MAX_POLY         3     // voices per synth track (VOICES param 1..3)
#define NUM_DRUM_LANES   8
#define NUM_STEPS        16
#define NUM_PATTERNS     8
#define SONG_LENGTH      64
#define SONG_EMPTY       0xFF

// ---------- Display ----------
#define SCREEN_W         240
#define SCREEN_H         135

// ---------- SD card (Cardputer-ADV pinout, verified against M5 docs) ----------
#define SD_SPI_CS_PIN    12
#define SD_SPI_MOSI_PIN  14
#define SD_SPI_CLK_PIN   40
#define SD_SPI_MISO_PIN  39

#define DIR_ROOT         "/groovebox"
#define DIR_SAMPLES      "/groovebox/samples"
#define DIR_WAVETABLES   "/groovebox/wavetables"
#define DIR_PROJECTS     "/groovebox/projects"

// ---------- Sample memory ----------
#define SAMPLE_POOL_BYTES (192 * 1024)   // ~4.3 s of mono 16-bit @ 22.05 kHz
#define MAX_SAMPLES       16
#define SAMPLE_NAME_LEN   32

// ---------- Wavetables ----------
#define WT_SIZE          256
#define NUM_BUILTIN_WT   8
#define MAX_USER_WT      8
#define NUM_WT_TOTAL     (NUM_BUILTIN_WT + MAX_USER_WT)
#define WT_NAME_LEN      12

// ---------- Shared enums ----------
enum OscMode : uint8_t { OSC_SAW = 0, OSC_SQR, OSC_TRI, OSC_SIN, OSC_WT, OSC_COUNT };

enum DrumEngine : uint8_t { ENG_808 = 0, ENG_909, ENG_SMPL, ENG_COUNT };

// type index within a synth drum engine
enum DrumType : uint8_t { DT_KICK = 0, DT_SNARE, DT_HAT_C, DT_HAT_O_OR_CLAP, DT_COUNT };
// 808 set: KICK SNARE CHAT CLAP  |  909 set: KICK SNARE CHAT OHAT

enum Page : uint8_t { PAGE_PATTERN = 0, PAGE_SOUND, PAGE_SAMPLE, PAGE_SONG, PAGE_MOTION, PAGE_HELP, PAGE_COUNT };

// note values 1..12 = C..B, 0 = empty
#define NOTE_EMPTY 0

// ---------- Colors (RGB565) ----------
#define COL_BG       0x0000
#define COL_GRID     0x2965
#define COL_TEXT     0xFFFF
#define COL_DIM      0x7BEF
#define COL_SYNTH1   0x07FF   // cyan
#define COL_SYNTH2   0x07E0   // green
#define COL_SYNTH3   0xFFE0   // yellow
#define COL_DRUMS    0xF81F   // magenta
#define COL_ACCENT   0xFD20   // orange
#define COL_PLAYHEAD 0xFFFF
#define COL_REC      0xF800   // red
#define COL_CURSOR   0xFFFF
