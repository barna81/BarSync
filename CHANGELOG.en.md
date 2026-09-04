# Changelog — BarSync

*[Deutsche Version](CHANGELOG.md)*

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
- With `FUNCTION = TIMESIG` selected, a new **`TIMESIGS >`** item
  appears below it — a checkbox list of which of the five time
  signatures the custom button cycles through (default: all). The
  fixed value on page 1 (`SETUP > TIMESIG`) is unaffected and still
  cycles through all five.
- With `FUNCTION = SET 1.1` selected, a new **`MODE`** item
  (QUANTIZED/INSTANT) appears below it directly — the setting that
  used to live under `Switches > Reset` has moved here, since it only
  ever concerned SET 1.1.
- `Switches > Custom Switch` (formerly "Custom Role") and
  `Switches > Reset Switch` (formerly "Reset") renamed.
- `Switches > Reset Switch` now has **`RESET 1`** and **`RESET 2`**
  (both yes/no, default yes) — configurable per stage: whether that
  stage also resets the elapsed play time. The bar counter/grid always
  resets regardless (no separate toggle for that anymore - the
  short-lived `BARCOUNTER` option is removed again). SET 1.1 is
  unaffected and always resets everything.
- **Menu visually unified:** every settings page now uses the same
  grid (item spacing, position of the explanation text, font size for
  explanations) — previously this varied from page to page (e.g. 8px
  here, 12px there). Explanation text now also consistently uses the
  smaller font throughout.

**Bugfix: BarSync wouldn't respond properly after standby (light
sleep) until the sequencer's second RUN/Start command — and the
original fix for that caused an even worse follow-up bug.**

Root cause of the original bug: GPIO-level wakeup on the MIDI RX line
only guarantees the CPU wakes up - not that the UART peripheral
cleanly receives the exact byte that triggered the wakeup (the clock
supply needs a brief moment to stabilize after light sleep).

The original fix (flush the UART buffer after waking, plus call
`MIDI.begin()` again) didn't actually fix that, and introduced a worse
bug instead: if the clock was already running when it woke us up, the
flush also discarded the genuine Start message and any Clock ticks
that had already queued up - `isRunning` never got set, and the
counter/time/grid just stayed completely frozen at their pre-sleep
state until an unrelated, fresh Stop+Start cycle came through later.
`MIDI.begin()` carries the same risk indirectly, since this library
re-initializes the transport internally, which can reset the
underlying HardwareSerial and just as easily wipe its buffer.

**Final fix:** both the buffer flush and the `MIDI.begin()` call are
removed again. A short settle delay after waking is enough on its own:
MIDI real-time messages (Clock/Start/Stop/Continue) are single bytes
that, unlike channel messages, don't rely on "running status", so they
can't desync the parser even if that one byte happens to arrive
corrupted. The beat-extrapolation anchor (`lastTickAnchorMicros`) is
still reset on wake, so a stale pre-sleep timestamp can't briefly
cause a wrong display.

**Reset logic reworked**, inspired by concepts from the Eurorack
version, but arrived at its own independent design after a few
iterations:

- Only **two reset stages** on the physical button (Reset 1/Reset 2)
  instead of three — stage 3 is removed. Short press = Reset 1, held
  past 1s = Reset 2 (press mechanism unchanged).
- **Reset 1** still waits for the **end of the current bar**, **Reset
  2** for the **end of the divisor cycle** - exactly as before; both
  operate within whichever beat pattern is currently active, without
  ever changing it themselves.
- **New: `SET 1.1`** — a standalone, immediate-acting function, only
  assignable to the custom button via `Switches > Custom > FUNCTION`
  (not reachable from the physical reset button). Immediately
  establishes a new "1.1" anchor, spinning up a brand new beat pattern
  that Reset 1 and 2 then keep operating within. Configurable via
  `Switches > Reset > MODE`:
  - **QUANTIZED** (default): rounds the new anchor down to the nearest
    already-existing beat in the current grid - the existing beat
    pattern (since MIDI Start, or an earlier SET 1.1) stays exactly in
    phase, only our own count gets realigned to the sequencer. Example:
    the display reads bar 1, beat 2, tick 1 - SET 1.1 pulls the new 1.1
    exactly to where beat 2 had just started.
  - **INSTANT**: uses the exact raw tick of the trigger instead,
    spinning up a brand new beat pattern independent of the previous
    grid.
- New setting `Switches > Reset > PLAYTIME` (yes/no, default yes):
  whether a reset (1, 2, or SET 1.1) also resets the elapsed play time
  - independent of which of the three it is.
- **Flash feedback adopted from the Eurorack:** after any reset action,
  "RESET 1"/"RESET 2"/"SET 1.1" now blinks exactly 2x in time with the
  beat (instead of time-based), in addition to the existing brief
  instant flash.

**The custom button is now freely assignable** (`Switches > Custom`),
instead of permanently cycling the time signature:

- New setting `Switches > Custom > FUNCTION` with four possible roles:
  `TIMESIG` (default, previous behavior), `RESET 1`, `RESET 2`,
  `SET 1.1`. The two reset roles directly trigger the matching,
  already-existing reset stage with a single short press — no need for
  the differently-timed hold that the reset button itself still
  requires; they wait for the bar/cycle end exactly like the physical
  button. `SET 1.1` instead always acts immediately (see above).
- Time signature is decoupled from button cycling for this and now
  lives as its **own fixed item on page 1** (`SETUP > TIMESIG`),
  directly cycling between the five implemented time signatures
  (default: 4/4). The previous enable/disable screen for restricting
  which time signatures were reachable via button cycling
  (`Switches > Timesig`) is removed entirely — without a dedicated
  time-signature button it no longer served a purpose; all five time
  signatures are now always reachable.
- Holding the custom button for 1s (toggle MIDI analyzer), the nudge
  combo (Custom+Divisor), and the MidiWar easter egg (Custom+Reset)
  are unchanged and work regardless of the assigned role.

**Settings menu fundamentally reworked**, adopted from the Eurorack
firmware's multi-level menu redesign (dated 2026-08-30 there) and
adapted for the desktop version:

- The menu is now two levels deep (`SETUP > SWITCHES/DISPLAY/STANDBY/
  DEFAULTS > detail page`) instead of a flat 6-item list. DIVISOR and
  the new RESET item now live under `SWITCHES` (alongside `CUSTOM`),
  CONTRAST and INVERT under `DISPLAY`.
- A short one/two-line explanation now appears at the bottom of the
  detail pages (DISPLAY, STANDBY, CUSTOM, RESET), where the 128x64
  landscape display has room for it. Deliberately omitted on DIVISOR
  SELECT — with up to 8 entries there's no vertical room left for it
  (unlike the Eurorack's taller portrait display), and the checkboxes
  are self-explanatory regardless.
- `DEFAULTS` now asks via its own YES/NO confirmation page instead of
  the previous "press again to confirm" flow.
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
