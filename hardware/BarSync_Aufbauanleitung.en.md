# Assembly Guide — BarSync

*[Deutsche Version](BarSync_Aufbauanleitung.md)*

Recommended build order, from simplest to most demanding.
Parts list and references match the verified `BarSync_BOM_en.md`.

---

## 1. Preparation

- Open the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)
- Check off all components against `BarSync_BOM_en.md`/`.csv` before you start
- **All four resistor positions (R1–R4) are populated** — no position is left empty

## 2. Solder the Small Parts First

Order: start with the flattest/most robust components, then the more delicate ones.

1. **Resistors R1, R2, R3, R4** (polarity doesn't matter, no orientation to worry about)
2. **Capacitors C1, C2** (100nF, ceramic/X7R — also non-polarized, can be soldered either way round)
3. **Diode D1** — mind the polarity! The cathode band on the diode must point toward the corresponding marking on the board
4. **DIP sockets** for U1 (8-pin) and U2 (14-pin) — the notch/dot on the socket marks pin 1; align it with the marking on the board

## 3. Solder the Headers

5. **J1** (2-pin, MIDI-IN), **J2** (3-pin, MIDI-THRU), **J3/J4/J5** (2-pin each, buttons), **J6** (7-pin, display)
- Solder all of them straight and vertical — it's best to tack down one pin first, check the alignment, and only then solder the rest

## 4. Solder the ESP32 onto the Board

6. Place the ESP32 in its designated spot and solder it in
   > **Note:** Contrary to how the KiCad footprint (`DOIT_ESP32_DEVKIT_30Pins`) looks, the ESP32 is soldered **directly onto the board, with no socket/female headers in between**. The footprint's 3D model suggests a socket, but that's purely cosmetic. Only the peripheral headers (J1–J6) get pin headers — not the ESP32 itself.

## 5. Insert the ICs

7. **6N139** into the DIP-8 socket (U1) — mind the notch
8. **SN7406N** into the DIP-14 socket (U2) — mind the notch

## 6. Fit the Electronics into the Enclosure

Required hardware (see `BarSync_BOM_en.md`/`.csv`):

- 4× Ruthex threaded insert, M3
- 4× socket-head screw M3×12
- 4× socket-head screw M2×4
- 8× socket-head screw M2×10
- 4× nut M2
- 12× washer M2

9. **Fit the PCB:** Position the PCB in the bottom half of the enclosure and screw it down with 4× M2×10 socket-head screws into the designated mount
10. **Display:** Mount the display with 4× M2×4 socket-head screws (use 2 washers per screw to shorten the effective thread length)
11. **MIDI jacks:** Insert the MIDI jacks (mind the IN/THRU arrangement), fasten with 4× M2×10 socket-head screws, and secure from the inside with 4× M2 nuts and 4× M2 washers
12. **Mount the buttons:** Insert the 3× buttons (Custom, Grid, Reset) into their designated openings and secure with the lock nuts
13. **Set the threaded inserts:** Press/melt the 4× Ruthex threaded inserts (M3) into the corresponding mounts in the top half of the enclosure (set the soldering iron to roughly the material's forming temperature — e.g. 220°C for PLA)

## 7. Prepare and Connect the Cables

!!!IMPORTANT: make sure the polarity/pinout matches the [`schematic`](kicad/BarSync/BarSync_schematic.pdf)!!!

14. Plug the Dupont cables into the corresponding headers:
    - J1 (2-pin) → MIDI-IN socket (pin 4 + pin 5 of the DIN-5 socket)
    - J2 (3-pin) → MIDI-THRU socket (pin 2 + pin 4 + pin 5 of the DIN-5 socket)
    - J3/J4/J5 → respective button
    - J6 (7-pin) → display

## 8. Flash the Firmware

This section is deliberately detailed — including for anyone who has never worked with the Arduino IDE before.

15. **Install the Arduino IDE** (if not already installed): download the current version from https://www.arduino.cc/en/software and install it
16. **Add ESP32 board support**:
    - Open the Arduino IDE
    - File → Preferences (Windows) or Arduino IDE → Settings (Mac)
    - In the "Additional boards manager URLs" field, enter: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
    - Click OK
    - Tools → Board → Boards Manager → search for "esp32" → install the **"esp32 by Espressif Systems"** package (takes a few minutes, it's fairly large)
17. **Select the board**: Tools → Board → esp32 → **ESP32 Dev Module**
18. **Install the libraries** (if not done already): Sketch → Include Library → Manage Libraries → search for and install **"MIDI Library"** (FortySevenEffects) and **"U8g2"** (olikraus)
19. Connect the board to the computer via USB cable
20. **If no port shows up under Tools → Port**: the USB driver for the board's chip is probably missing — see the "Install the USB driver" box right below
21. **Select the port**: Tools → Port → choose the matching port (Windows: e.g. `COM3`; Mac: e.g. `/dev/cu.usbserial-...` or `/dev/cu.SLAB_USBtoUART`)
22. Open `barsync.ino` (located in the `firmware/barsync/` folder in the repo)
23. **Upload**: click the arrow button in the top-left of the Arduino IDE. If the upload fails with a timeout: set Tools → Upload Speed to `115200` and try again

> ### Install the USB driver (only needed if no port shows up)
>
> Most ESP32 boards use a **CP2102** USB chip for the USB connection. For the computer to recognize the board as a serial port, it needs a driver for that chip — this isn't included by default in Windows or macOS.
>
> **Windows:**
> 1. Open https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers in your browser
> 2. Scroll to "Downloads", download the **"CP210x Windows Drivers"** (ZIP file)
> 3. Extract the ZIP file (right-click → Extract All)
> 4. In the extracted folder, run the included `.exe` installer if there is one. If there isn't: open the Windows Device Manager (right-click the Start menu → Device Manager) → look for the board under "Other devices" or "Ports (COM & LPT)" (often shown with a warning icon) → right-click it → "Update driver" → "Browse my computer for drivers" → select the extracted folder
> 5. Unplug and replug the board's USB cable
> 6. A COM port should now appear under Tools → Port in the Arduino IDE (e.g. `COM3`)
>
> **Mac:**
> 1. Open https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers in your browser
> 2. Scroll to "Downloads", download the **"CP210x VCP Mac OSX Driver"** (ZIP containing a `.pkg` installer)
> 3. Unzip it, double-click the `.pkg` file and follow the installation steps
> 4. macOS blocks newly installed system extensions by default — right after installation you'll usually see a "System Extension Blocked" notice. Then: open System Settings → Privacy & Security, scroll to the bottom where a notice about the blocked extension from "Silicon Laboratories" appears → click "Allow"
> 5. Restart the Mac (macOS requires this after installing a kernel extension)
> 6. Reconnect the board via USB
> 7. An entry like `/dev/cu.SLAB_USBtoUART` or `/dev/cu.usbserial-...` should now appear under Tools → Port in the Arduino IDE
>
> If your board instead has a **CH340/CH341** chip (usually printed directly on the small IC next to the USB connector), you'll need the matching **WCH CH340 driver** instead of the CP210x one — same idea, different download source (manufacturer: WCH).

## 9. First Functional Test — Before Closing the Enclosure

24. On startup, the display should show the boot screen ("BarSync")
25. Connect a MIDI source, start the clock → the display should switch to "RUN"
26. Test all three buttons individually (trigger the Custom button — currently: SET 1.1 —, change Grid, trigger Reset)
27. Only once everything works: screw the top and bottom enclosure halves together with 4× M3×12 socket-head screws

---

## Known Pitfalls

| Problem | Solution |
|---|---|
| Display stays dark | Check SPI wiring at J6 (pin order), check contrast in the setup menu |
| No MIDI signal detected | Check optocoupler orientation (U1) and D1 polarity |
| Buttons don't respond or respond incorrectly | Check the mapping: J3=Custom(GPIO32), J4=Grid(GPIO33), J5=Reset(GPIO25) |

---

*For BarSync hardware rev. 1.2 — component references verified against `BarSync.kicad_pcb`/`.net`.
See also `pinplan_en.md` for the full pin reference and `CHANGELOG_en.md` for the change history.*

---

*[Deutsche Version](BarSync_Aufbauanleitung.md)*
