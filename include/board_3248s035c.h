// Secondary board: ESP32-3248S035C, 3.5" 320x480 portrait, ST7796 + GT911
// capacitive touch. Moved here unchanged from the original single-board
// firmware so the 3.5" board keeps working exactly as before.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "config.h"

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

// ---- touch (GT911, read directly -- the common Arduino GT911 drivers did
// not talk to this board's wiring reliably in testing, see the README) ----

static const int TP_SDA = 33, TP_SCL = 32, TP_INT = 21, TP_RST = 25;
static const uint8_t GT911_ADDR = 0x5D;

static void boardTouchInit() {
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
static bool readTouchBoard(int32_t* x, int32_t* y) {
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
