/*
 * ESP32 Electronics Inventory Terminal
 * STAGE 4 + session tools — Full web inventory management,
 * plus uptime/idle screen, menu, timer, Pomodoro, stopwatch, alarm
 *
 * Target: Arduino IDE, ESP32 Arduino Core 3.x, ESP32 DevKit V1
 * NOT using PlatformIO.
 *
 * Carried over from Stage 1-3: physical keypad/OLED terminal (search,
 * lend/return, vibration feedback) and the Wi-Fi AP + captive portal
 * foundation, all unchanged.
 *
 * New in Stage 4 — the web interface is no longer read-only:
 *  - "/inventory" now includes a search box (substring, case-insensitive,
 *    server-side — separate from and non-interfering with the physical
 *    terminal's own prefix search) plus Edit/Delete links per row.
 *  - "/add" (GET form, POST submit) — add a new item. Name and quantity
 *    are required; status must be one of working/faulty/untested;
 *    new items always start with lent = 0. Validated on the ESP32.
 *  - "/edit?id=N" (GET form, POST submit) — change name/location/qty/
 *    status. Quantity can never be set below the amount already lent.
 *  - "/delete?id=N" (GET confirmation, POST to actually delete) — asks
 *    for confirmation and shows a warning if anything is currently lent.
 *  - All writes reuse the Stage 2 safe-save path (temp file, verify,
 *    backup rotation, atomic rename) and roll back in RAM if the save
 *    fails, exactly like a physical lend/return.
 *  - Any web-triggered add/edit/delete returns the physical terminal to
 *    the search screen and recomputes results, since array indices can
 *    shift (in particular after a delete) — this avoids the keypad UI
 *    ever pointing at a stale or wrong record.
 *  - No frameworks, no CDN, no external assets — plain HTML/CSS only,
 *    all generated on-device.
 *
 * ---- Session tools (extra step requested between Stage 4 and 5) ----
 *  - Boot -> IDLE screen: shows the real time-of-day if an RTC is
 *    detected, otherwise falls back to session UPTIME — automatically,
 *    with no crash either way. Also shows lent-item count and this
 *    session's lend/return totals.
 *  - Any key wakes the screen into a MENU (the wake keypress itself is
 *    swallowed, not acted on) — unless a Timer/Pomodoro/Stopwatch was
 *    still running, in which case it wakes back into that screen.
 *  - MENU: Search parts / Lent items / Timer / Pomodoro / Stopwatch /
 *    Wi-Fi / Clock.
 *  - Countdown timer and Pomodoro (25/5) run in the background; when one
 *    ends, the display wakes and a speaker + vibration alarm plays until
 *    any key is pressed (30s safety cutoff either way).
 *  - Idle -> uptime/clock screen after IDLE_TIMEOUT_MS; OLED switched off
 *    after DISPLAY_OFF_TIMEOUT_MS (any key wakes it; the clock/uptime
 *    keeps counting underneath).
 *  - Every keypress gives a short haptic "click" via the vibration motor.
 *    The speaker/buzzer is reserved for alarms (timer/Pomodoro/RTC alarm
 *    end) only — it no longer sounds on ordinary confirm/error feedback,
 *    which stays vibration-only.
 *
 * Known limitation: row "id" values are just the item's current array
 * index. If you have two browser tabs open and delete from one, the
 * other tab's Edit/Delete links can point at the wrong row until it's
 * reloaded. Fine for a single-admin local device; not fixed at this
 * stage.
 *
 * ---- Wi-Fi on demand (power-saving follow-up) ----
 *  - Wi-Fi AP + web server are OFF by default at boot. The "Wi-Fi" menu
 *    item shows status (SSID/IP when on) and toggles it with '#'.
 *    The radio (the single biggest power draw on an ESP32) only runs
 *    while you're actually updating inventory from a browser.
 *
 * ---- Optional RTC (power-saving follow-up) ----
 *  - An RTC (DS3231/DS1307-compatible) is entirely optional and shares
 *    the OLED's existing I2C bus — no extra wiring beyond the chip
 *    itself. Detected once at boot; every RTC-dependent feature checks
 *    that flag first, so a missing or later-disconnected RTC degrades
 *    to uptime-only instead of crashing.
 *  - "Clock" menu: view the current time, set it (HHMM), and arm a
 *    one-shot HH:MM alarm that fires the same beep+vibrate alarm as the
 *    timer/Pomodoro.
 *
 * ---- Removed on request ----
 *  - The LDR-based auto-brightness feature from the previous revision
 *    has been taken back out entirely (config, wiring, and code) in the
 *    interest of simplicity and power.
 *
 * Deliberately NOT implemented yet (see chat reply for full list):
 *  - JSON import/export over the web (Stage 5)
 *  - True deep-sleep / light-sleep (this only dims/blanks the OLED and
 *    idles the loop a little — the ESP32 core, Wi-Fi AP and web server
 *    stay fully powered throughout; real sleep is deferred to Stage 5)
 *  - Flipper-Zero-style animated/graphical UI polish (Stage 5)
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

#define VISIBLE_RESULT_LINES 4

// ---- Vibration motor (coin-cell ERM motor via transistor driver) ----
#define VIBRATION_MOTOR_PIN 4
#define VIBRATE_CONFIRM_MS  150   // buzz on successful lend/return save
#define VIBRATE_ERROR_MS    300   // buzz on validation/save error

// ---- Wi-Fi access point + web server ----
#define AP_SSID       "InventoryTerminal"
#define AP_PASSWORD   "inventory123"   // WPA2 needs 8+ chars; use "" for an open network
#define DNS_PORT      53
#define HTTP_PORT     80

// ---- Speaker (passive speaker/buzzer via NPN transistor, like the motor) ----
#define SOUND_ENABLED       1
#define SPEAKER_PIN         18

// ---- Key feedback ----
#define KEY_CLICK_VIBRATION_MS  15    // tiny haptic tick on every keypress

// ---- Optional RTC (DS3231/DS1307, shares the OLED's I2C bus - no new pins) ----
#define RTC_I2C_ADDR   0x68
// If no RTC is wired, detection simply fails and every RTC feature is
// skipped automatically — the device falls back to uptime-only, no crash.

// ---- Idle / power behaviour ----
#define IDLE_TIMEOUT_MS         20000UL    // no key -> show uptime screen
#define DISPLAY_OFF_TIMEOUT_MS  300000UL   // idle this long -> OLED off (0 = never)

// ---- Timer / Pomodoro / alarm ----
#define POMO_WORK_MIN       25
#define POMO_BREAK_MIN      5
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
  STATE_MENU,
  STATE_TIMER,
  STATE_STOPWATCH,
  STATE_ALARM,
  STATE_WIFI,
  STATE_CLOCK
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

// Session / idle / tools
enum TimerKind { TIMER_NONE, TIMER_COUNTDOWN, TIMER_POMO_WORK, TIMER_POMO_BREAK };
unsigned long lastActivityMs = 0;
bool displayOff = false;
uint32_t sessionLendUnits = 0, sessionReturnUnits = 0;
bool lentMode = false;                 // search screen showing lent items
int8_t menuSel = 0, menuScroll = 0;
uint8_t timerKind = TIMER_NONE;
unsigned long timerStartMs = 0, timerDurationMs = 0;
char timerInput[4] = "";
uint8_t timerInputLen = 0;
bool pendingBreak = false;             // Pomodoro: start break after alarm dismissed
bool swRunning = false;
unsigned long swStartMs = 0, swAccumMs = 0;
bool alarmActive = false;
unsigned long alarmStartMs = 0;
char alarmMsg[24] = "";
bool toneOn = false;
unsigned long toneUntil = 0;

// RTC (optional — everything below stays safely inert if none is found)
bool rtcPresent = false;
uint8_t cachedRtcHour = 0, cachedRtcMin = 0, cachedRtcSec = 0;
unsigned long lastRtcCheckMs = 0;
bool rtcAlarmEnabled = false;
uint8_t rtcAlarmHour = 0, rtcAlarmMin = 0;
uint8_t lastAlarmFiredMinute = 255; // sentinel so a fresh alarm can fire at minute 0
enum ClockSubMode { CLOCK_VIEW, CLOCK_SET_TIME, CLOCK_SET_ALARM };
ClockSubMode clockSubMode = CLOCK_VIEW;
char clockDigits[5] = "";
uint8_t clockDigitsLen = 0;

// Vibration motor (non-blocking pulse via millis())
bool vibrating = false;
unsigned long vibrateUntil = 0;

// Wi-Fi is OFF by default and only started on demand (power saving)
bool wifiEnabled = false;

// Menu items (index order matches activateMenuItem())
const char* MENU_ITEMS[] = { "Search parts", "Lent items", "Timer", "Pomodoro 25/5", "Stopwatch", "Wi-Fi", "Clock" };
const uint8_t MENU_ITEM_COUNT = 7;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
bool loadInventory();
bool tryLoadFrom(const char* path);
bool parseInventoryFromFile(File &f, uint16_t &outCount);
bool isValidStatus(const char* s);
void reportDuplicates();

bool saveInventory();
void writeInventoryJson(File &f);
bool validateJsonFile(const char* path, uint16_t expectedCount);

void runSearch();
void adjustScroll();
void setError(const char* msg);
int availableOf(const InventoryItem &it);
void startVibration(unsigned long ms);
void updateVibration();
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
void activateMenuItem(uint8_t idx);
void handleMenuKey(char k);
void handleTimerKey(char k);
void handleStopwatchKey(char k);
void drawIdleScreen();
void adjustMenuScroll();
void drawMenuScreen();
void drawTimerScreen();
void drawStopwatchScreen();
void drawAlarmScreen();

bool detectRTC();
uint8_t bcdToDec(uint8_t b);
uint8_t decToBcd(uint8_t d);
bool readRtcTime(uint8_t &h, uint8_t &m, uint8_t &s);
bool writeRtcTime(uint8_t h, uint8_t m, uint8_t s);
void updateRtcAlarm();
void formatClock(char* buf, size_t n, uint8_t h, uint8_t m, uint8_t s);
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

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS mount failed");
  }

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  u8g2.setBusClock(400000);
  u8g2.begin();

  keypad.setDebounceTime(KEYPAD_DEBOUNCE_MS);

  pinMode(VIBRATION_MOTOR_PIN, OUTPUT);
  digitalWrite(VIBRATION_MOTOR_PIN, LOW);

  loadInventory();
  if (!inventoryLoadFailed) reportDuplicates();

  setupWebServer();     // registers HTTP routes only — Wi-Fi stays OFF until requested
  soundInit();

  rtcPresent = detectRTC();
  Serial.println(rtcPresent ? "RTC detected" : "No RTC detected (uptime-only mode)");

  query[0] = 0;
  queryLen = 0;
  runSearch();

  lastActivityMs = millis();
  state = STATE_IDLE;   // boot straight into the uptime screen
}

void loop() {
  updateVibration(); // must run every cycle so a pulse always turns off in time
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
    startVibration(KEY_CLICK_VIBRATION_MS); // haptic click on every keypress
    if (alarmActive) {
      dismissAlarm();                       // any key silences the alarm
    } else if (displayOff || state == STATE_IDLE) {
      wakeDisplay();                        // wake key is swallowed
      if (timerKind != TIMER_NONE) {
        state = STATE_TIMER;                // a countdown/Pomodoro is still running
      } else if (swRunning) {
        state = STATE_STOPWATCH;            // stopwatch kept counting underneath
      } else {
        state = STATE_MENU;
        menuSel = 0;
        menuScroll = 0;
      }
    } else {
      handleKey(k);
    }
  }

  updateIdle();

  if (displayOff) {
    delay(20);   // display is off: nothing to draw, ease off the CPU a little
    return;
  }

  switch (state) {
    case STATE_SEARCH:       drawSearchScreen();      break;
    case STATE_ITEM_DETAIL:  drawItemDetail();        break;
    case STATE_LEND_QTY:     drawQtyScreen(true);     break;
    case STATE_RETURN_QTY:   drawQtyScreen(false);    break;
    case STATE_IDLE:         drawIdleScreen();        break;
    case STATE_MENU:         drawMenuScreen();        break;
    case STATE_TIMER:        drawTimerScreen();       break;
    case STATE_STOPWATCH:    drawStopwatchScreen();   break;
    case STATE_ALARM:        drawAlarmScreen();       break;
    case STATE_WIFI:          drawWifiScreen();         break;
    case STATE_CLOCK:         drawClockScreen();        break;
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

// Parses a JSON array of inventory records from an open file directly
// into the global items[] array, validating/clamping each field.
// Does not close the file. Returns false only on structural JSON errors
// (not on individual bad records — those are skipped/clamped instead).
bool parseInventoryFromFile(File &f, uint16_t &outCount) {
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
  uint16_t skippedEmpty = 0;
  uint16_t clampedFields = 0;

  for (JsonObject obj : arr) {
    if (outCount >= MAX_RECORDS) {
      Serial.println("WARNING: MAX_RECORDS reached, remaining records skipped");
      break;
    }

    const char* nameVal = obj["name"] | "";
    if (strlen(nameVal) == 0) {
      skippedEmpty++;
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
      clampedFields++;
    }

    long q = obj["qty"]  | 0;
    long l = obj["lent"] | 0;
    if (q < 0) { q = 0; clampedFields++; }
    if (l < 0) { l = 0; clampedFields++; }
    if (l > q) { l = q; clampedFields++; } // corrupted lent > qty is impossible, clamp it

    tmp.qty  = (uint16_t)q;
    tmp.lent = (uint16_t)l;

    items[outCount++] = tmp;
  }

  if (skippedEmpty) Serial.printf("WARNING: skipped %u record(s) with empty name\n", skippedEmpty);
  if (clampedFields) Serial.printf("WARNING: clamped %u out-of-range field(s)\n", clampedFields);

  return true;
}

// Attempts to load and validate one specific file into items[]/itemCount.
bool tryLoadFrom(const char* path) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint16_t count = 0;
  bool ok = parseInventoryFromFile(f, count);
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
  startVibration(VIBRATE_ERROR_MS); // vibration = routine feedback; buzzer stays reserved for alarms
}

void startVibration(unsigned long ms) {
  digitalWrite(VIBRATION_MOTOR_PIN, HIGH);
  vibrating = true;
  vibrateUntil = millis() + ms;
}

void updateVibration() {
  if (vibrating && millis() >= vibrateUntil) {
    digitalWrite(VIBRATION_MOTOR_PIN, LOW);
    vibrating = false;
  }
}

// ============================================================
//  KEY HANDLING
// ============================================================
void handleKey(char k) {
  switch (state) {
    case STATE_SEARCH:      handleSearchKey(k);        break;
    case STATE_ITEM_DETAIL: handleDetailKey(k);        break;
    case STATE_LEND_QTY:    handleQtyKey(k, true);     break;
    case STATE_RETURN_QTY:  handleQtyKey(k, false);    break;
    case STATE_MENU:        handleMenuKey(k);          break;
    case STATE_TIMER:       handleTimerKey(k);         break;
    case STATE_STOPWATCH:   handleStopwatchKey(k);     break;
    case STATE_WIFI:        handleWifiKey(k);          break;
    case STATE_CLOCK:       handleClockKey(k);          break;
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
      state = STATE_MENU;
    } else if (queryLen == 0) {        // nothing to clear -> back to menu
      state = STATE_MENU;
    } else {
      queryLen = 0;
      query[0] = 0;
      lastMultitapKey = 0;
      runSearch();
    }
  } else if (k == 'C') {
    if (resultCount > 0) {
      selectedResult--;
      if (selectedResult < 0) selectedResult = resultCount - 1;
      adjustScroll();
    }
  } else if (k == 'D') {
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
  // A / B have no meaning until an item is selected — ignored here
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
    startVibration(VIBRATE_CONFIRM_MS); // vibration = routine feedback; buzzer stays reserved for alarms
    state = STATE_ITEM_DETAIL;
  }
}

// ============================================================
//  RENDERING
// ============================================================
void drawSearchScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (lentMode) {
    u8g2.drawStr(0, 9, "LENT OUT");
  } else {
    u8g2.drawStr(0, 9, ">");
    u8g2.drawStr(8, 9, query);
    if ((millis() / 500) % 2 == 0) {
      int w = u8g2.getStrWidth(query);
      u8g2.drawStr(8 + w, 9, "_");
    }
  }
  u8g2.drawHLine(0, 11, 128);

  if (lentMode && resultCount == 0) {
    u8g2.drawStr(0, 28, "Nothing lent out");
  } else if (!lentMode && queryLen == 0) {
    u8g2.drawStr(0, 28, "Type to search...");
  } else if (resultCount == 0) {
    u8g2.drawStr(0, 28, "No matches");
  } else {
    int y = 22;
    for (int i = 0; i < VISIBLE_RESULT_LINES && (scrollOffset + i) < (int16_t)resultCount; i++) {
      int ridx = scrollOffset + i;
      InventoryItem &it = items[resultIndices[ridx]];
      char line[24];
      char prefix = (ridx == selectedResult) ? '>' : ' ';
      snprintf(line, sizeof(line), "%c%.20s", prefix, it.name);
      u8g2.drawStr(0, y, line);
      y += 12;
    }
    if (resultCount > VISIBLE_RESULT_LINES) {
      char cnt[12];
      snprintf(cnt, sizeof(cnt), "%d/%d", selectedResult + 1, resultCount);
      u8g2.drawStr(96, 63, cnt);
    }
  }
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
//   on-demand Wi-Fi, optional RTC clock/alarm, speaker + key-click vibration)
// ============================================================

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

// ---- Display power ----
void wakeDisplay() {
  if (displayOff) {
    displayOff = false;
    u8g2.setPowerSave(0);
  }
}

void updateIdle() {
  unsigned long now = millis();
  unsigned long idleFor = now - lastActivityMs;

  // Drop back to the uptime screen after inactivity — but never while a
  // timer/stopwatch is actively being watched, and never during an alarm.
  if (!alarmActive && state != STATE_IDLE && state != STATE_TIMER && state != STATE_STOPWATCH) {
    if (idleFor >= IDLE_TIMEOUT_MS) {
      state = STATE_IDLE;
    }
  }

  if (DISPLAY_OFF_TIMEOUT_MS > 0 && !displayOff && !alarmActive) {
    if (idleFor >= DISPLAY_OFF_TIMEOUT_MS) {
      displayOff = true;
      u8g2.setPowerSave(1); // OLED driver sleep — uptime keeps counting underneath
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
    startAlarm("Break over!");
  } else {
    timerKind = TIMER_NONE;
    startAlarm("Timer done!");
  }
}

// ---- Alarm (beep + vibrate pattern until any key is pressed) ----
void startAlarm(const char* msg) {
  alarmActive = true;
  alarmStartMs = millis();
  strlcpy(alarmMsg, msg, sizeof(alarmMsg));
  state = STATE_ALARM;
  wakeDisplay();
  toneOn = false;
  toneUntil = millis(); // fire the first beep immediately
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
  } else {
    state = STATE_MENU;
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

  if (now >= toneUntil) {
    toneOn = !toneOn;
    if (toneOn) {
#if SOUND_ENABLED
      tone(SPEAKER_PIN, 2000, 250);
#endif
      startVibration(200);
      toneUntil = now + 350;
    } else {
      toneUntil = now + 150;
    }
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
  Wire.beginTransmission(RTC_I2C_ADDR);
  return (Wire.endTransmission() == 0);
}

bool readRtcTime(uint8_t &h, uint8_t &m, uint8_t &s) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)RTC_I2C_ADDR, 3) != 3) return false;

  uint8_t rs = Wire.read();
  uint8_t rm = Wire.read();
  uint8_t rh = Wire.read();
  s = bcdToDec(rs & 0x7F);
  m = bcdToDec(rm & 0x7F);
  h = bcdToDec(rh & 0x3F); // strip 12h/PM bits — we always write 24h mode
  return true;
}

bool writeRtcTime(uint8_t h, uint8_t m, uint8_t s) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(decToBcd(s));
  Wire.write(decToBcd(m));
  Wire.write(decToBcd(h) & 0x3F); // bit6=0 forces 24-hour mode on DS3231
  return (Wire.endTransmission() == 0);
}

void formatClock(char* buf, size_t n, uint8_t h, uint8_t m, uint8_t s) {
  snprintf(buf, n, "%02u:%02u:%02u", h, m, s);
}

// Throttled to ~1/sec: refreshes the cached time used for display and
// checks the one-shot HH:MM alarm, without hammering the I2C bus.
void updateRtcAlarm() {
  if (!rtcPresent || alarmActive) return;
  unsigned long now = millis();
  if (now - lastRtcCheckMs < 900) return;
  lastRtcCheckMs = now;

  uint8_t h, m, s;
  if (!readRtcTime(h, m, s)) {
    rtcPresent = false; // lost comms mid-session — degrade gracefully, don't crash
    return;
  }
  cachedRtcHour = h; cachedRtcMin = m; cachedRtcSec = s;

  if (rtcAlarmEnabled && h == rtcAlarmHour && m == rtcAlarmMin && lastAlarmFiredMinute != m) {
    lastAlarmFiredMinute = m;
    rtcAlarmEnabled = false; // one-shot alarm, not a daily recurring one
    startAlarm("Alarm!");
  }
}

void handleClockKey(char k) {
  if (!rtcPresent) {
    if (k == '*') state = STATE_MENU; // nothing else to do without an RTC — no crash, just back out
    return;
  }

  if (clockSubMode == CLOCK_VIEW) {
    if (k == 'A') { clockSubMode = CLOCK_SET_TIME; clockDigitsLen = 0; clockDigits[0] = 0; }
    else if (k == 'B') { clockSubMode = CLOCK_SET_ALARM; clockDigitsLen = 0; clockDigits[0] = 0; }
    else if (k == 'D') { rtcAlarmEnabled = !rtcAlarmEnabled; if (rtcAlarmEnabled) lastAlarmFiredMinute = 255; }
    else if (k == '*') { state = STATE_MENU; }
    return;
  }

  // CLOCK_SET_TIME / CLOCK_SET_ALARM: enter 4 digits as HHMM
  if (k >= '0' && k <= '9') {
    if (clockDigitsLen < 4) { clockDigits[clockDigitsLen++] = k; clockDigits[clockDigitsLen] = 0; }
  } else if (k == '*') {
    if (clockDigitsLen > 0) { clockDigitsLen--; clockDigits[clockDigitsLen] = 0; }
    else clockSubMode = CLOCK_VIEW;
  } else if (k == '#') {
    if (clockDigitsLen != 4) { setError("Enter HHMM"); return; }
    int hh = (clockDigits[0] - '0') * 10 + (clockDigits[1] - '0');
    int mm = (clockDigits[2] - '0') * 10 + (clockDigits[3] - '0');
    if (hh > 23 || mm > 59) { setError("Invalid time"); return; }

    if (clockSubMode == CLOCK_SET_TIME) {
      if (!writeRtcTime((uint8_t)hh, (uint8_t)mm, 0)) { setError("RTC write failed"); return; }
      startVibration(VIBRATE_CONFIRM_MS);
    } else { // CLOCK_SET_ALARM
      rtcAlarmHour = (uint8_t)hh;
      rtcAlarmMin = (uint8_t)mm;
      rtcAlarmEnabled = true;
      lastAlarmFiredMinute = 255;
      startVibration(VIBRATE_CONFIRM_MS);
    }
    clockSubMode = CLOCK_VIEW;
  }
}

void drawClockScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "CLOCK");
  u8g2.drawHLine(0, 11, 128);

  if (!rtcPresent) {
    u8g2.drawStr(0, 28, "No RTC found");
    u8g2.drawStr(0, 40, "(uptime-only mode)");
    u8g2.drawStr(0, 60, "*:Back");
    u8g2.sendBuffer();
    return;
  }

  if (clockSubMode == CLOCK_VIEW) {
    char t[10];
    formatClock(t, sizeof(t), cachedRtcHour, cachedRtcMin, cachedRtcSec);
    u8g2.setFont(u8g2_font_logisoso16_tr);
    int w = u8g2.getStrWidth(t);
    u8g2.drawStr((128 - w) / 2, 34, t);
    u8g2.setFont(u8g2_font_6x10_tf);

    char alarmLine[24];
    if (rtcAlarmEnabled) snprintf(alarmLine, sizeof(alarmLine), "Alarm %02u:%02u ON", rtcAlarmHour, rtcAlarmMin);
    else snprintf(alarmLine, sizeof(alarmLine), "Alarm: OFF");
    u8g2.drawStr(0, 48, alarmLine);

    if (errorMsg[0] && millis() < errorMsgUntil) u8g2.drawStr(0, 60, errorMsg);
    else u8g2.drawStr(0, 60, "A:SetTime B:Alarm D:Tgl");
  } else {
    u8g2.drawStr(0, 24, clockSubMode == CLOCK_SET_TIME ? "Set time (HHMM)" : "Set alarm (HHMM)");
    char line[20]; snprintf(line, sizeof(line), "HHMM: %s", clockDigits);
    u8g2.drawStr(0, 40, line);
    if (errorMsg[0] && millis() < errorMsgUntil) u8g2.drawStr(0, 60, errorMsg);
    else u8g2.drawStr(0, 60, "#:OK *:Del/Back");
  }
  u8g2.sendBuffer();
}

// ---- Menu ----
void adjustMenuScroll() {
  if (menuSel < menuScroll) menuScroll = menuSel;
  if (menuSel >= menuScroll + VISIBLE_RESULT_LINES) menuScroll = menuSel - VISIBLE_RESULT_LINES + 1;
}

void activateMenuItem(uint8_t idx) {
  switch (idx) {
    case 0: // Search parts
      lentMode = false;
      state = STATE_SEARCH;
      runSearch();
      break;
    case 1: // Lent items
      lentMode = true;
      state = STATE_SEARCH;
      runSearch();
      break;
    case 2: // Timer
      timerKind = TIMER_NONE;
      timerInputLen = 0;
      timerInput[0] = 0;
      state = STATE_TIMER;
      break;
    case 3: // Pomodoro
      startTimer(TIMER_POMO_WORK, (unsigned long)POMO_WORK_MIN * 60000UL);
      break;
    case 4: // Stopwatch
      swRunning = false;
      swAccumMs = 0;
      state = STATE_STOPWATCH;
      break;
    case 5: // Wi-Fi
      state = STATE_WIFI;
      break;
    case 6: // Clock
      clockSubMode = CLOCK_VIEW;
      clockDigitsLen = 0;
      clockDigits[0] = 0;
      state = STATE_CLOCK;
      break;
    default: break;
  }
}

void handleMenuKey(char k) {
  if (k == 'C') {
    menuSel--;
    if (menuSel < 0) menuSel = MENU_ITEM_COUNT - 1;
    adjustMenuScroll();
  } else if (k == 'D') {
    menuSel++;
    if (menuSel >= MENU_ITEM_COUNT) menuSel = 0;
    adjustMenuScroll();
  } else if (k == '#') {
    activateMenuItem((uint8_t)menuSel);
  } else if (k == '*') {
    state = STATE_IDLE;
  }
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
        state = STATE_MENU;
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
    state = STATE_MENU;
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
    state = STATE_MENU;
  }
}

// ---- Screens ----
void drawIdleScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "Inventory Terminal");
  u8g2.drawHLine(0, 11, 128);

  // Show the real wall-clock time if an RTC answered at boot; otherwise
  // fall back to elapsed session uptime. Either way this never crashes —
  // it just picks whichever source of truth is actually available.
  u8g2.drawStr(0, 20, rtcPresent ? "Time" : "Uptime");
  char big[16];
  if (rtcPresent) formatClock(big, sizeof(big), cachedRtcHour, cachedRtcMin, cachedRtcSec);
  else            formatHMS(big, sizeof(big), uptimeSeconds());
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(big);
  u8g2.drawStr((128 - w) / 2, 40, big);
  u8g2.setFont(u8g2_font_6x10_tf);

  uint16_t lentCount = 0;
  for (uint16_t i = 0; i < itemCount; i++) if (items[i].lent > 0) lentCount++;

  char line1[24]; snprintf(line1, sizeof(line1), "Lent items: %u", lentCount);
  char line2[24]; snprintf(line2, sizeof(line2), "L:%lu R:%lu WiFi:%s",
                            (unsigned long)sessionLendUnits, (unsigned long)sessionReturnUnits,
                            wifiEnabled ? "ON" : "OFF");
  u8g2.drawStr(0, 52, line1);
  u8g2.drawStr(0, 63, line2);
  u8g2.sendBuffer();
}

void drawMenuScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "MENU");
  u8g2.drawHLine(0, 11, 128);

  int y = 22;
  for (int i = 0; i < VISIBLE_RESULT_LINES && (menuScroll + i) < MENU_ITEM_COUNT; i++) {
    int idx = menuScroll + i;
    char line[24];
    char prefix = (idx == menuSel) ? '>' : ' ';
    snprintf(line, sizeof(line), "%c%s", prefix, MENU_ITEMS[idx]);
    u8g2.drawStr(0, y, line);
    y += 12;
  }
  u8g2.sendBuffer();
}

void drawTimerScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (timerKind == TIMER_NONE) {
    u8g2.drawStr(0, 12, "Set timer (minutes)");
    char line[20]; snprintf(line, sizeof(line), "Minutes: %s", timerInput);
    u8g2.drawStr(0, 30, line);
    u8g2.drawStr(0, 60, "#:Start *:Del/Back");
  } else {
    const char* label = (timerKind == TIMER_POMO_WORK)  ? "POMODORO - WORK"  :
                         (timerKind == TIMER_POMO_BREAK) ? "POMODORO - BREAK" : "TIMER";
    u8g2.drawStr(0, 12, label);

    char rem[10];
    formatMMSS(rem, sizeof(rem), timerRemainingMs() / 1000);
    u8g2.setFont(u8g2_font_logisoso16_tr);
    int w = u8g2.getStrWidth(rem);
    u8g2.drawStr((128 - w) / 2, 40, rem);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 60, "*:Cancel");
  }
  u8g2.sendBuffer();
}

void drawStopwatchScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "STOPWATCH");

  unsigned long ms = swAccumMs + (swRunning ? (millis() - swStartMs) : 0);
  char t[10];
  formatMMSS(t, sizeof(t), ms / 1000);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  int w = u8g2.getStrWidth(t);
  u8g2.drawStr((128 - w) / 2, 40, t);
  u8g2.setFont(u8g2_font_6x10_tf);

  u8g2.drawStr(0, 60, swRunning ? "#:Stop A:Rst *:Bck" : "#:Run A:Rst *:Bck");
  u8g2.sendBuffer();
}

void drawAlarmScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, alarmMsg);
  u8g2.drawStr(0, 40, "Press any key");
  u8g2.drawStr(0, 52, "to dismiss");
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
    state = STATE_MENU;
  }
}

void drawWifiScreen() {
  u8g2.clearBuffer();
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
  h += "<nav><a href='/'>Status</a><a href='/inventory'>Inventory</a><a href='/add'>Add Item</a></nav>";
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
  String target = "http://" + WiFi.softAPIP().toString() + "/";
  httpServer.sendHeader("Location", target, true);
  httpServer.send(302, "text/plain", "");
}