# Bill of Materials — BarSync

*[Deutsche Version](BarSync_BOM.md)*

Based on the parts list extracted directly from `BarSync.kicad_pcb` and
`BarSync.net` — the actual current state (every reference, every value,
every footprint, and every net connection individually verified). Parts
with identical properties are grouped into a single row; the **Reichelt**
column gives the matching Reichelt order number (use it as the search
term in the shop). A ready-made shopping list is at the end of this file.

---

## Components Part 1 (on the PCB)

| Ref. | Component | Value | Qty | Footprint | Reichelt | Note |
|---|---|---|---|---|---|---|
| U1 | 6N139 | Optocoupler | 1 | DIP-8, **socketed** | `6N 139` | MIDI-IN |
| U2 | SN7406N | Hex inverter, open collector | 1 | DIP-14, **socketed** | `SN 7406N TEX` | MIDI-THRU driver, 2 gates in series |
| D1 | 1N4148 | Switching diode | 1 | DO-35, horizontal | `1N 4148` | MIDI-IN protection diode, antiparallel to U1 — **mind the polarity** |
| R1, R3, R4 | Resistor | 220 Ω | 3 | Axial 0207, horizontal | `METALL 220` | R1 = MIDI-IN series resistor · R3 = pull-up, optocoupler output → **+3V3** (not +5V — feeds ESP32 GPIO15 directly!) · R4 = MIDI-THRU current loop |
| R2 | Resistor | 4.7 kΩ | 1 | Axial 0207, horizontal | `METALL 4,70K` | Vb bias resistor (6N139) |
| C1, C2 | Capacitor | 100 nF, X7R | 2 | Radial, 5 mm pitch | `X7R-5 100N` | Decoupling capacitors U1 / U2 |
| J1, J3, J4, J5 | Pin header | 2-pin, 2.54 mm | 4 | Vertical | `SL 1X36G 2,54` ¹ | J1 = MIDI-IN · J3 = Custom · J4 = Grid · J5 = Reset |
| J2 | Pin header | 3-pin, 2.54 mm | 1 | Vertical | `SL 1X36G 2,54` ¹ | MIDI-THRU |
| J6 | Pin header | 7-pin, 2.54 mm | 1 | Vertical | `SL 1X36G 2,54` ¹ | Display |
| A1 | ESP32 DevKit (WROOM-32, DOIT, 30 pins) | — | 1 | DOIT_ESP32_DEVKIT_30Pins | — ² | Soldered directly — no socket needed, the socket in the footprint's 3D model is cosmetic only |
| — | IC socket | DIP-8 | 1 | — | `GS 8P` | for U1 |
| — | IC socket | DIP-14 | 1 | — | `GS 14P` | for U2 |
| — | Mounting hole | M2, 2.2 mm | 4 | MountingHole | — | Mechanical only, not a component |

¹ All pin headers are snapped off **one** 36-pin strip (18 pins needed).
² Not available at Reichelt in the matching format — the ESP32-DevKitC sold there has 38 pins and does **not** fit the 30-pin footprint. The board built and tested is the **ELEGOO ESP-WROOM-32** (30 pins) — Amazon: [link](https://www.amazon.de/dp/B0D8T5XD3P).

**Total: 4 resistors, 2 capacitors, 1 diode, 2 ICs (socketed), 6 pin headers, 1 ESP32, 4 mounting holes.**

---

## Components Part 2 (connected to the board via cables)

| Component | Qty | Source | Note |
|---|---|---|---|
| OLED display SSD1309, 2.42", 128×64, SPI, 7-pin connector | 1 | Amazon: [Hailege 2.42" SSD1309](https://www.amazon.de/dp/B0CJY4WP8C) ³ | Via 7-pin Dupont cable to J6 |
| DIN-5 panel-mount socket (flange with 2 screw holes) | 2 | Amazon: [Link](https://www.amazon.de/dp/B0D22R6Q9R) | MIDI-IN + MIDI-THRU, matching the enclosure cutout |
| Stomp footswitch, SPST, momentary (panel mount, with lock nut) | 3 | Amazon: [Link](https://www.amazon.de/dp/B09KN9HJ7M) | Custom, Grid, Reset |
| Dupont cable female-female, 2-pin | 4 | Reichelt `DEBO KABELSET19` ⁴ | J1 (MIDI-IN) + J3/J4/J5 (buttons) |
| Dupont cable female-female, 3-pin | 1 | Reichelt `DEBO KABELSET19` ⁴ | J2 (MIDI-THRU) |
| Dupont cable female-female, 7-pin | 1 | Reichelt `DEBO KABELSET19` ⁴ | J6 (display) |

³ The Hailege module (7-pin SPI) is the one built and tested. The 2.42" OLED at Reichelt (`DEBO OLED 2.42`), by contrast, has a 20-pin connector and does not plug directly into J6.
⁴ All Dupont connections come from **one** ribbon jumper cable `DEBO KABELSET19` (40 wires, female-female, 15 cm): peel off as many wires as each connection needs (18 wires in total). Each wire has its own single-pin housing — keep the wire order identical at both ends; bundle with tape if desired.

---

## Components Part 3 (Enclosure Fastening Hardware)

| Component | Value | Qty | Note |
|---|---|---|---|
| Threaded insert, Ruthex | M3 | 4 | Enclosure top |
| Socket head screw | M3×12 | 4 | Enclosure, into threaded inserts |
| Phillips screw | M1.7×4 | 4 | Display |
| Socket head screw | M2×10 | 8 | PCB (4) + MIDI sockets (4) |
| Nut | M2 | 4 | MIDI sockets |
| Washer | M2 | 4 | MIDI sockets |

---

## Reichelt Shopping List

**Ready-made cart:** [https://www.reichelt.de/my/2391791](https://www.reichelt.de/my/2391791) — all parts with the correct quantities,
add them to your own Reichelt cart in one click. Alternatively search the
order numbers individually, or use the list as CSV:
[`BarSync_Reichelt.csv`](BarSync_Reichelt.csv).

| Reichelt order no. | Description | Qty |
|---|---|---|
| `6N 139` | Optocoupler, DIP-8 | 1 |
| `SN 7406N TEX` | Hex inverter, open collector, DIL-14 | 1 |
| `1N 4148` | Switching diode, DO-35 | 1 |
| `METALL 220` | Resistor 220 Ω, metal film, 0207 | 3 |
| `METALL 4,70K` | Resistor 4.7 kΩ, metal film, 0207 | 1 |
| `X7R-5 100N` | Ceramic capacitor 100 nF, X7R, 5 mm pitch | 2 |
| `GS 8P` | IC socket, 8-pin | 1 |
| `GS 14P` | IC socket, 14-pin | 1 |
| `SL 1X36G 2,54` | Pin header strip, 36-pin, straight | 1 |
| `DEBO KABELSET19` | Ribbon jumper cable, 40 wires, female-female, 15 cm (wires separable) | 1 |

Not at Reichelt (source separately, electronics sources in Parts 1 and 2): ESP32 DevKit DOIT 30-pin, OLED
SSD1309 2.42" SPI (7-pin), 2× DIN-5 panel-mount socket, 3× stomp footswitch,
Ruthex M3 threaded inserts and M1.7/M2/M3 screws, nuts and washers.

---

## Sourcing Notes

- **Resistors:** metal film, 0207 size (0.6 W, 1 %) — fits the 1/4 W
  footprint; the extra power rating does no harm
- **Capacitors (C1/C2):** 100 nF, X7R (not Y5V/Z5U — more stable over
  temperature/voltage), THT, 5.0 mm pitch
- **Pin headers (J1–J6):** all 2.54 mm pitch, straight/vertical
- Order numbers checked on Sep 28, 2026 (all in stock); availability may change

---

*Created for BarSync hardware rev. 1.2 — matches `BarSync.kicad_pcb`/`.net` as of
Sep 17, 2026 (D1 antiparallel to U1). Changes vs. rev. 1.0: fixed R3 pull-up
(+5V → +3V3, protects ESP32 GPIO15), added C1/C2 as decoupling capacitors, tied
unused 7406 gate inputs to GND. See `CHANGELOG.en.md`. BOM grouped and Reichelt
order numbers added on Sep 28, 2026.*

---

*[Deutsche Version](BarSync_BOM.md)*
