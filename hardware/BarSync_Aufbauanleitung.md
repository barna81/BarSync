# Aufbauanleitung — BarSync

*[English version](BarSync_Aufbauanleitung_en.md)*

Empfohlene Reihenfolge für den Zusammenbau, vom Einfachsten zum Anspruchsvollsten.
Bauteilliste und Referenzen entsprechen der verifizierten `BarSync_BOM.md`.

---

## 1. Vorbereitung

- [`Schaltplan`](kicad/BarSync/BarSync_schematic.pdf) öffnen
- Alle Bauteile gegen die `BarSync_BOM.md`/`.csv` durchzählen, bevor du anfängst
- **Alle vier Widerstands-Positionen (R1–R4) werden bestückt** — keine Position bleibt leer

## 2. Kleinteile zuerst löten

Reihenfolge: erst die flachsten/robustesten Bauteile, dann die empfindlicheren.

1. **Widerstände R1, R2, R3, R4** (Polarität egal, keine Ausrichtung zu beachten)
2. **Kondensatoren C1, C2** (100nF, Keramik/X7R — ebenfalls keine Polarität, in beide Richtungen einlötbar)
3. **Diode D1** — Polarität beachten! Kathoden-Ring auf der Diode muss zur entsprechenden Markierung auf der Platine zeigen
4. **DIP-Sockel** für U1 (8-polig) und U2 (14-polig) — Kerbe/Punkt am Sockel zeigt Pin 1 an, mit der Markierung auf der Platine ausrichten

## 3. Stiftleisten löten

5. **J1** (2-polig, MIDI-IN), **J2** (3-polig, MIDI-THRU), **J3/J4/J5** (je 2-polig, Taster), **J6** (7-polig, Display)
- Alle vertikal, gerade einlöten — am besten erst einen Pin anlöten, Ausrichtung prüfen, dann erst die restlichen fixieren

## 4. ESP32 auf Platine verlöten

6. ESP32 auf dem vorgesehenen Platz einsetzen und verlöten
   > **Hinweis:** Der ESP32 wird — entgegen dem Erscheinungsbild des KiCad-Footprints (`DOIT_ESP32_DEVKIT_30Pins`) — **direkt und ohne Zwischenstecker/Sockelleiste** auf die Platine gelötet. Der Footprint suggeriert durch sein 3D-Modell einen Sockel, dieser ist aber rein kosmetisch. Nur die Peripherie-Header (J1–J6) erhalten Stiftleisten, nicht der ESP32 selbst.

## 5. ICs einsetzen

7. **6N139** in den DIP-8-Sockel (U1) — Kerbe beachten
8. **SN7406N** in den DIP-14-Sockel (U2) — Kerbe beachten

## 6. Einbau der Elektronik ins Gehäuse

Benötigtes Befestigungsmaterial (siehe `BarSync_BOM.md`/`.csv`):

- 4× Gewindeeinsatz Ruthex, M3
- 4× Inbusschraube M3×12
- 4× Inbusschraube M2×4
- 8× Inbusschraube M2×10
- 4× Mutter M2
- 12× Unterlegscheibe M2

9. **Platine einsetzen:** Platine im Gehäuseunterteil positionieren und mit den 4× M2×10-Inbusschrauben in der dafür vorgesehenen Aufnahme verschrauben
10. **Display:** Display mit den M2-Inbusschrauben 4× M2×4 befestigen (jeweils 2 Unterlegscheiben benutzen um das Gewinde der Schraube zu verkürzen)
11. **Midi-Buchsen:** Midibuchsen einsetzen (Anordnung IN und THRU beachten), mit den 4× M2×10-Inbusschrauben verschrauben und mit 4× M2-Muttern und 4× M2-Unterlegscheiben auf der Innenseite kontern
12. **Taster montieren:** Die 3× Taster (Custom, Grid, Reset) in die dafür vorgesehenen Öffnungen einsetzen und mit den Kontermuttern sichern
13. **Gewindeeinsätze setzen:** Die 4× Ruthex-Gewindeeinsätze (M3) in die entsprechenden Aufnahmen des Gehäuseoberteils einpressen/einschmelzen (die Temperatur des Lötkolbens etwa auf die Drucktemperatur des Materials - bei PLA z.B. 220°C)

## 7. Kabel vorbereiten und anschließen

!!!WICHTIG: auf die korrekte Polarität/Pinbelegung gemäß [`Schaltplan`](kicad/BarSync/BarSync_schematic.pdf) achten!!!

14. Dupont-Kabel an die entsprechenden Header stecken:
    - J1 (2-pol.) → MIDI-IN-Buchse (Pin 4 + Pin 5 der DIN-5-Buchse)
    - J2 (3-pol.) → MIDI-THRU-Buchse (Pin 2 + Pin 4 + Pin 5 der DIN-5-Buchse)
    - J3/J4/J5 → jeweiliger Taster
    - J6 (7-pol.) → Display

## 8. Firmware flashen

15. `barsync.ino` in der Arduino-IDE öffnen
16. Board: **ESP32 Dev Module** auswählen
17. Bibliotheken installieren (falls noch nicht geschehen): **"MIDI Library"** (FortySevenEffects), **"U8g2"** (olikraus)
18. Hochladen

## 9. Erster Funktionstest — vor dem schließen des Gehäuses

19. Display sollte bei Start den Boot-Screen zeigen ("BarSync")
20. MIDI-Quelle anschließen, Clock starten → Anzeige sollte auf "RUN" wechseln
21. Alle drei Taster einzeln durchtesten (Custom-Taster auslösen — aktuell: SET 1.1 —, Grid wechseln, Reset auslösen)
22. Erst wenn alles funktioniert: Gehäuse oberteil und Unterteil mit 4× M3×12-Inbusschrauben verschrauben

---

## Bekannte Stolperfallen

| Problem | Lösung |
|---|---|
| Display bleibt dunkel | SPI-Verkabelung an J6 prüfen (Pin-Reihenfolge), Kontrast im Setup-Menü prüfen |
| Kein MIDI-Signal erkannt | Optokoppler-Orientierung (U1) und D1-Polarität prüfen |
| Taster reagieren nicht/falsch | Zuordnung prüfen: J3=Custom(GPIO32), J4=Grid(GPIO33), J5=Reset(GPIO25) |

---

*Für BarSync Hardware-Rev. 1.2 — Bauteilreferenzen verifiziert gegen `BarSync.kicad_pcb`/`.net`.
Siehe auch `pinplan.md` für die vollständige Pin-Referenz und `CHANGELOG.md` für die Änderungshistorie.*

---

*[English version](BarSync_Aufbauanleitung_en.md)*
