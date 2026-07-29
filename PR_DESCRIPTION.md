# Pull Request — Cardputer-ADV: motion control, sampling fixes, boot-into-last-project

**Title suggestion:** `Cardputer-ADV: IMU motion control, mic sampling fixes, GBX v3`

---

This PR brings the firmware to life on the **Cardputer-ADV** (Stamp-S3A /
ES8311 / BMI270, no PSRAM): it fixes mic sampling, adds hands-on motion control
via the accelerometer, and adds a few quality-of-life features. 12 files, ~460
insertions, **no breaking changes** — existing v2 projects load unchanged.

### Highlights

- **Motion control (BMI270):** tilt L/R = filter cutoff; tilt one way = stutter
  (1/8·1/16·1/32, to-tempo, latched slice); tilt the other way = step
  probability thinning (targetable ALL/DRUMS/SYNTHS, RANDOM/LOCKED). Configured
  from a new **MOTION page**; all feel constants centralised at the top of the
  sketch (incl. a per-unit axis-invert).
- **Mic sampling fixes:** codec re-init race (reset after "SAMPLED!"), sample
  assign race (double sound / occasional freeze), reboot filename overwrite,
  capture normalization for the quiet ADV mic.
- **RAM handling (no PSRAM):** non-destructive preview (audition from scratch,
  doesn't fill the pool) and a clear-RAM control on the SAMPLE page.
- **Boot into last saved project** (marker on SD), empty pattern otherwise; demo
  still available via LOAD+SAVE.
- **GBX v3:** motion settings saved per project; v1/v2 still load and upgrade on
  save (reserved bytes for future growth).
- Synth defaults tuned so filter moves are audible; arrow-symbol UI footers.

Full details and file-by-file breakdown in `CHANGELOG_ADV.md`.

### Build note

Mic capture needs **arduino-esp32 core 3.2.x** (ESP-IDF 5.4.x). Core 3.3.x
(IDF 5.5.x) records silence on the ES8311 due to upstream
espressif/esp-idf #18621 — not a firmware bug.

### Tested on

Cardputer-ADV, Arduino IDE, arduino-esp32 3.2.x.

---

*If you'd prefer this split into smaller PRs (e.g. sampling fixes / motion
control / persistence as three separate ones), happy to do that.*
