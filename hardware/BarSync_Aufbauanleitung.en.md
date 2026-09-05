# Assembly Guide — BarSync

*[Deutsche Version](BarSync_Aufbauanleitung.md)*

Recommended build order, from simplest to most demanding.
Parts list and references match the verified `BarSync_BOM_en.md`.

---

## 1. Preparation

- Open the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)
- Check off all components against `BarSync_BOM_en.md`/`.csv` before you start
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

## 4. Solder the ESP32 onto the Board

6. Place the ESP32 in its designated spot and solder it in
   > **Note:** Contrary to how the KiCad footprint (`DOIT_ESP32_DEVKIT_30Pins`) looks, the ESP32 is soldered **directly onto the board, with no socket/female headers in between**. The footprint's 3D model suggests a socket, but that's purely cosmetic. Only the peripheral headers (J1–J6) get pin headers — not the ESP32 itself.

## 5. Insert the ICs

7. **6N139** into the DIP-8 socket (U1) — mind the notch
8. **SN7406N** into the DIP-14 socket (U2) — mind the notch

## 6. Fit the Electronics into the Enclosure

Required hardware (see `BarSync_BOM_en.md`/`.csv`):

- 4× Ruthex threaded insert, M3
- 4× socket-head screw M3×12
- 4× socket-head screw M2×4
- 8× socket-head screw M2×10
- 4× nut M2
- 12× washer M2

9. **Fit the PCB:** Position the PCB in the bottom half of the enclosure and screw it down with 4× M2×10 socket-head screws into the designated mount
10. **Display:** Mount the display with 4× M2×4 socket-head screws (use 2 washers per screw to shorten the effective thread length)
11. **MIDI jacks:** Insert the MIDI jacks (mind the IN/THRU arrangement), fasten with 4× M2×10 socket-head screws, and secure from the inside with 4× M2 nuts and 4× M2 washers
12. **Mount the buttons:** Insert the 3× buttons (Custom, Grid, Reset) into their designated openings and secure with the lock nuts
13. **Set the threaded inserts:** Press/melt the 4× Ruthex threaded inserts (M3) into the corresponding mounts in the top half of the enclosure (set the soldering iron to roughly the material's forming temperature — e.g. 220°C for PLA)

## 7. Prepare and Connect the Cables

!!!IMPORTANT: make sure the polarity/pinout matches the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)!!!

14. Plug the Dupont cables into the corresponding headers:
    - J1 (2-pin) → MIDI-IN socket (pin 4 + pin 5 of the DIN-5 socket)
    - J2 (3-pin) → MIDI-THRU socket (pin 2 + pin 4 + pin 5 of the DIN-5 socket)
    - J3/J4/J5 → respective button
    - J6 (7-pin) → display

## 8. Flash the Firmware

15. Open `barsync.ino` in the Arduino IDE
16. Board: select **ESP32 Dev Module**
17. Install libraries (if not done already): **"MIDI Library"** (FortySevenEffects), **"U8g2"** (olikraus)
18. Upload

## 9. First Functional Test — Before Closing the Enclosure

19. On startup, the display should show the boot screen ("BarSync")
20. Connect a MIDI source, start the clock → the display should switch to "RUN"
21. Test all three buttons individually (trigger the Custom button — currently: SET 1.1 —, change Grid, trigger Reset)
22. Only once everything works: screw the top and bottom enclosure halves together with 4× M3×12 socket-head screws

---

## Known Pitfalls

| Problem | Solution |
|---|---|
| Display stays dark | Check SPI wiring at J6 (pin order), check contrast in the setup menu |
| No MIDI signal detected | Check optocoupler orientation (U1) and D1 polarity |
| Buttons don't respond or respond incorrectly | Check the mapping: J3=Custom(GPIO32), J4=Grid(GPIO33), J5=Reset(GPIO25) |

---

*For BarSync hardware rev. 1.2 — component references verified against `BarSync.kicad_pcb`/`.net`.
See also `pinplan_en.md` for the full pin reference and `CHANGELOG_en.md` for the change history.*

---

*[Deutsche Version](BarSync_Aufbauanleitung.md)*
