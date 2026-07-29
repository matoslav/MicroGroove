# MicroGroove — Cardputer-ADV update

Motion control, sampling fixes, and quality-of-life changes for the M5Stack
**Cardputer-ADV** (Stamp-S3A / ES8311 / BMI270, no PSRAM).

**Scope:** 12 files changed, ~460 insertions. No breaking changes — existing v2
projects load unchanged.

---

## Build note (read first if the mic is silent)

Mic capture on the ADV needs an **arduino-esp32 core that bundles ESP-IDF 5.4.x**
(core **3.2.x**). With core 3.3.x (IDF 5.5.x) the ES8311 mic returns silent/DC
samples — an upstream regression (espressif/esp-idf #18621), not a MicroGroove
bug. Build with core 3.2.x and the mic works.

---

## New features

### Motion control (BMI270 IMU)

The ADV's accelerometer becomes a hands-on performance controller. Neutral is
learned at boot (rest the unit flat), then:

- **Tilt left / right → filter cutoff.** Absolute control on the selected synth
  track: a centre deadzone keeps the SOUND-page cutoff, tilting sweeps toward
  fully open / fully shut. The per-note filter envelope defaults were lowered so
  these moves are clearly audible.
- **Tilt one way → stutter / beat-repeat.** Three musical rates (1/8, 1/16,
  1/32). "To tempo": the groove keeps running underneath, so on release it
  resumes where it would have been. The repeated slice latches a *sounding* step
  (and its pattern), so it never repeats silence and stays stable across
  song-mode pattern changes. Onset is phase-locked to the grid (no stumble).
- **Tilt the other way → step probability (thinning).** Three bands (90 / 70 /
  50 %). Target is selectable: ALL / DRUMS / SYNTHS. Mode is selectable: RANDOM
  (fresh each step) or LOCKED (deterministic per step index — groovy, repeatable).

### MOTION page

A new page (in the PG cycle) to configure the above from the device — same
row/value editing as the SOUND page:

- **MOTION** — master on/off
- **STUTTER** — TOWARD YOU / AWAY (which tilt end stutters; the other end thins)
- **THIN TRACKS** — ALL / DRUMS / SYNTHS
- **THIN MODE** — RANDOM / LOCKED

All the tilt angles and feel live in one clearly-named block of `#define`s at the
top of `Microgroove.ino` (deadzones, bands, smoothing, and a `MOT_PITCH_INVERT`
for units whose accelerometer axis is flipped).

### Boot into your last project

On boot the firmware reopens the **last saved project** (tracked by a small
`/groovebox/last.dat` marker) instead of the hard-coded demo. If there's no
marker or the load fails, it starts on an **empty pattern**. The factory demo is
still available on demand (hold LOAD + SAVE).

---

## Bug fixes

- **Mic sampling reset.** The audio render task (core 0) was never paused while
  mic start/stop tore down and re-initialised the shared ES8311 codec, crashing
  the device right after "SAMPLED!". Added a pause/parked handshake so nothing
  touches the Speaker during codec re-init.
- **Double sound / freeze when assigning a sample.** The lane's engine/voice was
  swapped on the main thread while the render task read it — a torn read that
  played the old sound then the new one, and occasionally dereferenced a stale
  voice pointer (freeze). The swap now happens with the render task parked, old
  voices stopped first. Mute flags made `volatile`.
- **Recordings overwriting after reboot.** The MIC/RSM name counter reset each
  boot and overwrote earlier files. Now picks the next free `NNxx.wav` on the SD.
- **Stutter onset stumble.** The repeat sub-clock now phase-aligns to the grid.

---

## Improvements

- **RAM management (no PSRAM).** On the SAMPLE page, **CL/Z unloads all samples
  from the pool** (files stay on the SD), safely detaching any sample lanes.
- **Non-destructive preview.** Auditioning ("." on the SAMPLE page) plays from a
  temporary scratch buffer instead of permanently filling the shared pool — you
  can browse freely; only assigning to a lane consumes RAM.
- **Sample normalization.** Captures are peak-normalised toward -1 dBFS on commit
  (boost-only, +18 dB ceiling) so the quiet ADV mic yields usable levels.
- **Synth defaults.** Lower filter-envelope amount and shorter filter decay, so
  cutoff moves (knob or tilt) read clearly instead of being masked by each note's
  self-sweep. Pad/amp decay left long.
- **Readable UI.** Arrow-key footers now show `^ v` / `< >` to match a printed
  overlay.

---

## Project format: GBX v3 (backward compatible)

Motion settings are stored **per project**. The format is versioned:

- Save writes **v3** (v2 body + a trailing motion block with reserved bytes for
  future settings — no version bump needed to add more later).
- Load accepts **v1, v2, v3**. Older projects load exactly as before and get
  default motion settings; re-saving upgrades them to v3. No manual conversion,
  nothing lost.

---

## File-by-file

| File | What changed |
|------|--------------|
| `Microgroove.ino` | IMU read + mapping (`motionUpdate`), tuning `#define`s, boot-into-last-project |
| `sequencer.cpp/.h` | stutter + probability engine, motion config, `volatile` mutes, `triggerStep` thinning |
| `audio_engine.cpp/.h` | render-task pause/park handshake; motion cutoff bias |
| `synth_voice.h` | cutoff bias input; lower default env amount / filter decay |
| `mic_sampler.cpp` | codec-safe start/stop, unique naming, normalization |
| `input.cpp` | MOTION page editing; clear-RAM; safe sample-assign; transient preview |
| `ui.cpp` | MOTION page rendering; arrow footers |
| `storage.cpp/.h` | GBX v3 with backward-compatible load; last-slot marker |
| `config.h` | `PAGE_MOTION` |

---

## Tested on

Cardputer-ADV, Arduino IDE, arduino-esp32 core 3.2.x (ESP-IDF 5.4.x).
