# Changelog — BarSync

*[English version](CHANGELOG.en.md)*

## Hardware Rev. 1.1 (25.08.2026)

**Wichtiger Fix:**
- **R3 (Pull-up des Optokoppler-Ausgangs) korrigiert: +5V → +3V3.** In Rev. 1.0
  hing dieser Pull-up fälschlicherweise am +5V-Netz. Da derselbe Knoten direkt
  auf ESP32 GPIO15 geführt ist, lag GPIO15 im MIDI-Ruhezustand dauerhaft nahe
  5V an — außerhalb der Spezifikation des ESP32 (nicht 5V-tolerant, absolute
  Grenzspannung ca. VDD+0,3V). Rev. 1.0 wurde vor Fertigung storniert, dieser
  Fix ist von Anfang an in Rev. 1.1 enthalten.

**Weitere Änderungen:**
- **C1, C2 ergänzt** — 100nF-Abblockkondensatoren an den Vcc/GND-Pins von U1
  (6N139) und U2 (SN7406N) für saubereres MIDI-Timing.
- **Ungenutzte 7406-Gate-Eingänge (Pins 5, 9, 11, 13) auf GND gelegt**, statt
  offen zu bleiben — vermeidet unnötiges Schalten/Rauschen an unbenutzten
  TTL-Eingängen.

Betrifft: `hardware/kicad/BarSync/*`, `hardware/BarSync_BOM.md`/`.csv`,
`hardware/BarSync_Aufbauanleitung.md`. Die Firmware (v1.0.1) ist von diesen
Änderungen nicht betroffen.

---

## Hardware Rev. 1.0 / Firmware v1.0.1 (23.08.2026)

- Erste vollständige Version: Schaltplan, PCB-Layout und Firmware fertig.
- Firmware komplett auf Englisch (Display-Texte + Kommentare).
- Settings-Menü, Nudge-Modus, Drei-Stufen-Reset, MIDI-Analyzer.
- **Hinweis:** Diese Revision wurde vor der PCB-Fertigung storniert, siehe
  Rev. 1.1 oben — nicht nachbauen.

---

*[English version](CHANGELOG.en.md)*
