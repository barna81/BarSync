# Changelog — BarSync

*[Deutsche Version](CHANGELOG.md)*

## Hardware Rev. 1.1 (2026-08-25)

**Important fix:**
- **R3 (optocoupler output pull-up) corrected: +5V → +3V3.** In rev. 1.0,
  this pull-up was incorrectly tied to the +5V rail. Since the same node
  feeds directly into ESP32 GPIO15, GPIO15 sat near 5V continuously during
  MIDI idle — outside the ESP32's specification (not 5V-tolerant, absolute
  max rating roughly VDD+0.3V). Rev. 1.0 was cancelled before fabrication;
  this fix is included from the start in rev. 1.1.

**Other changes:**
- **Added C1, C2** — 100nF decoupling capacitors on the Vcc/GND pins of U1
  (6N139) and U2 (SN7406N) for cleaner MIDI timing.
- **Tied unused 7406 gate inputs (pins 5, 9, 11, 13) to GND** instead of
  leaving them floating — avoids unnecessary switching/noise on unused TTL
  inputs.

Affects: `hardware/kicad/BarSync/*`, `hardware/BarSync_BOM.md`/`.csv`,
`hardware/BarSync_Aufbauanleitung.md`. The firmware (v1.0.1) is unaffected
by these changes.

---

## Hardware Rev. 1.0 / Firmware v1.0.1 (2026-08-23)

- First complete version: schematic, PCB layout, and firmware finished.
- Firmware fully in English (display text + comments).
- Settings menu, nudge mode, three-stage reset, MIDI analyzer.
- **Note:** This revision was cancelled before PCB fabrication, see rev. 1.1
  above — do not build.

---

*[Deutsche Version](CHANGELOG.md)*
