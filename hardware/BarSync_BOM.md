# Bill of Materials — BarSync

*[English version](BarSync_BOM.en.md)*

Basierend auf der direkt aus `BarSync.kicad_pcb` und `BarSync.net` ausgewerteten,
tatsächlich aktuellen Bauteilliste (jede Referenz, jeder Wert, jeder Footprint
und jede Netzverbindung einzeln verifiziert). Bauteile mit identischen
Eigenschaften sind in einer Zeile zusammengefasst; die Spalte **Reichelt**
nennt die passende Reichelt-Artikelnummer (Suchbegriff im Shop). Eine
fertige Einkaufsliste steht am Ende dieser Datei.

---

## Bauteile Part 1 (auf der Platine)

| Ref. | Bauteil | Wert | Menge | Footprint | Reichelt | Hinweis |
|---|---|---|---|---|---|---|
| U1 | 6N139 | Optokoppler | 1 | DIP-8, **gesockelt** | `6N 139` | MIDI-IN |
| U2 | SN7406N | Hex-Inverter, Open-Kollektor | 1 | DIP-14, **gesockelt** | `SN 7406N TEX` | MIDI-THRU-Treiber, 2 Gatter in Reihe |
| D1 | 1N4148 | Schaltdiode | 1 | DO-35, liegend | `1N 4148` | MIDI-IN-Schutzdiode, antiparallel zu U1 — **Polarität beachten** |
| R1, R3, R4 | Widerstand | 220 Ω | 3 | Axial 0207, liegend | `METALL 220` | R1 = MIDI-IN-Vorwiderstand · R3 = Pull-up Opto-Ausgang → **+3V3** (nicht +5V — geht direkt auf ESP32 GPIO15!) · R4 = MIDI-THRU-Stromschleife |
| R2 | Widerstand | 4,7 kΩ | 1 | Axial 0207, liegend | `METALL 4,70K` | Vb-Ableitwiderstand (6N139) |
| C1, C2 | Kondensator | 100 nF, X7R | 2 | Radial, RM 5 | `X7R-5 100N` | Abblockkondensatoren U1 / U2 |
| J1, J3, J4, J5 | Stiftleiste | 2-polig, 2,54 mm | 4 | Vertikal | `SL 1X36G 2,54` ¹ | J1 = MIDI-IN · J3 = Custom · J4 = Grid · J5 = Reset |
| J2 | Stiftleiste | 3-polig, 2,54 mm | 1 | Vertikal | `SL 1X36G 2,54` ¹ | MIDI-THRU |
| J6 | Stiftleiste | 7-polig, 2,54 mm | 1 | Vertikal | `SL 1X36G 2,54` ¹ | Display |
| A1 | ESP32 DevKit (WROOM-32, DOIT, 30 Pins) | — | 1 | DOIT_ESP32_DEVKIT_30Pins | — ² | Wird direkt eingelötet — kein Sockel nötig, der Sockel im 3D-Modell des Footprints ist rein kosmetisch |
| — | IC-Sockel | DIP-8 | 1 | — | `GS 8P` | für U1 |
| — | IC-Sockel | DIP-14 | 1 | — | `GS 14P` | für U2 |
| — | Befestigungsloch | M2, 2,2 mm | 4 | MountingHole | — | Nur mechanisch, kein Bauteil |

¹ Alle Stiftleisten werden von **einer** 36-poligen Leiste abgebrochen (benötigt: 18 Pins).
² Bei Reichelt nicht im passenden Format erhältlich — das dort geführte ESP32-DevKitC hat 38 Pins und passt **nicht** auf den 30-Pin-Footprint. Verbaut und getestet ist das **ELEGOO ESP-WROOM-32** (30 Pins) — Amazon: [Link](https://www.amazon.de/dp/B0D8T5XD3P).

**Insgesamt 4 Widerstände, 2 Kondensatoren, 1 Diode, 2 ICs (gesockelt), 6 Stiftleisten, 1 ESP32, 4 Befestigungslöcher.**

---

## Bauteile Part 2 (per Kabel mit der Platine verbunden)

| Bauteil | Menge | Bezugsquelle | Hinweis |
|---|---|---|---|
| OLED-Display SSD1309, 2,42", 128×64, SPI, 7-poliger Anschluss | 1 | Amazon: [Hailege 2,42" SSD1309](https://www.amazon.de/dp/B0CJY4WP8C) ³ | Über 7-poliges Dupont-Kabel an J6 |
| DIN-5-Buchse (Einbau, Flansch mit 2 Schraublöchern) | 2 | Amazon: [Link](https://www.amazon.de/dp/B0D22R6Q9R) | MIDI-IN + MIDI-THRU, passend zum Gehäuseausschnitt |
| Stomp-Fußschalter, SPST, tastend (Einbau, mit Kontermutter) | 3 | Amazon: [Link](https://www.amazon.de/dp/B09KN9HJ7M) | Custom, Grid, Reset |
| Dupont-Kabel Buchse-Buchse, 2-polig | 4 | Reichelt `DEBO KABELSET19` ⁴ | J1 (MIDI-IN) + J3/J4/J5 (Taster) |
| Dupont-Kabel Buchse-Buchse, 3-polig | 1 | Reichelt `DEBO KABELSET19` ⁴ | J2 (MIDI-THRU) |
| Dupont-Kabel Buchse-Buchse, 7-polig | 1 | Reichelt `DEBO KABELSET19` ⁴ | J6 (Display) |

³ Verbaut und getestet ist das Hailege-Modul (7-polig SPI). Das 2,42"-OLED bei Reichelt (`DEBO OLED 2.42`) hat dagegen einen 20-poligen Anschluss und passt nicht direkt an J6.
⁴ Alle Dupont-Verbindungen aus **einem** Flachband-Steckbrückenkabel `DEBO KABELSET19` (40 Adern, Buchse-Buchse, 15 cm): Adern je nach Polzahl abtrennen (18 Adern benötigt). Jede Ader hat einen einpoligen Stecker — Adern an beiden Enden in derselben Reihenfolge aufstecken; bei Bedarf mit Tape bündeln.

---

## Bauteile Part 3 (Gehäuse-Befestigungsmaterial)

| Bauteil | Wert | Menge | Hinweis |
|---|---|---|---|
| Gewindeeinsatz Ruthex | M3 | 4 | Gehäuseoberteil |
| Inbusschraube | M3×12 | 4 | Gehäuse, in die Gewindeeinsätze |
| Kreuzschlitzschraube | M1,7×4 | 4 | Display |
| Inbusschraube | M2×10 | 8 | Platine (4) + MIDI-Buchsen (4) |
| Mutter | M2 | 4 | MIDI-Buchsen |
| Unterlegscheibe | M2 | 4 | MIDI-Buchsen |

---

## Einkaufsliste Reichelt

**Fertiger Warenkorb:** [https://www.reichelt.de/my/2391791](https://www.reichelt.de/my/2391791) — alle Positionen mit den richtigen
Mengen, per Klick in den eigenen Reichelt-Warenkorb übernehmen. Alternativ
die Artikelnummern einzeln im Shop suchen, oder die Liste als CSV:
[`BarSync_Reichelt.csv`](BarSync_Reichelt.csv).

| Reichelt-Artikelnr. | Bezeichnung | Menge |
|---|---|---|
| `6N 139` | Optokoppler, DIP-8 | 1 |
| `SN 7406N TEX` | Hex-Inverter Open-Kollektor, DIL-14 | 1 |
| `1N 4148` | Schaltdiode, DO-35 | 1 |
| `METALL 220` | Widerstand 220 Ω, Metallschicht, 0207 | 3 |
| `METALL 4,70K` | Widerstand 4,7 kΩ, Metallschicht, 0207 | 1 |
| `X7R-5 100N` | Keramikkondensator 100 nF, X7R, RM 5 | 2 |
| `GS 8P` | IC-Sockel, 8-polig | 1 |
| `GS 14P` | IC-Sockel, 14-polig | 1 |
| `SL 1X36G 2,54` | Stiftleiste 36-polig, gerade | 1 |
| `DEBO KABELSET19` | Steckbrückenkabel 40 Adern, Buchse-Buchse, 15 cm (Adern abtrennbar) | 1 |

Nicht bei Reichelt (separat besorgen, Bezugsquellen für Elektronik siehe Part 1 und 2): ESP32 DevKit DOIT 30-Pin, OLED
SSD1309 2,42" SPI (7-polig), 2× DIN-5-Einbaubuchse, 3× Stomp-Fußschalter,
Ruthex-Gewindeeinsätze M3 sowie Schrauben/Muttern/Scheiben M1,7/M2/M3.

---

## Bestellhinweise

- **Widerstände:** Metallschicht, Bauform 0207 (0,6 W, 1 %) — passt in den
  1/4-W-Footprint; mehr Belastbarkeit schadet nicht
- **Kondensatoren (C1/C2):** 100 nF, X7R (nicht Y5V/Z5U — stabiler über
  Temperatur/Spannung), bedrahtet, RM 5,0 mm
- **Stiftleisten (J1–J6):** alle im 2,54-mm-Raster, gerade/vertikal
- Artikelnummern geprüft am 28.09.2026 (alle ab Lager); Verfügbarkeit kann sich ändern

---

*Erstellt für BarSync Hardware-Rev. 1.2 — Stand entspricht `BarSync.kicad_pcb`/`.net`
vom 17.09.2026 (D1 antiparallel zu U1). Änderungen gegenüber Rev. 1.0: R3-Pull-up
korrigiert (+5V → +3V3, schützt ESP32 GPIO15), C1/C2 als Abblockkondensatoren
ergänzt, ungenutzte 7406-Gate-Eingänge auf GND gelegt. Siehe `CHANGELOG.md`.
BOM am 28.09.2026 zusammengefasst und um Reichelt-Artikelnummern ergänzt.*

---

*[English version](BarSync_BOM.en.md)*
