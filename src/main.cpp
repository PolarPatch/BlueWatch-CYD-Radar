// BlueWatch display for the ESP32-3248S035C (320x480 portrait, capacitive touch).
// Fetches /api/display from BlueWatch and shows a scrollable device list with
// three toggles at the top: hide classified, grouped and unknown devices.

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <ArduinoJson.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"
#include "logo.h"

// ---------------------------------------------------------------- hardware

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = TFT_INVERT;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite rowSpr(&tft);


// ---------------------------------------------------------------- touch (GT911, read directly)

static const int TP_SDA = 33, TP_SCL = 32, TP_INT = 21, TP_RST = 25;
static const uint8_t GT911_ADDR = 0x5D;

static void gt911Init() {
  pinMode(TP_INT, OUTPUT);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_INT, HIGH);  // INT high during reset selects address 0x5D
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(10);
  pinMode(TP_INT, INPUT);
  delay(60);
  Wire1.begin(TP_SDA, TP_SCL, 400000);
}

static bool gt911Read(int32_t* x, int32_t* y) {
  Wire1.beginTransmission(GT911_ADDR);
  Wire1.write(0x81); Wire1.write(0x4E);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((int)GT911_ADDR, 1) != 1) return false;
  uint8_t status = Wire1.read();
  bool ready = status & 0x80;
  int points = status & 0x0F;
  bool got = false;
  if (ready && points > 0) {
    Wire1.beginTransmission(GT911_ADDR);
    Wire1.write(0x81); Wire1.write(0x50);
    Wire1.endTransmission(false);
    if (Wire1.requestFrom((int)GT911_ADDR, 4) == 4) {
      uint16_t rx = Wire1.read(); rx |= Wire1.read() << 8;
      uint16_t ry = Wire1.read(); ry |= Wire1.read() << 8;
      *x = rx; *y = ry;
      got = true;
    }
  }
  if (ready) {  // acknowledge so the controller reports the next sample
    Wire1.beginTransmission(GT911_ADDR);
    Wire1.write(0x81); Wire1.write(0x4E); Wire1.write(0);
    Wire1.endTransmission();
  }
  return got;
}

// Touch state with a short hold, since the GT911 only reports fresh samples.
static bool readTouch(int32_t* x, int32_t* y) {
  static uint32_t lastSeen = 0;
  static int32_t lx = 0, ly = 0;
  int32_t tx, ty;
  if (gt911Read(&tx, &ty)) {
    lx = tx; ly = ty; lastSeen = millis();
#if TOUCH_SWAP_XY
    int32_t t = lx; lx = ly; ly = t;
#endif
#if TOUCH_FLIP_X
    lx = 319 - lx;
#endif
#if TOUCH_FLIP_Y
    ly = 479 - ly;
#endif
  }
  if (millis() - lastSeen < 60) { *x = lx; *y = ly; return true; }
  return false;
}

// ---------------------------------------------------------------- layout

static const int W = 320;
static const int HDR_H = 62;
static const int BTN_Y = 64;
static const int BTN_H = 32;
static const int LISTBOX_Y = 100;
static const int LISTBOX_H = 480 - LISTBOX_Y;
static const int ROW_H = 26;
static const int DAREA_Y = 44;  // content area below the bar in detail and radar views
static const int DAREA_H = 480 - DAREA_Y;

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
static const float RADAR_MAXR = 159.0f;  // outer ring sits 1 px from the screen edges

static bool hideClassified = true, hideGrouped = true, hideUnknown = true;
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

static void fetchList() {
  String url = String("http://") + BW_HOST + ":" + BW_PORT + "/api/display?limit=100";
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

static void fetchRadar() {
  String url = String("http://") + BW_HOST + ":" + BW_PORT + "/api/display/radar?window=300";
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
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (radarWanted) fetchRadar();
      else if (detailWanted) fetchDetail();
      else fetchList();
    } else {
      fetchFailed = true;
    }
    for (int i = 0; i < REFRESH_MS / 100 && !refreshNow; i++) vTaskDelay(pdMS_TO_TICKS(100));
    refreshNow = false;
  }
}

// ---------------------------------------------------------------- drawing

static float scrollY = 0, velY = 0;

static void drawHeader() {
  tft.fillRect(0, 0, W, HDR_H, C_BG);
  tft.pushImage(4, 3, LOGO_W, LOGO_H, (const lgfx::rgb565_t*)LOGO_DATA);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(top_left);
  tft.setTextColor(colAccent, C_BG);
  tft.drawString("Blue", 68, 6);
  int bw = tft.textWidth("Blue");
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("Watch", 68 + bw, 6);

  tft.setFont(&fonts::Font2);
  char line[48];
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(line, sizeof(line), "connecting to Wi-Fi...");
    tft.setTextColor(colGold, C_BG);
  } else if (lastOkMs == 0) {
    snprintf(line, sizeof(line), "loading...");
    tft.setTextColor(C_MUTED, C_BG);
  } else if (fetchFailed) {
    snprintf(line, sizeof(line), "no answer from BlueWatch");
    tft.setTextColor(colAlert, C_BG);
  } else {
    snprintf(line, sizeof(line), "%d devices", totals[frontIdx]);
    tft.setTextColor(C_MUTED, C_BG);
  }
  tft.drawString(line, 70, 38);
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
  const int x0 = 42, bw = 88, gap = 4;
  for (int i = 0; i < 3; i++) {
    int x = x0 + i * (bw + gap);
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
  const int maxW = 196;
  rowSpr.setTextColor(r->l ? colAlert : C_TEXT, bg);
  while (strlen(name) > 1 && rowSpr.textWidth(name) > maxW) name[strlen(name) - 1] = 0;
  rowSpr.drawString(name, 28, ROW_H / 2);

  char age[8];
  formatAge(r->a, age, sizeof(age));
  rowSpr.setTextDatum(middle_right);
  rowSpr.setTextColor(C_MUTED, bg);
  rowSpr.drawString(age, 270, ROW_H / 2);

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
  sp.fillRoundRect(10, top - yo, 300, h, 6, C_ROW_B);
  sp.setFont(&fonts::Font0);
  sp.setTextColor(C_MUTED, C_ROW_B);
  sp.setTextDatum(middle_left);
  const int levels[3] = {-40, -60, -80};
  for (int i = 0; i < 3; i++) {
    int y = dbmY(top, h, levels[i]) - yo;
    sp.drawFastHLine(34, y, 268, C_GRID);
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
  while (strlen(title) > 1 && sp.textWidth(title) > 300) title[strlen(title) - 1] = 0;
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
    while (strlen(v) > 1 && sp.textWidth(v) > 210) v[strlen(v) - 1] = 0;
    sp.drawString(v, 310, y + 6);
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
    int x = 36 + (int)(i * 4.5f), y = dbmY(lTop, cH, d.live[i]);
    if (prevX >= 0) sp.drawLine(prevX, prevY - yo, x, y - yo, C_TEXT);
    sp.fillCircle(x, y - yo, 1, C_TEXT);
    prevX = x; prevY = y; lastX = x; lastY = y;
  }
  if (any) sp.fillCircle(lastX, lastY - yo, 3, colAccent);
  else {
    sp.setTextDatum(middle_center);
    sp.setTextColor(C_MUTED, C_ROW_B);
    sp.drawString("no signal in the last 15 min", 160, lTop + cH / 2 - yo);
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
    int x = hn == 1 ? 170 : 36 + (int)(i * 268.0f / (hn - 1));
    int y1 = dbmY(hTop, cH, d.hmax[i]), y2 = dbmY(hTop, cH, d.hmin[i]);
    int ym = (y1 + y2) / 2;
    if (px >= 0) sp.drawLine(px, py - yo, x, ym - yo, C_TEXT);
    sp.fillRect(x - 1, y1 - yo, 3, max(3, y2 - y1 + 1), C_TEXT);
    px = x; py = ym;
  }
  if (!hAny) {
    sp.setTextDatum(middle_center);
    sp.setTextColor(C_MUTED, C_ROW_B);
    sp.drawString("not enough data", 160, hTop + cH / 2 - yo);
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
static const int RBAND_H = 40;

static uint16_t mix565(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) | (((int)(ag + (bg - ag) * t)) << 5) | (int)(ab + (bb - ab) * t));
}

static void drawRadarBar() {
  tft.fillRect(0, 0, W, DAREA_Y, C_BG);
  tft.fillRoundRect(6, 6, 76, 32, 6, C_ROW_B);
  tft.drawRoundRect(6, 6, 76, 32, 6, C_MUTED);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_TEXT, C_ROW_B);
  tft.drawString("< Back", 44, 22);
  tft.setTextDatum(middle_left);
  tft.setTextColor(C_MUTED, C_BG);
  char line[32];
  snprintf(line, sizeof(line), "Radar  -  %d on air", rcounts[rFront]);
  tft.drawString(line, 96, 22);
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
  prefs.begin("bwdisp", false);
  hideClassified = prefs.getBool("hc", true);
  hideGrouped = prefs.getBool("hg", true);
  hideUnknown = prefs.getBool("hu", true);

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setBrightness(200);
  tft.fillScreen(C_BG);
  colAccent = tft.color565(88, 101, 242);
  colAlert = tft.color565(248, 81, 73);
  colGold = tft.color565(230, 170, 60);
  colGood = tft.color565(63, 185, 80);

  gt911Init();

  rowSpr.setColorDepth(16);
  rowSpr.createSprite(W, ROW_H);
  radSpr.setColorDepth(16);
  radSpr.createSprite(W, RBAND_H);

  drawHeader();
  drawButtons();
  drawList();

  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
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
      if (startY < DAREA_Y && startX < 100) {  // Back
        radarMode = false;
        radarWanted = false;
        refreshNow = true;
        lastState = -1;
        drawHeader();
        drawButtons();
        dirty = true;
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
      const int x0 = 42, bw = 88, gap = 4;
      int i = (startX - x0) / (bw + gap);
      if (startX >= x0 && i >= 0 && i < 3 && (startX - x0) % (bw + gap) < bw) {
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
    // The header only changes with the connection state or the device count.
    int state = WiFi.status() != WL_CONNECTED ? 0 : (lastOkMs == 0 ? 1 : (fetchFailed ? 2 : 3));
    if (state != lastState || totals[frontIdx] != lastTotal) {
      lastState = state;
      lastTotal = totals[frontIdx];
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
