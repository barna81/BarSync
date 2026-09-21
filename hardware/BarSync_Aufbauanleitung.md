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

Dieser Abschnitt ist bewusst ausführlich gehalten — auch für alle, die noch nie mit der Arduino-IDE gearbeitet haben.

15. **Arduino IDE installieren** (falls noch nicht vorhanden): aktuelle Version von https://www.arduino.cc/en/software herunterladen und installieren
16. **ESP32-Boardunterstützung hinzufügen**:
    - Arduino IDE öffnen
    - Datei → Voreinstellungen (Windows) bzw. Arduino IDE → Einstellungen (Mac)
    - Feld "Zusätzliche Boardverwalter-URLs" → eintragen: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
    - Mit OK bestätigen
    - Tools → Board → Boardverwalter (Boards Manager) → nach "esp32" suchen → Paket **"esp32 by Espressif Systems"** installieren (dauert ein paar Minuten, ist recht groß)
17. **Board auswählen**: Tools → Board → esp32 → **ESP32 Dev Module**
18. **Bibliotheken installieren** (falls noch nicht geschehen): Sketch → Bibliothek einbinden → Bibliotheken verwalten → **"MIDI Library"** (FortySevenEffects) und **"U8g2"** (olikraus) suchen und jeweils installieren
19. Board per USB-Kabel mit dem Rechner verbinden
20. **Falls unter Tools → Port kein Port auftaucht**: fehlt vermutlich der USB-Treiber für den Chip auf dem Board — siehe Kasten "USB-Treiber installieren" direkt unten
21. **Port auswählen**: Tools → Port → passenden Port wählen (Windows: z. B. `COM3`; Mac: z. B. `/dev/cu.usbserial-...` oder `/dev/cu.SLAB_USBtoUART`)
22. `barsync.ino` öffnen (liegt im Ordner `firmware/barsync/` im Repo)
23. **Hochladen**: Pfeil-Button oben links in der Arduino-IDE klicken. Bricht der Upload mit Timeout ab: Tools → Upload Speed auf `115200` stellen und erneut versuchen

> ### USB-Treiber installieren (nur nötig, falls kein Port erscheint)
>
> Die meisten ESP32-Boards verwenden einen **CP2102**-USB-Chip für die USB-Verbindung. Damit der Rechner das Board als seriellen Port erkennt, braucht es dafür einen Treiber — der ist nicht automatisch in Windows oder macOS enthalten.
>
> **Windows:**
> 1. https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers im Browser öffnen
> 2. Zu "Downloads" scrollen, die **"CP210x Windows Drivers"** (ZIP-Datei) herunterladen
> 3. ZIP-Datei entpacken (Rechtsklick → Alle extrahieren)
> 4. Im entpackten Ordner die enthaltene `.exe`-Installationsdatei ausführen, falls vorhanden. Ist keine dabei: Windows-Gerätemanager öffnen (Rechtsklick auf Start-Menü → Geräte-Manager) → unter "Andere Geräte" oder "Anschlüsse (COM & LPT)" das Board suchen (oft mit Warnsymbol) → rechtsklicken → "Treiber aktualisieren" → "Auf dem Computer nach Treibersoftware suchen" → den entpackten Ordner auswählen
> 5. Board kurz aus- und wieder einstecken (USB-Kabel abziehen und neu verbinden)
> 6. In der Arduino-IDE unter Tools → Port sollte jetzt ein COM-Port erscheinen (z. B. `COM3`)
>
> **Mac:**
> 1. https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers im Browser öffnen
> 2. Zu "Downloads" scrollen, die **"CP210x VCP Mac OSX Driver"** herunterladen (ZIP mit `.pkg`-Installer drin)
> 3. ZIP entpacken, die `.pkg`-Datei doppelklicken und den Installationsschritten folgen
> 4. macOS blockiert neu installierte Systemerweiterungen standardmäßig — meist erscheint direkt nach der Installation ein Hinweis "Systemerweiterung blockiert". Dann: Systemeinstellungen → Datenschutz & Sicherheit öffnen, ganz unten auf der Seite erscheint ein Hinweis auf die blockierte Erweiterung von "Silicon Laboratories" → auf "Zulassen" klicken
> 5. Mac neu starten (macOS verlangt das nach der Installation einer Kernel-Erweiterung)
> 6. Board per USB wieder anschließen
> 7. In der Arduino-IDE unter Tools → Port sollte jetzt ein Eintrag wie `/dev/cu.SLAB_USBtoUART` oder `/dev/cu.usbserial-...` erscheinen
>
> Falls dein Board stattdessen einen **CH340/CH341**-Chip hat (steht meist direkt auf dem kleinen IC neben dem USB-Anschluss aufgedruckt), brauchst du statt des CP210x-Treibers den passenden **WCH-CH340-Treiber** — gleiches Prinzip, andere Downloadquelle (Hersteller: WCH).

## 9. Erster Funktionstest — vor dem schließen des Gehäuses

24. Display sollte bei Start den Boot-Screen zeigen ("BarSync")
25. MIDI-Quelle anschließen, Clock starten → Anzeige sollte auf "RUN" wechseln
26. Alle drei Taster einzeln durchtesten (Custom-Taster auslösen — aktuell: SET 1.1 —, Grid wechseln, Reset auslösen)
27. Erst wenn alles funktioniert: Gehäuse oberteil und Unterteil mit 4× M3×12-Inbusschrauben verschrauben

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
