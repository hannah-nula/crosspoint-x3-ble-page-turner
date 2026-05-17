#include "BluetoothHIDManager.h"
#include <BluetoothDiagnostics.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <esp_system.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
// Arduino-ESP32 3.x releases BT controller memory during startup unless a
// Bluetooth library marks it as in use before app_main(). NimBLE-Arduino does
// not do that automatically in this build, which can crash later in
// `NimBLEDevice::init()` / `esp_bt_controller_init()` when Bluetooth is enabled
// from the settings UI on ESP32-C3. Pulling in this header sets the core's
// `_btLibraryInUse` flag early via a constructor and keeps BLE memory reserved.
#include <esp32-hal-bt-mem.h>
#endif

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";

static constexpr uint8_t GAMEBRICK_ACTION_A_CODE = 0xF1;
static constexpr uint8_t GAMEBRICK_ACTION_B_CODE = 0xF2;

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
// Keep latency at 0 for low input lag while allowing a longer supervision timeout
// to reduce disconnects at marginal range.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;   // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;   // 30ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_TIMEOUT = 600;       // 6s
constexpr uint16_t BLE_CONN_SCAN_INTERVAL = 48;
constexpr uint16_t BLE_CONN_SCAN_WINDOW = 48;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t BLE_RECONNECT_SCAN_MS = 8000;
constexpr uint32_t BLE_MANUAL_RECONNECT_SCAN_MS = 45000;
constexpr uint32_t BLE_MANUAL_SCAN_RESPONSE_TIMEOUT_MS = 1200;
constexpr uint32_t BLE_AUTO_PAGE_TURNER_SCAN_MS = 9000;
constexpr uint16_t BLE_RECONNECT_SCAN_INTERVAL = 48;
constexpr uint16_t BLE_RECONNECT_SCAN_WINDOW = 48;
constexpr uint32_t BLE_MANUAL_RECONNECT_TIMEOUT_MS = 12000;
constexpr unsigned long BLE_AUTO_RECONNECT_BOOT_GRACE_MS = 60000;
constexpr unsigned long BLE_AUTO_RECONNECT_WAKE_GRACE_MS = 6000;
constexpr unsigned long BLE_RECONNECT_CHECK_INTERVAL_MS = 1000;
constexpr unsigned long BLE_RECONNECT_USER_INTERVAL_MS = 2000;
constexpr unsigned long BLE_RECONNECT_FAST_INTERVAL_MS = 15000;
constexpr unsigned long BLE_RECONNECT_WAKE_INTERVAL_MS = 4000;
constexpr unsigned long BLE_RECONNECT_FAST_WINDOW_MS = 45000;
constexpr unsigned long BLE_RECONNECT_BASE_INTERVAL_MS = 60000;
constexpr unsigned long BLE_RECONNECT_MAX_INTERVAL_MS = 300000;
constexpr unsigned long BLE_PAGE_TURNER_IDLE_RECONNECT_MS = 195000;
constexpr unsigned long BLE_PAGE_TURNER_RECONNECT_INTERVAL_MS = 10000;
constexpr unsigned long BLE_PAGE_TURNER_RECONNECT_FAST_WINDOW_MS = 20UL * 60UL * 1000UL;
constexpr unsigned long BLE_PAGE_TURNER_RECONNECT_SLOW_INTERVAL_MS = 60000;
constexpr unsigned long BLE_PAGE_TURNER_RECONNECT_WINDOW_MS = 4UL * 60UL * 60UL * 1000UL;
constexpr unsigned long BLE_INTENTIONAL_DISCONNECT_SUPPRESS_MS = 5000;
constexpr unsigned long FREE2_STALE_RELEASE_DEFAULT_MS = 250;
constexpr unsigned long FREE2_STALE_RELEASE_READER_MS = 500;
constexpr uint8_t BLE_ADDR_TYPE_UNKNOWN = 0xFF;
constexpr char BLE_AUTO_RECONNECT_GUARD_FILE[] = "/.crosspoint/ble_auto_reconnect_guard";
constexpr char BLE_AUTO_RECONNECT_WAKE_FILE[] = "/.crosspoint/ble_reconnect_after_wake";
constexpr UBaseType_t BLE_INPUT_EVENT_QUEUE_LENGTH = 24;
constexpr UBaseType_t BLE_CONNECTION_EVENT_QUEUE_LENGTH = 8;
constexpr uint8_t BLE_RECONNECT_SEEN_LOG_LIMIT = 8;

static unsigned long reconnectBackoffMs(uint8_t failureCount) {
  const uint8_t cappedFailures = failureCount > 4 ? 4 : failureCount;
  unsigned long interval = BLE_RECONNECT_BASE_INTERVAL_MS;
  for (uint8_t i = 0; i < cappedFailures; i++) {
    interval *= 2;
  }
  return interval > BLE_RECONNECT_MAX_INTERVAL_MS ? BLE_RECONNECT_MAX_INTERVAL_MS : interval;
}

static bool resetReasonSuggestsCrash(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
      return true;
    default:
      return false;
  }
}

static bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
  if (!needle || !*needle) {
    return false;
  }

  const size_t needleLen = strlen(needle);
  if (haystack.size() < needleLen) {
    return false;
  }

  for (size_t i = 0; i + needleLen <= haystack.size(); i++) {
    size_t j = 0;
    while (j < needleLen) {
      char h = haystack[i + j];
      char n = needle[j];
      if (h >= 'A' && h <= 'Z') h = static_cast<char>(h - 'A' + 'a');
      if (n >= 'A' && n <= 'Z') n = static_cast<char>(n - 'A' + 'a');
      if (h != n) break;
      j++;
    }
    if (j == needleLen) {
      return true;
    }
  }
  return false;
}

static bool isUnknownOrEmptyName(const std::string& name) {
  return name.empty() || name == "Unknown" || name == "(unknown)";
}

static bool addressLooksRandom(const std::string& address) {
  if (address.size() < 2) {
    return false;
  }

  char* end = nullptr;
  const unsigned long firstByte = strtoul(address.substr(0, 2).c_str(), &end, 16);
  if (!end || *end != '\0') {
    return false;
  }

  // For random BLE addresses, the two most significant bits in the displayed
  // first byte encode random-address subtypes. This catches Free3-R's saved
  // address after upgrading from older builds that stored all devices as public.
  return (firstByte & 0xC0) != 0;
}

static uint8_t normalizeAddressType(uint8_t addressType, const std::string& address, const std::string& name) {
  if (addressType == BLE_ADDR_PUBLIC && addressLooksRandom(address) &&
      (containsCaseInsensitive(name, "free2") || containsCaseInsensitive(name, "free3"))) {
    return BLE_ADDR_RANDOM;
  }

  if (addressType <= BLE_ADDR_RANDOM_ID) {
    return addressType;
  }

  return BLE_ADDR_PUBLIC;
}

static void addUniqueAddressType(std::vector<uint8_t>& types, uint8_t type) {
  if (type > BLE_ADDR_RANDOM_ID) {
    return;
  }
  if (std::find(types.begin(), types.end(), type) == types.end()) {
    types.push_back(type);
  }
}

static uint8_t baseAddressType(uint8_t type) {
  if (type == BLE_ADDR_PUBLIC_ID) {
    return BLE_ADDR_PUBLIC;
  }
  if (type == BLE_ADDR_RANDOM_ID) {
    return BLE_ADDR_RANDOM;
  }
  return type;
}

static std::vector<uint8_t> connectionAddressTypeCandidates(uint8_t primaryType, const std::string& address,
                                                            const std::string& name) {
  std::vector<uint8_t> types;
  const bool freeLike = containsCaseInsensitive(name, "free2") || containsCaseInsensitive(name, "free3");
  const uint8_t normalizedType = normalizeAddressType(primaryType, address, name);

  if (freeLike && addressLooksRandom(address)) {
    addUniqueAddressType(types, BLE_ADDR_RANDOM);
  }

  addUniqueAddressType(types, normalizedType);
  addUniqueAddressType(types, baseAddressType(normalizedType));

  if (normalizedType == BLE_ADDR_RANDOM || normalizedType == BLE_ADDR_RANDOM_ID) {
    addUniqueAddressType(types, BLE_ADDR_PUBLIC);
  } else {
    addUniqueAddressType(types, BLE_ADDR_RANDOM);
  }

  return types;
}

static uint32_t perAttemptConnectTimeout(uint32_t totalTimeoutMs, size_t attemptCount) {
  if (attemptCount <= 1) {
    return totalTimeoutMs;
  }

  const uint32_t divided = totalTimeoutMs / static_cast<uint32_t>(attemptCount);
  if (divided < 3500) {
    return 3500;
  }
  if (divided > 5000) {
    return 5000;
  }
  return divided;
}

class SemaphoreLock {
 public:
  SemaphoreLock(SemaphoreHandle_t semaphore, TickType_t ticks) : _semaphore(semaphore) {
    _locked = _semaphore && xSemaphoreTake(_semaphore, ticks) == pdTRUE;
  }

  ~SemaphoreLock() {
    if (_locked) {
      xSemaphoreGive(_semaphore);
    }
  }

  bool locked() const { return _locked; }

 private:
  SemaphoreHandle_t _semaphore = nullptr;
  bool _locked = false;
};
}

struct ReportMapHints {
  bool hasConsumerPage = false;
  bool hasKeyboardPage = false;
  uint8_t preferredByteIndex = 0xFF;
};

struct ExtractedHIDKey {
  uint8_t keycode = 0x00;
  uint8_t reportIndex = 0xFF;
};

static ExtractedHIDKey extractGenericPageTurnKeycode(const uint8_t* report, size_t length) {
  ExtractedHIDKey result;

  if (!report || length == 0) {
    return result;
  }

  // First pass: prefer known page-turn keycodes anywhere in short reports.
  const size_t scanLen = length < 8 ? length : 8;
  for (size_t i = 0; i < scanLen; i++) {
    const uint8_t code = report[i];
    if (DeviceProfiles::isCommonPageTurnCode(code)) {
      result.keycode = code;
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Second pass: typical keyboard report key slots (bytes 2..7)
  for (size_t i = 2; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Final fallback for non-keyboard HID layouts: first non-zero byte.
  for (size_t i = 0; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  return result;
}

static uint8_t classifyFree2Direction(const uint8_t keycode) {
  if (keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
      keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D) {
    return 0x01;
  }

  if (keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
      keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D) {
    return 0x00;
  }

  return 0xFF;
}

static bool isFree2Profile(const DeviceProfiles::DeviceProfile* profile) {
  if (profile == nullptr || profile->name == nullptr) {
    return false;
  }

  return strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0 ||
         strcmp(profile->name, "Free3-M") == 0;
}

static bool isFreePageTurnerName(const std::string& name) {
  return containsCaseInsensitive(name, "free2") ||
         containsCaseInsensitive(name, "free 2") ||
         containsCaseInsensitive(name, "free-2") ||
         containsCaseInsensitive(name, "free3") ||
         containsCaseInsensitive(name, "free 3") ||
         containsCaseInsensitive(name, "free-3");
}

static bool isKnownPairableRemoteName(const std::string& name) {
  if (isUnknownOrEmptyName(name)) {
    return false;
  }

  return isFreePageTurnerName(name) ||
         (containsCaseInsensitive(name, "game") && containsCaseInsensitive(name, "brick")) ||
         containsCaseInsensitive(name, "iine") ||
         (containsCaseInsensitive(name, "mini") && containsCaseInsensitive(name, "keyboard")) ||
         containsCaseInsensitive(name, "kobo");
}

static bool isFreePageTurnerDevice(const ConnectedDevice& device) {
  return isFree2Profile(device.profile) ||
         isFreePageTurnerName(device.name);
}

static bool isProfilePageCode(const DeviceProfiles::DeviceProfile* profile, uint8_t code) {
  return profile && (code == profile->pageUpCode || code == profile->pageDownCode);
}

static ExtractedHIDKey extractProfileOrGenericKeycode(const ConnectedDevice* device, const uint8_t* report,
                                                      size_t length) {
  ExtractedHIDKey result;
  if (!device || !device->profile || !report || length == 0) {
    return result;
  }

  const auto* profile = device->profile;
  if (length > profile->reportByteIndex) {
    const uint8_t code = report[profile->reportByteIndex];
    if (isProfilePageCode(profile, code) || DeviceProfiles::isCommonPageTurnCode(code)) {
      result.keycode = code;
      result.reportIndex = profile->reportByteIndex;
      return result;
    }
  }

  // Some BLE remotes prepend a report ID or use a compact consumer report.
  // For non-strict profiles (Free3, mini keyboards, generic remotes), scan the
  // short report before giving up on a name-matched profile. Skip byte 0 first
  // when there are multiple bytes so a report ID of 0x01 is not mistaken for a
  // Free3 key.
  const size_t scanLen = length < 8 ? length : 8;
  const size_t firstPassStart = scanLen > 1 ? 1 : 0;
  for (size_t i = firstPassStart; i < scanLen; i++) {
    const uint8_t code = report[i];
    if (isProfilePageCode(profile, code)) {
      result.keycode = code;
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  const ExtractedHIDKey generic = extractGenericPageTurnKeycode(report, length);
  if (generic.keycode != 0x00 && generic.keycode != 0xFF) {
    return generic;
  }

  if (length > profile->reportByteIndex) {
    result.keycode = report[profile->reportByteIndex];
    result.reportIndex = profile->reportByteIndex;
  }
  return result;
}

static ReportMapHints parseReportMapHints(const std::string& map) {
  ReportMapHints hints;
  if (map.empty()) {
    return hints;
  }

  for (size_t i = 0; i + 1 < map.size(); i++) {
    const uint8_t b = static_cast<uint8_t>(map[i]);
    const uint8_t next = static_cast<uint8_t>(map[i + 1]);

    // Usage Page (1 byte value)
    if (b == 0x05) {
      if (next == 0x0C) {
        hints.hasConsumerPage = true;
      } else if (next == 0x07) {
        hints.hasKeyboardPage = true;
      }
    }
  }

  // Heuristic preferred byte index:
  // keyboard-like reports commonly place keycode at byte[2], consumer-control
  // reports are often compact and keycode-like values appear at byte[1].
  if (hints.hasKeyboardPage) {
    hints.preferredByteIndex = 2;
  } else if (hints.hasConsumerPage) {
    hints.preferredByteIndex = 1;
  }

  return hints;
}

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      LOG_ERR("BT", "onResult called but g_instance is NULL!");
    }
  }
  
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    LOG_INF("BT", "Client connected: %s", pClient->getPeerAddress().toString().c_str());
    BluetoothDiagnostics::recordf("client_connect", "addr=%s", pClient->getPeerAddress().toString().c_str());
  }
  
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    const auto address = pClient->getPeerAddress().toString();
    LOG_ERR("BT", "Client disconnected: %s (reason: %d)", address.c_str(), reason);
    BluetoothDiagnostics::recordf("client_disconnect", "addr=%s reason=%d",
                                  address.c_str(), reason);
    if (g_instance) {
      g_instance->onClientDisconnect(address.c_str(), reason);
    }
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    LOG_INF("BT", "BluetoothHIDManager instance created");
  }
  return *g_instance;
}

BluetoothHIDManager::BluetoothHIDManager() {
  LOG_DBG("BT", "BluetoothHIDManager constructor");
}

BluetoothHIDManager::~BluetoothHIDManager() {
  cleanup();
}

void BluetoothHIDManager::ensureReconnectMutex() {
  if (!_reconnectMutex) {
    _reconnectMutex = xSemaphoreCreateMutex();
  }
}

void BluetoothHIDManager::ensureScanResultsMutex() const {
  if (!_scanResultsMutex) {
    _scanResultsMutex = xSemaphoreCreateMutex();
  }
}

void BluetoothHIDManager::ensureConnectedDevicesMutex() const {
  if (!_connectedDevicesMutex) {
    _connectedDevicesMutex = xSemaphoreCreateMutex();
  }
}

void BluetoothHIDManager::ensureInputEventQueue() {
  if (!_inputEventQueue) {
    _inputEventQueue = xQueueCreate(BLE_INPUT_EVENT_QUEUE_LENGTH, sizeof(QueuedHIDReport));
  }
}

void BluetoothHIDManager::ensureConnectionEventQueue() {
  if (!_connectionEventQueue) {
    _connectionEventQueue = xQueueCreate(BLE_CONNECTION_EVENT_QUEUE_LENGTH, sizeof(QueuedConnectionEvent));
  }
}

std::vector<BluetoothDevice> BluetoothHIDManager::getDiscoveredDevices() const {
  ensureScanResultsMutex();
  if (!_scanResultsMutex || xSemaphoreTake(_scanResultsMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return {};
  }

  auto devices = _discoveredDevices;
  xSemaphoreGive(_scanResultsMutex);
  return devices;
}

void BluetoothHIDManager::clearDiscoveredDevices() {
  ensureScanResultsMutex();
  if (!_scanResultsMutex || xSemaphoreTake(_scanResultsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  _discoveredDevices.clear();
  xSemaphoreGive(_scanResultsMutex);
}

size_t BluetoothHIDManager::discoveredDeviceCount() const {
  ensureScanResultsMutex();
  if (!_scanResultsMutex || xSemaphoreTake(_scanResultsMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return 0;
  }

  const size_t count = _discoveredDevices.size();
  xSemaphoreGive(_scanResultsMutex);
  return count;
}

void BluetoothHIDManager::initializeAutoReconnectGuard() {
  if (_autoReconnectGuardInitialized) {
    return;
  }

  _autoReconnectGuardInitialized = true;
  _autoReconnectGuardPresent = false;
  const esp_reset_reason_t resetReason = esp_reset_reason();

  if (Storage.ready()) {
    _autoReconnectGuardPresent = Storage.exists(BLE_AUTO_RECONNECT_GUARD_FILE);
  }

  if (_autoReconnectGuardPresent || resetReasonSuggestsCrash(resetReason)) {
    _autoReconnectDisabledThisBoot = true;
    _autoReconnectArmed = false;
    BluetoothDiagnostics::recordf("auto_reconnect_guard_disable", "guard=%d reset=%d",
                                  _autoReconnectGuardPresent, static_cast<int>(resetReason));
  } else {
    _autoReconnectDisabledThisBoot = false;
  }
}

void BluetoothHIDManager::setAutoReconnectGuard(bool active) {
  if (!Storage.ready()) {
    return;
  }

  Storage.mkdir("/.crosspoint");
  if (active) {
    const String content = String("active bootMs=") + String(millis()) + "\n";
    if (Storage.writeFile(BLE_AUTO_RECONNECT_GUARD_FILE, content)) {
      _autoReconnectGuardPresent = true;
    }
  } else if (Storage.exists(BLE_AUTO_RECONNECT_GUARD_FILE)) {
    Storage.remove(BLE_AUTO_RECONNECT_GUARD_FILE);
    _autoReconnectGuardPresent = false;
  }
}

void BluetoothHIDManager::setAutoReconnectWakeRequest(bool active) {
  if (!Storage.ready()) {
    return;
  }

  Storage.mkdir("/.crosspoint");
  if (active) {
    const String content = String("wake bootMs=") + String(millis()) + "\n";
    Storage.writeFile(BLE_AUTO_RECONNECT_WAKE_FILE, content);
  } else if (Storage.exists(BLE_AUTO_RECONNECT_WAKE_FILE)) {
    Storage.remove(BLE_AUTO_RECONNECT_WAKE_FILE);
  }
}

bool BluetoothHIDManager::consumeAutoReconnectWakeRequest() {
  if (!Storage.ready() || !Storage.exists(BLE_AUTO_RECONNECT_WAKE_FILE)) {
    return false;
  }

  Storage.remove(BLE_AUTO_RECONNECT_WAKE_FILE);
  BluetoothDiagnostics::record("auto_reconnect_wake_request_consumed");
  return true;
}

void BluetoothHIDManager::armAutoReconnect(const char* reason, bool clearCrashGuard) {
  if (_bondedDeviceAddress.empty()) {
    return;
  }

  if (_autoReconnectDisabledThisBoot && !clearCrashGuard) {
    BluetoothDiagnostics::recordf("auto_reconnect_arm_blocked", "reason=%s addr=%s",
                                  reason ? reason : "unknown", _bondedDeviceAddress.c_str());
    return;
  }

  _autoReconnectArmed = true;
  if (clearCrashGuard) {
    _autoReconnectDisabledThisBoot = false;
  }
  _lastReconnectAttempt = 0;
  _reconnectFailureCount = 0;
  _fastReconnectUntil = millis() + BLE_RECONNECT_FAST_WINDOW_MS;
  if (!_reconnectJobRunning || !_reconnectJobAutomatic) {
    setAutoReconnectGuard(false);
  }
  BluetoothDiagnostics::recordf("auto_reconnect_armed", "reason=%s addr=%s",
                                reason ? reason : "unknown", _bondedDeviceAddress.c_str());
}

void BluetoothHIDManager::markIntentionalDisconnect(const std::string& address) {
  if (address.empty()) {
    _intentionalDisconnectAddress[0] = '\0';
    return;
  }

  strncpy(_intentionalDisconnectAddress, address.c_str(), sizeof(_intentionalDisconnectAddress) - 1);
  _intentionalDisconnectAddress[sizeof(_intentionalDisconnectAddress) - 1] = '\0';
  _intentionalDisconnectMarkedAt = millis();
}

bool BluetoothHIDManager::consumeIntentionalDisconnect(const char* address) {
  if (!address || !*address || _intentionalDisconnectAddress[0] == '\0') {
    return false;
  }

  if (_intentionalDisconnectMarkedAt != 0 &&
      millis() - _intentionalDisconnectMarkedAt > BLE_INTENTIONAL_DISCONNECT_SUPPRESS_MS) {
    _intentionalDisconnectAddress[0] = '\0';
    _intentionalDisconnectMarkedAt = 0;
    return false;
  }

  if (strncmp(_intentionalDisconnectAddress, address, sizeof(_intentionalDisconnectAddress)) != 0) {
    return false;
  }

  _intentionalDisconnectAddress[0] = '\0';
  _intentionalDisconnectMarkedAt = 0;
  return true;
}

bool BluetoothHIDManager::bondedDeviceLooksLikePageTurner() const {
  if (isFreePageTurnerName(_bondedDeviceName)) {
    return true;
  }

  const auto* profile = DeviceProfiles::findDeviceProfile(_bondedDeviceAddress.c_str(),
                                                          _bondedDeviceName.c_str());
  return isFree2Profile(profile);
}

bool BluetoothHIDManager::pageTurnerReconnectModeActive(unsigned long now) {
  if (_pageTurnerReconnectUntil == 0) {
    return false;
  }

  if (static_cast<int32_t>(now - _pageTurnerReconnectUntil) >= 0) {
    _pageTurnerReconnectUntil = 0;
    _pageTurnerReconnectFastUntil = 0;
    BluetoothDiagnostics::record("page_turner_reconnect_window_expired");
    return false;
  }

  return true;
}

bool BluetoothHIDManager::pageTurnerReconnectFastModeActive(unsigned long now) {
  if (!pageTurnerReconnectModeActive(now) || _pageTurnerReconnectFastUntil == 0) {
    return false;
  }

  if (static_cast<int32_t>(now - _pageTurnerReconnectFastUntil) >= 0) {
    _pageTurnerReconnectFastUntil = 0;
    BluetoothDiagnostics::record("page_turner_reconnect_fast_window_expired");
    return false;
  }

  return true;
}

void BluetoothHIDManager::enterPageTurnerReconnectMode(const char* reason, bool confirmedPageTurner) {
  if (!confirmedPageTurner && !bondedDeviceLooksLikePageTurner()) {
    return;
  }

  const unsigned long now = millis();
  _pageTurnerReconnectUntil = now + BLE_PAGE_TURNER_RECONNECT_WINDOW_MS;
  _pageTurnerReconnectFastUntil = now + BLE_PAGE_TURNER_RECONNECT_FAST_WINDOW_MS;
  BluetoothDiagnostics::recordf("page_turner_reconnect_window", "reason=%s confirmed=%d fastUntilMs=%lu untilMs=%lu",
                                reason ? reason : "unknown", confirmedPageTurner,
                                _pageTurnerReconnectFastUntil, _pageTurnerReconnectUntil);
}

void BluetoothHIDManager::armAutoReconnectOnNextWake() {
  if (_bondedDeviceAddress.empty()) {
    return;
  }

  setAutoReconnectWakeRequest(true);
  BluetoothDiagnostics::recordf("auto_reconnect_wake_request_set", "addr=%s name=%s",
                                _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
  BluetoothDiagnostics::flushToStorage();
}

void BluetoothHIDManager::cleanup() {
  if (_enabled) {
    disable();
  }
}

bool BluetoothHIDManager::enable() {
  if (_enabled) {
    LOG_DBG("BT", "Already enabled");
    return true;
  }
  
  LOG_INF("BT", "Enabling Bluetooth...");
  BluetoothDiagnostics::record("enable_start");
  BluetoothDiagnostics::flushToStorage();
  
  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi to enable Bluetooth (mutual exclusion)");
    BluetoothDiagnostics::record("wifi_off_for_ble");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }
  
  // Initialize NimBLE stack
  BluetoothDiagnostics::record("nimble_init_start");
  BluetoothDiagnostics::flushToStorage();
  NimBLEDevice::init("CrossPoint");
  BluetoothDiagnostics::record("nimble_init_ok");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityAuth(true, false, false);
  
  _enabled = true;
  lastError = "";
  _lastReconnectCheck = 0;
  _lastReconnectAttempt = 0;
  _btEnabledAt = millis();
  _fastReconnectUntil = 0;
  _reconnectFailureCount = 0;
  _pageTurnerReconnectUntil = 0;
  _pageTurnerReconnectFastUntil = 0;
  _autoReconnectArmed = false;
  _autoReconnectWakeRequestedThisBoot = false;
  _intentionalDisconnectAddress[0] = '\0';
  _intentionalDisconnectMarkedAt = 0;
  ensureInputEventQueue();
  if (_inputEventQueue) {
    xQueueReset(_inputEventQueue);
  }
  ensureConnectionEventQueue();
  if (_connectionEventQueue) {
    xQueueReset(_connectionEventQueue);
  }
  initializeAutoReconnectGuard();
  
  LOG_INF("BT", "Bluetooth enabled successfully");
  BluetoothDiagnostics::record("enable_ok");
  BluetoothDiagnostics::flushToStorage();
  loadState();
  const bool wakeRequest = consumeAutoReconnectWakeRequest();
  if (wakeRequest && !_autoReconnectDisabledThisBoot) {
    _autoReconnectWakeRequestedThisBoot = true;
    if (bondedDeviceLooksLikePageTurner()) {
      enterPageTurnerReconnectMode("wake_request", false);
    }
    armAutoReconnect("wake_request");
  } else if (wakeRequest) {
    BluetoothDiagnostics::record("auto_reconnect_wake_request_blocked_by_guard");
  }
  return true;
}

bool BluetoothHIDManager::disable() {
  if (!_enabled) {
    LOG_DBG("BT", "Already disabled");
    return true;
  }

  if (isBondedReconnectInProgress()) {
    lastError = "Reconnect in progress";
    LOG_INF("BT", "Bluetooth disable skipped: reconnect in progress");
    return false;
  }
  
  LOG_INF("BT", "Disabling Bluetooth...");
  size_t connectedCount = 0;
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(50));
    if (lock.locked()) {
      connectedCount = _connectedDevices.size();
    }
  }
  BluetoothDiagnostics::recordf("disable_start", "connected=%u scanning=%d",
                                static_cast<unsigned>(connectedCount), _scanning);
  BluetoothDiagnostics::flushToStorage();
  
  if (_scanning) {
    stopScan();
  }

  if (_inputEventQueue) {
    xQueueReset(_inputEventQueue);
  }
  
  // Disconnect all devices
  while (true) {
    std::string address;
    ensureConnectedDevicesMutex();
    if (_connectedDevicesMutex) {
      SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(50));
      if (lock.locked() && !_connectedDevices.empty()) {
        address = _connectedDevices[0].address;
      }
    }
    if (address.empty()) {
      break;
    }
    disconnectFromDevice(address);
  }
  
  // Deinitialize NimBLE stack
  NimBLEDevice::deinit(false);
  
  _enabled = false;
  lastError = "";
  _lastReconnectCheck = 0;
  _lastReconnectAttempt = 0;
  _fastReconnectUntil = 0;
  _reconnectFailureCount = 0;
  _pageTurnerReconnectUntil = 0;
  _pageTurnerReconnectFastUntil = 0;
  _btEnabledAt = 0;
  _autoReconnectArmed = false;
  _intentionalDisconnectAddress[0] = '\0';
  _intentionalDisconnectMarkedAt = 0;
  
  LOG_INF("BT", "Bluetooth disabled");
  BluetoothDiagnostics::record("disable_ok");
  BluetoothDiagnostics::flushToStorage();
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs) {
  if (!_enabled || _scanning) {
    LOG_DBG("BT", "Cannot scan: enabled=%d scanning=%d", _enabled, _scanning);
    BluetoothDiagnostics::recordf("scan_skip", "enabled=%d scanning=%d", _enabled, _scanning);
    BluetoothDiagnostics::flushToStorage();
    return;
  }
  
  if (durationMs == 0) {
    durationMs = 5000;
  }

  LOG_INF("BT", "Starting finite passive BLE scan for %lu ms", durationMs);
  BluetoothDiagnostics::recordf("scan_start", "durationMs=%lu", static_cast<unsigned long>(durationMs));
  BluetoothDiagnostics::flushToStorage();
  _scanning = true;
  clearDiscoveredDevices();
  
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    LOG_ERR("BT", "Failed to get scan object");
    BluetoothDiagnostics::record("scan_get_object_failed");
    BluetoothDiagnostics::flushToStorage();
    _scanning = false;
    lastError = "Scan failed";
    return;
  }
  
  // Use finite, passive, low-duty scanning. Continuous active scans were
  // unstable on the X3 BLE stack and could reboot the OS.
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(false);
  pScan->setDuplicateFilter(true);
  pScan->setMaxResults(16);
  pScan->setInterval(220);
  pScan->setWindow(30);
  
  bool started = pScan->start(durationMs, false);
  
  if (!started) {
    LOG_ERR("BT", "Failed to start scan!");
    BluetoothDiagnostics::record("scan_start_failed");
    BluetoothDiagnostics::flushToStorage();
    _scanning = false;
    lastError = "Scan failed";
    return;
  }
  
  _scanStopTime = millis() + durationMs + 1000;
  LOG_INF("BT", "Scan started asynchronously");
  BluetoothDiagnostics::record("scan_started");
  BluetoothDiagnostics::flushToStorage();
}

void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;
  
  LOG_INF("BT", "Stopping scan");
  
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan && pScan->isScanning()) {
    pScan->stop();
  }
  
  _scanStopTime = 0;
  _scanning = false;
  const size_t found = discoveredDeviceCount();
  LOG_INF("BT", "Scan complete, found %d devices", static_cast<int>(found));
  BluetoothDiagnostics::recordf("scan_stop", "found=%u", static_cast<unsigned>(found));
  BluetoothDiagnostics::flushToStorage();
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;
  
  std::string address = advertisedDevice->getAddress().toString();
  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();
  uint8_t addressType = advertisedDevice->getAddressType();
  bool connectable = advertisedDevice->isConnectable();
  
  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));

  BluetoothDevice loggedDevice;
  bool shouldLog = false;

  ensureScanResultsMutex();
  if (!_scanResultsMutex || xSemaphoreTake(_scanResultsMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      dev.addressType = addressType;
      if (!name.empty() && dev.name == "Unknown") dev.name = name;
      if (isHID) dev.isHID = true;
      if (connectable) dev.connectable = true;
      if (_reconnectJobRunning && dev.connectable) {
        if (_reconnectJobPairNew) {
          if (isHighConfidencePairingCandidate(dev)) {
            _reconnectScanMatched = true;
          }
        } else if (matchesBondedReconnectCandidate(dev, _bondedDeviceAddress, _bondedDeviceName)) {
          _reconnectScanMatched = true;
        }
      }
      loggedDevice = dev;
      xSemaphoreGive(_scanResultsMutex);
      return;
    }
  }
  
  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.addressType = addressType;
  device.isHID = isHID;
  device.connectable = connectable;
  
  _discoveredDevices.push_back(device);

  if (_reconnectJobRunning && device.connectable) {
    if (_reconnectJobPairNew) {
      if (isHighConfidencePairingCandidate(device)) {
        _reconnectScanMatched = true;
      }
    } else if (matchesBondedReconnectCandidate(device, _bondedDeviceAddress, _bondedDeviceName)) {
      _reconnectScanMatched = true;
    }
  }

  loggedDevice = device;
  shouldLog = true;
  xSemaphoreGive(_scanResultsMutex);

  if (shouldLog) {
    LOG_DBG("BT", "Found device: %s (%s) type:%u RSSI:%d HID:%d conn:%d",
            loggedDevice.name.c_str(), loggedDevice.address.c_str(), static_cast<unsigned>(addressType), rssi, isHID,
            connectable);
  }
}

bool BluetoothHIDManager::connectToDevice(const std::string& address, uint32_t timeoutMs, uint8_t addressType,
                                          const std::string& nameHint) {
  if (!_enabled) {
    LOG_ERR("BT", "Cannot connect: Bluetooth not enabled");
    BluetoothDiagnostics::recordf("connect_skip", "addr=%s reason=disabled", address.c_str());
    BluetoothDiagnostics::flushToStorage();
    lastError = "Bluetooth not enabled";
    return false;
  }
  
  // Check if already connected
  if (isConnected(address)) {
    LOG_INF("BT", "Already connected to %s", address.c_str());
    BluetoothDiagnostics::recordf("connect_skip", "addr=%s reason=already_connected", address.c_str());
    return true;
  }
  
  if (timeoutMs == 0) {
    timeoutMs = BLE_CONNECT_TIMEOUT_MS;
  }

  uint8_t resolvedAddressType = addressType;
  if (resolvedAddressType == BLE_ADDR_TYPE_UNKNOWN) {
    if (!_bondedDeviceAddress.empty() && _bondedDeviceAddress == address) {
      resolvedAddressType = _bondedDeviceAddressType;
    }
    const auto discoveredDevices = getDiscoveredDevices();
    for (const auto& dev : discoveredDevices) {
      if (dev.address == address) {
        resolvedAddressType = dev.addressType;
        break;
      }
    }
  }
  const std::string connectionNameHint = !nameHint.empty() ? nameHint : _bondedDeviceName;
  resolvedAddressType = normalizeAddressType(resolvedAddressType, address, connectionNameHint);

  LOG_INF("BT", "Connecting to device %s (type=%u timeout=%lu ms)", address.c_str(),
          static_cast<unsigned>(resolvedAddressType), static_cast<unsigned long>(timeoutMs));
  BluetoothDiagnostics::recordf("connect_start", "addr=%s type=%u timeoutMs=%lu", address.c_str(),
                                static_cast<unsigned>(resolvedAddressType),
                                static_cast<unsigned long>(timeoutMs));
  BluetoothDiagnostics::flushToStorage();
  
    const auto addressTypeAttempts =
        connectionAddressTypeCandidates(resolvedAddressType, address, connectionNameHint);
    const uint32_t attemptTimeoutMs = perAttemptConnectTimeout(timeoutMs, addressTypeAttempts.size());
    NimBLEClient* pClient = nullptr;
    int lastConnectError = 0;
    bool connected = false;

    static ClientCallbacks clientCallbacks;

    for (uint8_t attemptType : addressTypeAttempts) {
      NimBLEAddress bleAddress(address, attemptType);
      BluetoothDiagnostics::recordf("connect_attempt", "addr=%s type=%u timeoutMs=%lu", address.c_str(),
                                    static_cast<unsigned>(attemptType),
                                    static_cast<unsigned long>(attemptTimeoutMs));
      BluetoothDiagnostics::flushToStorage();

      if (NimBLEClient* staleClient = NimBLEDevice::getClientByPeerAddress(bleAddress)) {
        if (!staleClient->isConnected()) {
          staleClient->deleteServices();
          NimBLEDevice::deleteClient(staleClient);
        }
      }

      pClient = NimBLEDevice::createClient(bleAddress);
      if (!pClient) {
        pClient = NimBLEDevice::getDisconnectedClient();
        if (pClient) {
          pClient->setPeerAddress(bleAddress);
        }
      }

      if (!pClient) {
        lastConnectError = BLE_HS_ENOMEM;
        BluetoothDiagnostics::recordf("connect_attempt_failed", "addr=%s type=%u reason=create_client",
                                      address.c_str(), static_cast<unsigned>(attemptType));
        continue;
      }

      // Keep client lifetime under manager control so disconnect callbacks do not free it in NimBLE context.
      pClient->setSelfDelete(false, false);
      pClient->setConnectTimeout(attemptTimeoutMs);
      pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                                   BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
      pClient->deleteServices();
      pClient->setClientCallbacks(&clientCallbacks, false);
      NimBLEDevice::deleteBond(bleAddress);

      if (pClient->connect(bleAddress, true, false, false)) {
        resolvedAddressType = attemptType;
        connected = true;
        break;
      }

      lastConnectError = pClient->getLastError();
      BluetoothDiagnostics::recordf("connect_attempt_failed", "addr=%s type=%u err=%d",
                                    address.c_str(), static_cast<unsigned>(attemptType), lastConnectError);
      BluetoothDiagnostics::flushToStorage();

      if (!pClient->isConnected()) {
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
      }
    }

    if (!connected || !pClient) {
      char errBuf[64];
      if (lastConnectError == BLE_HS_ETIMEOUT) {
        snprintf(errBuf, sizeof(errBuf), "Connection timed out");
      } else {
        snprintf(errBuf, sizeof(errBuf), "Connection failed (%d)", lastConnectError);
      }
      lastError = errBuf;
      LOG_ERR("BT", "Failed to connect to %s", address.c_str());
      BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=connect err=%d",
                                    address.c_str(), lastConnectError);
      BluetoothDiagnostics::flushToStorage();
      return false;
    }

    const auto discoveredDevices = getDiscoveredDevices();
    
    // Get HID service
    BluetoothDiagnostics::recordf("connect_hid_service_start", "addr=%s", address.c_str());
    BluetoothDiagnostics::flushToStorage();
    NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
    if (!pService) {
      lastError = "HID service not found";
      LOG_ERR("BT", "Device %s doesn't have HID service", address.c_str());
      BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=no_hid_service", address.c_str());
      BluetoothDiagnostics::flushToStorage();
      pClient->disconnect();
      return false;
    }
    BluetoothDiagnostics::recordf("connect_hid_service_ok", "addr=%s", address.c_str());
    BluetoothDiagnostics::flushToStorage();

    ReportMapHints reportHints;
    
    LOG_INF("BT", "Found HID service, enumerating report characteristics...");
    
    // BLE HID has multiple report characteristics (input, output, feature)
    // We need to find one that supports NOTIFY or INDICATE (input report)
    // In NimBLE 2.x, getCharacteristics() returns std::vector<NimBLERemoteCharacteristic*>
    int reportCount = 0;
    std::vector<NimBLERemoteCharacteristic*> reportChars;

    BluetoothDiagnostics::recordf("connect_report_discovery_start", "addr=%s minimal=0",
                                  address.c_str());
    BluetoothDiagnostics::flushToStorage();
    auto pCharacteristics = pService->getCharacteristics(true);
    for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
      auto* pChar = *it;
      LOG_DBG("BT", "Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
              pChar->getUUID().toString().c_str(),
              pChar->canRead(), pChar->canWrite(), pChar->canNotify(), pChar->canIndicate());

      if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
        reportCount++;

        // Check if this report supports notify or indicate (input report)
        if (pChar->canNotify() || pChar->canIndicate()) {
          reportChars.push_back(pChar);
          LOG_INF("BT", "Added Report char #%d for subscription", reportCount);
        }
      }
    }
    BluetoothDiagnostics::recordf("connect_report_discovery_done", "addr=%s reports=%u",
                                  address.c_str(), static_cast<unsigned>(reportChars.size()));
    BluetoothDiagnostics::flushToStorage();
    
    if (reportChars.empty()) {
      lastError = "No input report characteristic found";
      LOG_ERR("BT", "No Report characteristic with notify/indicate found");
      BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=no_input_reports reports=%u", address.c_str(),
                                    static_cast<unsigned>(reportCount));
      BluetoothDiagnostics::flushToStorage();
      pClient->disconnect();
      return false;
    }
    
    // Subscribe to ALL Report characteristics with notify capability
    LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
    size_t successfulSubscriptions = 0;
    
    for (size_t i = 0; i < reportChars.size(); i++) {
      auto* pChar = reportChars[i];
      
      // Use notifications when available, otherwise indications.
      const bool useNotify = pChar->canNotify();
      BluetoothDiagnostics::recordf("connect_subscribe_start", "addr=%s index=%u notify=%d",
                                    address.c_str(), static_cast<unsigned>(i + 1), useNotify);
      BluetoothDiagnostics::flushToStorage();
      bool subResult = pChar->subscribe(useNotify, onHIDNotify);
      LOG_INF("BT", "Report char #%d subscribe (%s) result: %d", i + 1, useNotify ? "notify" : "indicate",
              subResult);
      if (subResult) {
        successfulSubscriptions++;
      }
      
      if (!subResult) {
        LOG_INF("BT", "Failed to subscribe to Report char #%d (continuing)", i + 1);
      }
    }
    BluetoothDiagnostics::recordf("connect_subscribe_done", "addr=%s ok=%u total=%u",
                                  address.c_str(), static_cast<unsigned>(successfulSubscriptions),
                                  static_cast<unsigned>(reportChars.size()));
    BluetoothDiagnostics::flushToStorage();

    if (successfulSubscriptions == 0) {
      lastError = "Failed to subscribe to input reports";
      LOG_ERR("BT", "No HID report subscriptions succeeded for %s", address.c_str());
      BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=subscribe reports=%u", address.c_str(),
                                    static_cast<unsigned>(reportChars.size()));
      BluetoothDiagnostics::flushToStorage();
      pClient->disconnect();
      return false;
    }
    
    LOG_INF("BT", "Subscribed to %u/%u HID Report characteristics",
            static_cast<unsigned>(successfulSubscriptions), static_cast<unsigned>(reportChars.size()));
    
    // Save connection with activity timestamp
    ConnectedDevice connDev;
    connDev.address = address;
    connDev.client = pClient;
    connDev.reportChars = reportChars;
    connDev.connectedTime = millis();
    connDev.subscribed = true;
    connDev.lastActivityTime = millis();  // Initialize activity timer
    connDev.wasConnected = true;  // Mark for auto-reconnect if disconnected
    connDev.descriptorHasKeyboardPage = reportHints.hasKeyboardPage;
    connDev.descriptorHasConsumerPage = reportHints.hasConsumerPage;
    connDev.descriptorSuggestedIndex = reportHints.preferredByteIndex;
    
    // Detect device profile
    // First, try to find the device in scan results to get its name
    bool foundInScan = false;
    for (const auto& dev : discoveredDevices) {
      if (dev.address == address) {
        connDev.name = dev.name;
        foundInScan = true;
        LOG_INF("BT", "Device found in scan results: %s (%s)", dev.name.c_str(), address.c_str());
        break;
      }
    }
    
    if (!foundInScan) {
      LOG_INF("BT", "Device not in scan results (may be previously paired): %s", address.c_str());
      if (!nameHint.empty() && nameHint != "Unknown") {
        connDev.name = nameHint;
        LOG_INF("BT", "Using pairing name hint: %s", connDev.name.c_str());
      } else if (connDev.name.empty() && !_bondedDeviceAddress.empty() && _bondedDeviceAddress == address &&
          !_bondedDeviceName.empty()) {
        connDev.name = _bondedDeviceName;
        LOG_INF("BT", "Using bonded device name hint: %s", connDev.name.c_str());
      }
    }
    
    // Profile matching priority:
    //  1. MAC-prefix exact match  (hardware ID, precise – always wins)
    //  2. Per-device learned profile by full MAC address
    //  3. User-learned global custom profile (explicitly taught by the user)
    //     → only if the name-matched known profile is NOT marked strictProfile
    //  4. Fuzzy name-pattern match  (last resort – can produce false positives)
    connDev.profile = DeviceProfiles::findDeviceProfile(address.c_str(), nullptr);

    DeviceProfiles::DeviceProfile perDeviceProfile;
    const bool hasPerDeviceProfile = DeviceProfiles::getCustomProfileForDevice(address, perDeviceProfile);

    if (!connDev.profile) {
      // Check if a name-matched profile exists and whether it is strict.
      const DeviceProfiles::DeviceProfile* nameMatch =
          DeviceProfiles::findDeviceProfile(nullptr, connDev.name.c_str());
      const bool nameMatchIsStrict = nameMatch && nameMatch->strictProfile;

      if (hasPerDeviceProfile && !nameMatchIsStrict) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleBackKeycode = perDeviceProfile.pageUpCode;
        connDev.simpleForwardKeycode = perDeviceProfile.pageDownCode;
        LOG_INF("BT", "Using per-device learned profile for %s: up=0x%02X down=0x%02X idx=%u", address.c_str(),
          perDeviceProfile.pageUpCode, perDeviceProfile.pageDownCode,
          static_cast<unsigned>(perDeviceProfile.reportByteIndex));
      }

      // Prefer the user's learned mapping over a non-strict name-based guess.
      const auto* customProfile = DeviceProfiles::getCustomProfile();
      if (!connDev.profile && customProfile && !nameMatchIsStrict) {
        connDev.profile = customProfile;
        LOG_INF("BT", "Using learned custom profile (overrides non-strict name match): up=0x%02X dn=0x%02X",
                customProfile->pageUpCode, customProfile->pageDownCode);
      } else if (!connDev.profile && nameMatch) {
        connDev.profile = nameMatch;
        if (nameMatchIsStrict) {
          LOG_INF("BT", "Using strict name-matched profile '%s' (custom profile bypassed)",
                  nameMatch->name);
        } else {
          LOG_INF("BT", "Using name-matched profile '%s' (no custom profile set)", nameMatch->name);
        }
      }
    }
    
    if (connDev.profile) {
      LOG_INF("BT", "✓ Using device profile: %s (byte[%d] for keycode)", 
              connDev.profile->name, connDev.profile->reportByteIndex);
      connDev.simpleFallbackEnabled = false;
    } else {
      LOG_INF("BT", "No known profile matched for %s, will auto-detect from HID codes", address.c_str());
      if (!connDev.simpleFallbackEnabled) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleForwardKeycode = 0x00;
        connDev.simpleBackKeycode = 0x00;
      }
      LOG_INF("BT", "Simple fallback enabled for unknown device %s", address.c_str());
    }
    
    ensureConnectedDevicesMutex();
    if (!_connectedDevicesMutex) {
      BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=connected_mutex", address.c_str());
      lastError = "Connection state unavailable";
      return false;
    } else {
      SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(200));
      if (lock.locked()) {
        auto existing = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                                     [&address](const ConnectedDevice& dev) { return dev.address == address; });
        if (existing != _connectedDevices.end()) {
          *existing = connDev;
        } else {
          _connectedDevices.push_back(connDev);
        }
      } else {
        BluetoothDiagnostics::recordf("connect_failed", "addr=%s reason=connected_lock", address.c_str());
        lastError = "Connection state busy";
        return false;
      }
    }
    if (_bondedDeviceAddress == address) {
      _bondedDeviceAddressType = resolvedAddressType;
      _fastReconnectUntil = 0;
      _reconnectFailureCount = 0;
      _pageTurnerReconnectUntil = 0;
      _pageTurnerReconnectFastUntil = 0;
      armAutoReconnect("connect_success");
    }
    
    LOG_INF("BT", "Successfully connected to %s", address.c_str());
    BluetoothDiagnostics::recordf("connect_ok", "addr=%s reports=%u profile=%s", address.c_str(),
                                  static_cast<unsigned>(successfulSubscriptions),
                                  connDev.profile ? connDev.profile->name : "fallback");
    BluetoothDiagnostics::flushToStorage();
    lastError = "Connected";
    return true;
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  LOG_INF("BT", "Disconnecting from device %s", address.c_str());
  BluetoothDiagnostics::recordf("disconnect_start", "addr=%s", address.c_str());
  BluetoothDiagnostics::flushToStorage();

  NimBLEClient* client = nullptr;
  bool found = false;
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock stateLock(_connectedDevicesMutex, pdMS_TO_TICKS(200));
    if (!stateLock.locked()) {
      lastError = "Connection state busy";
      return false;
    }

    auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
      [&address](const ConnectedDevice& dev) { return dev.address == address; });

    if (it != _connectedDevices.end()) {
      found = true;
      if (_buttonInjector && it->activeInjectedButton != 0xFF) {
        _buttonInjector(it->activeInjectedButton, false);
      }
      client = it->client;
      _connectedDevices.erase(it);
    }
  }

  if (found) {
    const bool disconnectsBondedDevice = !_bondedDeviceAddress.empty() && address == _bondedDeviceAddress;
    if (disconnectsBondedDevice) {
      _autoReconnectArmed = false;
      _fastReconnectUntil = 0;
      _pageTurnerReconnectUntil = 0;
      _pageTurnerReconnectFastUntil = 0;
      BluetoothDiagnostics::recordf("auto_reconnect_disarmed", "reason=explicit_disconnect addr=%s",
                                    address.c_str());
    }

    // Ensure normal CPU speed during BLE termination to avoid WDT in low-power mode.
    if (client && client->isConnected()) {
      HalPowerManager::Lock lock;
      markIntentionalDisconnect(address);
      client->disconnect();
    }

    LOG_INF("BT", "Disconnected from %s", address.c_str());
    BluetoothDiagnostics::recordf("disconnect_ok", "addr=%s", address.c_str());
    BluetoothDiagnostics::flushToStorage();
    lastError = "Disconnected";
    return true;
  }
  
  LOG_INF("BT", "Device %s not in connected list", address.c_str());
  BluetoothDiagnostics::recordf("disconnect_skip", "addr=%s reason=not_connected", address.c_str());
  BluetoothDiagnostics::flushToStorage();
  return false;
}

bool BluetoothHIDManager::isConnected(const std::string& address) const {
  ensureConnectedDevicesMutex();
  if (!_connectedDevicesMutex) {
    return false;
  }

  SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(50));
  if (!lock.locked()) {
    return false;
  }

  return std::find_if(_connectedDevices.begin(), _connectedDevices.end(), [&address](const ConnectedDevice& dev) {
           return dev.address == address && dev.client && dev.client->isConnected();
         }) != _connectedDevices.end();
}

std::vector<std::string> BluetoothHIDManager::getConnectedDevices() const {
  if (isBondedReconnectInProgress()) {
    return {};
  }

  std::vector<std::string> addresses;
  ensureConnectedDevicesMutex();
  if (!_connectedDevicesMutex) {
    return addresses;
  }

  SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(50));
  if (!lock.locked()) {
    return addresses;
  }

  for (const auto& dev : _connectedDevices) {
    if (dev.client && dev.client->isConnected()) {
      addresses.push_back(dev.address);
    }
  }
  return addresses;
}

bool BluetoothHIDManager::startPairNewRemote(uint32_t timeoutMs) {
  ensureReconnectMutex();

  if (!_reconnectMutex || xSemaphoreTake(_reconnectMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    lastError = "Bluetooth busy";
    return false;
  }

  if (_reconnectJobRunning) {
    _reconnectJobMessage = _reconnectJobPairNew ? "Pairing already running" : "Reconnect already running";
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  _reconnectJobFinished = false;
  _reconnectJobSuccess = false;
  _reconnectJobPairNew = false;

  if (!_enabled) {
    _reconnectJobMessage = "Enable BT first";
    _reconnectJobFinished = true;
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  if (_scanning) {
    _reconnectJobMessage = "Scan already running";
    _reconnectJobFinished = true;
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  _reconnectJobRunning = true;
  _reconnectJobTimeoutMs = timeoutMs == 0 ? BLE_MANUAL_RECONNECT_TIMEOUT_MS : timeoutMs;
  _reconnectJobScanMs = BLE_MANUAL_RECONNECT_SCAN_MS;
  _reconnectJobAutomatic = false;
  _reconnectJobPairNew = true;
  _reconnectJobMessage = "Pairing...";

  TaskHandle_t handle = nullptr;
  const BaseType_t created =
      xTaskCreate(&BluetoothHIDManager::bondedReconnectTaskEntry, "bt_pair", 12288, this, 1, &handle);
  if (created != pdPASS) {
    _reconnectJobRunning = false;
    _reconnectJobFinished = true;
    _reconnectJobSuccess = false;
    _reconnectJobAutomatic = false;
    _reconnectJobPairNew = false;
    _reconnectJobScanMs = 0;
    _reconnectJobMessage = "Pair task failed";
    lastError = _reconnectJobMessage;
    BluetoothDiagnostics::record("pair_new_task_failed");
    BluetoothDiagnostics::flushToStorage();
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  _reconnectTaskHandle = handle;
  lastError = _reconnectJobMessage;
  BluetoothDiagnostics::recordf("pair_new_queued", "oldAddr=%s oldName=%s timeoutMs=%lu scanMs=%lu",
                                _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str(),
                                static_cast<unsigned long>(_reconnectJobTimeoutMs),
                                static_cast<unsigned long>(_reconnectJobScanMs));
  BluetoothDiagnostics::flushToStorage();
  xSemaphoreGive(_reconnectMutex);
  return true;
}

bool BluetoothHIDManager::startBondedReconnect(uint32_t timeoutMs, bool automatic) {
  ensureReconnectMutex();

  if (!_reconnectMutex || xSemaphoreTake(_reconnectMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    lastError = "Reconnect busy";
    return false;
  }

  if (_reconnectJobRunning) {
    _reconnectJobMessage = "Reconnect already running";
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  _reconnectJobFinished = false;
  _reconnectJobSuccess = false;
  _reconnectJobPairNew = false;

  if (!_enabled) {
    _reconnectJobMessage = "Enable BT first";
    _reconnectJobFinished = true;
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  if (_bondedDeviceAddress.empty()) {
    _reconnectJobMessage = "No bonded remote";
    _reconnectJobFinished = true;
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  initializeAutoReconnectGuard();
  if (!automatic && _autoReconnectGuardPresent) {
    BluetoothDiagnostics::recordf("manual_reconnect_guard_present", "addr=%s",
                                  _bondedDeviceAddress.c_str());
    BluetoothDiagnostics::flushToStorage();
  }

  if (isConnected(_bondedDeviceAddress)) {
    _reconnectJobSuccess = true;
    _reconnectJobFinished = true;
    _reconnectJobMessage = "Already connected";
    lastError = _reconnectJobMessage;
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  const unsigned long now = millis();
  const bool pageTurnerAutoScan =
      automatic && bondedDeviceLooksLikePageTurner() && pageTurnerReconnectModeActive(now);
  _reconnectJobRunning = true;
  _reconnectJobTimeoutMs = timeoutMs == 0 ? BLE_MANUAL_RECONNECT_TIMEOUT_MS : timeoutMs;
  _reconnectJobScanMs = automatic ? (pageTurnerAutoScan ? BLE_AUTO_PAGE_TURNER_SCAN_MS : BLE_RECONNECT_SCAN_MS)
                                  : BLE_MANUAL_RECONNECT_SCAN_MS;
  _reconnectJobAutomatic = automatic;
  _reconnectJobMessage = "Reconnecting...";

  TaskHandle_t handle = nullptr;
  const BaseType_t created =
      xTaskCreate(&BluetoothHIDManager::bondedReconnectTaskEntry, "bt_reconn", 12288, this, 1, &handle);
  if (created != pdPASS) {
    _reconnectJobRunning = false;
    _reconnectJobFinished = true;
    _reconnectJobSuccess = false;
    _reconnectJobAutomatic = false;
    _reconnectJobPairNew = false;
    _reconnectJobScanMs = 0;
    _reconnectJobMessage = "Reconnect task failed";
    lastError = _reconnectJobMessage;
    BluetoothDiagnostics::record(automatic ? "auto_reconnect_task_failed" : "manual_reconnect_task_failed");
    BluetoothDiagnostics::flushToStorage();
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  _reconnectTaskHandle = handle;
  lastError = _reconnectJobMessage;
  BluetoothDiagnostics::recordf(automatic ? "auto_reconnect_queued" : "manual_reconnect_queued",
                                "addr=%s type=%u timeoutMs=%lu pageTurner=%d",
                                _bondedDeviceAddress.c_str(), static_cast<unsigned>(_bondedDeviceAddressType),
                                static_cast<unsigned long>(_reconnectJobTimeoutMs), pageTurnerAutoScan);
  BluetoothDiagnostics::flushToStorage();
  xSemaphoreGive(_reconnectMutex);
  return true;
}

BluetoothReconnectStatus BluetoothHIDManager::getReconnectStatus() {
  BluetoothReconnectStatus status;
  ensureReconnectMutex();

  if (!_reconnectMutex || xSemaphoreTake(_reconnectMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    status.state = BluetoothReconnectStatus::State::Running;
    status.message = "Reconnect busy";
    return status;
  }

  status.message = _reconnectJobMessage;
  if (_reconnectJobRunning) {
    status.state = BluetoothReconnectStatus::State::Running;
  } else if (_reconnectJobFinished) {
    status.state = _reconnectJobSuccess ? BluetoothReconnectStatus::State::Succeeded
                                        : BluetoothReconnectStatus::State::Failed;
  } else {
    status.state = BluetoothReconnectStatus::State::Idle;
  }

  xSemaphoreGive(_reconnectMutex);
  return status;
}

bool BluetoothHIDManager::consumeReconnectResult(bool& success, std::string& message) {
  ensureReconnectMutex();

  if (!_reconnectMutex || xSemaphoreTake(_reconnectMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }

  if (_reconnectJobRunning || !_reconnectJobFinished) {
    xSemaphoreGive(_reconnectMutex);
    return false;
  }

  success = _reconnectJobSuccess;
  message = _reconnectJobMessage;
  _reconnectJobFinished = false;
  _reconnectJobSuccess = false;
  if (_reconnectJobMessage != "Already connected") {
    _reconnectJobMessage.clear();
  }

  xSemaphoreGive(_reconnectMutex);
  return true;
}

void BluetoothHIDManager::bondedReconnectTaskEntry(void* param) {
  auto* manager = static_cast<BluetoothHIDManager*>(param);
  if (manager) {
    manager->runBondedReconnectTask();
  }
  vTaskDelete(nullptr);
}

void BluetoothHIDManager::runBondedReconnectTask() {
  HalPowerManager::Lock reconnectPowerLock;
  const std::string address = _bondedDeviceAddress;
  const std::string name = _bondedDeviceName;
  const uint8_t addressType = _bondedDeviceAddressType;
  const uint32_t timeoutMs = _reconnectJobTimeoutMs == 0 ? BLE_RECONNECT_USER_INTERVAL_MS : _reconnectJobTimeoutMs;
  const uint32_t scanMs = _reconnectJobScanMs == 0 ? BLE_RECONNECT_SCAN_MS : _reconnectJobScanMs;
  const bool automatic = _reconnectJobAutomatic;
  const bool pairNew = _reconnectJobPairNew;

  if (automatic) {
    setAutoReconnectGuard(true);
  }

  const char* startEvent = pairNew ? "pair_new_start" : (automatic ? "auto_reconnect_start" : "manual_reconnect_start");
  BluetoothDiagnostics::recordf(startEvent,
                                "addr=%s name=%s type=%u timeoutMs=%lu scanMs=%lu",
                                address.c_str(), name.c_str(), static_cast<unsigned>(addressType),
                                static_cast<unsigned long>(timeoutMs), static_cast<unsigned long>(scanMs));
  BluetoothDiagnostics::flushToStorage();
  BluetoothDiagnostics::setStorageFlushSuppressed(true);

  bool success = false;
  std::string message;

  if (!_enabled) {
    message = "Bluetooth disabled";
  } else if (!pairNew && address.empty()) {
    message = "No bonded remote";
  } else {
    BluetoothDevice candidate;
    const bool foundCandidate =
        pairNew ? scanForPairingCandidate(candidate, scanMs) : scanForBondedReconnectCandidate(candidate, scanMs);
    if (foundCandidate) {
      BluetoothDiagnostics::recordf(pairNew ? "pair_new_candidate" : "manual_reconnect_candidate",
                                    "addr=%s name=%s type=%u",
                                    candidate.address.c_str(), candidate.name.c_str(),
                                    static_cast<unsigned>(candidate.addressType));
      BluetoothDiagnostics::flushToStorage();
      success = connectToDevice(candidate.address, timeoutMs, candidate.addressType, candidate.name);
      if (success) {
        _bondedDeviceAddress = candidate.address;
        if (pairNew) {
          _bondedDeviceName = candidate.name.empty() ? "Unknown" : candidate.name;
        } else if (!candidate.name.empty() && candidate.name != "Unknown") {
          _bondedDeviceName = candidate.name;
        }
        _bondedDeviceAddressType = candidate.addressType;
        armAutoReconnect(pairNew ? "pair_new_success" : (automatic ? "auto_reconnect_success" : "manual_reconnect_success"),
                         !automatic);
      }
    } else {
      BluetoothDiagnostics::recordf(pairNew ? "pair_new_no_candidate" : "manual_reconnect_no_candidate",
                                    "savedAddr=%s", address.c_str());
      BluetoothDiagnostics::flushToStorage();
      if (lastError.empty()) {
        lastError = pairNew ? "No pairable remote found" : "Remote not found; wake it";
      }
    }

    if (success) {
      if (pairNew) {
        message = _bondedDeviceName.empty() ? "Paired remote" : ("Paired " + _bondedDeviceName);
      } else {
        message = _bondedDeviceName.empty() ? "Reconnected" : ("Reconnected to " + _bondedDeviceName);
      }
    } else {
      message = lastError.empty() ? (pairNew ? "Pairing failed" : "Reconnect failed") : lastError;
    }
  }

  ensureReconnectMutex();
  if (_reconnectMutex && xSemaphoreTake(_reconnectMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    const unsigned long finishNow = millis();
    if (success) {
      _lastReconnectAttempt = 0;
      _fastReconnectUntil = 0;
      _reconnectFailureCount = 0;
    } else {
      _lastReconnectAttempt = finishNow;
      if (_reconnectFailureCount < 6) {
        _reconnectFailureCount++;
      }
      if (_fastReconnectUntil != 0 && static_cast<int32_t>(finishNow - _fastReconnectUntil) >= 0) {
        _fastReconnectUntil = 0;
      }
    }
    _reconnectJobSuccess = success;
    _reconnectJobFinished = true;
    _reconnectJobRunning = false;
    _reconnectJobAutomatic = false;
    _reconnectJobPairNew = false;
    _reconnectJobScanMs = 0;
    _reconnectJobMessage = message;
    _reconnectTaskHandle = nullptr;
    lastError = message;
    xSemaphoreGive(_reconnectMutex);
  } else {
    const unsigned long finishNow = millis();
    if (success) {
      _lastReconnectAttempt = 0;
      _fastReconnectUntil = 0;
      _reconnectFailureCount = 0;
    } else {
      _lastReconnectAttempt = finishNow;
      if (_reconnectFailureCount < 6) {
        _reconnectFailureCount++;
      }
      if (_fastReconnectUntil != 0 && static_cast<int32_t>(finishNow - _fastReconnectUntil) >= 0) {
        _fastReconnectUntil = 0;
      }
    }
    _reconnectJobRunning = false;
    _reconnectJobFinished = true;
    _reconnectJobSuccess = success;
    _reconnectJobAutomatic = false;
    _reconnectJobPairNew = false;
    _reconnectJobScanMs = 0;
    _reconnectJobMessage = message;
    _reconnectTaskHandle = nullptr;
    lastError = message;
  }

  BluetoothDiagnostics::setStorageFlushSuppressed(false);
  if (automatic) {
    setAutoReconnectGuard(false);
  }
  const char* doneEvent = pairNew ? "pair_new_done" : (automatic ? "auto_reconnect_done" : "manual_reconnect_done");
  BluetoothDiagnostics::recordf(doneEvent,
                                "success=%d msg=%s", success, message.c_str());
  BluetoothDiagnostics::flushToStorage(true);
}

bool BluetoothHIDManager::scanForBondedReconnectCandidate(BluetoothDevice& candidate, uint32_t scanMs) {
  if (!_enabled) {
    lastError = "Bluetooth not enabled";
    return false;
  }

  if (_scanning) {
    LOG_INF("BT", "Reconnect scan skipped: scan already active");
    BluetoothDiagnostics::record("manual_reconnect_scan_busy");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  if (scanMs == 0) {
    scanMs = BLE_RECONNECT_SCAN_MS;
  }

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    lastError = "Scan failed";
    BluetoothDiagnostics::record("manual_reconnect_scan_no_object");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  const bool activePageTurnerAutoScan =
      _reconnectJobAutomatic && bondedDeviceLooksLikePageTurner() && pageTurnerReconnectModeActive(millis());
  const bool activeReconnectScan = !_reconnectJobAutomatic || activePageTurnerAutoScan;
  LOG_INF("BT", "Starting worker reconnect scan for %lu ms active=%d pageTurner=%d",
          static_cast<unsigned long>(scanMs), activeReconnectScan, activePageTurnerAutoScan);
  BluetoothDiagnostics::recordf("manual_reconnect_scan_start", "durationMs=%lu active=%d pageTurner=%d name=%s",
                                static_cast<unsigned long>(scanMs), activeReconnectScan,
                                activePageTurnerAutoScan,
                                _bondedDeviceName.c_str());
  BluetoothDiagnostics::flushToStorage();

  clearDiscoveredDevices();
  _reconnectScanMatched = false;
  _scanning = true;
  _scanStopTime = millis() + scanMs + 1000;

  pScan->setScanCallbacks(&scanCallbacks, activeReconnectScan);
  pScan->setActiveScan(activeReconnectScan);
  pScan->setDuplicateFilter(activeReconnectScan ? 0 : 1);
  if (activeReconnectScan) {
    pScan->setScanResponseTimeout(BLE_MANUAL_SCAN_RESPONSE_TIMEOUT_MS);
  }
  pScan->setMaxResults(16);
  pScan->setInterval(BLE_RECONNECT_SCAN_INTERVAL);
  pScan->setWindow(BLE_RECONNECT_SCAN_WINDOW);

  const bool started = pScan->start(scanMs, false);
  if (!started) {
    _scanStopTime = 0;
    _scanning = false;
    lastError = "Scan failed";
    BluetoothDiagnostics::record("manual_reconnect_scan_start_failed");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  const unsigned long deadline = millis() + scanMs + 1200;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    if (_reconnectScanMatched) {
      const size_t found = discoveredDeviceCount();
      BluetoothDiagnostics::recordf("manual_reconnect_scan_early_match", "found=%u",
                                    static_cast<unsigned>(found));
      BluetoothDiagnostics::flushToStorage();
      break;
    }
    if (!pScan->isScanning()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (pScan->isScanning()) {
    pScan->stop();
  }

  _scanStopTime = 0;
  _scanning = false;

  vTaskDelay(pdMS_TO_TICKS(20));

  if (findBondedReconnectCandidate(candidate)) {
    const size_t found = discoveredDeviceCount();
    LOG_INF("BT", "Reconnect scan matched %s (%s type=%u)", candidate.name.c_str(), candidate.address.c_str(),
            static_cast<unsigned>(candidate.addressType));
    BluetoothDiagnostics::recordf("manual_reconnect_scan_match", "addr=%s name=%s type=%u found=%u hid=%d conn=%d",
                                  candidate.address.c_str(), candidate.name.c_str(),
                                  static_cast<unsigned>(candidate.addressType),
                                  static_cast<unsigned>(found), candidate.isHID,
                                  candidate.connectable);
    BluetoothDiagnostics::flushToStorage();
    return true;
  }

  const size_t found = discoveredDeviceCount();
  LOG_INF("BT", "Reconnect scan found %u devices, no bonded match", static_cast<unsigned>(found));
  BluetoothDiagnostics::recordf("manual_reconnect_scan_no_match", "found=%u",
                                static_cast<unsigned>(found));
  uint8_t logged = 0;
  for (const auto& device : getDiscoveredDevices()) {
    if (logged >= BLE_RECONNECT_SEEN_LOG_LIMIT) {
      break;
    }
    BluetoothDiagnostics::recordf("manual_reconnect_seen", "addr=%s name=%s type=%u hid=%d conn=%d rssi=%d",
                                  device.address.c_str(), device.name.c_str(),
                                  static_cast<unsigned>(device.addressType), device.isHID,
                                  device.connectable, device.rssi);
    logged++;
  }
  BluetoothDiagnostics::flushToStorage();
  lastError = "Remote not found; wake it";
  return false;
}

bool BluetoothHIDManager::scanForPairingCandidate(BluetoothDevice& candidate, uint32_t scanMs) {
  if (!_enabled) {
    lastError = "Bluetooth not enabled";
    return false;
  }

  if (_scanning) {
    LOG_INF("BT", "Pairing scan skipped: scan already active");
    BluetoothDiagnostics::record("pair_new_scan_busy");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  if (scanMs == 0) {
    scanMs = BLE_MANUAL_RECONNECT_SCAN_MS;
  }

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    lastError = "Scan failed";
    BluetoothDiagnostics::record("pair_new_scan_no_object");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  LOG_INF("BT", "Starting worker pairing scan for %lu ms", static_cast<unsigned long>(scanMs));
  BluetoothDiagnostics::recordf("pair_new_scan_start", "durationMs=%lu oldAddr=%s oldName=%s",
                                static_cast<unsigned long>(scanMs),
                                _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
  BluetoothDiagnostics::flushToStorage();

  clearDiscoveredDevices();
  _reconnectScanMatched = false;
  _scanning = true;
  _scanStopTime = millis() + scanMs + 1000;

  // Use the stable reconnect worker scan shape for first pairing too: active,
  // finite, duplicate callbacks enabled, and a bounded scan-response wait.
  pScan->setScanCallbacks(&scanCallbacks, true);
  pScan->setActiveScan(true);
  pScan->setDuplicateFilter(false);
  pScan->setScanResponseTimeout(BLE_MANUAL_SCAN_RESPONSE_TIMEOUT_MS);
  pScan->setMaxResults(16);
  pScan->setInterval(BLE_RECONNECT_SCAN_INTERVAL);
  pScan->setWindow(BLE_RECONNECT_SCAN_WINDOW);

  const bool started = pScan->start(scanMs, false);
  if (!started) {
    _scanStopTime = 0;
    _scanning = false;
    lastError = "Scan failed";
    BluetoothDiagnostics::record("pair_new_scan_start_failed");
    BluetoothDiagnostics::flushToStorage();
    return false;
  }

  const unsigned long deadline = millis() + scanMs + 1200;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    if (_reconnectScanMatched) {
      const size_t found = discoveredDeviceCount();
      BluetoothDiagnostics::recordf("pair_new_scan_early_match", "found=%u",
                                    static_cast<unsigned>(found));
      BluetoothDiagnostics::flushToStorage();
      break;
    }
    if (!pScan->isScanning()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (pScan->isScanning()) {
    pScan->stop();
  }

  _scanStopTime = 0;
  _scanning = false;

  vTaskDelay(pdMS_TO_TICKS(20));

  if (findPairingCandidate(candidate)) {
    const size_t found = discoveredDeviceCount();
    LOG_INF("BT", "Pairing scan selected %s (%s type=%u)", candidate.name.c_str(), candidate.address.c_str(),
            static_cast<unsigned>(candidate.addressType));
    BluetoothDiagnostics::recordf("pair_new_scan_match", "addr=%s name=%s type=%u found=%u hid=%d conn=%d",
                                  candidate.address.c_str(), candidate.name.c_str(),
                                  static_cast<unsigned>(candidate.addressType),
                                  static_cast<unsigned>(found), candidate.isHID,
                                  candidate.connectable);
    BluetoothDiagnostics::flushToStorage();
    return true;
  }

  const size_t found = discoveredDeviceCount();
  LOG_INF("BT", "Pairing scan found %u devices, no safe candidate", static_cast<unsigned>(found));
  BluetoothDiagnostics::recordf("pair_new_scan_no_match", "found=%u", static_cast<unsigned>(found));
  uint8_t logged = 0;
  for (const auto& device : getDiscoveredDevices()) {
    if (logged >= BLE_RECONNECT_SEEN_LOG_LIMIT) {
      break;
    }
    BluetoothDiagnostics::recordf("pair_new_seen", "addr=%s name=%s type=%u hid=%d conn=%d rssi=%d",
                                  device.address.c_str(), device.name.c_str(),
                                  static_cast<unsigned>(device.addressType), device.isHID,
                                  device.connectable, device.rssi);
    logged++;
  }
  BluetoothDiagnostics::flushToStorage();
  if (lastError.empty()) {
    lastError = "No pairable remote found";
  }
  return false;
}

bool BluetoothHIDManager::findBondedReconnectCandidate(BluetoothDevice& candidate) const {
  const auto discoveredDevices = getDiscoveredDevices();
  for (const auto& device : discoveredDevices) {
    if (!device.connectable) {
      continue;
    }
    if (matchesBondedReconnectCandidate(device, _bondedDeviceAddress, _bondedDeviceName)) {
      candidate = device;
      return true;
    }
  }

  const bool allowSingleHidFallback =
      containsCaseInsensitive(_bondedDeviceName, "free3") ||
      containsCaseInsensitive(_bondedDeviceName, "free2") ||
      isUnknownOrEmptyName(_bondedDeviceName) ||
      addressLooksRandom(_bondedDeviceAddress);

  if (!allowSingleHidFallback) {
    return false;
  }

  const BluetoothDevice* singleHidCandidate = nullptr;
  uint8_t hidCandidateCount = 0;
  for (const auto& device : discoveredDevices) {
    if (!device.isHID || !device.connectable) {
      continue;
    }
    singleHidCandidate = &device;
    hidCandidateCount++;
  }

  if (hidCandidateCount == 1 && singleHidCandidate) {
    BluetoothDiagnostics::recordf("manual_reconnect_fallback_single_hid", "addr=%s name=%s rssi=%d",
                                  singleHidCandidate->address.c_str(), singleHidCandidate->name.c_str(),
                                  singleHidCandidate->rssi);
    candidate = *singleHidCandidate;
    return true;
  }

  if (!_reconnectJobAutomatic) {
    const BluetoothDevice* singleConnectableCandidate = nullptr;
    uint8_t connectableCandidateCount = 0;
    for (const auto& device : discoveredDevices) {
      if (!device.connectable) {
        continue;
      }
      singleConnectableCandidate = &device;
      connectableCandidateCount++;
    }

    if (connectableCandidateCount == 1 && singleConnectableCandidate) {
      BluetoothDiagnostics::recordf("manual_reconnect_fallback_single_connectable",
                                    "addr=%s name=%s hid=%d rssi=%d",
                                    singleConnectableCandidate->address.c_str(),
                                    singleConnectableCandidate->name.c_str(),
                                    singleConnectableCandidate->isHID,
                                    singleConnectableCandidate->rssi);
      candidate = *singleConnectableCandidate;
      return true;
    }

    BluetoothDiagnostics::recordf("manual_reconnect_fallback_skip", "hid=%u connectable=%u",
                                  static_cast<unsigned>(hidCandidateCount),
                                  static_cast<unsigned>(connectableCandidateCount));
  }

  return false;
}

bool BluetoothHIDManager::findPairingCandidate(BluetoothDevice& candidate) {
  const auto discoveredDevices = getDiscoveredDevices();
  const BluetoothDevice* knownCandidate = nullptr;
  int knownCandidateRssi = -1000;
  uint8_t knownCandidateCount = 0;
  const BluetoothDevice* singleHidCandidate = nullptr;
  uint8_t hidCandidateCount = 0;
  uint8_t connectableCandidateCount = 0;

  for (const auto& device : discoveredDevices) {
    if (!device.connectable) {
      continue;
    }

    if (!_bondedDeviceAddress.empty() && device.address == _bondedDeviceAddress) {
      continue;
    }

    connectableCandidateCount++;

    if (isKnownPairableRemoteName(device.name)) {
      knownCandidateCount++;
      if (!knownCandidate || device.rssi > knownCandidateRssi) {
        knownCandidate = &device;
        knownCandidateRssi = device.rssi;
      }
    }

    if (device.isHID) {
      singleHidCandidate = &device;
      hidCandidateCount++;
    }
  }

  if (knownCandidate) {
    if (knownCandidateCount > 1) {
      BluetoothDiagnostics::recordf("pair_new_multiple_known", "count=%u selected=%s rssi=%d",
                                    static_cast<unsigned>(knownCandidateCount),
                                    knownCandidate->name.c_str(), knownCandidate->rssi);
    }
    candidate = *knownCandidate;
    return true;
  }

  if (hidCandidateCount == 1 && singleHidCandidate) {
    BluetoothDiagnostics::recordf("pair_new_fallback_single_hid", "addr=%s name=%s rssi=%d",
                                  singleHidCandidate->address.c_str(), singleHidCandidate->name.c_str(),
                                  singleHidCandidate->rssi);
    candidate = *singleHidCandidate;
    return true;
  }

  if (hidCandidateCount > 1) {
    lastError = "Multiple HID remotes found; retry closer";
  } else if (connectableCandidateCount == 0) {
    lastError = "No remote found; wake pairing mode";
  } else {
    lastError = "No pairable remote found";
  }

  BluetoothDiagnostics::recordf("pair_new_fallback_skip", "known=%u hid=%u connectable=%u",
                                static_cast<unsigned>(knownCandidateCount),
                                static_cast<unsigned>(hidCandidateCount),
                                static_cast<unsigned>(connectableCandidateCount));
  return false;
}

bool BluetoothHIDManager::matchesBondedReconnectCandidate(const BluetoothDevice& device, const std::string& address,
                                                          const std::string& name) const {
  if (!address.empty() && device.address == address) {
    return true;
  }

  if (isUnknownOrEmptyName(name) || isUnknownOrEmptyName(device.name)) {
    return false;
  }

  if (containsCaseInsensitive(device.name, name.c_str()) || containsCaseInsensitive(name, device.name.c_str())) {
    return true;
  }

  if (isFreePageTurnerName(name) && isFreePageTurnerName(device.name)) {
    return true;
  }

  return false;
}

bool BluetoothHIDManager::isHighConfidencePairingCandidate(const BluetoothDevice& device) const {
  if (!device.connectable) {
    return false;
  }

  if (!_bondedDeviceAddress.empty() && device.address == _bondedDeviceAddress) {
    return false;
  }

  return isKnownPairableRemoteName(device.name);
}

ConnectedDevice* BluetoothHIDManager::findConnectedDeviceLocked(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::queueInputReport(const char* address, const uint8_t* data, size_t length) {
  if (!address || !*address || !data || length == 0) {
    return;
  }

  if (!_inputEventQueue) {
    return;
  }

  QueuedHIDReport event;
  strncpy(event.address, address, sizeof(event.address) - 1);
  event.address[sizeof(event.address) - 1] = '\0';
  event.length = static_cast<uint8_t>(length > MAX_HID_REPORT_BYTES ? MAX_HID_REPORT_BYTES : length);
  memcpy(event.data, data, event.length);

  if (xQueueSend(_inputEventQueue, &event, 0) == pdTRUE) {
    return;
  }

  QueuedHIDReport dropped;
  (void)xQueueReceive(_inputEventQueue, &dropped, 0);
  if (xQueueSend(_inputEventQueue, &event, 0) != pdTRUE) {
    BluetoothDiagnostics::record("hid_input_queue_full");
  }
}

void BluetoothHIDManager::onClientDisconnect(const char* address, int reason) {
  if (!address || !*address) {
    return;
  }

  if (!_connectionEventQueue) {
    return;
  }

  QueuedConnectionEvent event;
  strncpy(event.address, address, sizeof(event.address) - 1);
  event.address[sizeof(event.address) - 1] = '\0';
  event.reason = reason;
  event.suppressAutoReconnect = consumeIntentionalDisconnect(address);

  if (xQueueSend(_connectionEventQueue, &event, 0) == pdTRUE) {
    return;
  }

  QueuedConnectionEvent dropped;
  (void)xQueueReceive(_connectionEventQueue, &dropped, 0);
  if (xQueueSend(_connectionEventQueue, &event, 0) != pdTRUE) {
    BluetoothDiagnostics::record("connection_event_queue_full");
  }
}

void BluetoothHIDManager::processConnectionEvents() {
  if (!_connectionEventQueue) {
    return;
  }

  QueuedConnectionEvent event;
  uint8_t processed = 0;
  while (processed < 4 && xQueueReceive(_connectionEventQueue, &event, 0) == pdTRUE) {
    bool matchedBondedDevice = false;
    bool removedTrackedDevice = false;
    bool eventLooksLikePageTurner = bondedDeviceLooksLikePageTurner();

    ensureConnectedDevicesMutex();
    if (_connectedDevicesMutex) {
      SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
      if (lock.locked()) {
        auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                               [&event](const ConnectedDevice& dev) { return dev.address == event.address; });
        if (it != _connectedDevices.end()) {
          if (_buttonInjector && it->activeInjectedButton != 0xFF) {
            _buttonInjector(it->activeInjectedButton, false);
          }
          matchedBondedDevice = it->wasConnected &&
                                !_bondedDeviceAddress.empty() &&
                                (it->address == _bondedDeviceAddress ||
                                 (!_bondedDeviceName.empty() && !it->name.empty() &&
                                  (containsCaseInsensitive(it->name, _bondedDeviceName.c_str()) ||
                                   containsCaseInsensitive(_bondedDeviceName, it->name.c_str()))));
          eventLooksLikePageTurner = eventLooksLikePageTurner || isFreePageTurnerDevice(*it);
          _connectedDevices.erase(it);
          removedTrackedDevice = true;
        }
      }
    }

    if (!event.suppressAutoReconnect &&
        !matchedBondedDevice &&
        !_bondedDeviceAddress.empty() &&
        _bondedDeviceAddress == event.address) {
      matchedBondedDevice = true;
    }

    BluetoothDiagnostics::recordf("client_disconnect_processed", "addr=%s reason=%d tracked=%d bonded=%d suppress=%d",
                                  event.address, event.reason, removedTrackedDevice, matchedBondedDevice,
                                  event.suppressAutoReconnect);

    if (matchedBondedDevice && !event.suppressAutoReconnect) {
      if (eventLooksLikePageTurner) {
        enterPageTurnerReconnectMode("disconnect_event", true);
      }
      armAutoReconnect("disconnect_event");
    }

    processed++;
  }
}

void BluetoothHIDManager::processInputEvents() {
  if (!_inputEventQueue) {
    return;
  }

  QueuedHIDReport event;
  uint8_t processed = 0;
  while (processed < 8 && xQueueReceive(_inputEventQueue, &event, 0) == pdTRUE) {
    if (event.length > 0 && event.address[0] != '\0') {
      processQueuedHIDReport(event.address, event.data, event.length);
    }
    processed++;
  }
}

void BluetoothHIDManager::setInputCallback(std::function<void(uint16_t)> callback) {
  _inputCallback = callback;
  LOG_DBG("BT", "Input callback registered");
}

void BluetoothHIDManager::setLearnInputCallback(std::function<void(uint8_t, uint8_t)> callback) {
  _learnInputCallback = callback;
  LOG_DBG("BT", "Learn input callback registered");
}

void BluetoothHIDManager::setButtonInjector(std::function<void(uint8_t, bool)> injector) {
  _buttonInjector = injector;
  LOG_DBG("BT", "Button injector registered");
}

void BluetoothHIDManager::setReaderContextCallback(std::function<bool()> callback) {
  _readerContextCallback = callback;
  LOG_DBG("BT", "Reader context callback registered");
}

void BluetoothHIDManager::setButtonActivityNotifier(std::function<void(uint8_t)> notifier) {
  _buttonActivityNotifier = notifier;
}

void BluetoothHIDManager::setBondedDevice(const std::string& address, const std::string& name, uint8_t addressType) {
  _bondedDeviceAddress = address;
  _bondedDeviceName = name;
  _bondedDeviceAddressType = normalizeAddressType(addressType, _bondedDeviceAddress, _bondedDeviceName);
  _lastReconnectCheck = 0;
  _lastReconnectAttempt = 0;
  _fastReconnectUntil = (!_bondedDeviceAddress.empty() && _enabled) ? millis() + BLE_RECONNECT_FAST_WINDOW_MS : 0;
  _reconnectFailureCount = 0;
  _pageTurnerReconnectUntil = 0;
  _pageTurnerReconnectFastUntil = 0;
  if (_bondedDeviceAddress.empty()) {
    _autoReconnectArmed = false;
    _autoReconnectDisabledThisBoot = false;
    setAutoReconnectGuard(false);
  } else if (_enabled && isConnected(_bondedDeviceAddress)) {
    armAutoReconnect("bonded_connected");
  }
  LOG_INF("BT", "Bonded device set: %s (%s) type=%u", _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str(),
          static_cast<unsigned>(_bondedDeviceAddressType));
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while the user is actively using a BLE controller.
  // Background reconnect scans are handled separately so they cannot keep the
  // reader awake forever while a remote is idle or powered off.
  unsigned long now = millis();
  ensureConnectedDevicesMutex();
  if (!_connectedDevicesMutex) {
    return false;
  }

  SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return true;
  }

  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

bool BluetoothHIDManager::hadRecentFree2Input(unsigned long windowMs) const {
  if (isBondedReconnectInProgress()) {
    return false;
  }

  const unsigned long now = millis();
  ensureConnectedDevicesMutex();
  if (!_connectedDevicesMutex) {
    return false;
  }

  SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
  if (!lock.locked()) {
    return false;
  }

  for (const auto& device : _connectedDevices) {
    if (device.lastNormalizedEventMs == 0 || (now - device.lastNormalizedEventMs) > windowMs) {
      continue;
    }

    // Keep the legacy method name for compatibility, but treat any recent BLE
    // page-turner input as a signal to prefer press-driven reader navigation.
    if (isFree2Profile(device.profile) || device.activeInjectedButton != 0xFF || device.lastNormalizedKeycode != 0x00) {
      return true;
    }
  }
  return false;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;
  (void)isNotify;
  
  std::string deviceAddr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      deviceAddr = client->getPeerAddress().toString();
    }
  }

  if (deviceAddr.empty()) return;

  g_instance->queueInputReport(deviceAddr.c_str(), pData, length);
}

void BluetoothHIDManager::processQueuedHIDReport(const char* address, const uint8_t* pData, size_t length) {
  if (!g_instance || !address || !*address || !pData || length == 0) return;

  g_instance->ensureConnectedDevicesMutex();
  if (!g_instance->_connectedDevicesMutex) return;
  SemaphoreLock stateLock(g_instance->_connectedDevicesMutex, pdMS_TO_TICKS(20));
  if (!stateLock.locked()) return;

  ConnectedDevice* device = g_instance->findConnectedDeviceLocked(address);
  if (!device) return;

  const unsigned long nowMs = millis();
  const bool free2Profile = isFree2Profile(device->profile);

  // GameBrick can occasionally miss a release tail, leaving a virtual button
  // latched as pressed. After a long idle gap, clear stale hold state so the
  // next tap is always treated as a fresh press.
  // Keep this comfortably above the reader's 700ms chapter-skip threshold so
  // a legitimate long press is not force-released early.
  if (device->profile && strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
    constexpr unsigned long STALE_GAMEBRICK_HOLD_RESET_MS = 1200;
    if (device->activeInjectedButton != 0xFF &&
        device->lastNormalizedEventMs > 0 &&
        (nowMs - device->lastNormalizedEventMs) > STALE_GAMEBRICK_HOLD_RESET_MS) {
      if (g_instance->_buttonInjector) {
        g_instance->_buttonInjector(device->activeInjectedButton, false);
      }
      device->activeInjectedButton = 0xFF;
      device->lastButtonState = false;
      device->lastHIDKeycode = 0x00;
      device->lastNormalizedPressed = false;
      device->lastGameBrickActiveKey = 0x00;
      device->gameBrickCenterPressFrames = 0;
      LOG_DBG("BT", "Game Brick: cleared stale held state after %lu ms idle", nowMs - device->lastNormalizedEventMs);
    }
  }
  
  // Update activity timestamp to keep connection alive
  device->lastActivityTime = millis();
  // Only Free2 needs hold-time capping based on BLE activity. Other remotes,
  // including GameBrick, should keep the original virtual hold semantics so
  // long-press chapter skip continues to use the full press duration.
  if (free2Profile && g_instance->_buttonActivityNotifier && device->activeInjectedButton != 0xFF) {
    g_instance->_buttonActivityNotifier(device->activeInjectedButton);
  }


  if (g_instance->_debugCaptureEnabled) {
    char rawBuf[128] = {0};
    size_t offset = 0;
    const size_t dumpLen = length < 8 ? length : 8;
    for (size_t i = 0; i < dumpLen && offset + 4 < sizeof(rawBuf); i++) {
      offset += snprintf(rawBuf + offset, sizeof(rawBuf) - offset, "%02X ", static_cast<unsigned>(pData[i]));
    }
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s", device->address.c_str(), static_cast<unsigned>(length), rawBuf);
  }

  auto releaseInjectedButton = [&]() {
    if (g_instance->_buttonInjector && device->activeInjectedButton != 0xFF) {
      g_instance->_buttonInjector(device->activeInjectedButton, false);
    }
    device->activeInjectedButton = 0xFF;
    device->pendingGameBrickRelease = false;
    device->pendingGameBrickReleaseMs = 0;
    device->pendingGameBrickKeycode = 0x00;
    device->pendingGameBrickButton = 0xFF;
  };
  
  // Extract keycode based on device profile or auto-detect
  uint8_t keycode = 0xFF;
  uint8_t keycodeIndex = 0xFF;
  bool isPressed = false;
  bool isGameBrickProfile = false;
  
  if (length < 1) {
    LOG_DBG("BT", "HID report empty, ignoring");
    return;
  }
  
  // Determine keycode source and press state based on device profile
  if (device->profile) {
    // Use device profile's byte index for keycode
    if (device->profile->strictProfile) {
      if (length >= device->profile->reportByteIndex + 1) {
        keycode = pData[device->profile->reportByteIndex];
        keycodeIndex = device->profile->reportByteIndex;
      }
    } else {
      const ExtractedHIDKey extracted = extractProfileOrGenericKeycode(device, pData, length);
      keycode = extracted.keycode;
      keycodeIndex = extracted.reportIndex;
    }

    // For custom/learned profiles: if the fixed-index byte is not one of the learned
    // keycodes, scan the entire report.  This handles remotes where the prev/next buttons
    // send their keycodes at different byte positions, or where they arrive on separate
    // HID report characteristics with their own frame layouts.
    const bool isCustomProfile = (strcmp(device->profile->name, "Custom BLE Remote") == 0);
    if (isCustomProfile &&
        keycode != device->profile->pageUpCode &&
        keycode != device->profile->pageDownCode) {
      for (size_t bi = 0; bi < length && bi < 8; bi++) {
        const uint8_t b = pData[bi];
        if (b == device->profile->pageUpCode || b == device->profile->pageDownCode) {
          keycode = b;
          keycodeIndex = static_cast<uint8_t>(bi);
          LOG_DBG("BT", "Custom profile: found learned code 0x%02X at byte[%u] (vs fixed idx %u)",
                  keycode, static_cast<unsigned>(bi),
                  static_cast<unsigned>(device->profile->reportByteIndex));
          break;
        }
      }
    }

    // For Game Brick: press state from byte[0] bit 0
    // For standard HID keyboards: press state from keycode (non-zero = pressed)
    if (strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
      isGameBrickProfile = true;
      bool gameBrickStandardMode = false;

      // --- GameBrick V2 report format (confirmed via RAW captures) ---
      // byte[0]   : frame status (0x13 pressed/active, 0x12 release tail)
      // byte[1-2] : 16-bit cycling counter (+125/frame, ~8 ms), NOT button data
      // byte[3]   : horizontal (X) joystick axis, center = 0x98
      // byte[4]   : button / vertical axis
      //               0x08 = idle / joystick center
      //               0x07 = physical UP button (d-pad up)
      //               0x09 = physical DOWN button (d-pad down)
      // LEFT/RIGHT are joystick-only: byte[4]==0x08 with byte[3] offset from 0x98.
      //
      // IMPORTANT: ignore any pre-extracted keycode from profile byte index because
      // byte[2] can naturally pass through 0x07/0x09 and cause false button presses.
      keycode = 0x00;
      keycodeIndex = 0xFF;

      auto isGameBrickSupportedCode = [](uint8_t code) {
         return code == 0x07 || code == 0x09 ||
           code == GAMEBRICK_ACTION_A_CODE ||
           code == GAMEBRICK_ACTION_B_CODE ||
               code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      // Some GameBrick C/T/H modes expose standard keyboard/consumer reports.
      // Prefer that path when a clear standard keycode is present.
      const ExtractedHIDKey generic = extractGenericPageTurnKeycode(pData, length);
      auto isStandardGameBrickCode = [](uint8_t code) {
        return code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      if (isStandardGameBrickCode(generic.keycode)) {
        gameBrickStandardMode = true;
        keycode = generic.keycode;
        keycodeIndex = generic.reportIndex;
      }

      if (!gameBrickStandardMode && length >= 5) {
        // bytes[1,2] form a 16-bit LE cycling counter (~+125/frame, LE).
        // The counter FREEZES to 0x07D0 when any physical button is pressed and
        // remains frozen through the entire press AND release-ramp sequence.
        // Joystick motion keeps the counter cycling freely.
        const uint16_t counter =
            static_cast<uint16_t>(pData[1]) | (static_cast<uint16_t>(pData[2]) << 8);
        const bool counterFrozen = (counter == device->lastGameBrickCounter);
        device->lastGameBrickCounter = counter;

        const bool isReleaseTail = (pData[0] & 0x01) == 0;
        const bool activeFrame = ((pData[0] & 0x01) != 0);
        const bool isDirectionalFreezeWindow = (counter == 0x07D0);

        // Clear the d-pad latch once the counter resumes cycling or a release-tail arrives.
        if (!counterFrozen || isReleaseTail) {
          device->lastGameBrickActiveKey = 0x00;
        }
        const uint8_t b4 = pData[4];
        if (b4 == 0x07 || b4 == 0x09) {
          const bool directionalFreezeWindow =
              isDirectionalFreezeWindow || (counterFrozen && device->lastGameBrickActiveKey != 0x00);
          if (directionalFreezeWindow) {
            // D-pad UP/DOWN uses the special 0x07D0 frozen counter window.
            // While held, the release ramp can cross the opposite code, so latch the
            // first directional code seen until release-tail/counter-change.
            if (device->lastGameBrickActiveKey == 0x00) {
              device->lastGameBrickActiveKey = b4;
            }
            if (b4 == device->lastGameBrickActiveKey) {
              keycode = b4;
              keycodeIndex = 4;
            }
          } else {
            // Non-0x07D0 window: treat 0x07/0x09 as A/B button family.
            // This preserves menu semantics (A=Select, B=Back) outside page-reading context.
            keycode = (b4 == 0x07) ? GAMEBRICK_ACTION_A_CODE : GAMEBRICK_ACTION_B_CODE;
            keycodeIndex = 4;
          }
          device->gameBrickCenterPressFrames = 0;
        } else if (b4 == 0x08) {
          // Joystick horizontal:
          // - usually appears while counter is cycling
          // - can also appear in some frozen windows for horizontal-only presses
          //
          // But while vertical d-pad latch (0x07/0x09 in 0x07D0 window) is active,
          // b4==0x08 frames are release/overshoot noise and must be ignored.
          const bool allowHorizontal = !counterFrozen || device->lastGameBrickActiveKey == 0x00;
          if (!allowHorizontal) {
            // Transitional frame from vertical press/release.
            keycode = 0x00;
            device->gameBrickCenterPressFrames = 0;
          } else {
            const int dx = static_cast<int>(pData[3]) - 0x98;
            // Empirical tuning from logs:
            // RIGHT tends to be stronger than LEFT on some units, so keep LEFT
            // threshold lower to catch weak positive deflections.
            constexpr int kDeadzoneRight = 2;
            constexpr int kDeadzoneLeft = 0;
            if (dx < -kDeadzoneRight) {
              keycode = DeviceProfiles::KEYBOARD_RIGHT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (dx > kDeadzoneLeft) {
              keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (activeFrame && !counterFrozen && device->lastGameBrickActiveKey == 0x00) {
              // Some GameBrick units appear to emit LEFT as a centered b4==0x08 burst
              // (dx≈0) with a cycling counter. Require several consecutive frames so
              // transitional noise from other keys is ignored.
              if (device->gameBrickCenterPressFrames < 255) {
                device->gameBrickCenterPressFrames++;
              }
              if (device->gameBrickCenterPressFrames >= 6) {
                keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
                keycodeIndex = 3;
              }
            } else {
              device->gameBrickCenterPressFrames = 0;
            }
            // else: centered idle → keycode stays 0x00
          }
        } else {
          device->gameBrickCenterPressFrames = 0;
        }
        // All other byte[4] values (ramp overshoot > 0x09 or < 0x07) → 0x00.
      }

      // If nothing found, keycode stays 0x00 → treated as release.

      // Game Brick: accept only stable digital-button report family (0x1x).
      // Ignore noisy transitional frames (commonly 0x2x/0x3x) that can trigger false presses.
      if (gameBrickStandardMode) {
        isPressed = (keycode != 0x00) && isGameBrickSupportedCode(keycode);
      } else {
        const bool stableButtonReport = (pData[0] & 0xF0) == 0x10;
        if (!stableButtonReport) {
          LOG_DBG("BT", "Game Brick: ignoring transitional report byte[0]=0x%02X, keycode=0x%02X", pData[0], keycode);
          // Keep the previous button state intact while skipping transitional frames.
          // Resetting state here can create a duplicate "new press" on the next stable
          // frame, which shows up as a double page-turn.
          return;
        }

        // Press is only valid with a supported decoded code plus active frame bit.
        isPressed = ((pData[0] & 0x01) != 0) && isGameBrickSupportedCode(keycode);
      }

      // Prevent initial stale pressed frame right after subscribe from triggering navigation.
      // Only allow presses after at least one clean release frame has been seen.
      if (!device->hasSeenRelease) {
        if (!isPressed) {
          device->hasSeenRelease = true;
        } else {
          // Some GameBrick variants do not emit an immediate release frame after
          // connect and would otherwise be blocked indefinitely. Arm input on
          // the first valid GameBrick press instead of discarding it.
          device->hasSeenRelease = true;
          LOG_DBG("BT", "Game Brick: arming on first valid press keycode=0x%02X", keycode);
        }
      }

      {
        // Full raw dump so we can reverse-engineer D-pad encoding.
        char rawBuf[64];
        int pos = 0;
        for (size_t ri = 0; ri < length && ri < 8 && pos < 56; ri++) {
          pos += snprintf(rawBuf + pos, sizeof(rawBuf) - pos, "%02X ", pData[ri]);
        }
        LOG_DBG("BT", "Game Brick RAW[%u]: %s=> keycode=0x%02X idx=%u pressed=%d",
                static_cast<unsigned>(length), rawBuf, keycode,
                static_cast<unsigned>(keycodeIndex), isPressed);
      }
    } else {
      // Standard HID keyboards/custom profiles: keycode non-zero = pressed.
      // Normalise 0xFF (= "nothing found in report") to 0x00 so that short
      // release frames (e.g. 1-byte consumer control [0x00]) are treated as
      // a key-release rather than a phantom press.
      if (keycode == 0xFF) {
        keycode = 0x00;
      }
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Device %s: keycode=0x%02X, pressed=%d", device->profile->name, keycode, isPressed);
    }
  } else {
    // Auto-detect mode: support a wider range of generic HID remotes.
    const ExtractedHIDKey extracted = extractGenericPageTurnKeycode(pData, length);
    keycode = extracted.keycode;
    keycodeIndex = extracted.reportIndex;

    if (device->descriptorSuggestedIndex != 0xFF && length > device->descriptorSuggestedIndex) {
      const uint8_t hintedCode = pData[device->descriptorSuggestedIndex];
      if (hintedCode != 0x00 && hintedCode != 0xFF &&
          (keycode == 0x00 || keycode == 0xFF || DeviceProfiles::isCommonPageTurnCode(hintedCode))) {
        keycode = hintedCode;
        keycodeIndex = device->descriptorSuggestedIndex;
      }
    }

    // Some remotes emit noisy 0x07/0x09 bytes in parallel with true rolling keycodes.
    // If we selected 0x07/0x09, search the short report for a stronger non-GameBrick code.
    if ((keycode == 0x07 || keycode == 0x09) && length > 0) {
      const size_t scanLen = length < 8 ? length : 8;
      for (size_t i = 0; i < scanLen; i++) {
        const uint8_t candidate = pData[i];
        if (candidate == 0x00 || candidate == 0xFF || candidate == 0x07 || candidate == 0x09) {
          continue;
        }
        if (DeviceProfiles::isCommonPageTurnCode(candidate)) {
          keycode = candidate;
          keycodeIndex = static_cast<uint8_t>(i);
          break;
        }
      }
    }

    // Keep existing GameBrick bit0 press-state behavior when applicable.
    if (length >= 5 && (keycode == 0x07 || keycode == 0x09)) {
      isPressed = ((pData[0] & 0x01) != 0) || (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (GameBrick-like): keycode=0x%02X, pressed=%d", keycode, isPressed);
    } else {
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (generic HID): keycode=0x%02X, pressed=%d", keycode, isPressed);
    }
  }

  // Update release state for startup noise gate
  // When we see the first release (isPressed = false), we enable button injection
  if (!isPressed && !device->hasSeenRelease) {
    device->hasSeenRelease = true;
  }
  
  // Ignore if no valid keycode detected
  if (keycode == 0x00 || keycode == 0xFF) {
    releaseInjectedButton();
    // Track state for transition detection
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    device->lastNormalizedDirection = 0xFF;
    return;
  }
  
  // CRITICAL GATE: Don't inject any buttons until we've seen the first release
  // This prevents startup transient noise from being interpreted as button presses
  if (!device->hasSeenRelease) {
    const bool likelyFree2Press =
        keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
        keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D ||
        keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
        keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D;

    if (device->profile == nullptr && likelyFree2Press && isPressed) {
      // Free 2 may not emit a clean initial release frame; arm on first valid press.
      device->hasSeenRelease = true;
      LOG_DBG("BT", "Arming auto-detect on first valid Free2 code: 0x%02X", keycode);
    }

    releaseInjectedButton();
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    return;
  }

  const uint8_t free2Direction = free2Profile ? classifyFree2Direction(keycode) : 0xFF;

  // Detect button PRESS transition.
  // For most remotes, key changes while held are treated as a new press event.
  // For Game Brick, ignore key-change retriggers while held to avoid duplicate events.
  bool isNewPressEvent =
      isPressed && (!device->lastButtonState || (!isGameBrickProfile && keycode != device->lastHIDKeycode));

  // Free2 reports rolling keycodes while one button is held.
  // Collapse that family to one logical press and ignore family flips until release.
  if (free2Profile && isPressed) {
    if (!device->lastButtonState) {
      device->lastNormalizedDirection = free2Direction;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction == device->lastNormalizedDirection) {
      isNewPressEvent = false;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction != 0xFF &&
               free2Direction != device->lastNormalizedDirection) {
      isNewPressEvent = false;
      if (device->activeInjectedButton != 0xFF) {
        keycode = device->lastHIDKeycode;
      }
    }
  }

  if (isGameBrickProfile && isPressed && !isNewPressEvent && keycode == device->lastHIDKeycode &&
      device->lastNormalizedEventMs > 0) {
    constexpr unsigned long GAMEBRICK_REPRESS_IDLE_MS = 220;
    if ((nowMs - device->lastNormalizedEventMs) > GAMEBRICK_REPRESS_IDLE_MS) {
      isNewPressEvent = true;
      device->lastButtonState = false;
      device->lastNormalizedPressed = false;
      LOG_DBG("BT", "Game Brick: promoting same-key re-press after %lu ms idle (key=0x%02X)",
              nowMs - device->lastNormalizedEventMs, keycode);
    }
  }

  if (isNewPressEvent && device->lastNormalizedPressed && device->lastNormalizedKeycode == keycode &&
      (nowMs - device->lastNormalizedEventMs) < 90) {
    isNewPressEvent = false;
    if (g_instance->_debugCaptureEnabled) {
      LOG_INF("BTDBG", "Suppressed jitter duplicate key=0x%02X dt=%lu", keycode,
              nowMs - device->lastNormalizedEventMs);
    }
  }
  if (isNewPressEvent) {
    LOG_INF("BT", ">>> BUTTON PRESSED: keycode=0x%02X <<<", keycode);

    if (g_instance->_learnInputCallback && keycode != 0x00 && keycode != 0xFF && keycodeIndex != 0xFF) {
      g_instance->_learnInputCallback(keycode, keycodeIndex);
    }
    
    // Also call original callback if set
    if (g_instance->_inputCallback) {
      g_instance->_inputCallback(keycode);
    }
  }

  uint8_t mappedButton = isPressed ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  // Free2 can wobble briefly while a key is held, causing opposite-direction flips or
  // transient unmapped frames. Keep the active direction latched during a continuous hold
  // and wait for an actual release before changing direction.
  if (free2Profile && isPressed && device->lastButtonState && device->activeInjectedButton != 0xFF) {
    if (mappedButton == 0xFF) {
      mappedButton = device->activeInjectedButton;
    } else if (mappedButton != device->activeInjectedButton) {
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BTDBG", "Hold wobble suppressed: active=%u incoming=%u key=0x%02X", device->activeInjectedButton,
                mappedButton, keycode);
      }
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    }
  }

  const bool isGameBrickActionKey = isGameBrickProfile &&
                                    (keycode == GAMEBRICK_ACTION_A_CODE || keycode == GAMEBRICK_ACTION_B_CODE);
  const uint8_t gameBrickActionButton = isGameBrickActionKey ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  if (device->pendingGameBrickRelease) {
    if (isPressed && keycode == device->pendingGameBrickKeycode && mappedButton == device->pendingGameBrickButton) {
      device->pendingGameBrickRelease = false;
      device->pendingGameBrickReleaseMs = 0;
      device->pendingGameBrickKeycode = 0x00;
      device->pendingGameBrickButton = 0xFF;
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    } else if (isPressed && mappedButton != device->pendingGameBrickButton) {
      releaseInjectedButton();
    }
  }

  if (isGameBrickProfile && g_instance->_debugCaptureEnabled && isPressed) {
    const char* keyLabel = "Unknown";
    switch (keycode) {
      case DeviceProfiles::KEYBOARD_UP_ARROW:
        keyLabel = "DPad Up";
        break;
      case DeviceProfiles::KEYBOARD_DOWN_ARROW:
        keyLabel = "DPad Down";
        break;
      case DeviceProfiles::KEYBOARD_LEFT_ARROW:
        keyLabel = "DPad Left";
        break;
      case DeviceProfiles::KEYBOARD_RIGHT_ARROW:
        keyLabel = "DPad Right";
        break;
      case GAMEBRICK_ACTION_A_CODE:
        keyLabel = "A";
        break;
      case GAMEBRICK_ACTION_B_CODE:
        keyLabel = "B";
        break;
      case 0x07:
        keyLabel = "Up";
        break;
      case 0x09:
        keyLabel = "Down";
        break;
      default:
        break;
    }

    const char* actionLabel = "Unmapped";
    switch (mappedButton) {
      case HalGPIO::BTN_UP:
        actionLabel = "Up/PageBack";
        break;
      case HalGPIO::BTN_DOWN:
        actionLabel = "Down/PageForward";
        break;
      case HalGPIO::BTN_LEFT:
        actionLabel = "Left";
        break;
      case HalGPIO::BTN_RIGHT:
        actionLabel = "Right";
        break;
      case HalGPIO::BTN_CONFIRM:
        actionLabel = "Select";
        break;
      case HalGPIO::BTN_BACK:
        actionLabel = "Back";
        break;
      default:
        break;
    }

    LOG_INF("BTDBG", "GameBrick %s (0x%02X) -> %s", keyLabel, keycode, actionLabel);
  }

  if (!isPressed || mappedButton == 0xFF) {
    if (isGameBrickActionKey && device->activeInjectedButton == gameBrickActionButton && gameBrickActionButton != 0xFF) {
      constexpr unsigned long GAMEBRICK_ACTION_RELEASE_GRACE_MS = 110;
      device->pendingGameBrickRelease = true;
      device->pendingGameBrickReleaseMs = nowMs + GAMEBRICK_ACTION_RELEASE_GRACE_MS;
      device->pendingGameBrickKeycode = keycode;
      device->pendingGameBrickButton = gameBrickActionButton;
    } else {
      releaseInjectedButton();
    }
  } else {
    if (device->activeInjectedButton != 0xFF && device->activeInjectedButton != mappedButton) {
      releaseInjectedButton();
    }

    if (g_instance->_buttonInjector && device->activeInjectedButton == 0xFF) {
      if (isGameBrickProfile && device->lastInjectedKeycode == keycode &&
          (millis() - device->lastInjectionTime) < 180) {
        LOG_DBG("BT", "Game Brick: debouncing duplicate key 0x%02X (%lu ms)", keycode,
                millis() - device->lastInjectionTime);
      } else {
      const char* buttonName = "Unknown";
      switch (mappedButton) {
        case HalGPIO::BTN_UP:
          buttonName = "Up/PageBack";
          break;
        case HalGPIO::BTN_DOWN:
          buttonName = "Down/PageForward";
          break;
        case HalGPIO::BTN_LEFT:
          buttonName = "Left";
          break;
        case HalGPIO::BTN_RIGHT:
          buttonName = "Right";
          break;
        case HalGPIO::BTN_CONFIRM:
          buttonName = "Select";
          break;
        case HalGPIO::BTN_BACK:
          buttonName = "Back";
          break;
        default:
          break;
      }
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped key 0x%02X -> %s", keycode, buttonName);
      }
      g_instance->_buttonInjector(mappedButton, true);
      device->activeInjectedButton = mappedButton;
      if (free2Profile && g_instance->_buttonActivityNotifier) {
        // Seed the hold timer on the very first injected Free2 press. This keeps a
        // missing release frame from letting a short tap age into a long-press skip.
        g_instance->_buttonActivityNotifier(mappedButton);
      }
      device->lastInjectionTime = millis();
      device->lastInjectedKeycode = keycode;
      }
    }
  }
  
  // Track the button state and keycode for next time
  device->lastButtonState = isPressed;
  device->lastHIDKeycode = keycode;
  device->lastNormalizedEventMs = nowMs;
  device->lastNormalizedKeycode = keycode;
  device->lastNormalizedPressed = isPressed;
  if (!isPressed) {
    device->lastNormalizedDirection = 0xFF;
  } else if (free2Profile && free2Direction != 0xFF) {
    device->lastNormalizedDirection = free2Direction;
  }
}

uint16_t BluetoothHIDManager::parseHIDReport(uint8_t* data, size_t length) {
  if (length < 3) {
    LOG_ERR("BT", "Invalid HID report length: %d", length);
    return 0;
  }
  
  uint8_t modifier = data[0];
  uint8_t keycode = data[2]; // First key in the report
  
  // If no key pressed (all zeros), return 0
  if (keycode == 0 && modifier == 0) {
    return 0;
  }
  
  // Log non-empty reports only during active debug capture to keep the hot path light.
  if (_debugCaptureEnabled) {
    LOG_INF("BT", "HID Report: mod=0x%02X key=0x%02X", modifier, keycode);
  }
  
  // Combine modifier and keycode (modifier in upper byte, keycode in lower)
  uint16_t combined = (static_cast<uint16_t>(modifier) << 8) | keycode;
  
  return combined;
}

// Map HID keycodes to navigator buttons based on device profile
// Only maps keycodes that match the current device's profile to prevent
// unwanted D-pad or other button inputs from triggering page turns
uint8_t BluetoothHIDManager::mapKeycodeToButton(uint8_t keycode, ConnectedDevice* device) {
  const DeviceProfiles::DeviceProfile* profile = device ? device->profile : nullptr;

  // Log keycode for debugging
  if (keycode != 0x00) {
    LOG_DBG("BT", "mapKeycodeToButton() called with keycode: 0x%02X", keycode);
  }
  
  // If we have a device profile, ONLY map keycodes specific to that profile
  if (profile) {
    // Free 2 reports a rolling keycode family while button is held.
    // These groups are captured from device logs and map to stable page actions.
    if (strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0) {
      const bool isForward =
          keycode == 0x1C || keycode == 0xC4 || keycode == 0x6C || keycode == 0xBC;
      const bool isBack =
          keycode == 0xB4 || keycode == 0x0E || keycode == 0x66 || keycode == 0x16;

      if (isForward) {
        LOG_INF("BT", "Free2 rolling-code forward match: 0x%02X", keycode);
        return HalGPIO::BTN_DOWN;
      }

      if (isBack) {
        LOG_INF("BT", "Free2 rolling-code back match: 0x%02X", keycode);
        return HalGPIO::BTN_UP;
      }
    }

    if (strncmp(profile->name, "IINE Game Brick", 15) == 0) {
      bool inReaderContext = false;
      if (_readerContextCallback) {
        inReaderContext = _readerContextCallback();
      }

      // Synthetic A/B mapping:
      // - Menus: A=Confirm, B=Back
      // - Reader: A=PageForward, B=PageBack
      if (keycode == GAMEBRICK_ACTION_A_CODE) {
        return inReaderContext ? HalGPIO::BTN_DOWN : HalGPIO::BTN_CONFIRM;
      }

      if (keycode == GAMEBRICK_ACTION_B_CODE) {
        return inReaderContext ? HalGPIO::BTN_UP : HalGPIO::BTN_BACK;
      }

      // Physical UP button (byte[4]=0x07 = profile->pageDownCode).
      // Maps to BTN_UP in all contexts: navigate up in menus, page-back in reader.
      if (keycode == profile->pageDownCode) {
        return HalGPIO::BTN_UP;
      }

      // Physical DOWN button (byte[4]=0x09 = profile->pageUpCode).
      // Maps to BTN_DOWN in all contexts: navigate down in menus, page-forward in reader.
      if (keycode == profile->pageUpCode) {
        return HalGPIO::BTN_DOWN;
      }

      // Keyboard/consumer-mode directional mappings (C/T/H mode variants).
      if (keycode == DeviceProfiles::KEYBOARD_UP_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_UP ||
          keycode == DeviceProfiles::STANDARD_PAGE_DOWN) {
        return HalGPIO::BTN_UP;
      }

      if (keycode == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
          keycode == DeviceProfiles::STANDARD_PAGE_UP) {
        return HalGPIO::BTN_DOWN;
      }

      // Joystick LEFT/RIGHT (decoded from byte[3] offset when byte[4]=0x08).
      // In non-reader context: emit true LEFT/RIGHT so activities can decide
      // behavior (many menus already treat LEFT/RIGHT as prev/next via ButtonNavigator).
      // In reader context: suppress to avoid accidental exits/actions.
      if (!inReaderContext) {
        if (keycode == DeviceProfiles::KEYBOARD_LEFT_ARROW) return HalGPIO::BTN_LEFT;
        if (keycode == DeviceProfiles::KEYBOARD_RIGHT_ARROW) return HalGPIO::BTN_RIGHT;
      }

      if (keycode == DeviceProfiles::KEYBOARD_ENTER || keycode == DeviceProfiles::KEYBOARD_SPACE) {
        return HalGPIO::BTN_CONFIRM;
      }

      return 0xFF;
    }

    if (keycode == profile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageUpCode 0x%02X (%s) -> PageBack", keycode, profile->name);
      }
      return HalGPIO::BTN_UP;
    } else if (keycode == profile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageDownCode 0x%02X (%s) -> PageForward", keycode, profile->name);
      }
      return HalGPIO::BTN_DOWN;
    }

    // The known profile didn't recognise this keycode. For non-strict (standard layout)
    // profiles, also consult the user-learned custom mapping as a fallback. This covers
    // the common case where a device partially matches a known profile (e.g. its back
    // button matches MINI_KEYBOARD but its forward button uses a different code).
    const bool isStrict = profile->strictProfile;
    if (!isStrict) {
      if (const auto* learned = DeviceProfiles::getCustomProfile()) {
        if (keycode == learned->pageUpCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageBack (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_UP;
        }
        if (keycode == learned->pageDownCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageForward (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_DOWN;
        }
      }

      bool pageForward = false;
      if (DeviceProfiles::mapCommonCodeToDirection(keycode, pageForward)) {
        if (_debugCaptureEnabled) {
          LOG_INF("BT", "Non-strict profile generic fallback: 0x%02X -> %s (profile=%s)", keycode,
                  pageForward ? "PageForward" : "PageBack", profile->name);
        }
        return pageForward ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
      }
    }

    // Not matched by profile or fallback - ignore
    LOG_DBG("BT", "Keycode 0x%02X not in profile %s (expecting 0x%02X/0x%02X), ignoring",
            keycode, profile->name, profile->pageUpCode, profile->pageDownCode);
    return 0xFF;
  }

  // Learned mappings are only used for unknown devices.
  if (const auto* customProfile = DeviceProfiles::getCustomProfile()) {
    if (keycode == customProfile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageBack", keycode);
      }
      return HalGPIO::BTN_UP;
    }
    if (keycode == customProfile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageForward", keycode);
      }
      return HalGPIO::BTN_DOWN;
    }
  }

  // No profile match - use broad common-key mapping for generic remotes/keyboards.
  bool pageForward = false;
  if (DeviceProfiles::mapCommonCodeToDirection(keycode, pageForward)) {
    if (_debugCaptureEnabled) {
      if (pageForward) {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageForward", keycode);
      } else {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageBack", keycode);
      }
    }
    return pageForward ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
  }

  if (keycode == 0x00) {
    return 0xFF;
  }

  if (device && device->simpleFallbackEnabled) {
    if (device->simpleForwardKeycode == 0x00) {
      device->simpleForwardKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned FORWARD keycode 0x%02X", keycode);

      if (device->simpleBackKeycode != 0x00) {
        const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
        DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                  device->simpleForwardKeycode, idx);
      }
      return HalGPIO::BTN_DOWN;
    }

    if (keycode == device->simpleForwardKeycode) {
      return HalGPIO::BTN_DOWN;
    }

    if (device->simpleBackKeycode == 0x00) {
      device->simpleBackKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned BACK keycode 0x%02X", keycode);
      const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
      DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                device->simpleForwardKeycode, idx);
      return HalGPIO::BTN_UP;
    }

    if (keycode == device->simpleBackKeycode) {
      return HalGPIO::BTN_UP;
    }
  }

  LOG_DBG("BT", "Unmapped keycode: 0x%02X (no profile)", keycode);
  return 0xFF;
}

void BluetoothHIDManager::updateActivity() {
  if (isBondedReconnectInProgress()) {
    return;
  }

  processConnectionEvents();

  unsigned long now = millis();

  if (_scanning) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && !pScan->isScanning()) {
      _scanStopTime = 0;
      _scanning = false;
      const size_t found = discoveredDeviceCount();
      LOG_INF("BT", "Scan complete, found %d devices", static_cast<int>(found));
      BluetoothDiagnostics::recordf("scan_complete", "found=%u", static_cast<unsigned>(found));
      BluetoothDiagnostics::flushToStorage();
    } else if (_scanStopTime != 0 && static_cast<int32_t>(now - _scanStopTime) >= 0) {
      stopScan();
    }
  }

  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
    if (lock.locked()) {
      for (auto& device : _connectedDevices) {
        if (device.pendingGameBrickRelease && device.pendingGameBrickReleaseMs > 0 && now >= device.pendingGameBrickReleaseMs) {
          if (_buttonInjector && device.activeInjectedButton != 0xFF) {
            _buttonInjector(device.activeInjectedButton, false);
          }
          device.activeInjectedButton = 0xFF;
          device.lastButtonState = false;
          device.lastHIDKeycode = 0x00;
          device.lastNormalizedPressed = false;
          device.pendingGameBrickRelease = false;
          device.pendingGameBrickReleaseMs = 0;
          device.pendingGameBrickKeycode = 0x00;
          device.pendingGameBrickButton = 0xFF;
          LOG_DBG("BT", "Game Brick: released deferred action button for %s", device.address.c_str());
        }
      }
    }
  }

  // Fast path: release stale injected buttons promptly for Free2 only.
  // Free2 often omits timely release frames; other remotes should keep their prior behavior.
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
    if (lock.locked()) {
      for (auto& device : _connectedDevices) {
        if (isFree2Profile(device.profile) && device.activeInjectedButton != 0xFF) {
          const bool inReaderContext = _readerContextCallback && _readerContextCallback();
          const unsigned long staleReleaseMs = inReaderContext ? FREE2_STALE_RELEASE_READER_MS
                                                               : FREE2_STALE_RELEASE_DEFAULT_MS;
          if (now - device.lastActivityTime <= staleReleaseMs) {
            continue;
          }
          if (_buttonInjector) {
            _buttonInjector(device.activeInjectedButton, false);
          }
          device.activeInjectedButton = 0xFF;
          device.lastButtonState = false;
          device.lastHIDKeycode = 0x00;
          LOG_DBG("BT", "Released stale injected button for %s", device.address.c_str());
        }
      }
    }
  }

  // Slow path: connection maintenance every 10 seconds.
  if (now - lastMaintenanceCheck < 10000) {
    return;
  }
  lastMaintenanceCheck = now;

  // Preserve the original stale-release maintenance behavior for non-Free2 devices.
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
    if (lock.locked()) {
      for (auto& device : _connectedDevices) {
        if (!isFree2Profile(device.profile) && device.activeInjectedButton != 0xFF && now - device.lastActivityTime > 250) {
          if (_buttonInjector) {
            _buttonInjector(device.activeInjectedButton, false);
          }
          device.activeInjectedButton = 0xFF;
          device.lastButtonState = false;
          device.lastHIDKeycode = 0x00;
          LOG_DBG("BT", "Released stale injected button for %s", device.address.c_str());
        }
      }
    }
  }

  // Check for one inactive connection and disconnect it in-place.
  std::string inactiveAddress;
  unsigned long inactiveTimeMs = 0;
  bool inactiveMatchesBonded = false;
  bool inactiveIsPageTurner = false;
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
    if (lock.locked()) {
      for (const auto& device : _connectedDevices) {
        if (device.lastActivityTime == 0) {
          continue;
        }

        unsigned long inactiveTime = now - device.lastActivityTime;
        const bool pageTurnerLike = isFreePageTurnerDevice(device);
        const unsigned long timeoutMs = pageTurnerLike ? BLE_PAGE_TURNER_IDLE_RECONNECT_MS : INACTIVITY_TIMEOUT_MS;
        if (inactiveTime > timeoutMs) {
          inactiveAddress = device.address;
          inactiveTimeMs = inactiveTime;
          inactiveIsPageTurner = pageTurnerLike;
          inactiveMatchesBonded =
              !_bondedDeviceAddress.empty() &&
              (device.address == _bondedDeviceAddress ||
               (!_bondedDeviceName.empty() && !device.name.empty() &&
                (containsCaseInsensitive(device.name, _bondedDeviceName.c_str()) ||
                 containsCaseInsensitive(_bondedDeviceName, device.name.c_str()))));
          break;
        }
      }
    }
  }

  if (!inactiveAddress.empty()) {
    LOG_INF("BT", "Device %s inactive for %lu ms, disconnecting", inactiveAddress.c_str(), inactiveTimeMs);
    BluetoothDiagnostics::recordf("idle_disconnect", "addr=%s idleMs=%lu pageTurner=%d bonded=%d",
                                  inactiveAddress.c_str(), inactiveTimeMs, inactiveIsPageTurner,
                                  inactiveMatchesBonded);
    if (disconnectFromDevice(inactiveAddress) && inactiveMatchesBonded) {
      if (inactiveIsPageTurner) {
        enterPageTurnerReconnectMode("page_turner_idle", true);
      }
      armAutoReconnect(inactiveIsPageTurner ? "page_turner_idle" : "idle_timeout");
    }
  }
}

void BluetoothHIDManager::checkAutoReconnect(bool userInputDetected) {
  if (isBondedReconnectInProgress()) {
    return;
  }

  if (!_enabled) {
    return;
  }

  unsigned long now = millis();
  
  if (now - _lastReconnectCheck < BLE_RECONNECT_CHECK_INTERVAL_MS) {
    return;
  }
  _lastReconnectCheck = now;

  // Remove stale disconnected clients from active list.
  bool hasConnectedDevices = false;
  ensureConnectedDevicesMutex();
  if (_connectedDevicesMutex) {
    SemaphoreLock lock(_connectedDevicesMutex, pdMS_TO_TICKS(20));
    if (lock.locked()) {
      for (auto it = _connectedDevices.begin(); it != _connectedDevices.end();) {
        if (!it->client || !it->client->isConnected()) {
          const bool staleIsPageTurner = isFreePageTurnerDevice(*it);
          if (_buttonInjector && it->activeInjectedButton != 0xFF) {
            _buttonInjector(it->activeInjectedButton, false);
          }
          if (it->wasConnected) {
            if (staleIsPageTurner) {
              enterPageTurnerReconnectMode("stale_client", true);
            }
            armAutoReconnect("stale_client");
          }
          LOG_DBG("BT", "Pruning stale disconnected client entry: %s client=%p", it->address.c_str(), it->client);
          it = _connectedDevices.erase(it);
        } else {
          ++it;
        }
      }
      hasConnectedDevices = !_connectedDevices.empty();
    } else {
      hasConnectedDevices = true;
    }
  }

  // Already connected.
  if (hasConnectedDevices) {
    _reconnectFailureCount = 0;
    LOG_DBG("BT", "AutoReconnect skipped: already connected");
    return;
  }

  if (_scanning) {
    LOG_DBG("BT", "AutoReconnect skipped: scan active");
    return;
  }

  if (_bondedDeviceAddress.empty()) {
    LOG_DBG("BT", "AutoReconnect skipped: no bonded device configured");
    return;
  }

  initializeAutoReconnectGuard();
  if (_autoReconnectDisabledThisBoot) {
    LOG_DBG("BT", "AutoReconnect skipped: crash guard active");
    return;
  }

  if (!_autoReconnectArmed) {
    LOG_DBG("BT", "AutoReconnect skipped: not armed this session");
    return;
  }

  const unsigned long graceMs =
      _autoReconnectWakeRequestedThisBoot ? BLE_AUTO_RECONNECT_WAKE_GRACE_MS : BLE_AUTO_RECONNECT_BOOT_GRACE_MS;
  if (_btEnabledAt != 0 && now - _btEnabledAt < graceMs) {
    LOG_DBG("BT", "AutoReconnect skipped: boot grace active (%lu/%lu ms)", now - _btEnabledAt, graceMs);
    return;
  }

  const bool bondedPageTurner = bondedDeviceLooksLikePageTurner();
  if (userInputDetected) {
    _fastReconnectUntil = now + BLE_RECONNECT_FAST_WINDOW_MS;
    if (bondedPageTurner) {
      enterPageTurnerReconnectMode("user_input", false);
    }
  }

  const bool pageTurnerLostWindowActive = bondedPageTurner && pageTurnerReconnectModeActive(now);
  const bool pageTurnerFastWindowActive =
      pageTurnerLostWindowActive && pageTurnerReconnectFastModeActive(now);
  const bool fastWindowActive =
      _fastReconnectUntil != 0 && static_cast<int32_t>(now - _fastReconnectUntil) < 0;
  unsigned long requiredInterval =
      fastWindowActive
          ? (_autoReconnectWakeRequestedThisBoot ? BLE_RECONNECT_WAKE_INTERVAL_MS : BLE_RECONNECT_FAST_INTERVAL_MS)
          : reconnectBackoffMs(_reconnectFailureCount);
  if (pageTurnerLostWindowActive) {
    const unsigned long pageTurnerInterval = pageTurnerFastWindowActive ? BLE_PAGE_TURNER_RECONNECT_INTERVAL_MS
                                                                       : BLE_PAGE_TURNER_RECONNECT_SLOW_INTERVAL_MS;
    if (requiredInterval > pageTurnerInterval) {
      requiredInterval = pageTurnerInterval;
    }
  }
  if (_lastReconnectAttempt != 0 && now - _lastReconnectAttempt < requiredInterval) {
    LOG_DBG("BT", "AutoReconnect skipped: cooldown active (%lu/%lu ms)", now - _lastReconnectAttempt,
            requiredInterval);
    return;
  }

  _lastReconnectAttempt = now;
  BluetoothDiagnostics::recordf("auto_reconnect_attempt", "addr=%s failures=%u fast=%d pageTurner=%d pageTurnerFast=%d",
                                _bondedDeviceAddress.c_str(), static_cast<unsigned>(_reconnectFailureCount),
                                fastWindowActive, pageTurnerLostWindowActive, pageTurnerFastWindowActive);
  BluetoothDiagnostics::flushToStorage();

  if (!startBondedReconnect(BLE_MANUAL_RECONNECT_TIMEOUT_MS, true)) {
    BluetoothDiagnostics::recordf("auto_reconnect_queue_failed", "msg=%s", lastError.c_str());
    BluetoothDiagnostics::flushToStorage();
  }
}

void BluetoothHIDManager::saveState() {
  LOG_DBG("BT", "Saving state (stub)");
  // Stub: would save paired devices to file
}

void BluetoothHIDManager::loadState() {
  LOG_DBG("BT", "Loading state (stub)");
  // Stub: would load paired devices from file
}
