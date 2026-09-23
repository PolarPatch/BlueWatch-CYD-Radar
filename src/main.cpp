// BlueWatch display for the ESP32-3248S035C (320x480 portrait, capacitive touch).
// Fetches /api/display from BlueWatch and shows a scrollable device list with
// three toggles at the top: hide classified, grouped and unknown devices.

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "config.h"
#include "logo.h"
#include "ble_scan.h"

#define FW_VERSION "0.1.0"

// ---------------------------------------------------------------- hardware
//
// The display driver, touch driver and pin mapping live in a per-board
// header (see include/board_*.h), selected by a build flag in
// platformio.ini (-DBOARD_CYD_2432S028 or -DBOARD_CYD_3248S035C). Both
// headers provide the same three names: the `tft` object, boardTouchInit()
// and readTouchBoard(x, y). Both boards are 320 px wide, so only the
// height-dependent layout constants below differ between them.

// W/SCR_H come from the physical board plus, on the 2432S028, the
// SCREEN_LANDSCAPE choice in config.h (see board_2432s028.h) -- everything
// below is then sized off W and SCR_H rather than hardcoded per board, so
// any width/height combination lays out sensibly: a short screen (a
// landscape 2432S028) gets the compact single-line header and smaller
// buttons a tall one doesn't need room for, and the radar's radius fits
// whichever of width/height is the tighter fit.
#if defined(BOARD_CYD_2432S028)
#include "board_2432s028.h"
#if SCREEN_LANDSCAPE
static const int W = 320, SCR_H = 240;
#else
static const int W = 240, SCR_H = 320;
#endif
#else
#include "board_3248s035c.h"
static const int W = 320, SCR_H = 480;
#endif

#if SCR_H < 300
static const int HDR_H = 34;
static const int BTN_Y = 36;
static const int BTN_H = 22;
static const int LISTBOX_Y = 62;
static const int DAREA_Y = 30;
static const int RBAND_H = 30;
static const int HDR_LOGO_W = LOGO_SMALL_W, HDR_LOGO_H = LOGO_SMALL_H;
#define HDR_LOGO_DATA LOGO_SMALL_DATA
#else
static const int HDR_H = 62;
static const int BTN_Y = 64;
static const int BTN_H = 32;
static const int LISTBOX_Y = 100;
static const int DAREA_Y = 44;
static const int RBAND_H = 40;
static const int HDR_LOGO_W = LOGO_W, HDR_LOGO_H = LOGO_H;
#define HDR_LOGO_DATA LOGO_DATA
#endif
static const int LISTBOX_H = SCR_H - LISTBOX_Y;
static const int DAREA_H = SCR_H - DAREA_Y;
// Outer ring 1-2 px from whichever of width/height is the tighter fit.
static const float RADAR_MAXR = (min(W, DAREA_H) - 2) / 2.0f;

static LGFX_Sprite rowSpr(&tft);
static bool readTouch(int32_t* x, int32_t* y) { return readTouchBoard(x, y); }

// ---------------------------------------------------------------- layout

static const int ROW_H = 26;

static const uint16_t C_BG = 0x0882;      // near black (13,17,23)
static const uint16_t C_ROW_A = 0x0882;
static const uint16_t C_ROW_B = 0x10A4;   // slightly lighter
static const uint16_t C_TEXT = 0xE73C;    // light grey
static const uint16_t C_MUTED = 0x8C71;   // muted grey
static uint16_t colAccent, colAlert, colGold, colGood;

// ---------------------------------------------------------------- data

struct Row {
  char m[18];
  char n[29];
  char t[14];
  int16_t r;
  uint32_t a;
  uint8_t w, l;
};

static const int MAX_ROWS = 100;
static Row bufs[2][MAX_ROWS];
static int counts[2] = {0, 0};
static int totals[2] = {0, 0};
static volatile int frontIdx = 0;
static volatile uint32_t dataVersion = 0;
static volatile uint32_t lastOkMs = 0;
static volatile bool fetchFailed = false;
static volatile bool refreshNow = false;
static volatile bool localMode = false;   // true = showing the board's own BLE scan, not BlueWatch
static volatile int consecFails = 0;

struct Detail {
  char m[20], n[32], v[32], ty[24], g[24], nt[84], fs[16], ls[16], px[12];
  uint32_t sg;
  int16_t rs;
  uint8_t w;
  int8_t live[60];
  int8_t hmin[80], hmax[80];
};
static Detail dets[2];
static volatile int detFront = 0;
static volatile uint32_t detVersion = 0;
static volatile bool detailWanted = false;
static volatile bool detLoaded = false;
static char detailMac[20] = {0};

struct RDot {
  float ang, rad;
  uint16_t col;
  uint8_t alert, watched;
  int16_t rssi;
  char lab[17];
  int16_t lx, ly;  // label position on screen, valid when lab[0] != 0
};
static const int MAX_DOTS = 150;
static RDot rdots[2][MAX_DOTS];
static int rcounts[2] = {0, 0};
static volatile int rFront = 0;
static volatile uint32_t rVersion = 0;
static volatile bool radarWanted = false;

static bool hideClassified = true, hideGrouped = true, hideUnknown = true;
static bool hideRadarClassified = false, hideRadarGrouped = false, hideRadarUnknown = false, hideRadarApple = false;
static Preferences prefs;

static uint16_t typeColor(const char* t) {
  auto is = [&](const char* s) { return strcmp(t, s) == 0; };
  if (is("tracker")) return tft.color565(167, 139, 250);
  if (is("phone") || is("tablet")) return tft.color565(99, 102, 241);
  if (is("smart")) return tft.color565(94, 190, 170);
  if (is("vehicle")) return tft.color565(224, 122, 122);
  if (is("tv")) return tft.color565(214, 176, 74);
  if (is("camera")) return tft.color565(240, 110, 80);
  if (is("network")) return tft.color565(160, 160, 190);
  if (is("printer")) return tft.color565(170, 170, 180);
  if (is("watch") || is("wearable")) return tft.color565(120, 200, 120);
  if (is("audio") || is("speaker")) return tft.color565(90, 170, 230);
  if (is("flipper") || is("skimmer") || is("drone")) return tft.color565(255, 90, 90);
  return tft.color565(110, 115, 125);  // unknown and everything else
}

static void formatAge(uint32_t s, char* out, size_t n) {
  if (s < 10) snprintf(out, n, "now");
  else if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

// A short, honest label for the lite classifier's type keys -- distinct
// from typeColor()'s palette lookup, used as the Device Details subtitle
// in standalone mode (BlueWatch's own get_type_label() does this
// server-side; there's no equivalent data on the board, so this is a
// small fixed table covering the types classifyLite() can produce).
static const char* typeLabelLite(const char* t) {
  auto is = [&](const char* s) { return strcmp(t, s) == 0; };
  if (is("tracker")) return "Tracker";
  if (is("flipper")) return "Flipper Zero";
  if (is("wearable")) return "Wearable";
  if (is("audio")) return "Audio";
  if (is("drone")) return "Drone (Remote ID)";
  return "Unknown";
}

static const char* proximityLite(int16_t rssi) {
  if (rssi >= -50) return "close";
  if (rssi >= -70) return "near";
  if (rssi >= -85) return "medium";
  return "remote";
}

static uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (const char* c = s; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
  return h;
}

// Same angle rule as the server's radar and BlueWatch's own web /radar page
// (an FNV-1a hash of the MAC's text form), so a given device sits at the
// same spot whichever data source is behind it.
static float hashAngle(const char* mac) {
  return (fnv1a(mac) / 4294967296.0f) * 6.2831853f;
}

#ifdef DEMO_MODE
// Same idea (and the same name pool) as the web dashboard's own demo mode
// (?demo=1 -- see templates.py's DEMO_NAMES): every real name becomes a
// generic placeholder, picked from the MAC so the same device keeps the
// same fake name across the list, the radar and a refresh. For safe
// screenshots/photos -- nothing else about the data changes.
static const char* DEMO_NAMES[] = {
  "Guest Phone", "Kitchen Speaker", "Smart Plug", "Wireless Headset", "Fitness Tracker",
  "Smart TV", "Tablet", "Car Bluetooth", "IoT Sensor", "Robot Vacuum",
  "Doorbell Camera", "Smart Watch", "Bluetooth Mouse", "Game Controller", "E-bike Lock",
};
static const int DEMO_NAME_COUNT = sizeof(DEMO_NAMES) / sizeof(DEMO_NAMES[0]);
static const char* demoNameFor(const char* mac) { return DEMO_NAMES[fnv1a(mac) % DEMO_NAME_COUNT]; }
#endif

// ---------------------------------------------------------------- standalone (local BLE scan)
//
// Fills the exact same bufs[]/dets[]/rdots[] buffers the server path below
// fills, from the board's own scan table (see ble_scan.h) instead of a
// BlueWatch HTTP response -- so every drawing function downstream works
// unchanged no matter which source is behind the data. See the README for
// what standalone mode can't do that a real BlueWatch server can (no
// multi-day history, no vendor names, no categories or alerts).

static int bleCompareLastSeen(const void* a, const void* b) {
  const BleEntry* ea = (const BleEntry*)a;
  const BleEntry* eb = (const BleEntry*)b;
  return (int)(eb->lastSeenMs - ea->lastSeenMs);
}

static void buildListLocal() {
  static BleEntry snap[MAX_ROWS > 150 ? MAX_ROWS : 150];
  int n = bleScanSnapshot(snap, sizeof(snap) / sizeof(snap[0]));
  qsort(snap, n, sizeof(BleEntry), bleCompareLastSeen);

  int back = 1 - frontIdx;
  int out = 0;
  uint32_t now = millis();
  for (int i = 0; i < n && out < MAX_ROWS; i++) {
    const BleEntry& e = snap[i];
    bool unknown = e.name[0] == 0 && strcmp(e.type, "unknown") == 0;
    if (hideUnknown && unknown) continue;
    // hideClassified/hideGrouped have no local equivalent (no categories
    // without a server) -- both are no-ops in standalone mode.
    uint32_t ageSec = (now - e.lastSeenMs) / 1000;
    if (ageSec > (uint32_t)LIST_WINDOW_SECONDS) continue;
    Row& r = bufs[back][out++];
    macToStr(e.mac, r.m, sizeof(r.m));
    if (e.name[0]) strlcpy(r.n, e.name, sizeof(r.n));
    else snprintf(r.n, sizeof(r.n), "BLE %02X:%02X", e.mac[4], e.mac[5]);
#ifdef DEMO_MODE
    strlcpy(r.n, demoNameFor(r.m), sizeof(r.n));
#endif
    strlcpy(r.t, e.type, sizeof(r.t));
    r.r = e.rssi;
    r.a = ageSec;
    r.w = 0;
    r.l = 0;
  }
  counts[back] = out;
  totals[back] = out;
  frontIdx = back;
  dataVersion++;
  lastOkMs = millis();
  fetchFailed = false;
}

static void buildDetailLocal() {
  uint8_t mac[6];
  if (!strToMac(detailMac, mac)) return;
  BleEntry e;
  if (!bleScanFind(mac, &e)) return;  // not seen (yet, or evicted) -- leave the "loading" state

  int back = 1 - detFront;
  Detail& d = dets[back];
  memset(&d, 0, sizeof(d));
  strlcpy(d.m, detailMac, sizeof(d.m));
  strlcpy(d.n, e.name, sizeof(d.n));
#ifdef DEMO_MODE
  strlcpy(d.n, demoNameFor(detailMac), sizeof(d.n));
  strlcpy(d.m, "00:00:00:00:00:00", sizeof(d.m));
#endif
  strlcpy(d.ty, typeLabelLite(e.type), sizeof(d.ty));
  strlcpy(d.px, proximityLite(e.rssi), sizeof(d.px));
  uint32_t now = millis();
  char buf[16];
  formatAge((now - e.firstSeenMs) / 1000, buf, sizeof(buf));
  snprintf(d.fs, sizeof(d.fs), "%s ago", buf);
  formatAge((now - e.lastSeenMs) / 1000, buf, sizeof(buf));
  snprintf(d.ls, sizeof(d.ls), "%s ago", buf);
  d.sg = e.sightings;
  d.rs = e.rssi;
  d.w = 0;
  // The ring holds only the last few samples (no timed buckets like the
  // server's 15-minute grid) -- placed at the end of the 60-slot live
  // array so they read as "just now" on the chart; hist[] stays all zero,
  // there is no persistent history without a server, and the chart says so.
  int ringLen = sizeof(e.liveRing) / sizeof(e.liveRing[0]);
  for (int i = 0; i < ringLen; i++) {
    int8_t v = e.liveRing[(e.liveHead + i) % ringLen];
    if (v != 0) d.live[60 - ringLen + i] = v;
  }

  if (detailWanted && strcmp(d.m, detailMac) == 0) {
    detFront = back;
    detLoaded = true;
    detVersion++;
  }
}

static void fetchList() {
  String url = String("http://") + BW_HOST + ":" + BW_PORT + "/api/display?limit=100&active_within=" + String((unsigned long)LIST_WINDOW_SECONDS);
  if (hideClassified) url += "&hide_classified=1";
  if (hideGrouped) url += "&hide_grouped=1";
  if (hideUnknown) url += "&hide_nameless=1";
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      int back = 1 - frontIdx;
      int n = 0;
      for (JsonObject o : doc["d"].as<JsonArray>()) {
        if (n >= MAX_ROWS) break;
        Row& r = bufs[back][n++];
        strlcpy(r.m, o["m"] | "", sizeof(r.m));
        strlcpy(r.n, o["n"] | "?", sizeof(r.n));
#ifdef DEMO_MODE
        strlcpy(r.n, demoNameFor(r.m), sizeof(r.n));
#endif
        strlcpy(r.t, o["t"] | "unknown", sizeof(r.t));
        r.r = o["r"] | 0;
        r.a = o["a"] | 0;
        r.w = o["w"] | 0;
        r.l = o["l"] | 0;
      }
      counts[back] = n;
      totals[back] = doc["total"] | n;
      frontIdx = back;
      dataVersion++;
      lastOkMs = millis();
      ok = true;
    }
  }
  http.end();
  fetchFailed = !ok;
}

static void fetchDetail() {
  char mac[20];
  strlcpy(mac, detailMac, sizeof(mac));
  String url = String("http://") + BW_HOST + ":" + BW_PORT + "/api/display/device?mac=" + mac;
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      int back = 1 - detFront;
      Detail& d = dets[back];
      memset(&d, 0, sizeof(d));
      strlcpy(d.m, doc["m"] | "", sizeof(d.m));
      strlcpy(d.n, doc["n"] | "", sizeof(d.n));
#ifdef DEMO_MODE
      strlcpy(d.n, demoNameFor(d.m), sizeof(d.n));
      strlcpy(d.m, "00:00:00:00:00:00", sizeof(d.m));
#endif
      strlcpy(d.v, doc["v"] | "", sizeof(d.v));
      strlcpy(d.ty, doc["ty"] | "", sizeof(d.ty));
      strlcpy(d.g, doc["g"] | "", sizeof(d.g));
      strlcpy(d.nt, doc["nt"] | "", sizeof(d.nt));
      strlcpy(d.fs, doc["fs"] | "", sizeof(d.fs));
      strlcpy(d.ls, doc["ls"] | "", sizeof(d.ls));
      strlcpy(d.px, doc["px"] | "", sizeof(d.px));
      d.sg = doc["sg"] | 0;
      d.rs = doc["rs"] | 0;
      d.w = doc["w"] | 0;
      int i = 0;
      for (JsonVariant v : doc["live"].as<JsonArray>()) { if (i >= 60) break; d.live[i++] = v.as<int>(); }
      i = 0;
      for (JsonVariant v : doc["hist"].as<JsonArray>()) {
        if (i >= 80) break;
        JsonArray p = v.as<JsonArray>();
        d.hmin[i] = p[0] | 0;
        d.hmax[i] = p[1] | 0;
        i++;
      }
      if (detailWanted && strcmp(d.m, detailMac) == 0) {
        detFront = back;
        detLoaded = true;
        detVersion++;
      }
      ok = true;
    }
  }
  http.end();
  fetchFailed = !ok;
}

// Picks up to 16 devices to label (alerts, watched and strong signals first) and
// places each label next to its dot without overlapping the ones already placed.
static void placeLabels(RDot* dots, int n) {
  const int cx = W / 2, cy = DAREA_Y + DAREA_H / 2;
  int order[MAX_DOTS];
  for (int i = 0; i < n; i++) order[i] = i;
  auto score = [&](int i) { return (dots[i].alert ? 2000 : 0) + (dots[i].watched ? 1000 : 0) + dots[i].rssi; };
  for (int i = 1; i < n; i++) {  // insertion sort, best first
    int k = order[i], j = i - 1;
    while (j >= 0 && score(order[j]) < score(k)) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }
  struct Box { int x, y, w, h; };
  Box placed[16];
  int np = 0;
  for (int oi = 0; oi < n; oi++) {
    RDot& d = dots[order[oi]];
    if (np >= 16 || !d.lab[0]) { if (np >= 16) { d.lab[0] = 0; continue; } else continue; }
    int w = 6 * (int)strlen(d.lab);
    int dx = cx + (int)(cosf(d.ang) * d.rad), dy = cy + (int)(sinf(d.ang) * d.rad);
    int tries[2][2] = {{dx + 6, dy - 8}, {dx - 6 - w, dy - 8}};  // right of the dot, then left
    bool done = false;
    for (int t = 0; t < 2 && !done; t++) {
      int x = tries[t][0], y = tries[t][1];
      if (x < 2 || x + w > W - 2 || y < DAREA_Y + 2 || y + 17 > 478) continue;
      bool clash = false;
      for (int k = 0; k < np; k++)
        if (x < placed[k].x + placed[k].w + 2 && x + w + 2 > placed[k].x && y < placed[k].y + placed[k].h + 1 && y + 17 + 1 > placed[k].y) { clash = true; break; }
      if (clash) continue;
      d.lx = x; d.ly = y;
      placed[np++] = {x, y, w, 17};
      done = true;
    }
    if (!done) d.lab[0] = 0;
  }
  for (int i = 0; i < n; i++) {  // devices that never got a slot show no label
    bool has = false;
    for (int k = 0; k < np && !has; k++) has = (placed[k].x == dots[i].lx && placed[k].y == dots[i].ly && dots[i].lab[0]);
    if (!has) dots[i].lab[0] = 0;
  }
}

static void buildRadarLocal() {
  static BleEntry snap[MAX_DOTS];
  int n = bleScanSnapshot(snap, MAX_DOTS);

  int back = 1 - rFront;
  int out = 0;
  for (int i = 0; i < n; i++) {
    const BleEntry& e = snap[i];
    // hideRadarGrouped has no local equivalent (no categories without a
    // server) and is always a no-op in standalone mode.
    if (hideRadarClassified && strcmp(e.type, "unknown") != 0) continue;
    if (hideRadarUnknown && e.name[0] == 0 && strcmp(e.type, "unknown") == 0) continue;
    if (hideRadarApple && e.isApple) continue;
    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    float t = (-30.0f - (float)e.rssi) / 70.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    RDot& d = rdots[back][out++];
    d.ang = hashAngle(macStr);
    d.rad = RADAR_MAXR * (0.12f + t * 0.88f);
    d.col = typeColor(e.type);
    d.alert = 0;
    d.watched = 0;
    d.rssi = e.rssi;
    strlcpy(d.lab, e.name, sizeof(d.lab));
#ifdef DEMO_MODE
    strlcpy(d.lab, demoNameFor(macStr), sizeof(d.lab));
#endif
    d.lx = d.ly = 0;
  }
  placeLabels(rdots[back], out);
  rcounts[back] = out;
  rFront = back;
  rVersion++;
}

static void fetchRadar() {
  String url = String("http://") + BW_HOST + ":" + BW_PORT + "/api/display/radar?window=300";
  if (hideRadarClassified) url += "&hide_classified=1";
  if (hideRadarGrouped) url += "&hide_grouped=1";
  if (hideRadarUnknown) url += "&hide_nameless=1";
  if (hideRadarApple) url += "&hide_apple=1";
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      int back = 1 - rFront;
      int n = 0;
      for (JsonObject o : doc["d"].as<JsonArray>()) {
        if (n >= MAX_DOTS) break;
        const char* mac = o["m"] | "";
        uint32_t h = 2166136261u;  // same hash as the web radar, so dots sit at the same angle
        for (const char* c = mac; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
        float t = ((-30.0f - (float)(o["r"] | -100)) / 70.0f);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        RDot& d = rdots[back][n++];
        d.ang = (h / 4294967296.0f) * 6.2831853f;
        d.rad = RADAR_MAXR * (0.12f + t * 0.88f);
        d.col = typeColor(o["t"] | "unknown");
        d.alert = o["l"] | 0;
        d.watched = o["w"] | 0;
        d.rssi = o["r"] | -100;
        strlcpy(d.lab, o["n"] | "", sizeof(d.lab));
#ifdef DEMO_MODE
        strlcpy(d.lab, demoNameFor(mac), sizeof(d.lab));
#endif
        d.lx = d.ly = 0;
      }
      placeLabels(rdots[back], n);
      rcounts[back] = n;
      rFront = back;
      rVersion++;
      ok = true;
    }
  }
  http.end();
  fetchFailed = !ok;
}

// Runs on core 0: keeps Wi-Fi up and fetches data, never blocks the UI.
static void fetchTask(void*) {
  // Leave WIFI_SSID empty in config.h to skip Wi-Fi/BlueWatch entirely and
  // run standalone-only from boot -- no connection attempts, no retries.
#ifdef FORCE_STANDALONE
  // Test build (see the *-standalone-test env in platformio.ini): never
  // touch Wi-Fi or BlueWatch, only the board's own BLE scan, regardless of
  // what config.h says.
  bool haveWifi = false;
#else
  bool haveWifi = WIFI_SSID[0] != 0;
#endif
  if (haveWifi) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  } else {
    localMode = true;
  }
  for (;;) {
    if (haveWifi && WiFi.status() == WL_CONNECTED) {
      if (radarWanted) fetchRadar();
      else if (detailWanted) fetchDetail();
      else fetchList();
    } else if (haveWifi) {
      fetchFailed = true;
    }
    if (haveWifi) {
      // A few misses in a row (server down, wrong BW_HOST, first boot
      // before BlueWatch starts) fall back to the board's own scan rather
      // than sitting on a stale or empty screen -- and a later success
      // switches straight back, so this recovers on its own either way.
      if (fetchFailed) { consecFails++; if (consecFails >= 3) localMode = true; }
      else { consecFails = 0; localMode = false; }
    }
    if (localMode) {
      if (radarWanted) buildRadarLocal();
      else if (detailWanted) buildDetailLocal();
      else buildListLocal();
    }
    for (int i = 0; i < REFRESH_MS / 100 && !refreshNow; i++) vTaskDelay(pdMS_TO_TICKS(100));
    refreshNow = false;
  }
}

// ---------------------------------------------------------------- drawing

static float scrollY = 0, velY = 0;

// Status text: what the header's second (or right-hand, compact layout)
// line says, and its colour -- shared between the compact and roomy header
// layouts below.
static void headerStatus(char* line, size_t lineSz, uint16_t* color) {
  if (WIFI_SSID[0] != 0 && WiFi.status() != WL_CONNECTED) {
    snprintf(line, lineSz, "connecting to Wi-Fi...");
    *color = colGold;
  } else if (localMode) {
    snprintf(line, lineSz, "%d devices (local scan)", bleScanTotal());
    *color = colGold;
  } else if (lastOkMs == 0) {
    snprintf(line, lineSz, "loading...");
    *color = C_MUTED;
  } else if (fetchFailed) {
    snprintf(line, lineSz, "no answer from BlueWatch");
    *color = colAlert;
  } else {
    snprintf(line, lineSz, "%d devices", totals[frontIdx]);
    *color = C_MUTED;
  }
}

static void drawHeader() {
  tft.fillRect(0, 0, W, HDR_H, C_BG);
  tft.pushImage(4, (HDR_H - HDR_LOGO_H) / 2, HDR_LOGO_W, HDR_LOGO_H, (const lgfx::rgb565_t*)HDR_LOGO_DATA);
  char line[48];
  uint16_t color;
  headerStatus(line, sizeof(line), &color);

  if (HDR_H < 50) {
    // Compact single-line header for the smaller/shorter board: logo,
    // "BlueWatch", status right-aligned, all on one row.
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(middle_left);
    tft.setTextColor(colAccent, C_BG);
    tft.drawString("Blue", HDR_LOGO_W + 8, HDR_H / 2);
    int bw = tft.textWidth("Blue");
    tft.setTextColor(TFT_WHITE, C_BG);
    tft.drawString("Watch", HDR_LOGO_W + 8 + bw, HDR_H / 2);
    tft.setTextDatum(middle_right);
    tft.setTextColor(color, C_BG);
    tft.drawString(line, W - 4, HDR_H / 2);
    return;
  }

  tft.setFont(&fonts::Font4);
  tft.setTextDatum(top_left);
  tft.setTextColor(colAccent, C_BG);
  tft.drawString("Blue", 68, 6);
  int bw = tft.textWidth("Blue");
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("Watch", 68 + bw, 6);

  tft.setFont(&fonts::Font2);
  tft.setTextColor(color, C_BG);
  tft.drawString(line, 70, 38);
}

static const int LBTN_X0 = 42, LBTN_GAP = 4;
static const int LBTN_W = (W - LBTN_X0 - 4 - 2 * LBTN_GAP) / 3;
static void listBtnRect(int i, int* x, int* w) {
  *x = LBTN_X0 + i * (LBTN_W + LBTN_GAP);
  *w = LBTN_W;
}

static void drawButtons() {
  tft.fillRect(0, BTN_Y, W, BTN_H + 4, C_BG);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(middle_left);
  tft.drawString("Hide", 6, BTN_Y + BTN_H / 2);
  const char* labels[3] = {"classified", "grouped", "unknowns"};
  bool on[3] = {hideClassified, hideGrouped, hideUnknown};
  if (W < 300) tft.setFont(&fonts::Font0);  // narrower board: the full words need a smaller font to fit
  for (int i = 0; i < 3; i++) {
    int x, bw;
    listBtnRect(i, &x, &bw);
    if (on[i]) {
      tft.fillRoundRect(x, BTN_Y, bw, BTN_H, 6, colAccent);
      tft.setTextColor(TFT_WHITE, colAccent);
    } else {
      tft.fillRoundRect(x, BTN_Y, bw, BTN_H, 6, C_ROW_B);
      tft.drawRoundRect(x, BTN_Y, bw, BTN_H, 6, C_MUTED);
      tft.setTextColor(C_MUTED, C_ROW_B);
    }
    tft.setTextDatum(middle_center);
    tft.drawString(labels[i], x + bw / 2, BTN_Y + BTN_H / 2);
  }
}

static void renderRow(const Row* r, int idx) {
  uint16_t bg = (idx & 1) ? C_ROW_B : C_ROW_A;
  rowSpr.fillSprite(bg);
  if (!r) return;
  rowSpr.setFont(&fonts::Font2);
  rowSpr.setTextDatum(middle_left);

  uint16_t dot = typeColor(r->t);
  rowSpr.fillCircle(13, ROW_H / 2, 5, dot);
  if (r->w) rowSpr.drawCircle(13, ROW_H / 2, 8, colGold);

  // name, truncated to fit before the age column
  char name[32];
  strlcpy(name, r->n, sizeof(name));
  const int maxW = W - 124;
  rowSpr.setTextColor(r->l ? colAlert : C_TEXT, bg);
  while (strlen(name) > 1 && rowSpr.textWidth(name) > maxW) name[strlen(name) - 1] = 0;
  rowSpr.drawString(name, 28, ROW_H / 2);

  char age[8];
  formatAge(r->a, age, sizeof(age));
  rowSpr.setTextDatum(middle_right);
  rowSpr.setTextColor(C_MUTED, bg);
  rowSpr.drawString(age, W - 50, ROW_H / 2);

  char rssi[8];
  snprintf(rssi, sizeof(rssi), "%d", r->r);
  uint16_t rc = r->r >= -60 ? colGood : (r->r >= -80 ? C_TEXT : C_MUTED);
  rowSpr.setTextColor(rc, bg);
  rowSpr.drawString(rssi, W - 6, ROW_H / 2);
}

static void drawList() {
  int f = frontIdx;
  int n = counts[f];
  float maxScroll = max(0, n * ROW_H - LISTBOX_H + 4);
  if (scrollY < 0) { scrollY = 0; velY = 0; }
  if (scrollY > maxScroll) { scrollY = maxScroll; velY = 0; }

  int first = (int)(scrollY / ROW_H);
  float off = scrollY - first * ROW_H;
  tft.setClipRect(0, LISTBOX_Y, W, LISTBOX_H);
  for (int i = 0; i <= LISTBOX_H / ROW_H + 1; i++) {
    int y = LISTBOX_Y - (int)off + i * ROW_H;
    if (y >= LISTBOX_Y + LISTBOX_H) break;
    int idx = first + i;
    renderRow(idx < n ? &bufs[f][idx] : nullptr, idx);
    rowSpr.pushSprite(0, y);
  }
  tft.clearClipRect();
}

// ---------------------------------------------------------------- device details

static const int DCONTENT_H = 640;
static float dScroll = 0, dVel = 0;
static bool detailMode = false;
static const uint16_t C_GRID = 0x2965;

static int dbmY(int top, int h, int dbm) {
  float t = (-30.0f - dbm) / 70.0f;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return top + 6 + (int)(t * (h - 12));
}

static void chartBox(LGFX_Sprite& sp, int yo, int top, int h) {
  sp.fillRoundRect(10, top - yo, W - 20, h, 6, C_ROW_B);
  sp.setFont(&fonts::Font0);
  sp.setTextColor(C_MUTED, C_ROW_B);
  sp.setTextDatum(middle_left);
  const int levels[3] = {-40, -60, -80};
  for (int i = 0; i < 3; i++) {
    int y = dbmY(top, h, levels[i]) - yo;
    sp.drawFastHLine(34, y, W - 52, C_GRID);
    char lb[8];
    snprintf(lb, sizeof(lb), "%d", levels[i]);
    sp.drawString(lb, 14, y);
  }
}

// Draws the whole detail card into a sprite whose top edge is content row `yo`.
static void drawDetailContent(LGFX_Sprite& sp, int yo, const Detail& d) {
  sp.fillSprite(C_BG);
  sp.setTextDatum(top_left);

  // title and subtitle
  sp.setFont(&fonts::Font4);
  sp.setTextColor(C_TEXT, C_BG);
  char title[40];
  strlcpy(title, d.n[0] ? d.n : (d.v[0] ? d.v : d.m), sizeof(title));
  while (strlen(title) > 1 && sp.textWidth(title) > W - 20) title[strlen(title) - 1] = 0;
  sp.drawString(title, 10, 8 - yo);
  sp.setFont(&fonts::Font2);
  sp.setTextColor(C_MUTED, C_BG);
  char sub[64];
  snprintf(sub, sizeof(sub), "%s%s%s", d.ty, d.g[0] ? "  -  " : "", d.g);
  sp.drawString(sub, 10, 40 - yo);

  // fields
  char rs[16], sg[16], nt[84];
  snprintf(rs, sizeof(rs), d.rs ? "%d dBm" : "-", d.rs);
  snprintf(sg, sizeof(sg), "%u", (unsigned)d.sg);
  strlcpy(nt, d.nt[0] ? d.nt : "-", sizeof(nt));
  const char* labels[9] = {"Address", "Vendor", "Proximity", "Signal", "Sightings", "First seen", "Last seen", "Watched", "Notes"};
  const char* values[9] = {d.m, d.v[0] ? d.v : "-", d.px, rs, sg, d.fs, d.ls, d.w ? "yes" : "no", nt};
  for (int i = 0; i < 9; i++) {
    int y = 70 + i * 28 - yo;
    if (y > ROW_H || y < -28) continue;
    sp.fillRect(0, y, W, 28, (i & 1) ? C_ROW_B : C_BG);
    sp.setTextDatum(top_left);
    sp.setTextColor(C_MUTED, (i & 1) ? C_ROW_B : C_BG);
    sp.drawString(labels[i], 10, y + 6);
    sp.setTextDatum(top_right);
    sp.setTextColor(C_TEXT, (i & 1) ? C_ROW_B : C_BG);
    char v[40];
    strlcpy(v, values[i], sizeof(v));
    while (strlen(v) > 1 && sp.textWidth(v) > W - 130) v[strlen(v) - 1] = 0;
    sp.drawString(v, W - 10, y + 6);
  }

  // Live Signal
  sp.setTextDatum(top_left);
  sp.setFont(&fonts::Font2);
  sp.setTextColor(C_MUTED, C_BG);
  sp.drawString("LIVE SIGNAL (15 min)", 10, 334 - yo);
  const int lTop = 358, cH = 110;
  chartBox(sp, yo, lTop, cH);
  int prevX = -1, prevY = 0, lastX = -1, lastY = 0;
  bool any = false;
  for (int i = 0; i < 60; i++) {
    if (d.live[i] == 0) { prevX = -1; continue; }
    any = true;
    int x = 36 + (int)(i * (W - 56) / 59.0f), y = dbmY(lTop, cH, d.live[i]);
    if (prevX >= 0) sp.drawLine(prevX, prevY - yo, x, y - yo, C_TEXT);
    sp.fillCircle(x, y - yo, 1, C_TEXT);
    prevX = x; prevY = y; lastX = x; lastY = y;
  }
  if (any) sp.fillCircle(lastX, lastY - yo, 3, colAccent);
  else {
    sp.setTextDatum(middle_center);
    sp.setTextColor(C_MUTED, C_ROW_B);
    sp.drawString("no signal in the last 15 min", W / 2, lTop + cH / 2 - yo);
  }

  // Signal History
  sp.setTextDatum(top_left);
  sp.setTextColor(C_MUTED, C_BG);
  sp.drawString("SIGNAL HISTORY (7d)", 10, 484 - yo);
  const int hTop = 508;
  chartBox(sp, yo, hTop, cH);
  bool hAny = false;
  int hn = 0;
  while (hn < 80 && d.hmax[hn] != 0) hn++;
  int px = -1, py = 0;
  for (int i = 0; i < hn; i++) {
    hAny = true;
    int x = hn == 1 ? W / 2 : 36 + (int)(i * (W - 56) / (float)(hn - 1));
    int y1 = dbmY(hTop, cH, d.hmax[i]), y2 = dbmY(hTop, cH, d.hmin[i]);
    int ym = (y1 + y2) / 2;
    if (px >= 0) sp.drawLine(px, py - yo, x, ym - yo, C_TEXT);
    sp.fillRect(x - 1, y1 - yo, 3, max(3, y2 - y1 + 1), C_TEXT);
    px = x; py = ym;
  }
  if (!hAny) {
    sp.setTextDatum(middle_center);
    sp.setTextColor(C_MUTED, C_ROW_B);
    sp.drawString("not enough data", W / 2, hTop + cH / 2 - yo);
  }
}

static void drawDetailBar() {
  tft.fillRect(0, 0, W, DAREA_Y, C_BG);
  tft.fillRoundRect(6, 6, 76, 32, 6, C_ROW_B);
  tft.drawRoundRect(6, 6, 76, 32, 6, C_MUTED);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_TEXT, C_ROW_B);
  tft.drawString("< Back", 44, 22);
  tft.setTextDatum(middle_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Device details", 96, 22);
}

static void drawDetail() {
  if (!detLoaded) {
    tft.fillRect(0, DAREA_Y, W, DAREA_H, C_BG);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(middle_center);
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(WiFi.status() == WL_CONNECTED ? "loading..." : "no Wi-Fi", W / 2, DAREA_Y + DAREA_H / 2);
    return;
  }
  const Detail& d = dets[detFront];
  float maxScroll = max(0, DCONTENT_H - DAREA_H);
  if (dScroll < 0) { dScroll = 0; dVel = 0; }
  if (dScroll > maxScroll) { dScroll = maxScroll; dVel = 0; }
  int first = (int)(dScroll / ROW_H);
  float off = dScroll - first * ROW_H;
  tft.setClipRect(0, DAREA_Y, W, DAREA_H);
  for (int i = 0; i <= DAREA_H / ROW_H + 1; i++) {
    int y = DAREA_Y - (int)off + i * ROW_H;
    if (y >= DAREA_Y + DAREA_H) break;
    drawDetailContent(rowSpr, (first + i) * ROW_H, d);
    rowSpr.pushSprite(0, y);
  }
  tft.clearClipRect();
}

// ---------------------------------------------------------------- radar

static LGFX_Sprite radSpr(&tft);
static bool radarMode = false;

static uint16_t mix565(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) | (((int)(ag + (bg - ag) * t)) << 5) | (int)(ab + (bb - ab) * t));
}

// Back button + four filter toggles (Cls/Grp/Unk/Apl), all in the one bar
// above the radar -- there's no room here for both this and a device
// count, and the toggles are more useful to have visible at a glance.
static const int RBACK_W = HDR_H < 50 ? 40 : 68;
static const int RBAR_BTN_H = DAREA_Y - 6;
static const int RCHIP_X0 = RBACK_W + 8;
static const int RCHIP_GAP = 3;
static const int RCHIP_W = (W - RCHIP_X0 - 4 - 3 * RCHIP_GAP) / 4;

static void radarChipRect(int i, int* x, int* w) {
  *x = RCHIP_X0 + i * (RCHIP_W + RCHIP_GAP);
  *w = RCHIP_W;
}

static void drawRadarBar() {
  tft.fillRect(0, 0, W, DAREA_Y, C_BG);
  tft.fillRoundRect(4, 3, RBACK_W, RBAR_BTN_H, 5, C_ROW_B);
  tft.drawRoundRect(4, 3, RBACK_W, RBAR_BTN_H, 5, C_MUTED);
  if (HDR_H < 50) tft.setFont(&fonts::Font0);
  else tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_TEXT, C_ROW_B);
  tft.drawString("< Back", 4 + RBACK_W / 2, 3 + RBAR_BTN_H / 2);

  const char* labels[4] = {"Cls", "Grp", "Unk", "Apl"};
  bool on[4] = {hideRadarClassified, hideRadarGrouped, hideRadarUnknown, hideRadarApple};
  for (int i = 0; i < 4; i++) {
    int x, w;
    radarChipRect(i, &x, &w);
    if (on[i]) {
      tft.fillRoundRect(x, 3, w, RBAR_BTN_H, 5, colAccent);
      tft.setTextColor(TFT_WHITE, colAccent);
    } else {
      tft.fillRoundRect(x, 3, w, RBAR_BTN_H, 5, C_ROW_B);
      tft.drawRoundRect(x, 3, w, RBAR_BTN_H, 5, C_MUTED);
      tft.setTextColor(C_MUTED, C_ROW_B);
    }
    tft.drawString(labels[i], x + w / 2, 3 + RBAR_BTN_H / 2);
  }
}

static void drawRadarBand(int y0, float sweep) {
  const int cx = W / 2, cy = DAREA_Y + DAREA_H / 2;
  const uint16_t green = tft.color565(74, 222, 128);
  radSpr.fillSprite(C_BG);

  // rings at -40, -60, -80, -100 dBm and the cross-hair
  const int levels[4] = {-40, -60, -80, -100};
  for (int i = 0; i < 4; i++) {
    float t = (-30.0f - levels[i]) / 70.0f;
    int r = (int)(RADAR_MAXR * (0.12f + t * 0.88f));
    radSpr.drawCircle(cx, cy - y0, r, C_GRID);
    int ly = cy - r - y0 - 9;
    if (ly > -9 && ly < RBAND_H) {
      radSpr.setFont(&fonts::Font0);
      radSpr.setTextDatum(top_left);
      radSpr.setTextColor(C_MUTED, C_BG);
      char lb[6];
      snprintf(lb, sizeof(lb), "%d", levels[i]);
      radSpr.drawString(lb, cx + 3, ly);
    }
  }
  const int mr = (int)RADAR_MAXR;
  radSpr.drawFastHLine(cx - mr, cy - y0, 2 * mr + 1, C_GRID);
  radSpr.drawFastVLine(cx, cy - mr - y0, 2 * mr + 1, C_GRID);

  // sweep beam: a striped wedge trailing the bright leading edge
  const int stripes = 12;
  const float wedge = 0.64f, pitch = wedge / stripes;
  for (int i = 0; i < stripes; i++) {
    float t = (float)i / (stripes - 1);
    float a1 = sweep - i * pitch, a0 = a1 - pitch * 0.9f;
    uint16_t c = mix565(C_BG, green, 0.06f + 0.7f * powf(1.0f - t, 1.2f));
    radSpr.fillTriangle(cx, cy - y0,
                        cx + (int)(cosf(a0) * RADAR_MAXR), cy - y0 + (int)(sinf(a0) * RADAR_MAXR),
                        cx + (int)(cosf(a1) * RADAR_MAXR), cy - y0 + (int)(sinf(a1) * RADAR_MAXR), c);
  }
  radSpr.drawLine(cx, cy - y0, cx + (int)(cosf(sweep) * RADAR_MAXR), cy - y0 + (int)(sinf(sweep) * RADAR_MAXR), green);

  // devices: bright right after the beam passes, then fading (phosphor afterglow)
  int f = rFront;
  for (int i = 0; i < rcounts[f]; i++) {
    const RDot& d = rdots[f][i];
    int x = cx + (int)(cosf(d.ang) * d.rad);
    int y = cy + (int)(sinf(d.ang) * d.rad) - y0;
    if (y < -12 || y > RBAND_H + 12) continue;
    float behind = fmodf(sweep - d.ang + 12.566371f, 6.2831853f);
    float glow = expf(-behind / 2.4f);
    uint16_t c = mix565(C_BG, d.col, 0.28f + 0.72f * glow);
    radSpr.fillCircle(x, y, glow > 0.85f ? 3 : 2, c);
    if (behind < 0.7f) radSpr.drawCircle(x, y, 5 + (int)(behind * 14), mix565(C_BG, d.col, 0.6f * (1.0f - behind / 0.7f)));
    if (d.alert) radSpr.drawCircle(x, y, 5, colAlert);
  }
  // labels: name and signal strength, small
  radSpr.setFont(&fonts::Font0);
  radSpr.setTextDatum(top_left);
  for (int i = 0; i < rcounts[f]; i++) {
    const RDot& d = rdots[f][i];
    if (!d.lab[0]) continue;
    int ty = d.ly - y0;
    if (ty < -17 || ty > RBAND_H) continue;
    float behind = fmodf(sweep - d.ang + 12.566371f, 6.2831853f);
    float glow = expf(-behind / 2.4f);
    radSpr.setTextColor(d.alert ? colAlert : mix565(C_BG, C_TEXT, 0.55f + 0.45f * glow), C_BG);
    radSpr.drawString(d.lab, d.lx, ty);
    char db[8];
    snprintf(db, sizeof(db), "%d dBm", d.rssi);
    radSpr.setTextColor(mix565(C_BG, C_MUTED, 0.7f + 0.3f * glow), C_BG);
    radSpr.drawString(db, d.lx, ty + 9);
  }
}

static void drawRadar() {
  float sweep = fmodf(millis() / 1000.0f * RADAR_SPEED, 6.2831853f);
  tft.setClipRect(0, DAREA_Y, W, DAREA_H);
  for (int y0 = DAREA_Y; y0 < DAREA_Y + DAREA_H; y0 += RBAND_H) {
    drawRadarBand(y0, sweep);
    radSpr.pushSprite(0, y0);
  }
  tft.clearClipRect();
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  Serial.printf("BlueWatch CYD Radar v%s\n", FW_VERSION);
  prefs.begin("bwdisp", false);
  hideClassified = prefs.getBool("hc", true);
  hideGrouped = prefs.getBool("hg", true);
  hideUnknown = prefs.getBool("hu", true);
  hideRadarClassified = prefs.getBool("rc", false);
  hideRadarGrouped = prefs.getBool("rg", false);
  hideRadarUnknown = prefs.getBool("ru", false);
  hideRadarApple = prefs.getBool("ra", false);

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setBrightness(200);
  tft.fillScreen(C_BG);
  colAccent = tft.color565(88, 101, 242);
  colAlert = tft.color565(248, 81, 73);
  colGold = tft.color565(230, 170, 60);
  colGood = tft.color565(63, 185, 80);

  boardTouchInit();

  rowSpr.setColorDepth(16);
  rowSpr.createSprite(W, ROW_H);
  radSpr.setColorDepth(16);
  radSpr.createSprite(W, RBAND_H);

  drawHeader();
  drawButtons();
  drawList();

  bleScanStart();

  // Core 0 already carries the Wi-Fi driver and NimBLE's host/controller
  // tasks -- adding this one too was enough to starve its idle task and
  // trip the watchdog in a BLE-dense area. Core 1 (where loop() runs) has
  // headroom to spare.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 1);
}

void loop() {
  static bool wasTouch = false, moved = false, longFired = false;
  static int32_t startX = 0, startY = 0, lastY = 0;
  static uint32_t seenVersion = 0, seenDetVersion = 0, lastTick = 0, touchStartMs = 0;
  static bool dirty = true;
  static int lastState = -1, lastTotal = -1;

  int32_t x, y;
  bool touching = readTouch(&x, &y);
  float& sc = detailMode ? dScroll : scrollY;
  float& vel = detailMode ? dVel : velY;
  int areaY = detailMode ? DAREA_Y : LISTBOX_Y;

  if (touching) {
    if (!wasTouch) {
      startX = x; startY = y; lastY = y; moved = false; longFired = false;
      touchStartMs = millis(); vel = 0;
    } else {
      int dy = y - lastY;
      if (abs(y - startY) > 8) moved = true;
      if (moved && startY >= areaY && !radarMode) {
        sc -= dy;
        vel = -dy;
        dirty = true;
      }
      lastY = y;
      // long press on a row opens its details
      if (!detailMode && !moved && !longFired && startY >= LISTBOX_Y && millis() - touchStartMs > 600) {
        longFired = true;
        int f = frontIdx;
        int idx = (int)((scrollY + startY - LISTBOX_Y) / ROW_H);
        if (idx >= 0 && idx < counts[f] && bufs[f][idx].m[0]) {
          strlcpy(detailMac, bufs[f][idx].m, sizeof(detailMac));
          detLoaded = false;
          detailWanted = true;
          detailMode = true;
          dScroll = 0; dVel = 0;
          refreshNow = true;
          drawDetailBar();
          dirty = true;
        }
      }
    }
  } else if (wasTouch && !moved && !longFired) {
    if (radarMode) {
      if (startY < DAREA_Y && startX < RBACK_W + 8) {  // Back
        radarMode = false;
        radarWanted = false;
        refreshNow = true;
        lastState = -1;
        drawHeader();
        drawButtons();
        dirty = true;
      } else if (startY < DAREA_Y) {
        for (int i = 0; i < 4; i++) {
          int x, w;
          radarChipRect(i, &x, &w);
          if (startX >= x && startX < x + w) {
            if (i == 0) { hideRadarClassified = !hideRadarClassified; prefs.putBool("rc", hideRadarClassified); }
            if (i == 1) { hideRadarGrouped = !hideRadarGrouped; prefs.putBool("rg", hideRadarGrouped); }
            if (i == 2) { hideRadarUnknown = !hideRadarUnknown; prefs.putBool("ru", hideRadarUnknown); }
            if (i == 3) { hideRadarApple = !hideRadarApple; prefs.putBool("ra", hideRadarApple); }
            refreshNow = true;
            drawRadarBar();
            dirty = true;
            break;
          }
        }
      }
    } else if (!detailMode && startY < HDR_H && startX < 64) {  // logo -> radar
      radarMode = true;
      radarWanted = true;
      refreshNow = true;
      drawRadarBar();
      dirty = true;
    } else if (detailMode) {
      if (startY < DAREA_Y && startX < 100) {  // Back
        detailMode = false;
        detailWanted = false;
        refreshNow = true;
        lastState = -1;
        drawHeader();
        drawButtons();
        dirty = true;
      }
    } else if (startY >= BTN_Y && startY < BTN_Y + BTN_H) {
      int i = (startX - LBTN_X0) / (LBTN_W + LBTN_GAP);
      if (startX >= LBTN_X0 && i >= 0 && i < 3 && (startX - LBTN_X0) % (LBTN_W + LBTN_GAP) < LBTN_W) {
        if (i == 0) { hideClassified = !hideClassified; prefs.putBool("hc", hideClassified); }
        if (i == 1) { hideGrouped = !hideGrouped; prefs.putBool("hg", hideGrouped); }
        if (i == 2) { hideUnknown = !hideUnknown; prefs.putBool("hu", hideUnknown); }
        scrollY = 0; velY = 0;
        refreshNow = true;
        drawButtons();
      }
    }
  }
  wasTouch = touching;

  if (!touching && fabsf(vel) > 0.5f) {
    sc += vel;
    vel *= 0.93f;
    dirty = true;
  } else if (!touching) {
    vel = 0;
  }

  if (radarMode) {
    static uint32_t seenR = 0, frames = 0, fpsT = 0;
    if (rVersion != seenR) { seenR = rVersion; drawRadarBar(); }
    drawRadar();
    frames++;
    if (millis() - fpsT > 5000) { Serial.printf("radar %.1f fps\n", frames * 1000.0f / (millis() - fpsT)); frames = 0; fpsT = millis(); }
    delay(2);
    return;
  }
  if (dataVersion != seenVersion) { seenVersion = dataVersion; dirty = true; }
  if (detVersion != seenDetVersion) { seenDetVersion = detVersion; dirty = true; }

  if (!detailMode) {
    // The header only changes with the connection state, localMode (server
    // vs. standalone), or the device count -- localMode used to be left out
    // here, so a switch back to the server after a brief hiccup left the
    // header frozen on "local scan" even though the list itself (which
    // redraws on dataVersion, not this) was already showing real data again.
    int state = (WiFi.status() != WL_CONNECTED ? 0 : (lastOkMs == 0 ? 1 : (fetchFailed ? 2 : 3))) * 2 + (localMode ? 1 : 0);
    int shownTotal = localMode ? bleScanTotal() : totals[frontIdx];
    if (state != lastState || shownTotal != lastTotal) {
      lastState = state;
      lastTotal = shownTotal;
      drawHeader();
    }
  }
  if (millis() - lastTick > 1000) {  // keeps the ages ticking
    lastTick = millis();
    dirty = true;
  }
  if (dirty) {
    dirty = false;
    if (detailMode) drawDetail();
    else drawList();
  }
  delay(8);
}
