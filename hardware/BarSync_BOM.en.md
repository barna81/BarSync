# Bill of Materials — BarSync

*[Deutsche Version](BarSync_BOM.md)*

Based on the parts list extracted directly from `BarSync.kicad_pcb` and
`BarSync.net` — the actual current state (every reference, every value,
every footprint, and every net connection individually verified).

---

## Components Part 1 (matching the PCB layout I designed)

| Ref. | Component | Value | Qty | Footprint | Note |
|---|---|---|---|---|---|
| U1 | 6N139 | — | 1 | DIP-8, **socketed** | Optocoupler, MIDI-IN |
| U2 | SN7406N | — | 1 | DIP-14, **socketed** | Hex inverter, 2 gates in series for MIDI-Thru |
| D1 | 1N4148 | — | 1 | DO-35, horizontal | MIDI-IN series diode |
| R1 | Resistor | 220Ω, 1/4W | 1 | Axial, horizontal | MIDI-IN series resistor |
| R2 | Resistor | 4.7kΩ, 1/4W | 1 | Axial, horizontal | Vb bias resistor (6N139) |
| R3 | Resistor | 220Ω, 1/4W | 1 | Axial, horizontal | Pull-up, optocoupler output → **+3V3** (not +5V — feeds ESP32 GPIO15 directly!) |
| R4 | Resistor | 220Ω, 1/4W | 1 | Axial, horizontal | MIDI-THRU pin 4 current loop |
| C1 | Capacitor | 100nF, X7R | 1 | Radial, 5mm pitch | Decoupling capacitor for U1 (Vcc/GND) |
| C2 | Capacitor | 100nF, X7R | 1 | Radial, 5mm pitch | Decoupling capacitor for U2 (Vcc/GND) |
| J1 | Header | 2-pin, 2.54mm | 1 | Vertical | Board header → MIDI-IN socket (cable) |
| J2 | Header | 3-pin, 2.54mm | 1 | Vertical | Board header → MIDI-THRU socket (cable) |
| J3 | Header | 2-pin, 2.54mm | 1 | Vertical | Custom Switch |
| J4 | Header | 2-pin, 2.54mm | 1 | Vertical | Grid button |
| J5 | Header | 2-pin, 2.54mm | 1 | Vertical | Reset button |
| J6 | Header | 7-pin, 2.54mm | 1 | Vertical | Board header → Display (cable) |
| A1 | ESP32 female headers | 2× 15-pin, 2.54mm | 2 | DOIT_ESP32_DEVKIT_30Pins footprint | ESP32 board plugs in |
| — | Socket for U1 | DIP-8 | 1 | — | For 6N139 |
| — | Socket for U2 | DIP-14 | 1 | — | For SN7406N |
| — | Mounting hole | M2, 2.2mm | 4 | MountingHole | Mechanical only, not a component |

**Total: 4 resistors (R1–R4), 2 capacitors (C1–C2), 1 diode, 2 ICs (socketed), 6 headers (J1–J6), 4 mounting holes.**

---

## Components Part 2 (connected to the board via cables)

| Component | Qty | Note |
|---|---|---|
| ESP32 dev board (WROOM-32, DOIT 30-pin layout) | 1 | Plugs into the female headers (A1) |
| OLED display SSD1309, 2.42", 128×64, SPI | 1 | Via 7-pin Dupont cable to J6 |
| DIN-5 panel-mount socket | 2 | MIDI-IN + MIDI-THRU |
| Button (tactile switch or foot switch) | 3 | Custom, grid, reset |
| Dupont cable female-female, 2-pin | 4 | J1 (MIDI-IN) + J3/J4/J5 (buttons) |
| Dupont cable female-female, 3-pin | 1 | J2 (MIDI-THRU) |
| Dupont cable female-female, 7-pin | 1 | J6 (display) |

---

## Sourcing Notes

- **6N139 / SN7406N:** Standard logic ICs, searchable at Reichelt/Mouser/Digikey as "SN7406N" and "6N139" respectively
- **DIN-5 sockets:**
- **Resistors:** All 1/4W (0.25W), standard carbon film
- **Capacitors (C1/C2):** 100nF, X7R (not Y5V/Z5U — more stable over temperature/voltage), THT, e.g. Reichelt "X7R-5 100N" (5.0mm pitch)
- **Headers (J1–J6):** All 2.54mm pitch, straight/vertical

---

*Created for BarSync hardware rev. 1.1 — matches `BarSync.kicad_pcb`/`.net` as of
Aug 25, 2026. Changes vs. rev. 1.0: fixed R3 pull-up (+5V → +3V3, protects
ESP32 GPIO15), added C1/C2 as decoupling capacitors, tied unused 7406 gate
inputs to GND. See `CHANGELOG.en.md`.*

---

*[Deutsche Version](BarSync_BOM.md)*
