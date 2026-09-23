// Main board: ESP32-2432S028 ("Cheap Yellow Display"), 2.8", XPT2046
// resistive touch. There are, by every account, dozens of panel/wiring
// variants sold under this same part number -- this file cannot know
// which one a given reader has. What it does instead is expose every knob
// that turned out to matter as a config.h define with a sensible default,
// so a different unit is a config.h edit + reflash, not a code change:
//
//   CYD_PANEL_ILI9341   -- undefined (default) = ST7789, defined = ILI9341.
//                           Same part number ships with either chip; if the
//                           picture stays mirrored no matter which
//                           TFT_ROTATION you try, this is almost always why
//                           (see README "Troubleshooting other CYD panels").
//   SCREEN_LANDSCAPE     -- 0 (default) = portrait 240x320, 1 = landscape
//                           320x240. Whichever you pick, TFT_ROTATION still
//                           needs to be found by trying 0/1/2/3 -- there is
//                           no universal "landscape is always rotation 1"
//                           rule across panel batches.
//   PANEL_OFFSET_ROTATION, TOUCH_OFFSET_ROTATION, PANEL_RGB_ORDER,
//   PANEL_SPI_FREQ_WRITE -- lower-level knobs, defaulted to values verified
//                           against LovyanGFX's own autodetect entry for
//                           this exact board (board_Sunton_ESP32_2432S028
//                           in LGFX_AutoDetect_ESP32_all.hpp) for whichever
//                           panel chip is selected above.
//
// Pin mapping and defaults below are not guessed -- copied from that
// detector, since an earlier guessed version (wrong panel offset_rotation,
// wrong touch bus, wrong panel dimensions) produced a rotated *and*
// mirrored image that no TFT_ROTATION value alone could fix.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

// ---- defaults, all overridable from config.h ----

#ifndef SCREEN_LANDSCAPE
#define SCREEN_LANDSCAPE 0
#endif

#ifndef PANEL_RGB_ORDER
#define PANEL_RGB_ORDER false
#endif

#if defined(CYD_PANEL_ILI9341)
  #define CYD_PANEL_CLASS lgfx::Panel_ILI9341
  #ifndef PANEL_SPI_FREQ_WRITE
  #define PANEL_SPI_FREQ_WRITE 40000000
  #endif
  #ifndef PANEL_OFFSET_ROTATION
  #define PANEL_OFFSET_ROTATION 2   // LovyanGFX's ILI9341 detector sets this on the panel
  #endif
  #ifndef TOUCH_OFFSET_ROTATION
  #define TOUCH_OFFSET_ROTATION 0
  #endif
#else
  #define CYD_PANEL_CLASS lgfx::Panel_ST7789
  #ifndef PANEL_SPI_FREQ_WRITE
  #define PANEL_SPI_FREQ_WRITE 80000000
  #endif
  #ifndef PANEL_OFFSET_ROTATION
  #define PANEL_OFFSET_ROTATION 0
  #endif
  #ifndef TOUCH_OFFSET_ROTATION
  #define TOUCH_OFFSET_ROTATION 2  // LovyanGFX's ST7789 detector sets this on the touch controller instead
  #endif
#endif

// Reasonable touch calibration defaults if config.h doesn't define these --
// see the README if taps land off by a consistent amount. The Y range is
// deliberately inverted (min > max) to correct this panel's mounting.
#ifndef TOUCH_CAL_X_MIN
#define TOUCH_CAL_X_MIN 300
#endif
#ifndef TOUCH_CAL_X_MAX
#define TOUCH_CAL_X_MAX 3900
#endif
#ifndef TOUCH_CAL_Y_MIN
#define TOUCH_CAL_Y_MIN 3700
#endif
#ifndef TOUCH_CAL_Y_MAX
#define TOUCH_CAL_Y_MAX 200
#endif

class LGFX : public lgfx::LGFX_Device {
  CYD_PANEL_CLASS _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = PANEL_SPI_FREQ_WRITE;
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
      // The panel is physically 240x320 -- Panel_ST7789's own class default
      // is 240x240 (the common square module), so this must be explicit or
      // everything past x=240 is clipped/misdrawn.
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_rotation = PANEL_OFFSET_ROTATION;
      cfg.readable = false;
      cfg.invert = TFT_INVERT;
      cfg.rgb_order = PANEL_RGB_ORDER;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      // XPT2046 on its own *software* (bit-banged) SPI bus, not the shared
      // hardware one -- matches LovyanGFX's own Sunton detector.
      auto cfg = _touch.config();
      cfg.x_min = TOUCH_CAL_X_MIN;
      cfg.x_max = TOUCH_CAL_X_MAX;
      cfg.y_min = TOUCH_CAL_Y_MIN;
      cfg.y_max = TOUCH_CAL_Y_MAX;
      cfg.pin_int = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = TOUCH_OFFSET_ROTATION;
      cfg.spi_host = (spi_host_device_t)-1;  // -1 = software SPI for XPT2046
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;

static void boardTouchInit() {}  // LovyanGFX's Touch_XPT2046 needs no extra init

static bool readTouchBoard(int32_t* x, int32_t* y) {
  int32_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return false;
#if TOUCH_SWAP_XY
  int32_t t = tx; tx = ty; ty = t;
#endif
#if SCREEN_LANDSCAPE
  const int32_t maxX = 319, maxY = 239;
#else
  const int32_t maxX = 239, maxY = 319;
#endif
#if TOUCH_FLIP_X
  tx = maxX - tx;
#endif
#if TOUCH_FLIP_Y
  ty = maxY - ty;
#endif
  *x = tx; *y = ty;
  return true;
}
