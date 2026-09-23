# BlueWatch CYD Radar

A standalone display for [BlueWatch](https://github.com/PolarPatch/BlueWatch) on
a "Cheap Yellow Display" board — a scrollable device list and a live signal
radar, no PC needed once it's flashed.

Built for the **ESP32-3248S035C** (3.5", 320x480, ST7796 + GT911 capacitive
touch, portrait). It talks to a BlueWatch server over Wi-Fi and shows what
BlueWatch is seeing right now.

## What it does

- **Device list.** Every device BlueWatch has seen recently, newest first,
  scrollable, up to 100 rows. Each row shows a colour dot for the device
  type, name, signal strength and how long ago it was last seen.
- **Filters.** Three toggles at the top — Hide classified, Hide grouped, Hide
  unknowns — matching BlueWatch's own dashboard filters. Your choice is
  remembered across restarts.
- **Device details.** Long-press (about 0.6 s) any row to open its detail
  card: address, vendor, proximity, signal, sightings, first/last seen,
  watched, notes, plus the **Live Signal** (last 15 minutes) and **Signal
  History** (7 days) graphs, scrollable, redrawn from BlueWatch's own data.
- **Radar.** Tap the BlueWatch logo to open a live radar view: a sweeping
  beam, devices coloured by type, an afterglow trail, and small labels
  (name + dBm) on the strongest, watched or alerting devices. Distance from
  the centre is signal strength — same idea as BlueWatch's own `/radar`
  page. The angle is derived from each device's address, so a given device
  always sits at the same spot.

## Requirements

- An ESP32-3248S035C board (~$15-20, sold as a 3.5" "Cheap Yellow Display").
- A [BlueWatch](https://github.com/PolarPatch/BlueWatch) server on the same
  network, built from a version that has the `/api/display`,
  `/api/display/device` and `/api/display/radar` endpoints (added after
  v0.1.4 — check the BlueWatch repo's commit history if you're unsure).
- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).

## Setup

1. Clone this repo:
   ```sh
   git clone https://github.com/PolarPatch/BlueWatch-CYD-Radar
   cd BlueWatch-CYD-Radar
   ```
2. Copy the config template and edit it:
   ```sh
   cp src/config.example.h src/config.h
   ```
   Set `WIFI_SSID`, `WIFI_PASS`, and `BW_HOST`/`BW_PORT` to your BlueWatch
   server's address. `config.h` is gitignored, since it holds your Wi-Fi
   password.
3. Plug the board in over USB-C and build/flash:
   ```sh
   pio run -t upload
   ```
   The first build pulls in LovyanGFX and ArduinoJson; after that it's
   incremental. If PlatformIO picks the wrong serial port, uncomment and set
   `upload_port`/`monitor_port` in `platformio.ini`.
4. On first boot the screen shows "connecting to Wi-Fi..." and then the
   device count once it reaches BlueWatch.

If colours look inverted, or the display is upside down, or touches land in
the wrong place, adjust `TFT_INVERT`, `TFT_ROTATION` and the `TOUCH_*` flags
in `config.h` and re-flash.

## Hardware notes

Pin mapping (SPI display + I2C touch) is set for the common ESP32-3248S035C
wiring: display on `SPI2_HOST` (SCLK 14, MOSI 13, MISO 12, DC 2, CS 15,
backlight 27), touch (GT911) on I2C (SDA 33, SCL 32, INT 21, RST 25, address
`0x5D`). Touch is read directly rather than through a library, since the
common Arduino GT911 drivers didn't talk to this particular board's wiring
reliably in testing.

## Status

Working, tested against one board and one BlueWatch server. Not yet tested
against multiple simultaneous screens, a slow/unreliable Wi-Fi link, or a
BlueWatch server with a very large device list.

## License

MIT — see [LICENSE](LICENSE). Uses
[LovyanGFX](https://github.com/lovyan03/LovyanGFX) (MIT) and
[ArduinoJson](https://github.com/bblanchon/ArduinoJson) (MIT) as library
dependencies (not vendored, pulled in by PlatformIO).
