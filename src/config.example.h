// Copy this file to config.h, edit the values, then build and upload.
// config.h is gitignored on purpose: it holds your Wi-Fi password.
#pragma once

#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// The BlueWatch server (the machine running BlueWatch)
#define BW_HOST "192.168.1.10"
#define BW_PORT 8080

#define REFRESH_MS 3000   // how often the list/radar is fetched, in ms
#define TFT_INVERT false  // set to true if colours look inverted
#define TFT_ROTATION 0    // 0 = portrait, 2 = portrait upside down

// Touch orientation (adjust if taps land in the wrong place)
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

// Radar sweep speed in radians per second (0.7 = one turn in 9 s, 1.2 = one turn in about 5 s)
#define RADAR_SPEED 1.2f
