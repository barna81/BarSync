# Changelog — BarSync

*[English version](CHANGELOG.en.md)*

## Gehäuse/Dokumentation (05.09.2026)

**Taster-Bezeichnung auf dem Gehäuse von "Divisor" zu "Grid" geändert**
— reine Umbenennung des Aufdrucks, keine Funktionsänderung. Alle
Dokumente (README, Pinplan, Aufbauanleitung, BOM, Quickstart) verwenden
jetzt durchgängig "Grid" statt "Divisor" als sichtbare Bezeichnung für
diesen Taster und die dazugehörige Fortschrittsanzeige. Firmware-interne
Bezeichner (`divisor`, `divisorIndex`, `PIN_BTN_DIVISOR` etc. in
`barsync.ino`) bleiben unverändert, ebenso die Bezeichnung "Divisor
Switch" im KiCad-Schaltplan/Netzliste — falls gewünscht, muss dies
separat direkt im KiCad-Projekt nachgezogen werden.

## Firmware v1.2.0 (02.09.2026)

**Bugfix: BarSync blieb nach dem Aufwachen aus dem Standby eingefroren
auf dem Stand vor dem Einschlafen, obwohl die MIDI-Clock bereits lief —
erst ein manuelles Stop+Run am Sequencer brachte ihn wieder zum
Laufen.** Ursache: `isRunning` wird bei uns ausschließlich durch eine
explizite Start/Continue-Nachricht gesetzt. Lief der Sequencer beim
Aufwecken bereits durch (kein neuer Start/Continue, da aus seiner Sicht
nie gestoppt wurde), kamen zwar Clock-Ticks an, aber die allein setzen
`isRunning` nicht — BarSync wartete auf eine Nachricht, die nie kommen
würde. Fix: Wurde der Aufwach-Grund nicht durch einen Taster ausgelöst
(also vermutlich MIDI-Aktivität), werden jetzt alle temporären
Grid-/Zeit-Daten gelöscht und direkt bei 1.1 neu gestartet — unabhängig
davon, ob danach noch eine explizite Start-Nachricht durchkommt. Kommt
doch noch eine, setzt sie einfach harmlos erneut zurück; kommt gar kein
echter Clock hinterher, fängt die bestehende
Clock-Verlust-Erkennung (`CLOCK_LOST_TIMEOUT_MS`) das nach wenigen
Sekunden von selbst wieder ab.

**Bugfix: kurze Tastendrücke wurden manchmal komplett verschluckt.**
Der alte Debounce-Mechanismus verlangte, dass ein neuer Pegel danach
mindestens 50ms durchgehend stabil blieb, bevor er überhaupt als
Wechsel galt — ein Druck+Loslassen, das komplett innerhalb dieser 50ms
passierte, wurde dadurch nie als "stabil" erkannt und verschwand
spurlos, statt nur verzögert zu werden. Umgestellt auf einen
flankenbasierten Debounce: die Flanke wird sofort akzeptiert, danach
folgt ein 50ms-Sperrfenster gegen Kontaktprellen. Betrifft alle drei
Taster gleichermaßen, da sie durch dieselbe `updateButton()`-Funktion
laufen.

**SET 1.1 (QUANTIZED-Modus) rundet jetzt auf den nächstgelegenen statt
immer auf den zuletzt vergangenen Beat.** Liegt der Tastendruck näher
am kommenden Beat als am aktuellen, wird kurz gewartet (maximal 12
Ticks, also höchstens ein halber Beat) und exakt dort committet, statt
immer rückwärts auf den letzten bereits vergangenen Beat zu runden.

**Menü-Feinschliff: Custom-Rolle und Reset-Einstellungen neu geordnet.**

- `Switches > Custom > FUNCTION` hat jetzt nur noch zwei Rollen:
  `TIMESIG` und `SET 1.1` (**neuer Default**) — `RESET 1`/`RESET 2`
  entfallen als Custom-Taster-Rolle ersatzlos (der physische
  Reset-Taster behält seine beiden Stufen unverändert).
- Ist `FUNCTION = TIMESIG` eingestellt, erscheint darunter neu
  **`TIMESIGS >`** — eine Checkbox-Liste, welche der fünf Taktarten der
  Custom-Taster beim Durchrotieren berücksichtigt (Default: alle). Der
  feste Taktart-Wert auf Seite 1 (`SETUP > TIMESIG`) bleibt davon
  unberührt und rotiert weiterhin durch alle fünf.
- Ist `FUNCTION = SET 1.1` eingestellt, erscheint darunter neu direkt
  **`MODE`** (QUANTIZED/INSTANT) — die Einstellung, die vorher unter
  `Switches > Reset` lag, ist jetzt hierher verschoben, da sie
  ausschließlich SET 1.1 betrifft.
- `Switches > Custom Switch` (vormals "Custom Role") und
  `Switches > Reset Switch` (vormals "Reset") umbenannt.
- `Switches > Reset Switch` hat jetzt **`RESET 1`** und **`RESET 2`**
  (beide Ja/Nein, Default Ja) — pro Stufe einzeln einstellbar, ob diese
  Stufe zusätzlich die Spielzeit zurücksetzt. Der Bar-Zähler/das Grid
  wird dabei immer zurückgesetzt (kein eigener Schalter mehr dafür —
  die zwischenzeitlich eingeführte `BARCOUNTER`-Option entfällt wieder).
  SET 1.1 bleibt davon unberührt und setzt immer alles zurück.
- **Menü optisch vereinheitlicht:** Alle Einstellungsseiten nutzen jetzt
  dasselbe Raster (Punktabstand, Position der Erklärung, Schriftgröße
  der Erklärungstexte) — vorher unterschied sich das von Seite zu Seite
  (z. B. 8px hier, 12px dort). Die Erklärungstexte laufen jetzt
  außerdem einheitlich in der kleineren Schrift.

**Bugfix: BarSync reagierte nach dem Standby (Light Sleep) erst auf den
zweiten RUN/Start-Befehl des Sequencers — und der ursprüngliche Fix
dafür hat einen noch schlimmeren Nachfolgebug verursacht.**

Ursache des ursprünglichen Bugs: Der GPIO-Level-Wakeup auf der MIDI-
RX-Leitung garantiert nur, dass die CPU aufwacht — nicht, dass die
UART-Peripherie genau das Byte, das den Wakeup auslöst, sauber
empfängt (die Taktversorgung braucht nach dem Light-Sleep einen kurzen
Moment zum Stabilisieren).

Der ursprüngliche Fix (UART-Puffer nach dem Aufwachen leeren +
`MIDI.begin()` erneut aufrufen) hat das Problem aber nicht behoben,
sondern durch ein schlimmeres ersetzt: Lag beim Aufwachen bereits eine
laufende MIDI-Clock an (Sequencer läuft schon), wurden durch das Leeren
des Puffers auch das echte Start-Kommando und bereits angekommene
Clock-Ticks verworfen — `isRunning` blieb false, Zähler/Zeit/Grid
blieben komplett eingefroren auf dem Stand vor dem Einschlafen, bis ein
frisches, damit unabhängiges Stop+Start-Paar durchkam. `MIDI.begin()`
birgt dasselbe Risiko indirekt, da die Bibliothek dabei intern erneut
den Transport initialisiert, was die zugrunde liegende UART ebenfalls
zurücksetzen und den Puffer leeren kann.

**Endgültiger Fix:** Beides (Puffer-Leeren und `MIDI.begin()`-Aufruf)
wieder entfernt. Es bleibt nur eine kurze Stabilisierungspause nach dem
Aufwachen — die reicht aus, da MIDI-Echtzeitnachrichten (Clock/Start/
Stop/Continue) einzelne Bytes sind, die anders als Kanalnachrichten
keinen "Running Status" erwarten und daher den Parser nicht aus dem
Tritt bringen können, selbst wenn ausgerechnet dieses eine Byte mal
korrupt ankommen sollte. Zusätzlich wird weiterhin der
Beat-Extrapolations-Anker (`lastTickAnchorMicros`) beim Aufwachen
zurückgesetzt, damit kein veralteter Zeitstempel von vor dem
Schlafengehen kurzzeitig zu einer falschen Anzeige führt.

**Reset-Logik überarbeitet**, angelehnt an Konzepte der Eurorack-Version,
aber mit eigenständigem Ergebnis nach mehreren Iterationen:

- Nur noch **zwei Reset-Stufen** am physischen Taster (Reset 1/Reset 2)
  statt drei — Stufe 3 entfällt. Kurzer Druck = Reset 1, Halten über 1s
  = Reset 2 (Drückmechanismus unverändert).
- **Reset 1** wartet wie eh und je auf das **Ende des aktuellen Takts**,
  **Reset 2** auf das **Ende des Divisor-Zyklus** — beide arbeiten
  innerhalb des gerade aktiven Beatmusters, ohne es selbst zu verändern.
- **Neu: `SET 1.1`** — eine eigenständige, sofort wirkende Funktion, nur
  über `Switches > Custom > FUNCTION` auf den Custom-Taster legbar
  (nicht über den physischen Reset-Taster erreichbar). Setzt sofort
  einen neuen "1.1"-Anker und spannt damit ein komplett neues Beatmuster
  auf, innerhalb dessen Reset 1 und 2 danach weiterarbeiten. Über
  `Switches > Reset > MODE` einstellbar:
  - **QUANTIZED** (Default): rundet den neuen Anker auf den
    nächstgelegenen, bereits im aktuellen Grid existierenden Beat ab —
    das bestehende Beatmuster (seit MIDI-Start oder einem vorherigen
    SET 1.1) bleibt phasengleich erhalten, nur unsere eigene Zählung
    wird auf den Sequencer nachjustiert. Beispiel: Anzeige steht bei
    Takt 1, Beat 2, Tick 1 — SET 1.1 zieht die neue 1.1 exakt dorthin,
    wo eben noch die 1.2 war.
  - **INSTANT**: verwendet den exakten rohen Tick des Auslösens und
    spannt damit ein komplett neues, vom bisherigen Grid unabhängiges
    Beatmuster auf.
- Neue Einstellung `Switches > Reset > PLAYTIME` (Ja/Nein, Default Ja):
  ob ein Reset (1, 2, oder SET 1.1) zusätzlich auch die Spielzeit
  zurücksetzt — unabhängig davon, welche der drei Funktionen es ist.
- **Blitzfeedback wie beim Eurorack übernommen:** Nach jeder Reset-
  Aktion blinkt "RESET 1"/"RESET 2"/"SET 1.1" jetzt exakt 2x im
  Beat-Takt (statt zeitbasiert), zusätzlich zur kurzen
  Sofort-Blitz-Animation.

**Custom-Taster ist jetzt frei belegbar** (`Switches > Custom`), statt
fest die Taktart zu zyklen:

- Neue Einstellung `Switches > Custom > FUNCTION` mit vier möglichen
  Rollen: `TIMESIG` (Default, bisheriges Verhalten), `RESET 1`,
  `RESET 2`, `SET 1.1`. Die beiden Reset-Rollen lösen direkt die
  jeweils gleichnamige, bereits vorhandene Reset-Stufe aus — mit einem
  einzelnen kurzen Tastendruck, ohne die sonst nötige unterschiedlich
  lange Haltezeit am Reset-Taster selbst; sie warten wie der physische
  Taster ganz normal auf Takt- bzw. Zyklusende. `SET 1.1` wirkt dagegen
  immer sofort (siehe oben).
- Die Taktart ist dafür aus dem Taster-Zyklus herausgelöst und liegt
  jetzt **fest als eigener Menüpunkt auf Seite 1** (`SETUP > TIMESIG`),
  direkt zwischen den fünf vorhandenen Taktarten umschaltbar (Default:
  4/4). Der bisherige Auswahl-Screen zum Ein-/Ausblenden einzelner
  Taktarten für die Taster-Zyklisierung (`Switches > Timesig`) entfällt
  damit ersatzlos — er ergab ohne festen Taktart-Taster keinen Sinn
  mehr; alle fünf Taktarten sind jetzt immer erreichbar.
- 1s-Halten des Custom-Tasters (MIDI-Analyzer ein/aus), die
  Nudge-Kombination (Custom+Divisor) sowie das MidiWar-Easter-Egg
  (Custom+Reset) bleiben unverändert und funktionieren unabhängig von
  der gewählten Rolle.

**Einstellungsmenü grundlegend überarbeitet**, übernommen aus dem
mehrstufigen Menü-Redesign der Eurorack-Firmware (dort Stand
30.08.2026) — angepasst auf die Desktop-Version:

- Menü jetzt zweistufig (`SETUP > SWITCHES/DISPLAY/STANDBY/DEFAULTS >
  Detailseite`) statt einer flachen 6-Punkte-Liste. DIVISOR und das
  neue RESET liegen unter `SWITCHES` (dort jetzt neben `CUSTOM`),
  CONTRAST und INVERT unter `DISPLAY`.
- Kurze Sinn-Erklärung am unteren Bildschirmrand auf den Detailseiten
  (DISPLAY, STANDBY, CUSTOM, RESET), wo auf dem 128×64-Landscape-Display
  Platz dafür ist. Bei DIVISOR SELECT bewusst weggelassen — bei bis zu 8
  Einträgen bleibt dafür kein vertikaler Platz mehr (anders als beim
  höheren Hochformat-Display der Eurorack-Version), die Checkboxen sind
  aber auch so selbsterklärend.
- `DEFAULTS` fragt jetzt über eine eigene JA/NEIN-Bestätigungsseite
  nach, statt wie bisher per "nochmal drücken zum Bestätigen".
- **Nicht übernommen:** die CV-Inputs-Kategorie der Eurorack-Version
  (keine CV-Hardware auf diesem Board) sowie `DISPLAY > ROTATE`
  (dieses Gerät bleibt fest im Landscape-Format verbaut, keine
  Laufzeit-Drehung nötig).

**Umbenennung (Fortführung von Rev. 1.1):** interne Firmware-Bezeichner
für den Taktart-Taster (`PIN_BTN_TIMESIG`, `btnTimeSig`, `onTimeSigButton`
usw.) auf `Custom` umbenannt (`PIN_BTN_CUSTOM`, `btnCustom`,
`onCustomButton`), passend zur Custom-Switch-Umbenennung im Schaltplan.
Rein funktionslogische Bezeichner der Taktart-Auswahl selbst
(`timeSigIndex`, `TIME_SIG_*` usw.) bleiben unverändert, da sie
unabhängig vom auslösenden Taster sind. Die Ein-/Ausblende-Maske für
Taktarten (`enabledTimeSigMask` u.ä.) wurde mit der Taster-Freigabe
oben komplett entfernt, da sie ohne festen Taktart-Taster keinen Zweck
mehr hatte.

Betrifft: `firmware/barsync.ino`. Keine Hardware-Änderungen in dieser
Version.

---

## Hardware Rev. 1.1 (25.08.2026)

**Wichtiger Fix:**
- **R3 (Pull-up des Optokoppler-Ausgangs) korrigiert: +5V → +3V3.** In Rev. 1.0
  hing dieser Pull-up fälschlicherweise am +5V-Netz. Da derselbe Knoten direkt
  auf ESP32 GPIO15 geführt ist, lag GPIO15 im MIDI-Ruhezustand dauerhaft nahe
  5V an — außerhalb der Spezifikation des ESP32 (nicht 5V-tolerant, absolute
  Grenzspannung ca. VDD+0,3V). Rev. 1.0 wurde vor Fertigung storniert, dieser
  Fix ist von Anfang an in Rev. 1.1 enthalten.

**Weitere Änderungen:**
- **C1, C2 ergänzt** — 100nF-Abblockkondensatoren an den Vcc/GND-Pins von U1
  (6N139) und U2 (SN7406N) für saubereres MIDI-Timing.
- **Ungenutzte 7406-Gate-Eingänge (Pins 5, 9, 11, 13) auf GND gelegt**, statt
  offen zu bleiben — vermeidet unnötiges Schalten/Rauschen an unbenutzten
  TTL-Eingängen.

Betrifft: `hardware/kicad/BarSync/*`, `hardware/BarSync_BOM.md`/`.csv`,
`hardware/BarSync_Aufbauanleitung.md`. Die Firmware (v1.0.1) ist von diesen
Änderungen nicht betroffen.

---

## Hardware Rev. 1.0 / Firmware v1.0.1 (23.08.2026)

- Erste vollständige Version: Schaltplan, PCB-Layout und Firmware fertig.
- Firmware komplett auf Englisch (Display-Texte + Kommentare).
- Settings-Menü, Nudge-Modus, Drei-Stufen-Reset, MIDI-Analyzer.
- **Hinweis:** Diese Revision wurde vor der PCB-Fertigung storniert, siehe
  Rev. 1.1 oben — nicht nachbauen.

---

*[English version](CHANGELOG.en.md)*
