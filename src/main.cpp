// Auckland bin collection display for the ESP32 Cheap Yellow Display (CYD).
//
// Fetches the next collection dates from an aklapi server
// (https://github.com/rusq/aklapi) and shows the next collection day
// plus which bins go out.
//
// When it fetches:
//   - at boot
//   - every Monday at WEEKLY_REFRESH_HOUR
//   - when the displayed collection day is in the past (data is stale)
//   - when you press the BOOT button
//   - after a failure, it retries every 15 minutes
//
// The RGB LED on the back lights red on the day before and on collection day.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy include/config.example.h to include/config.h and fill in your settings"
#endif

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static const int PIN_BOOT_BUTTON = 0;   // the "BOOT" button on the board
static const int PIN_LED_RED     = 4;   // RGB LED, active LOW
static const int PIN_LED_GREEN   = 16;
static const int PIN_LED_BLUE    = 17;

static const uint32_t RETRY_INTERVAL_MS = 15UL * 60UL * 1000UL;  // 15 min

TFT_eSPI tft;

// Colours (RGB565)
static const uint16_t COL_BG        = TFT_WHITE;
static const uint16_t COL_HEADER    = 0x1A6B;   // dark teal-blue
static const uint16_t COL_TEXT      = TFT_BLACK;
static const uint16_t COL_SUBTLE    = 0x7BEF;   // grey
static const uint16_t COL_BIN_BODY  = 0x39E7;   // dark grey
static const uint16_t COL_LID_RUB   = 0xD8A3;   // red
static const uint16_t COL_LID_REC   = 0xFEA0;   // yellow
static const uint16_t COL_LID_FOOD  = 0x5E05;   // lime green
static const uint16_t COL_TODAY     = 0xD8A3;   // red
static const uint16_t COL_TOMORROW  = 0xFC00;   // orange
static const uint16_t COL_LATER     = 0x2C4A;   // green-ish

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
struct BinData {
  bool   valid = false;
  String address;
  String nextDate;      // "YYYY-MM-DD" of the next collection
  bool   rubbish = false;
  bool   recycle = false;
  bool   food    = false;
  String updatedAt;     // human readable time of the last successful fetch
};

static BinData  bins;
static String   lastError;
static bool     wantFetch       = true;    // fetch at boot
static uint32_t lastAttemptMs   = 0;
static bool     everAttempted   = false;
static int      lastMondayYday  = -1;      // tm_yday of the last Monday refresh
static int      lastDrawnYday   = -1;      // to redraw when the day changes

static const char* WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
static const char* MONTHS[]   = {"January", "February", "March", "April",
                                 "May", "June", "July", "August",
                                 "September", "October", "November", "December"};

// ---------------------------------------------------------------------------
// Date helpers
// ---------------------------------------------------------------------------

// Number of days since 1970-01-01 for a calendar date (Howard Hinnant's algorithm).
// Lets us compute "days until" without timezone / DST headaches.
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;
  const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static bool parseIsoDate(const String& s, int& y, int& m, int& d) {
  return s.length() >= 10 && sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3;
}

static bool getLocalNow(struct tm& now) {
  return getLocalTime(&now, 100);
}

// Days from today until the given "YYYY-MM-DD" date (0 = today, 1 = tomorrow, <0 = past).
// Returns INT32_MIN if the date or the clock is not available.
static long daysUntil(const String& iso) {
  int y, m, d;
  struct tm now;
  if (!parseIsoDate(iso, y, m, d) || !getLocalNow(now)) return INT32_MIN;
  long today = daysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
  return daysFromCivil(y, m, d) - today;
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
static String urlEncode(const char* s) {
  static const char* HEX_CHARS = "0123456789ABCDEF";
  String out;
  for (; *s; ++s) {
    const unsigned char c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += HEX_CHARS[c >> 4];
      out += HEX_CHARS[c & 0x0F];
    }
  }
  return out;
}

static void setLed(bool r, bool g, bool b) {
  // The CYD LED is active LOW
  digitalWrite(PIN_LED_RED,   r ? LOW : HIGH);
  digitalWrite(PIN_LED_GREEN, g ? LOW : HIGH);
  digitalWrite(PIN_LED_BLUE,  b ? LOW : HIGH);
}

// Shorten a string with "..." so it fits in maxWidth pixels with the current font.
static String fitText(String s, int maxWidth) {
  if (tft.textWidth(s) <= maxWidth) return s;
  while (s.length() > 0 && tft.textWidth(s + "...") > maxWidth) {
    s.remove(s.length() - 1);
  }
  return s + "...";
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawHeader(const char* title) {
  tft.fillRect(0, 0, tft.width(), 32, COL_HEADER);
  tft.setTextColor(TFT_WHITE, COL_HEADER);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(title, tft.width() / 2, 16);
}

// Simple status screen used during boot and for errors when we have no data.
static void drawStatus(const char* title, const String& line1, const String& line2 = "") {
  tft.fillScreen(COL_BG);
  drawHeader(title);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(fitText(line1, tft.width() - 10), tft.width() / 2, 110);
  if (line2.length()) {
    tft.setTextColor(COL_SUBTLE, COL_BG);
    tft.drawString(fitText(line2, tft.width() - 10), tft.width() / 2, 140);
  }
}

// Draws a wheelie bin centred on cx, with its top at `top`.
static void drawBin(int cx, int top, int w, int h, uint16_t lidColor, const char* label) {
  const int x = cx - w / 2;
  // handle on the lid
  tft.fillRoundRect(cx - w / 4, top, w / 2, 5, 2, lidColor);
  // lid
  tft.fillRoundRect(x - 4, top + 4, w + 8, 10, 3, lidColor);
  // body (slightly tapered: rectangle + two triangles)
  const int bodyTop = top + 15;
  const int bodyBottom = top + h;
  const int taper = w / 10;
  tft.fillRect(x + taper, bodyTop, w - 2 * taper, bodyBottom - bodyTop, COL_BIN_BODY);
  tft.fillTriangle(x, bodyTop, x + taper, bodyTop, x + taper, bodyBottom, COL_BIN_BODY);
  tft.fillTriangle(x + w, bodyTop, x + w - taper, bodyTop, x + w - taper, bodyBottom, COL_BIN_BODY);
  // coloured stripe on the body, so you can tell bins apart even with a grey body
  tft.fillRect(x + taper + 3, bodyTop + 6, w - 2 * taper - 6, 4, lidColor);
  // wheels
  tft.fillCircle(x + taper + 5, bodyBottom + 2, 4, TFT_BLACK);
  tft.fillCircle(x + w - taper - 5, bodyBottom + 2, 4, TFT_BLACK);
  // label
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextFont(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(label, cx, bodyBottom + 9);
}

static void drawBins() {
  int y, m, d;
  if (!bins.valid || !parseIsoDate(bins.nextDate, y, m, d)) return;

  tft.fillScreen(COL_BG);
  drawHeader("Next bin collection");

  const int cx = tft.width() / 2;
  const long dayNum = daysFromCivil(y, m, d);
  const int weekday = (int)((dayNum + 4) % 7);   // 1970-01-01 was a Thursday

  // Weekday
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(WEEKDAYS[weekday], cx, 56);

  // Day + month
  tft.setFreeFont(&FreeSans12pt7b);
  tft.drawString(String(d) + " " + MONTHS[m - 1], cx, 88);

  // Relative day
  const long until = daysUntil(bins.nextDate);
  String rel;
  uint16_t relCol = COL_LATER;
  if (until == INT32_MIN)   { rel = "";                 }
  else if (until < 0)       { rel = "(past - updating)"; relCol = COL_SUBTLE; }
  else if (until == 0)      { rel = "TODAY!";           relCol = COL_TODAY; }
  else if (until == 1)      { rel = "Tomorrow - bins out tonight!"; relCol = COL_TOMORROW; }
  else                      { rel = "in " + String(until) + " days"; }
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(relCol, COL_BG);
  tft.drawString(rel, cx, 114);

  // Bins collected that day: big wheelie bins for rubbish/recycling, small one for food scraps.
  struct Item { int w, h; uint16_t lid; const char* label; };
  Item items[3];
  int n = 0;
  if (bins.rubbish) items[n++] = {44, 58, COL_LID_RUB,  "Rubbish"};
  if (bins.recycle) items[n++] = {44, 58, COL_LID_REC,  "Recycling"};
  if (bins.food)    items[n++] = {30, 40, COL_LID_FOOD, "Food scraps"};

  const int slot = tft.width() / (n > 0 ? n : 1);
  const int baseLine = 192;  // all bins sit on the same "ground"
  for (int i = 0; i < n; ++i) {
    const int bx = slot * i + slot / 2;
    drawBin(bx, baseLine - items[i].h, items[i].w, items[i].h, items[i].lid, items[i].label);
  }

  // Footer: address + last update (+ error marker if the last refresh failed)
  tft.drawFastHLine(0, 220, tft.width(), COL_SUBTLE);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_SUBTLE, COL_BG);
  tft.drawString(fitText(bins.address, tft.width() - 4), 2, 224);
  String footer = "Updated " + bins.updatedAt;
  if (lastError.length()) footer += "  ! " + lastError;
  tft.setTextColor(lastError.length() ? COL_TODAY : COL_SUBTLE, COL_BG);
  tft.drawString(fitText(footer, tft.width() - 4), 2, 233);

  // Reminder LED: red the day before and on the day
  setLed(until == 0 || until == 1, false, false);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
static bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }
  Serial.printf("WiFi OK, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// configTzTime() (called once in setup) starts SNTP in the background;
// this just waits for the first sync.
static bool waitForTime() {
  struct tm now;
  for (int i = 0; i < 20; ++i) {           // up to ~20 s
    if (getLocalTime(&now, 1000) && now.tm_year > (2020 - 1900)) {
      Serial.printf("Time synced: %s", asctime(&now));
      return true;
    }
  }
  Serial.println("NTP sync failed");
  return false;
}

// Calls GET {AKLAPI_BASE_URL}/api/v1/rr?addr=...
// Response: {"rubbish":"2026-11-24","recycle":"2026-12-01","foodscraps":"2026-11-24","address":"..."}
static bool fetchBins() {
  lastAttemptMs = millis();
  everAttempted = true;

  if (!ensureWiFi()) { lastError = "no WiFi"; return false; }

  const String url = String(AKLAPI_BASE_URL) + "/api/v1/rr?addr=" + urlEncode(COLLECTION_ADDRESS);
  Serial.printf("GET %s\n", url.c_str());

  HTTPClient http;
  http.setTimeout(20000);   // aklapi scrapes the council website, it can be slow
  if (!http.begin(url)) { lastError = "bad URL"; return false; }

  const int code = http.GET();
  if (code <= 0) {
    lastError = "server unreachable";
    Serial.printf("HTTP error: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  Serial.printf("HTTP %d: %s\n", code, body.c_str());

  JsonDocument doc;
  const DeserializationError jsonErr = deserializeJson(doc, body);
  if (jsonErr) { lastError = "bad JSON (HTTP " + String(code) + ")"; return false; }

  if (code != 200) {
    lastError = doc["error"] | String("HTTP ") + code;
    return false;
  }

  const String rubbish = doc["rubbish"]    | "";
  const String recycle = doc["recycle"]    | "";
  const String food    = doc["foodscraps"] | "";

  // Next collection = earliest non-empty date. ISO dates compare correctly as strings.
  String next;
  for (const String* s : {&rubbish, &recycle, &food}) {
    if (s->length() && (next.isEmpty() || *s < next)) next = *s;
  }
  if (next.isEmpty()) { lastError = "no dates for address"; return false; }

  bins.valid    = true;
  bins.address  = doc["address"] | COLLECTION_ADDRESS;
  bins.nextDate = next;
  bins.rubbish  = (rubbish == next);
  bins.recycle  = (recycle == next);
  bins.food     = (food == next);

  struct tm now;
  char buf[32] = "?";
  if (getLocalNow(now)) strftime(buf, sizeof(buf), "%a %d %b %H:%M", &now);
  bins.updatedAt = buf;

  lastError = "";
  Serial.printf("Next collection %s: rubbish=%d recycle=%d food=%d\n",
                next.c_str(), bins.rubbish, bins.recycle, bins.food);
  return true;
}

static void doFetchAndDraw() {
  if (!bins.valid) drawStatus("Bin collection", "Fetching collection days...", AKLAPI_BASE_URL);

  if (fetchBins()) {
    wantFetch = false;
  } else {
    Serial.printf("Fetch failed: %s\n", lastError.c_str());
  }

  if (bins.valid) {
    drawBins();   // keep showing old data, with the error in the footer
  } else {
    drawStatus("Error", lastError, "Retrying in 15 min (or press BOOT)");
  }
  struct tm now;
  if (getLocalNow(now)) lastDrawnYday = now.tm_yday;
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nAuckland bin collection display");

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setLed(false, false, false);

  tft.init();
  tft.setRotation(1);   // landscape 320x240, USB port on the right. Use 3 to flip.
  drawStatus("Bin collection", "Connecting to WiFi...", WIFI_SSID);

  // Start SNTP with the Auckland timezone. It keeps syncing in the background.
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  if (!ensureWiFi()) {
    drawStatus("Error", "WiFi connection failed", "Check SSID / password in config.h");
  } else {
    drawStatus("Bin collection", "Getting time...", "pool.ntp.org");
    waitForTime();
  }

  struct tm now;
  if (getLocalNow(now) && now.tm_wday == 1) lastMondayYday = now.tm_yday;  // boot fetch counts for today

  doFetchAndDraw();
}

void loop() {
  static uint32_t lastCheck = 0;

  // BOOT button = manual refresh
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    delay(30);                                    // crude debounce
    if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
      Serial.println("Manual refresh");
      wantFetch = true;
      doFetchAndDraw();
      while (digitalRead(PIN_BOOT_BUTTON) == LOW) delay(10);
    }
  }

  // Everything else: check once per second
  if (millis() - lastCheck < 1000) { delay(10); return; }
  lastCheck = millis();

  struct tm now;
  const bool haveTime = getLocalNow(now);

  if (haveTime) {
    // Weekly refresh on Monday morning
    if (now.tm_wday == 1 && now.tm_hour >= WEEKLY_REFRESH_HOUR && lastMondayYday != now.tm_yday) {
      lastMondayYday = now.tm_yday;
      Serial.println("Weekly Monday refresh");
      wantFetch = true;
      lastAttemptMs = 0;
      everAttempted = false;   // fetch right now, not after the retry delay
    }

    // Collection day is over -> data is stale, fetch the next one
    const long until = daysUntil(bins.nextDate);
    if (bins.valid && until != INT32_MIN && until < 0) wantFetch = true;

    // New day -> redraw ("in 3 days" becomes "in 2 days", LED reminder, ...)
    if (bins.valid && now.tm_yday != lastDrawnYday) {
      lastDrawnYday = now.tm_yday;
      drawBins();
    }
  }

  if (wantFetch && (!everAttempted || millis() - lastAttemptMs >= RETRY_INTERVAL_MS)) {
    doFetchAndDraw();
  }
}
