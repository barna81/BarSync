# Pinplan — BarSync (ESP32)

*[English version](pinplan.en.md)*

> **Hinweis:** Zur besseren Übersicht sind die PCB-Header-Bezeichner (J1–J6)
> hier bewusst weggelassen — dieser Pinplan fokussiert auf die Signal-Logik
> (welches Signal geht wohin). Die Zuordnung zu den Board-Headern findest du
> in [`hardware/BarSync_BOM.md`](hardware/BarSync_BOM.md) und
> [`hardware/BarSync_Aufbauanleitung.md`](hardware/BarSync_Aufbauanleitung.md).

## 1. Display — SSD1309 2,42" OLED (SPI, 128×64)

**Anbindung (für Bausatz/PCB-Version):** Auf der Hauptplatine sitzt eine
**7-polige Stiftleiste (2,54mm-Raster)**, die die relevanten ESP32-Pins
herausführt. Vom Display selbst geht ein **7-adriges Flachbandkabel
(Rainbow-Kabel) mit Dupont-Buchsen an beiden Enden** zur Stiftleiste auf
der Platine. Dadurch kann das Display mechanisch unabhängig von der
Hauptplatine im Gehäusefenster montiert werden (andere Position/Höhe),
ohne dass am Display selbst gelötet werden muss — beide Seiten sind
steckbar.

| Display-Pin        | ESP32 Pin | GPIO   | Kabelfarbe (Flachbandkabel) |
|---------------------|-----------|--------|------------|
| GND                 | GND       | –      |            |
| VCC                  | 3V3       | –      |            |
| SCK / D0 / CLK       | GPIO18    | 18     |            |
| SDA / D1 / MOSI      | GPIO23    | 23     |            |
| RES / RST            | GPIO4     | 4      |            |
| DC                   | GPIO21    | 21     |            |
| CS                   | GPIO5     | 5      |            |

**Stückliste für diese Verbindung:**
- 1× 7-polige Stiftleiste, gerade, 2,54mm-Raster (auf der Hauptplatine
  eingelötet)
- 1× 7-adriges Flachbandkabel mit Dupont-Buchse-Buchse-Anschlüssen
  (female-female), Länge je nach Gehäusetiefe (z. B. 10-15cm)
- Display selbst behält seine eigene, werkseitige Stiftleiste (male) —
  keine Lötarbeit am Display nötig

> Hinweis: Falls das Modul einen BS-Pin/Lötjumper für SPI/I2C-Umschaltung hat,
> muss dieser auf **SPI** stehen.

---

## 2. MIDI-IN (Optokoppler-Schaltung, 6N139)

| Signal                          | ESP32 Pin | GPIO |
|----------------------------------|-----------|------|
| MIDI-Daten (Optokoppler-Ausgang) | GPIO15 (RX2) | 15 |

**Beschaltung der Optokoppler-Stufe (6N139):**

```
MIDI-IN (DIN-5, Pin 4) ──[220Ω]──►│ (1N4148, Durchlassrichtung) ──► 6N139 Pin 2 (Anode)
MIDI-IN (DIN-5, Pin 5) ─────────────────────────────────────────► 6N139 Pin 3 (Kathode)
                                                                    │
                                                    6N139 Pin 8 (Vcc) ── +5V
                                                    6N139 Pin 7 (Vb)  ── über 4,7–10kΩ nach GND
                                                    6N139 Pin 6 (Ausgang) ── über 220Ω Pull-up nach +3V3
                                                                          └──► ESP32 GPIO15 (RX2)
                                                    6N139 Pin 5 (GND) ── GND (ESP32-Seite)
```

- DIN-5 Pin 2 = Schirm/nicht verbunden (je nach Buchse)
- Die 5V für den Optokoppler können separat oder vom ESP32-VIN kommen,
  je nach Stromversorgung — GND muss in jedem Fall gemeinsam sein.
- Der Basis-Widerstand an Pin 7 (Vb) ist wichtig für saubere, schnelle
  Flanken bei MIDI-Clock (24 PPQN) — ohne ihn können Clock-Ticks verloren
  gehen oder Stop-Nachrichten fehlerhaft interpretiert werden.

---

## 3. MIDI-THRU (gepufferte Weiterleitung, 7406/74LS05)

**Bauteile:**
- 1× 7406 oder 74LS05 (Hex-Inverter, Open-Kollektor-Ausgang)
- 1× DIN-5-Buchse (zusätzlich, für Thru)
- 1× 220Ω-Widerstand (Pull-up für die neue Thru-Stromschleife)
- 1× DIP-14-Sockel (empfohlen)

**Beschaltung:**

```
6N139 Pin 6 (Ausgang, = derselbe Knoten wie GPIO15) ──► 7406 Eingang (z.B. Pin 1)
                                                          │
                                                    7406 Ausgang (Pin 2, Open-Kollektor)
                                                          │
                                     ──────────────────────────────────► THRU-Buchse Pin 5
+5V ──[220Ω]──────────────────────────────────────────────────────────► THRU-Buchse Pin 4
                                                    THRU-Buchse Pin 2 ── nicht verbunden
```

- Nicht genutzte Gates des 7406 (5 von 6 bleiben frei) können offen
  bleiben oder auf GND gelegt werden (Datenblatt-Empfehlung beachten)

---

## 4. Taster

| Funktion         | ESP32 Pin | GPIO | Beschaltung                     |
|-------------------|-----------|------|----------------------------------|
| Grid umschalten | GPIO33  | 33   | Taster gegen GND, INPUT_PULLUP  |
| Custom-Taster (frei belegbar, aktuell: SET 1.1) | GPIO32  | 32   | Taster gegen GND, INPUT_PULLUP  |
| Reset (kurz/mittel) | GPIO25  | 25   | Taster gegen GND, INPUT_PULLUP  |

Alle drei Taster (Schließer): ein Pin an den genannten GPIO, der andere Pin an GND.
(Interne Pull-ups werden sind im Code aktiviert (`INPUT_PULLUP`), keine externen
Widerstände nötig.)

---

## 5. Übersicht aller belegten ESP32-Pins

| GPIO | Funktion            |
|------|----------------------|
| 4    | OLED RESET           |
| 5    | OLED CS              |
| 15   | MIDI IN (RX2)        |
| 17   | frei (nicht auf allen Boards herausgeführt) |
| 18   | OLED SCK (= VSPI SCK)|
| 19   | frei (Vorsicht: VSPI-Standard-MISO, nicht belegen!) |
| 21   | OLED DC              |
| 23   | OLED MOSI            |
| 25   | Taster Reset         |
| 32   | Custom-Taster (aktuell: SET 1.1) |
| 33   | Taster Grid          |
| 3V3  | Display VCC          |
| GND  | Display GND, Optokoppler GND (gemeinsame Masse!) |

> **INFO: MIDI-Thru belegt keinen ESP32-Pin** — der 7406-Puffer hängt direkt am
> Optokoppler-Ausgangsknoten, nicht am ESP32 selbst.

---

## Wichtige Hinweise

1. **Gemeinsame Masse:** ESP32-GND, Display-GND und die Ausgangsseite des
   Optokopplers müssen alle miteinander verbunden sein. Die MIDI-Eingangsseite
   (vom Sender kommend) bleibt durch den Optokoppler galvanisch getrennt.
2. **Kurze Kabel bei SPI** (SCK/MOSI), unter ca. 15–20 cm ist unproblematisch.
3. **GPIO15** wird hier exklusiv für MIDI-RX genutzt — nicht zusätzlich für
   andere Zwecke belegen.
4. **VSPI-Standardpins beachten:** Der ESP32-Hardware-SPI-Bus (VSPI) belegt
   standardmäßig SCK=18, MISO=19, MOSI=23, CS=5. GPIO19 (MISO) sollte NICHT
   für andere Signale (z. B. DC) verwendet werden, auch wenn das Display
   MISO nicht braucht — die SPI-Peripherie/Bibliothek konfiguriert den Pin
   trotzdem intern, was zu Störungen/Bildrauschen führen kann. Deshalb liegt
   DC hier auf GPIO21 statt GPIO19.

---

## Aktueller Stand: echtes KiCad-Projekt (autoritative Quelle)

Dieser Pinplan beschreibt weiterhin korrekt die grundsätzliche Pin-Logik,
aber die **verbindliche, geprüfte Quelle** für Schaltplan und Platine ist
jetzt das fertige KiCad-Projekt `BarSync` (Schaltplan + PCB,
vollständig geroutet, DRC-geprüft, Netzliste mehrfach verifiziert):

- `BarSync.kicad_pro` / `.kicad_sch` / `.kicad_pcb`
- `BarSync.net` (exportierte Netzliste)
- Gerber-Dateien in [`hardware/kicad/BarSync/gerbers/`](hardware/kicad/BarSync/gerbers/)
- [`Schaltplan`](hardware/kicad/BarSync/BarSync_schematic.pdf)

> Ein DRC-Report und ein gerendertes Platinenbild liegen aktuell nicht im Repo,
> lassen sich bei Bedarf aber jederzeit direkt aus KiCad exportieren
> (Werkzeuge → DRC-Prüfung durchführen bzw. 3D-Ansicht → Bild exportieren).

---

*[English version](pinplan.en.md)*
