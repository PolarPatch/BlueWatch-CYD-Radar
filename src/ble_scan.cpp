// Standalone BLE scanning and a small on-device classifier -- a much
// reduced version of BlueWatch's own bluewatch/classifier.py, ported by
// hand for what fits on a microcontroller. It knows a handful of specific
// signatures (trackers, Flipper Zero, Meta/Ray-Ban glasses, fitness/audio
// gear); everything else stays "unknown". There is no MAC-vendor (OUI)
// database on the board -- that table is tens of MB on the BlueWatch
// server -- so a device with no advertised name shows no vendor at all
// in standalone mode, unlike a real BlueWatch server.
#include "ble_scan.h"

#include <NimBLEDevice.h>

static const int MAX_ENTRIES = 150;
static BleEntry table[MAX_ENTRIES];
static int tableCount = 0;
static uint32_t totalSeen = 0;
static SemaphoreHandle_t tableLock;

void macToStr(const uint8_t mac[6], char* out, size_t outSz) {
  snprintf(out, outSz, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool strToMac(const char* s, uint8_t mac[6]) {
  unsigned v[6];
  if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
  return true;
}

// ---------------------------------------------------------------- lite classifier

// Same type strings typeColor() in main.cpp already knows how to colour.
// A signature not listed here (most of them) falls through to "unknown".
static const char* classifyLite(NimBLEAdvertisedDevice* d) {
  // Manufacturer data: company ID (2 bytes, little-endian) + payload.
  if (d->haveManufacturerData()) {
    std::string mfg = d->getManufacturerData();
    if (mfg.size() >= 2) {
      uint16_t companyId = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
      if (companyId == 0x0E29) return "flipper";                    // Flipper Devices Inc.
      if (companyId == 0x01AB || companyId == 0x058E ||              // Meta Platforms /
          companyId == 0x0D53 || companyId == 0x03C2)                 // Luxottica / Snap
        return "wearable";                                            // (camera glasses)
      if (companyId == 0x004C && mfg.size() >= 4) {
        // Apple TLV chain: type byte, length byte, payload... Only the
        // Find My message (0x12) is matched here -- narrow on purpose, to
        // avoid guessing at bytes not cross-checked against real hardware.
        size_t off = 2;
        while (off + 1 < mfg.size()) {
          uint8_t type = (uint8_t)mfg[off];
          uint8_t len = (uint8_t)mfg[off + 1];
          if (type == 0x12) return "tracker";  // Find My network broadcast
          off += 2 + len;
          if (len == 0) break;  // malformed / truncated, stop rather than loop
        }
      }
    }
  }

  // Service UUIDs (checked as 16-bit where possible).
  for (int i = 0; i < (int)d->getServiceUUIDCount(); i++) {
    NimBLEUUID u = d->getServiceUUID(i);
    if (u.bitSize() != 16) continue;
    uint16_t v = u.getNative()->u16.value;
    switch (v) {
      case 0x3081: case 0x3082: case 0x3083: return "flipper";                 // Flipper Zero
      case 0xFEED: case 0xFEEC: return "tracker";                              // Tile
      case 0xFD5A: return "tracker";                                          // Samsung SmartTag
      case 0xFEAA: return "tracker";                                          // Google Find My Device / Eddystone
      case 0xFFFA: return "drone";                                            // OpenDroneID Remote ID
      case 0x180D: case 0x1814: case 0x1816: case 0x1818: case 0x1826:        // heart rate, RSC, CSC,
      case 0x181C: case 0x181D:                                               // power, fitness machine,
        return "wearable";                                                    // user data, weight scale
      case 0x110A: case 0x110B: case 0x110D: case 0x111E: return "audio";     // A2DP / handsfree
    }
  }

  return "unknown";
}

// ---------------------------------------------------------------- table

static BleEntry* findLocked(const uint8_t mac[6]) {
  for (int i = 0; i < tableCount; i++)
    if (memcmp(table[i].mac, mac, 6) == 0) return &table[i];
  return nullptr;
}

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    uint8_t mac[6];
    memcpy(mac, d->getAddress().getNative(), 6);
    bool isApple = false;
    if (d->haveManufacturerData()) {
      std::string mfg = d->getManufacturerData();
      if (mfg.size() >= 2 && (uint8_t)mfg[0] == 0x4C && (uint8_t)mfg[1] == 0x00) isApple = true;
    }
    // NimBLE reports MACs little-endian-first from getNative(); BlueWatch
    // (and everything a person reads) writes them big-endian-first.
    for (int i = 0; i < 3; i++) { uint8_t t = mac[i]; mac[i] = mac[5 - i]; mac[5 - i] = t; }

    xSemaphoreTake(tableLock, portMAX_DELAY);
    BleEntry* e = findLocked(mac);
    if (!e) {
      if (tableCount < MAX_ENTRIES) {
        e = &table[tableCount++];
      } else {
        // Table full: evict the entry that has been quiet the longest.
        e = &table[0];
        for (int i = 1; i < tableCount; i++)
          if (table[i].lastSeenMs < e->lastSeenMs) e = &table[i];
      }
      memset(e, 0, sizeof(*e));
      memcpy(e->mac, mac, 6);
      e->firstSeenMs = millis();
      totalSeen++;
    }
    e->lastSeenMs = millis();
    e->sightings++;
    e->rssi = d->getRSSI();
    e->liveRing[e->liveHead] = (int8_t)e->rssi;
    e->liveHead = (e->liveHead + 1) % (sizeof(e->liveRing) / sizeof(e->liveRing[0]));
    if (d->haveName() && e->name[0] == 0) strlcpy(e->name, d->getName().c_str(), sizeof(e->name));
    strlcpy(e->type, classifyLite(d), sizeof(e->type));
    if (isApple) e->isApple = 1;
    xSemaphoreGive(tableLock);
  }
};
static ScanCB scanCallbacks;

// Restarts the scan every so often so NimBLE's own duplicate filter (see
// setAdvertisedDeviceCallbacks below) drops and a device already seen this
// pass gets a fresh callback -- that's what keeps its RSSI and "last seen"
// updating in a dense area without keeping duplicate delivery on all the
// time, which is what overloaded the CPU in the first place (see below).
static void scanRestartTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->start(0, nullptr, false);
  }
}

void bleScanStart() {
  tableLock = xSemaphoreCreateMutex();
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEScan* scan = NimBLEDevice::getScan();
  // wantDuplicates=false: only the first advertisement from a given device
  // per scan window reaches onResult(). A dense neighbourhood (the kind
  // BlueWatch itself is built for -- easily thousands of devices) can
  // otherwise deliver so many callbacks that the Bluetooth/Wi-Fi radio
  // coexistence work alone starves the idle task and trips the watchdog,
  // rebooting the board. A lower duty cycle plus periodic restarts (above)
  // trade continuous updates for a board that stays up.
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(320);  // ~200 ms
  scan->setWindow(96);     // ~60 ms -- ~30% duty cycle, kept low: this environment is unusually BLE-dense
  scan->setMaxResults(0);  // don't keep NimBLE's own results list, we have our own table
  scan->start(0, nullptr, false);  // 0 duration = scan forever (until restarted above)
  xTaskCreatePinnedToCore(scanRestartTask, "blerestart", 3072, nullptr, 1, nullptr, 1);
}

int bleScanSnapshot(BleEntry* out, int maxOut) {
  xSemaphoreTake(tableLock, portMAX_DELAY);
  int n = min(maxOut, tableCount);
  memcpy(out, table, n * sizeof(BleEntry));
  xSemaphoreGive(tableLock);
  return n;
}

int bleScanTotal() { return (int)totalSeen; }

bool bleScanFind(const uint8_t mac[6], BleEntry* out) {
  xSemaphoreTake(tableLock, portMAX_DELAY);
  BleEntry* e = findLocked(mac);
  if (e) *out = *e;
  xSemaphoreGive(tableLock);
  return e != nullptr;
}
