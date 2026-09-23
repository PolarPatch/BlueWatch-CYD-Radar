// Copy this file to config.h, edit the values, then build and upload.
// config.h is gitignored on purpose: it holds your Wi-Fi password.
#pragma once

// Leave WIFI_SSID empty ("") to skip Wi-Fi and BlueWatch entirely and run
// fully standalone from boot: the board scans Bluetooth itself and never
// tries to connect to anything.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// The BlueWatch server (the machine running BlueWatch). If Wi-Fi is set
// above but this server can't be reached, the board falls back to its own
// standalone BLE scan automatically and keeps retrying the server in the
// background -- see README.md for what's different in that mode.
#define BW_HOST "192.168.1.10"
#define BW_PORT 8080

#define REFRESH_MS 3000   // how often the list/radar is fetched, in ms
#define LIST_WINDOW_SECONDS 900  // the list only shows devices seen within this many seconds (900 = 15 min, 86400 = 1 day). A small board is a "what's around right now" glance, not a browsable archive.
#define TFT_INVERT false  // set to true if colours look inverted
#define TFT_ROTATION 0    // try 0/1/2/3 -- see README "Troubleshooting other CYD panels"

// 2.8" board only: 0 (default) = portrait 240x320, 1 = landscape 320x240.
// Whichever you pick, TFT_ROTATION above still needs to be found by trying
// each of 0/1/2/3 -- there's no single rotation value that always means
// "landscape" across different panel/wiring batches.
#define SCREEN_LANDSCAPE 0

// 2.8" board only, uncomment if the picture stays mirrored no matter which
// TFT_ROTATION you try: this exact board part number ships with either an
// ST7789 (the default here) or an ILI9341 panel controller depending on
// the batch, and using the wrong one looks like a rotation bug that no
// rotation value actually fixes. See README "Troubleshooting other CYD
// panels" before reaching for this.
// #define CYD_PANEL_ILI9341

// 2.8" board only, deeper knobs -- defaults are verified against
// LovyanGFX's own autodetect entry for this board, not guessed. Only
// change these if a different panel/wiring variant needs it; see README.
// #define PANEL_RGB_ORDER true          // colours look swapped (red/blue) even after CYD_PANEL_ILI9341
// #define PANEL_OFFSET_ROTATION 0       // rotation is off by a fixed amount no TFT_ROTATION value corrects
// #define TOUCH_OFFSET_ROTATION 0       // touches are rotated relative to what's on screen
// #define PANEL_SPI_FREQ_WRITE 40000000 // display glitches/noise at the default SPI speed

// Touch orientation (adjust if taps land in the wrong place)
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

// 2.8" board only: resistive touch (XPT2046) raw ADC calibration. These
// are reasonable defaults, not a measurement of your specific panel -- if
// taps consistently land short of an edge, adjust the matching MIN/MAX
// inward; if they overshoot, move it outward.
#define TOUCH_CAL_X_MIN 300
#define TOUCH_CAL_X_MAX 3900
#define TOUCH_CAL_Y_MIN 3700  // inverted on purpose (min > max) -- corrects this panel's mounting
#define TOUCH_CAL_Y_MAX 200

// Radar sweep speed in radians per second (0.7 = one turn in 9 s, 1.2 = one turn in about 5 s)
#define RADAR_SPEED 1.2f
