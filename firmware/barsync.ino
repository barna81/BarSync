/*
 * ============================================================================
 *  BARSYNC — MIDI Clock Bar Counter & Visualizer — ESP32 + SSD1309 OLED (SPI, 128x64)
 *  Version: 1.0.1
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

#define FW_VERSION "1.0.1"


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
#define PIN_BTN_TIMESIG 32
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
bool suppressTimeSigAction = false;
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

struct Settings {
  uint8_t timeSigIndex;
  uint8_t divisorIndex;
  uint8_t contrast;
  bool    invert;       // true color inversion (controller command)
  uint8_t enabledTimeSigMask; // bit i = time signature i selectable
  uint8_t enabledDivisorMask; // bit i = divisor i selectable
  bool    standbyEnabled;
  uint8_t standbyDelayIndex; // index into STANDBY_DELAY_MINUTES
};
Settings settings;

const uint8_t CONTRAST_STEPS[] = {50, 100, 150, 200, 255};
const uint8_t CONTRAST_STEP_COUNT = 5;

const uint16_t STANDBY_DELAY_MINUTES[] = {1, 5, 10, 30, 60};
const uint8_t  STANDBY_DELAY_COUNT = 5;
uint32_t getStandbyTimeoutMs() {
  return (uint32_t)STANDBY_DELAY_MINUTES[settings.standbyDelayIndex] * 60000UL;
}

bool isTimeSigEnabled(uint8_t i) { return (settings.enabledTimeSigMask >> i) & 0x01; }
bool isDivisorEnabled(uint8_t i) { return (settings.enabledDivisorMask >> i) & 0x01; }

uint8_t countEnabledTimeSig() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) if (isTimeSigEnabled(i)) c++;
  return c;
}
uint8_t countEnabledDivisor() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < DIVISOR_COUNT; i++) if (isDivisorEnabled(i)) c++;
  return c;
}

// Find the next enabled index (wrap-around), for normal
// cycling via button during operation.
uint8_t nextEnabledTimeSig(uint8_t current) {
  uint8_t idx = current;
  for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) {
    idx = (idx + 1) % TIME_SIG_COUNT;
    if (isTimeSigEnabled(idx)) return idx;
  }
  return current;
}
uint8_t nextEnabledDivisor(uint8_t current) {
  uint8_t idx = current;
  for (uint8_t i = 0; i < DIVISOR_COUNT; i++) {
    idx = (idx + 1) % DIVISOR_COUNT;
    if (isDivisorEnabled(idx)) return idx;
  }
  return current;
}

void loadSettings() {
  prefs.begin("midiclock", true);
  settings.timeSigIndex = prefs.getUChar("tsig", 0);
  settings.divisorIndex = prefs.getUChar("div", 2);
  settings.contrast     = prefs.getUChar("contrast", 255);
  settings.invert       = prefs.getBool("invert", false);
  settings.enabledTimeSigMask = prefs.getUChar("tsigmask", 0xFF);
  settings.enabledDivisorMask = prefs.getUChar("divmask", 0xFF);
  settings.standbyEnabled     = prefs.getBool("stbyon", true);
  settings.standbyDelayIndex  = prefs.getUChar("stbydelay", 1); // default 5 min
  prefs.end();

  // Apply loaded values to the active runtime variables
  if (settings.timeSigIndex >= TIME_SIG_COUNT) settings.timeSigIndex = 0;
  if (settings.divisorIndex >= DIVISOR_COUNT)  settings.divisorIndex = 2;
  if (countEnabledTimeSig() == 0) settings.enabledTimeSigMask = 0xFF; // safety net
  if (countEnabledDivisor() == 0) settings.enabledDivisorMask = 0xFF;
  if (settings.standbyDelayIndex >= STANDBY_DELAY_COUNT) settings.standbyDelayIndex = 1;
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
  prefs.putUChar("tsigmask", settings.enabledTimeSigMask);
  prefs.putUChar("divmask", settings.enabledDivisorMask);
  prefs.putBool("stbyon", settings.standbyEnabled);
  prefs.putUChar("stbydelay", settings.standbyDelayIndex);
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
  settings.enabledTimeSigMask = 0xFF;
  settings.enabledDivisorMask = 0xFF;
  settings.standbyEnabled     = true;
  settings.standbyDelayIndex  = 1;
  timeSigIndex = settings.timeSigIndex;
  divisorIndex = settings.divisorIndex;
}



// Reset request (quantized, only takes effect at the end of the divisor cycle)
volatile uint8_t resetMode = 0; // 0 = no reset, 1 = short (bar only), 2 = long (bar+time)
// Timestamp since a reset was registered - acts as a minimum lockout
// against mechanical contact bounce of the footswitch, which would
// otherwise immediately cancel the just-registered reset again as an
// (unwanted) second press.
uint32_t resetRegisteredAtMs = 0;
const uint32_t RESET_DEBOUNCE_CANCEL_MS = 300;

// Instant flash feedback on button press (independent of the quantized
// execution at bar end) - blinks briefly 2-3x.
volatile bool    resetFlashActive   = false;
volatile uint32_t resetFlashStartMs = 0;
volatile uint8_t resetFlashKind     = 0; // 1=short, 2=medium, 3=very long (also A)
const uint32_t   RESET_FLASH_TOTAL_MS  = 700; // total duration of the flash animation
const uint32_t   RESET_FLASH_PERIOD_MS = 120; // toggle rate (~3 flashes in 700ms)

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
bool timeSigLongAlreadyHandled = false;

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
  bool     lastReading   = HIGH;
  bool     stableState   = HIGH;
  uint32_t lastChangeMs  = 0;
  uint32_t pressStartMs  = 0;
  bool     isPressed     = false;
};

Button btnDivisor{PIN_BTN_DIVISOR};
Button btnTimeSig{PIN_BTN_TIMESIG};
Button btnReset{PIN_BTN_RESET};

// Prevents a long press triggered while held from being processed a
// second time on the later release.
bool resetLongAlreadyHandled = false;

const uint32_t DEBOUNCE_MS   = 50;
const uint32_t LONGPRESS_MS  = 1000;  // "medium" threshold (1-3s)
const uint32_t VERYLONG_MS   = 3000;  // "very long" threshold (>=3s)

// Callback type for "pressed briefly" / "held long"
typedef void (*ButtonCallback)(bool longPress);

// Manual prototype: prevents the Arduino IDE from inserting a
// (faulty) prototype at the very top of the file before
// struct Button/ButtonCallback are known there.
void updateButton(Button &b, ButtonCallback onRelease);

void updateButton(Button &b, ButtonCallback onRelease) {
  bool reading = digitalRead(b.pin);

  if (reading != b.lastReading) {
    b.lastChangeMs = millis();
  }

  if ((millis() - b.lastChangeMs) > DEBOUNCE_MS) {
    if (reading != b.stableState) {
      b.stableState = reading;
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
  b.lastReading = reading;
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

void onTimeSigButton(bool longPress) {
  // If the long press already toggled the analyzer while held,
  // do nothing more here (prevents a double action / an accidental
  // further cycling of the time signature on release).
  if (timeSigLongAlreadyHandled) {
    timeSigLongAlreadyHandled = false;
    return;
  }
  if (suppressTimeSigAction) { suppressTimeSigAction = false; return; } // was only a wake-up press
  if (currentMode != MODE_NORMAL) return; // no accidental change in nudge mode/analyzer
  timeSigIndex = nextEnabledTimeSig(timeSigIndex);
  saveQuickState(); // save the latest state immediately, kept after restart
}

// Called once the time signature button has been held for 1s without
// release -> toggles between normal operation and MIDI analyzer (in
// both directions with the same button press).
void onTimeSigLongHeldDuringPress() {
  if (suppressTimeSigAction) return; // wake-up press does not trigger a long-press effect
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

void onResetButton(bool longPress) {
  // If the press was already handled while held
  // (see onResetMediumHeldDuringPress/onResetVeryLongHeldDuringPress),
  // do nothing more here.
  if (resetLongAlreadyHandled) {
    resetLongAlreadyHandled = false;
    return;
  }
  if (suppressResetAction) { suppressResetAction = false; return; } // was only a wake-up press

  if (!isRunning) {
    // In STOP mode no clock is running, so a quantized reset makes no
    // sense here. Instead: if there's anything at all to reset
    // (time/bar/divisor cycle), a FULL reset 3 is executed immediately
    // on any button press (no holding needed) (bar + divisor cycle +
    // time). If everything is already at zero, nothing happens (see
    // "NOTHING TO RESET" in the G display).
    if (hasSomethingToReset()) {
      currentBar  = 0;
      currentBeat = 0; // so E (beat bar) is also reset
      tickInBeat  = 0;
      totalTicks  = 0;
      startMillis = millis();
      pausedAt    = startMillis; // stays paused (STOP), but time shows 00:00
      resetFlashActive  = true;
      resetFlashStartMs = millis();
      resetFlashKind    = 3;
    }
    return;
  }

  if (resetMode != 0) {
    // A reset is already registered (stage 1-3) -> a short press
    // cancels it instead of overwriting it. Minimum lockout against
    // this: a mechanically bouncing footswitch can produce two press
    // events from a single tap - the second would otherwise
    // immediately cancel the reset just registered.
    if (millis() - resetRegisteredAtMs < RESET_DEBOUNCE_CANCEL_MS) {
      return; // likely contact bounce, ignore
    }
    resetMode = 0;
    return;
  }

  // Short press (<1s, on release):
  // Bar counter + divisor cycle are reset at the end of the current
  // bar - the fastest of the three stages.
  resetMode = 1;
  resetRegisteredAtMs = millis();

  // Trigger instant flash feedback ("this will be reset now")
  resetFlashActive   = true;
  resetFlashStartMs  = millis();
  resetFlashKind     = 1;
}

// Called once the reset button has been held for 1s (not just on
// release). If a reset is already registered, this renewed press
// takes it back (cancel), as long as it hasn't been executed yet.
void onResetMediumHeldDuringPress() {
  if (suppressResetAction) return; // wake-up press does not trigger an effect
  if (!isRunning) return; // nothing to reset in STOP mode
  if (resetMode == 0) {
    // Medium stage (1-3s): bar counter + divisor cycle are reset at
    // the end of the current divisor cycle (later than short).
    resetMode = 2;
    resetRegisteredAtMs = millis();
    resetFlashActive  = true;
    resetFlashStartMs = millis();
    resetFlashKind    = 2;
  } else {
    // An already-registered reset is taken back (no extra flash
    // needed - G simply disappears again, that's feedback enough).
    resetMode = 0;
  }
}

// Called once the reset button has been held for 3s -> upgrades an
// already-running medium registration to the strongest stage
// (additional time reset), still quantized to the end of the divisor
// cycle.
void onResetVeryLongHeldDuringPress() {
  if (suppressResetAction) return;
  if (!isRunning) return; // nothing to reset in STOP mode
  if (resetMode == 2) {
    resetMode = 3;
    resetFlashActive  = true;
    resetFlashStartMs = millis();
    resetFlashKind    = 3;
  }
}

// ---------------------------------------------------------------------------
// MIDI ANALYZER: ring buffer of the most recent tick intervals for jitter/stability
// ---------------------------------------------------------------------------
#define ANALYZER_HISTORY_SIZE 96 // = 4 Beats with 24 PPQN
volatile uint32_t tickIntervalHistory[ANALYZER_HISTORY_SIZE];
volatile uint8_t  analyzerHistIndex = 0;
volatile bool     analyzerHistFull  = false;
volatile uint32_t lastTickMicrosForAnalyzer = 0;

// Anchor for beat extrapolation: timestamp of the last REAL tick.
// Between two ticks, the display (E bar, beat blink) is smoothly
// extrapolated based on this, instead of visibly "jumping" with every
// small jitter of the source - the anchor is reset on every real tick,
// so a permanent drift can never occur.
volatile uint32_t lastTickAnchorMicros = 0;

// ---------------------------------------------------------------------------
// MIDI CALLBACKS
// ---------------------------------------------------------------------------
void handleClock() {
  lastClockTickMillis = millis(); // watchdog: "clock is still alive"

  uint32_t nowMicrosForAnchor = micros();
  lastTickAnchorMicros = nowMicrosForAnchor; // reset anchor for extrapolation

  // --- Analyzer: write tick interval into the ring buffer ---
  uint32_t nowMicrosA = nowMicrosForAnchor;
  if (lastTickMicrosForAnalyzer != 0) {
    tickIntervalHistory[analyzerHistIndex] = nowMicrosA - lastTickMicrosForAnalyzer;
    analyzerHistIndex++;
    if (analyzerHistIndex >= ANALYZER_HISTORY_SIZE) {
      analyzerHistIndex = 0;
      analyzerHistFull = true;
    }
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

  // --- Quantized reset ---
  // Stage 1 (short): at the end of the current bar (the running bar).
  // Stage 2+3 (medium/very long): at the end of the divisor cycle; stage 3
  // also resets time.
  if (resetMode == 1) {
    uint32_t ticksPerBar = (uint32_t)beatsPerBar() * 24;
    if ((totalTicks % ticksPerBar) == 0) {
      currentBar = 0;
      totalTicks = 0; // so F (divisor bar) starts over
      resetMode = 0;
    }
  } else if (resetMode == 2 || resetMode == 3) {
    uint32_t ticksPerCycle = (uint32_t)divisor() * beatsPerBar() * 24;
    if ((totalTicks % ticksPerCycle) == 0) {
      currentBar = 0;
      if (resetMode == 3) {
        startMillis = millis();
      }
      resetMode = 0;
    }
  }
}

void handleStart() {
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
  isRunning    = false;
  pausedAt     = millis();
  timeIsPaused = true;
}

void handleContinue() {
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
// MIDI ANALYZER DISPLAY (time signature button 1s hold to toggle on/off)
// ---------------------------------------------------------------------------
void renderAnalyzer() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(2, 8, "MIDI ANALYZER");

  noInterrupts();
  uint8_t histCount = analyzerHistFull ? ANALYZER_HISTORY_SIZE : analyzerHistIndex;
  uint32_t localHist[ANALYZER_HISTORY_SIZE];
  for (uint8_t i = 0; i < histCount; i++) localHist[i] = tickIntervalHistory[i];
  uint32_t lastTickMs = lastClockTickMillis;
  bool     runningSnap = isRunning;
  interrupts();

  if (histCount < 2) {
    u8g2.drawStr(2, 24, "Waiting for clock...");
    u8g2.sendBuffer();
    return;
  }

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

  uint32_t sumIv = 0;
  for (uint8_t i = 0; i < histCount; i++) sumIv += localHist[i];
  uint32_t avgIv = sumIv / histCount;
  uint32_t jitter = (maxIv > minIv) ? (maxIv - minIv) : 0;
  uint32_t jitterPct = (avgIv > 0) ? (jitter * 100 / avgIv) : 0;

  // most recent interval = the one directly before the current write index
  uint32_t curIv = localHist[(analyzerHistIndex + ANALYZER_HISTORY_SIZE - 1) % ANALYZER_HISTORY_SIZE];

  float bpmMin = (maxIv > 0) ? (60000000.0f / ((float)maxIv * 24.0f)) : 0;
  float bpmMax = (minIv > 0) ? (60000000.0f / ((float)minIv * 24.0f)) : 0;

  char line[28];
  sprintf(line, "Interv: %lu.%01lums", (unsigned long)(curIv / 1000), (unsigned long)((curIv / 100) % 10));
  u8g2.drawStr(2, 19, line);

  sprintf(line, "P5/P95:%lu.%01lu/%lu.%01lums",
          (unsigned long)(minIv / 1000), (unsigned long)((minIv / 100) % 10),
          (unsigned long)(maxIv / 1000), (unsigned long)((maxIv / 100) % 10));
  u8g2.drawStr(2, 29, line);

  sprintf(line, "Jitter:+-%lu.%01lums(%lu%%)",
          (unsigned long)(jitter / 1000), (unsigned long)((jitter / 100) % 10),
          (unsigned long)jitterPct);
  u8g2.drawStr(2, 39, line);

  char bpmMinBuf[8], bpmMaxBuf[8];
  dtostrf(bpmMin, 3, 1, bpmMinBuf);
  dtostrf(bpmMax, 3, 1, bpmMaxBuf);
  sprintf(line, "BPM: %s-%s", bpmMinBuf, bpmMaxBuf);
  u8g2.drawStr(2, 49, line);

  // Mini graph: last min(histCount,24) intervals as bars
  const int graphX = 2, graphY = 62, graphH = 10, barW = 5, barGap = 1;
  uint8_t graphSamples = histCount < 24 ? histCount : 24;
  uint32_t gMin = 0xFFFFFFFF, gMax = 0;
  for (uint8_t i = 0; i < graphSamples; i++) {
    uint8_t idx = (analyzerHistIndex + ANALYZER_HISTORY_SIZE - graphSamples + i) % ANALYZER_HISTORY_SIZE;
    uint32_t v = localHist[idx];
    if (v < gMin) gMin = v;
    if (v > gMax) gMax = v;
  }
  uint32_t gRange = (gMax > gMin) ? (gMax - gMin) : 1;
  for (uint8_t i = 0; i < graphSamples; i++) {
    uint8_t idx = (analyzerHistIndex + ANALYZER_HISTORY_SIZE - graphSamples + i) % ANALYZER_HISTORY_SIZE;
    uint32_t v = localHist[idx];
    int h = 2 + (int)(((v - gMin) * (graphH - 2)) / gRange);
    int x = graphX + i * (barW + barGap);
    u8g2.drawBox(x, graphY - h, barW, h);
  }

  // Status bottom right: clock currently running / how long since nothing more
  uint32_t sinceLastTick = millis() - lastTickMs;
  char statusBuf[16];
  if (!runningSnap) {
    sprintf(statusBuf, "STOP");
  } else if (sinceLastTick > 999) {
    sprintf(statusBuf, "%lus", (unsigned long)(sinceLastTick / 1000));
  } else {
    sprintf(statusBuf, "OK");
  }
  int stW = u8g2.getStrWidth(statusBuf);
  u8g2.drawStr(126 - stW, 8, statusBuf);

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
  bool     flashActiveSnap = resetFlashActive;
  uint32_t flashStartSnap  = resetFlashStartMs;
  uint8_t  flashKindSnap   = resetFlashKind;
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

  // Instant flash feedback after a button press: B+F (and with option
  // 2 also A) blink briefly 2-3x, independent of the actual quantized
  // reset time.
  bool flashHideBar  = false;
  bool flashHideTime = false;
  if (flashActiveSnap) {
    uint32_t flashElapsed = millis() - flashStartSnap;
    if (flashElapsed >= RESET_FLASH_TOTAL_MS) {
      resetFlashActive = false; // animation done
    } else {
      bool suppress = ((flashElapsed / RESET_FLASH_PERIOD_MS) % 2) == 1;
      flashHideBar  = suppress;
      flashHideTime = suppress && (flashKindSnap == 3);
    }
  }

  // As long as a reset is registered (but not yet executed), blink B
  // continuously (every option) and also A (only option 2) in time
  // with the beat - as a persistent note "a reset is pending here".
  bool pendingHideBar  = (resetSnap != 0) && !beatPulseOnNormal;
  bool pendingHideTime = (resetSnap == 3) && !beatPulseOnNormal;

  bool hideBar  = flashHideBar  || pendingHideBar;
  bool hideTime = flashHideTime || pendingHideTime;

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
    // Live feedback while held: a bar sweeps across the text and
    // inverts it as it goes (XOR mode) - shows live how close you are
    // to the next reset stage. Replaces RUN/STOP for the duration of the hold.
    uint32_t heldMs = millis() - btnReset.pressStartMs;
    const char* label;
    float progress;
    if (heldMs < LONGPRESS_MS) {
      label = "RESET 2";
      progress = (float)heldMs / (float)LONGPRESS_MS;
    } else if (heldMs < VERYLONG_MS) {
      label = "RESET 3";
      progress = (float)(heldMs - LONGPRESS_MS) / (float)(VERYLONG_MS - LONGPRESS_MS);
    } else {
      label = "RESET 3";
      progress = 1.0f;
    }
    if (progress > 1.0f) progress = 1.0f;

    int w = u8g2.getStrWidth(label);
    int barW = w + 10; // a bit wider than the text
    int barX = (128 - barW) / 2;
    int fillW = (int)(barW * progress);

    u8g2.setDrawColor(1);
    u8g2.drawStr((128 - w) / 2, 7, label);
    u8g2.setDrawColor(2); // XOR: inverts the text wherever the bar passes over it
    if (fillW > 0) {
      u8g2.drawBox(barX, 0, fillW, 9);
    }
    u8g2.setDrawColor(1);
  } else if (resetSnap != 0) {
    // Reset registered, waiting for quantized execution
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
const char* MENU_ITEM_NAMES[] = {
  "TIMESIG", "DIVISOR", "STANDBY", "CONTRAST", "INVERT", "DEFAULTS"
};
const uint8_t MENU_ITEM_COUNT = 6;

uint8_t menuIndex = 0;
bool    menuConfirmReset = false;
bool    menuExitRequested = false;
bool    menuSaveOnExit = true;
bool    menuLeavingArmed = false; // true once the top level has been held >=1s ("LEAVING MENU" blinks)
bool    menuWillRestart = false;  // true if the exit was triggered via the 3s threshold (-> restart)

// Sub-screen for the checkbox selection for time signature/divisor as well as standby
enum MenuSub { SUB_NONE, SUB_TIMESIG, SUB_DIVISOR, SUB_STANDBY };
MenuSub subScreen = SUB_NONE;
uint8_t subCursor = 0;

void menuGetValueStr(uint8_t item, char* buf, size_t bufLen) {
  switch (item) {
    case 0: snprintf(buf, bufLen, "%u USED", countEnabledTimeSig()); break;
    case 1: snprintf(buf, bufLen, "%u USED", countEnabledDivisor()); break;
    case 2:
      if (settings.standbyEnabled) {
        snprintf(buf, bufLen, "ON %uMIN", STANDBY_DELAY_MINUTES[settings.standbyDelayIndex]);
      } else {
        snprintf(buf, bufLen, "OFF");
      }
      break;
    case 3: snprintf(buf, bufLen, "%u", settings.contrast); break;
    case 4: snprintf(buf, bufLen, "%s", settings.invert ? "ON" : "OFF"); break;
    case 5: snprintf(buf, bufLen, "%s", menuConfirmReset ? "AGAIN=OK" : ""); break;
  }
}

void renderMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (subScreen == SUB_TIMESIG) {
    u8g2.drawStr(2, 9, "TIME SIG SELECT");
    for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) {
      int y = 20 + i * 8;
      char line[20];
      snprintf(line, sizeof(line), "[%s] %s", isTimeSigEnabled(i) ? "x" : " ", TIME_SIG_LABEL[i]);
      if (i == subCursor) u8g2.drawStr(2, y, ">");
      u8g2.drawStr(12, y, line);
    }
    u8g2.sendBuffer();
    return;
  }
  if (subScreen == SUB_DIVISOR) {
    u8g2.drawStr(2, 9, "DIVISOR SELECT");
    const uint8_t rowsPerCol = 4;
    for (uint8_t i = 0; i < DIVISOR_COUNT; i++) {
      uint8_t col = i / rowsPerCol;
      uint8_t row = i % rowsPerCol;
      int x = (col == 0) ? 2 : 68;
      int y = 20 + row * 10;
      char line[16];
      snprintf(line, sizeof(line), "[%s]x%-3u", isDivisorEnabled(i) ? "x" : " ", DIVISOR_VALUES[i]);
      if (i == subCursor) u8g2.drawStr(x, y, ">");
      u8g2.drawStr(x + 10, y, line);
    }
    u8g2.sendBuffer();
    return;
  }
  if (subScreen == SUB_STANDBY) {
    u8g2.drawStr(2, 9, "STANDBY SETUP");
    char line0[20];
    snprintf(line0, sizeof(line0), "STANDBY   %s", settings.standbyEnabled ? "ON" : "OFF");
    if (subCursor == 0) u8g2.drawStr(2, 24, ">");
    u8g2.drawStr(12, 24, line0);

    char line1[20];
    snprintf(line1, sizeof(line1), "TIME      %uMIN", STANDBY_DELAY_MINUTES[settings.standbyDelayIndex]);
    if (subCursor == 1) u8g2.drawStr(2, 36, ">");
    u8g2.drawStr(12, 36, line1);
    u8g2.sendBuffer();
    return;
  }

  if (subScreen == SUB_NONE && menuLeavingArmed) {
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

  u8g2.drawStr(2, 9, "** SETUP **");
  for (uint8_t i = 0; i < MENU_ITEM_COUNT; i++) {
    int y = 20 + i * 8;
    char valBuf[16];
    menuGetValueStr(i, valBuf, sizeof(valBuf));
    char line[26];
    snprintf(line, sizeof(line), "%-11s%s", MENU_ITEM_NAMES[i], valBuf);
    if (i == menuIndex) {
      u8g2.drawStr(2, y, ">");
    }
    u8g2.drawStr(12, y, line);
  }
  u8g2.sendBuffer();
}

// time signature button (in the menu = navigate up)
void onMenuUp(bool longPress) {
  if (subScreen == SUB_TIMESIG) {
    subCursor = (subCursor + TIME_SIG_COUNT - 1) % TIME_SIG_COUNT;
  } else if (subScreen == SUB_DIVISOR) {
    subCursor = (subCursor + DIVISOR_COUNT - 1) % DIVISOR_COUNT;
  } else if (subScreen == SUB_STANDBY) {
    subCursor = (subCursor + 2 - 1) % 2;
  } else {
    menuIndex = (menuIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
    menuConfirmReset = false;
  }
}

// Divisor button (in the menu = navigate down)
void onMenuDown(bool longPress) {
  if (subScreen == SUB_TIMESIG) {
    subCursor = (subCursor + 1) % TIME_SIG_COUNT;
  } else if (subScreen == SUB_DIVISOR) {
    subCursor = (subCursor + 1) % DIVISOR_COUNT;
  } else if (subScreen == SUB_STANDBY) {
    subCursor = (subCursor + 1) % 2;
  } else {
    menuIndex = (menuIndex + 1) % MENU_ITEM_COUNT;
    menuConfirmReset = false;
  }
}

// Reset button pressed briefly (in the menu = value change): in the sub-screen =
// toggle checkbox (at least 1 must stay active), otherwise = change
// menu item / enter sub-screen. Now called directly from
// runSettingsMenu() (no simple button-release callback anymore, since
// reset in the menu now has to distinguish several press durations).
void onMenuChange(bool longPress) {
  if (subScreen == SUB_TIMESIG) {
    bool curEnabled = isTimeSigEnabled(subCursor);
    if (curEnabled && countEnabledTimeSig() <= 1) return; // protect the last checkbox
    settings.enabledTimeSigMask ^= (1 << subCursor);
    return;
  }
  if (subScreen == SUB_DIVISOR) {
    bool curEnabled = isDivisorEnabled(subCursor);
    if (curEnabled && countEnabledDivisor() <= 1) return;
    settings.enabledDivisorMask ^= (1 << subCursor);
    return;
  }
  if (subScreen == SUB_STANDBY) {
    if (subCursor == 0) {
      settings.standbyEnabled = !settings.standbyEnabled;
    } else {
      settings.standbyDelayIndex = (settings.standbyDelayIndex + 1) % STANDBY_DELAY_COUNT;
    }
    return;
  }

  switch (menuIndex) {
    case 0:
      subScreen = SUB_TIMESIG;
      subCursor = 0;
      break;
    case 1:
      subScreen = SUB_DIVISOR;
      subCursor = 0;
      break;
    case 2:
      subScreen = SUB_STANDBY;
      subCursor = 0;
      break;
    case 3: {
      uint8_t idx = 0;
      for (uint8_t i = 0; i < CONTRAST_STEP_COUNT; i++) {
        if (CONTRAST_STEPS[i] == settings.contrast) { idx = i; break; }
      }
      settings.contrast = CONTRAST_STEPS[(idx + 1) % CONTRAST_STEP_COUNT];
      u8g2.setContrast(settings.contrast); // live preview
      break;
    }
    case 4:
      settings.invert = !settings.invert;
      u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6); // live preview
      break;
    case 5:
      if (!menuConfirmReset) {
        menuConfirmReset = true;
      } else {
        factoryResetSettings();
        menuConfirmReset = false;
      }
      break;
  }
}

void runSettingsMenu() {
  menuIndex = 0;
  menuConfirmReset = false;
  menuExitRequested = false;
  menuSaveOnExit = true;
  menuLeavingArmed = false;
  menuWillRestart = false;
  subScreen = SUB_NONE;
  subCursor = 0;

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
    updateButton(btnTimeSig, onMenuUp);
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
        if (subScreen != SUB_NONE) {
          // In a sub-screen: holding 1s goes one level up
          if (heldMs >= LONGPRESS_MS) {
            subScreen = SUB_NONE;
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
          // A genuine short press (whether in a sub-screen or above):
          // value change / enter sub-screen / toggle checkbox.
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
  pinMode(PIN_BTN_TIMESIG, INPUT_PULLUP);
  pinMode(PIN_BTN_RESET,   INPUT_PULLUP);

  btnDivisor.pin = PIN_BTN_DIVISOR;
  btnTimeSig.pin = PIN_BTN_TIMESIG;
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
  u8g2.setBusClock(4000000); // 4 MHz SPI, sufficient and stable for SSD1309
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
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_TIMESIG, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_RESET,   GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start(); // blocks here until a wake source triggers

  // --- Woke up ---
  u8g2.setPowerSave(0); // turn display back on

  // Disable GPIO wakeup again, otherwise the interrupt type remains
  // permanently active on the pins.
  gpio_wakeup_disable((gpio_num_t)PIN_MIDI_RX);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_DIVISOR);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_TIMESIG);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_RESET);

  // Check whether a button was the wake reason -> suppress its next
  // action, so the wake-up press doesn't trigger a function.
  // (No direct status bitmask API available for this wakeup path,
  // so instead: which button is still LOW right after waking.)
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    if (digitalRead(PIN_BTN_DIVISOR) == LOW) suppressDivisorAction = true;
    if (digitalRead(PIN_BTN_TIMESIG) == LOW) suppressTimeSigAction = true;
    if (digitalRead(PIN_BTN_RESET)   == LOW) suppressResetAction   = true;
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

    bool tsigRaw = (digitalRead(PIN_BTN_TIMESIG) == LOW);
    bool divRaw  = (digitalRead(PIN_BTN_DIVISOR) == LOW);
    bool resetRaw = (digitalRead(PIN_BTN_RESET) == LOW);

    if (!exitArmed) {
      // Only once at least one of the two start buttons has been
      // released is the combo armed again as an "exit" signal.
      if (!(tsigRaw && resetRaw)) exitArmed = true;
    } else if (tsigRaw && resetRaw) {
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

    if (tsigRaw) playerX -= 2;
    if (divRaw)  playerX += 2;
    if (playerX < 0) playerX = 0;
    if (playerX > 118) playerX = 118;

    // Shot: reset edge (only if time signature isn't also pressed)
    if (resetRaw && !lastResetRaw && !tsigRaw && !bulletActive) {
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

void loop() {
  MIDI.read();

  // Gimmick: time signature+reset pressed simultaneously -> MidiWar
  if (digitalRead(PIN_BTN_TIMESIG) == LOW && digitalRead(PIN_BTN_RESET) == LOW) {
    suppressTimeSigAction = true;
    suppressResetAction   = true;
    runMidiWarGame();
    // Wait until both buttons are released again, so exiting the
    // game doesn't also trigger a normal function.
    while (digitalRead(PIN_BTN_TIMESIG) == LOW || digitalRead(PIN_BTN_RESET) == LOW) {
      delay(10);
    }
    lastClockTickMillis = millis(); // restart standby timer after the game
    return;
  }

  updateButton(btnDivisor, onDivisorButton);
  updateButton(btnTimeSig, onTimeSigButton);
  updateButton(btnReset,   onResetButton);

  // Nudge mode: time signature+divisor simultaneously -> in and out
  // (toggle, same combo). Uses the already-debounced stableState
  // values of the buttons (same debouncing as normal time signature/
  // divisor selection). "armed" flag prevents a still-held combo
  // press from immediately counting as exit again - both buttons must
  // be fully released before the combo can trigger again ("press again").
  static bool    nudgeComboArmed = true;
  static uint8_t nudgeSoloButton = 0; // 0=none, 1=time signature, 2=divisor (currently held alone)
  {
    bool tsigDown  = (btnTimeSig.stableState == LOW);
    bool divDown   = (btnDivisor.stableState == LOW);
    bool comboNow  = tsigDown && divDown;

    if (comboNow && nudgeComboArmed &&
        (currentMode == MODE_NORMAL || currentMode == MODE_NUDGE)) {
      currentMode = (currentMode == MODE_NORMAL) ? MODE_NUDGE : MODE_NORMAL;
      if (currentMode == MODE_NUDGE) nudgeOffsetSixteenths = 0;
      suppressTimeSigAction = true;
      suppressDivisorAction = true;
      nudgeComboArmed = false;
      nudgeSoloButton = 0; // discard any ongoing solo detection
    }
    if (!tsigDown && !divDown) {
      nudgeComboArmed = true; // both released -> combo armed again
    }

    // Single nudge step: triggered only on release (no auto-repeat
    // while held). "nudgeComboArmed" as an extra condition prevents a
    // not-yet-fully-released combo remnant (one finger still lingers)
    // from incorrectly counting as a new solo press.
    if (currentMode == MODE_NUDGE) {
      if (comboNow) {
        nudgeSoloButton = 0;
      } else if (tsigDown && !divDown) {
        if (nudgeSoloButton == 0 && nudgeComboArmed) nudgeSoloButton = 1;
      } else if (divDown && !tsigDown) {
        if (nudgeSoloButton == 0 && nudgeComboArmed) nudgeSoloButton = 2;
      } else if (nudgeSoloButton != 0) {
        doNudge(nudgeSoloButton == 1 ? -6 : +6);
        nudgeSoloButton = 0;
      }
    } else {
      nudgeSoloButton = 0;
    }
  }

  // Detect two hold thresholds of the reset button already while held
  // (not just on release): 1s = medium, 3s = very long.
  static bool resetVeryLongHandled = false;
  if (btnReset.isPressed) {
    uint32_t heldMs = millis() - btnReset.pressStartMs;
    if (!resetLongAlreadyHandled && heldMs >= LONGPRESS_MS) {
      onResetMediumHeldDuringPress();
      resetLongAlreadyHandled = true;
    }
    if (resetLongAlreadyHandled && !resetVeryLongHandled && heldMs >= VERYLONG_MS) {
      onResetVeryLongHeldDuringPress();
      resetVeryLongHandled = true;
    }
  } else {
    resetLongAlreadyHandled = false;
    resetVeryLongHandled = false;
  }

  // Detect long press of the time signature button already while held
  // -> toggles the MIDI analyzer (in both directions).
  if (btnTimeSig.isPressed) {
    if (!timeSigLongAlreadyHandled && (millis() - btnTimeSig.pressStartMs) >= LONGPRESS_MS) {
      onTimeSigLongHeldDuringPress();
      timeSigLongAlreadyHandled = true;
    }
  } else {
    timeSigLongAlreadyHandled = false;
  }

  // Every button press also counts as "activity" and keeps standby
  // away / cancels a running countdown (not just MIDI bytes).
  if (btnDivisor.isPressed || btnTimeSig.isPressed || btnReset.isPressed) {
    lastClockTickMillis = millis();
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

  if (millis() - lastRenderMs >= RENDER_INTERVAL_MS) {
    lastRenderMs = millis();
    if (currentMode == MODE_ANALYZER) {
      renderAnalyzer();
    } else {
      render();
    }
  }
}
