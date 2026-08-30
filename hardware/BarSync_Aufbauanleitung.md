# Aufbauanleitung — BarSync

*[English version](BarSync_Aufbauanleitung.en.md)*

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

## 4. ESP32-Buchsenleisten

6. Die beiden 15-poligen Buchsenleisten (A1-Footprint) einlöten — **vorher das ESP32-Board probeweise auflegen**, um zu prüfen, dass der Pin-Abstand exakt passt, bevor final verlötet wird

## 5. ICs einsetzen

7. **6N139** in den DIP-8-Sockel (U1) — Kerbe beachten
8. **SN7406N** in den DIP-14-Sockel (U2) — Kerbe beachten

## 6. Kabel vorbereiten und anschließen

!!!WICHTIG: auf die korrekte Polarität/Pinbelegung gemäß [`Schaltplan`](kicad/BarSync/BarSync_schematic.pdf) achten!!!

9. Dupont-Kabel an die entsprechenden Header stecken:
   - J1 (2-pol.) → MIDI-IN-Buchse (Pin 4 + Pin 5 der DIN-5-Buchse)
   - J2 (3-pol.) → MIDI-THRU-Buchse (Pin 2 + Pin 4 + Pin 5 der DIN-5-Buchse)
   - J3/J4/J5 → jeweiliger Taster
   - J6 (7-pol.) → Display

## 7. ESP32-Board aufstecken

10. ESP32-Board vorsichtig auf die Buchsenleisten stecken — auf korrekte Ausrichtung achten (USB-Buchse zeigt zur vorgesehenen Gehäuseöffnung)

## 8. Firmware flashen

11. `barsync.ino` in der Arduino-IDE öffnen
12. Board: **ESP32 Dev Module** auswählen
13. Bibliotheken installieren (falls noch nicht geschehen): **"MIDI Library"** (FortySevenEffects), **"U8g2"** (olikraus)
14. Hochladen

## 9. Erster Funktionstest — vor dem Gehäuseeinbau

15. Display sollte bei Start den Boot-Screen zeigen ("BarSync")
16. MIDI-Quelle anschließen, Clock starten → Anzeige sollte auf "RUN" wechseln
17. Alle drei Taster einzeln durchtesten (Taktart wechseln, Divisor wechseln, Reset auslösen)
18. Erst wenn alles funktioniert: ins Gehäuse einbauen

---

## Bekannte Stolperfallen

| Problem | Lösung |
|---|---|
| Display bleibt dunkel | SPI-Verkabelung an J6 prüfen (Pin-Reihenfolge), Kontrast im Setup-Menü prüfen |
| Kein MIDI-Signal erkannt | Optokoppler-Orientierung (U1) und D1-Polarität prüfen |
| Taster reagieren nicht/falsch | Zuordnung prüfen: J3=Taktart(GPIO32), J4=Divisor(GPIO33), J5=Reset(GPIO25) |

---

*Für BarSync Hardware-Rev. 1.1 — Bauteilreferenzen verifiziert gegen `BarSync.kicad_pcb`/`.net`.
Siehe auch `pinplan.md` für die vollständige Pin-Referenz und `CHANGELOG.md` für die Änderungshistorie.*

---

*[English version](BarSync_Aufbauanleitung.en.md)*
