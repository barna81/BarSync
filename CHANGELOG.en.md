# Changelog — BarSync

*[Deutsche Version](CHANGELOG.md)*

## Firmware v1.3.0 (2026-09-11)

**New: Marker Editor.** You can now place your own markers directly on
the bar grid (four symbols to choose from) — handy for anything that
needs a fixed reference to the bar count:
- Mark arrangement changes (break, drop, build), so you always know
  what's coming next.
- Flag the start of each phrase, so the band can follow the arrangement
  live without memorizing it.
- Cue points for planned lighting/FX changes, synced exactly to the bar.

Reached via `OPERATION SETUP > SET MARKERS`.

**New: two grid display modes.** `CYCLE` loops the grid repeatedly
through the same window of bars (x1 to x128) — you see where you stand
within the current cycle. `SCROLL` instead runs continuously forward,
always showing the current bar plus what's coming up next, including
any markers you've placed scrolling in from the right. Switchable via
`OPERATION SETUP > GRID MODE`.

**New: End Bar.** Defines a bar at which the count should loop back,
stop, or simply carry on - either at a fixed bar number, or
automatically at your last placed marker. Configurable via
`OPERATION SETUP > END BAR` / `END BAR ACT`.

Affects: `firmware/barsync.ino`. No hardware changes in this version.

---

## Firmware v1.2.2 (2026-09-06)

**MIDI Analyzer replaced with MIDI Monitor.** The previous jitter
analysis (deviation graph, peak-hold scaling) has been removed
entirely — the benefit no longer justified the complexity, and the
associated render-time diagnostic (`Rmax`) was only ever a temporary
debug aid anyway. The screen (still reached by holding the custom
button for 1s) now shows:

- **Note/CC/Program Change/Pitch Bend log**: the last 6 received
  messages, newest on top, older ones pushed down and dropped after
  6 entries. Plain FIFO buffer, no per-channel dedup.
- **MIDI clock graph**: one spike per detected beat, independent of
  Start/Stop (so it still shows an incoming-but-stopped signal). Now
  always exactly 8 beats wide, so the sample rate automatically scales
  with tempo. The downbeat (1.1, time-signature aware) is marked with
  a double-width spike. The "MIDI CLK" label flashes inverted on the
  beat, and shows a blinking "NO CLOCK" if no clock byte has arrived
  for 5s.
- **RUN/STOP trace**: level trace on the same timebase as the clock
  graph.
- **BPM readout** top right, one decimal place.
- The reset button clears the Note/CC log on this screen instead of
  triggering the normal Reset 1/2 function.

**Divisor/Grid menu: visible label finally switched to "Grid".** After
the button silkscreen and all documents were already renamed to "Grid"
in the last update, the corresponding setup menu item itself
(`SWITCHES > DIVISOR` / `DIVISOR SELECT`) still internally said
"Divisor". Now consistently `GRID` / `GRID SELECT`. Internal firmware
identifiers (`divisorIndex`, `PIN_BTN_DIVISOR`, `SCR_DIVISOR`, etc.)
are unchanged, same as with the button rename.

**Grid view: 32-bar group separators for x16/x32/x64/x128**, each with
its own separator thickness so the overall height stays exactly
identical across every divisor view (35px, previously a fixed 32px).
Can now be turned off via `DISPLAY > GRID LINES` (Yes/default,
No = original layout with no grouping at all).

**x128 view: remaining-bars indicator on the right edge** (adopted
from the Eurorack variant) — one tick per row (= 8 bars) not yet
played through, visibly counting down from 16 to 0, current row
blinks on the beat.

**New contrast steps and defaults:**
- Contrast steps now `25/50/100/150/200/255` (previously
  `50/100/150/200/255`), default 50 (previously 255).
- Grid Select (divisor selection) now starts with x4–x128 enabled
  (x1/x2 disabled) instead of all — matches the Eurorack variant's
  factory default.

**Boot screen: BarSync logo added**, to the left of the "**BARSYNC**"
text — the four tiles fill up in step with the loading blink cadence,
fully filled after at most 4 blink cycles.

Affects: `firmware/barsync.ino`. No hardware changes in this version.

---

## Hardware Rev. 1.2 (2026-09-05)

**Button net name in the schematic/netlist renamed from "Divisor
Switch" to "Grid Switch"** — a pure identifier change in the KiCad
project (schematic and netlist), no electrical or pin change. This
completes the renaming that previously only covered the enclosure
print and documentation (see entry below), now also carried through
into the KiCad project.

**PCB silkscreen updated:** the corresponding button's label on the
board changed from "DIVISOR" to "GRID", matching the enclosure print.

Affects: `hardware/kicad/BarSync/BarSync.kicad_sch`,
`BarSync.kicad_pcb`, `BarSync.net`/`BarSync_Netlist.txt`. Internal
firmware identifiers (`divisor`, `divisorIndex`, `PIN_BTN_DIVISOR`,
etc.) remain unchanged.

## Firmware v1.2.0 (2026-09-02)

**Bugfix: BarSync stayed frozen at its pre-sleep state after waking
from standby, even with MIDI clock already running - only a manual
Stop+Run on the sequencer got it moving again.** Root cause:
`isRunning` is only ever set by an explicit Start/Continue message. If
the sequencer was already running when it woke us up (no fresh Start/
Continue, since from its side playback never stopped), Clock ticks did
arrive, but those alone never set `isRunning` - BarSync was waiting for
a message that was never going to come. Fix: if the wake reason wasn't
a button (so, presumably, MIDI activity), every temporary grid/time
value is now cleared and BarSync starts fresh at 1.1 right away,
regardless of whether an explicit Start message follows. If one does
arrive anyway, it just harmlessly resets everything again; if no real
clock follows at all, the existing clock-loss watchdog
(`CLOCK_LOST_TIMEOUT_MS`) catches that on its own within a few seconds.

**Bugfix: short button presses were sometimes swallowed entirely.**
The old debounce required a new level to stay stable for at least
50ms before it counted as a change at all - a press-and-release that
both happened inside that window was never recognized as "stable" and
simply vanished, rather than just being delayed. Switched to
edge-triggered debounce instead: the edge is accepted immediately,
followed by a 50ms blackout window against contact bounce. Affects
all three buttons equally, since they all go through the same
`updateButton()` function.

**SET 1.1 (QUANTIZED mode) now rounds to the nearest beat instead of
always rounding down to the last one that passed.** If the press is
closer to the upcoming beat than the current one, it now waits briefly
(at most 12 ticks, i.e. at most half a beat) and commits exactly there,
instead of always snapping backward to the beat that already passed.

**Menu refinement: reorganized the custom role and reset settings.**

- `Switches > Custom > FUNCTION` now only has two roles: `TIMESIG` and
  `SET 1.1` (**new default**) — `RESET 1`/`RESET 2` are removed
  entirely as custom-button roles (the physical reset button keeps its
  two stages unchanged).
- With `FUNCTION = TIMESIG` set, a new **`TIMESIGS >`** entry appears
  below it — a checkbox list of which of the five time signatures the
  custom button cycles through (default: all). The fixed time
  signature value on page 1 (`SETUP > TIMESIG`) is unaffected and
  still rotates through all five.
- With `FUNCTION = SET 1.1` set, a new **`MODE`** (QUANTIZED/INSTANT)
  appears directly below it instead — the setting that used to live
  under `Switches > Reset` has moved here, since it only ever affects
  SET 1.1.
- `Switches > Custom Switch` (formerly "Custom Role") and
  `Switches > Reset Switch` (formerly "Reset") renamed.
- `Switches > Reset Switch` now has **`RESET 1`** and **`RESET 2`**
  (both Yes/No, default Yes) — independently configurable per stage
  whether it also resets the elapsed play time. The bar counter/grid
  always resets regardless (no separate switch for that anymore - the
  `BARCOUNTER` option introduced in the meantime is removed again).
  SET 1.1 is unaffected by this and always resets everything.
- **Menu visually unified:** every settings page now uses the same
  grid (item spacing, explanation position, explanation font size) -
  previously this differed from page to page (e.g. 8px here, 12px
  there). The explanation text also now consistently runs in the
  smaller font.

**Bugfix: BarSync only responded to the second RUN/Start command from
the sequencer after standby (light sleep) - and the original fix for
this caused an even worse follow-on bug.**

Root cause of the original bug: the GPIO level wakeup on the MIDI RX
line only guarantees that the CPU wakes up - not that the UART
peripheral cleanly receives the very byte that triggered the wakeup
(the clock supply needs a brief moment to stabilize after light
sleep).

The original fix (clearing the UART buffer after waking + calling
`MIDI.begin()` again) didn't fix the problem, though - it replaced it
with a worse one: if a MIDI clock was already running when it woke up
(sequencer already playing), clearing the buffer also discarded the
real Start command and any clock ticks that had already arrived -
`isRunning` stayed false, counters/time/grid stayed completely frozen
at their pre-sleep state until a fresh, independent Stop+Start pair
came through. `MIDI.begin()` carries the same risk indirectly, since
the library re-initializes the transport internally, which can also
reset the underlying UART and clear its buffer.

**Final fix:** both (buffer-clearing and the `MIDI.begin()` call)
removed again. All that's left is a brief stabilization pause after
waking - which is enough, since MIDI real-time messages (Clock/Start/
Stop/Continue) are single bytes that, unlike channel messages, don't
expect "running status" and therefore can't throw the parser off step,
even if that one byte happens to arrive corrupted. The beat
extrapolation anchor (`lastTickAnchorMicros`) is also still reset on
waking, so no stale pre-sleep timestamp can briefly cause a wrong
display.

**Reset logic reworked**, inspired by concepts from the Eurorack
version, but with its own distinct result after several iterations:

- Only **two reset stages** on the physical button (Reset 1/Reset 2)
  instead of three - stage 3 is gone. Short press = Reset 1, hold past
  1s = Reset 2 (press mechanics unchanged).
- **Reset 1** still waits for the **end of the current bar** as
  before, **Reset 2** for the **end of the divisor cycle** - both
  operate within whatever beat pattern is currently active, without
  changing it themselves.
- **New: `SET 1.1`** - a standalone, instant-acting function, only
  assignable to the custom button via `Switches > Custom > FUNCTION`
  (not reachable via the physical reset button). Immediately
  establishes a new "1.1" anchor, spinning up a brand new beat
  pattern within which Reset 1 and 2 then keep operating. Configurable
  via `Switches > Reset > MODE`:
  - **QUANTIZED** (default): rounds the new anchor down to the
    nearest beat boundary already existing in the current grid - the
    existing beat pattern (since MIDI Start or a previous SET 1.1)
    stays in phase, only our own count is nudged back in line with
    the sequencer. Example: the display sits at bar 1, beat 2, tick 1
    - SET 1.1 pulls the new 1.1 to exactly where the 1.2 just was.
  - **INSTANT**: uses the exact raw tick of the trigger as-is,
    spinning up a brand new beat pattern independent of the previous
    grid.
- New setting `Switches > Reset > PLAYTIME` (Yes/No, default Yes):
  whether a reset (1, 2, or SET 1.1) also resets the elapsed play
  time - independent of which of the three functions it is.
- **Flash feedback adopted from the Eurorack:** after every reset
  action, "RESET 1"/"RESET 2"/"SET 1.1" now blinks exactly 2x on the
  beat (instead of time-based), in addition to the brief instant flash
  animation.

**The custom button is now freely assignable** (`Switches > Custom`),
instead of permanently cycling the time signature:

- New setting `Switches > Custom > FUNCTION` with four possible roles:
  `TIMESIG` (default, previous behavior), `RESET 1`, `RESET 2`,
  `SET 1.1`. The two reset roles directly trigger the correspondingly
  named, already-existing reset stage - with a single short press,
  without the otherwise-required differently-timed hold on the reset
  button itself; they wait for the bar/cycle end just like the
  physical button normally would. `SET 1.1`, by contrast, always acts
  instantly (see above).
- The time signature has been decoupled from the button cycle for
  this and now lives **fixed as its own menu item on page 1**
  (`SETUP > TIMESIG`), directly switchable between the five existing
  time signatures (default: 4/4). The previous screen for showing/
  hiding individual time signatures for button cycling
  (`Switches > Timesig`) is gone without replacement - it no longer
  made sense without a dedicated time-signature button; all five time
  signatures are now always reachable.
- Holding the custom button for 1s (toggle MIDI analyzer), the nudge
  combo (Custom+Divisor), and the MidiWar easter egg (Custom+Reset)
  remain unchanged and work independently of the chosen role.

**Settings menu fundamentally reworked**, adopted from the multi-level
menu redesign of the Eurorack firmware (there as of 2026-08-30) -
adapted for the desktop version:

- Menu is now two-tiered (`SETUP > SWITCHES/DISPLAY/STANDBY/DEFAULTS >
  detail page`) instead of a flat 6-item list. DIVISOR and the new
  RESET live under `SWITCHES` (now alongside `CUSTOM`), CONTRAST and
  INVERT under `DISPLAY`.
- A brief explanation at the bottom of the screen on the detail pages
  (DISPLAY, STANDBY, CUSTOM, RESET), where there's room for it on the
  128×64 landscape display. Deliberately left out on DIVISOR SELECT -
  with up to 8 entries there's no vertical room left for it (unlike
  the Eurorack version's taller portrait display), but the checkboxes
  are self-explanatory anyway.
- `DEFAULTS` now asks via its own YES/NO confirmation page, instead of
  the previous "press again to confirm".
- **Not adopted:** the Eurorack version's CV Inputs category (no CV
  hardware on this board) and `DISPLAY > ROTATE` (this unit is
  permanently mounted in landscape, no runtime rotation needed).

**Renaming (continuing rev. 1.1):** internal firmware identifiers for
the custom button (`PIN_BTN_TIMESIG`, `btnTimeSig`, `onTimeSigButton`,
etc.) renamed to `Custom` (`PIN_BTN_CUSTOM`, `btnCustom`,
`onCustomButton`), matching the custom-switch renaming already done in
the schematic. Identifiers that belong to the time-signature logic
itself (`timeSigIndex`, `TIME_SIG_*`, etc.) are unchanged, since they
are independent of which button triggers them. The time-signature
enable/disable mask (`enabledTimeSigMask` etc.) was removed entirely
along with the button reassignment above, since it no longer served a
purpose without a dedicated time-signature button.

Affects: `firmware/barsync.ino`. No hardware changes in this version.

---

## Hardware Rev. 1.1 (2026-08-25)

**Important fix:**
- **R3 (optocoupler output pull-up) corrected: +5V → +3V3.** In rev. 1.0,
  this pull-up was incorrectly tied to the +5V rail. Since the same node
  feeds directly into ESP32 GPIO15, GPIO15 sat near 5V continuously during
  MIDI idle — outside the ESP32's specification (not 5V-tolerant, absolute
  max rating roughly VDD+0.3V). Rev. 1.0 was cancelled before fabrication;
  this fix is included from the start in rev. 1.1.

**Other changes:**
- **Added C1, C2** — 100nF decoupling capacitors on the Vcc/GND pins of U1
  (6N139) and U2 (SN7406N) for cleaner MIDI timing.
- **Tied unused 7406 gate inputs (pins 5, 9, 11, 13) to GND** instead of
  leaving them floating — avoids unnecessary switching/noise on unused TTL
  inputs.

Affects: `hardware/kicad/BarSync/*`, `hardware/BarSync_BOM.md`/`.csv`,
`hardware/BarSync_Aufbauanleitung.md`. The firmware (v1.0.1) is unaffected
by these changes.

---

## Hardware Rev. 1.0 / Firmware v1.0.1 (2026-08-23)

- First complete version: schematic, PCB layout, and firmware finished.
- Firmware fully in English (display text + comments).
- Settings menu, nudge mode, three-stage reset, MIDI analyzer.
- **Note:** This revision was cancelled before PCB fabrication, see rev. 1.1
  above — do not build.

---

*[Deutsche Version](CHANGELOG.md)*
