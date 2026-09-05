# Assembly Guide — BarSync

*[Deutsche Version](BarSync_Aufbauanleitung.md)*

Recommended build order, from simplest to most demanding.
Parts list and references match the verified `BarSync_BOM.en.md`.

---

## 1. Preparation

- Open the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)
- Check off all components against `BarSync_BOM.en.md`/`.csv` before you start
- **All four resistor positions (R1–R4) are populated** — no position is left empty

## 2. Solder the Small Parts First

Order: start with the flattest/most robust components, then the more delicate ones.

1. **Resistors R1, R2, R3, R4** (polarity doesn't matter, no orientation to worry about)
2. **Capacitors C1, C2** (100nF, ceramic/X7R — also non-polarized, can be soldered either way round)
3. **Diode D1** — mind the polarity! The cathode band on the diode must point toward the corresponding marking on the board
4. **DIP sockets** for U1 (8-pin) and U2 (14-pin) — the notch/dot on the socket marks pin 1; align it with the marking on the board

## 3. Solder the Headers

5. **J1** (2-pin, MIDI-IN), **J2** (3-pin, MIDI-THRU), **J3/J4/J5** (2-pin each, buttons), **J6** (7-pin, display)
- Solder all of them straight and vertical — it's best to tack down one pin first, check the alignment, and only then solder the rest

## 4. ESP32 Female Headers

6. Solder in the two 15-pin female headers (A1 footprint) — **first test-fit the ESP32 board on top** to check that the pin spacing matches exactly before soldering it in for good

## 5. Insert the ICs

7. **6N139** into the DIP-8 socket (U1) — mind the notch
8. **SN7406N** into the DIP-14 socket (U2) — mind the notch

## 6. Prepare and Connect the Cables

!!!IMPORTANT: make sure the polarity/pinout matches the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)!!!

9. Plug the Dupont cables into the corresponding headers:
   - J1 (2-pin) → MIDI-IN socket (pin 4 + pin 5 of the DIN-5 socket)
   - J2 (3-pin) → MIDI-THRU socket (pin 2 + pin 4 + pin 5 of the DIN-5 socket)
   - J3/J4/J5 → respective button
   - J6 (7-pin) → display

## 7. Plug in the ESP32 Board

10. Carefully plug the ESP32 board onto the female headers — make sure it's oriented correctly (USB port facing the intended enclosure opening)

## 8. Flash the Firmware

11. Open `barsync.ino` in the Arduino IDE
12. Board: select **ESP32 Dev Module**
13. Install libraries (if not done already): **"MIDI Library"** (FortySevenEffects), **"U8g2"** (olikraus)
14. Upload

## 9. First Functional Test — Before Enclosure Installation

15. On startup, the display should show the boot screen ("BarSync")
16. Connect a MIDI source, start the clock → the display should switch to "RUN"
17. Test all three buttons individually (trigger custom button — currently: SET 1.1 —, change grid, trigger reset)
18. Only once everything works: install it into the enclosure

---

## Known Pitfalls

| Problem | Solution |
|---|---|
| Display stays dark | Check SPI wiring at J6 (pin order), check contrast in the setup menu |
| No MIDI signal detected | Check optocoupler orientation (U1) and D1 polarity |
| Buttons don't respond or respond incorrectly | Check the mapping: J3=Custom(GPIO32), J4=Grid(GPIO33), J5=Reset(GPIO25) |

---

*For BarSync hardware rev. 1.1 — component references verified against `BarSync.kicad_pcb`/`.net`.
See also `pinplan.en.md` for the full pin reference and `CHANGELOG.en.md` for the change history.*

---

*[Deutsche Version](BarSync_Aufbauanleitung.md)*
