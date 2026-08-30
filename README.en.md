# BarSync

*[Deutsche Version](README.md)*

**MIDI Clock Bar Counter & Visualizer**    -*Always in Time.*-

BarSync is an ESP32-based device that receives an incoming MIDI clock and
displays bar position, beat progress, tempo, and elapsed play time on a
128×64 OLED display.

![BarSync](images/barsync_pic2.jpeg)


## Status (as of 2026-08-25)

BarSync - Desktop version (this device)
- ✅ Schematic, PCB layout (hardware rev. 1.1), and firmware finished and tested
- ✅ PCBs have been ordered
- 🔜 A matching 3D-printable enclosure is in development
- 🔜 A solder-it-yourself kit is being considered, depending on interest

BarSync - Eurorack version (in development)
- ✅ Code about 80% done
- 🔜 Schematic and PCB layout to follow
- 🔜 3D-printable Eurorack frame is in development

*(Interested in the kit or the Eurorack version? Open an issue in this repo or get in touch.)*


---


## What is this?
Never missthe drop...

BarSync keeps you locked into your track's arrangement — live or in the studio. Know exactly where you stand in the bar count, so drops, breaks and builds never catch you off guard.

Since I couldn't find anything similar on the market or in the DIY scene, I built BarSync.
BarSync supports you during your live performance. During a show your hands are usually full, and it's easy to lose track of the timing. No more counting bars in your head to land the drop at the right moment: BarSync counts and visualizes the bars elapsed since a starting point you define yourself. That starting point can be the first MIDI clock signal from your sequencer, or you can reset it at any time with the reset button. This way you always know exactly which bar you're in. The visualization adapts to your needs (1-128 bars can be displayed).


## Features

- Bar/beat counter, synced to the incoming MIDI clock (24 PPQN)
- Divisor progress bar (x1 to x128 bars) — overall area stays constant,
  tile size adapts accordingly
- 5 selectable time signatures (2/4, 3/4, 4/4, 5/4, 7/8)
- Three-stage reset (short/medium/long, quantized to bar or cycle end)
- Built-in MIDI analyzer (jitter, interval, BPM range) right on the device
- Nudge mode for manually compensating clock phase drift
- MIDI-Thru
- Standby (light sleep) on MIDI inactivity
- On-device settings menu (no app or software required)


## Operation

Quick overview — full guide in
[`docs/BarSync_Quickstart_EN.pdf`](docs/BarSync_Quickstart_EN.pdf):

| Button | Function |
|---|---|
| Time Sig (short) | Next time signature |
| Time Sig (hold 1s) | Toggle MIDI analyzer |
| Divisor (short) | Next divisor value |
| Reset (short/medium/long) | Reset stage 1/2/3 |
| Time Sig + Divisor together | Toggle nudge mode |
| Hold Reset 1s at boot | Settings menu |


## Cool! How do I get one?

If you'd like to build the device yourself, see [LICENSE](LICENSE) — you'll find the corresponding firmware, schematic, and parts list here. I've also designed a PCB, whose layout is included as well. Since it's a fairly simple layout, you can also use perfboard or even a breadboard to build BarSync. Basic knowledge of the ESP32, a soldering iron, and general electronics will be helpful.

In short: the project is currently still in a full DIY stage. Depending on interest, I'm considering offering a BarSync kit. It would come with a 3D-printed enclosure, a PCB, and all the necessary electronic components ready to be assembled. All that would be left to do is soldering and flashing the ESP32.

---


## Building Your Own

Full [`schematic`](hardware/kicad/BarSync/BarSync_schematic.pdf), PCB layout, and Gerber files are in
[`hardware/`](hardware/).
Full bill of materials: [`hardware/BarSync_BOM.en.md`](hardware/BarSync_BOM.en.md)
Assembly guide: [`hardware/BarSync_Aufbauanleitung.en.md`](hardware/BarSync_Aufbauanleitung.en.md)


## Key Hardware

- ESP32 dev board (30-pin DOIT layout)
- SSD1309 OLED, 2.42", 128×64, SPI
- 3 buttons (time signature, divisor, reset)


## Flashing the Firmware

1. Install the Arduino IDE, add ESP32 board support
2. Install libraries: **"MIDI Library"** (FortySevenEffects), **"U8g2"** (olikraus)
3. Open [`firmware/barsync.ino`](firmware/barsync.ino), select board "ESP32 Dev Module", upload


## License

This project is licensed under **CC BY-NC-SA 4.0** — see [LICENSE](LICENSE).
Free to use, modify, and share for **non-commercial** purposes, with
attribution and share-alike terms. For commercial use, please get in touch.

---


*[Deutsche Version](README.md)*

*Built with a lot of debugging, deep MIDI timing analysis, and an
occasional detour into building a Space Invaders clone.*
