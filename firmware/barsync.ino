/*
 * ============================================================================
 *  BARSYNC — MIDI Clock Bar Counter & Visualizer — ESP32 + SSD1309 OLED (SPI, 128x64)
 *  Version: 1.2.1
 * ============================================================================
 *
 * Counts incoming MIDI clock (24 PPQN), derives beat/bar from it, and shows
 * bar number, BPM, elapsed time, beat progress, and a multi-row divisor
 * progress grid (x1 to x64 bars) on a monochrome 128x64 OLED, in a
 * reduced "Elektron" style.
 *
 * REQUIRED LIBRARIES (Library Manager):
 *   - "MIDI Library" by Francois Best (FortySevenEffects)
 *   - "U8g2" by olikraus
 *
 * HARDWARE / PINOUT: see pinplan.md
 * CHANGE HISTORY: see CHANGELOG.md
 * ============================================================================
 */

#define FW_VERSION "1.2.1"


#include <MIDI.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_sleep.h"
#include "driver/uart.h"
#include "driver/gpio.h"

// ---------------------------------------------------------------------------
// PIN DEFINITIONS
// ---------------------------------------------------------------------------
#define PIN_OLED_CS    5
#define PIN_OLED_DC    21
#define PIN_OLED_RESET 4
#define PIN_OLED_SCK   18
#define PIN_OLED_MOSI  23

#define PIN_MIDI_RX    15   // Hardware UART2 RX (GPIO16 not available on this board)
#define PIN_MIDI_TX    -1   // TX not required (MIDI-IN only)

#define PIN_BTN_DIVISOR 33
#define PIN_BTN_CUSTOM  32
#define PIN_BTN_RESET   25

// ---------------------------------------------------------------------------
// DISPLAY (SSD1309, Hardware-SPI)
// ---------------------------------------------------------------------------
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(
    U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RESET);

// ---------------------------------------------------------------------------
// MIDI (Hardware-Serial2 on the ESP32)
// ---------------------------------------------------------------------------
HardwareSerial MidiSerial(2);
MIDI_CREATE_INSTANCE(HardwareSerial, MidiSerial, MIDI);

// ---------------------------------------------------------------------------
// STATE — Clock / Bar / Beat
// ---------------------------------------------------------------------------
volatile uint32_t totalTicks   = 0;   // since start, runs "forever"
volatile uint16_t tickInBeat   = 0;   // 0..23
volatile uint16_t currentBeat  = 0;   // 0..(beatsPerBar-1)
volatile uint32_t currentBar   = 0;   // absolute bar number (0-based internally)
volatile bool     isRunning    = false;

// Clock watchdog: timestamp of the last received clock tick (in ms).
// Used to detect when the MIDI clock is "lost"
// (e.g. cable unplugged, source stopped without sending a Stop message).
volatile uint32_t lastClockTickMillis = 0;
const uint32_t    CLOCK_LOST_TIMEOUT_MS = 5000;

// ---------------------------------------------------------------------------
// STANDBY / LIGHT SLEEP
// ---------------------------------------------------------------------------
const uint32_t STANDBY_COUNTDOWN_MS = 10000; // show countdown 10s ahead (independent of chosen delay)

// Prevents a button that woke the ESP32 from standby
// from also triggering its normal function on release.
bool suppressDivisorAction = false;
bool suppressCustomAction = false;
bool suppressResetAction   = false;

// time signature: counter values (denominator is only carried along for the display)
const uint8_t  TIME_SIG_NUM[]   = {4, 3, 5, 6, 7};
const char*    TIME_SIG_LABEL[] = {"4/4", "3/4", "5/4", "6/8", "7/8"};
const uint8_t  TIME_SIG_COUNT   = 5;
uint8_t timeSigIndex = 0; // index into the array above
uint8_t beatsPerBar()  { return TIME_SIG_NUM[timeSigIndex]; }

// Divisor (bars per progress cycle)
const uint8_t DIVISOR_VALUES[] = {1, 2, 4, 8, 16, 32, 64, 128};
const uint8_t DIVISOR_COUNT    = 8;
uint8_t divisorIndex = 2; // starts at x4 (index freely selectable)
uint8_t divisor()      { return DIVISOR_VALUES[divisorIndex]; }

// ---------------------------------------------------------------------------
// settings (persistent in the Flash via Preferences/NVS)
// ---------------------------------------------------------------------------
Preferences prefs;

// Custom button role (Switches > Custom): which fixed function the
// custom button performs on a normal short press. Default: SET 1.1
// (instant re-anchor - see triggerSet11()).
enum CustomRole { CUSTOM_ROLE_TIMESIG = 0, CUSTOM_ROLE_SET11 = 1 };
const uint8_t CUSTOM_ROLE_COUNT = 2;
const char* CUSTOM_ROLE_LABEL[] = {"TIMESIG", "SET 1.1"};

struct Settings {
  uint8_t timeSigIndex;
  uint8_t divisorIndex;
  uint8_t contrast;
  bool    invert;       // true color inversion (controller command)
  uint8_t enabledDivisorMask; // bit i = divisor i selectable
  bool    standbyEnabled;
  uint8_t standbyDelayIndex; // index into STANDBY_DELAY_MINUTES
  uint8_t customButtonRole;  // one of the CustomRole values above (Switches > Custom)
  uint8_t enabledTimeSigMask; // bit i = time signature i selectable for the
                               // custom button's TIMESIG role to cycle through
                               // (Switches > Custom > Function=TIMESIG > Timesigs).
                               // Does NOT affect the fixed value on page 1
                               // (SETUP > TIMESIG), which always cycles through
                               // all of them.
  bool    resetInstantMode;  // Only affects SET 1.1 (Custom button role) - Reset 1
                              // and Reset 2 (physical reset button only, it's no
                              // longer a custom-button role) are unaffected by
                              // this and always just wait for the next bar/cycle
                              // end, exactly like before any of this existed.
                              // false (default) = QUANTIZED: SET 1.1's new anchor
                              // is rounded down to the nearest already-existing
                              // beat boundary (realigns with the sequencer's own
                              // beat pattern without changing its phase); true =
                              // INSTANT: SET 1.1 uses the raw current tick as-is,
                              // spinning up a brand new phase - see triggerSet11().
                              // Menu: Switches > Custom > Function=SET 1.1 > Mode
  bool    reset1PlaytimeEnabled; // whether Reset 1 (physical button, short
                                  // press) also resets the elapsed play time.
                                  // The bar counter/grid always resets
                                  // unconditionally for Reset 1 - only the
                                  // play time is optional. Does not affect
                                  // SET 1.1, which always resets everything.
                                  // Menu: Switches > Reset Switch > Reset 1
  bool    reset2PlaytimeEnabled; // same as above, for Reset 2 (physical
                                  // button, held past 1s) - independent
                                  // setting, not shared with Reset 1.
                                  // Menu: Switches > Reset Switch > Reset 2
};
Settings settings;

const uint8_t CONTRAST_STEPS[] = {50, 100, 150, 200, 255};
const uint8_t CONTRAST_STEP_COUNT = 5;

const uint16_t STANDBY_DELAY_MINUTES[] = {1, 5, 10, 30, 60};
const uint8_t  STANDBY_DELAY_COUNT = 5;
uint32_t getStandbyTimeoutMs() {
  return (uint32_t)STANDBY_DELAY_MINUTES[settings.standbyDelayIndex] * 60000UL;
}

bool isDivisorEnabled(uint8_t i) { return (settings.enabledDivisorMask >> i) & 0x01; }

uint8_t countEnabledDivisor() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < DIVISOR_COUNT; i++) if (isDivisorEnabled(i)) c++;
  return c;
}

// Find the next enabled index (wrap-around), for normal
// cycling via button during operation.
uint8_t nextEnabledDivisor(uint8_t current) {
  uint8_t idx = current;
  for (uint8_t i = 0; i < DIVISOR_COUNT; i++) {
    idx = (idx + 1) % DIVISOR_COUNT;
    if (isDivisorEnabled(idx)) return idx;
  }
  return current;
}

bool isTimeSigEnabled(uint8_t i) { return (settings.enabledTimeSigMask >> i) & 0x01; }

uint8_t countEnabledTimeSig() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) if (isTimeSigEnabled(i)) c++;
  return c;
}

// Find the next enabled time signature (wrap-around) - only used for
// the custom button's TIMESIG role (Switches > Custom); the fixed
// value on page 1 (SETUP > TIMESIG) always cycles through all of them
// regardless of this mask.
uint8_t nextEnabledTimeSig(uint8_t current) {
  uint8_t idx = current;
  for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) {
    idx = (idx + 1) % TIME_SIG_COUNT;
    if (isTimeSigEnabled(idx)) return idx;
  }
  return current;
}

void loadSettings() {
  prefs.begin("midiclock", true);
  settings.timeSigIndex = prefs.getUChar("tsig", 0);
  settings.divisorIndex = prefs.getUChar("div", 2);
  settings.contrast     = prefs.getUChar("contrast", 255);
  settings.invert       = prefs.getBool("invert", false);
  settings.enabledDivisorMask = prefs.getUChar("divmask", 0xFF);
  settings.standbyEnabled     = prefs.getBool("stbyon", true);
  settings.standbyDelayIndex  = prefs.getUChar("stbydelay", 1); // default 5 min
  settings.customButtonRole   = prefs.getUChar("customrole", CUSTOM_ROLE_SET11);
  settings.enabledTimeSigMask = prefs.getUChar("tsigmask", 0xFF);
  settings.resetInstantMode   = prefs.getBool("rstinstant", false); // default: QUANTIZED
  settings.reset1PlaytimeEnabled = prefs.getBool("r1playtime", true);
  settings.reset2PlaytimeEnabled = prefs.getBool("r2playtime", true);
  prefs.end();

  // Apply loaded values to the active runtime variables
  if (settings.timeSigIndex >= TIME_SIG_COUNT) settings.timeSigIndex = 0;
  if (settings.divisorIndex >= DIVISOR_COUNT)  settings.divisorIndex = 2;
  if (countEnabledDivisor() == 0) settings.enabledDivisorMask = 0xFF;
  if (settings.standbyDelayIndex >= STANDBY_DELAY_COUNT) settings.standbyDelayIndex = 1;
  if (settings.customButtonRole >= CUSTOM_ROLE_COUNT) settings.customButtonRole = CUSTOM_ROLE_SET11;
  if (countEnabledTimeSig() == 0) settings.enabledTimeSigMask = 0xFF; // safety net
  timeSigIndex = settings.timeSigIndex;
  divisorIndex = settings.divisorIndex;
}

void saveSettings() {
  settings.timeSigIndex = timeSigIndex;
  settings.divisorIndex = divisorIndex;
  prefs.begin("midiclock", false);
  prefs.putUChar("tsig", settings.timeSigIndex);
  prefs.putUChar("div", settings.divisorIndex);
  prefs.putUChar("contrast", settings.contrast);
  prefs.putBool("invert", settings.invert);
  prefs.putUChar("divmask", settings.enabledDivisorMask);
  prefs.putBool("stbyon", settings.standbyEnabled);
  prefs.putUChar("stbydelay", settings.standbyDelayIndex);
  prefs.putUChar("customrole", settings.customButtonRole);
  prefs.putUChar("tsigmask", settings.enabledTimeSigMask);
  prefs.putBool("rstinstant", settings.resetInstantMode);
  prefs.putBool("r1playtime", settings.reset1PlaytimeEnabled);
  prefs.putBool("r2playtime", settings.reset2PlaytimeEnabled);
  prefs.end();
}

// Saves only time signature+divisor (fast, called on every normal
// change during operation, so the last state is always kept -
// without writing all other settings on every button press).
void saveQuickState() {
  prefs.begin("midiclock", false);
  prefs.putUChar("tsig", timeSigIndex);
  prefs.putUChar("div", divisorIndex);
  prefs.end();
}

void factoryResetSettings() {
  prefs.begin("midiclock", false);
  prefs.clear();
  prefs.end();
  settings.timeSigIndex = 0;
  settings.divisorIndex = 2;
  settings.contrast     = 255;
  settings.invert       = false;
  settings.enabledDivisorMask = 0xFF;
  settings.standbyEnabled     = true;
  settings.standbyDelayIndex  = 1;
  settings.customButtonRole   = CUSTOM_ROLE_SET11;
  settings.enabledTimeSigMask = 0xFF;
  settings.resetInstantMode   = false;
  settings.reset1PlaytimeEnabled = true;
  settings.reset2PlaytimeEnabled = true;
  timeSigIndex = settings.timeSigIndex;
  divisorIndex = settings.divisorIndex;
}



// Reset request. 0 = no reset registered. 1 = Reset 1 - stays pending
// until the end of the current bar (see handleClock()). 2 = Reset 2 -
// stays pending until the end of the current divisor cycle (see
// handleClock()). Which stage a given hold becomes is decided purely
// by hold duration (short press = 1, held past RESET2_HOLD_MS =
// escalates to 2). Both simply wait for the next boundary of whichever
// grid is currently active - neither one re-anchors anything itself
// (that's what SET 1.1 is for, see triggerSet11()), and both always
// reset the bar counter/grid unconditionally once they fire. Whether
// the elapsed play time also resets is the only optional part, and is
// configured separately per stage (Switches > Reset Switch) - see
// resetPendingResetsTime.
volatile uint8_t resetMode = 0;
// Snapshot of settings.reset1PlaytimeEnabled/reset2PlaytimeEnabled
// (whichever stage is being armed) taken at the moment a reset is
// armed (press for Reset 1, hold-threshold for Reset 2) - stays fixed
// for that action even if the setting is changed later, exactly like
// the Eurorack firmware's resetPendingResetsTime.
volatile bool resetPendingResetsTime = true;

// SET 1.1, QUANTIZED mode only: true while waiting for the nearest
// UPCOMING beat boundary to arrive (only happens when that boundary is
// closer than the one that already passed - see triggerSet11()). At
// most 12 ticks / half a beat of wait, then handleClock() commits it.
bool set11PendingNextBeat = false;

// Timestamp since a reset was registered - acts as a minimum lockout
// against mechanical contact bounce of the footswitch, which would
// otherwise immediately cancel the just-registered reset again as an
// (unwanted) second press.
uint32_t resetRegisteredAtMs = 0;
const uint32_t RESET_DEBOUNCE_CANCEL_MS = 300;

// Instant flash feedback right when a reset is registered/committed -
// blinks briefly 2-3x, independent of the confirm-blink and of when
// the reset is actually (quantized) executed.
volatile bool    resetFlashActive   = false;
volatile uint32_t resetFlashStartMs = 0;
volatile uint8_t resetFlashKind     = 0; // 1 = Reset 1, 2 = Reset 2 (also used for the STOP-mode full reset), 3 = SET 1.1
const uint32_t   RESET_FLASH_TOTAL_MS  = 700; // total duration of the flash animation
const uint32_t   RESET_FLASH_PERIOD_MS = 120; // toggle rate (~3 flashes in 700ms)

// "RESET 1"/"RESET 2"/"SET 1.1" confirmation: blinks exactly 2x in
// time with the beat right after a reset is registered/committed
// (adopted from the Eurorack firmware) - counts real beat pulses
// rather than elapsed time, so it always looks like exactly 2 blinks
// regardless of tempo. Runs independently of whether the reset itself
// is still pending (quantized) or has already executed by the time
// this finishes.
uint8_t confirmResetKind       = 0;     // 1, 2, or 3 while the confirmation is running, otherwise 0
uint8_t confirmResetBlinksLeft = 0;     // how many more "on" phases are left
bool    confirmResetPulsePrev  = false; // edge detection on beatPulseOnNormal

// Time
uint32_t startMillis  = 0;
uint32_t pausedAt     = 0;
bool     timeIsPaused = true;

// ---------------------------------------------------------------------------
// APP MODE: normal operation / settings menu / MIDI analyzer
// ---------------------------------------------------------------------------
enum AppMode { MODE_NORMAL, MODE_ANALYZER, MODE_NUDGE };
int16_t nudgeOffsetSixteenths = 0; // cumulative offset since entering nudge mode, in 1/16-beat steps

// Shifts the current tick position manually by 1/16 (6 ticks), to
// compensate for phase drift of the MIDI clock source relative to the
// actual signal (e.g. turntable) by ear - like a pitch fader,
// just in discrete steps instead of continuously.
void doNudge(int tickDelta) {
  if (!isRunning) return; // nudging only makes sense while the clock is running
  int32_t newTotal = (int32_t)totalTicks + tickDelta;
  if (newTotal < 0) newTotal = 0;
  totalTicks = (uint32_t)newTotal;
  uint32_t ticksPerBarLocal = (uint32_t)beatsPerBar() * 24;
  currentBar  = totalTicks / ticksPerBarLocal;
  uint32_t rest = totalTicks % ticksPerBarLocal;
  currentBeat = rest / 24;
  tickInBeat  = rest % 24;
  nudgeOffsetSixteenths += (tickDelta > 0) ? 1 : -1;
}
AppMode currentMode = MODE_NORMAL;
bool customLongAlreadyHandled = false;

// BPM calculation: averaged over one full beat (24 ticks) instead of
// per individual tick, so that timing variations of individual ticks
// are already averaged out before smoothing (noticeably less jitter).
uint32_t beatStartMicros    = 0;
uint8_t  bpmTickCounter     = 0;
float    bpmFiltered        = 120.0;

// ---------------------------------------------------------------------------
// BUTTON DEBOUNCING
// ---------------------------------------------------------------------------
struct Button {
  uint8_t  pin;
  bool     stableState   = HIGH;
  uint32_t lastChangeMs  = 0;
  uint32_t pressStartMs  = 0;
  bool     isPressed     = false;
};

Button btnDivisor{PIN_BTN_DIVISOR};
Button btnCustom{PIN_BTN_CUSTOM};
Button btnReset{PIN_BTN_RESET};

// Prevents a long press triggered while held from being processed a
// second time on the later release.
bool resetLongAlreadyHandled = false;

const uint32_t DEBOUNCE_MS   = 50;
const uint32_t LONGPRESS_MS  = 1000;  // "medium" threshold (1-3s) - custom button (analyzer toggle) and menu navigation
const uint32_t VERYLONG_MS   = 3000;  // "very long" threshold (>=3s)
const uint32_t RESET2_HOLD_MS = 2000; // Reset button only: hold time to escalate RESET 1 -> RESET 2 (and the sweep bar's full duration)

// Callback type for "pressed briefly" / "held long"
typedef void (*ButtonCallback)(bool longPress);

// Manual prototype: prevents the Arduino IDE from inserting a
// (faulty) prototype at the very top of the file before
// struct Button/ButtonCallback are known there.
void updateButton(Button &b, ButtonCallback onRelease);

// Edge-triggered debounce: the moment the raw reading differs from the
// last accepted state, that's taken as the real edge immediately - no
// "must stay stable for DEBOUNCE_MS first" requirement. Afterwards,
// any further reads are ignored for DEBOUNCE_MS (a blackout window
// that swallows mechanical contact bounce). This fixes short/quick
// button presses being silently dropped entirely: the previous
// "stable-for-DEBOUNCE_MS" approach required the pressed level to
// persist longer than DEBOUNCE_MS before it counted as anything at
// all, so a press-and-release that both happened inside that window
// was invisible - not delayed, just gone, since the level was never
// "stable" for long enough to be promoted to a real transition. Here,
// the edge is accepted right away and only the FOLLOWING transition
// briefly waits out the blackout window if it lands inside it -
// worst case, a very fast release is recognized up to DEBOUNCE_MS
// late, but it's never lost.
void updateButton(Button &b, ButtonCallback onRelease) {
  bool reading = digitalRead(b.pin);

  if (millis() - b.lastChangeMs < DEBOUNCE_MS) return; // still in the post-edge blackout window

  if (reading != b.stableState) {
    b.stableState  = reading;
    b.lastChangeMs = millis(); // starts the blackout window for this new edge
    if (b.stableState == LOW) {
      // button pressed
      b.isPressed    = true;
      b.pressStartMs = millis();
    } else {
      // button released
      if (b.isPressed) {
        uint32_t duration = millis() - b.pressStartMs;
        bool longPress = duration >= LONGPRESS_MS;
        onRelease(longPress);
      }
      b.isPressed = false;
    }
  }
}

// ---------------------------------------------------------------------------
// button-CALLBACKS
// ---------------------------------------------------------------------------
void onDivisorButton(bool longPress) {
  if (suppressDivisorAction) { suppressDivisorAction = false; return; } // was only a wake-up press
  if (currentMode != MODE_NORMAL) return; // no accidental change in nudge mode/analyzer (nudge runs via its own logic in loop())
  divisorIndex = nextEnabledDivisor(divisorIndex);
  saveQuickState(); // save the latest state immediately, kept after restart
}

// Custom button — freely assignable via Switches > Custom (default
// role: SET 1.1). Dispatches to whichever function is currently
// assigned; see triggerSet11() below for the SET 1.1 role.
void onCustomButton(bool longPress) {
  // If the long press already toggled the analyzer while held,
  // do nothing more here (prevents a double action / an accidental
  // further cycling of the time signature on release).
  if (customLongAlreadyHandled) {
    customLongAlreadyHandled = false;
    return;
  }
  if (suppressCustomAction) { suppressCustomAction = false; return; } // was only a wake-up press
  if (currentMode != MODE_NORMAL) return; // no accidental change in nudge mode/analyzer

  switch (settings.customButtonRole) {
    case CUSTOM_ROLE_TIMESIG:
      timeSigIndex = nextEnabledTimeSig(timeSigIndex);
      saveQuickState(); // save the latest state immediately, kept after restart
      break;
    case CUSTOM_ROLE_SET11: triggerSet11(); break;
  }
}

// Called once the custom button has been held for 1s without
// release -> toggles between normal operation and MIDI analyzer (in
// both directions with the same button press). Independent of the
// role above - available no matter what the custom button's short
// press is currently assigned to.
void onCustomButtonLongHeldDuringPress() {
  if (suppressCustomAction) return; // wake-up press does not trigger a long-press effect
  if (currentMode == MODE_NUDGE) return; // no analyzer toggle in nudge mode
  currentMode = (currentMode == MODE_NORMAL) ? MODE_ANALYZER : MODE_NORMAL;
}

// Checks whether time, bar counter, or divisor cycle have any value
// different from zero at all - relevant for the reset button in STOP
// mode (a quantized reset makes no sense there since no clock is
// running; instead it's checked directly whether there's anything at
// all to reset).
bool hasSomethingToReset() {
  uint32_t elapsedMs = timeIsPaused ? (pausedAt - startMillis) : (millis() - startMillis);
  return (currentBar != 0) || (totalTicks != 0) || (elapsedMs != 0);
}

// Starts the beat-synced "RESET 1"/"RESET 2"/"SET 1.1" confirmation
// blink (see confirmResetKind and render()) and the short instant
// flash - called from every place that registers or commits a reset.
void startResetConfirm(uint8_t kind) {
  confirmResetKind       = kind;
  confirmResetBlinksLeft = 2;
  confirmResetPulsePrev  = false;
  resetFlashActive  = true;
  resetFlashStartMs = millis();
  resetFlashKind    = kind;
}

// SET 1.1 - a standalone, momentary function (Custom button role only,
// no physical-button hold gesture): immediately (or after at most half
// a beat, see QUANTIZED below) establishes a brand new beat-grid
// anchor, "spinning up" a fresh 1.1, and always resets everything
// (bar, beat, tick, and elapsed play time) - unlike Reset 1/2
// (Switches > Reset Switch), this isn't configurable per item, since
// by definition it always starts a clean new grid+timer. Reset 1 and
// Reset 2 keep operating normally afterwards, within whatever grid
// results from this - they never re-anchor anything themselves, they
// just wait for the next bar/cycle boundary of whichever grid is
// currently active.
// - QUANTIZED (Switches > Custom > Function=SET 1.1 > Mode, default):
//   rounds the new anchor to whichever already-established beat
//   boundary is CLOSEST - the one that just passed, or the upcoming
//   one, whichever is nearer. Rounding to a boundary that already
//   passed commits immediately; rounding to the upcoming one means
//   waiting for it first (at most 12 ticks / half a beat - see
//   set11PendingNextBeat/handleClock()). Either way, this realigns our
//   count with the sequencer's own beat pattern without touching its
//   underlying phase at all.
// - INSTANT: uses the exact current tick as-is, no rounding, always
//   commits immediately - spins up a brand new phase, disconnected
//   from whatever grid existed before.
void commitSet11(uint32_t anchor) {
  uint32_t elapsed = totalTicks - anchor;
  totalTicks = elapsed;
  uint32_t ticksPerBarLocal = (uint32_t)beatsPerBar() * 24;
  currentBar  = totalTicks / ticksPerBarLocal;
  uint32_t rest = totalTicks % ticksPerBarLocal;
  currentBeat = rest / 24;
  tickInBeat  = rest % 24;
  startMillis = millis();
}

void triggerSet11() {
  if (!isRunning) {
    // STOP mode: no running clock to anchor against, so a full reset
    // is executed immediately, same as the other reset paths in this
    // case.
    if (hasSomethingToReset()) {
      currentBar  = 0;
      currentBeat = 0;
      tickInBeat  = 0;
      totalTicks  = 0;
      startMillis = millis();
      pausedAt    = startMillis;
      startResetConfirm(3);
    }
    return;
  }

  if (set11PendingNextBeat) {
    // Already waiting for the upcoming beat (see below) - pressing
    // again cancels it, same idea as Reset 1/2's cancel-on-second-press.
    set11PendingNextBeat = false;
    return;
  }

  // A newly-spun-up grid makes any pending Reset 1/2 (waiting on the
  // OLD grid's bar/cycle end) meaningless - discard it.
  resetMode = 0;

  if (settings.resetInstantMode) {
    commitSet11(totalTicks); // INSTANT: exact raw tick, new phase, no rounding
    startResetConfirm(3);
    return;
  }

  // QUANTIZED: round to the NEAREST beat boundary, not always down.
  uint32_t remainder = totalTicks % 24;
  if (remainder <= 12) {
    // Closer to (or exactly at) the beat that just started - that
    // boundary is already in the past, so commit to it right now.
    commitSet11(totalTicks - remainder);
    startResetConfirm(3);
  } else {
    // Closer to the upcoming beat instead - that boundary hasn't
    // happened yet, so there's nothing to "round down" to; wait the
    // short remainder (at most 12 ticks) and commit once it arrives
    // (see handleClock()). The confirmation blink starts now, same as
    // Reset 1/2 do when they're first armed.
    set11PendingNextBeat = true;
    startResetConfirm(3);
  }
}

void onResetButton(bool longPress) {
  // If the press was already handled while held (escalated to Reset
  // 2, see onResetMediumHeldDuringPress()), it stays pending - Reset 2
  // always waits for the natural end of the divisor cycle. Just start
  // the confirmation blink for it now.
  if (resetLongAlreadyHandled) {
    resetLongAlreadyHandled = false;
    if (resetMode == 2) startResetConfirm(2);
    return;
  }
  if (suppressResetAction) { suppressResetAction = false; return; } // was only a wake-up press

  if (!isRunning) {
    // In STOP mode no clock is running, so a quantized reset makes no
    // sense here. Instead: if there's anything at all to reset
    // (time/bar/divisor cycle), a full reset is executed immediately
    // on any button press (no holding needed) - the bar counter/grid
    // always resets, the elapsed play time only if Reset 1's Playtime
    // setting is on (there's no stage distinction in STOP mode, so
    // Reset 1's setting is used as the reference here). If everything
    // relevant is already at zero, nothing happens (see "NOTHING TO
    // RESET" in the G display).
    if (hasSomethingToReset()) {
      currentBar  = 0;
      currentBeat = 0; // so E (beat bar) is also reset
      tickInBeat  = 0;
      totalTicks  = 0;
      if (settings.reset1PlaytimeEnabled) {
        startMillis = millis();
        pausedAt    = startMillis; // stays paused (STOP), but time shows 00:00
      }
      startResetConfirm(2);
    }
    return;
  }

  // Short press (not escalated): arms Reset 1 for the end of the
  // current bar; a second press while one is already pending cancels
  // it instead.
  if (resetMode != 0) {
    if (millis() - resetRegisteredAtMs < RESET_DEBOUNCE_CANCEL_MS) {
      return; // likely contact bounce, ignore
    }
    resetMode = 0;
    return;
  }

  resetMode = 1;
  resetPendingResetsTime = settings.reset1PlaytimeEnabled;
  resetRegisteredAtMs = millis();
  startResetConfirm(1);
}

// Called once the reset button has been held past RESET2_HOLD_MS (not
// just on release) - arms Reset 2 while the button is still held; a
// still-pending registration at this point is taken back instead
// (renewed press = cancel). Neither stage commits here - both only
// ever execute later, in handleClock(), once the actual bar/cycle
// boundary is reached.
void onResetMediumHeldDuringPress() {
  if (suppressResetAction) return; // wake-up press does not trigger an effect
  if (!isRunning) return; // nothing to reset in STOP mode

  if (resetMode == 0) {
    resetMode = 2;
    resetPendingResetsTime = settings.reset2PlaytimeEnabled;
    resetRegisteredAtMs = millis();
  } else {
    resetMode = 0;
  }
}

// TEMPORARY DIAGNOSTIC: measures how long the last render() /
// renderAnalyzer() call (including the SPI framebuffer push) actually
// took, to empirically check whether a slow render is really what's
// delaying MIDI byte processing and polluting the analyzer's own
// jitter measurement - rather than continuing to guess. maxRenderUs
// resets every 2s so it reflects a recent worst-case, not an
// ever-growing all-time record. Shown on the Analyzer screen; remove
// once the root cause is confirmed. Declared here (rather than next
// to its actual use in loop(), near the bottom of the file) because
// renderAnalyzer() reads it and is defined much earlier - plain global
// variables need to be declared before their point of use in the
// file, unlike functions which Arduino auto-prototypes.
volatile uint32_t lastRenderUs    = 0;
volatile uint32_t maxRenderUs     = 0;
uint32_t maxRenderWindowStartMs   = 0;

// ---------------------------------------------------------------------------
// MIDI ANALYZER: ring buffer of the most recent tick intervals for jitter/stability
// ---------------------------------------------------------------------------
#define ANALYZER_HISTORY_SIZE 96 // = 4 Beats with 24 PPQN
volatile uint32_t tickIntervalHistory[ANALYZER_HISTORY_SIZE];
volatile uint8_t  analyzerHistIndex = 0;
volatile bool     analyzerHistFull  = false;
volatile uint32_t lastTickMicrosForAnalyzer = 0;

// Plausible tick-interval bounds @ 24 PPQN, matching the same 20-400 BPM
// "realistic tempo" window already used for the main BPM smoothing in
// handleClock() - used to keep glitch intervals out of the analyzer
// ring buffer (see handleClock() below).
#define ANALYZER_MIN_TICK_US 6250UL   // 400 BPM
#define ANALYZER_MAX_TICK_US 125000UL // 20 BPM

// MIDI RUN/STOP state history: samples isRunning (derived from the MIDI
// Start/Stop/Continue messages - see handleStart()/handleStop()/
// handleContinue() below) every MIDI_RUNSTOP_SAMPLE_INTERVAL_MS into a
// ring buffer, so the analyzer can draw a run/stop level trace over time.
// This is the MIDI-only equivalent of the CV RUN/STOP trace on the
// Eurorack variant (this board has no CV inputs).
#define MIDI_RUNSTOP_HISTORY_SIZE 60 // 60 * 200ms = 12s window
#define MIDI_RUNSTOP_SAMPLE_INTERVAL_MS 200
volatile bool     midiRunStopHistory[MIDI_RUNSTOP_HISTORY_SIZE];
volatile uint8_t  midiRunStopHistIndex = 0;
volatile bool     midiRunStopHistFull  = false;
uint32_t lastMidiRunStopSampleMs = 0;

// Peak-hold scale for the analyzer's tick-deviation ruler: locks onto
// the largest deviation actually seen and only relaxes to the next
// largest once RULER_PEAK_HOLD_BEATS beats have passed without seeing
// a deviation at least that large again. Everything here is driven
// purely by received ticks (not by isRunning/Start-Stop), matching
// "the analyzer processes what's received, not our own grid".
#define RULER_PEAK_HOLD_BEATS 4
volatile uint32_t analyzerRunningAvgUs   = 0;    // continuously-updated reference average, separate from the render-time percentile average
volatile bool     analyzerRunningAvgInit = false;
volatile uint32_t analyzerBeatCounter    = 0;    // increments every 24 valid analyzer ticks
volatile uint8_t  analyzerBeatTickCount  = 0;    // 0..23 toward the next beat
volatile uint32_t rulerLockedScaleUs     = 1000; // current locked ruler range (raw us, before display padding/rounding)
volatile uint32_t rulerLastConfirmBeat   = 0;    // beat number the locked scale was last matched or exceeded at

// Anchor for beat extrapolation: timestamp of the last REAL tick.
// Between two ticks, the display (E bar, beat blink) is smoothly
// extrapolated based on this, instead of visibly "jumping" with every
// small jitter of the source - the anchor is reset on every real tick,
// so a permanent drift can never occur.
volatile uint32_t lastTickAnchorMicros = 0;

// ---------------------------------------------------------------------------
// MIDI RX TIMESTAMP QUEUE
//
// Problem this solves: handleClock() used to call micros() itself,
// i.e. it timestamped "when loop() got around to calling MIDI.read()",
// not "when the byte actually arrived on the wire". If loop() was busy
// (most notably renderAnalyzer()'s SPI framebuffer push, several ms),
// an incoming clock byte sat in the UART's hardware FIFO the whole
// time and got timestamped late - the Analyzer was partly measuring
// its own render time, not the real MIDI clock.
//
// Fix: MidiSerial.onReceive() (see setup()) fires from the ESP32
// core's own UART event task, independent of whether loop() is
// currently blocked - it just records micros() and how many bytes
// just arrived, pushing one timestamp per byte into this ring buffer.
// handleClock()/handleStart()/handleStop()/handleContinue() then pop
// their timestamp from here instead of calling micros() fresh. If the
// queue is ever empty (e.g. onReceive unsupported on an older core),
// popMidiRxTimestampOr() falls back to a fresh micros() call - same
// behavior as before, never a crash or a wrong value.
//
// This queue is genuinely written from two different execution
// contexts now (the UART event task vs. the main loop() task), unlike
// the rest of this file's shared state which is only ever touched
// from loop()'s own call chain. On the ESP32's dual cores, the
// classic Arduino noInterrupts()/interrupts() only affects the
// current core and is not sufficient here - a real portMUX_TYPE
// spinlock (portENTER_CRITICAL/portEXIT_CRITICAL) is used instead.
// ---------------------------------------------------------------------------
#define MIDI_RX_TS_QUEUE_SIZE 32
volatile uint32_t midiRxTsQueue[MIDI_RX_TS_QUEUE_SIZE];
volatile uint8_t  midiRxTsHead  = 0; // next slot to write (onReceive side)
volatile uint8_t  midiRxTsTail  = 0; // next slot to read (loop()/handleClock() side)
volatile uint8_t  midiRxTsCount = 0;
portMUX_TYPE midiRxTsMux = portMUX_INITIALIZER_UNLOCKED;

// Called from the UART event task (see MidiSerial.onReceive() in
// setup()) - keep this fast and simple, no floating point, no loops
// over the analyzer history, nothing that could itself introduce a
// stall. Only pushes timestamps; all the real work still happens in
// handleClock() etc. as before, just using an earlier timestamp.
void onMidiSerialReceive() {
  uint32_t nowUs = micros();
  int avail = MidiSerial.available();
  static int lastAvail = 0;
  int newBytes = avail - lastAvail;
  // available() can also drop between calls (loop() consumed bytes
  // via MIDI.read() in the meantime) - only newly arrived bytes matter
  // here, a negative or implausible delta just means "resync, no new
  // bytes to timestamp this time".
  if (newBytes > 0 && newBytes <= MIDI_RX_TS_QUEUE_SIZE) {
    portENTER_CRITICAL(&midiRxTsMux);
    for (int i = 0; i < newBytes; i++) {
      if (midiRxTsCount >= MIDI_RX_TS_QUEUE_SIZE) {
        // Queue full (shouldn't normally happen at MIDI clock rates) -
        // drop the oldest entry so we always keep the most recent
        // timestamps rather than getting stuck behind stale ones.
        midiRxTsTail = (midiRxTsTail + 1) % MIDI_RX_TS_QUEUE_SIZE;
        midiRxTsCount--;
      }
      midiRxTsQueue[midiRxTsHead] = nowUs;
      midiRxTsHead = (midiRxTsHead + 1) % MIDI_RX_TS_QUEUE_SIZE;
      midiRxTsCount++;
    }
    portEXIT_CRITICAL(&midiRxTsMux);
  }
  lastAvail = avail;
}

// Pops the oldest queued RX timestamp, or returns fallbackUs if the
// queue is empty (e.g. onReceive() isn't supported on this core
// version, or this specific byte's event was missed for some reason -
// degrades gracefully to the old "timestamp at processing time"
// behavior rather than ever returning garbage).
uint32_t popMidiRxTimestampOr(uint32_t fallbackUs) {
  uint32_t ts = fallbackUs;
  portENTER_CRITICAL(&midiRxTsMux);
  if (midiRxTsCount > 0) {
    ts = midiRxTsQueue[midiRxTsTail];
    midiRxTsTail = (midiRxTsTail + 1) % MIDI_RX_TS_QUEUE_SIZE;
    midiRxTsCount--;
  }
  portEXIT_CRITICAL(&midiRxTsMux);
  return ts;
}

// ---------------------------------------------------------------------------
// MIDI CALLBACKS
// ---------------------------------------------------------------------------
void handleClock() {
  lastClockTickMillis = millis(); // watchdog: "clock is still alive"

  // The actual fix: use the timestamp captured when this byte really
  // arrived (see onMidiSerialReceive()), not "whenever loop() got
  // around to calling MIDI.read()". Falls back to a fresh micros() if
  // the queue is empty for any reason - same as the old behavior.
  uint32_t nowMicrosForAnchor = popMidiRxTimestampOr(micros());
  lastTickAnchorMicros = nowMicrosForAnchor; // reset anchor for extrapolation

  // --- Analyzer: write tick interval into the ring buffer ---
  // Same "realistic tempo" plausibility bound as the BPM smoothing
  // below (20-400 BPM @ 24 PPQN) - reused here so a single glitch
  // interval (double-tick burst, a stray/delayed byte, a rapid tempo
  // jump on the source) never enters the analyzer history at all.
  // Without this, one such outlier not only skewed the graph (it's
  // part of the plain average used as its centerline, unlike the
  // jitter range below which is already percentile-trimmed) but could
  // also turn into an implausibly high "BPM" in the Analyzer's
  // min/max readout, overflowing the small dtostrf buffers there and
  // crashing the board.
  uint32_t nowMicrosA = nowMicrosForAnchor;
  if (lastTickMicrosForAnalyzer != 0) {
    uint32_t ivUs = nowMicrosA - lastTickMicrosForAnalyzer;
    if (ivUs >= ANALYZER_MIN_TICK_US && ivUs <= ANALYZER_MAX_TICK_US) {
      tickIntervalHistory[analyzerHistIndex] = ivUs;
      analyzerHistIndex++;
      if (analyzerHistIndex >= ANALYZER_HISTORY_SIZE) {
        analyzerHistIndex = 0;
        analyzerHistFull = true;
      }

      // --- Ruler peak-hold: continuously-updated reference average,
      // independent of the render-time percentile average, so the
      // scale-lock logic below works the same regardless of whether
      // the analyzer screen is even being looked at right now.
      if (!analyzerRunningAvgInit) {
        analyzerRunningAvgUs = ivUs;
        analyzerRunningAvgInit = true;
      } else {
        analyzerRunningAvgUs = (analyzerRunningAvgUs * 7 + ivUs) / 8;
      }
      uint32_t devNow = (ivUs > analyzerRunningAvgUs) ? (ivUs - analyzerRunningAvgUs) : (analyzerRunningAvgUs - ivUs);

      analyzerBeatTickCount++;
      if (analyzerBeatTickCount >= 24) { analyzerBeatTickCount = 0; analyzerBeatCounter++; }

      if (devNow >= rulerLockedScaleUs) {
        // New (or re-confirmed) peak - lock onto it and reset the hold timer.
        rulerLockedScaleUs = devNow;
        rulerLastConfirmBeat = analyzerBeatCounter;
      } else if ((analyzerBeatCounter - rulerLastConfirmBeat) >= RULER_PEAK_HOLD_BEATS) {
        // The locked peak hasn't been matched or exceeded in
        // RULER_PEAK_HOLD_BEATS beats - it's aged out of the visible
        // 4-beat window by now anyway, so drop down to whatever the
        // largest deviation still actually present in that window is.
        uint8_t validCount = analyzerHistFull ? ANALYZER_HISTORY_SIZE : analyzerHistIndex;
        uint32_t nextMax = 0;
        for (uint8_t k = 0; k < validCount; k++) {
          uint32_t vv = tickIntervalHistory[k];
          uint32_t dd = (vv > analyzerRunningAvgUs) ? (vv - analyzerRunningAvgUs) : (analyzerRunningAvgUs - vv);
          if (dd > nextMax) nextMax = dd;
        }
        if (nextMax < 1000) nextMax = 1000; // 1ms floor, matches the display floor
        rulerLockedScaleUs = nextMax;
        rulerLastConfirmBeat = analyzerBeatCounter;
      }
    }
    // else: implausible interval - discarded, ring buffer keeps its
    // last valid contents instead of being contaminated by a glitch.
  }
  lastTickMicrosForAnalyzer = nowMicrosA;

  // --- BPM calculation: once per full beat (24 ticks) ---
  uint32_t nowMicros = nowMicrosForAnchor;
  bpmTickCounter++;
  if (bpmTickCounter >= 24) {
    if (beatStartMicros != 0) {
      uint32_t beatMicros = nowMicros - beatStartMicros;
      if (beatMicros > 0) {
        float instBpm = 60.0f * 1000000.0f / (float)beatMicros;
        // Plausibility check: realistic tempi are roughly between
        // 20 and 400 BPM. Values outside that almost always result
        // from ticks reprocessed in bursts (e.g. after a mode that
        // polled MIDI less often) rather than a real tempo change -
        // we discard such outliers instead of letting them feed into
        // the smoothing (otherwise a brief visible spike would occur).
        if (instBpm >= 20.0f && instBpm <= 400.0f) {
          // Stronger smoothing, since this now only updates once per
          // beat (instead of 24x per beat as before) -> noticeably
          // calmer display.
          bpmFiltered = bpmFiltered * 0.8f + instBpm * 0.2f;
        }
      }
    }
    beatStartMicros = nowMicros;
    bpmTickCounter = 0;
  }

  if (!isRunning) return;

  // --- Tick/Beat/Bar-counting ---
  tickInBeat++;
  totalTicks++;

  if (tickInBeat >= 24) {
    tickInBeat = 0;
    currentBeat++;
    if (currentBeat >= beatsPerBar()) {
      currentBeat = 0;
      currentBar++;
    }
  }

  // --- Quantized reset: Reset 1 (bar end) / Reset 2 (cycle end) ---
  // Neither stage re-anchors anything - they both just wait for the
  // next boundary of whichever grid is currently active (the original
  // one from MIDI Start, or a new one established by SET 1.1, see
  // triggerSet11()). Not affected by Switches > Custom > Mode at all -
  // that setting only concerns SET 1.1's own anchor. The bar counter/
  // grid always resets unconditionally; only the elapsed play time is
  // optional, decided by resetPendingResetsTime (captured per-stage
  // when the reset was armed - Switches > Reset Switch > Reset 1/2).
  if (resetMode == 1) {
    uint32_t ticksPerBar = (uint32_t)beatsPerBar() * 24;
    if ((totalTicks % ticksPerBar) == 0) {
      currentBar = 0;
      totalTicks = 0; // so F (divisor bar) starts over
      if (resetPendingResetsTime) startMillis = millis();
      resetMode = 0;
    }
  } else if (resetMode == 2) {
    uint32_t ticksPerCycle = (uint32_t)divisor() * beatsPerBar() * 24;
    if ((totalTicks % ticksPerCycle) == 0) {
      currentBar = 0;
      if (resetPendingResetsTime) startMillis = millis();
      resetMode = 0;
    }
  }

  // --- SET 1.1, QUANTIZED mode: commit once the nearer, upcoming beat
  // boundary we're waiting for finally arrives (see triggerSet11()).
  // Independent of resetMode/Reset 1/2 above - can be pending at the
  // same time as either of those (SET 1.1 already cleared them when it
  // was triggered, so in practice this only ever fires on its own).
  if (set11PendingNextBeat && (totalTicks % 24) == 0) {
    commitSet11(totalTicks);
    set11PendingNextBeat = false;
  }
}

void handleStart() {
  popMidiRxTimestampOr(0); // keep the RX timestamp queue in sync - this byte consumed one slot too, even though we don't need its value here
  totalTicks   = 0;
  tickInBeat   = 0;
  currentBeat  = 0;
  currentBar   = 0;
  isRunning    = true;
  resetMode    = 0;
  startMillis  = millis();
  timeIsPaused = false;
  beatStartMicros = 0; // prevents a false BPM outlier after stop/start
  bpmTickCounter  = 0;
  lastClockTickMillis = millis(); // restart watchdog
}

void handleStop() {
  popMidiRxTimestampOr(0); // keep the RX timestamp queue in sync, see handleStart()
  isRunning    = false;
  pausedAt     = millis();
  timeIsPaused = true;
}

void handleContinue() {
  popMidiRxTimestampOr(0); // keep the RX timestamp queue in sync, see handleStart()
  isRunning = true;
  if (timeIsPaused) {
    uint32_t pauseDuration = millis() - pausedAt;
    startMillis += pauseDuration; // subtract out the pause duration
    timeIsPaused = false;
  }
  lastClockTickMillis = millis(); // restart watchdog
}

// Called when no MIDI clock tick has been received for 5s, even
// though the clock was considered "running" (e.g. cable unplugged,
// source stopped without sending a Stop message). Resets everything
// and goes to STOP.
void handleClockLost() {
  isRunning    = false;
  totalTicks   = 0;
  tickInBeat   = 0;
  currentBeat  = 0;
  currentBar   = 0;
  resetMode    = 0;
  resetFlashActive = false;
  startMillis  = millis();
  pausedAt     = startMillis;
  timeIsPaused = true;
  bpmTickCounter  = 0;
  beatStartMicros = 0;
}

// ---------------------------------------------------------------------------
// RENDERING
// ---------------------------------------------------------------------------
void updateBlinkStates() {
  // No time-based blink states needed anymore - C and G now blink
  // beat-synchronously via the MIDI clock ticks (see render()), B is
  // permanently visible.
}

void formatTime(char *buf, uint32_t ms) {
  uint32_t totalSeconds = ms / 1000;
  uint32_t mm = (totalSeconds / 60) % 100; // limited to 2 digits
  uint32_t ss = totalSeconds % 60;
  sprintf(buf, "%02u:%02u", mm, ss);
}

// Simulates "dimming" on the monochrome display via a dot pattern
// (checkerboard, every other pixel) instead of a solid area - this
// looks lighter/less obtrusive than a full white box.
void drawDitheredBox(int x, int y, int w, int h) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      if (((xx + yy) % 2) == 0) {
        u8g2.drawPixel(xx, yy);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// MIDI ANALYZER DISPLAY (custom button 1s hold to toggle on/off)
// ---------------------------------------------------------------------------
void renderAnalyzer() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_4x6_tf); // smaller than the rest of the UI, needed to fit the graph + run/stop trace

  u8g2.drawStr(1, 6, "MIDI CLOCK ANALYZER");

  noInterrupts();
  uint8_t  histCount   = analyzerHistFull ? ANALYZER_HISTORY_SIZE : analyzerHistIndex;
  uint8_t  histIdxSnap = analyzerHistIndex; // frozen snapshot: a tick arriving mid-render must not
                                             // point at a different slot than the localHist copy below
  uint32_t localHist[ANALYZER_HISTORY_SIZE];
  for (uint8_t i = 0; i < histCount; i++) localHist[i] = tickIntervalHistory[i];
  uint32_t lastTickMs  = lastClockTickMillis;
  bool     runningSnap = isRunning;
  uint32_t avgRefSnap        = analyzerRunningAvgUs;
  uint32_t lockedScaleSnap   = rulerLockedScaleUs;
  uint8_t  rsIdxSnap   = midiRunStopHistIndex;
  bool     rsHistSnap[MIDI_RUNSTOP_HISTORY_SIZE];
  for (uint8_t i = 0; i < MIDI_RUNSTOP_HISTORY_SIZE; i++) rsHistSnap[i] = midiRunStopHistory[i];
  interrupts();

  // NOTE: all text below uses snprintf(..., sizeof(line), ...) rather
  // than sprintf() - sprintf() has no idea how big "line" is and will
  // happily write past its end if a formatted value ever turns out
  // longer than expected (that's what crashed the board before: two
  // BPM figures and a jitter percentage combined into one sprintf()
  // could exceed the buffer in an edge case). Keeping each line short
  // AND using snprintf is belt-and-suspenders: a display glitch is
  // now the worst case, never a crash.
  char line[32];

  // Status top right: clock currently running / how long since nothing more
  uint32_t sinceLastTick = millis() - lastTickMs;
  char statusBuf[16];
  if (!runningSnap) {
    snprintf(statusBuf, sizeof(statusBuf), "STOP");
  } else if (sinceLastTick > 999) {
    snprintf(statusBuf, sizeof(statusBuf), "%lus", (unsigned long)(sinceLastTick / 1000));
  } else {
    snprintf(statusBuf, sizeof(statusBuf), "OK");
  }
  int stW = u8g2.getStrWidth(statusBuf);
  u8g2.drawStr(126 - stW, 6, statusBuf);

  if (histCount < 2) {
    u8g2.drawStr(1, 16, "Waiting for clock...");
  } else {
    // Robust min/max detection via 5%/95% percentile instead of
    // absolute min/max, so that individual outliers (e.g. from brief
    // processing delays in our own code) don't distort the display.
    uint32_t sorted[ANALYZER_HISTORY_SIZE];
    for (uint8_t i = 0; i < histCount; i++) sorted[i] = localHist[i];
    // simple insertion sort, histCount <= 96 -> plenty fast enough
    for (uint8_t i = 1; i < histCount; i++) {
      uint32_t key = sorted[i];
      int8_t j = i - 1;
      while (j >= 0 && sorted[j] > key) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = key;
    }
    uint8_t p05idx = (histCount * 5) / 100;
    uint8_t p95idx = (histCount * 95) / 100;
    if (p95idx >= histCount) p95idx = histCount - 1;
    uint32_t minIv = sorted[p05idx];
    uint32_t maxIv = sorted[p95idx];

    // Trimmed mean (same P5/P95 window as minIv/maxIv above) instead of
    // a plain average of all samples, so a lingering outlier can't
    // skew the graph's centerline off to one side.
    uint32_t sumIv = 0;
    for (uint8_t i = p05idx; i <= p95idx; i++) sumIv += sorted[i];
    uint32_t avgIv = sumIv / (p95idx - p05idx + 1);
    uint32_t jitter = (maxIv > minIv) ? (maxIv - minIv) : 0;
    uint32_t jitterPct = (avgIv > 0) ? (jitter * 100 / avgIv) : 0;
    if (jitterPct > 999) jitterPct = 999; // defensive cap, keeps the string length bounded

    // most recent interval = the one directly before the current write index
    uint32_t curIv = localHist[(histIdxSnap + ANALYZER_HISTORY_SIZE - 1) % ANALYZER_HISTORY_SIZE];

    float bpmMin = (maxIv > 0) ? (60000000.0f / ((float)maxIv * 24.0f)) : 0;
    float bpmMax = (minIv > 0) ? (60000000.0f / ((float)minIv * 24.0f)) : 0;
    // Defensive clamp: handleClock() already keeps implausible tick
    // intervals out of the ring buffer, so this should never trigger -
    // but capping here too means a stray edge case can only ever
    // produce "999.9" instead of a runaway string.
    if (bpmMin > 999.9f) bpmMin = 999.9f;
    if (bpmMax > 999.9f) bpmMax = 999.9f;

    char bpmMinBuf[10], bpmMaxBuf[10]; // sized for "999.9"+NUL with headroom
    dtostrf(bpmMin, 3, 1, bpmMinBuf);
    dtostrf(bpmMax, 3, 1, bpmMaxBuf);
    snprintf(line, sizeof(line), "IV %lu.%01lums  BPM %s-%s",
             (unsigned long)(curIv / 1000), (unsigned long)((curIv / 100) % 10), bpmMinBuf, bpmMaxBuf);
    u8g2.drawStr(1, 13, line);

    // TEMPORARY DIAGNOSTIC: showing max render duration (last 2s
    // window) here instead of the 16th-note jitter %, to check whether
    // a slow render() is really what's delaying MIDI reads and
    // polluting this very readout. Revert once confirmed either way.
    uint32_t maxRenderMsX10 = (maxRenderUs * 10) / 1000; // e.g. 23 -> 2.3ms
    snprintf(line, sizeof(line), "JIT +-%lu.%01lums (%lu%%) Rmax %lu.%01lums",
             (unsigned long)(jitter / 1000), (unsigned long)((jitter / 100) % 10),
             (unsigned long)jitterPct,
             (unsigned long)(maxRenderMsX10 / 10), (unsigned long)(maxRenderMsX10 % 10));
    u8g2.drawStr(1, 20, line);

    const uint8_t RULER_N = 24;
    uint8_t n = histCount < RULER_N ? histCount : RULER_N;
    int32_t devUs[RULER_N];
    for (uint8_t i = 0; i < n; i++) {
      uint8_t idxr = (histIdxSnap + ANALYZER_HISTORY_SIZE - 1 - i) % ANALYZER_HISTORY_SIZE;
      devUs[i] = (int32_t)localHist[idxr] - (int32_t)avgRefSnap;
    }

    // Scale: the persistent peak-hold value (see handleClock()) rather
    // than just this beat's own max, so a big outlier keeps the ruler
    // "zoomed out" for at least RULER_PEAK_HOLD_BEATS beats even after
    // it's a beat or two in the past, instead of the scale snapping
    // back in immediately and hiding how bad things briefly got.
    // +20% headroom so nothing sits exactly on the edge, then rounded
    // UP to a whole millisecond so the grid below lines up cleanly.
    int32_t rangeUs = (int32_t)lockedScaleSnap;
    rangeUs = rangeUs + rangeUs / 5;
    if (rangeUs < 1000) rangeUs = 1000;
    rangeUs = ((rangeUs + 999) / 1000) * 1000;
    int32_t rangeMs = rangeUs / 1000;

    const int axisX0 = 8, axisX1 = 120, axisY = 30, halfW = (axisX1 - axisX0) / 2;
    const int axisCx = axisX0 + halfW;
    u8g2.drawHLine(axisX0, axisY, axisX1 - axisX0);
    // One gridline per whole millisecond on each side (e.g. a 6ms range
    // draws 5 gridlines between 0 and 6, matching the ms markings a
    // ruler would actually have) instead of an arbitrary fixed count.
    for (int32_t m = 1; m < rangeMs; m++) {
      int gxPos = axisCx + (int)((m * (int32_t)halfW) / rangeMs);
      int gxNeg = axisCx - (int)((m * (int32_t)halfW) / rangeMs);
      u8g2.drawVLine(gxPos, axisY + 1, 1);
      u8g2.drawVLine(gxNeg, axisY + 1, 1);
    }

    // Sort ascending by deviation first, so ticks that land on the same
    // (or an adjacent) pixel get dodged outward from smallest to
    // largest in a consistent, symmetric-looking order rather than
    // whatever order they happened to be found in the ring buffer.
    for (uint8_t i = 1; i < n; i++) {
      int32_t key = devUs[i];
      int8_t j = i - 1;
      while (j >= 0 && devUs[j] > key) {
        devUs[j + 1] = devUs[j];
        j--;
      }
      devUs[j + 1] = key;
    }

    // Anti-overlap: two ticks are never drawn on top of each other. If
    // a pixel is already taken, nudge outward (checking +1, -1, +2,
    // -2, ... from the wanted spot) to the nearest free one instead -
    // "rather show it somewhere else" than let it disappear into
    // another mark. With up to 24 marks on ~112px this triggers more
    // often than before, but always finds room.
    bool usedX[128];
    for (int k = axisX0; k <= axisX1; k++) usedX[k] = false;
    for (uint8_t i = 0; i < n; i++) {
      int xr = axisCx + (int)((devUs[i] * (int32_t)halfW) / rangeUs);
      if (xr < axisX0) xr = axisX0;
      if (xr > axisX1) xr = axisX1;
      int finalX = xr;
      if (usedX[finalX]) {
        for (int step = 1; step <= (axisX1 - axisX0); step++) {
          int tryPos1 = xr + step;
          int tryPos2 = xr - step;
          if (tryPos1 <= axisX1 && !usedX[tryPos1]) { finalX = tryPos1; break; }
          if (tryPos2 >= axisX0 && !usedX[tryPos2]) { finalX = tryPos2; break; }
        }
      }
      usedX[finalX] = true;
      u8g2.drawVLine(finalX, axisY - 8, 8); // the tick itself, sitting right on the ruler
    }

    snprintf(line, sizeof(line), "-%ldms", (long)rangeMs);
    u8g2.drawStr(axisX0 - 4, 38, line);
    u8g2.drawStr(axisCx - 2, 38, "0");
    snprintf(line, sizeof(line), "+%ldms", (long)rangeMs);
    int rw = u8g2.getStrWidth(line);
    u8g2.drawStr(axisX1 - rw + 4, 38, line);

    // =======================================================================
    // 4-BEAT TIMELINE: a chart-recorder-style trace below the ms ruler -
    // running left (oldest) to right (newest) across the full
    // 4-beat/96-tick window, one spike per tick. Bipolar: a tick that
    // arrived late (positive deviation) draws upward from the center
    // line, one that arrived early (negative) draws downward - so the
    // direction of the deviation is visible, not just its size. Height
    // scales the same way as the ms ruler above (max 8px = rangeUs),
    // so "tall" means the same thing on both axes. Every tick gets a
    // mark, like a real pen never leaving the paper - that's what
    // makes it possible to see AT WHICH BEAT POSITION the big ones
    // cluster, instead of just seeing isolated marks with no context.
    // =======================================================================
    const int tlX0 = 8, tlX1 = 120, tlCenterY = 48, tlW = tlX1 - tlX0;
    u8g2.drawHLine(tlX0, tlCenterY, tlW);
    uint8_t hcMinus1 = (histCount > 1) ? (histCount - 1) : 1;
    for (uint8_t p = 0; p < histCount; p++) {
      // p=0 is the oldest sample (left edge), p=histCount-1 the newest
      // (right edge) - the pen sweeps this way every cycle.
      uint8_t ticksAgo = (histCount - 1) - p;
      uint8_t idxr = (histIdxSnap + ANALYZER_HISTORY_SIZE - 1 - ticksAgo) % ANALYZER_HISTORY_SIZE;
      int32_t d = (int32_t)localHist[idxr] - (int32_t)avgRefSnap;
      uint32_t ad = (uint32_t)((d < 0) ? -d : d);
      int x = tlX0 + (int)(((uint32_t)p * (uint32_t)tlW) / hcMinus1);
      int h = (int)(((uint64_t)ad * 8) / (uint32_t)rangeUs);
      if (h < 1) h = 1; // the pen always leaves a mark, even for a near-perfect tick
      if (h > 8) h = 8;
      if (d >= 0) {
        u8g2.drawVLine(x, tlCenterY - h, h);     // late -> up
      } else {
        u8g2.drawVLine(x, tlCenterY + 1, h);     // early -> down
      }
    }
  }

  // =========================================================================
  // MIDI RUN/STOP: level trace over the last ~12s, extracted from the
  // MIDI Start/Stop/Continue messages (see handleStart()/handleStop()/
  // handleContinue()) rather than a CV gate input - this board has no CV
  // inputs, so this replaces the CV I/O status area from the Eurorack
  // variant's analyzer with the MIDI-equivalent information. Current
  // state as a single "RUN = HIGH"/"RUN = LOW" label to the left of
  // the trace, instead of a separate caption line above it.
  // =========================================================================
  {
    const int traceX0 = 46, traceX1 = 122, highY = 58, lowY = 63;
    const int traceW = traceX1 - traceX0;
    u8g2.drawStr(1, lowY, runningSnap ? "RUN = HIGH" : "RUN = LOW");
    bool prevLevel = rsHistSnap[rsIdxSnap % MIDI_RUNSTOP_HISTORY_SIZE];
    int prevX = traceX0;
    for (int x = 0; x < MIDI_RUNSTOP_HISTORY_SIZE; x++) {
      uint8_t idx = (rsIdxSnap + x) % MIDI_RUNSTOP_HISTORY_SIZE; // oldest sample left, newest right
      bool level = rsHistSnap[idx];
      int curX = traceX0 + (x * traceW) / (MIDI_RUNSTOP_HISTORY_SIZE - 1);
      int yPrev = prevLevel ? highY : lowY;
      int yCur  = level    ? highY : lowY;
      if (x > 0) u8g2.drawLine(prevX, yPrev, curX, yPrev); // horizontal segment
      if (level != prevLevel) u8g2.drawLine(curX, yPrev, curX, yCur); // edge
      prevLevel = level;
      prevX = curX;
    }
  }

  u8g2.sendBuffer();
}

void render() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  // Consistent copies of the volatile variables for this frame
  noInterrupts();
  uint32_t barSnapshot   = currentBar;
  uint16_t beatSnapshot  = currentBeat;
  uint16_t tickSnapshot  = tickInBeat;
  uint32_t ticksSnapshot = totalTicks;
  bool     runningSnap   = isRunning;
  uint8_t  resetSnap     = resetMode;
  bool     resetPendingTimeSnap = resetPendingResetsTime;
  bool     flashActiveSnap = resetFlashActive;
  uint32_t flashStartSnap  = resetFlashStartMs;
  uint32_t anchorSnap      = lastTickAnchorMicros;
  interrupts();

  // --- Beat extrapolation: smoothly interpolate between two real
  // ticks, instead of visibly "jumping" with every small jitter of
  // the source. The anchor is reset on every real tick -> no drift
  // possible, maximum deviation is one tick length (typ. <1ms at 120 BPM).
  float microsPerTick = 60000000.0f / (bpmFiltered * 24.0f);
  if (microsPerTick < 100.0f) microsPerTick = 100.0f; // safety net
  uint32_t microsSinceAnchor = micros() - anchorSnap;
  float extraTicks = (float)microsSinceAnchor / microsPerTick;
  if (extraTicks > 0.999f) extraTicks = 0.999f; // never extrapolate past the next (not yet confirmed) tick
  if (!runningSnap) extraTicks = 0.0f; // no extrapolation while stopped
  float tickExtrapolated = (float)tickSnapshot + extraTicks;

  // Beat-synchronous pulse (computed early, shared by A/B/C, G, F and
  // the reset-pending display) -> "on" for 1/3 of the beat.
  // Uses the extrapolated position instead of the raw tick (see above).
  bool beatPulseOnNormal = tickExtrapolated < 8.0f;  // 1x per beat
  bool beatPulseOnFast   = fmodf(tickExtrapolated, 12.0f) < 4.0f;  // 2x per beat (last row x128)

  // Instant flash feedback right when a reset is registered/committed:
  // B+F (and with option 2 also A) blink briefly 2-3x, independent of
  // the actual quantized reset time and of the beat-synced confirm
  // blink below.
  bool flashHideBar  = false;
  bool flashHideTime = false;
  if (flashActiveSnap) {
    uint32_t flashElapsed = millis() - flashStartSnap;
    if (flashElapsed >= RESET_FLASH_TOTAL_MS) {
      resetFlashActive = false; // animation done
    } else {
      bool suppress = ((flashElapsed / RESET_FLASH_PERIOD_MS) % 2) == 1;
      flashHideBar  = suppress;
      flashHideTime = suppress && resetPendingTimeSnap;
    }
  }

  // As long as a reset is registered (but not yet executed), blink B
  // continuously (every option) and also A (only option 2, and only if
  // this pending reset will also reset the elapsed time) in time with
  // the beat - as a persistent note "a reset is pending here".
  bool pendingHideBar  = (resetSnap != 0) && !beatPulseOnNormal;
  bool pendingHideTime = (resetSnap != 0) && resetPendingTimeSnap && !beatPulseOnNormal;

  bool hideBar  = flashHideBar  || pendingHideBar;
  bool hideTime = flashHideTime || pendingHideTime;

  // "RESET 1"/"RESET 2" confirmation blink (adopted from the Eurorack
  // firmware): counts down exactly 2 real beat pulses, independent of
  // tempo. Runs on every frame regardless of which G-content ends up
  // being drawn below, so it can't get stuck if a frame is skipped.
  if (confirmResetKind != 0) {
    if (!beatPulseOnNormal && confirmResetPulsePrev) {
      if (confirmResetBlinksLeft > 0) confirmResetBlinksLeft--;
      if (confirmResetBlinksLeft == 0) confirmResetKind = 0;
    }
  }
  confirmResetPulsePrev = beatPulseOnNormal;

  uint8_t bpb  = beatsPerBar();
  uint8_t div_ = divisor();

  // Divisor cycle progress (computed early, shared by G and the
  // lower bar F)
  uint32_t ticksPerCycle  = (uint32_t)div_ * bpb * 24;
  uint32_t ticksIntoCycle = ticksSnapshot % ticksPerCycle;
  uint32_t barsIntoCycle  = ticksIntoCycle / ((uint32_t)bpb * 24);

  // ---------------- Header row: time (A) | status/reset (G) | BPM (D) ----------------
  bool nothingToResetShowing = btnReset.isPressed && !runningSnap && !hasSomethingToReset();
  // No MIDI clock received for 5s while we're in STOP ->
  // "NO MIDI-CLOCK" instead of "STOP" is shown, tempo (D) is hidden. Only
  // relevant at the lowest priority level (no reset feedback/nudge active).
  bool noMidiClockShowing = !runningSnap && (millis() - lastClockTickMillis > 5000);

  char timeBuf[8];
  uint32_t elapsedMs = timeIsPaused ? (pausedAt - startMillis) : (millis() - startMillis);
  formatTime(timeBuf, elapsedMs);
  if (!hideTime && !nothingToResetShowing) {
    u8g2.drawStr(2, 7, timeBuf);
  }

  // G: status/reset display, centered in the header row
  if (nothingToResetShowing) {
    // Nothing to reset in STOP mode (see onResetButton guard) -
    // instead a brief note while held. A and D are hidden for this,
    // since the text wouldn't fit otherwise.
    const char* msg = "NOTHING TO RESET";
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 7, msg);
  } else if (btnReset.isPressed) {
    // Live feedback while held (Eurorack-style two-phase): for the
    // first RESET_CONFIRM_MS, just shows "RESET 1" plainly (that's
    // what releasing right now would register) with no sweep bar yet.
    // Past that, a sweep bar toward "RESET 2" appears, reaching full
    // at RESET2_HOLD_MS total hold time (when it actually escalates).
    const uint32_t RESET_CONFIRM_MS = 500;
    uint32_t heldMs = millis() - btnReset.pressStartMs;
    const char* label;
    float progress = 0.0f;
    bool showBar = false;
    if (heldMs < RESET_CONFIRM_MS) {
      label = "RESET 1";
    } else {
      label = "RESET 2";
      showBar = true;
      progress = (float)(heldMs - RESET_CONFIRM_MS) / (float)(RESET2_HOLD_MS - RESET_CONFIRM_MS);
      if (progress > 1.0f) progress = 1.0f;
    }

    int w = u8g2.getStrWidth(label);
    u8g2.setDrawColor(1);
    u8g2.drawStr((128 - w) / 2, 7, label);
    if (showBar) {
      int barW = w + 10; // a bit wider than the text
      int barX = (128 - barW) / 2;
      int fillW = (int)(barW * progress);
      u8g2.setDrawColor(2); // XOR: inverts the text wherever the bar passes over it
      if (fillW > 0) {
        u8g2.drawBox(barX, 0, fillW, 9);
      }
      u8g2.setDrawColor(1);
    }
  } else if (confirmResetKind != 0) {
    // 2x confirmation blink right after a reset was registered/
    // committed - runs independent of whether it's already executed
    // or (Reset 1/2) still pending.
    if (beatPulseOnNormal) {
      const char* label = (confirmResetKind == 1) ? "RESET 1" : (confirmResetKind == 2) ? "RESET 2" : "SET 1.1";
      int w = u8g2.getStrWidth(label);
      u8g2.drawStr((128 - w) / 2, 7, label);
    }
  } else if (resetSnap != 0) {
    // Reset 1 or 2 still registered after the initial confirmation
    // blink finished, waiting for the quantized bar/cycle end.
    if (beatPulseOnNormal) {
      char resetBuf[10];
      sprintf(resetBuf, "RESET %u", resetSnap);
      int w = u8g2.getStrWidth(resetBuf);
      u8g2.drawStr((128 - w) / 2, 7, resetBuf);
    }
  } else if (currentMode == MODE_NUDGE) {
    char nudgeBuf[20];
    sprintf(nudgeBuf, "NUDGE %+d/16", nudgeOffsetSixteenths);
    int w = u8g2.getStrWidth(nudgeBuf);
    u8g2.drawStr((128 - w) / 2, 7, nudgeBuf);
  } else {
    if (noMidiClockShowing) {
      // Time-based blink (no MIDI clock present to sync to - hence
      // millis()-based instead of beat-synchronous).
      bool blinkOn = (millis() / 500) % 2 == 0;
      if (blinkOn) {
        const char* statusBuf = "NO MIDI-CLOCK";
        int w = u8g2.getStrWidth(statusBuf);
        u8g2.drawStr((128 - w) / 2, 7, statusBuf);
      }
    } else {
      const char* statusBuf = runningSnap ? "RUN" : "STOP";
      int w = u8g2.getStrWidth(statusBuf);
      u8g2.drawStr((128 - w) / 2, 7, statusBuf);
    }
  }

  char bpmBuf[10];
  dtostrf(bpmFiltered, 4, 1, bpmBuf);
  if (!nothingToResetShowing && !noMidiClockShowing) {
    u8g2.drawStr(100, 7, bpmBuf);
  }

  // ---------------- Top bar: beat progress (5px) ----------------
  const int barX = 2, barW = 124;
  const int topY = 9, topH = 5;
  u8g2.drawFrame(barX, topY, barW, topH);

  float beatProgress = ((float)beatSnapshot * 24.0f + tickExtrapolated) / (float)(bpb * 24);
  int fillEndPx = (int)(beatProgress * barW); // global progress in pixels
  for (uint8_t i = 0; i < bpb; i++) {
    int segX0 = barX + (i * barW) / bpb;       // even integer distribution
    int segX1 = barX + ((i + 1) * barW) / bpb; // (no rounding "clump" effect)
    int fillInThisSeg = (barX + fillEndPx) - segX0;
    if (fillInThisSeg > segX1 - segX0) fillInThisSeg = segX1 - segX0;
    if (fillInThisSeg > 0) {
      u8g2.drawBox(segX0, topY, fillInThisSeg, topH);
    }
  }
  // Draw inverted (color0) separator lines AFTER the fill, so they
  // appear as a visible gap even when the segment is fully filled.
  u8g2.setDrawColor(0);
  for (uint8_t i = 1; i < bpb; i++) {
    u8g2.drawVLine(barX + (i * barW) / bpb, topY, topH);
  }
  u8g2.setDrawColor(1);

  // ---------------- Nudge position bar (4px, nudge mode only) ----------------
  // Sits in the gap between E (ends at y=14) and F (starts at
  // y=18), without shifting either. Grows from the center
  // toward left (negative) or right (positive).
  if (currentMode == MODE_NUDGE) {
    const int nudgeY = 14, nudgeH = 4;
    int centerX = barX + barW / 2;
    const int16_t NUDGE_DISPLAY_RANGE = 16; // +-16/16 = +-1 beat full deflection
    int16_t clamped = nudgeOffsetSixteenths;
    if (clamped > NUDGE_DISPLAY_RANGE) clamped = NUDGE_DISPLAY_RANGE;
    if (clamped < -NUDGE_DISPLAY_RANGE) clamped = -NUDGE_DISPLAY_RANGE;
    int halfWidth = barW / 2;
    int fillPx = ((int)abs(clamped) * halfWidth) / NUDGE_DISPLAY_RANGE;
    if (fillPx > 0) {
      if (clamped > 0) {
        u8g2.drawBox(centerX, nudgeY, fillPx, nudgeH);
      } else {
        u8g2.drawBox(centerX - fillPx, nudgeY, fillPx, nudgeH);
      }
    }
    // 1/16 subdivisions across the full width, in the same style as
    // E: inverted (color0) separator lines, so they still appear as
    // a visible gap even within the filled area.
    u8g2.setDrawColor(0);
    for (int16_t i = -NUDGE_DISPLAY_RANGE; i <= NUDGE_DISPLAY_RANGE; i++) {
      int x = centerX + (i * halfWidth) / NUDGE_DISPLAY_RANGE;
      u8g2.drawVLine(x, nudgeY, nudgeH);
    }
    u8g2.setDrawColor(1);
    u8g2.drawVLine(centerX, nudgeY, nudgeH); // center marker, always visible
  }

  // ---------------- Bottom bar: divisor cycle, fixed 32px height ----------------
  const int lowerY = 18, lowerH = 32;
  uint8_t segsPerRow = (div_ < 8) ? div_ : 8;
  uint8_t rows = (div_ + 7) / 8; // rounded up, gives 1/2/4/8 with our values
  int rowH = lowerH / rows;      // 32 is always evenly divisible by 1/2/4/8

  bool thinRows = rowH < 4; // e.g. at x128 (16 rows of 2px) -> frame/inset would no longer be visible

  uint32_t activeRow = barsIntoCycle / segsPerRow; // row the active step is located in
  bool activeInLastRow = thinRows && (rows > 1) && (activeRow == (uint32_t)(rows - 1));

  if (!flashHideBar) {
    for (uint8_t r = 0; r < rows; r++) {
      int y = lowerY + r * rowH;
      for (uint8_t i = 0; i < segsPerRow; i++) {
        uint32_t globalIndex = (uint32_t)r * segsPerRow + i;
        int x0 = barX + (i * barW) / segsPerRow;       // even integer distribution
        int x1 = barX + ((i + 1) * barW) / segsPerRow;
        int segWpx = x1 - x0;

        bool isFilled = globalIndex < barsIntoCycle;
        bool beatPulseOn = activeInLastRow ? beatPulseOnFast : beatPulseOnNormal;
        bool isActivePulse = (globalIndex == barsIntoCycle && runningSnap && beatPulseOn);

        if (thinRows) {
          // Very thin rows: no frame, fill fully without inset directly,
          // otherwise nothing would remain visible after the inset at
          // 2px row height.
          if (isActivePulse) {
            u8g2.drawBox(x0, y, segWpx, rowH);
          } else if (isFilled) {
            // At x128 (2px height only): fall back to the old layout
            // with a continuous line instead of a dot pattern
            u8g2.drawHLine(x0, y, segWpx);
          }
        } else {
          u8g2.drawFrame(x0, y, segWpx, rowH);
          if (isActivePulse) {
            u8g2.drawBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
          } else if (isFilled) {
            // Elapsed step: dimmed/striped instead of fully filled,
            // so it stands out less than the active, blinking step
            drawDitheredBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
          }
        }
      }
    }
    // Redraw separator lines between segments inverted (color0), so
    // they stay visible as a gap even on filled/blinking steps.
    u8g2.setDrawColor(0);
    for (uint8_t r = 0; r < rows; r++) {
      int y = lowerY + r * rowH;
      for (uint8_t i = 1; i < segsPerRow; i++) {
        int x = barX + (i * barW) / segsPerRow;
        u8g2.drawVLine(x, y, rowH);
      }
    }
    // Only draw row separators when rows are tall enough, otherwise
    // the separator line would eat up most of the fill height at very
    // thin rows (e.g. x128).
    if (!thinRows) {
      for (uint8_t r = 1; r < rows; r++) {
        int y = lowerY + r * rowH;
        u8g2.drawHLine(barX, y, barW);
      }
    }
    u8g2.setDrawColor(1);
  }

  // ---------------- Footer: B+C (bar/cycle end) | time signature (H) | divisor (I) ----------------
  const int footY = 62;

  char barBuf[16];
  sprintf(barBuf, "BAR %lu", (unsigned long)(barSnapshot + 1)); // 1-based for display
  int barTextWidth = u8g2.getStrWidth(barBuf);

  uint32_t barDisplay1Based = barSnapshot + 1;
  uint32_t cycleEndBar = ((barDisplay1Based + div_ - 1) / div_) * div_;
  char cycleBuf[10];
  sprintf(cycleBuf, ">%lu", (unsigned long)cycleEndBar);
  int cycleTextWidth = u8g2.getStrWidth(cycleBuf);

  // Space for B+C is always reserved (even when C is currently "off"
  // in the beat pulse), so H doesn't jump back and forth with every blink.
  int bcRightEdge = 2 + barTextWidth + 2 + cycleTextWidth;

  if (!hideBar) {
    u8g2.drawStr(2, footY, barBuf);
    if (beatPulseOnNormal) {
      u8g2.drawStr(2 + barTextWidth + 2, footY, cycleBuf);
    }
  }

  // H: time signature display, centered - shifts right if B+C
  // (including the space reserved for C) would otherwise overlap it.
  const char* timeSigStr = TIME_SIG_LABEL[timeSigIndex];
  int timeSigWidth = u8g2.getStrWidth(timeSigStr);
  int timeSigX = (128 - timeSigWidth) / 2;
  if (timeSigX < bcRightEdge + 3) {
    timeSigX = bcRightEdge + 3;
  }
  u8g2.drawStr(timeSigX, footY, timeSigStr);

  char divBuf[12];
  sprintf(divBuf, "x%u %s", div_, (div_ > 1) ? "BARS" : "BAR");
  int divWidth = u8g2.getStrWidth(divBuf);
  u8g2.drawStr(126 - divWidth, footY, divBuf);

  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
// BOOT SCREEN (approx. 5 seconds, C64 style with blinking cursor + explosion outro)
// ---------------------------------------------------------------------------

// Small 8x8-pixel "invader" in retro game style, two run-animation frames
static const unsigned char invaderFrameA[] PROGMEM = {
  0x3C, 0x7E, 0xDB, 0xFF, 0xBD, 0x24, 0x42, 0x81
};
static const unsigned char invaderFrameB[] PROGMEM = {
  0x3C, 0x7E, 0xDB, 0xFF, 0x3C, 0x5A, 0xA5, 0x24
};

// Duration of the boot screen text phase: invader runs across exactly
// this long from left to right, on arrival at the right "load time" is over.
const uint32_t BOOT_TEXT_DURATION = 3800; // ms

int computeInvaderX(uint32_t animT) {
  const int startX = 120; // right edge (wraparound fixed via NONAME0 constructor)
  int readyWidth = u8g2.getStrWidth("READY.");
  int endX = 2 + readyWidth + 2; // target position: under the cursor
  if (animT >= BOOT_TEXT_DURATION) return endX;
  int delta = startX - endX;
  int progressPx = (int)(((uint64_t)animT * delta) / BOOT_TEXT_DURATION);
  return startX - progressPx;
}

void drawInvaderSprite(uint32_t animT) {
  const int frameStep = 350; // ms per run-frame change (a bit slower)
  int x = computeInvaderX(animT);
  bool frameA = ((animT / frameStep) % 2) == 0;
  u8g2.drawXBMP(x, 56, 8, 8, frameA ? invaderFrameA : invaderFrameB);
}

void drawBootTextFrame(const char* ramLine, const char* fwLine, bool cursorOn, uint32_t animT, uint8_t visibleLines = 4, bool showInvader = true) {
  if (visibleLines >= 1) {
    u8g2.drawStr(2, 9,  "**BARSYNC**");
  }

  if (visibleLines >= 2) {
    u8g2.drawStr(2, 29, ramLine);
    int strikeWidth = u8g2.getStrWidth("64K");
    u8g2.drawHLine(2, 25, strikeWidth);
  }

  if (visibleLines >= 3) {
    u8g2.drawStr(2, 39, fwLine);
  }

  if (visibleLines >= 4) {
    u8g2.drawStr(2, 50, "READY.");
    if (cursorOn) {
      int readyWidth = u8g2.getStrWidth("READY.");
      u8g2.drawBox(2 + readyWidth + 2, 42, 6, 9);
    }
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(2, 62, "HOLD RESET FOR SETTINGS");
    u8g2.setFont(u8g2_font_6x10_tf);
  }

  if (showInvader) {
    drawInvaderSprite(animT);
  }
}

// Checks whether the reset button is currently held continuously for
// 1s. Uses a static local timer so it works across multiple calls
// without a global variable. Called per animation frame, so that
// during boot/the boot animation it's possible to jump into the
// settings menu at any time.
bool menuEntryHoldCheck() {
  static uint32_t holdStart = 0;
  bool pressed = (digitalRead(PIN_BTN_RESET) == LOW);
  if (pressed) {
    if (holdStart == 0) holdStart = millis();
    if (millis() - holdStart >= LONGPRESS_MS) {
      holdStart = 0;
      return true;
    }
  } else {
    holdStart = 0;
  }
  return false;
}

// Returns true if the animation was aborted because the reset button
// was held for 1s (-> settings menu should be opened). Returns false
// if the animation ran to its normal end.
bool showBootScreen() {
  const uint32_t shootDuration   = 300;  // ms, invader freezes and shoots
  const uint32_t explodeDuration = 700;  // ms, explosion outro
  const uint32_t frameDelay      = 40;   // ms between frames
  const uint32_t blinkPeriod     = 400;  // ms, classic C64 cursor rate
  const int      maxRadius       = 150;  // covers the whole screen from any edge point
  const int      bulletHeight    = 3;    // height of a single shot, in pixels

  char fwLine[24];
  sprintf(fwLine, "FIRMWARE V%s", FW_VERSION);

  char ramLine[24];
  uint32_t heapKB = ESP.getHeapSize() / 1024; // actual available heap size
  sprintf(ramLine, "64K %luK RAM SYSTEM", (unsigned long)heapKB);

  u8g2.setFont(u8g2_font_6x10_tf); // monospace, for the retro look

  // --- Phase 0: lines appear one after another ---
  const uint32_t lineRevealDelay = 350; // ms per line
  for (uint8_t visibleLines = 1; visibleLines <= 4; visibleLines++) {
    u8g2.clearBuffer();
    drawBootTextFrame(ramLine, fwLine, false, 0, visibleLines, /*showInvader=*/false);
    u8g2.sendBuffer();
    delay(lineRevealDelay);
    if (menuEntryHoldCheck()) return true;
  }

  // --- Phase 1: text display, invader comes from the right and runs to the cursor ---
  uint32_t startT = millis();
  while (millis() - startT < BOOT_TEXT_DURATION) {
    uint32_t elapsed = millis() - startT;
    bool cursorOn = ((elapsed / blinkPeriod) % 2) == 0;

    u8g2.clearBuffer();
    drawBootTextFrame(ramLine, fwLine, cursorOn, elapsed);
    u8g2.sendBuffer();
    delay(frameDelay);
    if (menuEntryHoldCheck()) return true;
  }

  // Invader has arrived at the right, load time is over
  int frozenX = computeInvaderX(BOOT_TEXT_DURATION);
  int bulletX = frozenX + 4; // center of the 8px sprite

  // --- Phase 1b: invader fires a single shot at the cursor ---
  const int shootStartY = 56; // top edge of invader sprite
  const int shootEndY   = 42; // top edge of cursor block
  uint32_t shootStart = millis();
  while (millis() - shootStart < shootDuration) {
    uint32_t elapsedS = millis() - shootStart;
    float progress = (float)elapsedS / shootDuration;
    if (progress > 1.0f) progress = 1.0f;
    int bulletY = shootStartY - (int)(progress * (shootStartY - shootEndY));

    u8g2.clearBuffer();
    // Cursor keeps blinking normally during the approach, until it's hit
    bool cursorStillThere = progress < 1.0f;
    drawBootTextFrame(ramLine, fwLine, cursorStillThere, BOOT_TEXT_DURATION);
    u8g2.drawVLine(bulletX, bulletY, bulletHeight); // single shot, no beam
    u8g2.sendBuffer();
    delay(frameDelay);
    if (menuEntryHoldCheck()) return true;
  }

  // --- Phase 2: explosion from the impact point (with the destroyed cursor) ---
  uint32_t explodeStart = millis();
  while (millis() - explodeStart < explodeDuration) {
    uint32_t elapsed2 = millis() - explodeStart;
    float progress = (float)elapsed2 / explodeDuration;
    if (progress > 1.0f) progress = 1.0f;
    int radius = (int)(progress * maxRadius);

    u8g2.clearBuffer();
    drawBootTextFrame(ramLine, fwLine, false, BOOT_TEXT_DURATION); // cursor destroyed, stays off
    u8g2.drawDisc(bulletX, shootEndY, radius); // growing white circle from the impact point
    u8g2.sendBuffer();
    delay(frameDelay);
    if (menuEntryHoldCheck()) return true;
  }

  // Hold fully white briefly before the main display takes over
  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 128, 64);
  u8g2.sendBuffer();
  delay(120);

  return false; // animation ran to its normal end, no menu requested
}

// ---------------------------------------------------------------------------
// SETTINGS MENU (on boot: hold reset button 1s to open)
// ---------------------------------------------------------------------------
// Menu structure (v1.2.0, updated): adopted from the Eurorack
// firmware's menu redesign (see BarSync Eurorack CHANGELOG), minus the
// CV INPUTS category (no CV hardware on this board) and minus Display
// > Rotate (this unit stays mounted in landscape, no runtime display
// rotation needed here). TIMESIG moved to page 1 as a direct value
// (custom button is now freely reassignable, see Switches > Custom):
//
//   SETUP (page 1)
//     TIMESIG                 (direct value, cycles 4/4-3/4-5/4-6/8-7/8, default 4/4)
//     SWITCHES >              (page 2)
//       CUSTOM >                 (page 3: which function the custom
//                                 button performs - TIMESIG/RESET 1/RESET 2)
//       RESET >                  (page 3: MODE = QUANTIZED/INSTANT,
//                                 PLAYTIME = also reset elapsed time?)
//       DIVISOR >                (page 3: existing checkbox list)
//     DISPLAY >               (page 2: CONTRAST, INVERT)
//     STANDBY                 (as before: ON/OFF + TIME)
//     DEFAULTS                (YES/NO confirmation page)
//
// Navigation unchanged: custom button = up, divisor button = down,
// reset button short = change value / open item, reset button held 1s
// = one level back (only on page 1: 1s arms "LEAVING MENU", 3s total
// saves+exits+restarts - as before).
// ---------------------------------------------------------------------------

enum MenuScreen {
  SCR_TOP, SCR_SWITCHES, SCR_CUSTOM_ROLE, SCR_TIMESIG_ENABLE, SCR_SWITCH_RESET, SCR_DIVISOR,
  SCR_DISPLAY, SCR_STANDBY, SCR_CONFIRM_DEFAULTS
};
MenuScreen menuScreen = SCR_TOP;
uint8_t menuCursor = 0;
uint8_t topCursorSaved      = 0; // cursor position on page 1, to return to
uint8_t categoryCursorSaved = 0; // cursor position on page 2, to return to
uint8_t customRoleCursorSaved = 0; // cursor position on Switches > Custom, to return to

// Manual prototypes: several of the functions below take MenuScreen as
// a parameter. Without these, the Arduino IDE's ctags-based automatic
// prototype generation would insert its own (faulty) prototypes near
// the very top of the file, BEFORE "enum MenuScreen" is known there -
// exactly the same problem already solved further up for struct
// Button/ButtonCallback.
uint8_t menuItemCount(MenuScreen s);
MenuScreen menuParent(MenuScreen s);
const char* menuHeader(MenuScreen s);
void menuGetValueStr(MenuScreen s, uint8_t item, char* buf, size_t bufLen);
void renderValueList(MenuScreen s, const char* const* names, uint8_t count,
                      const char* const* helpLines, uint8_t helpCount);
void enterScreen(MenuScreen target);

bool    menuExitRequested = false;
bool    menuSaveOnExit    = true;
bool    menuLeavingArmed  = false; // true once the top level has been held >=1s ("LEAVING MENU" blinks)
bool    menuWillRestart   = false; // true if the exit was triggered via the 3s threshold (-> restart)
uint32_t menuDefaultsLoadedUntilMs = 0; // != 0 while "DEFAULTS LOADED" blinks on page 1

uint8_t menuItemCount(MenuScreen s) {
  switch (s) {
    case SCR_TOP:              return 5; // TIMESIG + SWITCHES + DISPLAY + STANDBY + DEFAULTS
    case SCR_SWITCHES:         return 3;
    case SCR_CUSTOM_ROLE:      return 2; // FUNCTION + (Timesigs> or MODE, depending on FUNCTION)
    case SCR_TIMESIG_ENABLE:   return TIME_SIG_COUNT;
    case SCR_SWITCH_RESET:     return 2; // RESET 1 PLAYTIME, RESET 2 PLAYTIME
    case SCR_DIVISOR:          return DIVISOR_COUNT;
    case SCR_DISPLAY:          return 2;
    case SCR_STANDBY:          return 2;
    case SCR_CONFIRM_DEFAULTS: return 2;
  }
  return 1;
}

// Which screen does "back" (hold 1s) lead to?
MenuScreen menuParent(MenuScreen s) {
  switch (s) {
    case SCR_TIMESIG_ENABLE:
      return SCR_CUSTOM_ROLE;
    case SCR_CUSTOM_ROLE: case SCR_SWITCH_RESET: case SCR_DIVISOR:
      return SCR_SWITCHES;
    default:
      return SCR_TOP; // SCR_SWITCHES, SCR_DISPLAY, SCR_STANDBY, SCR_CONFIRM_DEFAULTS
  }
}

// Header shown on every page - always the name of the parent menu item.
const char* menuHeader(MenuScreen s) {
  switch (s) {
    case SCR_TOP:              return "SETUP";
    case SCR_SWITCHES:         return "SWITCHES";
    case SCR_CUSTOM_ROLE:      return "CUSTOM SWITCH";
    case SCR_TIMESIG_ENABLE:   return "TIMESIGS FOR CUSTOM";
    case SCR_SWITCH_RESET:     return "RESET SWITCH";
    case SCR_DIVISOR:          return "DIVISOR SELECT";
    case SCR_DISPLAY:          return "DISPLAY";
    case SCR_STANDBY:          return "STANDBY SETUP";
    case SCR_CONFIRM_DEFAULTS: return "LOAD DEFAULTS?";
  }
  return "SETUP";
}

// Item names for the pure navigation entries on page 1 (everything
// after the TIMESIG value item, see renderTopScreen()) and for page 2
// of Switches - no own value shown here, every item leads one level
// deeper. Deliberately WITHOUT a help footer: the explanation lives on
// the final settings page, not on the way there.
const char* TOP_NAV_NAMES[]  = {"SWITCHES", "DISPLAY", "STANDBY", "DEFAULTS"};
const char* SWITCHES_NAMES[] = {"CUSTOM", "DIVISOR", "RESET"};

// Item names for the editable leaves that have room for a footer on
// this display (name + value).
const char* DISPLAY_NAMES[]      = {"CONTRAST", "INVERT"};

// Short explanations of what each item is for, shown at the bottom of
// the final settings page, set off by a divider line (see
// renderHelpFooterSmall()). Kept to short lines that fit the 128px width at
// this font size. The DIVISOR and TIMESIG_ENABLE checkbox pages skip
// the footer: with up to 8/5 items there simply isn't vertical room
// left on this 128x64 landscape display (unlike the Eurorack's taller
// portrait screen), and the checkboxes are self-explanatory anyway.
const char* HELP_DISPLAY[]      = {"CONTRAST: level", "INVERT: b/w swap"};
const char* HELP_STANDBY[]      = {"ON/OFF: auto sleep", "TIME: sleep delay"};
const char* HELP_CONFIRM[]      = {"WARNING: resets", "ALL settings!"};

void menuGetValueStr(MenuScreen s, uint8_t item, char* buf, size_t bufLen) {
  switch (s) {
    case SCR_DISPLAY:
      if (item == 0) snprintf(buf, bufLen, "%u", settings.contrast);
      else           snprintf(buf, bufLen, "%s", settings.invert ? "ON" : "OFF");
      break;
    default:
      buf[0] = '\0';
      break;
  }
}

// Divider + short explanation at the bottom of a settings page,
// rendered at the smaller 4x6 font (used consistently across every
// submenu now, so all explanations look the same size) - called by
// renderValueList() and the dedicated STANDBY/CONFIRM_DEFAULTS/CUSTOM
// SWITCH/RESET SWITCH leaves after the actual content. y = divider
// position, text follows below. linePitch is normally 7px; pass 6 for
// pages that need to fit 4 lines in the available space (Custom
// Switch/Reset Switch).
void renderHelpFooterSmall(int y, const char* const* lines, uint8_t count, uint8_t linePitch) {
  u8g2.drawStr(2, y, "--------------------");
  u8g2.setFont(u8g2_font_4x6_tf);
  for (uint8_t i = 0; i < count; i++) {
    u8g2.drawStr(2, y + linePitch * (i + 1), lines[i]);
  }
  u8g2.setFont(u8g2_font_6x10_tf); // restore - other code assumes this is the active font
}

// Unified menu grid, used consistently by every settings page: first
// item at MENU_Y0, MENU_STEP px between items, help footer divider
// MENU_FOOTER_GAP px below the last item, footer text lines
// MENU_FOOTER_PITCH px apart (small 4x6 font). Kept identical across
// every screen so the whole menu looks like one consistent design
// instead of each page having its own spacing.
const int MENU_Y0 = 20;
const int MENU_STEP = 9;
const int MENU_FOOTER_GAP = 5;
const int MENU_FOOTER_PITCH = 7;

// Pure navigation list (no value, every item leads one level deeper) -
// one line per entry, ">" marks the cursor.
void renderNavList(const char* header, const char* const* names, uint8_t count) {
  u8g2.drawStr(2, 9, header);
  for (uint8_t i = 0; i < count; i++) {
    int y = MENU_Y0 + i * MENU_STEP;
    if (i == menuCursor) u8g2.drawStr(2, y, ">");
    u8g2.drawStr(12, y, names[i]);
  }
}

// Editable leaf: one line per entry (name + value), plus a help footer.
void renderValueList(MenuScreen s, const char* const* names, uint8_t count,
                      const char* const* helpLines, uint8_t helpCount) {
  u8g2.drawStr(2, 9, menuHeader(s));
  for (uint8_t i = 0; i < count; i++) {
    int y = MENU_Y0 + i * MENU_STEP;
    char valBuf[16];
    menuGetValueStr(s, i, valBuf, sizeof(valBuf));
    char line[26];
    snprintf(line, sizeof(line), "%-10s%s", names[i], valBuf);
    if (i == menuCursor) u8g2.drawStr(2, y, ">");
    u8g2.drawStr(12, y, line);
  }
  int footerY = MENU_Y0 + count * MENU_STEP + MENU_FOOTER_GAP;
  renderHelpFooterSmall(footerY, helpLines, helpCount, MENU_FOOTER_PITCH);
}

// Page 1 mixes one direct value item (TIMESIG) with plain navigation
// items (SWITCHES/DISPLAY/STANDBY/DEFAULTS) - gets its own renderer
// rather than forcing it through renderNavList()/renderValueList().
void renderTopScreen() {
  u8g2.drawStr(2, 9, menuHeader(SCR_TOP));
  uint8_t count = menuItemCount(SCR_TOP);
  for (uint8_t i = 0; i < count; i++) {
    int y = MENU_Y0 + i * MENU_STEP;
    char line[20];
    if (i == 0) {
      snprintf(line, sizeof(line), "%-11s%s", "TIMESIG", TIME_SIG_LABEL[timeSigIndex]);
    } else {
      snprintf(line, sizeof(line), "%s", TOP_NAV_NAMES[i - 1]);
    }
    if (i == menuCursor) u8g2.drawStr(2, y, ">");
    u8g2.drawStr(12, y, line);
  }
}

// Switches > Custom Switch: item 0 (FUNCTION) is a direct value; item
// 1's label/value depends on what FUNCTION is currently set to - a
// navigation entry ("AVAILABLE TIMESIGS") when FUNCTION=TIMESIG, or a
// direct value (MODE) when FUNCTION=SET 1.1. Gets its own renderer
// since that second item isn't a static name/value pair, and the
// explanation text below also changes depending on FUNCTION.
void renderCustomRoleScreen() {
  u8g2.drawStr(2, 9, menuHeader(SCR_CUSTOM_ROLE));

  char line0[20];
  snprintf(line0, sizeof(line0), "%-10s%s", "FUNCTION", CUSTOM_ROLE_LABEL[settings.customButtonRole]);
  if (menuCursor == 0) u8g2.drawStr(2, MENU_Y0, ">");
  u8g2.drawStr(12, MENU_Y0, line0);

  char line1[24];
  if (settings.customButtonRole == CUSTOM_ROLE_TIMESIG) {
    snprintf(line1, sizeof(line1), "AVAILABLE TIMESIGS");
  } else {
    snprintf(line1, sizeof(line1), "%-10s%s", "MODE", settings.resetInstantMode ? "INSTANT" : "QUANTIZED");
  }
  int y1 = MENU_Y0 + MENU_STEP;
  if (menuCursor == 1) u8g2.drawStr(2, y1, ">");
  u8g2.drawStr(12, y1, line1);

  int footerY = MENU_Y0 + 2 * MENU_STEP + MENU_FOOTER_GAP;
  if (settings.customButtonRole == CUSTOM_ROLE_TIMESIG) {
    static const char* helpTimesig[] = {"For cycling through the", "different time signatures."};
    renderHelpFooterSmall(footerY, helpTimesig, 2, MENU_FOOTER_PITCH);
  } else {
    static const char* helpSet11[] = {"Sets new bargrid start &", "resets bar count + time."};
    renderHelpFooterSmall(footerY, helpSet11, 2, MENU_FOOTER_PITCH);
  }
}

// Switches > Reset Switch: RESET 1 and RESET 2, each an independent
// yes/no toggle for whether that stage also resets the elapsed play
// time (the bar counter/grid itself always resets unconditionally).
void renderResetSwitchScreen() {
  u8g2.drawStr(2, 9, menuHeader(SCR_SWITCH_RESET));

  char line0[20];
  snprintf(line0, sizeof(line0), "%-10s%s", "RESET 1", settings.reset1PlaytimeEnabled ? "YES" : "NO");
  if (menuCursor == 0) u8g2.drawStr(2, MENU_Y0, ">");
  u8g2.drawStr(12, MENU_Y0, line0);

  char line1[20];
  int y1 = MENU_Y0 + MENU_STEP;
  snprintf(line1, sizeof(line1), "%-10s%s", "RESET 2", settings.reset2PlaytimeEnabled ? "YES" : "NO");
  if (menuCursor == 1) u8g2.drawStr(2, y1, ">");
  u8g2.drawStr(12, y1, line1);

  int footerY = MENU_Y0 + 2 * MENU_STEP + MENU_FOOTER_GAP;
  static const char* helpReset[] = {"Choose whether Reset 1 or 2", "should reset the playtime."};
  renderHelpFooterSmall(footerY, helpReset, 2, MENU_FOOTER_PITCH);
}

void renderMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (menuScreen == SCR_TIMESIG_ENABLE) {
    u8g2.drawStr(2, 9, menuHeader(SCR_TIMESIG_ENABLE));
    for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) {
      int y = MENU_Y0 + i * MENU_STEP;
      char line[20];
      snprintf(line, sizeof(line), "[%s] %s", isTimeSigEnabled(i) ? "x" : " ", TIME_SIG_LABEL[i]);
      if (i == menuCursor) u8g2.drawStr(2, y, ">");
      u8g2.drawStr(12, y, line);
    }
    u8g2.sendBuffer();
    return;
  }
  if (menuScreen == SCR_DIVISOR) {
    u8g2.drawStr(2, 9, menuHeader(SCR_DIVISOR));
    const uint8_t rowsPerCol = 4;
    for (uint8_t i = 0; i < DIVISOR_COUNT; i++) {
      uint8_t col = i / rowsPerCol;
      uint8_t row = i % rowsPerCol;
      int x = (col == 0) ? 2 : 68;
      int y = MENU_Y0 + row * MENU_STEP;
      char line[16];
      snprintf(line, sizeof(line), "[%s]x%-3u", isDivisorEnabled(i) ? "x" : " ", DIVISOR_VALUES[i]);
      if (i == menuCursor) u8g2.drawStr(x, y, ">");
      u8g2.drawStr(x + 10, y, line);
    }
    u8g2.sendBuffer();
    return;
  }
  if (menuScreen == SCR_STANDBY) {
    u8g2.drawStr(2, 9, menuHeader(SCR_STANDBY));
    char line0[20];
    snprintf(line0, sizeof(line0), "%-10s%s", "STANDBY", settings.standbyEnabled ? "ON" : "OFF");
    if (menuCursor == 0) u8g2.drawStr(2, MENU_Y0, ">");
    u8g2.drawStr(12, MENU_Y0, line0);

    char line1[20];
    int y1 = MENU_Y0 + MENU_STEP;
    snprintf(line1, sizeof(line1), "%-10s%uMIN", "TIME", STANDBY_DELAY_MINUTES[settings.standbyDelayIndex]);
    if (menuCursor == 1) u8g2.drawStr(2, y1, ">");
    u8g2.drawStr(12, y1, line1);
    int footerY = MENU_Y0 + 2 * MENU_STEP + MENU_FOOTER_GAP;
    renderHelpFooterSmall(footerY, HELP_STANDBY, sizeof(HELP_STANDBY) / sizeof(HELP_STANDBY[0]), MENU_FOOTER_PITCH);
    u8g2.sendBuffer();
    return;
  }
  if (menuScreen == SCR_CONFIRM_DEFAULTS) {
    u8g2.drawStr(2, 9, menuHeader(SCR_CONFIRM_DEFAULTS));
    int y1 = MENU_Y0 + MENU_STEP;
    if (menuCursor == 0) u8g2.drawStr(2, MENU_Y0, ">");
    u8g2.drawStr(12, MENU_Y0, "NO");
    if (menuCursor == 1) u8g2.drawStr(2, y1, ">");
    u8g2.drawStr(12, y1, "YES");
    int footerY = MENU_Y0 + 2 * MENU_STEP + MENU_FOOTER_GAP;
    renderHelpFooterSmall(footerY, HELP_CONFIRM, sizeof(HELP_CONFIRM) / sizeof(HELP_CONFIRM[0]), MENU_FOOTER_PITCH);
    u8g2.sendBuffer();
    return;
  }

  if (menuScreen == SCR_TOP && menuLeavingArmed) {
    // Blinking warning: reset is being held at the top level,
    // upon reaching 3s total the menu is exited (+restart)
    bool blinkOn = (millis() / 350) % 2 == 0;
    if (blinkOn) {
      const char* msg = "LEAVING MENU";
      int w = u8g2.getStrWidth(msg);
      u8g2.drawStr((128 - w) / 2, 36, msg);
    }
    u8g2.sendBuffer();
    return;
  }

  if (menuScreen == SCR_TOP && millis() < menuDefaultsLoadedUntilMs) {
    // Short blinking confirmation after "Load Defaults" was confirmed
    // with YES (see onMenuChange(), SCR_CONFIRM_DEFAULTS).
    bool blinkOn = (millis() / 250) % 2 == 0;
    if (blinkOn) {
      const char* msg = "DEFAULTS LOADED";
      int w = u8g2.getStrWidth(msg);
      u8g2.drawStr((128 - w) / 2, 36, msg);
    }
    u8g2.sendBuffer();
    return;
  }

  switch (menuScreen) {
    case SCR_TOP:          renderTopScreen(); break;
    case SCR_SWITCHES:     renderNavList(menuHeader(SCR_SWITCHES), SWITCHES_NAMES, menuItemCount(SCR_SWITCHES)); break;
    case SCR_CUSTOM_ROLE:  renderCustomRoleScreen(); break;
    case SCR_SWITCH_RESET: renderResetSwitchScreen(); break;
    case SCR_DISPLAY:      renderValueList(SCR_DISPLAY, DISPLAY_NAMES, menuItemCount(SCR_DISPLAY), HELP_DISPLAY, sizeof(HELP_DISPLAY) / sizeof(HELP_DISPLAY[0])); break;
    default: break;
  }
  u8g2.sendBuffer();
}

// custom button (in the menu = navigate up)
void onMenuUp(bool longPress) {
  uint8_t count = menuItemCount(menuScreen);
  menuCursor = (menuCursor + count - 1) % count;
}

// Divisor button (in the menu = navigate down)
void onMenuDown(bool longPress) {
  uint8_t count = menuItemCount(menuScreen);
  menuCursor = (menuCursor + 1) % count;
}

// Move one level deeper - remembers the cursor position of the current
// page, so "back" (goBack()) returns to exactly that spot. Three
// distinct cursor slots now, one per possible parent screen (TOP,
// SWITCHES, and - new with Switches > Custom > Timesigs - CUSTOM_ROLE).
void enterScreen(MenuScreen target) {
  MenuScreen parent = menuParent(target);
  if (parent == SCR_TOP) {
    topCursorSaved = menuCursor;
  } else if (parent == SCR_CUSTOM_ROLE) {
    customRoleCursorSaved = menuCursor;
  } else {
    categoryCursorSaved = menuCursor;
  }
  menuScreen = target;
  menuCursor = 0;
}

// One level back (reset button held 1s, except on page 1).
void goBack() {
  MenuScreen parent = menuParent(menuScreen);
  if (parent == SCR_TOP) {
    menuCursor = topCursorSaved;
  } else if (parent == SCR_CUSTOM_ROLE) {
    menuCursor = customRoleCursorSaved;
  } else {
    menuCursor = categoryCursorSaved;
  }
  menuScreen = parent;
}

// Reset button pressed briefly (in the menu = value change): on
// navigation pages (page 1/2) = one level deeper, on editable leaves
// (page 3, Standby, Confirm) = change value/toggle. Called directly
// from runSettingsMenu() (no simple button-release callback anymore,
// since reset in the menu has to distinguish several hold durations).
void onMenuChange(bool longPress) {
  switch (menuScreen) {
    case SCR_TOP:
      if (menuCursor == 0) {
        // TIMESIG: direct value on page 1, cycles through all
        // implemented time signatures (no more enabled-subset mask -
        // the custom button is no longer the only way to change it).
        timeSigIndex = (timeSigIndex + 1) % TIME_SIG_COUNT;
      } else {
        switch (menuCursor) {
          case 1: enterScreen(SCR_SWITCHES);         break;
          case 2: enterScreen(SCR_DISPLAY);          break;
          case 3: enterScreen(SCR_STANDBY);          break;
          case 4: enterScreen(SCR_CONFIRM_DEFAULTS); break;
        }
      }
      break;
    case SCR_SWITCHES:
      switch (menuCursor) {
        case 0: enterScreen(SCR_CUSTOM_ROLE);  break;
        case 1: enterScreen(SCR_DIVISOR);      break;
        case 2: enterScreen(SCR_SWITCH_RESET); break;
      }
      break;
    case SCR_CUSTOM_ROLE:
      if (menuCursor == 0) {
        settings.customButtonRole = (settings.customButtonRole + 1) % CUSTOM_ROLE_COUNT;
      } else if (settings.customButtonRole == CUSTOM_ROLE_TIMESIG) {
        enterScreen(SCR_TIMESIG_ENABLE);
      } else {
        settings.resetInstantMode = !settings.resetInstantMode;
      }
      break;
    case SCR_TIMESIG_ENABLE: {
      bool curEnabled = isTimeSigEnabled(menuCursor);
      if (curEnabled && countEnabledTimeSig() <= 1) return; // protect the last checkbox
      settings.enabledTimeSigMask ^= (1 << menuCursor);
      break;
    }
    case SCR_SWITCH_RESET:
      if (menuCursor == 0) settings.reset1PlaytimeEnabled = !settings.reset1PlaytimeEnabled;
      else                 settings.reset2PlaytimeEnabled = !settings.reset2PlaytimeEnabled;
      break;
    case SCR_DIVISOR: {
      bool curEnabled = isDivisorEnabled(menuCursor);
      if (curEnabled && countEnabledDivisor() <= 1) return;
      settings.enabledDivisorMask ^= (1 << menuCursor);
      break;
    }
    case SCR_DISPLAY:
      if (menuCursor == 0) {
        uint8_t idx = 0;
        for (uint8_t i = 0; i < CONTRAST_STEP_COUNT; i++) {
          if (CONTRAST_STEPS[i] == settings.contrast) { idx = i; break; }
        }
        settings.contrast = CONTRAST_STEPS[(idx + 1) % CONTRAST_STEP_COUNT];
        u8g2.setContrast(settings.contrast); // live preview
      } else {
        settings.invert = !settings.invert;
        u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6); // live preview
      }
      break;
    case SCR_STANDBY:
      if (menuCursor == 0) {
        settings.standbyEnabled = !settings.standbyEnabled;
      } else {
        settings.standbyDelayIndex = (settings.standbyDelayIndex + 1) % STANDBY_DELAY_COUNT;
      }
      break;
    case SCR_CONFIRM_DEFAULTS:
      if (menuCursor == 1) {
        factoryResetSettings(); // YES chosen
        menuDefaultsLoadedUntilMs = millis() + 1500; // "DEFAULTS LOADED" blinks briefly on page 1
      }
      menuScreen = SCR_TOP;
      menuCursor = topCursorSaved;
      break;
  }
}

void runSettingsMenu() {
  menuScreen = SCR_TOP;
  menuCursor = 0;
  topCursorSaved = 0;
  categoryCursorSaved = 0;
  customRoleCursorSaved = 0;
  menuExitRequested = false;
  menuSaveOnExit = true;
  menuLeavingArmed = false;
  menuWillRestart = false;
  menuDefaultsLoadedUntilMs = 0;

  // The reset button is typically still physically held when entering
  // the menu (the entry hold itself). So that this hold doesn't
  // immediately trigger an action, the button must be released once
  // before it's considered here at all.
  bool resetReleasedOnce = false;

  // Own state tracking for the reset button in the menu: short =
  // value change, 1s = one level up (or at the top level: arm
  // "LEAVING MENU"), 3s at the top level = save, exit and restart.
  bool     resetIsDown       = false;
  uint32_t resetPressStartMs = 0;
  bool     longActionHandled = false;

  u8g2.setFont(u8g2_font_6x10_tf);

  while (!menuExitRequested) {
    updateButton(btnCustom, onMenuUp);
    updateButton(btnDivisor, onMenuDown);

    bool rawReset = (digitalRead(PIN_BTN_RESET) == LOW);
    if (!rawReset) resetReleasedOnce = true;

    if (resetReleasedOnce) {
      if (rawReset && !resetIsDown) {
        // fresh button press starts
        resetIsDown       = true;
        resetPressStartMs = millis();
        longActionHandled = false;
      } else if (rawReset && resetIsDown && !longActionHandled) {
        uint32_t heldMs = millis() - resetPressStartMs;
        if (menuScreen != SCR_TOP) {
          // Not on page 1: holding 1s goes one level back
          if (heldMs >= LONGPRESS_MS) {
            goBack();
            longActionHandled = true; // this press is "consumed" -
            // continuing to hold doesn't also trigger
            // "LEAVING MENU"; for that the button must be
            // released and pressed again.
          }
        } else {
          // Top level: 1s arms "LEAVING MENU",
          // 3s total saves, exits, and restarts.
          if (heldMs >= LONGPRESS_MS) {
            menuLeavingArmed = true;
          }
          if (heldMs >= VERYLONG_MS) {
            longActionHandled  = true;
            menuSaveOnExit     = true;
            menuWillRestart    = true;
            menuExitRequested  = true;
          }
        }
      } else if (!rawReset && resetIsDown) {
        // button released
        uint32_t heldMs = millis() - resetPressStartMs;
        resetIsDown = false;
        if (!longActionHandled && heldMs < LONGPRESS_MS) {
          // A genuine short press (on any page): value change /
          // enter sub-screen / toggle checkbox.
          onMenuChange(false);
        }
        // Released before the 3s threshold was reached (but after
        // 1s) -> "LEAVING MENU" is discarded again, no action.
        menuLeavingArmed = false;
      }
    }

    renderMenu();
    delay(30);
  }

  if (menuSaveOnExit) {
    saveSettings();
    timeSigIndex = settings.timeSigIndex;
    divisorIndex = settings.divisorIndex;
  }

  u8g2.setContrast(settings.contrast);
  u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6);

  if (menuWillRestart) {
    // Wait until the reset button is actually released before
    // restarting. Without this, a permanently held button would still
    // be LOW right after reboot, and the boot-time entry check
    // (hold reset 1s at power-on -> open settings menu) would
    // immediately jump straight back into the menu again.
    if (digitalRead(PIN_BTN_RESET) == LOW) {
      u8g2.clearBuffer();
      const char* releaseMsg = "RELEASE RESET";
      int rw = u8g2.getStrWidth(releaseMsg);
      u8g2.drawStr((128 - rw) / 2, 36, releaseMsg);
      u8g2.sendBuffer();
      while (digitalRead(PIN_BTN_RESET) == LOW) {
        delay(10);
      }
    }

    u8g2.clearBuffer();
    const char* msg = "RESTARTING...";
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 36, msg);
    u8g2.sendBuffer();
    delay(400);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.print("BarSync — Firmware Version ");
  Serial.println(FW_VERSION);

  // WiFi and Bluetooth are not needed anywhere - disable explicitly
  // for minimal power consumption (relevant especially on battery power).
  WiFi.mode(WIFI_OFF);
  btStop();

  pinMode(PIN_BTN_DIVISOR, INPUT_PULLUP);
  pinMode(PIN_BTN_CUSTOM,  INPUT_PULLUP);
  pinMode(PIN_BTN_RESET,   INPUT_PULLUP);

  btnDivisor.pin = PIN_BTN_DIVISOR;
  btnCustom.pin  = PIN_BTN_CUSTOM;
  btnReset.pin   = PIN_BTN_RESET;

  // Load saved settings (time signature/divisor/contrast/invert/boot anim)
  loadSettings();

  // Check whether the reset button is already held for 1s at power-on
  // -> then open the settings menu instead of a normal boot.
  // (Blind check before display init, in case it was already held at power-on.)
  bool enterMenu = false;
  if (digitalRead(PIN_BTN_RESET) == LOW) {
    uint32_t holdStart = millis();
    while (digitalRead(PIN_BTN_RESET) == LOW) {
      if (millis() - holdStart >= LONGPRESS_MS) {
        enterMenu = true;
        break;
      }
    }
  }

  // Display
  u8g2.begin();
  u8g2.setBusClock(10000000); // 10 MHz SPI - SSD1309 handles this fine, cuts the framebuffer
                              // push time vs. the previous 4 MHz; matters because that push
                              // blocks loop() (and therefore MIDI reads) for its full duration
  u8g2.setContrast(settings.contrast);
  u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6);

  // So the reset button can still be held for 1s during the boot
  // animation itself to jump into the menu (showBootScreen() then
  // aborts on its own). The animation always runs (no skipping anymore).
  if (!enterMenu) {
    enterMenu = showBootScreen();
  }

  if (enterMenu) {
    runSettingsMenu();
  }

  // MIDI via hardware UART2 (RX=GPIO15, TX not required)
  MidiSerial.begin(31250, SERIAL_8N1, PIN_MIDI_RX, PIN_MIDI_TX);
  // Fire onReceive() as close to per-byte as this core version allows,
  // so each MIDI Real-Time byte (Clock/Start/Stop/Continue - all
  // single bytes) gets its own timestamp instead of several being
  // batched into one. Depends on ESP32 Arduino core >= 2.0.x; if
  // setRxFIFOFull() isn't available on an older installed core, this
  // line simply needs removing - popMidiRxTimestampOr() falls back to
  // plain micros() either way, so nothing breaks, timing precision
  // just reverts to the old behavior.
  MidiSerial.setRxFIFOFull(1);
  MidiSerial.onReceive(onMidiSerialReceive);
  MIDI.setHandleClock(handleClock);
  MIDI.setHandleStart(handleStart);
  MIDI.setHandleStop(handleStop);
  MIDI.setHandleContinue(handleContinue);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff(); // no passthrough needed, only evaluating the clock

  startMillis = millis();
  pausedAt    = startMillis; // prevents underflow in the time display, as long as no MIDI start has been received yet
}

// ---------------------------------------------------------------------------
// STANDBY (Light Sleep) — wakeup via MIDI activity or button
// ---------------------------------------------------------------------------
void renderStandbyCountdown(uint32_t secsLeft) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  const char* msg1 = "NO MIDI ACTIVITY";
  int w1 = u8g2.getStrWidth(msg1);
  u8g2.drawStr((128 - w1) / 2, 22, msg1);

  char buf[24];
  sprintf(buf, "STANDBY IN %lus", (unsigned long)secsLeft);
  int w2 = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w2) / 2, 40, buf);

  const char* msg3 = "KEY = CANCEL";
  int w3 = u8g2.getStrWidth(msg3);
  u8g2.drawStr((128 - w3) / 2, 56, msg3);

  u8g2.sendBuffer();
}

void enterStandby() {
  // Brief note before going to sleep
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  const char* msg = "STANDBY";
  int w = u8g2.getStrWidth(msg);
  u8g2.drawStr((128 - w) / 2, 36, msg);
  u8g2.sendBuffer();
  delay(600);
  u8g2.setPowerSave(1); // turn off display

  // Wake source 1: MIDI activity on the RX pin.
  // Important: esp_sleep_enable_uart_wakeup() on the original ESP32
  // only works with UART0/UART1 - UART2 (which we use for MIDI) is
  // not supported per Espressif's documentation and never wakes it up!
  // So instead we use GPIO wakeup on the RX pin itself: the falling
  // edge of the start bit (idle=HIGH -> LOW) wakes reliably,
  // regardless of the UART module used.
  gpio_wakeup_enable((gpio_num_t)PIN_MIDI_RX, GPIO_INTR_LOW_LEVEL);

  // Wake source 2: all three buttons (LOW level = pressed).
  // Note: EXT1 on the original ESP32 only supports "all pins LOW"
  // or "any pin HIGH" - for "any pin LOW" (our buttons with
  // INPUT_PULLUP) the GPIO wakeup API for light sleep is the right
  // choice, since it allows a level/interrupt type per pin.
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_DIVISOR, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_CUSTOM,  GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_RESET,   GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start(); // blocks here until a wake source triggers

  // --- Woke up ---
  u8g2.setPowerSave(0); // turn display back on

  // Disable GPIO wakeup again, otherwise the interrupt type remains
  // permanently active on the pins.
  gpio_wakeup_disable((gpio_num_t)PIN_MIDI_RX);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_DIVISOR);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_CUSTOM);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_RESET);

  // Bugfix history: an earlier fix here for "needs a second Start/
  // Continue to wake up" (GPIO-level wakeup only guarantees the CPU
  // wakes up on the falling start bit, not that the UART cleanly
  // receives that same byte) tried to flush any stray bytes out of
  // the RX buffer right after waking, and additionally called
  // MIDI.begin() again to reset the parser. Both turned out to cause a
  // worse bug: if the clock was already running when it woke us, the
  // flush also discarded the genuine Start message plus any Clock
  // ticks that had already queued up by the time we got here - and
  // MIDI.begin() risks the same thing indirectly, since this library
  // calls the transport's begin() again internally, which re-inits
  // the underlying HardwareSerial and can just as easily wipe its RX
  // buffer/FIFO. Either way, isRunning never got set and the display
  // just stayed frozen at its pre-sleep state until a fresh, unrelated
  // Stop+Start cycle came through later. Both removed - the settle
  // delay below is enough on its own: MIDI real-time messages (Clock/
  // Start/Stop/Continue) are single bytes that can't desync a multi-
  // byte "running status" expectation the way a channel message could,
  // so even an occasional corrupted real-time byte right at the wake
  // boundary is harmless - the library just ignores it and the next
  // byte parses normally, no explicit buffer/parser reset needed.
  delay(2); // let the UART/APB clock finish stabilizing
  lastTickAnchorMicros = micros(); // avoid a stale (pre-sleep) beat-extrapolation anchor

  // Check whether a button was the wake reason -> suppress its next
  // action, so the wake-up press doesn't trigger a function.
  // (No direct status bitmask API available for this wakeup path,
  // so instead: which button is still LOW right after waking.)
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool wokenByButton = false;
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    if (digitalRead(PIN_BTN_DIVISOR) == LOW) { suppressDivisorAction = true; wokenByButton = true; }
    if (digitalRead(PIN_BTN_CUSTOM)  == LOW) { suppressCustomAction = true; wokenByButton = true; }
    if (digitalRead(PIN_BTN_RESET)   == LOW) { suppressResetAction   = true; wokenByButton = true; }
  }

  // Bugfix ("stays frozen at the pre-sleep state after waking, even
  // with a clock already running - only a manual Stop+Start on the
  // sequencer gets it moving again"): isRunning is only ever set true
  // by an explicit Start/Continue message (see handleStart()/
  // handleContinue()). If the sequencer was already running/looping
  // the whole time BarSync was asleep, it has no reason to ever send a
  // fresh Start/Continue when we wake up - from its side, playback
  // never stopped. Incoming Clock ticks alone never set isRunning
  // (handleClock() bails out immediately while !isRunning), so without
  // this fix BarSync would stay stuck showing its pre-sleep state
  // forever, since the message it's waiting for is simply never coming.
  // Fix: if the wake wasn't caused by a button (so, presumably, MIDI
  // activity), treat it as an implicit "start now" - clear every
  // temporary grid/time value and begin fresh at 1.1 right away,
  // exactly as handleStart() would. If a genuine Start/Continue/Clock
  // message does follow shortly after, it simply continues counting
  // from this same clean baseline (Start would just re-zero everything
  // again harmlessly; Clock ticks increment normally since isRunning
  // is already true). If the wake turns out to have been a false
  // positive with no real clock behind it, the existing
  // CLOCK_LOST_TIMEOUT_MS watchdog reverts to STOP within a few
  // seconds on its own, so the worst case is a brief incorrect "RUN"
  // flash, never a permanently stuck display.
  if (cause == ESP_SLEEP_WAKEUP_GPIO && !wokenByButton) {
    totalTicks   = 0;
    tickInBeat   = 0;
    currentBeat  = 0;
    currentBar   = 0;
    resetMode    = 0;
    set11PendingNextBeat = false;
    confirmResetKind     = 0;
    isRunning    = true;
    startMillis  = millis();
    timeIsPaused = false;
    beatStartMicros = 0;
    bpmTickCounter  = 0;
  }

  lastClockTickMillis = millis(); // restart standby timer
}

// ---------------------------------------------------------------------------
// GIMMICK: MidiWar (press time signature+reset simultaneously to
// start/exit). Controls in-game: time signature=left, divisor=right,
// reset=shoot. Uses the same invader sprites as the boot screen.
// ---------------------------------------------------------------------------
uint32_t loadHighscore() {
  prefs.begin("midiclock", true);
  uint32_t hs = prefs.getUInt("highscore", 0);
  prefs.end();
  return hs;
}

void saveHighscoreIfBeaten(uint32_t score) {
  if (score > loadHighscore()) {
    prefs.begin("midiclock", false);
    prefs.putUInt("highscore", score);
    prefs.end();
  }
}

void runMidiWarGame() {
  const uint8_t MAX_ROWS = 5, MAX_COLS = 7;
  const int colSpacing = 16, rowSpacing = 8;
  bool alive[MAX_ROWS][MAX_COLS];

  int playerX = 60;
  const int playerY = 58;
  bool bulletActive = false;
  int bulletX = 0, bulletY = 0;
  bool lastResetRaw = HIGH;
  bool exitArmed = false; // combo must be released once before it counts as "exit"
  uint32_t exitHoldStart = 0; // timestamp since both buttons have been held continuously

  uint32_t score = 0;
  uint8_t level = 1;
  const uint8_t MAX_LEVEL = 20;
  uint32_t highscore = loadHighscore();

  u8g2.setFont(u8g2_font_5x7_tr);

  // Intro screen with current high score
  u8g2.clearBuffer();
  const char* title = "MIDIWAR";
  int wt = u8g2.getStrWidth(title);
  u8g2.drawStr((128 - wt) / 2, 26, title);
  char hbuf[20];
  sprintf(hbuf, "HIGHSCORE %lu", (unsigned long)highscore);
  int wh = u8g2.getStrWidth(hbuf);
  u8g2.drawStr((128 - wh) / 2, 40, hbuf);
  u8g2.sendBuffer();
  delay(1200);

  // --- Level setup (also used for level changes) ---
  uint8_t currentRows, currentCols;
  int blockX, blockY, dir, blockWidth;
  uint32_t stepInterval;
  bool frameToggle = false;
  uint32_t lastStepMs;

  auto setupLevel = [&]() {
    // Difficulty increases only via tempo and alien count (rows+columns),
    // not via the start position - the bottom row always stays at the
    // same safe height, no matter how many rows there currently are.
    currentRows = (level <= 7) ? 3 : (level <= 14) ? 4 : 5;   // 3/4/5 rows
    currentCols = (level <= 10) ? 5 : (level <= 16) ? 6 : 7;  // 5/6/7 columns

    blockWidth = (currentCols - 1) * colSpacing + 8;

    uint8_t pattern = (level - 1) % 5; // 5 different formations, cycling every level
    uint8_t midRow = currentRows / 2;
    uint8_t midCol = currentCols / 2;
    for (uint8_t r = 0; r < MAX_ROWS; r++) {
      for (uint8_t c = 0; c < MAX_COLS; c++) {
        if (r >= currentRows || c >= currentCols) { alive[r][c] = false; continue; }
        switch (pattern) {
          case 0: // full rectangle (classic)
            alive[r][c] = true;
            break;
          case 1: // checkerboard
            alive[r][c] = ((r + c) % 2 == 0);
            break;
          case 2: // plus/cross shape
            alive[r][c] = (r == midRow || c == midCol);
            break;
          case 3: // two clusters (gap in the middle)
            alive[r][c] = (c <= 1 || c >= currentCols - 2);
            break;
          case 4: { // inverted pyramid (narrow at top, wide at bottom)
            int lo = (int)midCol - (int)r;
            int hi = (int)midCol + (int)r;
            if (lo < 0) lo = 0;
            if (hi >= currentCols) hi = currentCols - 1;
            alive[r][c] = ((int)c >= lo && (int)c <= hi);
            break;
          }
        }
      }
    }

    // Bottom row always at the same safe height (safeBottomY) -
    // with more rows, only the upper rows move further up,
    // the bottom one never starts lower/closer to the player.
    const int safeBottomY = 34;
    blockY = safeBottomY - (int)(currentRows - 1) * rowSpacing;
    if (blockY < 9) blockY = 9; // don't overlap the header row

    blockX = (128 - blockWidth) / 2;
    dir = 1;
    stepInterval = 420 - (uint32_t)level * 10; // gentle speed ramp over 20 levels (level1≈410ms, level20≈220ms)
    lastStepMs = millis();
    bulletActive = false;
  };
  setupLevel();

  while (true) {
    // Read multiple times instead of once: with delay(30) per frame,
    // at high tempo (e.g. 120 BPM = a tick every ~20.8ms) MIDI data
    // would otherwise arrive faster than we pick it up -> a backlog
    // builds up in the UART queue, which gets processed all at once
    // when exiting the game and briefly distorts the BPM calculation.
    for (uint8_t i = 0; i < 8; i++) {
      MIDI.read();
    }

    bool customRaw = (digitalRead(PIN_BTN_CUSTOM) == LOW);
    bool divRaw  = (digitalRead(PIN_BTN_DIVISOR) == LOW);
    bool resetRaw = (digitalRead(PIN_BTN_RESET) == LOW);

    if (!exitArmed) {
      // Only once at least one of the two start buttons has been
      // released is the combo armed again as an "exit" signal.
      if (!(customRaw && resetRaw)) exitArmed = true;
    } else if (customRaw && resetRaw) {
      // Both must now be held continuously for 2s, so the game isn't
      // exited by accident. No visual feedback, the game keeps
      // running normally in the background.
      if (exitHoldStart == 0) exitHoldStart = millis();
      if (millis() - exitHoldStart >= 2000) {
        saveHighscoreIfBeaten(score);
        break;
      }
    } else {
      exitHoldStart = 0; // combo released before 2s were reached
    }

    if (customRaw) playerX -= 2;
    if (divRaw)  playerX += 2;
    if (playerX < 0) playerX = 0;
    if (playerX > 118) playerX = 118;

    // Shot: reset edge (only if the custom button isn't also pressed)
    if (resetRaw && !lastResetRaw && !customRaw && !bulletActive) {
      bulletActive = true;
      bulletX = playerX + 4;
      bulletY = playerY - 3;
    }
    lastResetRaw = resetRaw;

    // Move the invader block
    if (millis() - lastStepMs >= stepInterval) {
      lastStepMs = millis();
      blockX += dir * 4;
      if (blockX <= 2 || blockX + blockWidth >= 126) {
        dir = -dir;
        blockY += 4;
      }
      frameToggle = !frameToggle;
    }

    // Move shot + collision
    if (bulletActive) {
      bulletY -= 3;
      if (bulletY < 0) {
        bulletActive = false;
      } else {
        for (uint8_t r = 0; r < currentRows && bulletActive; r++) {
          for (uint8_t c = 0; c < currentCols && bulletActive; c++) {
            if (!alive[r][c]) continue;
            int ix = blockX + c * colSpacing;
            int iy = blockY + r * rowSpacing;
            if (bulletX >= ix - 2 && bulletX <= ix + 10 && bulletY >= iy && bulletY <= iy + 8) {
              alive[r][c] = false;
              bulletActive = false;
              score += 10 * level; // higher level = more points per hit
            }
          }
        }
      }
    }

    // Win (level cleared) / loss check
    bool anyAlive = false;
    int lowestY = blockY;
    for (uint8_t r = 0; r < currentRows; r++) {
      for (uint8_t c = 0; c < currentCols; c++) {
        if (alive[r][c]) {
          anyAlive = true;
          int y = blockY + r * rowSpacing;
          if (y > lowestY) lowestY = y;
        }
      }
    }

    if (!anyAlive) {
      if (level >= MAX_LEVEL) {
        // all 10 levels cleared -> game fully won
        saveHighscoreIfBeaten(score);
        u8g2.clearBuffer();
        const char* msg = "ALL LEVELS CLEARED!";
        int w = u8g2.getStrWidth(msg);
        u8g2.drawStr((128 - w) / 2, 26, msg);
        char sbuf[20];
        sprintf(sbuf, "SCORE %lu", (unsigned long)score);
        int w2 = u8g2.getStrWidth(sbuf);
        u8g2.drawStr((128 - w2) / 2, 40, sbuf);
        if (score > highscore) {
          const char* nh = "NEW HIGHSCORE!";
          int w3 = u8g2.getStrWidth(nh);
          u8g2.drawStr((128 - w3) / 2, 52, nh);
        }
        u8g2.sendBuffer();
        delay(2200);
        break;
      } else {
        // Next level
        u8g2.clearBuffer();
        char lbuf[20];
        sprintf(lbuf, "LEVEL %u CLEARED!", level);
        int w = u8g2.getStrWidth(lbuf);
        u8g2.drawStr((128 - w) / 2, 30, lbuf);
        char sbuf[16];
        sprintf(sbuf, "SCORE %lu", (unsigned long)score);
        int w2 = u8g2.getStrWidth(sbuf);
        u8g2.drawStr((128 - w2) / 2, 42, sbuf);
        u8g2.sendBuffer();
        delay(1400);
        level++;
        setupLevel();
        continue;
      }
    }

    if (lowestY + 8 >= playerY) {
      saveHighscoreIfBeaten(score);
      u8g2.clearBuffer();
      const char* msg = "GAME OVER";
      int w = u8g2.getStrWidth(msg);
      u8g2.drawStr((128 - w) / 2, 24, msg);
      char lbuf[16];
      sprintf(lbuf, "LEVEL %u", level);
      int wl = u8g2.getStrWidth(lbuf);
      u8g2.drawStr((128 - wl) / 2, 36, lbuf);
      char sbuf[16];
      sprintf(sbuf, "SCORE %lu", (unsigned long)score);
      int w2 = u8g2.getStrWidth(sbuf);
      u8g2.drawStr((128 - w2) / 2, 48, sbuf);
      if (score > highscore) {
        const char* nh = "NEW HIGHSCORE!";
        int w3 = u8g2.getStrWidth(nh);
        u8g2.drawStr((128 - w3) / 2, 60, nh);
      }
      u8g2.sendBuffer();
      delay(2200);
      break;
    }

    // Render
    u8g2.clearBuffer();
    char sbuf[24];
    sprintf(sbuf, "L%u SCORE %lu", level, (unsigned long)score);
    u8g2.drawStr(2, 7, sbuf);

    for (uint8_t r = 0; r < currentRows; r++) {
      for (uint8_t c = 0; c < currentCols; c++) {
        if (alive[r][c]) {
          u8g2.drawXBMP(blockX + c * colSpacing, blockY + r * rowSpacing, 8, 8,
                        frameToggle ? invaderFrameA : invaderFrameB);
        }
      }
    }

    u8g2.drawBox(playerX, playerY, 10, 4);
    if (bulletActive) {
      u8g2.drawBox(bulletX, bulletY, 2, 4);
    }

    u8g2.sendBuffer();
    delay(30);
  }
}

// ---------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------
uint32_t lastRenderMs = 0;
const uint32_t RENDER_INTERVAL_MS = 20; // more frequent updates for smoother blink transitions
// The Analyzer screen has no blink animation to keep smooth, and its
// render is the heaviest one on the whole board (see Rmax diagnostic).
// Refreshing it this much less often directly cuts how often that
// multi-ms stall can even land on top of an incoming MIDI byte -
// doesn't eliminate a single stall's length, but roughly quarters how
// frequently one can occur.
const uint32_t ANALYZER_RENDER_INTERVAL_MS = 80;

void loop() {
  // Drain all pending MIDI bytes, not just one: if something earlier
  // in this loop() (most notably renderAnalyzer()'s SPI framebuffer
  // push) takes a few ms, one or more clock bytes can pile up in the
  // UART's hardware FIFO in the meantime. Reading only one per
  // iteration would then process them late and unevenly - exactly the
  // kind of artificial jitter the Analyzer is trying to measure, so a
  // slow display update must never get to masquerade as a signal
  // problem. Same fix already applied in runMidiWarGame() for the
  // same reason - see the comment there.
  while (MIDI.read()) {}

  // Gimmick: custom button+reset pressed simultaneously -> MidiWar
  if (digitalRead(PIN_BTN_CUSTOM) == LOW && digitalRead(PIN_BTN_RESET) == LOW) {
    suppressCustomAction = true;
    suppressResetAction   = true;
    runMidiWarGame();
    // Wait until both buttons are released again, so exiting the
    // game doesn't also trigger a normal function.
    while (digitalRead(PIN_BTN_CUSTOM) == LOW || digitalRead(PIN_BTN_RESET) == LOW) {
      delay(10);
    }
    lastClockTickMillis = millis(); // restart standby timer after the game
    return;
  }

  updateButton(btnDivisor, onDivisorButton);
  updateButton(btnCustom,  onCustomButton);
  updateButton(btnReset,   onResetButton);

  // Nudge mode: custom button+divisor simultaneously -> in and out
  // (toggle, same combo). Uses the already-debounced stableState
  // values of the buttons (same debouncing as normal time signature/
  // divisor selection). "armed" flag prevents a still-held combo
  // press from immediately counting as exit again - both buttons must
  // be fully released before the combo can trigger again ("press again").
  static bool    nudgeComboArmed = true;
  static uint8_t nudgeSoloButton = 0; // 0=none, 1=custom, 2=divisor (currently held alone)
  {
    bool customDown = (btnCustom.stableState == LOW);
    bool divDown   = (btnDivisor.stableState == LOW);
    bool comboNow  = customDown && divDown;

    if (comboNow && nudgeComboArmed &&
        (currentMode == MODE_NORMAL || currentMode == MODE_NUDGE)) {
      currentMode = (currentMode == MODE_NORMAL) ? MODE_NUDGE : MODE_NORMAL;
      if (currentMode == MODE_NUDGE) nudgeOffsetSixteenths = 0;
      suppressCustomAction = true;
      suppressDivisorAction = true;
      nudgeComboArmed = false;
      nudgeSoloButton = 0; // discard any ongoing solo detection
    }
    if (!customDown && !divDown) {
      nudgeComboArmed = true; // both released -> combo armed again
    }

    // Single nudge step: triggered only on release (no auto-repeat
    // while held). "nudgeComboArmed" as an extra condition prevents a
    // not-yet-fully-released combo remnant (one finger still lingers)
    // from incorrectly counting as a new solo press.
    if (currentMode == MODE_NUDGE) {
      if (comboNow) {
        nudgeSoloButton = 0;
      } else if (customDown && !divDown) {
        if (nudgeSoloButton == 0 && nudgeComboArmed) nudgeSoloButton = 1;
      } else if (divDown && !customDown) {
        if (nudgeSoloButton == 0 && nudgeComboArmed) nudgeSoloButton = 2;
      } else if (nudgeSoloButton != 0) {
        doNudge(nudgeSoloButton == 1 ? -6 : +6);
        nudgeSoloButton = 0;
      }
    } else {
      nudgeSoloButton = 0;
    }
  }

  // Detect the reset button's escalation threshold (Reset 1 -> Reset
  // 2) already while held, not just on release. Uses its own
  // RESET2_HOLD_MS (2s) instead of the generic LONGPRESS_MS, which is
  // still used unchanged for the custom button and menu navigation.
  if (btnReset.isPressed) {
    uint32_t heldMs = millis() - btnReset.pressStartMs;
    if (!resetLongAlreadyHandled && heldMs >= RESET2_HOLD_MS) {
      onResetMediumHeldDuringPress();
      resetLongAlreadyHandled = true;
    }
  } else {
    resetLongAlreadyHandled = false;
  }

  // Detect long press of the custom button already while held
  // -> toggles the MIDI analyzer (in both directions).
  if (btnCustom.isPressed) {
    if (!customLongAlreadyHandled && (millis() - btnCustom.pressStartMs) >= LONGPRESS_MS) {
      onCustomButtonLongHeldDuringPress();
      customLongAlreadyHandled = true;
    }
  } else {
    customLongAlreadyHandled = false;
  }

  // Every button press also counts as "activity" and keeps standby
  // away / cancels a running countdown (not just MIDI bytes).
  if (btnDivisor.isPressed || btnCustom.isPressed || btnReset.isPressed) {
    lastClockTickMillis = millis();
  }

  // MIDI analyzer: sample the current run/stop state (derived from MIDI
  // Start/Stop/Continue) into the ring buffer every
  // MIDI_RUNSTOP_SAMPLE_INTERVAL_MS, regardless of whether the analyzer
  // screen is currently shown, so its trace always has a full window of
  // history ready as soon as you switch to it.
  if (millis() - lastMidiRunStopSampleMs >= MIDI_RUNSTOP_SAMPLE_INTERVAL_MS) {
    lastMidiRunStopSampleMs = millis();
    midiRunStopHistory[midiRunStopHistIndex] = isRunning;
    midiRunStopHistIndex++;
    if (midiRunStopHistIndex >= MIDI_RUNSTOP_HISTORY_SIZE) {
      midiRunStopHistIndex = 0;
      midiRunStopHistFull = true;
    }
  }

  // Clock watchdog: the clock is running, but no tick has arrived for
  // CLOCK_LOST_TIMEOUT_MS -> treat the clock as "lost", reset
  // everything and go to STOP.
  if (isRunning && (millis() - lastClockTickMillis > CLOCK_LOST_TIMEOUT_MS)) {
    handleClockLost();
  }

  // Standby: no activity at all since the configured timeout
  // (MIDI or button, regardless of whether currently "running" or
  // "stopped") -> light sleep, wakeup via MIDI byte or button press.
  // A countdown appears 10s ahead as a warning. Only active if
  // STANDBY is enabled in the menu.
  if (settings.standbyEnabled) {
    uint32_t standbyTimeoutMs = getStandbyTimeoutMs();
    uint32_t idleMs = millis() - lastClockTickMillis;
    if (idleMs > standbyTimeoutMs) {
      enterStandby();
    } else if (standbyTimeoutMs > STANDBY_COUNTDOWN_MS && idleMs > standbyTimeoutMs - STANDBY_COUNTDOWN_MS) {
      uint32_t msLeft = standbyTimeoutMs - idleMs;
      uint32_t secsLeft = (msLeft + 999) / 1000; // round up
      if (millis() - lastRenderMs >= RENDER_INTERVAL_MS) {
        lastRenderMs = millis();
        renderStandbyCountdown(secsLeft);
      }
      return; // don't render the normal display while the countdown is active
    }
  }

  updateBlinkStates();

  uint32_t effectiveRenderIntervalMs = (currentMode == MODE_ANALYZER) ? ANALYZER_RENDER_INTERVAL_MS : RENDER_INTERVAL_MS;
  if (millis() - lastRenderMs >= effectiveRenderIntervalMs) {
    lastRenderMs = millis();
    uint32_t renderStartUs = micros();
    if (currentMode == MODE_ANALYZER) {
      renderAnalyzer();
    } else {
      render();
    }
    lastRenderUs = micros() - renderStartUs;
    if (millis() - maxRenderWindowStartMs > 2000) {
      maxRenderUs = lastRenderUs; // window rolled over - start fresh
      maxRenderWindowStartMs = millis();
    } else if (lastRenderUs > maxRenderUs) {
      maxRenderUs = lastRenderUs;
    }
  }
}
