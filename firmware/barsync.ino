/*
 * ============================================================================
 *  BARSYNC — MIDI Clock Bar Counter & Visualizer — ESP32 + SSD1309 OLED (SPI, 128x64)
 *  Version: 1.3.0
 * ============================================================================
 *
 * Counts incoming MIDI clock (24 PPQN), derives beat/bar from it, and shows
 * bar number, BPM, elapsed time, beat progress, and a multi-row divisor
 * progress grid (x1 to x64 bars) on a monochrome 128x64 OLED, in a
 * reduced "Elektron" style.
 *
 * Pattern Mode markers (up to 500, each pinned to a bar, one of 4
 * symbols) can be placed via OPERATION SETUP > SET MARKERS - see the
 * PATTERN MODE sections below. Two grid modes decide how the grid
 * shows them during normal playback (see gridMode, switched via
 * OPERATION SETUP > GRID MODE): CYCLING is the plain repeating divisor
 * grid; SCROLLING instead keeps the current bar fixed in the first
 * cell and scrolls markers in from the right as they approach, see
 * renderScrollingGrid(). Reset+Custom (held together) opens OPERATION
 * SETUP.
 *
 * REQUIRED LIBRARIES (Library Manager):
 *   - "MIDI Library" by Francois Best (FortySevenEffects)
 *   - "U8g2" by olikraus
 *
 * HARDWARE / PINOUT: see pinplan.md
 * CHANGE HISTORY: see CHANGELOG.md
 * ============================================================================
 */

#define FW_VERSION "1.3.0"


#include <MIDI.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_sleep.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h> // memset() - used by factoryResetSettings()/factoryResetPatternMarkers()

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
  bool    gridSeparatorsEnabled; // true (default): 32-bar group separators
                                  // in the divisor grid (x16/x32/x64/x128),
                                  // shared 35px reference footprint. false:
                                  // original layout, fixed 32px, no grouping.
                                  // Menu: Display > Grid Lines
};
Settings settings;

const uint8_t CONTRAST_STEPS[] = {25, 50, 100, 150, 200, 255};
const uint8_t CONTRAST_STEP_COUNT = 6;

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

// Same as nextEnabledDivisor() above, but only considers values within
// [minValue, maxValue] - used while SCROLLING grid mode is active,
// which only ever shows meaningfully at 2/4/8/16 (see
// onDivisorButton()) - 1 is excluded since a 1-bar window can't show
// any "scrolling ahead" at all, and above 16 the marker symbols
// wouldn't fit legibly (see SCROLLING_MAX_WINDOW_BARS).
uint8_t nextEnabledDivisorInRange(uint8_t current, uint8_t minValue, uint8_t maxValue) {
  uint8_t idx = current;
  for (uint8_t i = 0; i < DIVISOR_COUNT; i++) {
    idx = (idx + 1) % DIVISOR_COUNT;
    if (isDivisorEnabled(idx) && DIVISOR_VALUES[idx] >= minValue && DIVISOR_VALUES[idx] <= maxValue) return idx;
  }
  return current;
}

// Highest enabled divisor at or below 16 (DIVISOR_VALUES indices for
// 16,8,4,2, in that preference order) - used to initialize a sensible
// window size the moment SCROLLING mode is switched on (see
// onSetupMenuChange()'s GRID MODE row), same reasoning as
// nextEnabledDivisorInRange() above for why 1 is never a candidate.
uint8_t highestEnabledDivisorUpTo16() {
  for (int8_t i = 4; i >= 1; i--) { // DIVISOR_VALUES indices 4,3,2,1 = 16,8,4,2
    if (isDivisorEnabled(i)) return i;
  }
  return divisorIndex; // none of 2/4/8/16 enabled - leave unchanged, defensive
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
  settings.contrast     = prefs.getUChar("contrast", 50);
  settings.invert       = prefs.getBool("invert", false);
  settings.enabledDivisorMask = prefs.getUChar("divmask", 0xFC); // default: x4-x128 only, matches the Eurorack sibling
  settings.standbyEnabled     = prefs.getBool("stbyon", true);
  settings.standbyDelayIndex  = prefs.getUChar("stbydelay", 1); // default 5 min
  settings.customButtonRole   = prefs.getUChar("customrole", CUSTOM_ROLE_SET11);
  settings.enabledTimeSigMask = prefs.getUChar("tsigmask", 0xFF);
  settings.resetInstantMode   = prefs.getBool("rstinstant", false); // default: QUANTIZED
  settings.reset1PlaytimeEnabled = prefs.getBool("r1playtime", true);
  settings.reset2PlaytimeEnabled = prefs.getBool("r2playtime", true);
  settings.gridSeparatorsEnabled = prefs.getBool("gridseps", true);
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
  prefs.putBool("gridseps", settings.gridSeparatorsEnabled);
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
  settings.contrast     = 50;
  settings.invert       = false;
  settings.enabledDivisorMask = 0xFC; // x4-x128 only, matches the Eurorack sibling
  settings.standbyEnabled     = true;
  settings.standbyDelayIndex  = 1;
  settings.customButtonRole   = CUSTOM_ROLE_SET11;
  settings.enabledTimeSigMask = 0xFF;
  settings.resetInstantMode   = false;
  settings.reset1PlaytimeEnabled = true;
  settings.reset2PlaytimeEnabled = true;
  settings.gridSeparatorsEnabled = true;
  timeSigIndex = settings.timeSigIndex;
  divisorIndex = settings.divisorIndex;

  factoryResetPatternMarkers(); // pattern marker globals are declared further down - see there
}

// ---------------------------------------------------------------------------
// PATTERN MODE markers
// ---------------------------------------------------------------------------
// The ordinary, non-Song divisor grid - "the normal mode" - just with
// up to 500 optional markers, each pinned to one bar (1..9999), one of
// 4 simple, clearly distinguishable shapes (see SongSymbol/
// drawSongSymbol() below - no room for a character editor on this
// display with only 3 buttons and no encoder). Every marker is exactly
// 1 bar long. In CYCLING mode they only ever actually show up at
// x1/x2/x4/x8/x16, and only when their bar falls within the current
// cycle window - those are the only grid layouts whose cells map 1:1
// onto bar positions at all (never at x32/x64/x128, which group
// several bars per cell) - see the marker lookup in render()'s normal
// grid loop. SCROLLING mode (see renderScrollingGrid()) only ever
// operates within that same 2-16 range in the first place (see
// onDivisorButton()), so every marker there is always potentially
// visible once playback scrolls close enough. A marker set far beyond
// the current window is still stored and editable either way, it
// simply won't be visible until playback reaches it.
enum SongSymbol { SONG_SYM_SQUARE = 0, SONG_SYM_CIRCLE = 1, SONG_SYM_TRIANGLE = 2, SONG_SYM_X = 3 };
const uint8_t SONG_SYMBOL_COUNT = 4;
const char* SONG_SYMBOL_LABEL[SONG_SYMBOL_COUNT] = {"SQUARE", "CIRCLE", "TRIANGLE", "X"};

// Selects how drawSongSymbol() renders a marker symbol - see there for
// the full explanation of each.
enum SongSymbolFill { SYMFILL_OUTLINE = 0, SYMFILL_SOLID = 1, SYMFILL_CUTOUT = 2 };

// Manual prototype - Arduino's ctags-based auto-prototype generator
// doesn't reliably handle a custom enum as a parameter type (same
// issue this file has hit before with other menu enums - see
// SetupMenuScreen further down), producing a broken forward
// declaration that leaves SongSymbolFill undeclared at the point
// drawSongSymbol() is actually defined. Declaring it explicitly here,
// right after the enum itself, pre-empts that broken auto-generation.
void drawSongSymbol(uint8_t symbol, int x, int y, int w, int h, SongSymbolFill fill);

// The two ways the main grid displays during normal (non-editing)
// playback - see render()'s grid section further down. CYCLING is the
// original, always-there behavior: a plain repeating N-bar cycle,
// bar 1 of the cycle through the last bar, over and over. SCROLLING
// instead keeps the current bar fixed in the first cell and scrolls
// the timeline of upcoming markers toward it (see
// renderScrollingGrid()) - the same idea the old Song Mode used for its
// section symbols, just driven by patternMarkers now that Song Mode
// itself is gone.
enum GridMode { GRIDMODE_CYCLING = 0, GRIDMODE_SCROLLING = 1 };
const uint8_t GRIDMODE_COUNT = 2;
const char* GRIDMODE_LABEL[GRIDMODE_COUNT] = {"CYCLE", "SCROLL"};
uint8_t gridMode = GRIDMODE_CYCLING;

// End Bar - set via Operation Setup, independent of gridMode (applies
// the same way under CYCLE and SCROLL): once playback would move
// past the resolved end bar (see resolveEndBar()), endBarAction
// decides what happens - see the check in handleClock().
//   OFF         - the whole feature is disabled, currentBar just
//                 counts up forever like it always has
//   MANUAL      - a fixed bar number (endBarManualValue), stepped in
//                 groups of 4 (see stepEndBar()) since bars are
//                 musically grouped that way far more often than not.
//                 Only mode with an actual value to set - see the
//                 BAR NUMBER row (ROW_END_BAR_NUMBER) in Operation
//                 Setup, only shown while this mode is active.
//   LAST_MARKER - automatically follows whichever Pattern Mode marker
//                 currently sits furthest out (patternMarkers[] is
//                 kept sorted ascending, so that's just the last
//                 entry) - if there are no markers at all, this
//                 resolves the same as OFF
enum EndBarMode { ENDBAR_MODE_OFF = 0, ENDBAR_MODE_MANUAL = 1, ENDBAR_MODE_LAST_MARKER = 2 };
const uint8_t ENDBAR_MODE_COUNT = 3;
const char* ENDBAR_MODE_LABEL[ENDBAR_MODE_COUNT] = {"OFF", "MANUAL", "LAST MARKER"};
uint8_t  endBarMode        = ENDBAR_MODE_OFF;
uint32_t endBarManualValue = 4; // only meaningful while endBarMode == ENDBAR_MODE_MANUAL - always a multiple of 4

enum EndBarAction { ENDBAR_LOOP = 0, ENDBAR_STOP = 1, ENDBAR_CONTINUE = 2 };
const uint8_t ENDBAR_ACTION_COUNT = 3;
const char* ENDBAR_ACTION_LABEL[ENDBAR_ACTION_COUNT] = {"LOOP", "STOP", "CONTINUE"};
uint8_t endBarAction = ENDBAR_LOOP;
#define END_BAR_MAX 9999
#define END_BAR_STEP 4

// Cell size limit for SCROLLING mode: past this, cells in the shared
// 128x64 grid footprint get too small to tell the 4 marker symbols
// apart - the Grid/Divisor button simply skips any larger value while
// SCROLLING is active (see onDivisorButton()).
#define SCROLLING_MAX_WINDOW_BARS 16

#define PATTERN_MARKER_COUNT 500
struct PatternMarker {
  uint16_t bar; // 1..9999, the bar this marker sits on
  uint8_t  symbol;
};
uint16_t      patternMarkerCount = 0; // how many of the slots below are actually used - uint8_t isn't wide enough once PATTERN_MARKER_COUNT > 255
PatternMarker patternMarkers[PATTERN_MARKER_COUNT];

// Resolves whatever endBarMode currently means into one actual 1-based
// bar number to compare currentBar against - 0 means "no end bar right
// now" (OFF, or LAST_MARKER with nothing placed yet). patternMarkers[]
// is always kept sorted ascending by bar (see paintPatternMarkerAtCursor()),
// which is what makes "the last entry" the correct lookup for
// LAST_MARKER here.
uint32_t resolveEndBar() {
  switch (endBarMode) {
    case ENDBAR_MODE_MANUAL:
      return endBarManualValue;
    case ENDBAR_MODE_LAST_MARKER:
      return (patternMarkerCount > 0) ? patternMarkers[patternMarkerCount - 1].bar : 0;
    case ENDBAR_MODE_OFF:
    default:
      return 0;
  }
}

// Called from factoryResetSettings() above, which is defined earlier
// in this file than patternMarkers itself - Arduino auto-prototypes
// functions, but not plain global variables, so factoryResetSettings()
// calls this instead of touching patternMarkers directly.
void factoryResetPatternMarkers() {
  memset(patternMarkers, 0, sizeof(patternMarkers));
  patternMarkerCount = 0;
  gridMode = GRIDMODE_CYCLING;
  endBarMode        = ENDBAR_MODE_OFF;
  endBarManualValue = END_BAR_STEP;
  endBarAction      = ENDBAR_LOOP;
}

void loadPatternMarkers() {
  prefs.begin("midiclock", true);
  gridMode = prefs.getUChar("gridmode", GRIDMODE_CYCLING);
  endBarMode        = prefs.getUChar("endbarmode", ENDBAR_MODE_OFF);
  endBarManualValue = prefs.getUInt("endbarval", END_BAR_STEP);
  endBarAction      = prefs.getUChar("endbaract", ENDBAR_LOOP);
  patternMarkerCount = prefs.getUShort("patcount", 0);
  size_t got = prefs.getBytes("patmarkers", &patternMarkers, sizeof(patternMarkers));
  prefs.end();
  if (gridMode >= GRIDMODE_COUNT) gridMode = GRIDMODE_CYCLING; // safety net
  if (endBarMode >= ENDBAR_MODE_COUNT) endBarMode = ENDBAR_MODE_OFF; // safety net
  if (endBarManualValue == 0 || endBarManualValue > END_BAR_MAX) endBarManualValue = END_BAR_STEP; // safety net
  if (endBarAction >= ENDBAR_ACTION_COUNT) endBarAction = ENDBAR_LOOP; // safety net
  if (got != sizeof(patternMarkers)) { memset(patternMarkers, 0, sizeof(patternMarkers)); patternMarkerCount = 0; } // never saved before
  if (patternMarkerCount > PATTERN_MARKER_COUNT) patternMarkerCount = 0; // safety net
}

void savePatternMarkers() {
  prefs.begin("midiclock", false);
  prefs.putUChar("gridmode", gridMode);
  prefs.putUChar("endbarmode", endBarMode);
  prefs.putUInt("endbarval", endBarManualValue);
  prefs.putUChar("endbaract", endBarAction);
  prefs.putUShort("patcount", patternMarkerCount);
  prefs.putBytes("patmarkers", &patternMarkers, sizeof(patternMarkers));
  prefs.end();
}

// Looks up whether a marker sits on the ABSOLUTE bar that grid cell
// "gridIndex" (0-based, 0..15) currently represents. windowStartBar is
// the absolute (0-based) bar the x16 window currently begins at - so a
// marker only ever lights up on the one actual pass through its bar
// number, not on every repeat of the 16-bar cycle (which is what a
// plain relative-position match would give, and why a 9999-bar range
// wouldn't otherwise make sense on a 16-cell repeating grid). Markers
// store 1-based bar numbers, matching how bars are shown everywhere
// else in the UI. isFirstOfTypeOut is set to true when this marker is
// the first of a new run of its symbol - either the very first marker
// overall, or its symbol differs from the nearest earlier marker's
// (patternMarkers[] is kept sorted ascending by bar, so that's simply
// the previous array entry) - callers use this to highlight the start
// of each new symbol run (see SYMFILL_SOLID in render()).
bool findPatternMarkerSymbolAt(uint32_t windowStartBar, uint8_t gridIndex, uint8_t &symbolOut, bool &isFirstOfTypeOut) {
  uint32_t absoluteBar1Based = windowStartBar + gridIndex + 1;
  for (uint16_t i = 0; i < patternMarkerCount; i++) {
    if (patternMarkers[i].bar == absoluteBar1Based) {
      symbolOut = patternMarkers[i].symbol;
      isFirstOfTypeOut = (i == 0) || (patternMarkers[i].symbol != patternMarkers[i - 1].symbol);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// PATTERN MODE marker paint editor - a main-screen overlay, not a menu
// ---------------------------------------------------------------------------
// Entered from OPERATION SETUP's SET MARKERS item (Pattern Mode only -
// see onSetupMenuChange()/runOperationSetupMenu()). While active, Custom and
// Divisor no longer do their normal jobs - they move a cursor bar by
// bar (Custom = back, Divisor = forward, up to bar 9999) through the
// grid, paging to the next/previous 16-bar window automatically at the
// edges; held past 1s they instead page a full 16-bar window every
// PATTERN_EDIT_FAST_SCROLL_MS (see onCustomHeldDuringPress()/
// onDivisorHeldDuringPress()).
// Moving alone never paints - it's pure navigation. Holding Reset at
// the same time is what paints: every bar the cursor lands on while
// Reset is down - one step at a time, or while fast-scrolling - gets
// painted with whatever tool is currently selected (see
// paintPatternMarkerAtCursor()), so you sweep across several bars by
// holding Reset and moving. Reset on its own (a plain tap, not held
// into a move) just paints/uses the tool on the bar the cursor is
// already on.
//
// Tool selection itself is a separate gesture: Custom+Divisor pressed
// together (see the tool-cycle chord in loop()) cycles which tool is
// selected - the 4 symbols, then DEL - firing the instant the chord is
// recognized, no hold needed (selecting a tool is low-stakes, unlike
// the LEAVE combo below). DEL is a plain stop in that rotation like
// any other - it doesn't jump anywhere special, cycling straight
// through in order. Landing DEL as the current tool erases via the
// same paint-while-holding-Reset mechanic as any other tool, just
// removing a marker instead of placing one.
//
// The selected tool is a completely separate piece of state from
// patternEditCursorBar, untouched by paging between 16-bar windows -
// it stays selected however far you navigate; running out of room to
// paint further out is always the 500-marker cap, not the tool having
// reset.
//
// Exiting the paint editor (and persisting the markers) is the same
// Custom+Reset combo that opened OPERATION SETUP in the first place -
// see the "Operation Setup entry / Pattern Edit exit" combo in loop(), and
// it's also reachable from every screen inside Operation Setup itself
// (see the LEAVE chord in runOperationSetupMenu()), not just from TOP.
// Unlike the tool-cycle chord above, this one requires a full 1s hold
// (with a "LEAVE" sweep, see drawHoldSweep()) once the chord is
// detected, precisely so it can't be triggered by the tool-cycle chord
// itself or by holding Reset while tapping Custom/Divisor to paint -
// both of those are quick, unsustained interactions, while genuinely
// wanting to leave means holding still for a full second. Exiting from
// the paint editor lands back in OPERATION SETUP itself, not straight
// back to the live grid - from there, the normal Reset-held-at-TOP-
// level exit (or the same combo again) takes you the rest of the way
// out.
//
// See onCustomButton()/onDivisorButton()/onResetButton() for where
// these intercept the normal button roles, and render()'s normal-grid
// section for how the display itself changes while this is active.
bool     patternEditActive    = false;

// Custom+Reset chord hold-to-confirm (see the "Operation Setup entry /
// Pattern Edit exit" combo in loop()) - once the chord is detected,
// both buttons must stay down for a further 1s, with a "LEAVE"/"MENU"
// sweep shown via drawHoldSweep() in render(), before the actual
// enter/exit happens.
bool     leaveComboHoldActive   = false;
uint32_t leaveComboHoldStartMs  = 0;

// Same hold-to-confirm treatment for the Nudge mode combo (Custom+
// Divisor) - see loop()'s "Nudge mode" block. Separate state from the
// pair above since it's a different button combo/gesture, though it
// shares the same drawHoldSweep() visual.
bool     nudgeComboHoldActive  = false;
uint32_t nudgeComboHoldStartMs = 0;

// Set by runOperationSetupMenu() each iteration to tell renderOperationSetupMenu()
// whether to overlay the same hold-to-confirm sweep this frame -
// nullptr means none. Keeps every Reset-hold-to-leave/back moment in
// Operation Setup visually consistent with the combo above, without
// needing to thread the hold state through as a render() parameter.
const char* menuHoldSweepLabel   = nullptr;
uint32_t    menuHoldSweepHeldMs  = 0;
uint32_t patternEditCursorBar = 1; // 1-based absolute bar
uint8_t  patternEditTool      = 0; // 0..3 = SongSymbol directly, 4 = DEL - see paintPatternMarkerAtCursor()

// Fast-scroll state: holding Custom/Divisor for 1s starts paging by a
// full 16-bar window every PATTERN_EDIT_FAST_SCROLL_MS instead of
// moving one bar at a time - see onCustomHeldDuringPress()/
// onDivisorHeldDuringPress() (checked every loop() iteration while
// held) and the matching release handlers below, which skip their
// own single-step move once a hold has already paged things.
#define PATTERN_EDIT_FAST_SCROLL_MS 325 // 250ms slowed by 30%

// Every blink on the Pattern Mode editor screen (the tool indicator,
// the grid cursor, the cycle-end number) shares this one fixed
// period - deliberately not tied to BPM/the live tempo, just a flat,
// pleasant rate.
#define PATTERN_EDIT_BLINK_MS 300
// Step interval for the fast-scroll "running arrow" animation (see
// buildChaseFrame()) - deliberately faster than PATTERN_EDIT_BLINK_MS,
// since it's meant to read as motion, not a slow toggle.
#define PATTERN_EDIT_CHASE_MS 120
bool     patternEditDivisorFastScrolling = false;
bool     patternEditCustomFastScrolling  = false;
uint32_t patternEditDivisorLastPageMs    = 0;
uint32_t patternEditCustomLastPageMs     = 0;

// Paints (or erases) the marker at the cursor's current bar with
// whatever tool is selected. A symbol tool updates an existing marker
// there, or inserts a new one in sorted bar order if there's room
// (PATTERN_MARKER_COUNT total, same cap as everywhere else in Pattern
// Mode) - if it's full and there's no marker on this bar yet, this
// simply does nothing rather than bumping another marker out. The DEL
// tool removes whatever marker (if any) sits on this bar.
void paintPatternMarkerAtCursor() {
  if (patternEditTool == 4) { // DEL
    erasePatternMarkerAtCursor();
    return;
  }

  int16_t existing = -1;
  for (uint16_t i = 0; i < patternMarkerCount; i++) {
    if (patternMarkers[i].bar == patternEditCursorBar) { existing = i; break; }
  }

  uint8_t symbol = patternEditTool; // 0..3 map directly to SongSymbol now
  if (existing >= 0) {
    patternMarkers[existing].symbol = symbol;
    return;
  }
  if (patternMarkerCount >= PATTERN_MARKER_COUNT) return; // full - can't add a new one here

  uint16_t insertAt = patternMarkerCount;
  for (uint16_t i = 0; i < patternMarkerCount; i++) {
    if (patternMarkers[i].bar > patternEditCursorBar) { insertAt = i; break; }
  }
  for (uint16_t i = patternMarkerCount; i > insertAt; i--) patternMarkers[i] = patternMarkers[i - 1];
  patternMarkers[insertAt].bar    = patternEditCursorBar;
  patternMarkers[insertAt].symbol = symbol;
  patternMarkerCount++;
}

// Removes whatever marker (if any) sits on the cursor's current bar -
// called from paintPatternMarkerAtCursor() when DEL is the selected
// tool.
void erasePatternMarkerAtCursor() {
  for (uint16_t i = 0; i < patternMarkerCount; i++) {
    if (patternMarkers[i].bar == patternEditCursorBar) {
      for (uint16_t j = i; j + 1 < patternMarkerCount; j++) patternMarkers[j] = patternMarkers[j + 1];
      patternMarkerCount--;
      return;
    }
  }
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

// True if two press-start timestamps are within 400ms of each other -
// the shared "close enough to be one physical chord" test. Used by
// loop()'s Reset+Custom Operation Setup combo to tell a genuine fresh
// chord apart from Reset simply having already been held for a while
// when Custom happens to join in (a normal, frequent interaction in
// the Pattern Mode paint editor - see onCustomButton()/
// onDivisorButton()). onResetButton() and onCustomButton() below reuse
// the exact same test to recognize when their OWN release is part of
// an aborted chord attempt (the combo's 1s hold never completed) and
// swallow their normal short-press action instead of firing it -
// without this, letting go of either button early still registered as
// a plain Reset or Custom press (arming Reset 1, or worse, an instant
// SET 1.1/TIMESIG change from the Custom side).
bool isChordTiming(uint32_t pressStartMsA, uint32_t pressStartMsB) {
  int32_t diff = (int32_t)(pressStartMsA - pressStartMsB);
  if (diff < 0) diff = -diff;
  return diff <= 400;
}

void onDivisorButton(bool longPress) {
  if (patternEditActive) {
    if (suppressDivisorAction) { suppressDivisorAction = false; return; } // consumed by the Custom+Divisor tool-cycle chord, see loop()
    if (btnCustom.isPressed) return; // interlocked against Custom - only one of the two navigates at a time here (see the tool-cycle chord above for the one deliberate exception)
    if (patternEditDivisorFastScrolling) {
      patternEditDivisorFastScrolling = false; // already paged (and painted/erased) via the hold-repeat, see onDivisorHeldDuringPress()
      return;
    }
    if (patternEditCursorBar < 9999) patternEditCursorBar++;
    if (btnReset.isPressed) paintPatternMarkerAtCursor(); // holding Reset while moving paints/erases with the current tool
    return;
  }
  if (suppressDivisorAction) { suppressDivisorAction = false; return; } // was only a wake-up press
  if (currentMode != MODE_NORMAL) return; // no accidental change in nudge mode/analyzer (nudge runs via its own logic in loop())
  divisorIndex = (gridMode == GRIDMODE_SCROLLING)
    ? nextEnabledDivisorInRange(divisorIndex, 2, SCROLLING_MAX_WINDOW_BARS)
    : nextEnabledDivisor(divisorIndex);
  saveQuickState(); // save the latest state immediately, kept after restart
}

// Custom button — freely assignable via Switches > Custom (default
// role: SET 1.1). Dispatches to whichever function is currently
// assigned; see triggerSet11() below for the SET 1.1 role.
void onCustomButton(bool longPress) {
  // Custom's release is checked against Reset's CURRENT state and
  // press-start, not against btnCustom's own (which has already
  // flipped to "released" by the time this callback runs) - see
  // isChordTiming().
  if (btnReset.stableState == LOW && isChordTiming(btnCustom.pressStartMs, btnReset.pressStartMs)) return;
  if (patternEditActive) {
    if (suppressCustomAction) { suppressCustomAction = false; return; } // consumed by the Custom+Divisor tool-cycle chord, see loop()
    if (btnDivisor.isPressed) return; // interlocked against Divisor/Grid - only one of the two navigates at a time here (see the tool-cycle chord above for the one deliberate exception)
    if (patternEditCustomFastScrolling) {
      patternEditCustomFastScrolling = false; // already paged (and painted/erased) via the hold-repeat, see onCustomHeldDuringPress()
      return;
    }
    if (patternEditCursorBar > 1) patternEditCursorBar--;
    if (btnReset.isPressed) paintPatternMarkerAtCursor(); // holding Reset while moving paints/erases with the current tool
    return;
  }
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
  if (patternEditActive) return; // Custom navigates the paint cursor here, never the analyzer
  if (btnReset.isPressed) return; // Reset also down - this is the Operation Setup combo (see loop()), not a solo Custom hold
  if (btnDivisor.isPressed) return; // Divisor also down - this is the Nudge combo (see loop()), not a solo Custom hold
  if (suppressCustomAction) return; // wake-up press does not trigger a long-press effect
  if (currentMode == MODE_NUDGE) return; // no analyzer toggle in nudge mode
  currentMode = (currentMode == MODE_NORMAL) ? MODE_ANALYZER : MODE_NORMAL;
}

// Fast-scroll while held, checked every loop() iteration (unlike
// onCustomButtonLongHeldDuringPress()/onResetMediumHeldDuringPress()
// above, which are one-shot triggers gated behind their own
// "already handled" flags) - after 1s held, pages a full 16-bar
// window every PATTERN_EDIT_FAST_SCROLL_MS instead of moving one bar
// at a time. See
// onCustomButton()/onDivisorButton() for where the matching release
// skips its own single-step once this has already moved things.
void onCustomHeldDuringPress() {
  if (!btnCustom.isPressed) { patternEditCustomFastScrolling = false; return; } // the real bug: without this, a stale pressStartMs from any earlier press made heldMs look huge forever
  if (!patternEditActive) return;
  if (btnDivisor.isPressed) return; // interlocked against Divisor/Grid, see onCustomButton()
  uint32_t heldMs = millis() - btnCustom.pressStartMs;
  if (heldMs < 1000) return;
  if (!patternEditCustomFastScrolling) {
    patternEditCustomFastScrolling = true;
    patternEditCustomLastPageMs    = millis();
  }
  if (millis() - patternEditCustomLastPageMs >= PATTERN_EDIT_FAST_SCROLL_MS) {
    patternEditCustomLastPageMs = millis();
    patternEditCursorBar = (patternEditCursorBar > 16) ? (patternEditCursorBar - 16) : 1;
    if (btnReset.isPressed) paintPatternMarkerAtCursor(); // holding Reset while moving paints/erases with the current tool
  }
}

void onDivisorHeldDuringPress() {
  if (!btnDivisor.isPressed) { patternEditDivisorFastScrolling = false; return; } // same fix as onCustomHeldDuringPress() - see there
  if (!patternEditActive) return;
  if (btnCustom.isPressed) return; // interlocked against Custom, see onDivisorButton()
  uint32_t heldMs = millis() - btnDivisor.pressStartMs;
  if (heldMs < 1000) return;
  if (!patternEditDivisorFastScrolling) {
    patternEditDivisorFastScrolling = true;
    patternEditDivisorLastPageMs    = millis();
  }
  if (millis() - patternEditDivisorLastPageMs >= PATTERN_EDIT_FAST_SCROLL_MS) {
    patternEditDivisorLastPageMs = millis();
    uint32_t next = patternEditCursorBar + 16;
    patternEditCursorBar = (next > 9999) ? 9999 : next;
    if (btnReset.isPressed) paintPatternMarkerAtCursor(); // holding Reset while moving paints/erases with the current tool
  }
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

// ---------------------------------------------------------------------------
// MIDI MONITOR: last MIDI_MON_SLOTS non-realtime messages (Note/CC/Program
// Change/Pitch Bend), shown on the Monitor screen sorted by channel at
// render time - see renderAnalyzer() further down. Pure FIFO ring buffer,
// no per-channel dedup: two messages from the same channel can both be
// visible at once, oldest one evicted first as new ones arrive. Declared
// up here (rather than next to pushMidiMonEvent()/handleMon*() below)
// because onResetButton() below already needs to clear it, and plain
// global variables must be declared before their point of use in the
// file, unlike functions which Arduino auto-prototypes.
// ---------------------------------------------------------------------------
enum MidiMonType { MIDIMON_NOTE_ON, MIDIMON_NOTE_OFF, MIDIMON_CC, MIDIMON_PC, MIDIMON_PB };
#define MIDI_MON_SLOTS 6
struct MidiMonEvent {
  uint8_t type;    // one of MidiMonType above
  uint8_t channel; // 1-16
  uint8_t data1;   // note number / CC number / program number - unused for PB
  int16_t data2;   // velocity / CC value - signed bend amount for PB, unused for PC
};
volatile MidiMonEvent midiMonRing[MIDI_MON_SLOTS];
volatile uint8_t midiMonHead  = 0; // next slot to overwrite
volatile uint8_t midiMonCount = 0; // valid entries so far, caps at MIDI_MON_SLOTS

void onResetButton(bool longPress) {
  // Reset's release is checked against Custom's CURRENT state and
  // press-start, not against btnReset's own (which has already
  // flipped to "released" by the time this callback runs) - see
  // isChordTiming().
  if (btnCustom.stableState == LOW && isChordTiming(btnCustom.pressStartMs, btnReset.pressStartMs)) return;
  if (patternEditActive) {
    // Tool selection is now the Custom+Divisor chord (see loop()) -
    // Reset just uses whatever's currently selected, on the bar the
    // cursor is on right now.
    paintPatternMarkerAtCursor();
    return;
  }
  if (currentMode == MODE_ANALYZER) {
    // MIDI Monitor screen: Reset clears the Note/CC event log instead
    // of performing any transport-reset function here (see also
    // onResetMediumHeldDuringPress(), gated the same way).
    if (suppressResetAction) { suppressResetAction = false; return; } // was only a wake-up press
    midiMonCount = 0;
    midiMonHead  = 0;
    return;
  }

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
  if (patternEditActive) return; // Reset rotates/paints tools here instead, see onResetButton() - never Reset 2
  if (currentMode == MODE_ANALYZER) return; // Reset clears the log on release instead, see onResetButton()
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

// MIDI RUN/STOP + CLOCK trace history: sampled once per incoming MIDI
// clock TICK (not wall-clock time) - see handleClock() below - so the
// sample rate automatically scales with tempo and the trace always
// spans exactly MIDI_TRACE_BEATS beats, at any BPM. Both traces share
// one index/full flag since they're always sampled together on the
// same tick, keeping them on one identical timebase. This is the
// MIDI-only equivalent of the CV RUN/STOP trace on the Eurorack variant
// (this board has no CV inputs).
#define MIDI_TRACE_BEATS 8
#define MIDI_TRACE_TICKS_PER_BEAT 24 // MIDI clock is fixed at 24 PPQN
#define MIDI_RUNSTOP_HISTORY_SIZE (MIDI_TRACE_BEATS * MIDI_TRACE_TICKS_PER_BEAT) // 192 ticks = 8 beats
volatile bool     midiRunStopHistory[MIDI_RUNSTOP_HISTORY_SIZE];
volatile uint8_t  midiRunStopHistIndex = 0;
volatile bool     midiRunStopHistFull  = false;

// MIDI CLOCK beat-pulse history: true at the one tick per beat where a
// full beat (24 ticks) just completed - see the BPM block in
// handleClock(), which already detects this same event. Deliberately
// independent of isRunning/Start-Stop, unlike midiRunStopHistory above -
// this reflects the raw incoming clock signal itself, so it keeps
// pulsing even while stopped as long as clock bytes arrive.
volatile bool midiClockPulseHistory[MIDI_RUNSTOP_HISTORY_SIZE];

// True at the same tick as midiClockPulseHistory above, but only when
// that beat is also beat 1 of the bar (the "downbeat" - takes the
// current time signature into account via beatsPerBar()). Only
// meaningful while running, same as currentBeat/currentBar themselves.
// Lets renderAnalyzer() draw the downbeat as a thicker mark than a
// regular beat in the MIDI CLOCK trace.
volatile bool midiClockDownbeatHistory[MIDI_RUNSTOP_HISTORY_SIZE];

// Set on every beat (see isBeatTick in handleClock()) - independent of
// Start/Stop, same as midiClockPulseHistory. Consumed and cleared by
// renderAnalyzer() to flash the "MIDI CLK" label's background for
// exactly one frame per beat.
volatile bool midiClockBeatPending = false;

// Set by handleStart(): forces the very next sampled tick to be marked
// as a downbeat. Bar 1's downbeat has no preceding "wrap into a new
// bar" for the normal isDownbeatTick check below to detect, since
// there's no previous bar to wrap from right after Start.
volatile bool midiClockForceDownbeat = false;

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

  // --- RUN/STOP + CLOCK trace: one sample per incoming tick, not per
  // wall-clock interval - see MIDI_RUNSTOP_HISTORY_SIZE above. Runs
  // before the isRunning early-return below on purpose, same as the BPM
  // block above, so the CLOCK trace keeps showing incoming ticks even
  // while stopped; RUN/STOP simply records isRunning as false for those.
  bool isBeatTick = (bpmTickCounter == 0); // just wrapped a full beat this call
  // Downbeat = this beat-completing tick will wrap currentBeat back to 0
  // (i.e. currentBeat is currently sitting on the LAST beat of the bar,
  // takes the active time signature into account via beatsPerBar()).
  // Only meaningful while running - currentBeat is frozen otherwise.
  bool isDownbeatTick = (isBeatTick && isRunning && (currentBeat == (uint16_t)(beatsPerBar() - 1))) || midiClockForceDownbeat;
  midiClockForceDownbeat = false; // consumed - only the tick right after Start gets forced
  midiRunStopHistory[midiRunStopHistIndex]      = isRunning;
  midiClockPulseHistory[midiRunStopHistIndex]   = isBeatTick;
  midiClockDownbeatHistory[midiRunStopHistIndex] = isDownbeatTick;
  if (isBeatTick) midiClockBeatPending = true; // flashes the MIDI CLK label once per beat, see renderAnalyzer()
  midiRunStopHistIndex++;
  if (midiRunStopHistIndex >= MIDI_RUNSTOP_HISTORY_SIZE) {
    midiRunStopHistIndex = 0;
    midiRunStopHistFull = true;
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

  // --- End Bar: manually set via Operation Setup, 0 = OFF. Checked
  // right after currentBar is finalized for this tick, same spot the
  // Reset 1/2 quantized reset below hooks in - (currentBar + 1) is the
  // 1-based bar just reached, so this fires the instant playback would
  // move past the configured end bar. Applies to both grid modes
  // (CYCLING and SCROLLING) equally - it's independent of how the grid
  // happens to be drawn.
  uint32_t effectiveEndBar = resolveEndBar();
  if (effectiveEndBar > 0 && (currentBar + 1) > effectiveEndBar) {
    switch (endBarAction) {
      case ENDBAR_LOOP:
        currentBar  = 0;
        totalTicks  = 0; // so F (divisor bar) starts over, same as a quantized Reset
        break;
      case ENDBAR_STOP:
        isRunning = false;
        break;
      case ENDBAR_CONTINUE:
      default:
        break; // no action - end bar is informational only in this mode
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
  midiClockForceDownbeat = true; // bar 1's downbeat has no preceding wrap to detect - see handleClock()
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

void pushMidiMonEvent(uint8_t type, uint8_t channel, uint8_t data1, int16_t data2) {
  midiMonRing[midiMonHead].type    = type;
  midiMonRing[midiMonHead].channel = channel;
  midiMonRing[midiMonHead].data1   = data1;
  midiMonRing[midiMonHead].data2   = data2;
  midiMonHead = (midiMonHead + 1) % MIDI_MON_SLOTS;
  if (midiMonCount < MIDI_MON_SLOTS) midiMonCount++;
}

// MIDI MONITOR callbacks. Each of these pops as many entries off the RX
// timestamp queue as the message actually occupies on the wire, mirroring
// what handleStart()/handleStop()/handleContinue() above already do for
// their own (always 1-byte) real-time messages - see the MIDI RX
// TIMESTAMP QUEUE comment further up. Skipping this would leave
// unconsumed entries piling up in the queue, later popped by handleClock()
// instead of their own Clock byte and quietly corrupting exactly the
// timestamps the 1.2.1 fix exists to protect.
// Caveat: MIDI running status (a repeated status byte legally omitted on
// the wire for consecutive same-type messages) can make the true byte
// count 1 less than assumed here - a rare, low-impact edge case, worth a
// closer look only if clock timing issues ever resurface specifically
// while notes/CCs are flowing at the same time.
void handleMonNoteOn(byte channel, byte note, byte velocity) {
  popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); // 3-byte message
  if (velocity == 0) {
    // Wire-level convention: NoteOn with velocity 0 IS a NoteOff (saves a
    // status byte under running status) - shown as NoteOff, not "NoteOn v0".
    pushMidiMonEvent(MIDIMON_NOTE_OFF, channel, note, 0);
  } else {
    pushMidiMonEvent(MIDIMON_NOTE_ON, channel, note, velocity);
  }
}

void handleMonNoteOff(byte channel, byte note, byte velocity) {
  popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); // 3-byte message
  pushMidiMonEvent(MIDIMON_NOTE_OFF, channel, note, 0);
}

void handleMonCC(byte channel, byte number, byte value) {
  popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); // 3-byte message
  pushMidiMonEvent(MIDIMON_CC, channel, number, value);
}

void handleMonPC(byte channel, byte number) {
  popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); // 2-byte message
  pushMidiMonEvent(MIDIMON_PC, channel, number, 0);
}

void handleMonPitchBend(byte channel, int bend) {
  popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); popMidiRxTimestampOr(0); // 3-byte message
  pushMidiMonEvent(MIDIMON_PB, channel, 0, (int16_t)bend);
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

// Shared "hold to confirm" sweep - same Eurorack-style XOR-inverting
// progress bar already used for RESET 1->2 in the header, generalized
// with a label/duration/y-position so every "hold to leave/back"
// moment in the firmware (the Custom+Reset combo, and Reset-held
// inside Operation Setup) gives the same visual feedback. y is the
// text baseline; the bar and background sit centered on it.
//
// Nothing is drawn for the first GRACE_MS of the hold - a brief,
// incidental hold (or one that resolves before the actual action
// fires) shouldn't flash this on screen; progress is scaled to the
// remaining window after that, same idea as the existing RESET 1->2
// sweep's own confirm delay. A solid background - deliberately bigger
// than the tight text+bar footprint - is cleared first, so the label
// reads cleanly instead of blending into whatever the screen was
// already showing underneath.
// Draws text that alternates between plain and inverted (a filled box
// behind it in the opposite color) depending on blinkOn - used for
// header/footer labels that need to blink attention to themselves
// (SET MARKERS, tool indicators, etc.) without a full re-layout.
void drawBlinkableText(int x, int y, const char* text, bool blinkOn) {
  if (blinkOn) {
    int w = u8g2.getStrWidth(text);
    u8g2.setDrawColor(1);
    u8g2.drawBox(x - 1, y - 7, w + 2, 9);
    u8g2.setDrawColor(0);
    u8g2.drawStr(x, y, text);
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawStr(x, y, text);
  }
}

// Same idea as drawBlinkableText() above, but for one of the 4 marker
// symbols (see drawSongSymbol()) instead of a text string. The symbol
// is drawn 1px right of the highlight box's own inset area -
// drawSongSymbol()'s narrow-cell 1px-left correction (meant for the
// live grids' alternating 15/16px column widths, see there) doesn't
// apply to this fixed 7x7 icon, so it needs undoing here to land
// centered on the 9x9 background.
void drawBlinkableSymbol(uint8_t symbol, int x, int y, bool blinkOn) {
  if (blinkOn) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(x - 1, y - 1, 9, 9);
    u8g2.setDrawColor(0);
    drawSongSymbol(symbol, x + 1, y, 7, 7, SYMFILL_OUTLINE);
    u8g2.setDrawColor(1);
  } else {
    drawSongSymbol(symbol, x + 1, y, 7, 7, SYMFILL_OUTLINE);
  }
}

void drawHoldSweep(const char* label, uint32_t heldMs, uint32_t totalMs, int y) {
  const uint32_t GRACE_MS = 350;
  if (heldMs < GRACE_MS) return;
  float progress = (float)(heldMs - GRACE_MS) / (float)(totalMs - GRACE_MS);
  if (progress > 1.0f) progress = 1.0f;

  int w = u8g2.getStrWidth(label);
  int barW = w + 10; // a bit wider than the text
  int barX = (128 - barW) / 2;

  const int bgPadX = 8;
  int bgW = barW + bgPadX;
  int bgX = (128 - bgW) / 2;
  u8g2.setDrawColor(0);
  u8g2.drawBox(bgX, y - 9, bgW, 12);

  u8g2.setDrawColor(1);
  u8g2.drawStr((128 - w) / 2, y, label);

  int fillW = (int)(barW * progress);
  u8g2.setDrawColor(2); // XOR: inverts the text wherever the bar passes over it
  if (fillW > 0) {
    u8g2.drawBox(barX, y - 8, fillW, 9);
  }
  u8g2.setDrawColor(1);
}

// Same "draw label, XOR-fill it as the hold progresses" idea as
// drawHoldSweep() above, but anchored at an explicit x instead of
// always centered on the full 128px width - used by the Pattern Mode
// editor's directional arrows (see render()), which sit at the
// screen's own left/right edges. Unlike drawHoldSweep(), the first
// GRACE_MS shows a brief inverted flash rather than nothing at all -
// immediate confirmation that the press registered, even for a tap
// far too short to matter for fast-scroll - before settling into the
// normal "draw label, grow the XOR fill" sweep.
void drawDirectionalSweep(const char* label, uint32_t heldMs, uint32_t totalMs, int x, int y) {
  const uint32_t GRACE_MS = 350;
  int w = u8g2.getStrWidth(label);

  if (heldMs < GRACE_MS) {
    u8g2.drawBox(x - 1, y - 7, w + 2, 9);
    u8g2.setDrawColor(0);
    u8g2.drawStr(x, y, label);
    u8g2.setDrawColor(1);
    return;
  }

  u8g2.drawStr(x, y, label);

  float progress = (float)(heldMs - GRACE_MS) / (float)(totalMs - GRACE_MS);
  if (progress > 1.0f) progress = 1.0f;

  int fillW = (int)(w * progress);
  u8g2.setDrawColor(2); // XOR: inverts the label wherever the bar passes over it
  if (fillW > 0) {
    u8g2.drawBox(x, y - 7, fillW, 9);
  }
  u8g2.setDrawColor(1);
}

// Builds one frame of the fast-scroll "chase" animation for the
// Pattern Mode editor's directional arrows: a single arrow character
// stepping through 3 fixed slots, always running from the back toward
// the front - left-to-right for the ">>>" (Divisor/forward) arrow,
// right-to-left for "<<<" (Custom/back) - giving a sense of motion
// instead of a static tripled arrow. out must have room for 4 bytes
// (3 slots + terminator). u8g2_font_5x7_tr is fixed-width, so a space
// and an arrow character advance the same amount - the string's
// on-screen width (and thus its centering) stays identical across
// every frame.
void buildChaseFrame(char out[4], char arrowChar, bool pointsRight) {
  int step = (millis() / PATTERN_EDIT_CHASE_MS) % 3;
  int activeSlot = pointsRight ? step : (2 - step);
  for (int i = 0; i < 3; i++) out[i] = (i == activeSlot) ? arrowChar : ' ';
  out[3] = '\0';
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
// Marker symbol drawing (shared by Pattern Mode's grid, editor, and SCROLLING)
// ---------------------------------------------------------------------------
// Fixed on-screen sizes for the shape-based symbols (square, circle,
// triangle) - never stretched to fill whatever inset area a cell
// happens to have (that stretching was especially visible on square/
// circle, since cells aren't square themselves - fixed row height,
// variable column width - so a "circle" at x8 was actually a tall,
// narrow oval). LARGE is used in the roomier wide-column views
// (x1/x2/x4, and SCROLLING windows of the same size - inset width
// >= SONG_SYMBOL_ROOMY_MIN_W); SMALL in the narrow-column views
// (x8/x16, SCROLLING windows of the same size, and the Pattern Mode
// editor - inset width well under that). SMALL is sized to comfortably
// fit inside the narrowest of those (~11px inset); LARGE is one size
// up, still well within the wide views' inset (>=27px at x4).
#define SONG_SYMBOL_SIZE_SMALL 9
#define SONG_SYMBOL_SIZE_LARGE 12
#define SONG_SYMBOL_ROOMY_MIN_W 20

// Selects how a marker symbol is rendered:
//   SYMFILL_OUTLINE - plain outline, current draw color - the
//                     default "marker placed" look.
//   SYMFILL_SOLID   - a small solid mark of its own (X gets its own
//                     small background box, cut out for the glyph,
//                     since a font glyph has no "filled" variant of
//                     its own) - the steady "this marker's bar has
//                     already played this cycle" look (CYCLING's
//                     isFilled) and the Pattern Mode editor's
//                     cursor-blink look. Never itself blinks on the
//                     beat.
//   SYMFILL_CUTOUT  - the caller has already painted a *solid* box
//                     over this cell (the same fill a marker-less
//                     cell's own beat-pulse blink would draw) - this
//                     only punches the symbol's own shape out of
//                     that fill (drawn in color 0), rather than the
//                     symbol's own fill toggling. That way the pulse
//                     blink lives entirely in the cell's background,
//                     exactly like a marker-less cell, and the
//                     marker's shape itself never changes.

// Draws one of the 4 marker symbols into a grid cell. x,y,w,h is the
// cell's usual inset drawing area - only used here to find the cell's
// center and to tell a wide cell from a narrow one; the symbols
// themselves render at their own fixed size (see above for the
// shapes, and the font size switch below for X), not stretched to
// fill w/h. Narrow cells (x8/x16 and their SCROLLING/Pattern-Mode-
// editor equivalents) render everything 1px further left than a
// naive centered calculation would - their column widths round to an
// odd/even alternating sequence (124px / 8 isn't even), which
// otherwise reads as visibly off-center.
void drawSongSymbol(uint8_t symbol, int x, int y, int w, int h, SongSymbolFill fill) {
  bool roomy = (w >= SONG_SYMBOL_ROOMY_MIN_W);
  int cx = x + w / 2 - (roomy ? 0 : 1); // narrow cells: 1px left correction, see above
  int cy = y + h / 2;

  int targetSize = roomy ? SONG_SYMBOL_SIZE_LARGE : SONG_SYMBOL_SIZE_SMALL;
  int sw = (w < targetSize) ? w : targetSize;
  int sh = (h < targetSize) ? h : targetSize;
  int sx = cx - sw / 2;
  int sy = cy - sh / 2;

  // SOLID and CUTOUT both draw the shape's "filled" variant - only
  // the draw color differs (CUTOUT punches into an already-solid
  // background, so it draws in color 0 instead of the ambient color).
  bool drawFilledShape = (fill != SYMFILL_OUTLINE);
  if (fill == SYMFILL_CUTOUT) u8g2.setDrawColor(0);

  switch (symbol) {
    case SONG_SYM_SQUARE:
      if (drawFilledShape) u8g2.drawBox(sx, sy, sw, sh);
      else                 u8g2.drawFrame(sx, sy, sw, sh);
      break;
    case SONG_SYM_CIRCLE: {
      int r = ((sw < sh) ? sw : sh) / 2;
      if (drawFilledShape) u8g2.drawDisc(cx, cy, r);
      else                 u8g2.drawCircle(cx, cy, r);
      break;
    }
    case SONG_SYM_TRIANGLE:
      if (drawFilledShape) {
        u8g2.drawTriangle(sx, sy + sh - 1, sx + sw - 1, sy + sh - 1, cx, sy);
      } else {
        u8g2.drawLine(sx, sy + sh - 1, sx + sw - 1, sy + sh - 1);
        u8g2.drawLine(sx, sy + sh - 1, cx, sy);
        u8g2.drawLine(sx + sw - 1, sy + sh - 1, cx, sy);
      }
      break;
    case SONG_SYM_X:
    default: {
      // A larger, more legible font in the roomier one-full-height-
      // row cells (x1/x2/x4/x8 - h >= 24), the smaller font in the
      // tighter two-half-height-row cells (x16, SCROLLING, the
      // Pattern Mode editor - h < 24), where the bigger font wouldn't
      // fit. Restored to u8g2_font_5x7_tr afterwards, since every
      // caller of drawSongSymbol() assumes that's still the active
      // font.
      const uint8_t* xFont = (h >= 24) ? u8g2_font_9x15_tf : u8g2_font_6x10_tf;
      u8g2.setFont(xFont);
      int strW = u8g2.getStrWidth("X");
      int strX = cx - strW / 2;
      if (fill == SYMFILL_SOLID) {
        // Own small solid background box (sized to the glyph plus a
        // small fixed padding, not the full cell) - always a
        // self-contained two-color operation regardless of the
        // ambient draw color, since a font glyph has no "filled"
        // variant of its own to switch to.
        int bw = strW + 4;
        int bh = (u8g2.getAscent() - u8g2.getDescent()) + 4;
        if (bw > w) bw = w; // defensive clamp, never actually hit with our cell sizes
        if (bh > h) bh = h;
        int bx = cx - bw / 2;
        int by = cy - bh / 2;
        int strY = by + (bh + u8g2.getAscent()) / 2;
        u8g2.drawBox(bx, by, bw, bh);
        u8g2.setDrawColor(0);
        u8g2.drawStr(strX, strY, "X");
        u8g2.setDrawColor(1);
      } else {
        // OUTLINE: plain glyph in the ambient color. CUTOUT: the same
        // glyph, just drawn in color 0 (set above) onto the caller's
        // already-painted solid background - no separate box needed
        // here since that background already covers the full cell.
        int strY = y + (h + u8g2.getAscent()) / 2;
        u8g2.drawStr(strX, strY, "X");
      }
      u8g2.setFont(u8g2_font_5x7_tr);
      break;
    }
  }

  if (fill == SYMFILL_CUTOUT) u8g2.setDrawColor(1);
}

// Powers the SCROLLING grid mode (see gridMode) - the current bar
// always sits blinking in the first cell (top-left), never advancing
// position on screen, and always shows *something* there (a filled
// pulse if there's no marker) so it's never ambiguous whether
// playback is moving. Bars run continuously right to left, every
// following cell (2nd, 3rd, ...) showing whichever patternMarkers[]
// entry (if any) sits that many bars ahead, so an upcoming marker
// visibly scrolls in from the right well before it actually arrives.
// An empty cell (no marker) shows the absolute bar number instead
// wherever that bar is a multiple of the window size - the last bar
// of that mini-cycle (windowSize 16 -> 16, 32, 64, ...; windowSize 4
// -> 4, 8, 12, ...) - so a stretch with no markers at all still gives
// a concrete sense of which bar a cell corresponds to.
//
// Cell frames, fill states, grouping separators and row/column
// divider lines are all identical to the plain divisor grid's own
// cells in render() (see there) - windowSize plays exactly the role
// div_ does there, right down to reusing settings.gridSeparatorsEnabled
// and the same rowsPerGroup/groupSepH/refFootprintH rules. The only
// intentional difference is content: there's no "already elapsed"
// concept here (every cell beyond the current one is still ahead), so
// the dithered elapsed-fill never applies, and a plain empty cell
// shows its bar number instead of nothing. Window size (how many bars
// ahead are visible at once) = divisor(), capped to
// SCROLLING_MAX_WINDOW_BARS (16) - so in practice only the "x16" and
// "no grouping" branches below are ever reached; the x32/x64 branches
// and the thinRows fallback are kept only for exact parity with
// render() (and in case that cap is ever raised).
void renderScrollingGrid(uint32_t barSnapshot, bool runningSnap, bool flashHideBar, bool beatPulseOnNormal) {
  if (flashHideBar) return;

  const int barX = 2, barW = 124;
  const int lowerY = 18;

  uint8_t windowSize = divisor();
  if (windowSize > SCROLLING_MAX_WINDOW_BARS) windowSize = SCROLLING_MAX_WINDOW_BARS;

  uint8_t segsPerRow = (windowSize < 8) ? windowSize : 8;
  uint8_t rows       = (windowSize + 7) / 8;

  uint8_t rowsPerGroup;
  int groupSepH;
  int refFootprintH;
  if (settings.gridSeparatorsEnabled) {
    if (windowSize == 32)      { rowsPerGroup = 2; groupSepH = 3; }
    else if (windowSize == 64) { rowsPerGroup = 4; groupSepH = 3; }
    else if (windowSize == 16) { rowsPerGroup = 1; groupSepH = 1; }
    else                       { rowsPerGroup = 4; groupSepH = 1; }
    refFootprintH = 35;
  } else {
    rowsPerGroup = rows;
    groupSepH = 0;
    refFootprintH = 32;
  }
  uint8_t numGroups = (rows + rowsPerGroup - 1) / rowsPerGroup;
  uint8_t numGroupSeps = (numGroups > 0) ? (numGroups - 1) : 0;

  int rowH = (refFootprintH - numGroupSeps * groupSepH) / rows;
  if (rowH < 1) rowH = 1; // safety net, never actually hit with our window sizes

  bool thinRows = rowH < 4; // never actually hit at windowSize <= 16, kept for parity with render()

  for (uint8_t r = 0; r < rows; r++) {
    uint8_t groupsBefore = r / rowsPerGroup;
    int y = lowerY + r * rowH + groupsBefore * groupSepH;
    for (uint8_t i = 0; i < segsPerRow; i++) {
      uint32_t k = (uint32_t)r * segsPerRow + i; // 0 = current bar (fixed, first cell), higher = further ahead
      if (k >= windowSize) continue; // only relevant if windowSize weren't a power of 2 - defensive
      int x0 = barX + (i * barW) / segsPerRow;
      int x1 = barX + ((i + 1) * barW) / segsPerRow;
      int segWpx = x1 - x0;

      uint32_t absoluteBar1Based = barSnapshot + k + 1;
      uint8_t symbol = 0;
      bool symbolIsFirstOfType = false;
      bool hasSymbol = findPatternMarkerSymbolAt(barSnapshot, (uint8_t)k, symbol, symbolIsFirstOfType);

      // The current bar (k=0) always gets a visible "now" cursor,
      // marker or not - otherwise, with no marker sitting exactly on
      // the current bar, there would be nothing at all on screen
      // confirming playback is even moving.
      bool isActivePulse = (k == 0 && runningSnap && beatPulseOnNormal);

      if (hasSymbol) {
        // Same treatment as the plain divisor grid's marker cells:
        // the cell's normal frame stays exactly as it always would.
        // On the beat pulse, the background fills solid exactly like
        // a marker-less cell's own pulse (see the non-marker branch
        // below) and the symbol is cut out of it instead of the
        // symbol's own fill toggling - the marker's shape never
        // blinks, only the cell's background does. The first marker
        // of each new symbol run (symbolIsFirstOfType) gets a steady
        // solid mark too, so a change of symbol is visible at a
        // glance without needing to read every marker individually.
        u8g2.drawFrame(x0, y, segWpx, rowH);
        // 124px / 8 isn't even, so columns alternate 15/16px wide
        // (even i -> 15, odd i -> 16) once segsPerRow reaches 8 (the
        // x8/x16-equivalent windows). The narrower, odd-width columns
        // render 1px further left than drawSongSymbol()'s own
        // centering accounts for - shift those specifically back
        // right. Narrower/rarer at x1/x2/x4, where segsPerRow < 8 and
        // every column is already an even width.
        bool oddWidthCol = (segsPerRow == 8) && ((i % 2) == 0);
        int symX = x0 + 2 + (oddWidthCol ? 1 : 0);
        if (isActivePulse) {
          u8g2.drawBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
          drawSongSymbol(symbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_CUTOUT);
        } else if (symbolIsFirstOfType) {
          drawSongSymbol(symbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_SOLID);
        } else {
          drawSongSymbol(symbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_OUTLINE);
        }
      } else if (thinRows) {
        if (isActivePulse) u8g2.drawBox(x0, y, segWpx, rowH);
      } else {
        u8g2.drawFrame(x0, y, segWpx, rowH);
        if (isActivePulse) {
          u8g2.drawBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
        } else if (absoluteBar1Based % windowSize == 0) {
          // No marker, not the current bar: a cell landing on a
          // multiple of the window size is the last bar of that
          // mini-cycle - shown as its own bar number, inset the same
          // way a marker symbol would be, rather than the plain
          // divisor grid's dithered "elapsed" fill (nothing has
          // elapsed yet here). Uses the same small 4x6 font as the
          // MIDI Monitor event log to fit inside the narrow cells,
          // restored to u8g2_font_5x7_tr afterwards like
          // drawSongSymbol() does.
          char numBuf[8];
          sprintf(numBuf, "%lu", (unsigned long)absoluteBar1Based);
          u8g2.setFont(u8g2_font_4x6_tf);
          int strW = u8g2.getStrWidth(numBuf);
          int strX = x0 + (segWpx - strW) / 2;
          int strY = y + (rowH + u8g2.getAscent()) / 2;
          u8g2.drawStr(strX, strY, numBuf);
          u8g2.setFont(u8g2_font_5x7_tr);
        }
      }
    }
  }

  // Redraw separator lines between segments inverted (color0), exact
  // same treatment as the plain divisor grid, so they stay visible as
  // a gap even over a filled/blinking cell or a bar-number label.
  u8g2.setDrawColor(0);
  for (uint8_t r = 0; r < rows; r++) {
    uint8_t groupsBefore = r / rowsPerGroup;
    int y = lowerY + r * rowH + groupsBefore * groupSepH;
    for (uint8_t i = 1; i < segsPerRow; i++) {
      int x = barX + (i * barW) / segsPerRow;
      u8g2.drawVLine(x, y, rowH);
    }
  }
  // Same as render(): skip rows that start a new group (they already
  // have a real gap above them instead of a shared border to erase).
  if (!thinRows) {
    for (uint8_t r = 1; r < rows; r++) {
      if (r % rowsPerGroup == 0) continue;
      uint8_t groupsBefore = r / rowsPerGroup;
      int y = lowerY + r * rowH + groupsBefore * groupSepH;
      u8g2.drawHLine(barX, y, barW);
    }
  }
  u8g2.setDrawColor(1);
}

// Draws the x16 window containing patternEditCursorBar (see the
// PATTERN MODE marker paint editor section above) - the same cell
// geometry as the normal grid's own x16 case (2 rows of 8, 17px tall),
// but window position is driven by the edit cursor rather than the
// live transport. Every cell's frame is always drawn, marker or not -
// painting adds a symbol inset inside it, it never replaces the frame
// itself. The cursor's own cell blinks: with a marker, the symbol
// alternates filled/outline; without one, a plain filled mark blinks
// inside the frame so the cursor stays visible either way. Crossing
// bar 16/1 of the current window automatically pages to the next/
// previous window, since windowStart is simply derived from the
// cursor position.
void renderPatternEditGrid() {
  const int barX = 2, barW = 124;
  const int lowerY = 18;
  const int rowH = 17; // matches the normal grid's own x16 case exactly
  const uint8_t segsPerRow = 8;
  const uint8_t rows = 2;

  uint32_t windowStart = ((patternEditCursorBar - 1) / 16) * 16; // 0-based
  bool blinkOn = (millis() / PATTERN_EDIT_BLINK_MS) % 2 == 0;

  for (uint8_t r = 0; r < rows; r++) {
    int y = lowerY + r * rowH;
    for (uint8_t i = 0; i < segsPerRow; i++) {
      uint32_t idx = (uint32_t)r * segsPerRow + i;
      uint32_t absBar = windowStart + idx + 1; // 1-based
      int x0 = barX + (i * barW) / segsPerRow;
      int x1 = barX + ((i + 1) * barW) / segsPerRow;
      int segWpx = x1 - x0;

      uint8_t symbol = 0;
      bool hasMarker = false;
      for (uint16_t m = 0; m < patternMarkerCount; m++) {
        if (patternMarkers[m].bar == absBar) { symbol = patternMarkers[m].symbol; hasMarker = true; break; }
      }

      // The cell's frame always stays - painting only ever adds
      // something inset inside it, never swaps it out.
      u8g2.drawFrame(x0, y, segWpx, rowH);

      // 124px / 8 isn't even, so columns alternate 15/16px wide (even
      // i -> 15, odd i -> 16) - the narrower, odd-width columns
      // render 1px further left than drawSongSymbol()'s own centering
      // accounts for, same fix as the live CYCLING/SCROLLING grids.
      bool oddWidthCol = ((i % 2) == 0);
      int symX = x0 + 2 + (oddWidthCol ? 1 : 0);

      bool isCursor = (absBar == patternEditCursorBar);
      if (hasMarker) {
        // On the cursor's own cell, the symbol blinks filled/outline;
        // everywhere else it's a plain outline.
        drawSongSymbol(symbol, symX, y + 2, segWpx - 4, rowH - 4, (isCursor && blinkOn) ? SYMFILL_SOLID : SYMFILL_OUTLINE);
      } else if (isCursor && blinkOn) {
        // No marker here yet, but this is where the cursor sits - a
        // blinking filled mark inside the frame keeps it visible.
        u8g2.drawBox(x0 + 2, y + 2, segWpx - 4, rowH - 4);
      }
    }
  }

  // Separator lines between segments, inverted, same treatment as the
  // other grids.
  u8g2.setDrawColor(0);
  for (uint8_t r = 0; r < rows; r++) {
    int y = lowerY + r * rowH;
    for (uint8_t i = 1; i < segsPerRow; i++) {
      int x = barX + (i * barW) / segsPerRow;
      u8g2.drawVLine(x, y, rowH);
    }
  }
  u8g2.drawHLine(barX, lowerY + rowH, barW);
  u8g2.setDrawColor(1);
}

// ---------------------------------------------------------------------------
// MIDI MONITOR DISPLAY (custom button 1s hold to toggle on/off)
// ---------------------------------------------------------------------------
void midiNoteName(uint8_t note, char* buf, size_t bufSize) {
  static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  int octave = (int)(note / 12) - 1;
  snprintf(buf, bufSize, "%s%d", NOTE_NAMES[note % 12], octave);
}

void renderAnalyzer() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_4x6_tf); // smaller than the rest of the UI, needed to fit the event log + run/stop trace

  u8g2.drawStr(1, 6, "MIDI MONITOR");

  noInterrupts();
  bool     runningSnap = isRunning;
  uint32_t lastTickMs  = lastClockTickMillis;
  uint8_t  rsIdxSnap   = midiRunStopHistIndex;
  bool     rsHistSnap[MIDI_RUNSTOP_HISTORY_SIZE];
  for (uint8_t i = 0; i < MIDI_RUNSTOP_HISTORY_SIZE; i++) rsHistSnap[i] = midiRunStopHistory[i];
  bool     clkPulseSnap[MIDI_RUNSTOP_HISTORY_SIZE];
  for (uint8_t i = 0; i < MIDI_RUNSTOP_HISTORY_SIZE; i++) clkPulseSnap[i] = midiClockPulseHistory[i];
  bool     clkDownbeatSnap[MIDI_RUNSTOP_HISTORY_SIZE];
  for (uint8_t i = 0; i < MIDI_RUNSTOP_HISTORY_SIZE; i++) clkDownbeatSnap[i] = midiClockDownbeatHistory[i];
  bool     beatFlash = midiClockBeatPending;
  midiClockBeatPending = false; // consumed - one flash per beat, not per render frame
  MidiMonEvent monSnap[MIDI_MON_SLOTS];
  uint8_t monCountSnap = midiMonCount;
  uint8_t monHeadSnap  = midiMonHead;
  for (uint8_t i = 0; i < monCountSnap; i++) {
    uint8_t idx = (monHeadSnap + MIDI_MON_SLOTS - monCountSnap + i) % MIDI_MON_SLOTS;
    monSnap[i].type    = midiMonRing[idx].type;
    monSnap[i].channel = midiMonRing[idx].channel;
    monSnap[i].data1   = midiMonRing[idx].data1;
    monSnap[i].data2   = midiMonRing[idx].data2;
  }
  interrupts();

  // Top right: current BPM, one decimal place
  char bpmBuf[14];
  dtostrf(bpmFiltered, 4, 1, bpmBuf);
  strncat(bpmBuf, " BPM", sizeof(bpmBuf) - strlen(bpmBuf) - 1);
  int bpmW = u8g2.getStrWidth(bpmBuf);
  u8g2.drawStr(126 - bpmW, 6, bpmBuf);

  // =========================================================================
  // MIDI EVENT LOG: last MIDI_MON_SLOTS (6) Note/CC/Program Change/Pitch
  // Bend messages, newest on top - older ones get pushed down a row as
  // new ones arrive, and fall off the bottom once all 6 rows are full
  // (see pushMidiMonEvent() above for the underlying ring buffer).
  // Empty slots (fewer than 6 messages seen so far) show a placeholder
  // so the layout never jumps around.
  // =========================================================================
  {
    char monLine[24];
    for (uint8_t row = 0; row < MIDI_MON_SLOTS; row++) {
      int y = 15 + row * 6;
      if (row < monCountSnap) {
        // monSnap is oldest-first (see the snapshot loop above) - walk
        // it back-to-front so row 0 shows the newest message.
        MidiMonEvent* e = &monSnap[monCountSnap - 1 - row];
        char noteBuf[5];
        switch (e->type) {
          case MIDIMON_NOTE_ON:
            midiNoteName(e->data1, noteBuf, sizeof(noteBuf));
            snprintf(monLine, sizeof(monLine), "CH%02d NoteOn %-3s v%d", e->channel, noteBuf, e->data2);
            break;
          case MIDIMON_NOTE_OFF:
            midiNoteName(e->data1, noteBuf, sizeof(noteBuf));
            snprintf(monLine, sizeof(monLine), "CH%02d NoteOff %-3s", e->channel, noteBuf);
            break;
          case MIDIMON_CC:
            snprintf(monLine, sizeof(monLine), "CH%02d CC%-3d =%d", e->channel, e->data1, e->data2);
            break;
          case MIDIMON_PC:
            snprintf(monLine, sizeof(monLine), "CH%02d PC =%d", e->channel, e->data1);
            break;
          case MIDIMON_PB:
            snprintf(monLine, sizeof(monLine), "CH%02d PB %+d", e->channel, e->data2);
            break;
        }
      } else {
        snprintf(monLine, sizeof(monLine), "CH-- ----");
      }
      u8g2.drawStr(1, y, monLine);
    }
  }

  // =========================================================================
  // MIDI CLOCK: one spike per beat detected in the raw incoming clock
  // (independent of Start/Stop - see the beat-boundary detection in
  // handleClock()), on the exact same tick-based timebase/sample grid as the
  // RUN/STOP trace right below, so the two rows line up column-for-column.
  // Newest sample on the left, scrolling right as it ages - matches the
  // event log's newest-on-top direction above.
  // =========================================================================
  {
    const int clkX0 = 46, clkX1 = 122, clkHighY = 49, clkLowY = 54;
    const int clkW = clkX1 - clkX0;
    bool noClockShowing = (millis() - lastTickMs) > 5000; // same 5s threshold as the main screen
    if (noClockShowing) {
      // No incoming MIDI clock for 5s - blink "NO CLOCK" in place of the
      // label instead of the beat flash, and skip the trace below since
      // there's nothing live to show.
      bool blinkOn = (millis() / 500) % 2 == 0;
      if (blinkOn) u8g2.drawStr(1, clkLowY, "NO CLOCK");
    } else {
      const char* clkLabel = "MIDI CLK";
      if (beatFlash) {
        // Beat flash: brief inverted background behind the label, one
        // render frame long (the pending flag is consumed above), then
        // back to normal - fires on every beat, not just the downbeat.
        int lw = u8g2.getStrWidth(clkLabel);
        u8g2.drawBox(0, clkLowY - 6, lw + 2, 7);
        u8g2.setDrawColor(0);
        u8g2.drawStr(1, clkLowY, clkLabel);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(1, clkLowY, clkLabel);
      }
      for (int x = 0; x < MIDI_RUNSTOP_HISTORY_SIZE; x++) {
        uint8_t idx = (rsIdxSnap + x) % MIDI_RUNSTOP_HISTORY_SIZE; // oldest sample left, newest right
        if (clkDownbeatSnap[idx]) {
          // Downbeat (1.1, time-signature aware - see isDownbeatTick in
          // handleClock()): drawn twice as wide as a regular beat mark.
          int curX = clkX0 + (x * clkW) / (MIDI_RUNSTOP_HISTORY_SIZE - 1);
          u8g2.drawBox(curX, clkHighY, 2, clkLowY - clkHighY + 1);
        } else if (clkPulseSnap[idx]) {
          int curX = clkX0 + (x * clkW) / (MIDI_RUNSTOP_HISTORY_SIZE - 1);
          u8g2.drawVLine(curX, clkHighY, clkLowY - clkHighY + 1);
        }
      }
    }
  }

  // =========================================================================
  // MIDI RUN/STOP: level trace over the last MIDI_TRACE_BEATS beats, extracted from the
  // MIDI Start/Stop/Continue messages (see handleStart()/handleStop()/
  // handleContinue()) rather than a CV gate input - this board has no CV
  // inputs, so this replaces the CV I/O status area from the Eurorack
  // variant's analyzer with the MIDI-equivalent information. Current
  // state as a single "RUN = HIGH"/"RUN = LOW" label to the left of
  // the trace, instead of a separate caption line above it. Oldest
  // sample on the left, same direction as the MIDI CLOCK trace above.
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

  if (btnCustom.isPressed && !btnDivisor.isPressed && !btnReset.isPressed && !customLongAlreadyHandled) {
    // Solo Custom hold heading back out via
    // onCustomButtonLongHeldDuringPress()'s Analyzer toggle - same
    // sweep as everywhere else, just needs the bigger font switched in
    // first since this whole screen normally uses the smaller one.
    // customLongAlreadyHandled guards against the SAME continuous
    // press that just toggled us into this screen immediately
    // rendering as a "complete" LEAVE sweep too (millis()-pressStartMs
    // is already >=LONGPRESS_MS at that instant) - the button must be
    // released and pressed again fresh before this sweep reappears.
    uint32_t heldMs = millis() - btnCustom.pressStartMs;
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHoldSweep("LEAVE", heldMs, LONGPRESS_MS, 35);
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

  // Absolute (0-based) bar the current div_-bar window begins at - used
  // by the Pattern Mode marker lookup below to match a marker's actual
  // bar number rather than just its position within the repeating
  // cycle (see findPatternMarkerSymbolAt()).
  uint32_t cycleWindowStartBar = barSnapshot - barsIntoCycle;

  // ---------------- Header row: time (A) | status/reset (G) | BPM (D) ----------------
  // Reset+Custom held together is the Operation Setup entry chord (see
  // loop()'s leaveComboHoldActive), not a Reset gesture - so as soon as
  // Custom joins in, none of the plain Reset 1/2 header feedback below
  // should appear (it would otherwise show alongside/underneath the
  // "OPERATION SETUP" sweep once the chord is recognized).
  bool resetComboWithCustomHeld = btnReset.isPressed && btnCustom.isPressed;
  bool nothingToResetShowing = btnReset.isPressed && !runningSnap && !hasSomethingToReset() && !resetComboWithCustomHeld;
  // No MIDI clock received for 5s while we're in STOP ->
  // "NO MIDI-CLOCK" instead of "STOP" is shown, tempo (D) is hidden. Only
  // relevant at the lowest priority level (no reset feedback/nudge active).
  bool noMidiClockShowing = !runningSnap && (millis() - lastClockTickMillis > 5000);

  if (patternEditActive) {
    // A+G are replaced entirely by a single static label while the
    // marker paint editor is active - see patternEditActive.
    const char* msg = "SET MARKERS";
    int w = u8g2.getStrWidth(msg);
    u8g2.drawStr((128 - w) / 2, 7, msg);
  } else {
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
  } else if (btnReset.isPressed && !resetComboWithCustomHeld) {
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
  } // end of the pre-existing A+G content (else branch of "if (patternEditActive)" above)

  char bpmBuf[10];
  dtostrf(bpmFiltered, 4, 1, bpmBuf);
  if (!nothingToResetShowing && !noMidiClockShowing && !patternEditActive) {
    u8g2.drawStr(100, 7, bpmBuf);
  }

  // ---------------- Top bar: beat progress (5px) / SET MARKERS nav arrows ----------------
  const int barX = 2, barW = 124;
  const int topY = 9, topH = 5;

  if (!patternEditActive) {
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
  } else {
    // Beat progress doesn't mean anything while editing markers -
    // Custom/Divisor navigate the paint cursor here instead of
    // moving through a beat (see onCustomButton()/onDivisorButton()).
    // A pair of directional arrows takes the bar's place: "<"
    // (Custom, back) on the left, ">" (Divisor/Grid, forward) on the
    // right - static by default, since they're not signaling anything
    // to notice; the only visual change is a press itself, via
    // drawDirectionalSweep()'s own brief flash-then-sweep. Once
    // fast-scrolling actually kicks in (see onCustomHeldDuringPress()/
    // onDivisorHeldDuringPress()), a single arrow chases across the
    // tripled "<<<"/">>>" instead (see buildChaseFrame()) - the
    // sweep's job (signaling "about to speed up") is done by then,
    // this is "already moving". Either arrow is left out entirely
    // once that direction is actually a dead end (cursor already at
    // bar 1 or 9999, see onCustomButton()/onDivisorButton()) - an
    // arrow implies you can press it, so it shouldn't be there at all
    // once that's no longer true.
    const int navY = 15;

    if (patternEditCursorBar > 1) {
      const char* leftLabel = patternEditCustomFastScrolling ? "<<<" : "<";
      if (patternEditCustomFastScrolling) {
        char buf[4];
        buildChaseFrame(buf, '<', false);
        u8g2.drawStr(barX, navY, buf);
      } else if (btnCustom.isPressed) {
        drawDirectionalSweep(leftLabel, millis() - btnCustom.pressStartMs, 1000, barX, navY);
      } else {
        u8g2.drawStr(barX, navY, leftLabel);
      }
    }

    if (patternEditCursorBar < 9999) {
      const char* rightLabel = patternEditDivisorFastScrolling ? ">>>" : ">";
      int rightW = u8g2.getStrWidth(rightLabel);
      int rightX = barX + barW - rightW;
      if (patternEditDivisorFastScrolling) {
        char buf[4];
        buildChaseFrame(buf, '>', true);
        u8g2.drawStr(rightX, navY, buf);
      } else if (btnDivisor.isPressed) {
        drawDirectionalSweep(rightLabel, millis() - btnDivisor.pressStartMs, 1000, rightX, navY);
      } else {
        u8g2.drawStr(rightX, navY, rightLabel);
      }
    }

    // Short reminder of what Reset actually does here, centered
    // between the two arrows - phrased to match whatever the current
    // tool would do (paint the selected symbol, or erase with DEL).
    const char* hint = (patternEditTool == 4) ? "RESET = DELETE" : "RESET = PLACE";
    int hintW = u8g2.getStrWidth(hint);
    u8g2.drawStr((128 - hintW) / 2, navY, hint);
  }

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

  // ---------------- Bottom bar: divisor cycle / timeline / pattern paint editor ----------------
  const int lowerY = 18;

  if (patternEditActive) {
    renderPatternEditGrid();
  } else if (gridMode == GRIDMODE_SCROLLING) {
    renderScrollingGrid(barSnapshot, runningSnap, flashHideBar, beatPulseOnNormal);
  } else {
  uint8_t segsPerRow = (div_ < 8) ? div_ : 8;
  uint8_t rows = (div_ + 7) / 8; // rounded up, gives 1/2/4/8/16 with our values

  // Every 4 rows (= 32 bars at 8 cols/row) gets a genuine group
  // separator that consumes real space - unlike the free, overwritten
  // per-row separators below. x64 (8 rows) and x128 (16 rows) need this
  // from the general rule; x32 (exactly 4 rows -> 0 under the general
  // rule) additionally gets a smaller 2-row grouping instead, splitting
  // it into two 16-bar halves; x16 (2 rows) gets its own 1-row grouping,
  // splitting it into two 8-bar halves - none of these strictly need
  // grouping by bar-count alone, but each rowsPerGroup/separator pair
  // below was chosen so the leftover-pixel division against the target
  // footprint comes out even with zero truncation waste (see the
  // per-case comments), which incidentally also gives x32/x64 a
  // clearly visible blank middle row instead of a thin hairline.
  // This unavoidably makes some views taller than the old fixed 32px,
  // so that taller footprint (set by x128, the tallest/worst case) is
  // the shared target band every divisor view is drawn into - flush at
  // the top always (no vertical centering: with leftover amounts this
  // small, centering rounds inconsistently between cases and reads as
  // misalignment rather than an intentional smaller box).
  // Display > Grid Lines = NO reverts to the original: no grouping at
  // all, fixed 32px footprint, exactly the pre-existing behavior.
  uint8_t rowsPerGroup;
  int groupSepH;
  int refFootprintH;
  if (settings.gridSeparatorsEnabled) {
    if (div_ == 32)      { rowsPerGroup = 2; groupSepH = 3; } // 4 rows: (35-3)/4  = 8 exact
    else if (div_ == 64) { rowsPerGroup = 4; groupSepH = 3; } // 8 rows: (35-3)/8  = 4 exact
    else if (div_ == 16) { rowsPerGroup = 1; groupSepH = 1; } // 2 rows: (35-1)/2  = 17 exact
    else                 { rowsPerGroup = 4; groupSepH = 1; } // x128: 16 rows: (35-3)/16 = 2 exact; x1/2/4/8 (1 row): no grouping applies anyway
    refFootprintH = 35;
  } else {
    rowsPerGroup = rows; // rows never reaches a second group -> no separators
    groupSepH = 0;
    refFootprintH = 32; // original fixed height
  }
  uint8_t numGroups = (rows + rowsPerGroup - 1) / rowsPerGroup;
  uint8_t numGroupSeps = (numGroups > 0) ? (numGroups - 1) : 0;

  int rowH = (refFootprintH - numGroupSeps * groupSepH) / rows;
  if (rowH < 1) rowH = 1; // safety net, never actually hit with our divisor values

  bool thinRows = rowH < 4; // e.g. at x128 (16 rows of 2px) -> frame/inset would no longer be visible

  uint32_t activeRow = barsIntoCycle / segsPerRow; // row the active step is located in
  bool activeInLastRow = thinRows && (rows > 1) && (activeRow == (uint32_t)(rows - 1));

  if (!flashHideBar) {
    for (uint8_t r = 0; r < rows; r++) {
      uint8_t groupsBefore = r / rowsPerGroup;
      int y = lowerY + r * rowH + groupsBefore * groupSepH;
      for (uint8_t i = 0; i < segsPerRow; i++) {
        uint32_t globalIndex = (uint32_t)r * segsPerRow + i;
        int x0 = barX + (i * barW) / segsPerRow;       // even integer distribution
        int x1 = barX + ((i + 1) * barW) / segsPerRow;
        int segWpx = x1 - x0;

        bool isFilled = globalIndex < barsIntoCycle;
        bool beatPulseOn = activeInLastRow ? beatPulseOnFast : beatPulseOnNormal;
        bool isActivePulse = (globalIndex == barsIntoCycle && runningSnap && beatPulseOn);

        uint8_t markerSymbol;
        bool markerIsFirstOfType;
        bool markerEligibleDivisor = (div_ == 1 || div_ == 2 || div_ == 4 || div_ == 8 || div_ == 16);
        if (markerEligibleDivisor && findPatternMarkerSymbolAt(cycleWindowStartBar, (uint8_t)globalIndex, markerSymbol, markerIsFirstOfType)) {
          // Pattern Mode marker: the cell's normal frame stays exactly
          // as it always would. On the beat pulse, the background
          // fills solid exactly like a marker-less cell's own pulse
          // (see the non-marker branch below) and the symbol is cut
          // out of it instead of the symbol's own fill toggling - the
          // beat blink itself now lives entirely in the cell's
          // background, never in the marker's shape. Once played this
          // cycle (isFilled), the background gets the exact same
          // dithered "elapsed" fill a marker-less cell would, with the
          // marker's own steady solid mark on top of it. The first
          // marker of each new symbol run (markerIsFirstOfType) gets
          // that same steady solid mark too, even before it's been
          // played, so a change of symbol is visible at a glance
          // without needing to read every marker individually. Only
          // x1/x2/x4/x8/x16 have cells that map 1:1 onto one bar each
          // - x32/x64/x128 group multiple bars per cell, so a single
          // marker bar wouldn't have one clear cell to appear in
          // there.
          u8g2.drawFrame(x0, y, segWpx, rowH);
          // 124px / 8 isn't even, so columns alternate 15/16px wide
          // (even i -> 15, odd i -> 16) once segsPerRow reaches 8
          // (the x8/x16 divisors). The narrower, odd-width columns
          // render 1px further left than drawSongSymbol()'s own
          // centering accounts for - shift those specifically back
          // right. Narrower/rarer at x1/x2/x4, where segsPerRow < 8
          // and every column is already an even width.
          bool oddWidthCol = (segsPerRow == 8) && ((i % 2) == 0);
          int symX = x0 + 2 + (oddWidthCol ? 1 : 0);
          if (isActivePulse) {
            u8g2.drawBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
            drawSongSymbol(markerSymbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_CUTOUT);
          } else if (isFilled) {
            drawDitheredBox(x0 + 1, y + 1, segWpx - 2, rowH - 2);
            drawSongSymbol(markerSymbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_SOLID);
          } else if (markerIsFirstOfType) {
            drawSongSymbol(markerSymbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_SOLID);
          } else {
            drawSongSymbol(markerSymbol, symX, y + 2, segWpx - 4, rowH - 4, SYMFILL_OUTLINE);
          }
        } else if (thinRows) {
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
      uint8_t groupsBefore = r / rowsPerGroup;
      int y = lowerY + r * rowH + groupsBefore * groupSepH;
      for (uint8_t i = 1; i < segsPerRow; i++) {
        int x = barX + (i * barW) / segsPerRow;
        u8g2.drawVLine(x, y, rowH);
      }
    }
    // Only draw row separators when rows are tall enough, otherwise
    // the separator line would eat up most of the fill height at very
    // thin rows (e.g. x128). Skip rows that start a new 32-bar group:
    // those already have a real gap above them (drawn as nothing, just
    // spacing), so erasing a "shared border" there would instead punch
    // a hole in that row's own, otherwise complete top edge - it should
    // stay fully closed on top, exactly like row 0 already is.
    if (!thinRows) {
      for (uint8_t r = 1; r < rows; r++) {
        if (r % rowsPerGroup == 0) continue;
        uint8_t groupsBefore = r / rowsPerGroup;
        int y = lowerY + r * rowH + groupsBefore * groupSepH;
        u8g2.drawHLine(barX, y, barW);
      }
    }
    u8g2.setDrawColor(1);

    // =======================================================================
    // x128 only: one 1px vertical tick in the last display column (x=127,
    // just right of the grid) for every row not yet fully completed -
    // ported from the Eurorack variant's remaining-package indicator.
    // There, packages group 16 bars each; here a "row" is already the
    // natural equivalent (8 bars per row at x128), so one tick per
    // remaining row does the same job without introducing a second
    // grouping concept. The row currently in progress blinks at the
    // normal beat rate and loses its tick for good once fully filled -
    // so the ticks visibly count down from 16 to 0 across the cycle.
    // =======================================================================
    if (div_ == 128) {
      int tickX = barX + barW + 1; // x=127, the one spare pixel column right of the grid
      for (uint8_t r = 0; r < rows; r++) {
        if (r < activeRow) continue;                          // row already fully played -> tick gone
        if (r == activeRow && !beatPulseOnNormal) continue;    // row in progress -> blinks
        uint8_t groupsBefore = r / rowsPerGroup;
        int y = lowerY + r * rowH + groupsBefore * groupSepH;
        u8g2.drawVLine(tickX, y, rowH);
      }
    }
  }
  } // end of the pre-existing divisor-cycle grid (else branch of "if (gridMode == GRIDMODE_SCROLLING)" above)

  // ---------------- Footer: B+C (bar/cycle end) | time signature (H) | divisor (I) ----------------
  const int footY = 62;

  if (patternEditActive) {
    // BAR (B+C) stays in its usual left-side spot and format - just
    // repurposed: B is now the edit cursor's own bar instead of the
    // live transport's, and C (normally the next cycle-end bar) is now
    // the end of the current 16-bar window the cursor is in, so you
    // can see how much room is left before it pages. H is replaced by
    // the current paint tool (blinking, cycled with Reset - see
    // patternEditTool/onResetButton()); divisor/time signature aren't
    // meaningful controls here (Custom/Divisor navigate the cursor
    // instead - see onCustomButton()/onDivisorButton()), so I instead
    // shows how many of the available marker slots are used.
    char cursorBarBuf[16];
    sprintf(cursorBarBuf, "BAR %lu", (unsigned long)patternEditCursorBar);
    int cursorBarWidth = u8g2.getStrWidth(cursorBarBuf);
    u8g2.drawStr(2, footY, cursorBarBuf);
    if ((millis() / PATTERN_EDIT_BLINK_MS) % 2 == 0) {
      uint32_t windowStart = ((patternEditCursorBar - 1) / 16) * 16; // 0-based
      uint32_t cycleEndBar = windowStart + 16;
      char cycleBuf[10];
      snprintf(cycleBuf, sizeof(cycleBuf), ">%lu", (unsigned long)cycleEndBar);
      u8g2.drawStr(2 + cursorBarWidth + 2, footY, cycleBuf);
    }

    // How many of the (now 500) available marker slots are used - the
    // one thing that actually limits how far you can paint, not the
    // bar range itself (which goes to 9999) - see
    // paintPatternMarkerAtCursor(). Computed here, ahead of its own
    // draw call further down, since the tool indicator's centering
    // below needs its width too.
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%u/%u", patternMarkerCount, PATTERN_MARKER_COUNT);
    int countWidth = u8g2.getStrWidth(countBuf);

    // Tool indicator centers in the space actually left between BAR
    // (left) and the marker count (right), rather than a fixed screen
    // position - stays visually balanced regardless of how wide
    // either neighbor's text is. Deliberately ignores the blinking
    // cycle-end text (">N") that sometimes appears right after BAR -
    // including it would make the tool indicator jump sideways on
    // every beat pulse instead of staying put.
    int toolAreaLeft  = 2 + cursorBarWidth + 2;
    int toolAreaRight = 126 - countWidth - 2;
    int toolCenterX   = (toolAreaLeft + toolAreaRight) / 2;

    bool toolBlinkOn = (millis() / PATTERN_EDIT_BLINK_MS) % 2 == 0;
    if (patternEditTool == 4) {
      const char* delStr = "DEL";
      int w = u8g2.getStrWidth(delStr);
      drawBlinkableText(toolCenterX - w / 2, footY, delStr, toolBlinkOn);
    } else {
      // drawBlinkableSymbol()'s 9px-wide highlight box is centered 3px
      // right of the x it's given (box spans x-1..x+7) - offset here
      // so the box itself, not its origin, lands on toolCenterX.
      drawBlinkableSymbol(patternEditTool, toolCenterX - 3, footY - 7, toolBlinkOn);
    }

    u8g2.drawStr(126 - countWidth, footY, countBuf);
  } else {

  char barBuf[16];
  sprintf(barBuf, "BAR %lu", (unsigned long)(barSnapshot + 1)); // 1-based for display
  int barTextWidth = u8g2.getStrWidth(barBuf);

  uint32_t barDisplay1Based = barSnapshot + 1;
  // Same "last bar of the current cycle" computation as CYCLING,
  // now used unconditionally in SCROLLING too - the grid itself
  // already shows the next marker scrolling in from the right (see
  // renderScrollingGrid()), so this blinking footer number stays the
  // plain cycle end in both modes rather than duplicating that
  // lookahead here.
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

  } // end of the pre-existing footer content (else branch of "if (patternEditActive)" above)

  if (leaveComboHoldActive) {
    uint32_t heldMs = millis() - leaveComboHoldStartMs;
    drawHoldSweep(patternEditActive ? "LEAVE" : "OPERATION SETUP", heldMs, 1000, 35);
  } else if (nudgeComboHoldActive) {
    uint32_t heldMs = millis() - nudgeComboHoldStartMs;
    // Same toggle combo both ways - currentMode hasn't changed yet
    // while the hold is still in progress, so it still reflects
    // which direction this hold is heading: MODE_NUDGE means this
    // hold is on its way back OUT (show LEAVE, like every other exit
    // gesture), anything else means it's heading IN (show NUDGE).
    drawHoldSweep(currentMode == MODE_NUDGE ? "LEAVE" : "NUDGE", heldMs, 1000, 35);
  } else if (!patternEditActive && currentMode == MODE_NORMAL && btnCustom.isPressed &&
             !btnDivisor.isPressed && !btnReset.isPressed && !customLongAlreadyHandled) {
    // Solo Custom hold heading toward onCustomButtonLongHeldDuringPress()'s
    // Analyzer toggle - shown only in MODE_NORMAL (entering), not
    // MODE_ANALYZER (exiting uses renderAnalyzer()'s own screen instead,
    // so this sweep can't reach it there). customLongAlreadyHandled
    // guards against the same continuous press that just toggled us
    // OUT of the analyzer immediately rendering as a "complete" entry
    // sweep again - see the matching guard in renderAnalyzer().
    uint32_t heldMs = millis() - btnCustom.pressStartMs;
    drawHoldSweep("MIDI MONITOR", heldMs, LONGPRESS_MS, 35);
  }

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

// BarSync logo: one filled tile followed by three outlined tiles, matching
// BarSync_logo_4tiles.svg (4x 10mm squares, 1mm gaps, first solid, rest
// framed). At this pixel size a 1px u8g2.drawFrame() border is the natural
// monochrome equivalent of the SVG's ~12%-of-tile-width outline stroke.
const int LOGO_TILE = 8;
const int LOGO_GAP  = 1;
const int LOGO_W    = LOGO_TILE * 4 + LOGO_GAP * 3; // 35px total
void drawBarSyncLogo(int x, int y, uint8_t filledCount) {
  // filledCount (0-4): that many tiles from the left are drawn solid,
  // the rest as outline only - doubles as a coarse 4-step loading bar
  // during boot, filling up in step with the cursor blink (see
  // computeLogoFilledCount() in showBootScreen()).
  for (uint8_t i = 0; i < 4; i++) {
    int tx = x + i * (LOGO_TILE + LOGO_GAP);
    if (i < filledCount) {
      u8g2.drawBox(tx, y, LOGO_TILE, LOGO_TILE);
    } else {
      u8g2.drawFrame(tx, y, LOGO_TILE, LOGO_TILE);
    }
  }
}

// Maps elapsed boot time to how many logo tiles should be filled: one
// more tile at the start of every blink cycle (cursor-on + cursor-off,
// i.e. 2x blinkPeriod), capped at 4 - so all 4 fill exactly by the time
// the loading phase (BOOT_TEXT_DURATION) ends, in step with the cursor.
uint8_t computeLogoFilledCount(uint32_t elapsed, uint32_t blinkPeriod) {
  uint32_t cycle = elapsed / (blinkPeriod * 2);
  uint32_t filled = cycle + 1;
  return (filled > 4) ? 4 : (uint8_t)filled;
}

void drawBootTextFrame(const char* ramLine, const char* fwLine, bool cursorOn, uint32_t animT, uint8_t visibleLines = 4, bool showInvader = true, uint8_t logoFilled = 4) {
  if (visibleLines >= 1) {
    drawBarSyncLogo(2, 1, logoFilled);
    u8g2.drawStr(2 + LOGO_W + 4, 9, "**BARSYNC**");
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
    drawBootTextFrame(ramLine, fwLine, false, 0, visibleLines, /*showInvader=*/false, /*logoFilled=*/0);
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
    drawBootTextFrame(ramLine, fwLine, cursorOn, elapsed, 4, true, computeLogoFilledCount(elapsed, blinkPeriod));
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
    drawBootTextFrame(ramLine, fwLine, cursorStillThere, BOOT_TEXT_DURATION, 4, true, /*logoFilled=*/4);
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
    drawBootTextFrame(ramLine, fwLine, false, BOOT_TEXT_DURATION, 4, true, /*logoFilled=*/4); // cursor destroyed, stays off
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
//       GRID >                   (page 3: existing checkbox list)
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
// Same drawHoldSweep()-style sweep used everywhere else, reused here
// too (see menuHoldSweepLabel/menuHoldSweepHeldMs, already shared with
// Operation Setup) - menuHoldSweepTotalMs is this menu's own addition
// since its top-level exit takes VERYLONG_MS (3s, since it also saves
// and reboots) rather than the usual LONGPRESS_MS (1s) everywhere
// else - see runSettingsMenu().
uint32_t menuHoldSweepTotalMs = 1000;
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
    case SCR_DISPLAY:          return 3;
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
    case SCR_DIVISOR:          return "GRID SELECT";
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
const char* SWITCHES_NAMES[] = {"CUSTOM", "GRID", "RESET"};

// Item names for the editable leaves that have room for a footer on
// this display (name + value).
const char* DISPLAY_NAMES[]      = {"CONTRAST", "INVERT", "GRID LINES"};

// Short explanations of what each item is for, shown at the bottom of
// the final settings page, set off by a divider line (see
// renderHelpFooterSmall()). Kept to short lines that fit the 128px width at
// this font size. The GRID and TIMESIG_ENABLE checkbox pages skip
// the footer: with up to 8/5 items there simply isn't vertical room
// left on this 128x64 landscape display (unlike the Eurorack's taller
// portrait screen), and the checkboxes are self-explanatory anyway.
const char* HELP_DISPLAY[]      = {"CONTRAST: level", "INVERT: b/w swap", "GRID LINES: 32-bar seps"};
const char* HELP_STANDBY[]      = {"ON/OFF: auto sleep", "TIME: sleep delay"};
const char* HELP_CONFIRM[]      = {"WARNING: resets", "ALL settings!"};

void menuGetValueStr(MenuScreen s, uint8_t item, char* buf, size_t bufLen) {
  switch (s) {
    case SCR_DISPLAY:
      if (item == 0)      snprintf(buf, bufLen, "%u", settings.contrast);
      else if (item == 1) snprintf(buf, bufLen, "%s", settings.invert ? "ON" : "OFF");
      else                snprintf(buf, bufLen, "%s", settings.gridSeparatorsEnabled ? "YES" : "NO");
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
  u8g2.setFont(u8g2_font_5x7_tr); // restore - other code assumes this is the active font
}

// Unified menu grid, used consistently by every settings page: first
// item at MENU_Y0, MENU_STEP px between items, help footer divider
// MENU_FOOTER_GAP px below the last item, footer text lines
// MENU_FOOTER_PITCH px apart (small 4x6 font). Kept identical across
// every screen so the whole menu looks like one consistent design
// instead of each page having its own spacing. Same values as
// Operation Setup's SETUP_MENU_Y0/SETUP_MENU_STEP now that both use
// the same u8g2_font_5x7_tr for their row text.
const int MENU_Y0 = 18;
const int MENU_STEP = 8;
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
    snprintf(line, sizeof(line), "%-11s%s", names[i], valBuf);
    if (i == menuCursor) u8g2.drawStr(2, y, ">");
    u8g2.drawStr(12, y, line);
  }
  int footerY = MENU_Y0 + count * MENU_STEP + MENU_FOOTER_GAP;
  // Only the currently selected item's help line, not the full list -
  // with 3 items (since Grid Lines was added) there's only room for
  // one footer line before it runs off the bottom of the display.
  if (menuCursor < helpCount) {
    renderHelpFooterSmall(footerY, &helpLines[menuCursor], 1, MENU_FOOTER_PITCH);
  }
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
  u8g2.setFont(u8g2_font_5x7_tr);

  if (menuScreen == SCR_TIMESIG_ENABLE) {
    u8g2.drawStr(2, 9, menuHeader(SCR_TIMESIG_ENABLE));
    for (uint8_t i = 0; i < TIME_SIG_COUNT; i++) {
      int y = MENU_Y0 + i * MENU_STEP;
      char line[20];
      snprintf(line, sizeof(line), "[%s] %s", isTimeSigEnabled(i) ? "x" : " ", TIME_SIG_LABEL[i]);
      if (i == menuCursor) u8g2.drawStr(2, y, ">");
      u8g2.drawStr(12, y, line);
    }
    if (menuHoldSweepLabel != nullptr) {
      u8g2.setFont(u8g2_font_5x7_tr);
      drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, menuHoldSweepTotalMs, 35);
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
    if (menuHoldSweepLabel != nullptr) {
      u8g2.setFont(u8g2_font_5x7_tr);
      drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, menuHoldSweepTotalMs, 35);
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
    if (menuHoldSweepLabel != nullptr) {
      u8g2.setFont(u8g2_font_5x7_tr);
      drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, menuHoldSweepTotalMs, 35);
    }
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
    if (menuHoldSweepLabel != nullptr) {
      u8g2.setFont(u8g2_font_5x7_tr);
      drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, menuHoldSweepTotalMs, 35);
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
  if (menuHoldSweepLabel != nullptr) {
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, menuHoldSweepTotalMs, 35);
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
      } else if (menuCursor == 1) {
        settings.invert = !settings.invert;
        u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6); // live preview
      } else {
        settings.gridSeparatorsEnabled = !settings.gridSeparatorsEnabled;
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
  menuWillRestart = false;
  menuDefaultsLoadedUntilMs = 0;

  // The reset button is typically still physically held when entering
  // the menu (the entry hold itself). So that this hold doesn't
  // immediately trigger an action, the button must be released once
  // before it's considered here at all.
  bool resetReleasedOnce = false;

  // Own state tracking for the reset button in the menu: short =
  // value change, 1s = one level up, 3s at the top level = save, exit
  // and restart (longer than the usual 1s elsewhere since this one
  // also reboots the device - the sweep shown for it makes the
  // difference obvious either way, see the sweep block below).
  bool     resetIsDown       = false;
  uint32_t resetPressStartMs = 0;
  bool     longActionHandled = false;

  u8g2.setFont(u8g2_font_5x7_tr); // renderMenu() also sets this each frame - kept here for clarity, same convention as runOperationSetupMenu()

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
            longActionHandled = true;
          }
        } else {
          // Top level: holding 3s total saves, exits, and restarts.
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
      }
    }

    // Same hold-to-confirm sweep used everywhere else (see
    // drawHoldSweep()) - BACK on sub-screens (1s), or SAVE & RESTART
    // at the top level (3s, since exiting here always means saving
    // and rebooting, not just leaving).
    menuHoldSweepLabel = nullptr;
    if (resetReleasedOnce && rawReset && resetIsDown && !longActionHandled) {
      bool atTop = (menuScreen == SCR_TOP);
      menuHoldSweepLabel   = atTop ? "SAVE & RESTART" : "BACK";
      menuHoldSweepHeldMs  = millis() - resetPressStartMs;
      menuHoldSweepTotalMs = atTop ? VERYLONG_MS : LONGPRESS_MS;
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
// OPERATION SETUP SCREEN (Reset+Custom held together, from normal operation)
// ---------------------------------------------------------------------------
// Reachable any time during normal operation (unlike the main SETUP
// tree above, which is boot-entry only) - lets you switch the play
// view (see gridMode), set an End Bar (see endBarMode/endBarManualValue/endBarAction),
// and manage Pattern Mode's markers.
//
//   OPERATION SETUP (top)
//     GRID MODE     (CYCLE/SCROLL)
//     SET MARKERS   N/500 - count of markers currently placed; hands
//                       off to the live paint editor on the main
//                       screen (see patternEditActive) rather than a
//                       menu screen
//     DELETE ALL MARKERS -> YES/NO confirm - only shown once there's
//                       at least one marker to delete
//     END BAR       OFF / LAST MARKER / MANUAL - mode only, a plain
//                       toggle like GRID MODE (Reset short cycles
//                       it, in that order) - see onSetupMenuChange()/
//                       resolveEndBar()
//     BAR NUMBER    the actual bar number for MANUAL (stepped in
//                       groups of 4) - only shown while END BAR is
//                       set to MANUAL; selecting/editing it works
//                       like the old combined END BAR row used to:
//                       Reset short opens it for editing (blinks),
//                       Custom/Grid then step the value, Reset held
//                       steps back to browsing - see endBarEditActive
//     END BAR ACT   (LOOP/STOP/CONTINUE - see handleClock()) - only
//                       shown once END BAR is set to something other
//                       than OFF
//
// DELETE ALL MARKERS, BAR NUMBER, and END BAR ACT's visibility, and
// which row ends up at which position, are all decided in exactly one
// place - buildSetupTopRows() - so setupMenuItemCount()/
// onSetupMenuChange()/renderOperationSetupMenu() can never disagree
// about the layout.
//
// Navigation: Custom = up / Divisor = down while browsing; Reset short
// = select/toggle - on END BAR that just cycles OFF -> LAST MARKER ->
// MANUAL -> OFF directly, same as any other toggle row (e.g. GRID
// MODE). On BAR NUMBER specifically, Reset short instead opens it for editing
// (see endBarEditActive) - Custom/Grid then step its value (held,
// with acceleration) instead of moving the cursor, and Reset held
// steps back to browsing rather than leaving the screen, same idea
// Pattern Mode's DEL hold uses. Reset held anywhere else goes back a
// level (a no-op at bare TOP, since there's nothing to go back to
// there); Custom+Reset held together (a fresh chord, not Reset
// already down with Custom joining in) leaves Operation Setup
// entirely after a 1s hold (see the LEAVE chord in
// runOperationSetupMenu()), from any screen, not just TOP - same
// combo that opened it in the first place (see loop()).
// ---------------------------------------------------------------------------
enum SetupMenuScreen { SETUPSCR_TOP, SETUPSCR_CONFIRM_DELETE_ALL };

// Manual prototype - Arduino's ctags-based auto-prototype generator
// doesn't reliably handle a custom enum as a parameter type (same
// issue this file has hit before with other menu enums), producing a
// broken forward declaration that leaves SetupMenuScreen undeclared at
// the point it's needed. Declaring it explicitly here, right after the
// enum itself, pre-empts that broken auto-generation.
uint8_t setupMenuItemCount(SetupMenuScreen s);
void    setupEnterScreen(SetupMenuScreen target);

SetupMenuScreen setupMenuScreen     = SETUPSCR_TOP;
uint8_t         setupMenuCursor     = 0;
uint8_t         setupTopCursorSaved = 0;
bool            setupExitRequested  = false; // set by onSetupMenuChange() to unwind runOperationSetupMenu() immediately (e.g. handing off to Pattern Mode's live paint editor)

// True while the BAR NUMBER row is "opened" for editing (Reset short
// on that row toggles this) - Custom/Grid step its value instead of
// moving the cursor while this is active; Reset held then steps back
// to browsing instead of leaving the screen, same idea the old
// row-selected editing system used. See onSetupMenuChange()/
// onSetupMenuUp()/onSetupMenuDown()/stepEndBar().
bool endBarEditActive = false;

// Same row layout as the main SETUP menu's MENU_Y0/MENU_STEP - both
// use u8g2_font_5x7_tr (same font as the main screen's own footer
// line) for a consistent look across every menu in the firmware.
const int SETUP_MENU_Y0   = 18;
const int SETUP_MENU_STEP = 8;

// Builds the list of currently visible TOP rows into outRows (caller
// provides a buffer sized SETUP_TOP_MAX_ROWS) and returns how many are
// actually visible. DELETE ALL MARKERS only shows once there's at
// least one marker to delete; BAR NUMBER only while END BAR is set to
// MANUAL; END BAR ACT only once END BAR is set to something other
// than OFF. Kept in exactly one place so setupMenuItemCount()/
// onSetupMenuChange()/renderOperationSetupMenu() can never disagree
// with each other about which row sits where.
#define SETUP_TOP_MAX_ROWS 6
enum SetupTopRow { ROW_GRID_MODE, ROW_SET_MARKERS, ROW_DELETE_ALL, ROW_END_BAR, ROW_END_BAR_NUMBER, ROW_END_BAR_ACT };

// Manual prototype - same ctags workaround as setupMenuItemCount()/
// setupEnterScreen() above, needed here too since this one takes a
// pointer to the enum rather than the enum itself.
uint8_t buildSetupTopRows(SetupTopRow* outRows);

uint8_t buildSetupTopRows(SetupTopRow* outRows) {
  uint8_t n = 0;
  outRows[n++] = ROW_GRID_MODE;
  outRows[n++] = ROW_SET_MARKERS;
  if (patternMarkerCount > 0) outRows[n++] = ROW_DELETE_ALL;
  outRows[n++] = ROW_END_BAR;
  if (endBarMode == ENDBAR_MODE_MANUAL) outRows[n++] = ROW_END_BAR_NUMBER;
  if (endBarMode != ENDBAR_MODE_OFF) outRows[n++] = ROW_END_BAR_ACT;
  return n;
}

uint8_t setupMenuItemCount(SetupMenuScreen s) {
  switch (s) {
    case SETUPSCR_TOP: {
      SetupTopRow rows[SETUP_TOP_MAX_ROWS];
      return buildSetupTopRows(rows);
    }
    case SETUPSCR_CONFIRM_DELETE_ALL: return 2; // NO, YES
  }
  return 1;
}

void setupEnterScreen(SetupMenuScreen target) {
  if (setupMenuScreen == SETUPSCR_TOP) setupTopCursorSaved = setupMenuCursor;
  setupMenuScreen = target;
  setupMenuCursor = 0;
}

void setupGoBack() {
  switch (setupMenuScreen) {
    case SETUPSCR_CONFIRM_DELETE_ALL:
      setupMenuScreen = SETUPSCR_TOP;
      setupMenuCursor = setupTopCursorSaved;
      break;
    default: break; // SETUPSCR_TOP has no parent - leaving is exclusively the Custom+Reset chord
  }
}

// Nearest multiple of END_BAR_STEP at or below END_BAR_MAX - keeps
// every manual value stepping produces a clean multiple of 4, rather
// than landing on a lone non-multiple at the very top of the range.
#define END_BAR_MANUAL_MAX 9996

// Steps endBarManualValue by END_BAR_STEP (4), clamped to
// [END_BAR_STEP, END_BAR_MANUAL_MAX] - the BAR NUMBER row's Custom/
// Grid stepping while it's open for editing (see endBarEditActive).
// Mode (OFF/LAST MARKER/MANUAL) is a separate, plain toggle on the
// END BAR row itself now (see onSetupMenuChange()) - this only ever
// runs while that mode is already MANUAL, since BAR NUMBER isn't
// shown otherwise (see buildSetupTopRows()).
void stepEndBar(int32_t delta) {
  if (delta > 0) {
    if (endBarManualValue < END_BAR_MANUAL_MAX) endBarManualValue += END_BAR_STEP;
  } else if (delta < 0) {
    if (endBarManualValue > END_BAR_STEP) endBarManualValue -= END_BAR_STEP;
  }
}

void onSetupMenuUp(bool longPress) {
  if (setupMenuScreen == SETUPSCR_TOP && endBarEditActive) { stepEndBar(+1); return; }
  uint8_t count = setupMenuItemCount(setupMenuScreen);
  setupMenuCursor = (setupMenuCursor + count - 1) % count;
}
void onSetupMenuDown(bool longPress) {
  if (setupMenuScreen == SETUPSCR_TOP && endBarEditActive) { stepEndBar(-1); return; }
  uint8_t count = setupMenuItemCount(setupMenuScreen);
  setupMenuCursor = (setupMenuCursor + 1) % count;
}

void onSetupMenuChange() {
  switch (setupMenuScreen) {
    case SETUPSCR_TOP: {
      SetupTopRow rows[SETUP_TOP_MAX_ROWS];
      uint8_t rowCount = buildSetupTopRows(rows);
      if (setupMenuCursor >= rowCount) break; // defensive - see buildSetupTopRows()
      switch (rows[setupMenuCursor]) {
        case ROW_GRID_MODE:
          gridMode = (gridMode + 1) % GRIDMODE_COUNT;
          if (gridMode == GRIDMODE_SCROLLING) {
            divisorIndex = highestEnabledDivisorUpTo16(); // 16, or the next best of 8/4/2 if 16 isn't enabled
          }
          break;
        case ROW_SET_MARKERS:
          // Hands off to the live paint editor on the main screen
          // instead of a menu screen (see patternEditActive) -
          // requesting an exit here lets runOperationSetupMenu()'s own
          // loop unwind normally.
          patternEditActive    = true;
          patternEditCursorBar = 1;
          patternEditTool      = 0;
          setupExitRequested   = true;
          break;
        case ROW_DELETE_ALL:
          setupEnterScreen(SETUPSCR_CONFIRM_DELETE_ALL);
          break;
        case ROW_END_BAR:
          // Cycle order: OFF -> LAST MARKER -> MANUAL -> OFF - not the
          // enum's own declaration order (see EndBarMode), so the
          // enum's numeric values (and anyone's already-saved
          // endBarMode in NVS) stay meaningful across this change.
          switch (endBarMode) {
            case ENDBAR_MODE_OFF:         endBarMode = ENDBAR_MODE_LAST_MARKER; break;
            case ENDBAR_MODE_LAST_MARKER: endBarMode = ENDBAR_MODE_MANUAL;      break;
            case ENDBAR_MODE_MANUAL:      endBarMode = ENDBAR_MODE_OFF;         break;
          }
          break;
        case ROW_END_BAR_NUMBER:
          endBarEditActive = !endBarEditActive; // opens/closes editing for this row's Custom/Grid stepping
          break;
        case ROW_END_BAR_ACT:
          endBarAction = (endBarAction + 1) % ENDBAR_ACTION_COUNT;
          break;
      }
      break;
    }
    case SETUPSCR_CONFIRM_DELETE_ALL:
      if (setupMenuCursor == 1) {
        // YES - clear every marker.
        memset(patternMarkers, 0, sizeof(patternMarkers));
        patternMarkerCount = 0;
        savePatternMarkers();
      }
      setupMenuScreen = SETUPSCR_TOP;
      setupMenuCursor = setupTopCursorSaved;
      break;
  }
}

void renderOperationSetupMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  switch (setupMenuScreen) {
    case SETUPSCR_TOP: {
      u8g2.drawStr(2, 9, "OPERATION SETUP");
      char lineGridMode[22]; snprintf(lineGridMode, sizeof(lineGridMode), "%-11s %s", "GRID MODE", GRIDMODE_LABEL[gridMode]);
      char lineSetMarkers[22]; snprintf(lineSetMarkers, sizeof(lineSetMarkers), "%-11s %u/%u", "SET MARKERS", patternMarkerCount, PATTERN_MARKER_COUNT);
      char lineEndBar[24]; snprintf(lineEndBar, sizeof(lineEndBar), "%-11s %s", "END BAR", ENDBAR_MODE_LABEL[endBarMode]);
      char lineEndBarNumber[22]; snprintf(lineEndBarNumber, sizeof(lineEndBarNumber), "%-11s %lu", "BAR NUMBER", (unsigned long)endBarManualValue);
      char lineEndBarAct[24]; snprintf(lineEndBarAct, sizeof(lineEndBarAct), "%-11s %s", "END BAR ACT", ENDBAR_ACTION_LABEL[endBarAction]);

      SetupTopRow rows[SETUP_TOP_MAX_ROWS];
      uint8_t count = buildSetupTopRows(rows);
      if (setupMenuCursor >= count) setupMenuCursor = count - 1; // defensive - see buildSetupTopRows()

      bool editBlinkOn = (millis() / 300) % 2 == 0;
      for (uint8_t i = 0; i < count; i++) {
        int y = SETUP_MENU_Y0 + i * SETUP_MENU_STEP;
        if (i == setupMenuCursor) u8g2.drawStr(2, y, ">");
        const char* text;
        switch (rows[i]) {
          case ROW_GRID_MODE:      text = lineGridMode;      break;
          case ROW_SET_MARKERS:    text = lineSetMarkers;    break;
          case ROW_DELETE_ALL:     text = "DELETE ALL MARKERS"; break;
          case ROW_END_BAR:        text = lineEndBar;        break;
          case ROW_END_BAR_NUMBER: text = lineEndBarNumber;  break;
          case ROW_END_BAR_ACT:    text = lineEndBarAct;     break;
          default:                 text = "";                break;
        }
        if (rows[i] == ROW_END_BAR_NUMBER && endBarEditActive) {
          drawBlinkableText(12, y, text, editBlinkOn);
        } else {
          u8g2.drawStr(12, y, text);
        }
      }
      break;
    }
    case SETUPSCR_CONFIRM_DELETE_ALL: {
      u8g2.drawStr(2, 9, "DELETE ALL MARKERS?");
      const char* labels[2] = {"NO", "YES"};
      for (uint8_t i = 0; i < 2; i++) {
        int y = SETUP_MENU_Y0 + i * SETUP_MENU_STEP;
        if (i == setupMenuCursor) u8g2.drawStr(2, y, ">");
        u8g2.drawStr(12, y, labels[i]);
      }
      break;
    }
  }

  if (menuHoldSweepLabel != nullptr) {
    drawHoldSweep(menuHoldSweepLabel, menuHoldSweepHeldMs, LONGPRESS_MS, 35);
  }

  u8g2.sendBuffer();
}

// Blocking - MIDI clock ticks aren't processed while this is open
// (same trade-off the main SETUP menu already makes). Any edit made
// here takes effect immediately - there's no "confirm" step needed
// for a menu this small.
void runOperationSetupMenu() {
  setupMenuScreen     = SETUPSCR_TOP;
  setupMenuCursor     = 0;
  setupTopCursorSaved = 0;
  setupExitRequested  = false;
  endBarEditActive    = false;

  bool exitRequested = false;

  // Custom is typically still physically held on entry (part of the
  // Reset+Custom combo that opened this screen) - its first release
  // must be swallowed without counting as a press, same idea as
  // resetReleasedOnce below for Reset.
  bool customReleasedOnce = false;
  bool resetReleasedOnce  = false;

  bool     resetIsDown       = false;
  uint32_t resetPressStartMs = 0;
  bool     longActionHandled = false;

  u8g2.setFont(u8g2_font_5x7_tr); // renderOperationSetupMenu() also sets this each frame - kept here for clarity

  while (!exitRequested) {
    // --- Custom button (up) ---
    bool rawCustomLevel = digitalRead(PIN_BTN_CUSTOM);
    if (!customReleasedOnce) {
      // NOTE: stableState mirrors digitalRead() directly (HIGH/LOW),
      // same convention as everywhere else in this file - not an "is
      // pressed" boolean, which is the opposite (LOW = pressed,
      // pull-up wiring).
      btnCustom.stableState  = rawCustomLevel;
      btnCustom.isPressed    = (rawCustomLevel == LOW);
      btnCustom.lastChangeMs = millis();
      if (rawCustomLevel == HIGH) customReleasedOnce = true;
    } else {
      updateButton(btnCustom, onSetupMenuUp);
    }

    // --- Divisor button (down) ---
    updateButton(btnDivisor, onSetupMenuDown);

    // --- Reset button ---
    bool rawReset = (digitalRead(PIN_BTN_RESET) == LOW);
    if (!rawReset) resetReleasedOnce = true;

    if (resetReleasedOnce) {
      if (rawReset && !resetIsDown) {
        resetIsDown       = true;
        resetPressStartMs = millis();
        longActionHandled = false;
      } else if (rawReset && resetIsDown && !longActionHandled) {
        if (millis() - resetPressStartMs >= LONGPRESS_MS) {
          longActionHandled = true;
          if (setupMenuScreen == SETUPSCR_TOP && endBarEditActive) {
            endBarEditActive = false; // step back to browsing, not a full screen back
          } else {
            setupGoBack(); // no-op at bare TOP - leaving is exclusively the Custom+Reset chord below
          }
        }
      } else if (!rawReset && resetIsDown) {
        resetIsDown = false;
        if (!longActionHandled) {
          onSetupMenuChange();
          if (setupExitRequested) exitRequested = true;
        }
      }
    }

    // LEAVE from anywhere: Custom+Reset held together (same chord that
    // opened this menu in the first place - see loop()'s Operation Setup
    // entry combo) exits the whole menu regardless of which screen
    // it's on, not just from TOP. Same "close press-start timing"
    // check as that outer combo, using the local press-time tracking
    // already kept here for Custom/Reset - reused rather than
    // duplicated state, since leaveComboHoldActive/leaveComboHoldStartMs
    // are the same globals render()/drawHoldSweep() already read.
    bool comboCustomDown = (rawCustomLevel == LOW);
    int32_t comboDiff = (int32_t)(btnCustom.pressStartMs - resetPressStartMs);
    if (comboDiff < 0) comboDiff = -comboDiff;
    bool isLeaveChord = customReleasedOnce && resetReleasedOnce &&
                        comboCustomDown && rawReset && comboDiff <= 400;
    if (isLeaveChord) {
      if (!leaveComboHoldActive) {
        leaveComboHoldActive  = true;
        leaveComboHoldStartMs = millis();
      }
      if (millis() - leaveComboHoldStartMs >= 1000) {
        leaveComboHoldActive = false;
        exitRequested = true; // full exit, whichever screen this was
      }
    } else {
      leaveComboHoldActive = false;
    }

    // Sweep feedback: the LEAVE chord above takes priority when it's
    // in progress; otherwise, the same sweep covers a plain Reset hold
    // that means "go back a level" (or step out of BAR NUMBER editing) -
    // except at bare TOP with nothing open, where that no longer does
    // anything, so no sweep is shown for it there - see drawHoldSweep().
    menuHoldSweepLabel = nullptr;
    if (isLeaveChord && leaveComboHoldActive) {
      menuHoldSweepLabel  = "LEAVE";
      menuHoldSweepHeldMs = millis() - leaveComboHoldStartMs;
    } else if (resetReleasedOnce && rawReset && resetIsDown && !longActionHandled) {
      bool atBareTop = (setupMenuScreen == SETUPSCR_TOP && !endBarEditActive);
      if (!atBareTop) {
        menuHoldSweepLabel  = "BACK";
        menuHoldSweepHeldMs = millis() - resetPressStartMs;
      }
    }

    // Accelerating repeat while Custom/Grid is held during BAR NUMBER
    // editing - same idea as Pattern Edit's fast-scroll, just stepping
    // the value instead of paging bars: starts at +/-10 per 150ms,
    // ramps to +/-100 per 150ms once held past 2s, so the full
    // 0-9999 range stays reachable without an excessive wait. A short
    // press's own single +/-1 step (via onSetupMenuUp/onSetupMenuDown
    // above) already covers fine adjustment.
    if (setupMenuScreen == SETUPSCR_TOP && endBarEditActive) {
      static uint32_t endBarLastRepeatMs = 0;
      bool customHeld  = btnCustom.isPressed  && (millis() - btnCustom.pressStartMs  >= 400);
      bool divisorHeld = btnDivisor.isPressed && (millis() - btnDivisor.pressStartMs >= 400);
      if ((customHeld || divisorHeld) && millis() - endBarLastRepeatMs >= 150) {
        endBarLastRepeatMs = millis();
        uint32_t heldMs = customHeld ? (millis() - btnCustom.pressStartMs) : (millis() - btnDivisor.pressStartMs);
        int32_t step = (heldMs >= 2000) ? 100 : 10;
        for (int32_t s = 0; s < step; s++) stepEndBar(customHeld ? +1 : -1);
      }
    }

    renderOperationSetupMenu();
    delay(30);
  }

  savePatternMarkers();

  // Resync both buttons' debounce state with reality before returning
  // to the outer loop() - Custom's own release was already consumed
  // above for navigation, and Reset was only ever read raw here, so
  // without this either could otherwise be misread as a fresh edge (or
  // a stale still-pressed one) by the outer loop()'s own updateButton()
  // calls right after this returns.
  btnCustom.stableState = digitalRead(PIN_BTN_CUSTOM);
  btnCustom.isPressed   = false;
  btnDivisor.stableState = digitalRead(PIN_BTN_DIVISOR);
  btnDivisor.isPressed   = false;
  btnReset.stableState  = digitalRead(PIN_BTN_RESET);
  btnReset.isPressed    = false;

  u8g2.setContrast(settings.contrast);
  u8g2.sendF("c", settings.invert ? 0xA7 : 0xA6);
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
  loadPatternMarkers();

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
  MIDI.setHandleNoteOn(handleMonNoteOn);
  MIDI.setHandleNoteOff(handleMonNoteOff);
  MIDI.setHandleControlChange(handleMonCC);
  MIDI.setHandleProgramChange(handleMonPC);
  MIDI.setHandlePitchBend(handleMonPitchBend);
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
// LOOP
// ---------------------------------------------------------------------------
uint32_t lastRenderMs = 0;
const uint32_t RENDER_INTERVAL_MS = 20; // more frequent updates for smoother blink transitions
// The Analyzer screen has no blink animation to keep smooth, but its
// full-buffer SPI push is still the heaviest single render on the board.
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
  // iteration would then process them late and unevenly - a slow
  // display update must never delay clock processing more than
  // necessary.
  while (MIDI.read()) {}

  updateButton(btnDivisor, onDivisorButton);
  updateButton(btnCustom,  onCustomButton);
  updateButton(btnReset,   onResetButton);

  // Nudge mode: custom button+divisor simultaneously -> in and out
  // (toggle, same combo), now with the same 1s hold-to-confirm as the
  // Operation Setup entry/exit combo below - see nudgeComboHoldActive and
  // drawHoldSweep() in render(). Uses the already-debounced stableState
  // values of the buttons (same debouncing as normal time signature/
  // divisor selection). "armed" flag prevents a still-held combo
  // press from immediately counting as exit again - both buttons must
  // be fully released before the combo can trigger again ("press again").
  // Disabled entirely while the Pattern Mode paint editor is active
  // (patternEditActive) - Custom+Divisor there is already the
  // instant, non-hold tool-cycle chord (see loop() further down), so
  // this slower 1s-hold combo (and its sweep) must never also apply.
  static bool    nudgeComboArmed = true;
  static uint8_t nudgeSoloButton = 0; // 0=none, 1=custom, 2=divisor (currently held alone)
  {
    bool customDown = (btnCustom.stableState == LOW);
    bool divDown   = (btnDivisor.stableState == LOW);
    bool comboNow  = customDown && divDown;

    bool comboEligible = comboNow && nudgeComboArmed && !patternEditActive &&
                         (currentMode == MODE_NORMAL || currentMode == MODE_NUDGE);
    if (comboEligible) {
      if (!nudgeComboHoldActive) {
        nudgeComboHoldActive  = true;
        nudgeComboHoldStartMs = millis();
      }
      if (millis() - nudgeComboHoldStartMs >= 1000) {
        currentMode = (currentMode == MODE_NORMAL) ? MODE_NUDGE : MODE_NORMAL;
        if (currentMode == MODE_NUDGE) nudgeOffsetSixteenths = 0;
        suppressCustomAction = true;
        suppressDivisorAction = true;
        nudgeComboArmed = false;
        nudgeComboHoldActive = false;
        nudgeSoloButton = 0; // discard any ongoing solo detection
      }
    } else {
      nudgeComboHoldActive = false;
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

  // Operation Setup entry / Pattern Edit exit: Custom+Reset held together
  // (same idea as the Custom+Divisor combo above for Nudge, just a
  // different button pair) - the same "armed" flag serves both
  // directions, since only one of the two states can ever apply at
  // once. "armed" prevents a still-held finger from re-triggering
  // immediately on return; MODE_NORMAL guards entry (not exit) out of
  // Analyzer/Nudge, same restriction the Nudge combo places on itself.
  //
  // Exiting the paint editor always lands back in OPERATION SETUP
  // (not straight back to the live grid) - from there, the normal
  // Reset-held-at-TOP-level exit takes you the rest of the way out.
  //
  // isSimultaneousChord matters specifically inside the paint editor:
  // holding Reset while tapping Custom/Divisor to paint is now a
  // normal, frequent interaction there (see onCustomButton()/
  // onDivisorButton()), so "both down at once" alone can't mean exit -
  // Reset is very often already held when Custom/Divisor joins in.
  // Requiring the two presses to have actually started close together
  // in time is what tells a genuine fresh chord apart from that.
  //
  // Once a genuine chord is detected, both buttons must then stay held
  // for a further 1s (see leaveComboHoldActive) before anything
  // actually happens - drawHoldSweep() shows a "LEAVE"/"MENU" sweep in
  // render() for the duration, the same confirm-by-holding feedback
  // used everywhere else a Reset hold means "leave" or "go back" (see
  // runOperationSetupMenu() below).
  static bool operationSetupComboArmed = true;
  {
    bool customDown = (btnCustom.stableState == LOW);
    bool resetDown  = (btnReset.stableState == LOW);
    bool isSimultaneousChord = isChordTiming(btnCustom.pressStartMs, btnReset.pressStartMs);
    if (customDown && resetDown && operationSetupComboArmed && isSimultaneousChord) {
      if (!leaveComboHoldActive) {
        leaveComboHoldActive  = true;
        leaveComboHoldStartMs = millis();
      }
      if (millis() - leaveComboHoldStartMs >= 1000) {
        operationSetupComboArmed  = false;
        leaveComboHoldActive = false;
        if (patternEditActive) {
          // Exit the live paint editor and persist the markers, then
          // go straight into Operation Setup - runOperationSetupMenu()
          // handles its own button-state resync when it eventually
          // exits, so nothing needs to be suppressed here for the
          // outer loop().
          patternEditActive = false;
          savePatternMarkers();
          runOperationSetupMenu(); // lands on TOP, same as a fresh entry
        } else if (currentMode == MODE_NORMAL) {
          runOperationSetupMenu(); // blocking, see there
        }
      }
    } else {
      leaveComboHoldActive = false;
    }
    if (!customDown && !resetDown) {
      operationSetupComboArmed = true;
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

  // Pattern Mode marker editor fast-scroll: unlike the one-shot
  // trigger above, these run every iteration for as long as the
  // button stays held, so they can keep paging every
  // PATTERN_EDIT_FAST_SCROLL_MS rather than firing once.
  onCustomHeldDuringPress();
  onDivisorHeldDuringPress();

  // Pattern Mode paint-on-hold: the moment Reset goes down (not on
  // release, and not tied to any navigation) immediately paints/erases
  // the bar the cursor is already sitting on - so holding Reset and
  // then moving continues painting from where you already are, rather
  // than only starting once you move to a new bar. Edge-triggered via
  // its own static, since btnReset.isPressed itself stays true for the
  // whole hold and would otherwise repaint the same bar every
  // iteration for no reason.
  {
    static bool resetWasPressedInPatternEdit = false;
    if (patternEditActive && btnReset.isPressed && !resetWasPressedInPatternEdit) {
      paintPatternMarkerAtCursor();
    }
    resetWasPressedInPatternEdit = patternEditActive && btnReset.isPressed;
  }

  // Pattern Mode tool-cycle: Custom+Divisor pressed together (the same
  // "close press-start timing" chord concept as the Operation Setup entry/
  // exit combo, just without a hold-to-confirm - selecting a tool is
  // low-stakes, so it fires the instant the chord is recognized rather
  // than requiring a sustained hold). Plain rotation - NONE... wait,
  // there's no NONE anymore: SQUARE -> CIRCLE -> TRIANGLE -> X -> DEL ->
  // SQUARE, straight through, DEL included like any other stop.
  // suppressCustomAction/suppressDivisorAction stop each button's
  // normal navigate action from also firing once the chord is
  // released - without them, the cursor would move back then forward
  // (or vice versa) right after every tool change.
  static bool patternToolComboArmed = true;
  if (patternEditActive) {
    bool toolComboCustomDown  = (btnCustom.stableState == LOW);
    bool toolComboDivisorDown = (btnDivisor.stableState == LOW);
    int32_t toolComboDiff = (int32_t)(btnCustom.pressStartMs - btnDivisor.pressStartMs);
    if (toolComboDiff < 0) toolComboDiff = -toolComboDiff;
    bool isToolChord = toolComboCustomDown && toolComboDivisorDown && toolComboDiff <= 400;
    if (isToolChord && patternToolComboArmed) {
      patternToolComboArmed = false;
      patternEditTool = (patternEditTool + 1) % 5;
      suppressCustomAction  = true;
      suppressDivisorAction = true;
    }
    if (!toolComboCustomDown && !toolComboDivisorDown) {
      patternToolComboArmed = true;
    }
  } else {
    patternToolComboArmed = true;
  }

  // Every button press also counts as "activity" and keeps standby
  // away / cancels a running countdown (not just MIDI bytes).
  if (btnDivisor.isPressed || btnCustom.isPressed || btnReset.isPressed) {
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

  uint32_t effectiveRenderIntervalMs = (currentMode == MODE_ANALYZER) ? ANALYZER_RENDER_INTERVAL_MS : RENDER_INTERVAL_MS;
  if (millis() - lastRenderMs >= effectiveRenderIntervalMs) {
    lastRenderMs = millis();
    if (currentMode == MODE_ANALYZER) {
      renderAnalyzer();
    } else {
      render();
    }
  }
}
