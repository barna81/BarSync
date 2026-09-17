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
| GND                   | GND       | –      |            |
| VCC                   | 3V3       | –      |            |
| SCK / D0 / CLK        | GPIO18    | 18     |            |
| SDA / D1 / MOSI       | GPIO23    | 23     |            |
| RES / RST             | GPIO4     | 4      |            |
| DC                    | GPIO21    | 21     |            |
| CS                    | GPIO5     | 5      |            |

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

MIDI-IN (DIN-5) → 220Ω series resistor (R1) → input LED of U1 (6N139,
pin 2/3). D1 (1N4148) is wired **antiparallel** to the input LED and
protects it against reverse polarity/negative voltage spikes — it
doesn't conduct in normal operation. Output side: Vb (pin 7) via
4.7–10kΩ (R2) to GND, output (pin 6) via a 220Ω pull-up (R3) to +3V3 →
ESP32 GPIO15. Common ground is required on the output side (ESP32,
display, U1 pin 5) — the MIDI input side stays galvanically isolated by
the optocoupler.

Full wiring: [schematic](hardware/kicad/BarSync/BarSync_schematic.pdf),
parts: [`BarSync_BOM.en.md`](hardware/BarSync_BOM.en.md).

> The base resistor at pin 7 (Vb) is important for clean, fast edges with
> MIDI clock (24 PPQN) — without it, clock ticks can be lost or stop
> messages can be misinterpreted.

---

## 3. MIDI-THRU (Buffered Forwarding, 7406/74LS05)

U1 pin 6 (optocoupler output, = same node as GPIO15) feeds a gate input
of U2 (7406/74LS05, hex inverter with open-collector output); its output
drives the THRU socket (pin 5 signal, pin 4 pull-up via a 220Ω resistor
R4 to +5V, pin 2 not connected). Unused gates (5 of 6) are left open or
tied to GND (follow the datasheet recommendation). Uses no ESP32 pin.

Full wiring: [schematic](hardware/kicad/BarSync/BarSync_schematic.pdf).

---

## 4. Buttons

| Function            | ESP32 Pin | GPIO | Wiring                          |
|----------------------|-----------|------|----------------------------------|
| Toggle grid           | GPIO33    | 33   | Button to GND, INPUT_PULLUP     |
| Custom button (freely assignable, currently: SET 1.1) | GPIO32    | 32   | Button to GND, INPUT_PULLUP     |
| Reset (short/long)   | GPIO25    | 25   | Button to GND, INPUT_PULLUP     |

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
| 33   | Grid button           |
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
