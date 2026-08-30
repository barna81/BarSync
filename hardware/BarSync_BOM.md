# Bill of Materials — BarSync

*[English version](BarSync_BOM.en.md)*

Basierend auf der direkt aus `BarSync.kicad_pcb` und `BarSync.net` ausgewerteten,
tatsächlich aktuellen Bauteilliste (jede Referenz, jeder Wert, jeder Footprint
und jede Netzverbindung einzeln verifiziert).

---

## Bauteile Part 1 (passend für das von mir erstellte Platinenlayout)

| Ref. | Bauteil | Wert | Menge | Footprint | Hinweis |
|---|---|---|---|---|---|
| U1 | 6N139 | — | 1 | DIP-8, **Sockel** | Optokoppler, MIDI-IN |
| U2 | SN7406N | — | 1 | DIP-14, **Sockel** | Hex-Inverter, 2 Gates in Reihe für MIDI-Thru |
| D1 | 1N4148 | — | 1 | DO-35, liegend | MIDI-IN Vorwiderstand-Diode |
| R1 | Widerstand | 220Ω, 1/4W | 1 | Axial, liegend | MIDI-IN Vorwiderstand |
| R2 | Widerstand | 4,7kΩ, 1/4W | 1 | Axial, liegend | Vb-Ableitwiderstand (6N139) |
| R3 | Widerstand | 220Ω, 1/4W | 1 | Axial, liegend | Pull-up Optokoppler-Ausgang → **+3V3** (nicht +5V — geht direkt auf ESP32 GPIO15!) |
| R4 | Widerstand | 220Ω, 1/4W | 1 | Axial, liegend | MIDI-THRU Pin4-Stromschleife |
| C1 | Kondensator | 100nF, X7R | 1 | Radial, RM5 | Abblockkondensator U1 (Vcc/GND) |
| C2 | Kondensator | 100nF, X7R | 1 | Radial, RM5 | Abblockkondensator U2 (Vcc/GND) |
| J1 | Stiftleiste | 2-polig, 2,54mm | 1 | Vertikal | Board-Header → MIDI-IN-Buchse (Kabel) |
| J2 | Stiftleiste | 3-polig, 2,54mm | 1 | Vertikal | Board-Header → MIDI-THRU-Buchse (Kabel) |
| J3 | Stiftleiste | 2-polig, 2,54mm | 1 | Vertikal | Taktart-Taster |
| J4 | Stiftleiste | 2-polig, 2,54mm | 1 | Vertikal | Divisor-Taster |
| J5 | Stiftleiste | 2-polig, 2,54mm | 1 | Vertikal | Reset-Taster |
| J6 | Stiftleiste | 7-polig, 2,54mm | 1 | Vertikal | Board-Header → Display (Kabel) |
| A1 | ESP32 Buchsenleisten | 2× 15-polig, 2,54mm | 2 | DOIT_ESP32_DEVKIT_30Pins-Footprint | ESP32-Board wird gesteckt |
| — | Sockel für U1 | DIP-8 | 1 | — | Für 6N139 |
| — | Sockel für U2 | DIP-14 | 1 | — | Für SN7406N |
| — | Befestigungsloch | M2, 2,2mm | 4 | MountingHole | Nur mechanisch, kein Bauteil |

**Insgesamt 4 Widerstände (R1–R4), 2 Kondensatoren (C1–C2), 1 Diode, 2 ICs (mit Sockeln), 6 Stiftleisten (J1–J6), 4 Befestigungslöcher.**

---

## Bauteile Part 2 (werden mittels Kabel mit der Platine verbunden)

| Bauteil | Menge | Hinweis |
|---|---|---|
| ESP32 Dev Board (WROOM-32, DOIT-30-Pin-Layout) | 1 | Wird auf die Buchsenleisten (A1) gesteckt |
| OLED-Display SSD1309, 2,42", 128×64, SPI | 1 | Über 7-poliges Dupont-Kabel an J6 |
| DIN-5-Buchse (Einbau) | 2 | MIDI-IN + MIDI-THRU |
| Taster (Tact-Switch oder Fußtaster) | 3 | Taktart, Divisor, Reset |
| Dupont-Kabel Buchse-Buchse, 2-polig | 4 | J1 (MIDI-IN) + J3/J4/J5 (Taster) |
| Dupont-Kabel Buchse-Buchse, 3-polig | 1 | J2 (MIDI-THRU) |
| Dupont-Kabel Buchse-Buchse, 7-polig | 1 | J6 (Display) |

---

## Bestellhinweise

- **6N139 / SN7406N:** Standard-Logik-ICs, bei Reichelt/Mouser/Digikey als "SN7406N" bzw. "6N139" suchbar
- **DIN-5-Buchsen:**
- **Widerstände:** Alle 1/4W (0,25W), Standard-Kohleschicht
- **Kondensatoren (C1/C2):** 100nF, X7R (nicht Y5V/Z5U — stabiler über Temperatur/Spannung), bedrahtet, z. B. Reichelt "X7R-5 100N" (RM 5,0mm)
- **Stiftleisten (J1–J6):** Alle im 2,54mm-Raster, gerade/vertikal

---

*Erstellt für BarSync Hardware-Rev. 1.1 — Stand entspricht `BarSync.kicad_pcb`/`.net`
vom 25.08.2026. Änderungen gegenüber Rev. 1.0: R3-Pull-up korrigiert (+5V → +3V3,
schützt ESP32 GPIO15), C1/C2 als Abblockkondensatoren ergänzt, ungenutzte
7406-Gate-Eingänge auf GND gelegt. Siehe `CHANGELOG.md`.*

---

*[English version](BarSync_BOM.en.md)*
