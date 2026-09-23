// Standalone mode: the board scans Bluetooth LE itself with NimBLE and
// keeps a small in-RAM table, so it shows something even with no BlueWatch
// server configured or reachable. See ble_scan.cpp for what it can and
// can't tell you compared to a real BlueWatch server (mainly: no history
// beyond the last few minutes, and no OUI vendor lookup).
#pragma once
#include <Arduino.h>

// One row of what the board has learned about a device from its own
// advertisements. Kept intentionally small -- this lives in RAM, and there
// can be a few hundred of these.
struct BleEntry {
  uint8_t mac[6];
  char name[24];
  char type[14];       // same type palette as typeColor() in main.cpp
  int16_t rssi;
  uint32_t firstSeenMs;
  uint32_t lastSeenMs;
  uint32_t sightings;
  int8_t liveRing[16]; // last few RSSI samples, oldest first, 0 = empty slot
  uint8_t liveHead;
  uint8_t isApple;      // manufacturer data carried Apple's company ID (0x004C),
                         // regardless of which Apple message it was -- used for
                         // the "Hide Apple" radar filter, since most Apple
                         // devices have no advertised name or vendor to match on
};

// Starts the NimBLE scan task (call once from setup(), after Wi-Fi is
// already initialised -- NimBLE and Wi-Fi share the 2.4 GHz radio, which
// the ESP32 handles by itself, no extra coexistence setup needed).
void bleScanStart();

// Snapshot access, safe to call from another task -- copies under a lock so
// the scan callback (running on NimBLE's own task) never races the reader.
// Returns how many entries were copied (up to maxOut).
int bleScanSnapshot(BleEntry* out, int maxOut);
int bleScanTotal();  // how many distinct devices have been seen since boot

// One entry by MAC (6 raw bytes), or nullptr if not currently known.
// `out` receives a copy; safe to call from another task.
bool bleScanFind(const uint8_t mac[6], BleEntry* out);

void macToStr(const uint8_t mac[6], char* out, size_t outSz);
bool strToMac(const char* s, uint8_t mac[6]);
