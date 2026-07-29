// ============================================================
// CardputerGroovebox - audio_engine.h
// ============================================================
#pragma once
#include "config.h"

extern float g_scopeBuf[SCREEN_W];
extern volatile int g_scopeIdx;

// Set g_audioPaused=true to park the render task so the codec (ES8311) can be
// safely re-initialised from the other core (mic start/stop). The task sets
// g_audioParked=true once it has actually stopped touching the Speaker.
extern volatile bool g_audioPaused;
extern volatile bool g_audioParked;

// Motion control (Stage 1): tilt applied to one synth track's filter.
// g_motionCut is a signed tilt position (-1..+1): 0 keeps the track's own cutoff,
// >0 opens, <0 closes. g_motionTrack = target synth track (-1 = none).
// Written by the main loop from the IMU, read by the render task.
extern volatile float g_motionCut;
extern volatile int   g_motionTrack;

void audioEngineStart();   // creates the render task on core 0
