// ============================================================
// CardputerGroovebox - synth_voice.h
// Monophonic 303-style voice: osc (saw/sqr/tri/sin/wavetable),
// resonant SVF, decay envelopes, accent + slide.
// Voice/filter architecture derived from qwertyuu/Cardputer-Adv-Tracker (MIT).
// ============================================================
#pragma once
#include "config.h"
#include "wavetable.h"
#include <math.h>

struct SynthVoice {
    // Oscillator
    float   phase;
    float   freq;
    float   targetFreq;
    OscMode oscMode;
    uint8_t wtIndex;

    // State Variable Filter
    float fltLP, fltBP;
    float fltCutoff;    // 0..1
    float fltReso;      // 0..1
    float fltEnvAmt;    // 0..1

    // Envelopes (exponential decay)
    float ampEnv,  ampDecRate;
    float filtEnv, filtDecRate;

    // State
    float volume;
    bool  accent;
    bool  active;
    bool  slideActive;

    void init() {
        phase = 0; freq = 0; targetFreq = 0;
        oscMode = OSC_SAW; wtIndex = 0;
        fltLP = 0; fltBP = 0;
        fltCutoff = 0.4f; fltReso = 0.3f; fltEnvAmt = 0.30f;
        ampEnv = 0;  ampDecRate  = 0.99985f;
        filtEnv = 0; filtDecRate = 0.9993f;
        volume = 0.8f; accent = false; active = false; slideActive = false;
    }

    void noteOn(float newFreq, bool acc, bool slide) {
        targetFreq = newFreq;
        accent     = acc;
        if (slide && active) {
            slideActive = true;
            filtEnv = 1.0f;                       // re-squelch
            if (ampEnv < 0.6f) ampEnv = 0.6f;
        } else {
            slideActive = false;
            freq   = newFreq;
            ampEnv = 1.0f;
            filtEnv = 1.0f;
        }
        active = true;
    }

    inline float oscillator() {
        switch (oscMode) {
            case OSC_SAW: return 2.0f * phase - 1.0f;
            case OSC_SQR: return (phase < 0.5f) ? 0.7f : -0.7f;
            case OSC_TRI: return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
            case OSC_SIN: return sinf(phase * TWO_PI_F);
            case OSC_WT:  return wavetableRead(wtIndex, phase);
            default:      return 0.0f;
        }
    }

    inline float render(float cutPos = 0.0f) {
        if (!active) return 0.0f;

        if (slideActive) freq += (targetFreq - freq) * 0.005f;   // ~50 ms glide
        else             freq  = targetFreq;

        phase += freq * INV_SAMPLE_RATE;
        if (phase >= 1.0f) phase -= 1.0f;

        float osc = oscillator();

        ampEnv  *= ampDecRate;
        filtEnv *= filtDecRate;
        if (ampEnv < 0.002f) { ampEnv = 0; active = false; return 0.0f; }

        // cutPos: signed tilt position (-1..+1). 0 = keep the track's own cutoff.
        // >0 sweeps toward fully open, <0 toward shut, anchored at fltCutoff so the
        // centre is continuous with the SOUND-page value.
        float base = fltCutoff;
        if      (cutPos > 0.0f) base = fltCutoff + cutPos * (0.80f - fltCutoff);
        else if (cutPos < 0.0f) base = fltCutoff + cutPos * (fltCutoff - 0.05f);
        float cutMod = base + fltEnvAmt * filtEnv;
        if (accent) cutMod += 0.12f;
        if (cutMod > 0.8f)  cutMod = 0.8f;
        if (cutMod < 0.05f) cutMod = 0.05f;

        float fc = cutMod * 0.45f;
        float f  = 2.0f * sinf(3.14159f * fc);
        if (f > 0.85f) f = 0.85f;

        float q = 1.0f - fltReso * 0.85f;
        if (q < 0.1f) q = 0.1f;

        fltLP += f * fltBP;
        fltBP += f * (osc - fltLP - q * fltBP);

        if (fltLP > 3.0f) fltLP = 3.0f; else if (fltLP < -3.0f) fltLP = -3.0f;
        if (fltBP > 3.0f) fltBP = 3.0f; else if (fltBP < -3.0f) fltBP = -3.0f;

        float out = fltLP * ampEnv * volume;
        if (accent) out *= 1.25f;
        return out;
    }
};

// note (1..12) + octave -> Hz
static inline float noteToFreq(uint8_t note, uint8_t octave) {
    if (note == NOTE_EMPTY) return 0.0f;
    // C4 = 261.63 Hz; note 1 = C
    int semisFromC4 = (int)(note - 1) + ((int)octave - 4) * 12;
    return 261.6256f * powf(2.0f, (float)semisFromC4 / 12.0f);
}
