# Pin Plan — BarSync (ESP32)

*[Deutsche Version](pinplan.md)*

> **Note:** For readability, the PCB header designators (J1–J6) are
> intentionally left out here — this pin plan focuses on the signal logic
> (which signal goes where). For the mapping to the board headers, see
> [`hardware/BarSync_BOM.en.md`](hardware/BarSync_BOM.en.md) and
> [`hardware/BarSync_Aufbauanleitung.en.md`](hardware/BarSync_Aufbauanleitung.en.md).

## 1. Display — SSD1309 2.42" OLED (SPI, 128×64)

**Connection (for kit/PCB version):** The main board carries a
**7-pin header (2.54 mm pitch)** that breaks out the relevant ESP32 pins.
From the display itself, a **7-wire ribbon cable (rainbow cable) with
Dupont sockets on both ends** runs to the header on the board. This lets
the display be mounted mechanically independent of the main board in the
enclosure window (at a different position/height), without any soldering
on the display itself — both ends simply plug in.

| Display Pin          | ESP32 Pin | GPIO   | Cable Color (ribbon cable) |
|-----------------------|-----------|--------|------------|
| GND                   | GND       | –      | Blue       |
| VCC                   | 3V3       | –      | Orange     |
| SCK / D0 / CLK        | GPIO18    | 18     | Purple     |
| SDA / D1 / MOSI       | GPIO23    | 23     | Gray       |
| RES / RST             | GPIO4     | 4      | Green      |
| DC                    | GPIO21    | 21     | Brown      |
| CS                    | GPIO5     | 5      | Yellow     |

**Parts list for this connection:**
- 1× 7-pin header, straight, 2.54 mm pitch (soldered onto the main board)
- 1× 7-wire ribbon cable with Dupont female-female connectors,
  length depending on enclosure depth (e.g. 10-15 cm)
- The display keeps its own factory header (male) — no soldering
  needed on the display itself

> Note: If the module has a BS pin/solder jumper for SPI/I2C switching,
> it must be set to **SPI**.

---

## 2. MIDI-IN (Optocoupler Circuit, 6N139)

| Signal                          | ESP32 Pin | GPIO |
|----------------------------------|-----------|------|
| MIDI data (optocoupler output)  | GPIO15 (RX2) | 15 |

**Wiring of the optocoupler stage (6N139):**

```
MIDI-IN (DIN-5, Pin 4) ──[220Ω]──►│ (1N4148, forward direction) ──► 6N139 Pin 2 (Anode)
MIDI-IN (DIN-5, Pin 5) ─────────────────────────────────────────► 6N139 Pin 3 (Cathode)
                                                                    │
                                                    6N139 Pin 8 (Vcc) ── +5V
                                                    6N139 Pin 7 (Vb)  ── via 4.7–10kΩ to GND
                                                    6N139 Pin 6 (Output) ── via 220Ω pull-up to +3V3
                                                                          └──► ESP32 GPIO15 (RX2)
                                                    6N139 Pin 5 (GND) ── GND (ESP32 side)
```

- DIN-5 pin 2 = shield/not connected (depending on the socket)
- The 5V supply for the optocoupler can come separately or from the
  ESP32's VIN, depending on your power setup — GND must be common in
  either case.
- The base resistor at pin 7 (Vb) is important for clean, fast edges
  with MIDI clock (24 PPQN) — without it, clock ticks can be lost or
  stop messages can be misinterpreted.

---

## 3. MIDI-THRU (Buffered Forwarding, 7406/74LS05)

**Components:**
- 1× 7406 or 74LS05 (hex inverter, open-collector output)
- 1× DIN-5 socket (additional, for Thru)
- 1× 220Ω resistor (pull-up for the new Thru current loop)
- 1× DIP-14 socket (recommended)

**Wiring:**

```
6N139 Pin 6 (Output, = same node as GPIO15) ──► 7406 Input (e.g. Pin 1)
                                                          │
                                                    7406 Output (Pin 2, Open Collector)
                                                          │
                                     ──────────────────────────────────► THRU Socket Pin 5
+5V ──[220Ω]──────────────────────────────────────────────────────────► THRU Socket Pin 4
                                                    THRU Socket Pin 2 ── not connected
```

- Unused gates of the 7406 (5 of 6 remain free) can be left open or
  tied to GND (follow the datasheet recommendation)

---

## 4. Buttons

| Function            | ESP32 Pin | GPIO | Wiring                          |
|----------------------|-----------|------|----------------------------------|
| Toggle divisor       | GPIO33    | 33   | Button to GND, INPUT_PULLUP     |
| Custom button (freely assignable, currently: SET 1.1)| GPIO32    | 32   | Button to GND, INPUT_PULLUP     |
| Reset (short/medium) | GPIO25    | 25   | Button to GND, INPUT_PULLUP     |

All three buttons (normally-open): one pin to the listed GPIO, the other
pin to GND. (Internal pull-ups are enabled in the code (`INPUT_PULLUP`),
no external resistors needed.)

---

## 5. Overview of All Used ESP32 Pins

| GPIO | Function             |
|------|-----------------------|
| 4    | OLED RESET            |
| 5    | OLED CS               |
| 15   | MIDI IN (RX2)         |
| 17   | free (not broken out on all boards) |
| 18   | OLED SCK (= VSPI SCK) |
| 19   | free (caution: VSPI default MISO, do not use!) |
| 21   | OLED DC               |
| 23   | OLED MOSI             |
| 25   | Reset button          |
| 32   | Custom button (currently: SET 1.1) |
| 33   | Divisor button        |
| 3V3  | Display VCC           |
| GND  | Display GND, optocoupler GND (common ground!) |

> **INFO: MIDI-Thru does not use any ESP32 pin** — the 7406 buffer is
> connected directly to the optocoupler output node, not to the ESP32
> itself.

---

## Important Notes

1. **Common ground:** ESP32 GND, display GND, and the output side of the
   optocoupler must all be connected together. The MIDI input side (coming
   from the sender) remains galvanically isolated by the optocoupler.
2. **Keep SPI cables short** (SCK/MOSI); under about 15–20 cm is fine.
3. **GPIO15** is used exclusively for MIDI RX here — don't reuse it for
   any other purpose.
4. **Watch the VSPI default pins:** The ESP32's hardware SPI bus (VSPI)
   defaults to SCK=18, MISO=19, MOSI=23, CS=5. GPIO19 (MISO) should NOT be
   used for other signals (e.g. DC), even though the display doesn't need
   MISO — the SPI peripheral/library still configures the pin internally,
   which can cause interference/screen noise. That's why DC is on GPIO21
   here instead of GPIO19.

---

## Current Status: Real KiCad Project (Authoritative Source)

This pin plan still correctly describes the underlying pin logic, but
the **authoritative, verified source** for the schematic and PCB is now
the finished KiCad project `BarSync` (schematic + PCB, fully routed,
DRC-checked, netlist verified multiple times):

- `BarSync.kicad_pro` / `.kicad_sch` / `.kicad_pcb`
- `BarSync.net` (exported netlist)
- Gerber files in [`hardware/kicad/BarSync/gerbers/`](hardware/kicad/BarSync/gerbers/)
- [`Schematic`](hardware/kicad/BarSync/BarSync_schematic.pdf)

> A DRC report and a rendered PCB image aren't currently included in the repo,
> but can be exported directly from KiCad any time (Tools → Run DRC, or
> 3D viewer → export image).

---

*[Deutsche Version](pinplan.md)*
