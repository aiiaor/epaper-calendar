#include "driver.h"
#include "TFT_eSPI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Sarabun7.h"
#include <vector>

// ── Configuration ──────────────────────────────────────────────────────────
const char* ssid      = "ใส่ชื่อ wifi";
const char* password  = "ใส่รหัส wifi";
const char* scriptUrl = "ใส่ url ของ google script";

#define FONT_LINE_H  14

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

// ── Layout constants (landscape 800×480) ───────────────────────────────────
#define SCREEN_W     800
#define SCREEN_H     480
#define TIME_COL_W    38
#define HEADER_H      46
#define DAY_COLS       7
#define DAY_COL_W   ((SCREEN_W - TIME_COL_W) / DAY_COLS)
#define GRID_H       (SCREEN_H - HEADER_H)

#define MAX_EVENTS    30
#define MAX_LANES      3
#define TZ_OFFSET_H    7

#define EVENT_X_PAD    5
#define EVENT_Y_PAD    7
#define EVENT_BOTTOM_PAD 4

// Time range — recomputed from events on each fetch
int HOUR_START  = 9;
int HOUR_END    = 21;
int TOTAL_HOURS = 12;
int HOUR_H      = GRID_H / 12;

// ── Types ────────────────────────────────────────────────────────────────────
struct CalEvent {
  String title;
  int startHour, startMin;
  int endHour,   endMin;
  int dayOfWeek;   // 0=Mon … 6=Sun
};

struct EventLayout {
  int lane;
  int numLanes;
};

struct EventSlot {
  int x, y1, h, slotW;
  bool visible;
};

struct ThaiCluster {
  String base = "";
  String vowelAbove = "";
  String vowelBelow = "";
  String tone = "";
  int width = 0;
};

// ── State ────────────────────────────────────────────────────────────────────
CalEvent events[MAX_EVENTS];
EventLayout layout[MAX_EVENTS];
int eventCount = 0;

int colDay[7], colMonth[7];
int todayDow = -1;

// ── Forward declarations ─────────────────────────────────────────────────────
void fetchAndDisplay();
void computeLayout();
void drawGrid();

// ── Time / layout helpers ────────────────────────────────────────────────────
int minutesOf(int hour, int minute) { return hour * 60 + minute; }

bool isAllDay(const CalEvent& e) {
  return e.startHour == 0 && e.startMin == 0;
}

int timeToY(int hour, int minute) {
  float frac = (hour - HOUR_START) + (minute / 60.0);
  return HEADER_H + (int)(frac * HOUR_H);
}

int dayToX(int dow) { return TIME_COL_W + dow * DAY_COL_W; }
String pad2(int n) { return (n < 10 ? "0" : "") + String(n); }

EventSlot computeEventSlot(int i) {
  EventSlot slot = {0, 0, 0, 0, false};
  const CalEvent& e = events[i];
  if (e.dayOfWeek < 0 || e.dayOfWeek > 6) return slot;

  slot.slotW = (DAY_COL_W - 2) / layout[i].numLanes;
  slot.x     = dayToX(e.dayOfWeek) + 1 + layout[i].lane * slot.slotW;

  int y2 = timeToY(e.endHour, e.endMin);
  slot.y1 = timeToY(e.startHour, e.startMin);
  if (y2 <= HEADER_H || slot.y1 >= SCREEN_H) return slot;

  slot.y1 = max(slot.y1, HEADER_H + 1);
  y2      = min(y2, SCREEN_H - 1);
  slot.h  = max(2, y2 - slot.y1);
  slot.visible = true;
  return slot;
}

// ── Date helpers ─────────────────────────────────────────────────────────────
void parseISO(String iso, int& year, int& month, int& day, int& hour, int& minute) {
  year   = iso.substring(0,  4).toInt();
  month  = iso.substring(5,  7).toInt();
  day    = iso.substring(8,  10).toInt();
  hour   = iso.substring(11, 13).toInt() + TZ_OFFSET_H;
  minute = iso.substring(14, 16).toInt();
  if (hour >= 24) { hour -= 24; day++; }
}

int getDow(int y, int m, int d) {
  if (m < 3) { m += 12; y--; }
  int k = y % 100, j = y / 100;
  int dow = (d + (13 * (m + 1) / 5) + k + k / 4 + j / 4 + 5 * j) % 7;
  return (dow + 5) % 7;
}

String sanitizeTitle(String raw) {
  String clean = "";
  for (int i = 0; i < (int)raw.length(); i++) {
    if ((unsigned char)raw[i] >= 0x20) clean += raw[i];
  }
  return clean.length() > 0 ? clean : "(no title)";
}

void fillMissingWeekDates() {
  int refDow = -1;
  for (int i = 0; i < 7; i++) {
    if (colDay[i] > 0) { refDow = i; break; }
  }
  if (refDow < 0) return;

  int refD = colDay[refDow], refM = colMonth[refDow];
  int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  for (int i = 0; i < 7; i++) {
    if (colDay[i] > 0) continue;
    int d = refD + (i - refDow), m = refM;
    while (d > daysInMonth[m]) { d -= daysInMonth[m]; if (++m > 12) m = 1; }
    while (d < 1)             { if (--m < 1) m = 12; d += daysInMonth[m]; }
    colDay[i] = d;
    colMonth[i] = m;
  }
}

void updateHourRange() {
  if (eventCount == 0) return;

  int minHour = 23, maxHour = 0;
  for (int i = 0; i < eventCount; i++) {
    if (isAllDay(events[i])) continue;
    if (events[i].startHour < minHour) minHour = events[i].startHour;
    int endHour = (events[i].endMin > 0) ? events[i].endHour + 1 : events[i].endHour;
    if (endHour > maxHour) maxHour = endHour;
  }

  if (minHour > 10) minHour = 10;
  if (maxHour < 18) maxHour = 18;

  HOUR_START  = minHour;
  HOUR_END    = maxHour;
  TOTAL_HOURS = max(1, HOUR_END - HOUR_START);
  HOUR_H      = GRID_H / TOTAL_HOURS;
}

// ── Overlap layout ───────────────────────────────────────────────────────────
void computeLayout() {
  for (int i = 0; i < eventCount; i++) layout[i] = {0, 1};

  for (int d = 0; d < 7; d++) {
    int dayIdx[MAX_EVENTS], dayCount = 0;
    for (int i = 0; i < eventCount; i++) {
      if (events[i].dayOfWeek == d) dayIdx[dayCount++] = i;
    }
    if (dayCount == 0) continue;

    for (int a = 1; a < dayCount; a++) {
      int key = dayIdx[a];
      int startA = minutesOf(events[key].startHour, events[key].startMin);
      int b = a - 1;
      while (b >= 0 &&
             minutesOf(events[dayIdx[b]].startHour, events[dayIdx[b]].startMin) > startA) {
        dayIdx[b + 1] = dayIdx[b];
        b--;
      }
      dayIdx[b + 1] = key;
    }

    int laneEnd[MAX_LANES] = {-1, -1, -1};
    for (int a = 0; a < dayCount; a++) {
      int idx = dayIdx[a];
      if (isAllDay(events[idx])) continue;

      int startM = minutesOf(events[idx].startHour, events[idx].startMin);
      int endM   = minutesOf(events[idx].endHour,   events[idx].endMin);

      int lane = -1;
      for (int l = 0; l < MAX_LANES; l++) {
        if (laneEnd[l] <= startM) { lane = l; break; }
      }
      if (lane == -1) lane = MAX_LANES - 1;

      laneEnd[lane]    = endM;
      layout[idx].lane = lane;
    }

    for (int a = 0; a < dayCount; a++) {
      int idx = dayIdx[a];
      if (isAllDay(events[idx])) continue;

      int startA = minutesOf(events[idx].startHour, events[idx].startMin);
      int endA   = minutesOf(events[idx].endHour,   events[idx].endMin);
      int overlapCount = 1;

      for (int b = 0; b < dayCount; b++) {
        if (a == b) continue;
        int other = dayIdx[b];
        if (isAllDay(events[other])) continue;

        int startB = minutesOf(events[other].startHour, events[other].startMin);
        int endB   = minutesOf(events[other].endHour,   events[other].endMin);
        if (startB < endA && endB > startA) overlapCount++;
      }
      layout[idx].numLanes = min(overlapCount, MAX_LANES);
    }
  }
}

// ── Thai text rendering ────────────────────────────────────────────────────────
String getUTF8Char(const String& text, int& i) {
  if (i >= text.length()) return "";
  unsigned char c = text[i];
  int bytes = 1;
  if (c >= 0xF0)      bytes = 4;
  else if (c >= 0xE0) bytes = 3;
  else if (c >= 0xC0) bytes = 2;

  String res = text.substring(i, i + bytes);
  i += bytes;
  return res;
}

uint16_t getUnicode(const String& utf8Char) {
  if (utf8Char.length() < 3) {
    return (utf8Char.length() == 1) ? utf8Char[0] : 0;
  }
  return ((utf8Char[0] & 0x0F) << 12) | ((utf8Char[1] & 0x3F) << 6) | (utf8Char[2] & 0x3F);
}

bool isThaiVowelAbove(uint16_t code) {
  return (code == 0x0E31 || (code >= 0x0E34 && code <= 0x0E37) ||
          code == 0x0E47 || code == 0x0E4D);
}

bool isThaiVowelBelow(uint16_t code) {
  return (code >= 0x0E38 && code <= 0x0E3A);
}

bool isThaiToneMark(uint16_t code) {
  return (code >= 0x0E48 && code <= 0x0E4C);
}

bool isThaiCombining(uint16_t code) {
  return isThaiVowelAbove(code) || isThaiVowelBelow(code) || isThaiToneMark(code);
}

std::vector<ThaiCluster> parseToClusters(const String& text) {
  std::vector<ThaiCluster> clusters;
  int i = 0;
  while (i < text.length()) {
    String c = getUTF8Char(text, i);
    if (c == "") break;
    uint16_t code = getUnicode(c);

    if (isThaiCombining(code)) {
      if (clusters.empty()) {
        ThaiCluster fallback;
        fallback.base = " ";
        clusters.push_back(fallback);
      }
      if (isThaiVowelAbove(code))      clusters.back().vowelAbove = c;
      else if (isThaiVowelBelow(code)) clusters.back().vowelBelow = c;
      else if (isThaiToneMark(code))   clusters.back().tone = c;
    } else {
      ThaiCluster cl;
      cl.base = c;
      clusters.push_back(cl);
    }
  }

#ifdef EPAPER_ENABLE
  for (auto& cl : clusters) {
    cl.width = epaper.textWidth(cl.base);
  }
#endif
  return clusters;
}

void drawWrappedText(const String& text, int x, int y, int maxW, int maxH, uint16_t fg) {
#ifdef EPAPER_ENABLE
  int maxLines = maxH / FONT_LINE_H;
  if (maxLines < 1) return;

  std::vector<ThaiCluster> clusters = parseToClusters(text);
  int clCount = clusters.size();
  int pos  = 0;
  int line = 0;

  epaper.setTextColor(fg);

  while (pos < clCount && line < maxLines) {
    int currentW = 0;
    int endPos   = pos;

    while (endPos < clCount) {
      if (currentW + clusters[endPos].width > maxW) break;
      currentW += clusters[endPos].width;
      endPos++;
    }

    if (endPos == pos && clCount > 0) endPos = pos + 1;

    int drawX = x;
    for (int k = pos; k < endPos; k++) {
      ThaiCluster& cl = clusters[k];
      String clusterStr = cl.base;
      if (cl.vowelBelow != "") clusterStr += cl.vowelBelow;
      if (cl.vowelAbove != "") clusterStr += cl.vowelAbove;
      if (cl.tone != "")       clusterStr += cl.tone;

      epaper.drawString(clusterStr, drawX, y + line * FONT_LINE_H);
      drawX += cl.width;
    }

    pos = endPos;
    line++;
  }
#endif
}

// ── Network / display status ─────────────────────────────────────────────────
#ifdef EPAPER_ENABLE
void showStatus(const String& message) {
  epaper.fillScreen(TFT_WHITE);
  epaper.drawString(message, 10, 10);
  epaper.update();
}
#endif

void connectWifi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected!");
}

void syncTime() {
  configTime(TZ_OFFSET_H * 3600, 0, "pool.ntp.org");
  struct tm ntpCheck;
  int ntpRetry = 0;
  while (!getLocalTime(&ntpCheck) && ntpRetry++ < 20) delay(500);
  Serial.printf("NTP synced: %04d-%02d-%02d\n",
                ntpCheck.tm_year + 1900, ntpCheck.tm_mon + 1, ntpCheck.tm_mday);
}

// ── Setup / Loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

#ifdef EPAPER_ENABLE
  epaper.begin();
  showStatus("Connecting...");
#endif

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — Thai font unavailable");
  } else {
    Serial.println("LittleFS mounted OK");
  }

  connectWifi();
  syncTime();
  fetchAndDisplay();
}

void loop() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 1) {
      fetchAndDisplay();
      delay(60000);
    }
  }
  delay(30000);
}

// ── Fetch + parse ──────────────────────────────────────────────────────────────
bool fetchCalendarJson(String& json) {
  HTTPClient http;
  http.begin(scriptUrl);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "ESP32");

  int code = http.GET();
  Serial.println("HTTP: " + String(code));
  if (code != 200) {
#ifdef EPAPER_ENABLE
    showStatus("HTTP error: " + String(code));
#endif
    http.end();
    return false;
  }

  json = http.getString();
  http.end();
  return true;
}

void parseEvents(JsonArray calendar) {
  for (int i = 0; i < 7; i++) {
    colDay[i] = 0;
    colMonth[i] = 0;
  }
  todayDow = -1;
  eventCount = 0;

  for (JsonObject ev : calendar) {
    if (eventCount >= MAX_EVENTS) break;

    int sy, sm, sd, sh, smin;
    int ey, em, ed, eh, emin;
    parseISO(ev["start"].as<String>(), sy, sm, sd, sh, smin);
    parseISO(ev["end"].as<String>(),   ey, em, ed, eh, emin);

    int dow = getDow(sy, sm, sd);
    colDay[dow] = sd;
    colMonth[dow] = sm;

    events[eventCount++] = {
      sanitizeTitle(ev["title"].as<String>()),
      sh, smin, eh, emin, dow
    };
  }

  fillMissingWeekDates();
  updateHourRange();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    todayDow = getDow(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    Serial.println("Today dow: " + String(todayDow));
  }
}

void fetchAndDisplay() {
  Serial.println("Fetching calendar...");
#ifdef EPAPER_ENABLE
  showStatus("Fetching calendar...");
#endif

  String json;
  if (!fetchCalendarJson(json)) return;

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, json)) {
    Serial.println("JSON parse error");
    return;
  }

  parseEvents(doc.as<JsonArray>());
  computeLayout();
  drawGrid();
}

// ── Draw the calendar grid ─────────────────────────────────────────────────────
#ifdef EPAPER_ENABLE
void drawDayHeaders() {
  const char* dayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

  epaper.setTextSize(2);
  for (int d = 0; d < DAY_COLS; d++) {
    int x = dayToX(d);
    bool isToday = (d == todayDow);

    epaper.drawLine(x, 0, x, SCREEN_H, TFT_BLACK);

    if (isToday) {
      epaper.fillRect(x, 0, DAY_COL_W, HEADER_H, TFT_BLACK);
      epaper.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
      epaper.setTextColor(TFT_BLACK, TFT_WHITE);
    }

    epaper.drawString(dayNames[d], x + 4, 4);

    if (colDay[d] > 0) {
      epaper.setTextSize(1);
      epaper.drawString(pad2(colDay[d]) + "/" + pad2(colMonth[d]), x + 4, 26);
      epaper.setTextSize(2);
    }
  }

  epaper.drawLine(SCREEN_W - 1, 0, SCREEN_W - 1, SCREEN_H, TFT_BLACK);
  epaper.drawLine(0, HEADER_H, SCREEN_W, HEADER_H, TFT_BLACK);
}

void drawHourLabels() {
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.setTextSize(1);

  for (int h = HOUR_START; h <= HOUR_END; h++) {
    int y = timeToY(h, 0);
    if (y >= SCREEN_H) break;
    epaper.drawString(pad2(h), (TIME_COL_W - 12) / 2, y + 2);
    epaper.drawLine(TIME_COL_W, y, SCREEN_W, y, TFT_BLACK);
  }
  epaper.drawLine(TIME_COL_W, HEADER_H, TIME_COL_W, SCREEN_H, TFT_BLACK);
}

void drawEventBlocks() {
  for (int i = 0; i < eventCount; i++) {
    EventSlot slot = computeEventSlot(i);
    if (!slot.visible) continue;

    epaper.fillRect(slot.x, slot.y1, slot.slotW, slot.h, TFT_BLACK);
    epaper.drawRect(slot.x, slot.y1, slot.slotW, slot.h, TFT_WHITE);
  }
}

void drawEventTitles() {
  epaper.loadFont(Sarabun7);
  epaper.setTextSize(1);

  for (int i = 0; i < eventCount; i++) {
    EventSlot slot = computeEventSlot(i);
    if (!slot.visible || slot.h < FONT_LINE_H) continue;

    drawWrappedText(
      events[i].title,
      slot.x + EVENT_X_PAD,
      slot.y1 + EVENT_Y_PAD,
      slot.slotW - (EVENT_X_PAD * 2),
      slot.h - (EVENT_Y_PAD + EVENT_BOTTOM_PAD),
      TFT_WHITE
    );
  }
}
#endif

void drawGrid() {
#ifdef EPAPER_ENABLE
  epaper.fillScreen(TFT_WHITE);
  drawDayHeaders();
  drawHourLabels();
  drawEventBlocks();
  drawEventTitles();
  epaper.update();
  Serial.println("Grid drawn!");
#endif
}
