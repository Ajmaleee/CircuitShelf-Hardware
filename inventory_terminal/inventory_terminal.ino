/*
 * ESP32 Electronics Inventory Terminal
 * STAGE 6 (Increment 3 of several — see note below) — OLED UI redesign,
 * building on Increments 1-2 (3x3 icon-grid launcher, watch-style idle
 * clock, redesigned search + aliases, Timer/Pomodoro progress bars,
 * alarm/event animations). Nothing below the UI layer was touched in
 * any increment.
 *
 * ---- Stage 6, Increment 3 (this revision) ----
 *  - SETTINGS is now actually editable: idle timeout (5-600s, digit
 *    entry) and a buzzer mute toggle. Both are session-only; they reset
 *    to their #define defaults on reboot, not yet persisted to flash.
 *  - RTC date is now settable from the Clock screen ('C', enter
 *    DDMMYY) alongside the existing time and alarm setters — writes
 *    only the date/month/year registers, leaving time untouched.
 *  - Navigation-tier transition: entering any feature screen from the
 *    home grid now does a brief (~120ms) non-blocking left-to-right
 *    wipe-in, via u8g2's clip window — genuinely cheap, no double-
 *    buffering trickery, and never blocks keypad input.
 *
 * ---- Fixed: reset on Wi-Fi start ----
 *  - The ESP32's brownout detector is now disabled at the very start of
 *    setup(). This is a workaround for resets caused by the WiFi
 *    radio's current spike sagging a marginal power supply enough to
 *    trip it — NOT a fix for the underlying power issue. See the chat
 *    reply for the real fix (better cable/supply).
 *
 * ---- Stage 6, Increment 2 (this revision) ----
 *  - SEARCH: redesigned as a compact terminal-style screen — "SEARCH"
 *    title, "> query_" prompt line, up to 3 result rows with the
 *    available quantity right-aligned, fixed "A/B browse #:select"
 *    hint. A and B now browse results too, alongside the original C/D
 *    (both work — nothing old was removed). Search also now expands
 *    through a small alias table grounded in this device's real
 *    inventory (e.g. typing OLED also matches "SH1106", GPS also
 *    matches "Neo 6M GPS module") without touching the stored data.
 *  - TIMER vs POMODORO now have distinct screens. Both show a graphical
 *    progress bar (drawProgressBar, shared). Pomodoro additionally
 *    shows "FOCUS n/N" or "BREAK n/N" and now actually cycles through
 *    POMO_CYCLES_PER_SESSION focus/break pairs automatically (classic
 *    Pomodoro = 4), rather than stopping after one.
 *  - ALARM screen (drawAlarmAnimation) now does a non-blocking full-
 *    screen invert pulse (~250ms) for as long as the buzzer melody
 *    plays, instead of static text.
 *  - Lightest "event" tier: a brief single invert-flash (100-120ms) on
 *    a successful lend/return and on any validation error, shown on
 *    the item-detail and quantity screens — reuses the same XOR-invert
 *    technique as the alarm, just a single pulse instead of a loop.
 *  - RTC moved AGAIN, this time to GPIO19 (SDA) / GPIO23 (SCL) — the
 *    previous 16/17 aren't broken out on this particular board (common
 *    on WROVER-style modules, where 16/17 are used internally for
 *    PSRAM). 19/23 are standard, non-strapping, unused elsewhere.
 *
 * Target: Arduino IDE, ESP32 Arduino Core 3.x, ESP32 DevKit V1
 * NOT using PlatformIO.
 *
 * ---- Stage 6, Increment 1 (this revision) ----
 *  - HOME LAUNCHER: the old scrolling text menu is replaced by a 3x3
 *    grid of vector-drawn monochrome icons (Timer, Pomodoro, Stopwatch,
 *    Search, Wi-Fi, Clock, Inventory, Settings, More), navigated with
 *    A=left, B=right, C=up, D=down, #=select, *=sleep. The selected
 *    icon gets an animated corner-bracket focus indicator (non-blocking,
 *    ~150ms cycle) and its name is shown centered beneath the grid.
 *    `STATE_MENU` was renamed `STATE_HOME` throughout; every screen that
 *    returns to "the menu" (alarm dismiss, wake-from-idle, the back key
 *    cancels) now returns to this grid, unchanged in behavior.
 *  - IDLE CLOCK: redesigned as a minimal digital-watch screen — one big
 *    time, nothing else. When an RTC is present it now also shows the
 *    date (DD MON YYYY) beneath the time, reading date/month/year from
 *    the RTC for the first time (previously only time was read). Falls
 *    back to plain uptime with no date line if no RTC is present, same
 *    as before.
 *  - New Settings and More screens fill out the 3x3 grid honestly:
 *    Settings shows read-only device info (idle timeout, RTC/Wi-Fi
 *    status); More is a clearly-labeled "coming soon" placeholder,
 *    rather than pretending either is more functional than it is.
 *  - Nothing else changed: search, lend/return, timers, Pomodoro,
 *    stopwatch, Wi-Fi, web management, import/export, and all storage
 *    logic are untouched and still work exactly as before.
 *
 * Everything from the original Stage 6 doc's checklist is now addressed.
 * What's left is polish, not missing features:
 *  - Settings/idle-timeout/buzzer-mute aren't persisted to flash across
 *    reboot yet (session-only). Easy to add (one more field in the
 *    inventory save, or a tiny separate settings.json) if wanted.
 *  - The transition wipe only fires on grid -> feature-screen entry,
 *    not on every nested screen change (e.g. search result -> item
 *    detail) — scoped that way deliberately to keep things predictable
 *    rather than animating everything, per the doc's own "don't animate
 *    everything" philosophy.
 *
 * ---- Stage 1-2: physical terminal + robust storage ----
 * Keypad/OLED search (Nokia multi-tap), lend/return, safe LittleFS
 * writes (temp file -> verify -> backup rotate -> atomic rename),
 * corruption recovery, field validation.
 *
 * ---- Stage 3-4: Wi-Fi + full web management ----
 * On-demand Wi-Fi AP (OFF by default — see "Wi-Fi on demand" below) with
 * a built-in WebServer, no external web libraries. "/inventory" has
 * search, Add/Edit/Delete, all validated server-side and sharing the
 * same LittleFS inventory as the keypad. Any web-triggered change
 * returns the physical terminal to its search screen and recomputes
 * results, since array indices can shift after a delete.
 *
 * ---- Session tools: menu, idle screen, timer/Pomodoro/stopwatch ----
 * Any key wakes the screen into a MENU (Search / Lent items / Timer /
 * Pomodoro / Stopwatch / Wi-Fi / Clock); the wake keypress itself is
 * swallowed. A running Timer/Pomodoro/Stopwatch is woken back into its
 * own screen rather than the menu. Idle after IDLE_TIMEOUT_MS shows a
 * PURE time display — just the big number, nothing else — using the
 * real time-of-day if an RTC is present, otherwise session uptime.
 * OLED dims (never blanks) after DISPLAY_DIM_TIMEOUT_MS; any key wakes
 * it back to full brightness, and any in-progress event — alarm, timer,
 * Pomodoro, stopwatch — forces full brightness automatically. The speaker
 * is reserved for alarms only, and alarms play a short repeating original
 * jingle (not a reproduction of any existing song) rather than a flat beep.
 *
 * ---- Optional RTC, on its OWN dedicated I2C bus ----
 * A DS3231/DS1307-compatible RTC is entirely optional and now lives on
 * a second hardware I2C bus (RTC_SDA_PIN/RTC_SCL_PIN), separate from the
 * OLED's bus — this was changed specifically to rule out bus sharing as
 * a cause of detection trouble. If nothing answers at boot, a one-time
 * I2C scan is logged to Serial to help diagnose wiring, and every
 * RTC-dependent feature simply stays off — no crash, then or if it
 * drops out mid-session. "Clock" menu: view/set the time (HHMM) and arm
 * a one-shot HH:MM alarm.
 *
 * ---- Wi-Fi on demand (power saving) ----
 * Wi-Fi AP + web server are OFF by default at boot. The "Wi-Fi" menu
 * item shows status (SSID/IP when on) and toggles it with '#' — the
 * radio, the single biggest power draw on an ESP32, only runs while
 * you're actually updating inventory from a browser.
 *
 * ---- New in Stage 5 ----
 *  - "/import" (GET form, POST file upload): replaces the current
 *    inventory with an uploaded JSON file. Streamed straight to a temp
 *    file (never buffered whole in RAM), capped at MAX_IMPORT_BYTES,
 *    parsed into a scratch buffer and validated exactly like boot-time
 *    loading (bad records skipped/clamped, counts reported back) before
 *    anything live is touched. Quantities currently lent out on THIS
 *    device are carried over automatically for any item that still
 *    matches by name+location — the device, not a possibly-stale
 *    exported file, is the source of truth for what's checked out right
 *    now. Commits through the same safe-save path as everything else,
 *    and rolls back to whatever's genuinely on flash if the save fails.
 *  - "/export": downloads the current inventory as JSON, in exactly the
 *    format "/import" expects back.
 *  - "/restore": confirm-then-restore the inventory from the Stage 2
 *    .bak file — an explicit undo for a bad import or edit.
 *  - Captive portal response improved: unknown paths now get a tiny
 *    HTML page with both a meta-refresh and a manual link alongside the
 *    redirect, since a bare Location header isn't reliably auto-followed
 *    by every OS's captive-portal detector.
 *  - Stopwatch now shows milliseconds (MM:SS.mmm).
 *
 * ---- Removed / changed on request ----
 *  - The LDR-based auto-brightness feature has been fully removed
 *    (config, wiring, code) for simplicity and power.
 *  - Idle timeout raised from 20s to 30s.
 *
 * Known limitation: web row "id" values are just the item's current
 * array index. Two browser tabs open at once, with a delete happening
 * in one, can leave the other tab's Edit/Delete links pointing at the
 * wrong row until it's reloaded. Fine for a single-admin local device.
 *
 * Still NOT implemented (by design, not oversight):
 *  - True deep-sleep / light-sleep. Idle/display-off only dims or blanks
 *    the OLED and idles the loop slightly — the ESP32 core and (when
 *    enabled) Wi-Fi stay fully powered. Real sleep would mean dropping
 *    Wi-Fi/the web server entirely while asleep, which is a bigger
 *    architectural change best done as its own deliberate step.
 */

#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_timer.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"   // esp_reset_reason()

// ============================================================
//  CONFIGURATION — all pin/tunable definitions live here only
// ============================================================

// ---- OLED (SH1106, I2C) ----
#define OLED_SDA_PIN      21
#define OLED_SCL_PIN      22

// ---- 4x4 Keypad ----
#define KEYPAD_ROW_PINS   { 13, 12, 14, 27 }   // R1..R4
#define KEYPAD_COL_PINS   { 26, 25, 33, 32 }   // C1..C4

// ---- Behavior tuning ----
#define MULTITAP_TIMEOUT      800     // ms — max gap between taps on same key
#define KEYPAD_DEBOUNCE_MS    20
#define ERROR_MSG_DURATION_MS 2000

// ---- Data limits (RAM sizing) ----
#define MAX_RECORDS       300         // practical cap, see resource notes
#define NAME_MAX_LEN       52         // 51 chars + null
#define LOC_MAX_LEN         6         // e.g. "A4", "E12" + margin + null
#define STATUS_MAX_LEN     10         // "untested" + null + margin
#define QUERY_MAX_LEN      24
#define QTY_BUF_LEN          6

#define VISIBLE_RESULT_LINES 3   // search's new terminal layout fits 3 result rows cleanly

// ---- Vibration motor ----
// REMOVED: this motor is not present in the hardware. Every startVibration()
// call was writing to an unconnected GPIO, and the motor driver was one of
// the two large current draws firing during alarms on an already-marginal
// supply (suspected contributor to the post-Pomodoro restarts). Routine
// confirm/error feedback now relies on triggerFlash() + the screen, and
// alarms use the speaker alone. If haptics are ever added back, GPIO 4 is
// the pin the driver was wired to.

// ---- Wi-Fi access point + web server ----
#define AP_SSID       "InventoryTerminal"
#define AP_PASSWORD   "inventory123"   // WPA2 needs 8+ chars; use "" for an open network
#define DNS_PORT      53
#define HTTP_PORT     80
#define MAX_IMPORT_BYTES  65536UL      // reject oversized JSON uploads outright
#define IMPORT_TMP_FILE   "/import.tmp"

// ---- Speaker (passive speaker/buzzer via NPN transistor, like the motor) ----
#define SOUND_ENABLED       1
#define SPEAKER_PIN         18

// ---- Key feedback ----
// Was KEY_CLICK_VIBRATION_MS (haptic tick per keypress); removed with the
// motor. Keypresses are already confirmed visually by the focus animation.

// ---- Optional RTC (DS3231/DS1307) on its OWN dedicated I2C bus ----
// Given on a shared bus with the OLED, use separate pins instead — rules
// out bus contention / pull-up / address conflicts as a cause of a
// non-detected RTC, and makes wiring/debugging simpler.
#define RTC_I2C_ADDR   0x68
#define RTC_SDA_PIN    19   // was 16 — not broken out on some ESP32 modules (e.g. WROVER, used for PSRAM)
#define RTC_SCL_PIN    23   // was 17, same reason — 19/23 are free, standard, non-strapping pins
// If no RTC is wired (or it doesn't answer), detection simply fails and
// every RTC feature is skipped automatically — falls back to uptime-only,
// no crash.

// ---- Idle / power / brightness behaviour ----
#define IDLE_TIMEOUT_MS         30000UL    // no key -> pure uptime/clock screen
#define DISPLAY_DIM_TIMEOUT_MS  300000UL   // idle this long -> OLED dims to minimum (0 = never)

// SH1106 contrast runs 0-255. We dim rather than blank on purpose: a fully
// sleeping panel (u8g2.setPowerSave) looks exactly like a crashed or frozen
// unit, which is what a long-running countdown used to look like once this
// timeout elapsed. Dimming keeps the device visibly alive at near-zero cost.
#define CONTRAST_FULL           255
#define CONTRAST_DIM            32         // dim, but still legible in a dark room

// ---- Timer / Pomodoro / alarm ----
#define POMO_WORK_MIN       25
#define POMO_BREAK_MIN      5
#define POMO_CYCLES_PER_SESSION 4   // classic Pomodoro: 4 focus blocks per session
#define ALARM_MAX_MS        30000UL

// ---- Storage ----
// NOTE: the LittleFS data uploader flattens the local "data/" folder to
// the filesystem root, so paths here must NOT include "/data".
#define INVENTORY_FILE      "/inventory.json"
#define INVENTORY_TMP_FILE  "/inventory.tmp"
#define INVENTORY_BAK_FILE  "/inventory.bak"
#define MIN_FREE_BYTES_FOR_SAVE 4096   // safety margin required before writing

// ============================================================
//  DISPLAY
// ============================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ============================================================
//  WEB SERVER / CAPTIVE PORTAL
// ============================================================
WebServer httpServer(HTTP_PORT);
DNSServer dnsServer;
TwoWire RTCWire = TwoWire(1); // dedicated second I2C bus, separate from the OLED's Wire

// ============================================================
//  KEYPAD
// ============================================================
const byte KP_ROWS = 4;
const byte KP_COLS = 4;
char keyMap[KP_ROWS][KP_COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte kpRowPins[KP_ROWS] = KEYPAD_ROW_PINS;
byte kpColPins[KP_COLS] = KEYPAD_COL_PINS;
Keypad keypad = Keypad(makeKeymap(keyMap), kpRowPins, kpColPins, KP_ROWS, KP_COLS);

// ============================================================
//  MULTI-TAP TEXT ENTRY MAP
// ============================================================
const char* MULTITAP_MAP[10] = {
  " 0",        // 0 = space / 0
  ".,-!?1",    // 1 = punctuation / 1
  "ABC2",
  "DEF3",
  "GHI4",
  "JKL5",
  "MNO6",
  "PQRS7",
  "TUV8",
  "WXYZ9"
};

// ============================================================
//  DATA MODEL
// ============================================================
struct InventoryItem {
  char name[NAME_MAX_LEN];
  char location[LOC_MAX_LEN];
  uint16_t qty;
  char status[STATUS_MAX_LEN];
  uint16_t lent;
};

InventoryItem items[MAX_RECORDS];
uint16_t itemCount = 0;
bool inventoryLoadFailed = false;

const char* VALID_STATUSES[] = { "working", "faulty", "untested" };
const uint8_t VALID_STATUS_COUNT = 3;

// ============================================================
//  APP STATE
// ============================================================
enum AppState {
  STATE_SEARCH,
  STATE_ITEM_DETAIL,
  STATE_LEND_QTY,
  STATE_RETURN_QTY,
  STATE_IDLE,
  STATE_HOME,
  STATE_TIMER,
  STATE_STOPWATCH,
  STATE_ALARM,
  STATE_WIFI,
  STATE_CLOCK,
  STATE_SETTINGS,
  STATE_MORE
};
AppState state = STATE_SEARCH;

// Search / text entry
char query[QUERY_MAX_LEN] = "";
uint8_t queryLen = 0;

char lastMultitapKey = 0;
unsigned long lastMultitapTime = 0;
uint8_t multitapIndex = 0;

// Results
uint16_t resultIndices[MAX_RECORDS];
uint16_t resultCount = 0;
int16_t selectedResult = 0;
int16_t scrollOffset = 0;

// Selected item / lend-return
int16_t selectedItemIndex = -1;
char qtyBuffer[QTY_BUF_LEN] = "";
uint8_t qtyLen = 0;

// Transient error message
char errorMsg[24] = "";
unsigned long errorMsgUntil = 0;

// Brief invert-flash overlay for "event" level feedback (confirm/error),
// drawn by whichever screen is active when triggered. See triggerFlash().
unsigned long flashUntil = 0;

// Session / idle / tools
enum TimerKind { TIMER_NONE, TIMER_COUNTDOWN, TIMER_POMO_WORK, TIMER_POMO_BREAK };
unsigned long lastActivityMs = 0;
bool displayDim = false;   // true = panel dimmed, NOT blanked (see updateIdle)
uint32_t sessionLendUnits = 0, sessionReturnUnits = 0;
bool lentMode = false;                 // search screen showing lent items
int8_t menuSel = 0, menuScroll = 0;
uint8_t timerKind = TIMER_NONE;
unsigned long timerStartMs = 0, timerDurationMs = 0;
char timerInput[4] = "";
uint8_t timerInputLen = 0;
bool pendingBreak = false;             // Pomodoro: start break after alarm dismissed
bool pendingNextFocus = false;         // Pomodoro: start the next focus block after a break ends
uint8_t pomoCycleIndex = 0;            // 1-based current focus cycle within the session (0 = none active)
bool swRunning = false;
unsigned long swStartMs = 0, swAccumMs = 0;
bool alarmActive = false;
unsigned long alarmStartMs = 0;
char alarmMsg[24] = "";
uint8_t alarmNoteIdx = 0;
unsigned long alarmNoteStartMs = 0;

// RTC (optional — everything below stays safely inert if none is found)
bool rtcPresent = false;
uint8_t cachedRtcHour = 0, cachedRtcMin = 0, cachedRtcSec = 0;
uint8_t cachedRtcDate = 0, cachedRtcMonth = 0, cachedRtcYear = 0;
unsigned long lastRtcCheckMs = 0;
bool rtcAlarmEnabled = false;
uint8_t rtcAlarmHour = 0, rtcAlarmMin = 0;
uint8_t lastAlarmFiredMinute = 255; // sentinel so a fresh alarm can fire at minute 0
enum ClockSubMode { CLOCK_VIEW, CLOCK_SET_TIME, CLOCK_SET_ALARM, CLOCK_SET_DATE };
ClockSubMode clockSubMode = CLOCK_VIEW;
char clockDigits[7] = ""; // up to 6 digits (DDMMYY)
uint8_t clockDigitsLen = 0;

// Settings screen state (declared here, not next to its functions,
// because activateHomeItem() above references SETTINGS_VIEW directly)
enum SettingsSubMode { SETTINGS_VIEW, SETTINGS_EDIT_IDLE };
SettingsSubMode settingsSubMode = SETTINGS_VIEW;
int8_t settingsSel = 0; // 0 = idle timeout, 1 = buzzer
char settingsDigits[4] = "";
uint8_t settingsDigitsLen = 0;

// Vibration motor state — removed (motor not fitted; see the GPIO defines).

// Wi-Fi is OFF by default and only started on demand (power saving)
bool wifiEnabled = false;

// Runtime-editable settings (Settings screen). Session-only — reset to
// the #define defaults on reboot; not persisted to flash in this pass.
unsigned long idleTimeoutMs = IDLE_TIMEOUT_MS;
bool buzzerMuted = false;

// Screen-transition wipe: set whenever the home grid activates a
// feature screen; consumed by beginTransitionClip()/endTransitionClip().
unsigned long screenEnterMs = 0;
#define SCREEN_TRANSITION_MS 120

// Web import (file upload) state
bool importInProgress = false;
size_t importBytes = 0;
char importErr[64] = "";
File importFile;

// Home launcher grid (3x3, row-major). Index order matches both
// HOME_ICONS[] and activateHomeItem()'s switch.
const char* HOME_ITEMS[] = {
  "Timer", "Pomodoro", "Stopwatch",
  "Search", "Wi-Fi", "Clock",
  "Inventory", "Settings", "More"
};
const uint8_t HOME_ITEM_COUNT = 9;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
bool loadInventory();
bool tryLoadFrom(const char* path);
bool parseInventoryFromFile(File &f, InventoryItem* target, uint16_t &outCount, uint16_t &skippedOut, uint16_t &clampedOut);
bool isValidStatus(const char* s);
void reportDuplicates();

bool saveInventory();
void writeInventoryJson(File &f);
bool validateJsonFile(const char* path, uint16_t expectedCount);

void runSearch();
void adjustScroll();
void setError(const char* msg);
void triggerFlash(unsigned long ms);
int availableOf(const InventoryItem &it);
void handleKey(char k);
void handleSearchKey(char k);
void handleDetailKey(char k);
void handleQtyKey(char k, bool isLend);
void appendMultitapChar(char k);
void drawSearchScreen();
void drawItemDetail();
void drawQtyScreen(bool isLend);
void drawNoDataScreen();

uint32_t uptimeSeconds();
void formatHMS(char* buf, size_t n, uint32_t s);
void formatMMSS(char* buf, size_t n, uint32_t s);
void formatMMSSms(char* buf, size_t n, unsigned long totalMs);
void wakeDisplay();
void updateIdle();
void soundInit();
void startTone(uint16_t freq, uint16_t ms);
void updateTone();
void startTimer(uint8_t kind, unsigned long ms);
unsigned long timerRemainingMs();
void updateTimer();
void startAlarm(const char* msg);
void stopAlarm();
void dismissAlarm();
void updateAlarm();
void activateHomeItem(uint8_t idx);
void handleHomeKey(char k);
void handleTimerKey(char k);
void handleStopwatchKey(char k);
void handleSettingsKey(char k);
void handleMoreKey(char k);
void drawIdleClock();
void adjustMenuScroll();
void drawSelectionAnimation(int cx, int cy);
void drawHomeGrid();
void drawProgressBar(int x, int y, int w, int h, float fraction);
void drawTimerScreen();
void drawPomodoroScreen();
void drawStopwatchScreen();
void drawAlarmAnimation();
void drawSettingsScreen();
void drawMoreScreen();

// Simple vector-drawn monochrome icons, one per home-grid slot (same
// order as HOME_ITEMS[]/HOME_ICONS[]).
void iconTimer(int cx, int cy);
void iconPomodoro(int cx, int cy);
void iconStopwatch(int cx, int cy);
void iconSearch(int cx, int cy);
void iconWifi(int cx, int cy);
void iconClock(int cx, int cy);
void iconInventory(int cx, int cy);
void iconSettings(int cx, int cy);
void iconMore(int cx, int cy);

bool detectRTC();
void scanRtcBus();
uint8_t bcdToDec(uint8_t b);
uint8_t decToBcd(uint8_t d);
bool readRtcTime(uint8_t &h, uint8_t &m, uint8_t &s);
bool readRtcFull(uint8_t &h, uint8_t &m, uint8_t &s, uint8_t &date, uint8_t &month, uint8_t &year);
bool writeRtcTime(uint8_t h, uint8_t m, uint8_t s);
bool writeRtcDate(uint8_t date, uint8_t month, uint8_t year);
void beginTransitionClip();
void endTransitionClip();
void updateRtcAlarm();
void formatClock(char* buf, size_t n, uint8_t h, uint8_t m, uint8_t s);
void formatRtcDate(char* buf, size_t n, uint8_t date, uint8_t month, uint8_t year);
void handleClockKey(char k);
void drawClockScreen();

void setupWebServer();
void startWiFi();
void stopWiFi();
void handleWifiKey(char k);
void drawWifiScreen();
String htmlHeader(const char* title);
String htmlFooter();
bool containsCaseInsensitive(const char* haystack, const char* needle);
void invalidatePhysicalSelectionIfNeeded();
bool addItem(const char* name, const char* location, long qty, const char* status, char* errOut, size_t errOutSize);
bool editItem(uint16_t idx, const char* name, const char* location, long qty, const char* status, char* errOut, size_t errOutSize);
bool deleteItem(uint16_t idx, char* errOut, size_t errOutSize);
void handleWebRoot();
void handleWebInventory();
void handleWebAddForm();
void handleWebAddSubmit();
void handleWebEditForm();
void handleWebEditSubmit();
void handleWebDeleteConfirm();
void handleWebDeleteSubmit();
void handleWebNotFound();
void appendEscaped(char* dest, size_t destSize, const char* src);
void appendJsonEscaped(char* dest, size_t destSize, const char* src);
bool importInventoryFile(const char* path, uint16_t &resultCount, uint16_t &skippedOut, uint16_t &clampedOut, char* errOut, size_t errOutSize);
void handleImportForm();
void handleImportUpload();
void handleImportSubmit();
void handleExport();
void handleRestoreBackup();
void handleRestoreSubmit();

// ============================================================
//  SETUP / LOOP
// ============================================================

// Human-readable name for the chip's reset cause. ESP_RST_BROWNOUT is the
// one that matters for this board's power-margin problem: it means the 3V3
// rail sagged below the brownout threshold, almost always under load.
static const char* resetReasonToString(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON (normal power-on)";
    case ESP_RST_EXT:      return "EXT (external reset pin)";
    case ESP_RST_SW:       return "SW (software reset)";
    case ESP_RST_PANIC:    return "PANIC (crash/abort)";
    case ESP_RST_INT_WDT:  return "INT_WDT (interrupts watchdog)";
    case ESP_RST_TASK_WDT: return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:      return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply sag under load)";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
  }
}

void setup() {
  // NOTE: the brownout detector is deliberately left ENABLED here.
  // An earlier revision disabled it (WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0))
  // to hide resets caused by the Wi-Fi radio's current spike on a marginal
  // supply. That was backwards: it didn't prevent the sag, it just turned a
  // clean, detectable reset into undefined mid-execution behaviour — so the
  // board appeared to restart at random rather than at the moment of the
  // spike. A clean brownout reset is a *symptom* worth having. Fix the supply
  // (better cable/adapter, or a bulk cap) rather than blinding the detector.
  Serial.begin(115200);
  delay(100);

  // Report WHY we booted. This is the single most useful line for diagnosing
  // power-related resets: esp_reset_reason() distinguishes a supply sag
  // (ESP_RST_BROWNOUT) from a watchdog timeout, a software panic, and a
  // normal power-on. If resets ever come back, open Serial at 115200 and read
  // this before changing any code.
  Serial.println();
  Serial.print("[boot] reset reason: ");
  Serial.println(resetReasonToString(esp_reset_reason()));

  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS mount failed");
  }

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  u8g2.setBusClock(400000);
  u8g2.begin();

  keypad.setDebounceTime(KEYPAD_DEBOUNCE_MS);

  loadInventory();
  if (!inventoryLoadFailed) reportDuplicates();

  setupWebServer();     // registers HTTP routes only — Wi-Fi stays OFF until requested
  soundInit();

  RTCWire.begin(RTC_SDA_PIN, RTC_SCL_PIN); // RTC gets its own bus, separate from the OLED
  rtcPresent = detectRTC();
  Serial.println(rtcPresent ? "RTC detected" : "No RTC detected (uptime-only mode)");
  if (!rtcPresent) scanRtcBus(); // help diagnose wiring if it's supposed to be there

  query[0] = 0;
  queryLen = 0;
  runSearch();

  u8g2.setContrast(CONTRAST_FULL); // explicit, so a soft reset can't leave it dim

  lastActivityMs = millis();
  state = STATE_IDLE;   // boot straight into the uptime screen
}

void loop() {
  updateTone();
  updateAlarm();
  updateTimer();
  updateRtcAlarm();

  if (wifiEnabled) {
    dnsServer.processNextRequest(); // non-blocking, returns immediately if idle
    httpServer.handleClient();      // non-blocking, returns immediately if idle
  }

  if (inventoryLoadFailed) {
    drawNoDataScreen();
    return; // no usable data — don't process keys into a broken state
  }

  char k = keypad.getKey();
  if (k != NO_KEY) {
    lastActivityMs = millis();
    if (alarmActive) {
      dismissAlarm();                       // any key silences the alarm
    } else if (displayDim || state == STATE_IDLE) {
      wakeDisplay();                        // wake key is swallowed
      if (timerKind != TIMER_NONE) {
        state = STATE_TIMER;                // a countdown/Pomodoro is still running
      } else if (swRunning) {
        state = STATE_STOPWATCH;            // stopwatch kept counting underneath
      } else {
        state = STATE_HOME;
        menuSel = 0;
        menuScroll = 0;
      }
    } else {
      handleKey(k);
    }
  }

  updateIdle();

  switch (state) {
    case STATE_SEARCH:       drawSearchScreen();      break;
    case STATE_ITEM_DETAIL:  drawItemDetail();        break;
    case STATE_LEND_QTY:     drawQtyScreen(true);     break;
    case STATE_RETURN_QTY:   drawQtyScreen(false);    break;
    case STATE_IDLE:         drawIdleClock();         break;
    case STATE_HOME:         drawHomeGrid();          break;
    case STATE_TIMER:
      if (timerKind == TIMER_POMO_WORK || timerKind == TIMER_POMO_BREAK) drawPomodoroScreen();
      else drawTimerScreen();
      break;
    case STATE_STOPWATCH:    drawStopwatchScreen();   break;
    case STATE_ALARM:        drawAlarmAnimation();    break;
    case STATE_WIFI:         drawWifiScreen();        break;
    case STATE_CLOCK:        drawClockScreen();       break;
    case STATE_SETTINGS:     drawSettingsScreen();    break;
    case STATE_MORE:         drawMoreScreen();        break;
  }
}

// ============================================================
//  STORAGE
// ============================================================

// ---- Validation helpers ----
bool isValidStatus(const char* s) {
  for (uint8_t i = 0; i < VALID_STATUS_COUNT; i++) {
    if (strcasecmp(s, VALID_STATUSES[i]) == 0) return true;
  }
  return false;
}

// Parses a JSON array of inventory records from an open file into
// `target` (any InventoryItem buffer of at least MAX_RECORDS capacity —
// boot/reload pass the global items[], import passes a scratch buffer
// so the live inventory isn't touched until the caller is ready to
// commit). Does not close the file. Returns false only on structural
// JSON errors; individual bad records are skipped/clamped instead, and
// those counts are reported back via skippedOut/clampedOut so a caller
// (e.g. the web import page) can show them to the user.
bool parseInventoryFromFile(File &f, InventoryItem* target, uint16_t &outCount,
                             uint16_t &skippedOut, uint16_t &clampedOut) {
  JsonDocument doc; // ArduinoJson v7 — heap-allocated, sized automatically
  DeserializationError err = deserializeJson(doc, f);
  if (err) {
    Serial.print("ERROR: JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }
  if (!doc.is<JsonArray>()) {
    Serial.println("ERROR: inventory JSON root is not an array");
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  outCount = 0;
  skippedOut = 0;
  clampedOut = 0;

  for (JsonObject obj : arr) {
    if (outCount >= MAX_RECORDS) {
      Serial.println("WARNING: MAX_RECORDS reached, remaining records skipped");
      break;
    }

    const char* nameVal = obj["name"] | "";
    if (strlen(nameVal) == 0) {
      skippedOut++;
      continue; // refuse nameless records — nothing useful to search/show
    }

    InventoryItem tmp;
    strlcpy(tmp.name, nameVal, NAME_MAX_LEN);
    strlcpy(tmp.location, obj["location"] | "", LOC_MAX_LEN);

    const char* statusVal = obj["status"] | "working";
    if (isValidStatus(statusVal)) {
      strlcpy(tmp.status, statusVal, STATUS_MAX_LEN);
    } else {
      strlcpy(tmp.status, "working", STATUS_MAX_LEN);
      clampedOut++;
    }

    long q = obj["qty"]  | 0;
    long l = obj["lent"] | 0;
    if (q < 0) { q = 0; clampedOut++; }
    if (l < 0) { l = 0; clampedOut++; }
    if (l > q) { l = q; clampedOut++; } // corrupted lent > qty is impossible, clamp it

    tmp.qty  = (uint16_t)q;
    tmp.lent = (uint16_t)l;

    target[outCount++] = tmp;
  }

  if (skippedOut) Serial.printf("WARNING: skipped %u record(s) with empty name\n", skippedOut);
  if (clampedOut) Serial.printf("WARNING: clamped %u out-of-range field(s)\n", clampedOut);

  return true;
}

// Attempts to load and validate one specific file into items[]/itemCount.
bool tryLoadFrom(const char* path) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint16_t count = 0, skipped = 0, clamped = 0;
  bool ok = parseInventoryFromFile(f, items, count, skipped, clamped);
  f.close();

  if (!ok) return false;
  itemCount = count;
  Serial.printf("Loaded %u inventory record(s) from %s\n", itemCount, path);
  return true;
}

// Loads the primary inventory file, falling back to the backup if the
// primary is missing or corrupt. Sets inventoryLoadFailed if neither
// usable file exists, so the UI can show a clear recovery screen
// instead of silently running with zero records.
bool loadInventory() {
  if (tryLoadFrom(INVENTORY_FILE)) {
    inventoryLoadFailed = false;
    return true;
  }

  Serial.println("Primary inventory unreadable, attempting backup...");
  if (tryLoadFrom(INVENTORY_BAK_FILE)) {
    Serial.println("Recovered inventory from backup file.");
    inventoryLoadFailed = false;
    return true;
  }

  Serial.println("ERROR: no valid inventory file could be loaded (primary or backup).");
  itemCount = 0;
  inventoryLoadFailed = true;
  return false;
}

// Logs (but does not merge) exact name+location duplicates so real data
// issues are visible without silently discarding distinct physical entries.
void reportDuplicates() {
  uint16_t dupCount = 0;
  for (uint16_t i = 0; i < itemCount; i++) {
    for (uint16_t j = i + 1; j < itemCount; j++) {
      if (strcasecmp(items[i].name, items[j].name) == 0 &&
          strcasecmp(items[i].location, items[j].location) == 0) {
        Serial.printf("NOTICE: duplicate entry '%s' at %s (records %u & %u)\n",
                      items[i].name, items[i].location, i, j);
        dupCount++;
      }
    }
  }
  if (dupCount) {
    Serial.printf("Found %u exact duplicate record(s) (same name+location)\n", dupCount);
  }
}

// ---- Writing ----
void writeJsonString(File &f, const char* s) {
  f.print('"');
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') f.print('\\');
    f.print(*p);
  }
  f.print('"');
}

void writeInventoryJson(File &f) {
  f.print('[');
  for (uint16_t i = 0; i < itemCount; i++) {
    if (i > 0) f.print(',');
    f.print("{\"name\":");
    writeJsonString(f, items[i].name);
    f.print(",\"location\":");
    writeJsonString(f, items[i].location);
    f.print(",\"qty\":");
    f.print(items[i].qty);
    f.print(",\"status\":");
    writeJsonString(f, items[i].status);
    f.print(",\"lent\":");
    f.print(items[i].lent);
    f.print('}');
  }
  f.print(']');
}

// Re-parses a file (without touching the live items[] array) purely to
// confirm it's well-formed JSON with the expected record count, before
// we trust it enough to promote it to the primary file.
bool validateJsonFile(const char* path, uint16_t expectedCount) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err || !doc.is<JsonArray>()) return false;

  uint16_t n = 0;
  for (JsonObject obj : doc.as<JsonArray>()) { (void)obj; n++; }
  return n == expectedCount;
}

// Safe write sequence:
//   1. Write full inventory to a temp file.
//   2. Re-parse the temp file to confirm it's valid and complete.
//   3. Rotate the current primary file to .bak (best effort).
//   4. Atomically rename the temp file into place as the new primary.
// If any step fails, the previously-saved primary file is left untouched.
bool saveInventory() {
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  size_t freeBytes = (total > used) ? (total - used) : 0;
  if (freeBytes < MIN_FREE_BYTES_FOR_SAVE) {
    Serial.println("ERROR: low LittleFS free space, aborting save");
    return false;
  }

  File f = LittleFS.open(INVENTORY_TMP_FILE, "w");
  if (!f) {
    Serial.println("ERROR: cannot create temp file for save");
    return false;
  }
  writeInventoryJson(f);
  f.close();

  if (!validateJsonFile(INVENTORY_TMP_FILE, itemCount)) {
    Serial.println("ERROR: temp file failed post-write validation, save aborted");
    LittleFS.remove(INVENTORY_TMP_FILE);
    return false;
  }

  if (LittleFS.exists(INVENTORY_FILE)) {
    LittleFS.remove(INVENTORY_BAK_FILE);       // drop old backup, best effort
    LittleFS.rename(INVENTORY_FILE, INVENTORY_BAK_FILE);
  }

  if (!LittleFS.rename(INVENTORY_TMP_FILE, INVENTORY_FILE)) {
    Serial.println("ERROR: failed to finalize save (temp->primary rename)");
    return false;
  }

  Serial.println("Inventory saved successfully");
  return true;
}

// ============================================================
//  SEARCH
// ============================================================
// Search aliases: typing a KEY also pulls in items whose name contains
// the paired substring, in addition to normal prefix matching — lets
// "OLED" find "SH1106", "GPS" find "Neo 6M GPS module", etc. without
// touching the underlying inventory data. Grounded in this device's
// actual inventory categories; harmless (just matches nothing extra) if
// a given category isn't currently stocked.
struct SearchAlias { const char* key; const char* expandsTo; };
const SearchAlias SEARCH_ALIASES[] = {
  { "OLED",      "SH1106" },
  { "DISPLAY",   "SH1106" },
  { "DISPLAY",   "SEGMENT" },
  { "BT",        "BLUETOOTH" },
  { "BT",        "HC05" },
  { "BLE",       "BLUETOOTH" },
  { "WIFI",      "ESP" },
  { "INFRARED",  "IR" },
  { "MOTION",    "PIR" },
  { "LIGHT",     "LDR" },
  { "GSM",       "SIM" },
  { "GPS",       "NEO" },
  { "ULTRASONIC","HC-SR04" },
  { "DISTANCE",  "HC-SR04" },
  { "WIRELESS",  "NRF" },
  { "WIRELESS",  "RF" },
  { "AUDIO",     "SPEAKER" },
  { "AUDIO",     "MIC" },
  { "RTC",       "DS3231" },
};
const uint8_t SEARCH_ALIAS_COUNT = sizeof(SEARCH_ALIASES) / sizeof(SEARCH_ALIASES[0]);

void runSearch() {
  resultCount = 0;
  if (lentMode) {
    for (uint16_t i = 0; i < itemCount && resultCount < MAX_RECORDS; i++) {
      if (items[i].lent > 0) resultIndices[resultCount++] = i;
    }
  } else if (queryLen == 0) {
    selectedResult = 0;
    scrollOffset = 0;
    return;
  } else {
    for (uint16_t i = 0; i < itemCount && resultCount < MAX_RECORDS; i++) {
      if (strncasecmp(items[i].name, query, queryLen) == 0) {
        resultIndices[resultCount++] = i;
      }
    }

    // Alias expansion: if the typed query is a prefix of a known alias
    // key (so it kicks in progressively while multi-tap typing, same as
    // normal search), also pull in items containing that alias's
    // substring, skipping anything already found above.
    for (uint8_t a = 0; a < SEARCH_ALIAS_COUNT && resultCount < MAX_RECORDS; a++) {
      if (strncasecmp(SEARCH_ALIASES[a].key, query, queryLen) != 0) continue;
      for (uint16_t i = 0; i < itemCount && resultCount < MAX_RECORDS; i++) {
        if (!containsCaseInsensitive(items[i].name, SEARCH_ALIASES[a].expandsTo)) continue;
        bool dup = false;
        for (uint16_t r = 0; r < resultCount; r++) if (resultIndices[r] == i) { dup = true; break; }
        if (!dup) resultIndices[resultCount++] = i;
      }
    }
  }
  if (selectedResult >= (int16_t)resultCount) selectedResult = 0;
  if (selectedResult < 0) selectedResult = 0;
  scrollOffset = 0;
  adjustScroll(); // keep the highlighted row inside the visible window
}

void adjustScroll() {
  if (selectedResult < scrollOffset) scrollOffset = selectedResult;
  if (selectedResult >= scrollOffset + VISIBLE_RESULT_LINES)
    scrollOffset = selectedResult - VISIBLE_RESULT_LINES + 1;
}

// ============================================================
//  HELPERS
// ============================================================
int availableOf(const InventoryItem &it) {
  return (int)it.qty - (int)it.lent;
}

void setError(const char* msg) {
  strlcpy(errorMsg, msg, sizeof(errorMsg));
  errorMsgUntil = millis() + ERROR_MSG_DURATION_MS;
  triggerFlash(120); // on-screen flash is the error feedback; buzzer stays reserved for alarms
}

// A single quick full-screen invert, for "event" level feedback
// (successful lend/return, errors) — the lightest tier of animation
// per the Stage 6 philosophy: Idle (none) < Navigation (small focus
// animation) < Events (this, and the alarm's repeating version).
void triggerFlash(unsigned long ms) {
  flashUntil = millis() + ms;
}

// startVibration()/updateVibration() removed along with the motor.

// ============================================================
//  KEY HANDLING
// ============================================================
void handleKey(char k) {
  switch (state) {
    case STATE_SEARCH:      handleSearchKey(k);        break;
    case STATE_ITEM_DETAIL: handleDetailKey(k);        break;
    case STATE_LEND_QTY:    handleQtyKey(k, true);     break;
    case STATE_RETURN_QTY:  handleQtyKey(k, false);    break;
    case STATE_HOME:        handleHomeKey(k);          break;
    case STATE_TIMER:       handleTimerKey(k);         break;
    case STATE_STOPWATCH:   handleStopwatchKey(k);     break;
    case STATE_WIFI:        handleWifiKey(k);          break;
    case STATE_CLOCK:       handleClockKey(k);         break;
    case STATE_SETTINGS:    handleSettingsKey(k);      break;
    case STATE_MORE:        handleMoreKey(k);          break;
    default: break; // IDLE / ALARM are handled in loop()
  }
}

void appendMultitapChar(char k) {
  unsigned long now = millis();
  const char* cycle = MULTITAP_MAP[k - '0'];
  uint8_t cycleLen = strlen(cycle);

  if (k == lastMultitapKey && (now - lastMultitapTime) < MULTITAP_TIMEOUT && queryLen > 0) {
    // Same key pressed again within timeout -> cycle last character
    multitapIndex = (multitapIndex + 1) % cycleLen;
    query[queryLen - 1] = cycle[multitapIndex];
  } else {
    // New character
    if (queryLen < QUERY_MAX_LEN - 1) {
      multitapIndex = 0;
      query[queryLen] = cycle[0];
      queryLen++;
      query[queryLen] = 0;
    }
  }
  lastMultitapKey = k;
  lastMultitapTime = now;
}

void handleSearchKey(char k) {
  if (k >= '0' && k <= '9') {
    if (lentMode) return;              // no text entry in the lent-items list
    appendMultitapChar(k);
    selectedResult = 0;                // new text -> highlight the first match
    runSearch();
  } else if (k == '*') {
    if (lentMode) {                    // leave lent list -> menu
      lentMode = false;
      state = STATE_HOME;
    } else if (queryLen == 0) {        // nothing to clear -> back to menu
      state = STATE_HOME;
    } else {
      queryLen = 0;
      query[0] = 0;
      lastMultitapKey = 0;
      runSearch();
    }
  } else if (k == 'C' || k == 'A') { // C (legacy) and A (Stage 6 "browse") both move up
    if (resultCount > 0) {
      selectedResult--;
      if (selectedResult < 0) selectedResult = resultCount - 1;
      adjustScroll();
    }
  } else if (k == 'D' || k == 'B') { // D (legacy) and B (Stage 6 "browse") both move down
    if (resultCount > 0) {
      selectedResult++;
      if (selectedResult >= (int16_t)resultCount) selectedResult = 0;
      adjustScroll();
    }
  } else if (k == '#') {
    if (resultCount > 0) {
      selectedItemIndex = resultIndices[selectedResult];
      errorMsg[0] = 0;
      state = STATE_ITEM_DETAIL;
    }
  }
}

void handleDetailKey(char k) {
  if (selectedItemIndex < 0) { state = STATE_SEARCH; return; }
  InventoryItem &it = items[selectedItemIndex];

  if (k == '*') {
    state = STATE_SEARCH;
    runSearch();                       // lent list may have changed
  } else if (k == 'A') {
    if (availableOf(it) > 0) {
      qtyLen = 0; qtyBuffer[0] = 0; errorMsg[0] = 0;
      state = STATE_LEND_QTY;
    } else {
      setError("None available");
    }
  } else if (k == 'B') {
    if (it.lent > 0) {
      qtyLen = 0; qtyBuffer[0] = 0; errorMsg[0] = 0;
      state = STATE_RETURN_QTY;
    } else {
      setError("Nothing lent out");
    }
  }
}

void handleQtyKey(char k, bool isLend) {
  InventoryItem &it = items[selectedItemIndex];

  if (k >= '0' && k <= '9') {
    errorMsg[0] = 0;
    if (qtyLen < QTY_BUF_LEN - 1) {
      qtyBuffer[qtyLen++] = k;
      qtyBuffer[qtyLen] = 0;
    }
  } else if (k == '*') {
    // Backspace one digit; only cancel the screen once buffer is empty.
    errorMsg[0] = 0;
    if (qtyLen > 0) {
      qtyLen--;
      qtyBuffer[qtyLen] = 0;
    } else {
      state = STATE_ITEM_DETAIL;
    }
  } else if (k == '#') {
    int qv = atoi(qtyBuffer);
    if (qv <= 0) {
      setError("Enter a quantity");
      return;
    }

    uint16_t oldLent = it.lent; // remember so we can roll back on save failure
    if (isLend) {
      int avail = availableOf(it);
      if (qv > avail) {
        setError("Exceeds available");
        return;
      }
      it.lent = oldLent + qv;
    } else {
      if (qv > (int)oldLent) {
        setError("Exceeds lent qty");
        return;
      }
      it.lent = oldLent - qv;
    }

    if (!saveInventory()) {
      it.lent = oldLent; // keep RAM consistent with what's actually on flash
      setError("Save failed, reverted");
      return;
    }
    if (isLend) sessionLendUnits += qv; else sessionReturnUnits += qv;
    triggerFlash(100); // confirm feedback; buzzer stays reserved for alarms
    state = STATE_ITEM_DETAIL;
  }
}

// ============================================================
//  RENDERING
// ============================================================
// Terminal-style search screen: a title line, the typed query as its
// own "> query_" prompt line, then compact result rows with the
// available quantity right-aligned, and a fixed hint line at the
// bottom. VISIBLE_RESULT_LINES (3) is sized to fit this layout exactly.
void drawSearchScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);

  u8g2.drawStr(0, 8, lentMode ? "LENT OUT" : "SEARCH");

  if (!lentMode) {
    u8g2.drawStr(0, 19, ">");
    u8g2.drawStr(8, 19, query);
    if ((millis() / 500) % 2 == 0) {
      int w = u8g2.getStrWidth(query);
      u8g2.drawStr(8 + w, 19, "_");
    }
    u8g2.drawHLine(0, 21, 128);
  } else {
    u8g2.drawHLine(0, 10, 128);
  }

  int resultsTop = lentMode ? 22 : 32;

  if (lentMode && resultCount == 0) {
    u8g2.drawStr(0, resultsTop, "Nothing lent out");
  } else if (!lentMode && queryLen == 0) {
    u8g2.drawStr(0, resultsTop, "Type to search...");
  } else if (resultCount == 0) {
    u8g2.drawStr(0, resultsTop, "No matches");
  } else {
    int y = resultsTop;
    for (int i = 0; i < VISIBLE_RESULT_LINES && (scrollOffset + i) < (int16_t)resultCount; i++) {
      int ridx = scrollOffset + i;
      InventoryItem &it = items[resultIndices[ridx]];

      char nameBuf[18];
      char prefix = (ridx == selectedResult) ? '>' : ' ';
      snprintf(nameBuf, sizeof(nameBuf), "%c%.14s", prefix, it.name);
      u8g2.drawStr(0, y, nameBuf);

      char qtyBuf[6];
      snprintf(qtyBuf, sizeof(qtyBuf), "%d", availableOf(it));
      int qw = u8g2.getStrWidth(qtyBuf);
      u8g2.drawStr(128 - qw, y, qtyBuf);

      y += 10;
    }
  }

  u8g2.drawHLine(0, 53, 128);
  u8g2.drawStr(0, 63, "A/B browse  #:select");
  endTransitionClip();
  u8g2.sendBuffer();
}

void drawItemDetail() {
  if (selectedItemIndex < 0) return;
  InventoryItem &it = items[selectedItemIndex];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  char nameLine[22];
  snprintf(nameLine, sizeof(nameLine), "%.21s", it.name);
  u8g2.drawStr(0, 9, nameLine);
  u8g2.drawHLine(0, 11, 128);

  // Prominent location
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(it.location);
  u8g2.drawStr((128 - w) / 2, 34, it.location);

  u8g2.setFont(u8g2_font_6x10_tf);
  char combo[24];
  snprintf(combo, sizeof(combo), "Avail:%d  Lent:%d", availableOf(it), it.lent);
  u8g2.drawStr(0, 46, combo);

  if (errorMsg[0] && millis() < errorMsgUntil) {
    u8g2.drawStr(0, 60, errorMsg);
  } else {
    u8g2.drawStr(0, 60, "A:Lend B:Return *:Back");
  }

  if (millis() < flashUntil) {
    u8g2.setDrawColor(2);
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

void drawQtyScreen(bool isLend) {
  InventoryItem &it = items[selectedItemIndex];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  char nameLine[22];
  snprintf(nameLine, sizeof(nameLine), "%.21s", it.name);
  u8g2.drawStr(0, 9, nameLine);
  u8g2.drawHLine(0, 11, 128);

  u8g2.drawStr(0, 24, isLend ? "LEND" : "RETURN");

  char limLine[24];
  if (isLend) snprintf(limLine, sizeof(limLine), "Available: %d", availableOf(it));
  else        snprintf(limLine, sizeof(limLine), "Lent: %d", it.lent);
  u8g2.drawStr(0, 36, limLine);

  char qline[20];
  snprintf(qline, sizeof(qline), "Quantity: %s", qtyBuffer);
  u8g2.drawStr(0, 48, qline);

  if (errorMsg[0] && millis() < errorMsgUntil) {
    u8g2.drawStr(0, 60, errorMsg);
  } else {
    u8g2.drawStr(0, 60, "#:OK *:Del/Back");
  }

  if (millis() < flashUntil) {
    u8g2.setDrawColor(2);
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

void drawNoDataScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "No usable inventory");
  u8g2.drawStr(0, 26, "Primary + backup");
  u8g2.drawStr(0, 38, "files unreadable.");
  u8g2.drawStr(0, 52, "Check Serial Monitor,");
  u8g2.drawStr(0, 63, "re-upload data, reset.");
  u8g2.sendBuffer();
}

// ============================================================
//  SESSION / IDLE / TOOLS
//  (uptime/RTC idle screen, menu, timer, pomodoro, stopwatch, alarm,
//   on-demand Wi-Fi, optional RTC clock/alarm, speaker)
// ============================================================

// A short original jingle (not a reproduction of any specific song) used
// as the alarm "ringtone" — played on loop until dismissed. freq=0 is a
// rest (silence).
struct AlarmNote { uint16_t freq; uint16_t durMs; };
const AlarmNote ALARM_MELODY[] = {
  {880, 120}, {988, 120}, {1047, 120}, {1175, 120},
  {1319, 220}, {0, 70},
  {1175, 120}, {1047, 120}, {988, 120}, {880, 220},
  {0, 300}
};
const uint8_t ALARM_MELODY_LEN = sizeof(ALARM_MELODY) / sizeof(ALARM_MELODY[0]);

uint32_t uptimeSeconds() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

void formatHMS(char* buf, size_t n, uint32_t s) {
  uint32_t h = s / 3600;
  uint32_t m = (s % 3600) / 60;
  uint32_t sec = s % 60;
  snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)sec);
}

void formatMMSS(char* buf, size_t n, uint32_t s) {
  uint32_t m = s / 60;
  uint32_t sec = s % 60;
  snprintf(buf, n, "%02lu:%02lu", (unsigned long)m, (unsigned long)sec);
}

void formatMMSSms(char* buf, size_t n, unsigned long totalMs) {
  unsigned long ms = totalMs % 1000;
  unsigned long totalSec = totalMs / 1000;
  unsigned long m = totalSec / 60;
  unsigned long sec = totalSec % 60;
  snprintf(buf, n, "%02lu:%02lu.%03lu", m, sec, ms);
}

// ---- Display power ----
// Restores full brightness. Called on any keypress and whenever something
// important (alarm, running timer) becomes active.
void wakeDisplay() {
  if (displayDim) {
    displayDim = false;
    u8g2.setContrast(CONTRAST_FULL);
  }
}

void updateIdle() {
  unsigned long now = millis();
  unsigned long idleFor = now - lastActivityMs;

  // Drop back to the uptime screen after inactivity — but never while a
  // timer/stopwatch is actively being watched, and never during an alarm.
  if (!alarmActive && state != STATE_IDLE && state != STATE_TIMER && state != STATE_STOPWATCH) {
    if (idleFor >= idleTimeoutMs) {
      state = STATE_IDLE;
    }
  }

  // Dim after a long idle period, but NEVER blank the panel. Blanking is
  // what made a running countdown look frozen: the timer kept running but
  // the screen was dark, so it read as a hang. Dimming also keeps the
  // device visibly powered on, which matters because the brownout-prone
  // supply can't be distinguished from a dead unit otherwise.
  //
  // Anything that represents an in-progress event keeps full brightness:
  // an active alarm, a running timer/Pomodoro, or a running stopwatch.
  bool eventActive = alarmActive || (timerKind != TIMER_NONE) || swRunning;
  if (eventActive) {
    wakeDisplay();          // never dim while an event is live
    return;
  }

  if (DISPLAY_DIM_TIMEOUT_MS > 0) {
    if (idleFor >= DISPLAY_DIM_TIMEOUT_MS) {
      if (!displayDim) {
        displayDim = true;
        u8g2.setContrast(CONTRAST_DIM);
      }
    }
  }
}

// ---- Speaker (short beeps only; the alarm pattern in updateAlarm()
//      drives the speaker directly rather than through startTone()) ----
void soundInit() {
#if SOUND_ENABLED
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
#endif
}

void startTone(uint16_t freq, uint16_t ms) {
#if SOUND_ENABLED
  tone(SPEAKER_PIN, freq, ms); // ESP32 core 3.x tone(): non-blocking, self-stopping
#endif
}

void updateTone() {
  // Intentionally empty: tone(pin, freq, duration) on ESP32 core 3.x
  // stops itself asynchronously via the LEDC peripheral. Kept as a
  // named hook so alarm/timer code has one obvious place to extend.
}

// ---- Countdown / Pomodoro timer ----
void startTimer(uint8_t kind, unsigned long ms) {
  timerKind = kind;
  timerStartMs = millis();
  timerDurationMs = ms;
  state = STATE_TIMER;
}

unsigned long timerRemainingMs() {
  unsigned long elapsed = millis() - timerStartMs;
  if (elapsed >= timerDurationMs) return 0;
  return timerDurationMs - elapsed;
}

void updateTimer() {
  if (timerKind == TIMER_NONE) return;
  if (timerRemainingMs() > 0) return;

  if (timerKind == TIMER_POMO_WORK) {
    timerKind = TIMER_NONE;
    pendingBreak = true;
    startAlarm("Work done! Break?");
  } else if (timerKind == TIMER_POMO_BREAK) {
    timerKind = TIMER_NONE;
    if (pomoCycleIndex < POMO_CYCLES_PER_SESSION) {
      pendingNextFocus = true;
      startAlarm("Break over!");
    } else {
      pomoCycleIndex = 0; // whole session complete
      startAlarm("Pomodoro done!");
    }
  } else {
    timerKind = TIMER_NONE;
    startAlarm("Timer done!");
  }
}

// ---- Alarm (speaker melody until any key is pressed) ----
// Defined just below startAlarm(), which calls it — forward-declared here.
void applyAlarmNote(const AlarmNote &n);

void startAlarm(const char* msg) {
  alarmActive = true;
  alarmStartMs = millis();
  strlcpy(alarmMsg, msg, sizeof(alarmMsg));
  state = STATE_ALARM;
  wakeDisplay();

  alarmNoteIdx = 0;
  alarmNoteStartMs = millis();
  applyAlarmNote(ALARM_MELODY[0]);
}

// Drives the speaker for a single melody note. freq == 0 means a rest, which
// silences the pin.
//
// All tone/noTone calls funnel through here for one reason: on ESP32 core 3.x
// both are asynchronous, queued to a dedicated tone task that owns the LEDC
// peripheral. Calling tone() again without first stopping the previous note
// leaves that note's hardware state running underneath the new one, which on
// a marginal supply showed up as an unreliable alarm. Every note transition
// now explicitly stops before it starts, so the peripheral is never asked to
// change frequency while a tone is live.
void applyAlarmNote(const AlarmNote &n) {
#if SOUND_ENABLED
  if (buzzerMuted) {
    noTone(SPEAKER_PIN); // never leave a tone running just because mute was toggled mid-alarm
    return;
  }
  noTone(SPEAKER_PIN); // stop the previous note first (see comment above)
  if (n.freq > 0) tone(SPEAKER_PIN, n.freq);
#endif
}

void stopAlarm() {
  alarmActive = false;
#if SOUND_ENABLED
  noTone(SPEAKER_PIN);
  digitalWrite(SPEAKER_PIN, LOW);
#endif
}

void dismissAlarm() {
  stopAlarm();
  if (pendingBreak) {
    pendingBreak = false;
    startTimer(TIMER_POMO_BREAK, (unsigned long)POMO_BREAK_MIN * 60000UL);
  } else if (pendingNextFocus) {
    pendingNextFocus = false;
    pomoCycleIndex++;
    startTimer(TIMER_POMO_WORK, (unsigned long)POMO_WORK_MIN * 60000UL);
  } else {
    state = STATE_HOME;
    menuSel = 0;
    menuScroll = 0;
  }
}

void updateAlarm() {
  if (!alarmActive) return;
  unsigned long now = millis();

  if (now - alarmStartMs >= ALARM_MAX_MS) { // safety cap: never buzzes forever unattended
    dismissAlarm();
    return;
  }

  // Step through the melody — each note plays for its own duration, then we
  // advance (looping) to the next.
  if (now - alarmNoteStartMs >= ALARM_MELODY[alarmNoteIdx].durMs) {
    alarmNoteIdx = (alarmNoteIdx + 1) % ALARM_MELODY_LEN;
    alarmNoteStartMs = now;
    applyAlarmNote(ALARM_MELODY[alarmNoteIdx]);
  }
}

// ---- Optional RTC (DS3231/DS1307-compatible) ----
// Everything here is defensive: if no RTC chip answers on the bus,
// rtcPresent stays false and every caller already checks that flag,
// so the rest of the firmware behaves exactly as if this code didn't
// exist. A wire coming loose mid-session degrades the same way rather
// than crashing.
uint8_t bcdToDec(uint8_t b) { return (b / 16) * 10 + (b % 16); }
uint8_t decToBcd(uint8_t d) { return ((d / 10) << 4) + (d % 10); }

bool detectRTC() {
  RTCWire.beginTransmission(RTC_I2C_ADDR);
  return (RTCWire.endTransmission() == 0);
}

// Prints every address that answers on the RTC's dedicated bus — run
// automatically at boot if no RTC was found, purely to help diagnose a
// wiring/address problem from the Serial Monitor.
void scanRtcBus() {
  Serial.println("Scanning RTC I2C bus (SDA=" + String(RTC_SDA_PIN) + ", SCL=" + String(RTC_SCL_PIN) + ")...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    RTCWire.beginTransmission(addr);
    if (RTCWire.endTransmission() == 0) {
      Serial.printf("  Device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("  No I2C devices found on this bus — check wiring and power.");
}

bool readRtcTime(uint8_t &h, uint8_t &m, uint8_t &s) {
  RTCWire.beginTransmission(RTC_I2C_ADDR);
  RTCWire.write((uint8_t)0x00);
  if (RTCWire.endTransmission() != 0) return false;
  if (RTCWire.requestFrom((int)RTC_I2C_ADDR, 3) != 3) return false;

  uint8_t rs = RTCWire.read();
  uint8_t rm = RTCWire.read();
  uint8_t rh = RTCWire.read();
  s = bcdToDec(rs & 0x7F);
  m = bcdToDec(rm & 0x7F);
  h = bcdToDec(rh & 0x3F); // strip 12h/PM bits — we always write 24h mode
  return true;
}

// Reads time AND date/month/year (DS3231 registers 0x00-0x06) in one
// transaction — used by the idle clock screen. Register 3 (day-of-week)
// is read but discarded; we derive nothing from it.
bool readRtcFull(uint8_t &h, uint8_t &m, uint8_t &s, uint8_t &date, uint8_t &month, uint8_t &year) {
  RTCWire.beginTransmission(RTC_I2C_ADDR);
  RTCWire.write((uint8_t)0x00);
  if (RTCWire.endTransmission() != 0) return false;
  if (RTCWire.requestFrom((int)RTC_I2C_ADDR, 7) != 7) return false;

  uint8_t rs = RTCWire.read();
  uint8_t rm = RTCWire.read();
  uint8_t rh = RTCWire.read();
  RTCWire.read(); // day-of-week, unused
  uint8_t rdate = RTCWire.read();
  uint8_t rmonth = RTCWire.read();
  uint8_t ryear = RTCWire.read();

  s = bcdToDec(rs & 0x7F);
  m = bcdToDec(rm & 0x7F);
  h = bcdToDec(rh & 0x3F);
  date = bcdToDec(rdate & 0x3F);
  month = bcdToDec(rmonth & 0x1F); // bit7 is the century flag, ignored (we assume 20xx)
  year = bcdToDec(ryear);
  return true;
}

bool writeRtcTime(uint8_t h, uint8_t m, uint8_t s) {
  RTCWire.beginTransmission(RTC_I2C_ADDR);
  RTCWire.write((uint8_t)0x00);
  RTCWire.write(decToBcd(s));
  RTCWire.write(decToBcd(m));
  RTCWire.write(decToBcd(h) & 0x3F); // bit6=0 forces 24-hour mode on DS3231
  return (RTCWire.endTransmission() == 0);
}

// Writes just the date/month/year registers (0x04-0x06), leaving the
// time and day-of-week registers untouched.
bool writeRtcDate(uint8_t date, uint8_t month, uint8_t year) {
  RTCWire.beginTransmission(RTC_I2C_ADDR);
  RTCWire.write((uint8_t)0x04);
  RTCWire.write(decToBcd(date));
  RTCWire.write(decToBcd(month) & 0x1F); // bit7 century flag left 0 (assume 20xx)
  RTCWire.write(decToBcd(year));
  return (RTCWire.endTransmission() == 0);
}

void formatClock(char* buf, size_t n, uint8_t h, uint8_t m, uint8_t s) {
  snprintf(buf, n, "%02u:%02u:%02u", h, m, s);
}

const char* RTC_MONTH_NAMES[] = { "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC" };

void formatRtcDate(char* buf, size_t n, uint8_t date, uint8_t month, uint8_t year) {
  const char* mn = (month >= 1 && month <= 12) ? RTC_MONTH_NAMES[month - 1] : "???";
  snprintf(buf, n, "%02u %s 20%02u", date, mn, year);
}

// Throttled to ~1/sec: refreshes the cached time used for display and
// checks the one-shot HH:MM alarm, without hammering the I2C bus.
void updateRtcAlarm() {
  if (!rtcPresent || alarmActive) return;
  unsigned long now = millis();
  if (now - lastRtcCheckMs < 900) return;
  lastRtcCheckMs = now;

  uint8_t h, m, s, dd, mo, yy;
  if (!readRtcFull(h, m, s, dd, mo, yy)) {
    rtcPresent = false; // lost comms mid-session — degrade gracefully, don't crash
    return;
  }
  cachedRtcHour = h; cachedRtcMin = m; cachedRtcSec = s;
  cachedRtcDate = dd; cachedRtcMonth = mo; cachedRtcYear = yy;

  if (rtcAlarmEnabled && h == rtcAlarmHour && m == rtcAlarmMin && lastAlarmFiredMinute != m) {
    lastAlarmFiredMinute = m;
    rtcAlarmEnabled = false; // one-shot alarm, not a daily recurring one
    startAlarm("Alarm!");
  }
}

void handleClockKey(char k) {
  if (!rtcPresent) {
    if (k == '*') state = STATE_HOME; // nothing else to do without an RTC — no crash, just back out
    return;
  }

  if (clockSubMode == CLOCK_VIEW) {
    if (k == 'A') { clockSubMode = CLOCK_SET_TIME; clockDigitsLen = 0; clockDigits[0] = 0; }
    else if (k == 'B') { clockSubMode = CLOCK_SET_ALARM; clockDigitsLen = 0; clockDigits[0] = 0; }
    else if (k == 'C') { clockSubMode = CLOCK_SET_DATE; clockDigitsLen = 0; clockDigits[0] = 0; }
    else if (k == 'D') { rtcAlarmEnabled = !rtcAlarmEnabled; if (rtcAlarmEnabled) lastAlarmFiredMinute = 255; }
    else if (k == '*') { state = STATE_HOME; }
    return;
  }

  // CLOCK_SET_TIME / CLOCK_SET_ALARM take 4 digits (HHMM); CLOCK_SET_DATE takes 6 (DDMMYY).
  uint8_t maxDigits = (clockSubMode == CLOCK_SET_DATE) ? 6 : 4;

  if (k >= '0' && k <= '9') {
    if (clockDigitsLen < maxDigits) { clockDigits[clockDigitsLen++] = k; clockDigits[clockDigitsLen] = 0; }
  } else if (k == '*') {
    if (clockDigitsLen > 0) { clockDigitsLen--; clockDigits[clockDigitsLen] = 0; }
    else clockSubMode = CLOCK_VIEW;
  } else if (k == '#') {
    if (clockDigitsLen != maxDigits) {
      setError(clockSubMode == CLOCK_SET_DATE ? "Enter DDMMYY" : "Enter HHMM");
      return;
    }

    if (clockSubMode == CLOCK_SET_DATE) {
      int dd = (clockDigits[0] - '0') * 10 + (clockDigits[1] - '0');
      int mo = (clockDigits[2] - '0') * 10 + (clockDigits[3] - '0');
      int yy = (clockDigits[4] - '0') * 10 + (clockDigits[5] - '0');
      if (dd < 1 || dd > 31 || mo < 1 || mo > 12) { setError("Invalid date"); return; }
      if (!writeRtcDate((uint8_t)dd, (uint8_t)mo, (uint8_t)yy)) { setError("RTC write failed"); return; }
      triggerFlash(100);
      clockSubMode = CLOCK_VIEW;
      return;
    }

    int hh = (clockDigits[0] - '0') * 10 + (clockDigits[1] - '0');
    int mm = (clockDigits[2] - '0') * 10 + (clockDigits[3] - '0');
    if (hh > 23 || mm > 59) { setError("Invalid time"); return; }

    if (clockSubMode == CLOCK_SET_TIME) {
      if (!writeRtcTime((uint8_t)hh, (uint8_t)mm, 0)) { setError("RTC write failed"); return; }
    } else { // CLOCK_SET_ALARM
      rtcAlarmHour = (uint8_t)hh;
      rtcAlarmMin = (uint8_t)mm;
      rtcAlarmEnabled = true;
      lastAlarmFiredMinute = 255;
    }
    triggerFlash(100);
    clockSubMode = CLOCK_VIEW;
  }
}

void drawClockScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "CLOCK");
  u8g2.drawHLine(0, 11, 128);

  if (!rtcPresent) {
    u8g2.drawStr(0, 28, "No RTC found");
    u8g2.drawStr(0, 40, "(uptime-only mode)");
    u8g2.drawStr(0, 60, "*:Back");
    endTransitionClip();
    u8g2.sendBuffer();
    return;
  }

  if (clockSubMode == CLOCK_VIEW) {
    char t[10];
    formatClock(t, sizeof(t), cachedRtcHour, cachedRtcMin, cachedRtcSec);
    u8g2.setFont(u8g2_font_logisoso16_tr);
    int w = u8g2.getStrWidth(t);
    u8g2.drawStr((128 - w) / 2, 30, t);
    u8g2.setFont(u8g2_font_6x10_tf);

    char dateLine[16];
    formatRtcDate(dateLine, sizeof(dateLine), cachedRtcDate, cachedRtcMonth, cachedRtcYear);
    int dw = u8g2.getStrWidth(dateLine);
    u8g2.drawStr((128 - dw) / 2, 41, dateLine);

    char alarmLine[24];
    if (rtcAlarmEnabled) snprintf(alarmLine, sizeof(alarmLine), "Alarm %02u:%02u ON", rtcAlarmHour, rtcAlarmMin);
    else snprintf(alarmLine, sizeof(alarmLine), "Alarm: OFF");
    u8g2.drawStr(0, 52, alarmLine);

    if (errorMsg[0] && millis() < errorMsgUntil) u8g2.drawStr(0, 63, errorMsg);
    else u8g2.drawStr(0, 63, "A/B/C:Set D:Tgl");
  } else {
    const char* label = (clockSubMode == CLOCK_SET_TIME)  ? "Set time (HHMM)" :
                         (clockSubMode == CLOCK_SET_ALARM) ? "Set alarm (HHMM)" : "Set date (DDMMYY)";
    u8g2.drawStr(0, 24, label);
    char line[20]; snprintf(line, sizeof(line), "%s", clockDigits);
    u8g2.drawStr(0, 40, line);
    if (errorMsg[0] && millis() < errorMsgUntil) u8g2.drawStr(0, 60, errorMsg);
    else u8g2.drawStr(0, 60, "#:OK *:Del/Back");
  }
  endTransitionClip();
  u8g2.sendBuffer();
}

// ---- Home launcher: 3x3 icon grid ----
// menuSel doubles as the grid cursor (0-8, row-major). Unlike the old
// scrolling list this is kept because all 9 items always fit one screen
// — no scrolling is needed, so adjustMenuScroll() has no caller anymore
// (left defined, harmless, in case a future longer list wants it back).
void adjustMenuScroll() {
  if (menuSel < menuScroll) menuScroll = menuSel;
  if (menuSel >= menuScroll + VISIBLE_RESULT_LINES) menuScroll = menuSel - VISIBLE_RESULT_LINES + 1;
}

// ---- Icons: simple vector-drawn monochrome glyphs, ~12-14px, one per
// home-grid slot. Deliberately geometric/outline rather than filled
// blobs, to stay crisp at this size and match the rest of the UI's line
// language. Order matches HOME_ITEMS[] / HOME_ICONS[].
void iconTimer(int cx, int cy) {
  u8g2.drawTriangle(cx - 5, cy - 5, cx + 5, cy - 5, cx, cy);
  u8g2.drawTriangle(cx - 5, cy + 5, cx + 5, cy + 5, cx, cy);
}
void iconPomodoro(int cx, int cy) {
  u8g2.drawCircle(cx, cy + 1, 5);
  u8g2.drawLine(cx, cy - 4, cx + 3, cy - 7);
}
void iconStopwatch(int cx, int cy) {
  u8g2.drawCircle(cx, cy, 5);
  u8g2.drawBox(cx - 2, cy - 7, 4, 2);
  u8g2.drawLine(cx, cy, cx, cy - 3);
}
void iconSearch(int cx, int cy) {
  u8g2.drawCircle(cx - 1, cy - 1, 4);
  u8g2.drawLine(cx + 2, cy + 2, cx + 5, cy + 5);
}
void iconWifi(int cx, int cy) {
  u8g2.drawDisc(cx, cy + 5, 1);
  u8g2.drawCircle(cx, cy + 5, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(cx, cy + 5, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
}
void iconClock(int cx, int cy) {
  u8g2.drawCircle(cx, cy, 5);
  u8g2.drawLine(cx, cy, cx, cy - 3);
  u8g2.drawLine(cx, cy, cx + 3, cy);
}
void iconInventory(int cx, int cy) { // open-crate shape for the Inventory/lent-items slot
  u8g2.drawFrame(cx - 5, cy - 3, 10, 7);
  u8g2.drawLine(cx - 5, cy - 3, cx, cy - 6);
  u8g2.drawLine(cx + 5, cy - 3, cx, cy - 6);
}
void iconSettings(int cx, int cy) { // circle + 4 radiating ticks, approximating a gear
  u8g2.drawCircle(cx, cy, 3);
  u8g2.drawLine(cx, cy - 6, cx, cy - 4);
  u8g2.drawLine(cx, cy + 4, cx, cy + 6);
  u8g2.drawLine(cx - 6, cy, cx - 4, cy);
  u8g2.drawLine(cx + 4, cy, cx + 6, cy);
}
void iconMore(int cx, int cy) {
  u8g2.drawDisc(cx - 5, cy, 1);
  u8g2.drawDisc(cx, cy, 1);
  u8g2.drawDisc(cx + 5, cy, 1);
}

typedef void (*IconDrawFn)(int cx, int cy);
IconDrawFn HOME_ICONS[] = {
  iconTimer, iconPomodoro, iconStopwatch,
  iconSearch, iconWifi, iconClock,
  iconInventory, iconSettings, iconMore
};

const int HOME_COL_X[3] = { 21, 64, 107 };
const int HOME_ROW_Y[3] = { 11, 30, 48 };

// Fast (~150ms), lightweight "selected" indicator: four corner brackets
// that nudge in and out on a short cycle. Cheap to draw (8 short lines,
// no fill) and reads clearly as focus without a heavy redraw.
void drawSelectionAnimation(int cx, int cy) {
  bool expanded = (millis() / 150) % 2;
  int half = expanded ? 11 : 9;
  int x0 = cx - half, x1 = cx + half;
  int y0 = cy - half, y1 = cy + half;
  u8g2.drawLine(x0, y0, x0 + 3, y0);
  u8g2.drawLine(x0, y0, x0, y0 + 3);
  u8g2.drawLine(x1 - 3, y0, x1, y0);
  u8g2.drawLine(x1, y0, x1, y0 + 3);
  u8g2.drawLine(x0, y1 - 3, x0, y1);
  u8g2.drawLine(x0, y1, x0 + 3, y1);
  u8g2.drawLine(x1 - 3, y1, x1, y1);
  u8g2.drawLine(x1, y1 - 3, x1, y1);
}

// Navigation-tier transition: a brief (SCREEN_TRANSITION_MS) left-to-
// right wipe-in when a feature screen is first entered from the home
// grid, via u8g2's clip window — genuinely non-blocking, just narrows
// what the next few frames are allowed to draw into. Call right after
// clearBuffer() and pair with endTransitionClip() right before
// sendBuffer(), or the clip stays narrowed for later screens too.
void beginTransitionClip() {
  unsigned long elapsed = millis() - screenEnterMs;
  if (elapsed >= SCREEN_TRANSITION_MS) { u8g2.setMaxClipWindow(); return; }
  int w = (int)(128UL * elapsed / SCREEN_TRANSITION_MS);
  if (w < 1) w = 1;
  u8g2.setClipWindow(0, 0, w, 64);
}

void endTransitionClip() {
  u8g2.setMaxClipWindow();
}

void drawHomeGrid() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  for (uint8_t i = 0; i < HOME_ITEM_COUNT; i++) {
    int row = i / 3, col = i % 3;
    int cx = HOME_COL_X[col], cy = HOME_ROW_Y[row];
    HOME_ICONS[i](cx, cy);
    if (i == menuSel) drawSelectionAnimation(cx, cy);
  }

  const char* label = HOME_ITEMS[menuSel];
  int w = u8g2.getStrWidth(label);
  u8g2.drawStr((128 - w) / 2, 63, label);
  u8g2.sendBuffer();
}

void activateHomeItem(uint8_t idx) {
  switch (idx) {
    case 0: // Timer
      timerKind = TIMER_NONE;
      timerInputLen = 0;
      timerInput[0] = 0;
      state = STATE_TIMER;
      break;
    case 1: // Pomodoro
      pomoCycleIndex = 1;
      startTimer(TIMER_POMO_WORK, (unsigned long)POMO_WORK_MIN * 60000UL);
      break;
    case 2: // Stopwatch
      swRunning = false;
      swAccumMs = 0;
      state = STATE_STOPWATCH;
      break;
    case 3: // Search
      lentMode = false;
      state = STATE_SEARCH;
      runSearch();
      break;
    case 4: // Wi-Fi
      state = STATE_WIFI;
      break;
    case 5: // Clock
      clockSubMode = CLOCK_VIEW;
      clockDigitsLen = 0;
      clockDigits[0] = 0;
      state = STATE_CLOCK;
      break;
    case 6: // Inventory (lent-items view — reuses the same search screen/logic)
      lentMode = true;
      state = STATE_SEARCH;
      runSearch();
      break;
    case 7: // Settings
      settingsSubMode = SETTINGS_VIEW;
      settingsSel = 0;
      state = STATE_SETTINGS;
      break;
    case 8: // More (placeholder slot)
      state = STATE_MORE;
      break;
    default: break;
  }
  screenEnterMs = millis(); // drives the brief wipe-in transition on the new screen
}

// Grid navigation: A=left, B=right, C=up, D=down, #=select, *=manual
// sleep (go straight to the idle clock).
void handleHomeKey(char k) {
  int row = menuSel / 3, col = menuSel % 3;
  if (k == 'A') col = (col + 2) % 3;
  else if (k == 'B') col = (col + 1) % 3;
  else if (k == 'C') row = (row + 2) % 3;
  else if (k == 'D') row = (row + 1) % 3;
  else if (k == '#') { activateHomeItem((uint8_t)menuSel); return; }
  else if (k == '*') { state = STATE_IDLE; return; }
  else return;
  menuSel = row * 3 + col;
}

// ---- Timer / stopwatch key handling ----
void handleTimerKey(char k) {
  if (timerKind == TIMER_NONE) {
    // Entry mode: typing how many minutes to count down.
    if (k >= '0' && k <= '9') {
      if (timerInputLen < 3) {
        timerInput[timerInputLen++] = k;
        timerInput[timerInputLen] = 0;
      }
    } else if (k == '*') {
      if (timerInputLen > 0) {
        timerInputLen--;
        timerInput[timerInputLen] = 0;
      } else {
        state = STATE_HOME;
      }
    } else if (k == '#') {
      int mins = atoi(timerInput);
      if (mins > 0) {
        timerInputLen = 0;
        timerInput[0] = 0;
        startTimer(TIMER_COUNTDOWN, (unsigned long)mins * 60000UL);
      }
    }
  } else if (k == '*') {
    timerKind = TIMER_NONE;
    state = STATE_HOME;
  }
}

void handleStopwatchKey(char k) {
  if (k == '#') {
    if (swRunning) { swAccumMs += millis() - swStartMs; swRunning = false; }
    else { swStartMs = millis(); swRunning = true; }
  } else if (k == 'A') {
    swRunning = false;
    swAccumMs = 0;
  } else if (k == '*') {
    if (swRunning) { swAccumMs += millis() - swStartMs; swRunning = false; }
    state = STATE_HOME;
  }
}

// ---- Screens ----
// Pure sleep/idle screen: after IDLE_TIMEOUT_MS this shows ONLY the
// single most relevant number — real time if an RTC answered at boot,
// otherwise elapsed uptime — with no extra stats cluttering it. (A
// running Timer/Pomodoro/Stopwatch is already similarly "pure" on its
// own screen and is shown instead of this one — see loop()/updateIdle().)
// Minimal "digital watch" idle screen: one big number, centered, with a
// date line beneath it when an RTC is available — nothing else. No
// label, no stats, no border. Redraws every loop() call (cheap — a
// handful of primitives) but the content itself only visibly changes
// once a second, so it reads as calm rather than busy.
void drawIdleClock() {
  u8g2.clearBuffer();

  char big[16];
  if (rtcPresent) formatClock(big, sizeof(big), cachedRtcHour, cachedRtcMin, cachedRtcSec);
  else            formatHMS(big, sizeof(big), uptimeSeconds());
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(big);
  int y = rtcPresent ? 36 : 40; // nudge up a little when a date line follows
  u8g2.drawStr((128 - w) / 2, y, big);

  if (rtcPresent) {
    char dateLine[16];
    formatRtcDate(dateLine, sizeof(dateLine), cachedRtcDate, cachedRtcMonth, cachedRtcYear);
    u8g2.setFont(u8g2_font_6x10_tf);
    int dw = u8g2.getStrWidth(dateLine);
    u8g2.drawStr((128 - dw) / 2, 52, dateLine);
  }

  u8g2.sendBuffer();
}

// Settings is a tiny 2-item editable list (cursor via C/D, edit/toggle
// via #), plus a read-only status line. Session-only — see the globals'
// comment for why these aren't persisted to flash yet. (Enum + state
// vars live up in the GLOBALS section since activateHomeItem(), defined
// earlier in the file, needs to reference SETTINGS_VIEW.)
void handleSettingsKey(char k) {
  if (settingsSubMode == SETTINGS_EDIT_IDLE) {
    if (k >= '0' && k <= '9') {
      if (settingsDigitsLen < 3) { settingsDigits[settingsDigitsLen++] = k; settingsDigits[settingsDigitsLen] = 0; }
    } else if (k == '*') {
      if (settingsDigitsLen > 0) { settingsDigitsLen--; settingsDigits[settingsDigitsLen] = 0; }
      else settingsSubMode = SETTINGS_VIEW;
    } else if (k == '#') {
      int secs = atoi(settingsDigits);
      if (secs < 5 || secs > 600) { setError("5-600 sec only"); return; }
      idleTimeoutMs = (unsigned long)secs * 1000UL;
      triggerFlash(100);
      settingsSubMode = SETTINGS_VIEW;
    }
    return;
  }

  if (k == 'C') { settingsSel--; if (settingsSel < 0) settingsSel = 1; }
  else if (k == 'D') { settingsSel++; if (settingsSel > 1) settingsSel = 0; }
  else if (k == '#') {
    if (settingsSel == 0) {
      settingsSubMode = SETTINGS_EDIT_IDLE;
      settingsDigitsLen = 0;
      settingsDigits[0] = 0;
    } else {
      buzzerMuted = !buzzerMuted;
      triggerFlash(80);
    }
  } else if (k == '*') {
    state = STATE_HOME;
  }
}

void drawSettingsScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "SETTINGS");
  u8g2.drawHLine(0, 11, 128);

  if (settingsSubMode == SETTINGS_EDIT_IDLE) {
    u8g2.drawStr(0, 26, "Idle timeout (5-600s)");
    char line[20]; snprintf(line, sizeof(line), "Seconds: %s", settingsDigits);
    u8g2.drawStr(0, 40, line);
    if (errorMsg[0] && millis() < errorMsgUntil) u8g2.drawStr(0, 62, errorMsg);
    else u8g2.drawStr(0, 62, "#:OK *:Del/Back");
  } else {
    char line[28];
    snprintf(line, sizeof(line), "%cIdle timeout: %lus",
             settingsSel == 0 ? '>' : ' ', (unsigned long)(idleTimeoutMs / 1000));
    u8g2.drawStr(0, 24, line);
    snprintf(line, sizeof(line), "%cBuzzer: %s",
             settingsSel == 1 ? '>' : ' ', buzzerMuted ? "MUTED" : "ON");
    u8g2.drawStr(0, 36, line);

    char info[24];
    snprintf(info, sizeof(info), "RTC:%s  WiFi:%s", rtcPresent ? "OK" : "--", wifiEnabled ? "ON" : "OFF");
    u8g2.drawStr(0, 48, info);

    u8g2.drawStr(0, 62, "C/D:sel #:edit *:back");
  }

  if (millis() < flashUntil) {
    u8g2.setDrawColor(2);
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(1);
  }
  endTransitionClip();
  u8g2.sendBuffer();
}

void handleMoreKey(char k) {
  if (k == '*') state = STATE_HOME;
}

void drawMoreScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "MORE");
  u8g2.drawHLine(0, 11, 128);
  u8g2.drawStr(0, 34, "Coming soon.");
  u8g2.drawStr(0, 62, "*:Back");
  endTransitionClip();
  u8g2.sendBuffer();
}

// Simple bordered progress bar: a 1px frame with a filled portion
// proportional to `fraction` (0..1). Used by both Timer and Pomodoro.
void drawProgressBar(int x, int y, int w, int h, float fraction) {
  if (fraction < 0) fraction = 0;
  if (fraction > 1) fraction = 1;
  u8g2.drawFrame(x, y, w, h);
  int fillW = (int)((w - 2) * fraction);
  if (fillW > 0) u8g2.drawBox(x + 1, y + 1, fillW, h - 2);
}

// Plain countdown timer: minutes-entry screen, then a running screen
// with the big remaining time and a progress bar underneath. Pomodoro
// has its own screen (drawPomodoroScreen) even though it shares the
// same STATE_TIMER/timerKind machinery — see loop()'s draw switch.
void drawTimerScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (timerKind == TIMER_NONE) {
    u8g2.drawStr(0, 12, "Set timer (minutes)");
    char line[20]; snprintf(line, sizeof(line), "Minutes: %s", timerInput);
    u8g2.drawStr(0, 30, line);
    u8g2.drawStr(0, 60, "#:Start *:Del/Back");
  } else {
    u8g2.drawStr(0, 10, "TIMER");

    char rem[10];
    formatMMSS(rem, sizeof(rem), timerRemainingMs() / 1000);
    u8g2.setFont(u8g2_font_logisoso16_tr);
    int w = u8g2.getStrWidth(rem);
    u8g2.drawStr((128 - w) / 2, 34, rem);
    u8g2.setFont(u8g2_font_6x10_tf);

    float frac = (timerDurationMs > 0)
      ? (float)(timerDurationMs - timerRemainingMs()) / (float)timerDurationMs : 0;
    drawProgressBar(8, 42, 112, 10, frac);

    u8g2.drawStr(0, 62, "*:Cancel");
  }
  endTransitionClip();
  u8g2.sendBuffer();
}

// Pomodoro's own visual identity: progress bar + FOCUS/BREAK label +
// cycle count (e.g. "FOCUS 2/4"), distinct from the plain Timer screen.
// The FOCUS<->BREAK transition itself is the alarm screen in between
// (drawAlarmAnimation) — its invert-pulse doubles as the transition cue.
void drawPomodoroScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "POMODORO");

  char rem[10];
  formatMMSS(rem, sizeof(rem), timerRemainingMs() / 1000);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(rem);
  u8g2.drawStr((128 - w) / 2, 34, rem);
  u8g2.setFont(u8g2_font_6x10_tf);

  float frac = (timerDurationMs > 0)
    ? (float)(timerDurationMs - timerRemainingMs()) / (float)timerDurationMs : 0;
  drawProgressBar(8, 42, 112, 10, frac);

  char info[20];
  snprintf(info, sizeof(info), "%s  %u/%u",
           timerKind == TIMER_POMO_WORK ? "FOCUS" : "BREAK",
           (unsigned)pomoCycleIndex, (unsigned)POMO_CYCLES_PER_SESSION);
  int iw = u8g2.getStrWidth(info);
  u8g2.drawStr((128 - iw) / 2, 62, info);
  endTransitionClip();
  u8g2.sendBuffer();
}

void drawStopwatchScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "STOPWATCH");

  unsigned long ms = swAccumMs + (swRunning ? (millis() - swStartMs) : 0);
  char t[14];
  formatMMSSms(t, sizeof(t), ms);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(t);
  u8g2.drawStr((128 - w) / 2, 40, t);
  u8g2.setFont(u8g2_font_6x10_tf);

  u8g2.drawStr(0, 60, swRunning ? "#:Stop A:Rst *:Bck" : "#:Run A:Rst *:Bck");
  endTransitionClip();
  u8g2.sendBuffer();
}

// Alarm screen with a non-blocking invert-pulse animation: normal ->
// inverted -> normal -> inverted, while the buzzer melody plays. Uses
// u8g2's XOR draw mode (color 2) over the whole buffer after normal
// content is drawn — cheap, and works because the display is run in
// full-buffer (F) mode. Timing is independent of the melody's own
// note-by-note timing (updateAlarm()), just a steady ~250ms flip, which
// reads as "pulsing" without needing to be millisecond-synced to it.
void drawAlarmAnimation() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, alarmMsg);
  u8g2.drawStr(0, 40, "Press any key");
  u8g2.drawStr(0, 52, "to dismiss");

  bool inverted = (millis() / 250) % 2;
  if (inverted) {
    u8g2.setDrawColor(2); // XOR — flips every pixel drawn so far
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

// ============================================================
//  WEB SERVER / CAPTIVE PORTAL  (read-only in Stage 3)
// ============================================================

// Escapes &, <, >, " so item text can never break the HTML it's placed
// into. Truncates rather than overflowing if the destination is small.
void appendEscaped(char* dest, size_t destSize, const char* src) {
  size_t di = 0;
  for (const char* p = src; *p && di < destSize - 1; p++) {
    const char* rep = NULL;
    switch (*p) {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      default: break;
    }
    if (rep) {
      size_t rl = strlen(rep);
      if (di + rl >= destSize - 1) break;
      memcpy(dest + di, rep, rl);
      di += rl;
    } else {
      dest[di++] = *p;
    }
  }
  dest[di] = 0;
}

void setupWebServer() {
  // Registers routes once at boot. Does NOT start the radio or the
  // listener — that only happens on demand via startWiFi(), so the
  // Wi-Fi hardware draws no power until you actually ask for it.
  httpServer.on("/", HTTP_GET, handleWebRoot);
  httpServer.on("/inventory", HTTP_GET, handleWebInventory);
  httpServer.on("/add", HTTP_GET, handleWebAddForm);
  httpServer.on("/add", HTTP_POST, handleWebAddSubmit);
  httpServer.on("/edit", HTTP_GET, handleWebEditForm);
  httpServer.on("/edit", HTTP_POST, handleWebEditSubmit);
  httpServer.on("/delete", HTTP_GET, handleWebDeleteConfirm);
  httpServer.on("/delete", HTTP_POST, handleWebDeleteSubmit);
  httpServer.on("/import", HTTP_GET, handleImportForm);
  httpServer.on("/import", HTTP_POST, handleImportSubmit, handleImportUpload);
  httpServer.on("/export", HTTP_GET, handleExport);
  httpServer.on("/restore", HTTP_GET, handleRestoreBackup);
  httpServer.on("/restore", HTTP_POST, handleRestoreSubmit);
  httpServer.onNotFound(handleWebNotFound);
}

void startWiFi() {
  if (wifiEnabled) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIP);
  httpServer.begin();
  wifiEnabled = true;
  Serial.print("Wi-Fi AP started, IP: ");
  Serial.println(apIP);
}

void stopWiFi() {
  if (!wifiEnabled) return;
  httpServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiEnabled = false;
  Serial.println("Wi-Fi AP stopped (power saving)");
}

void handleWifiKey(char k) {
  if (k == '#') {
    if (wifiEnabled) stopWiFi(); else startWiFi();
  } else if (k == '*') {
    state = STATE_HOME;
  }
}

void drawWifiScreen() {
  u8g2.clearBuffer();
  beginTransitionClip();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "WI-FI");
  u8g2.drawHLine(0, 11, 128);

  if (wifiEnabled) {
    u8g2.drawStr(0, 26, "Status: ON");
    char line[24];
    snprintf(line, sizeof(line), "SSID: %s", AP_SSID);
    u8g2.drawStr(0, 38, line);
    snprintf(line, sizeof(line), "IP: %s", WiFi.softAPIP().toString().c_str());
    u8g2.drawStr(0, 50, line);
  } else {
    u8g2.drawStr(0, 30, "Status: OFF");
    u8g2.drawStr(0, 42, "(saving power)");
  }
  u8g2.drawStr(0, 62, "#:Toggle *:Back");
  endTransitionClip();
  u8g2.sendBuffer();
}

String htmlHeader(const char* title) {
  String h;
  h.reserve(500);
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>"; h += title; h += "</title>";
  h += "<style>body{font-family:sans-serif;font-size:14px;margin:10px}"
       "table{width:100%;border-collapse:collapse;margin-bottom:10px}"
       "th,td{padding:4px 6px;border-bottom:1px solid #ccc;text-align:left}"
       "th{background:#eee}"
       "input,select{padding:4px;margin:2px 0;width:100%;max-width:260px;box-sizing:border-box}"
       "button{padding:6px 12px;margin-top:6px}"
       ".err{color:#b00;font-weight:bold}"
       ".warn{color:#a60}"
       "nav a{margin-right:12px}</style></head><body>";
  h += "<nav><a href='/'>Status</a><a href='/inventory'>Inventory</a><a href='/add'>Add Item</a>"
       "<a href='/import'>Import</a><a href='/export'>Export</a></nav>";
  h += "<h2>"; h += title; h += "</h2>";
  return h;
}

String htmlFooter() {
  return "</body></html>";
}

// Case-insensitive substring match (ESP32 toolchain doesn't reliably
// provide strcasestr, so this is implemented directly).
bool containsCaseInsensitive(const char* haystack, const char* needle) {
  if (!*needle) return true;
  size_t nlen = strlen(needle);
  for (const char* p = haystack; *p; p++) {
    if (strncasecmp(p, needle, nlen) == 0) return true;
  }
  return false;
}

// A web-triggered add/edit/delete can shift array indices (deletes
// compact the array) or invalidate the currently-open item on the
// physical terminal. Rather than track that precisely, always fall
// back to the search screen and recompute results — safe and simple.
void invalidatePhysicalSelectionIfNeeded() {
  selectedItemIndex = -1;
  if (state == STATE_ITEM_DETAIL || state == STATE_LEND_QTY || state == STATE_RETURN_QTY) {
    state = STATE_SEARCH;              // only yank the user out of item screens
  }
  runSearch();
}

// ---- CRUD (validated on-device; all three reuse the Stage 2 safe save) ----

bool addItem(const char* name, const char* location, long qty, const char* status,
             char* errOut, size_t errOutSize) {
  if (itemCount >= MAX_RECORDS) {
    snprintf(errOut, errOutSize, "Inventory full (max %d records)", MAX_RECORDS);
    return false;
  }
  if (strlen(name) == 0) {
    snprintf(errOut, errOutSize, "Name is required");
    return false;
  }
  if (qty < 0 || qty > 65535) {
    snprintf(errOut, errOutSize, "Quantity must be 0-65535");
    return false;
  }
  if (!isValidStatus(status)) {
    snprintf(errOut, errOutSize, "Status must be working/faulty/untested");
    return false;
  }

  InventoryItem &it = items[itemCount];
  strlcpy(it.name, name, NAME_MAX_LEN);
  strlcpy(it.location, location, LOC_MAX_LEN);
  strlcpy(it.status, status, STATUS_MAX_LEN);
  it.qty = (uint16_t)qty;
  it.lent = 0;
  itemCount++;

  if (!saveInventory()) {
    itemCount--; // roll back — keep RAM matching what's actually on flash
    snprintf(errOut, errOutSize, "Save failed, item not added");
    return false;
  }
  return true;
}

bool editItem(uint16_t idx, const char* name, const char* location, long qty,
              const char* status, char* errOut, size_t errOutSize) {
  if (idx >= itemCount) {
    snprintf(errOut, errOutSize, "Item not found");
    return false;
  }
  if (strlen(name) == 0) {
    snprintf(errOut, errOutSize, "Name is required");
    return false;
  }
  if (!isValidStatus(status)) {
    snprintf(errOut, errOutSize, "Status must be working/faulty/untested");
    return false;
  }

  InventoryItem &it = items[idx];
  if (qty < 0 || qty > 65535) {
    snprintf(errOut, errOutSize, "Quantity must be 0-65535");
    return false;
  }
  if (qty < (long)it.lent) {
    snprintf(errOut, errOutSize, "Quantity can't be less than lent (%u)", it.lent);
    return false;
  }

  InventoryItem backup = it; // for rollback if save fails
  strlcpy(it.name, name, NAME_MAX_LEN);
  strlcpy(it.location, location, LOC_MAX_LEN);
  strlcpy(it.status, status, STATUS_MAX_LEN);
  it.qty = (uint16_t)qty;

  if (!saveInventory()) {
    it = backup;
    snprintf(errOut, errOutSize, "Save failed, changes reverted");
    return false;
  }
  return true;
}

bool deleteItem(uint16_t idx, char* errOut, size_t errOutSize) {
  if (idx >= itemCount) {
    snprintf(errOut, errOutSize, "Item not found");
    return false;
  }

  InventoryItem savedCopy = items[idx];
  for (uint16_t i = idx; i < itemCount - 1; i++) items[i] = items[i + 1];
  itemCount--;

  if (!saveInventory()) {
    // Roll back: shift back up and reinsert the deleted record
    for (uint16_t i = itemCount; i > idx; i--) items[i] = items[i - 1];
    items[idx] = savedCopy;
    itemCount++;
    snprintf(errOut, errOutSize, "Save failed, deletion reverted");
    return false;
  }
  return true;
}

// ---- Handlers ----

void handleWebRoot() {
  size_t freeFlash = 0;
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  if (total > used) freeFlash = total - used;

  String html = htmlHeader("ESP32 Inventory Terminal");
  html += "<p>Status: online</p>";
  { char up[16]; formatHMS(up, sizeof(up), uptimeSeconds());
    html += "<p>Session uptime: "; html += up; html += "</p>"; }
  html += "<p>Inventory records: " + String(itemCount) + "</p>";
  html += "<p>Free heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
  html += "<p>LittleFS free: " + String(freeFlash) + " bytes</p>";
  html += "<p><a href='/inventory'>View / manage inventory</a></p>";
  html += htmlFooter();

  httpServer.send(200, "text/html", html);
}

void handleWebInventory() {
  String q = httpServer.hasArg("q") ? httpServer.arg("q") : "";
  q.trim();

  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "text/html", "");
  httpServer.sendContent(htmlHeader("Inventory"));

  String searchForm = "<form method='GET' action='/inventory'>";
  searchForm += "<input type='text' name='q' placeholder='Search name...' value='";
  { char qEsc[QUERY_MAX_LEN * 2]; appendEscaped(qEsc, sizeof(qEsc), q.c_str()); searchForm += qEsc; }
  searchForm += "'> <button type='submit'>Search</button>";
  if (q.length() > 0) searchForm += " <a href='/inventory'>Clear</a>";
  searchForm += "</form>";
  httpServer.sendContent(searchForm);

  httpServer.sendContent(
    "<table><tr><th>Name</th><th>Loc</th><th>Qty</th><th>Avail</th>"
    "<th>Lent</th><th>Status</th><th>Actions</th></tr>"
  );

  char nameEsc[NAME_MAX_LEN * 2];
  char locEsc[LOC_MAX_LEN * 2];
  char statusEsc[STATUS_MAX_LEN * 2];
  char row[320];
  uint16_t shown = 0;

  for (uint16_t i = 0; i < itemCount; i++) {
    if (q.length() > 0 && !containsCaseInsensitive(items[i].name, q.c_str())) continue;

    appendEscaped(nameEsc, sizeof(nameEsc), items[i].name);
    appendEscaped(locEsc, sizeof(locEsc), items[i].location);
    appendEscaped(statusEsc, sizeof(statusEsc), items[i].status);

    snprintf(row, sizeof(row),
      "<tr><td>%s</td><td>%s</td><td>%u</td><td>%d</td><td>%u</td><td>%s</td>"
      "<td><a href='/edit?id=%u'>Edit</a><a href='/delete?id=%u'>Delete</a></td></tr>",
      nameEsc, locEsc, items[i].qty, availableOf(items[i]), items[i].lent, statusEsc, i, i);
    httpServer.sendContent(row);
    shown++;
  }
  if (shown == 0) httpServer.sendContent("<tr><td colspan='7'>No matching items</td></tr>");

  httpServer.sendContent("</table>");
  httpServer.sendContent("<p><small><a href='/restore'>Restore from backup</a></small></p>");
  httpServer.sendContent(htmlFooter());
}

void handleWebAddForm() {
  String html = htmlHeader("Add Item");
  html += "<form method='POST' action='/add'>";
  html += "<label>Name*<br><input type='text' name='name' maxlength='" + String(NAME_MAX_LEN - 1) + "' required></label><br>";
  html += "<label>Location<br><input type='text' name='location' maxlength='" + String(LOC_MAX_LEN - 1) + "'></label><br>";
  html += "<label>Quantity*<br><input type='number' name='qty' min='0' value='1' required></label><br>";
  html += "<label>Status*<br><select name='status'>";
  for (uint8_t s = 0; s < VALID_STATUS_COUNT; s++) {
    html += "<option value='"; html += VALID_STATUSES[s]; html += "'";
    if (s == 0) html += " selected";
    html += ">"; html += VALID_STATUSES[s]; html += "</option>";
  }
  html += "</select></label><br>";
  html += "<button type='submit'>Add Item</button> <a href='/inventory'>Cancel</a>";
  html += "</form>";
  html += htmlFooter();

  httpServer.send(200, "text/html", html);
}

void handleWebAddSubmit() {
  String name = httpServer.arg("name"); name.trim();
  String location = httpServer.arg("location"); location.trim();
  String status = httpServer.arg("status"); status.trim();
  long qty = httpServer.hasArg("qty") ? httpServer.arg("qty").toInt() : -1;

  char err[48] = "";
  if (!addItem(name.c_str(), location.c_str(), qty, status.c_str(), err, sizeof(err))) {
    String html = htmlHeader("Add Failed");
    html += "<p class='err'>"; html += err; html += "</p>";
    html += "<p><a href='/add'>Try again</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  invalidatePhysicalSelectionIfNeeded();
  httpServer.sendHeader("Location", "/inventory", true);
  httpServer.send(303, "text/plain", "");
}

void handleWebEditForm() {
  if (!httpServer.hasArg("id")) { httpServer.send(400, "text/plain", "Missing id"); return; }
  long idx = httpServer.arg("id").toInt();
  if (idx < 0 || idx >= (long)itemCount) { httpServer.send(404, "text/plain", "Item not found"); return; }
  InventoryItem &it = items[idx];

  String html = htmlHeader("Edit Item");
  html += "<form method='POST' action='/edit'>";
  html += "<input type='hidden' name='id' value='" + String(idx) + "'>";

  html += "<label>Name*<br><input type='text' name='name' value='";
  { char e[NAME_MAX_LEN * 2]; appendEscaped(e, sizeof(e), it.name); html += e; }
  html += "' maxlength='" + String(NAME_MAX_LEN - 1) + "' required></label><br>";

  html += "<label>Location<br><input type='text' name='location' value='";
  { char e[LOC_MAX_LEN * 2]; appendEscaped(e, sizeof(e), it.location); html += e; }
  html += "' maxlength='" + String(LOC_MAX_LEN - 1) + "'></label><br>";

  html += "<label>Quantity* (currently lent: " + String(it.lent) + ")<br>";
  html += "<input type='number' name='qty' min='" + String(it.lent) + "' value='" + String(it.qty) + "' required></label><br>";

  html += "<label>Status*<br><select name='status'>";
  for (uint8_t s = 0; s < VALID_STATUS_COUNT; s++) {
    html += "<option value='"; html += VALID_STATUSES[s]; html += "'";
    if (strcasecmp(VALID_STATUSES[s], it.status) == 0) html += " selected";
    html += ">"; html += VALID_STATUSES[s]; html += "</option>";
  }
  html += "</select></label><br>";

  html += "<button type='submit'>Save Changes</button> <a href='/inventory'>Cancel</a>";
  html += "</form>";
  html += htmlFooter();

  httpServer.send(200, "text/html", html);
}

void handleWebEditSubmit() {
  if (!httpServer.hasArg("id")) { httpServer.send(400, "text/plain", "Missing id"); return; }
  long idx = httpServer.arg("id").toInt();
  String name = httpServer.arg("name"); name.trim();
  String location = httpServer.arg("location"); location.trim();
  String status = httpServer.arg("status"); status.trim();
  long qty = httpServer.hasArg("qty") ? httpServer.arg("qty").toInt() : -1;

  char err[48] = "";
  if (idx < 0 || !editItem((uint16_t)idx, name.c_str(), location.c_str(), qty, status.c_str(), err, sizeof(err))) {
    if (idx < 0) snprintf(err, sizeof(err), "Invalid id");
    String html = htmlHeader("Edit Failed");
    html += "<p class='err'>"; html += err; html += "</p>";
    html += "<p><a href='/edit?id=" + String(idx) + "'>Try again</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  invalidatePhysicalSelectionIfNeeded();
  httpServer.sendHeader("Location", "/inventory", true);
  httpServer.send(303, "text/plain", "");
}

void handleWebDeleteConfirm() {
  if (!httpServer.hasArg("id")) { httpServer.send(400, "text/plain", "Missing id"); return; }
  long idx = httpServer.arg("id").toInt();
  if (idx < 0 || idx >= (long)itemCount) { httpServer.send(404, "text/plain", "Item not found"); return; }
  InventoryItem &it = items[idx];

  String html = htmlHeader("Delete Item");
  html += "<p>Delete <strong>";
  { char e[NAME_MAX_LEN * 2]; appendEscaped(e, sizeof(e), it.name); html += e; }
  html += "</strong> at "; html += it.location; html += "?</p>";
  if (it.lent > 0) {
    html += "<p class='warn'>Warning: " + String(it.lent) + " unit(s) currently lent out. "
            "Deleting will discard that lending record.</p>";
  }
  html += "<form method='POST' action='/delete'>";
  html += "<input type='hidden' name='id' value='" + String(idx) + "'>";
  html += "<button type='submit'>Confirm Delete</button> <a href='/inventory'>Cancel</a>";
  html += "</form>";
  html += htmlFooter();

  httpServer.send(200, "text/html", html);
}

void handleWebDeleteSubmit() {
  if (!httpServer.hasArg("id")) { httpServer.send(400, "text/plain", "Missing id"); return; }
  long idx = httpServer.arg("id").toInt();

  char err[48] = "";
  if (idx < 0 || !deleteItem((uint16_t)idx, err, sizeof(err))) {
    if (idx < 0) snprintf(err, sizeof(err), "Invalid id");
    String html = htmlHeader("Delete Failed");
    html += "<p class='err'>"; html += err; html += "</p>";
    html += "<p><a href='/inventory'>Back</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  invalidatePhysicalSelectionIfNeeded();
  httpServer.sendHeader("Location", "/inventory", true);
  httpServer.send(303, "text/plain", "");
}

// Any unrecognized path (including the various OS captive-portal probe
// URLs like /generate_204, /hotspot-detect.html, /connecttest.txt) is
// redirected to "/" — enough of a foundation to trigger the "sign in to
// network" prompt on most phones/laptops. OS-specific handling can be
// refined later if needed.
void handleWebNotFound() {
  // A bare redirect is enough for most captive-portal auto-detectors, but
  // some (notably a few Android/Windows variants) don't reliably act on
  // a Location header with no body. Sending a tiny HTML page with both a
  // meta-refresh AND a manual link covers those too, while staying tiny
  // and framework-free.
  String ip = WiFi.softAPIP().toString();
  String target = "http://" + ip + "/";
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                 "<meta http-equiv='refresh' content='0;url=" + target + "'>"
                 "<title>Inventory Terminal</title></head><body>"
                 "<p>Redirecting to the Inventory Terminal\342\200\246 "
                 "<a href='" + target + "'>tap here</a> if nothing happens.</p>"
                 "</body></html>";
  httpServer.sendHeader("Location", target, true);
  httpServer.send(302, "text/html", html);
}

// ---- JSON import / export / backup restore ----

// Same escaping as appendEscaped(), but for JSON string values (only
// '"' and '\' need escaping) rather than HTML.
void appendJsonEscaped(char* dest, size_t destSize, const char* src) {
  size_t di = 0;
  for (const char* p = src; *p && di < destSize - 1; p++) {
    if (*p == '"' || *p == '\\') {
      if (di + 2 >= destSize - 1) break;
      dest[di++] = '\\';
      dest[di++] = *p;
    } else {
      dest[di++] = *p;
    }
  }
  dest[di] = 0;
}

// Handles one chunk of a multipart file upload, streaming it straight to
// a temp file on LittleFS rather than buffering the whole thing in RAM.
// Enforces MAX_IMPORT_BYTES as it goes, so an oversized upload is
// rejected early rather than after fully landing on flash.
void handleImportUpload() {
  HTTPUpload &upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    importBytes = 0;
    importErr[0] = 0;
    LittleFS.remove(IMPORT_TMP_FILE); // best effort, in case a previous attempt left one behind
    importFile = LittleFS.open(IMPORT_TMP_FILE, "w");
    importInProgress = (bool)importFile;
    if (!importInProgress) strlcpy(importErr, "Could not create temp file", sizeof(importErr));

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!importInProgress) return;
    importBytes += upload.currentSize;
    if (importBytes > MAX_IMPORT_BYTES) {
      importInProgress = false;
      strlcpy(importErr, "File too large", sizeof(importErr));
      importFile.close();
      LittleFS.remove(IMPORT_TMP_FILE);
      return;
    }
    importFile.write(upload.buf, upload.currentSize);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (importInProgress) importFile.close();
  }
}

// Parses the uploaded file into a scratch heap buffer (never touching
// the live inventory until validated), carries over each matching
// item's currently-lent quantity from the live device state (the
// device, not an old export, is the source of truth for what's
// actually checked out right now), then commits via the existing
// Stage 2 safe-save path.
bool importInventoryFile(const char* path, uint16_t &resultCount, uint16_t &skippedOut,
                          uint16_t &clampedOut, char* errOut, size_t errOutSize) {
  File f = LittleFS.open(path, "r");
  if (!f) { snprintf(errOut, errOutSize, "Could not reopen uploaded file"); return false; }

  InventoryItem* newItems = (InventoryItem*)malloc(sizeof(InventoryItem) * MAX_RECORDS);
  if (!newItems) { f.close(); snprintf(errOut, errOutSize, "Out of memory"); return false; }

  uint16_t newCount = 0;
  bool ok = parseInventoryFromFile(f, newItems, newCount, skippedOut, clampedOut);
  f.close();

  if (!ok) {
    free(newItems);
    snprintf(errOut, errOutSize, "Uploaded file is not valid inventory JSON");
    return false;
  }
  if (newCount == 0) {
    free(newItems);
    snprintf(errOut, errOutSize, "Uploaded file has no valid records");
    return false;
  }

  for (uint16_t i = 0; i < newCount; i++) {
    for (uint16_t j = 0; j < itemCount; j++) {
      if (strcasecmp(newItems[i].name, items[j].name) == 0 &&
          strcasecmp(newItems[i].location, items[j].location) == 0) {
        uint16_t carried = items[j].lent;
        if (carried > newItems[i].qty) carried = newItems[i].qty; // clamp if the new qty shrank
        newItems[i].lent = carried;
        break;
      }
    }
  }

  memcpy(items, newItems, sizeof(InventoryItem) * newCount);
  free(newItems);
  itemCount = newCount;

  if (!saveInventory()) {
    loadInventory(); // roll back to whatever's genuinely still on flash
    snprintf(errOut, errOutSize, "Import parsed OK but save failed — reverted");
    return false;
  }

  resultCount = newCount;
  return true;
}

void handleImportForm() {
  String html = htmlHeader("Import Inventory");
  html += "<p>Upload a JSON file in this device's export format.</p>";
  html += "<form method='POST' action='/import' enctype='multipart/form-data'>";
  html += "<input type='file' name='file' accept='.json,application/json' required><br>";
  html += "<button type='submit'>Import</button> <a href='/inventory'>Cancel</a>";
  html += "</form>";
  html += "<p><small>Importing replaces the current inventory list. Quantities "
          "currently lent out on this device are carried over automatically "
          "for any item that still matches by name + location.</small></p>";
  html += htmlFooter();
  httpServer.send(200, "text/html", html);
}

void handleImportSubmit() {
  if (!importInProgress || importErr[0]) {
    String html = htmlHeader("Import Failed");
    html += "<p class='err'>"; html += (importErr[0] ? importErr : "Upload failed"); html += "</p>";
    html += "<p><a href='/import'>Try again</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    LittleFS.remove(IMPORT_TMP_FILE);
    importInProgress = false;
    return;
  }

  uint16_t newCount = 0, skipped = 0, clamped = 0;
  char err[64] = "";
  bool ok = importInventoryFile(IMPORT_TMP_FILE, newCount, skipped, clamped, err, sizeof(err));
  LittleFS.remove(IMPORT_TMP_FILE);
  importInProgress = false;

  if (!ok) {
    String html = htmlHeader("Import Failed");
    html += "<p class='err'>"; html += err; html += "</p>";
    html += "<p><a href='/import'>Try again</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  invalidatePhysicalSelectionIfNeeded();

  String html = htmlHeader("Import Complete");
  html += "<p>Imported " + String(newCount) + " record(s).</p>";
  if (skipped) html += "<p class='warn'>Skipped " + String(skipped) + " record(s) with no name.</p>";
  if (clamped) html += "<p class='warn'>Clamped " + String(clamped) + " out-of-range field(s).</p>";
  html += "<p><a href='/inventory'>View inventory</a></p>";
  html += htmlFooter();
  httpServer.send(200, "text/html", html);
}

// Streams the current inventory as a downloadable JSON file — the exact
// format /import expects back, so export-edit-reimport round-trips.
void handleExport() {
  httpServer.sendHeader("Content-Disposition", "attachment; filename=\"inventory-export.json\"");
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "application/json", "");

  httpServer.sendContent("[");
  char nameJ[NAME_MAX_LEN * 2], locJ[LOC_MAX_LEN * 2], statusJ[STATUS_MAX_LEN * 2];
  char chunk[300];
  for (uint16_t i = 0; i < itemCount; i++) {
    appendJsonEscaped(nameJ, sizeof(nameJ), items[i].name);
    appendJsonEscaped(locJ, sizeof(locJ), items[i].location);
    appendJsonEscaped(statusJ, sizeof(statusJ), items[i].status);
    snprintf(chunk, sizeof(chunk),
      "%s{\"name\":\"%s\",\"location\":\"%s\",\"qty\":%u,\"status\":\"%s\",\"lent\":%u}",
      i > 0 ? "," : "", nameJ, locJ, items[i].qty, statusJ, items[i].lent);
    httpServer.sendContent(chunk);
  }
  httpServer.sendContent("]");
}

void handleRestoreBackup() {
  if (!LittleFS.exists(INVENTORY_BAK_FILE)) {
    String html = htmlHeader("Restore Backup");
    html += "<p class='err'>No backup file found.</p>";
    html += "<p><a href='/inventory'>Back</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  String html = htmlHeader("Restore Backup");
  html += "<p>Restore the inventory from the last backup? This replaces the "
          "current inventory with whatever was saved immediately before the "
          "most recent change.</p>";
  html += "<form method='POST' action='/restore'>"
          "<button type='submit'>Confirm Restore</button> <a href='/inventory'>Cancel</a></form>";
  html += htmlFooter();
  httpServer.send(200, "text/html", html);
}

void handleRestoreSubmit() {
  if (!LittleFS.exists(INVENTORY_BAK_FILE)) {
    httpServer.send(404, "text/plain", "No backup found");
    return;
  }
  if (!tryLoadFrom(INVENTORY_BAK_FILE)) {
    String html = htmlHeader("Restore Failed");
    html += "<p class='err'>Backup file exists but failed to parse.</p>";
    html += "<p><a href='/inventory'>Back</a></p>";
    html += htmlFooter();
    httpServer.send(200, "text/html", html);
    return;
  }

  saveInventory(); // writes the restored data as the new primary (and rotates a fresh backup)
  invalidatePhysicalSelectionIfNeeded();
  httpServer.sendHeader("Location", "/inventory", true);
  httpServer.send(303, "text/plain", "");
}