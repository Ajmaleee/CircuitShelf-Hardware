/*
 * ESP32 Electronics Inventory Terminal
 * STAGE 1 — Core offline inventory terminal
 *
 * Target: Arduino IDE, ESP32 Arduino Core 3.x, ESP32 DevKit V1
 * NOT using PlatformIO.
 *
 * Features in this stage:
 *  - Loads inventory from /data/inventory.json (LittleFS) into RAM
 *  - Nokia-style multi-tap search (case-insensitive, prefix, real-time)
 *  - UP/DOWN navigation with wraparound, SELECT to view item
 *  - Item detail screen with prominent physical location
 *  - Lend / Return with quantity validation
 *  - Changes saved back to LittleFS
 *
 * Deliberately NOT implemented in Stage 1 (see chat reply for full list):
 *  - Wi-Fi / web interface / captive portal
 *  - Atomic / crash-safe writes, corruption recovery
 *  - JSON import/export, editing item fields, adding/deleting items
 */

#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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

// ---- Storage ----
#define INVENTORY_FILE "/data/inventory.json"

// ============================================================
//  DISPLAY
// ============================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

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

// ============================================================
//  APP STATE
// ============================================================
enum AppState {
  STATE_SEARCH,
  STATE_ITEM_DETAIL,
  STATE_LEND_QTY,
  STATE_RETURN_QTY
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

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
bool loadInventory();
bool saveInventory();
void runSearch();
void adjustScroll();
void setError(const char* msg);
int availableOf(const InventoryItem &it);
void handleKey(char k);
void handleSearchKey(char k);
void handleDetailKey(char k);
void handleQtyKey(char k, bool isLend);
void appendMultitapChar(char k);
void drawSearchScreen();
void drawItemDetail();
void drawQtyScreen(bool isLend);

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

  loadInventory();

  query[0] = 0;
  queryLen = 0;
  runSearch();
}

void loop() {
  char k = keypad.getKey();
  if (k != NO_KEY) {
    handleKey(k);
  }

  switch (state) {
    case STATE_SEARCH:       drawSearchScreen();      break;
    case STATE_ITEM_DETAIL:  drawItemDetail();        break;
    case STATE_LEND_QTY:     drawQtyScreen(true);     break;
    case STATE_RETURN_QTY:   drawQtyScreen(false);    break;
  }
}

// ============================================================
//  STORAGE
// ============================================================
bool loadInventory() {
  File f = LittleFS.open(INVENTORY_FILE, "r");
  if (!f) {
    Serial.println("ERROR: inventory file not found on LittleFS");
    itemCount = 0;
    return false;
  }

  JsonDocument doc; // ArduinoJson v7 — heap-allocated, sized automatically
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print("ERROR: JSON parse failed: ");
    Serial.println(err.c_str());
    itemCount = 0;
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  itemCount = 0;
  for (JsonObject obj : arr) {
    if (itemCount >= MAX_RECORDS) {
      Serial.println("WARNING: MAX_RECORDS reached, remaining records skipped");
      break;
    }
    InventoryItem &it = items[itemCount];
    strlcpy(it.name,     obj["name"]     | "",        NAME_MAX_LEN);
    strlcpy(it.location, obj["location"] | "",        LOC_MAX_LEN);
    strlcpy(it.status,   obj["status"]   | "working",  STATUS_MAX_LEN);
    it.qty  = obj["qty"]  | 0;
    it.lent = obj["lent"] | 0;
    itemCount++;
  }

  Serial.printf("Loaded %u inventory records\n", itemCount);
  return true;
}

void writeJsonString(File &f, const char* s) {
  f.print('"');
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') f.print('\\');
    f.print(*p);
  }
  f.print('"');
}

bool saveInventory() {
  File f = LittleFS.open(INVENTORY_FILE, "w");
  if (!f) {
    Serial.println("ERROR: cannot open inventory file for write");
    return false;
  }
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
  f.close();
  return true;
}

// ============================================================
//  SEARCH
// ============================================================
void runSearch() {
  resultCount = 0;
  if (queryLen == 0) {
    selectedResult = 0;
    scrollOffset = 0;
    return;
  }
  for (uint16_t i = 0; i < itemCount && resultCount < MAX_RECORDS; i++) {
    if (strncasecmp(items[i].name, query, queryLen) == 0) {
      resultIndices[resultCount++] = i;
    }
  }
  if (selectedResult >= (int16_t)resultCount) selectedResult = 0;
  if (selectedResult < 0) selectedResult = 0;
  scrollOffset = 0;
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
    appendMultitapChar(k);
    runSearch();
  } else if (k == '*') {
    queryLen = 0;
    query[0] = 0;
    lastMultitapKey = 0;
    runSearch();
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
    state = STATE_ITEM_DETAIL;
    errorMsg[0] = 0;
  } else if (k == '#') {
    int qv = atoi(qtyBuffer);
    if (qv <= 0) {
      setError("Enter a quantity");
      return;
    }
    if (isLend) {
      int avail = availableOf(it);
      if (qv > avail) {
        setError("Exceeds available");
        return;
      }
      it.lent += qv;
    } else {
      if (qv > (int)it.lent) {
        setError("Exceeds lent qty");
        return;
      }
      it.lent -= qv;
    }
    if (!saveInventory()) {
      setError("Save failed!");
      return;
    }
    state = STATE_ITEM_DETAIL;
  }
}

// ============================================================
//  RENDERING
// ============================================================
void drawSearchScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  u8g2.drawStr(0, 9, ">");
  u8g2.drawStr(8, 9, query);
  if ((millis() / 500) % 2 == 0) {
    int w = u8g2.getStrWidth(query);
    u8g2.drawStr(8 + w, 9, "_");
  }
  u8g2.drawHLine(0, 11, 128);

  if (queryLen == 0) {
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
    u8g2.drawStr(0, 60, "#:Confirm *:Cancel");
  }
  u8g2.sendBuffer();
}
