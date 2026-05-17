#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include "DeviceProfiles.h"

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  uint8_t addressType = 0;
  bool isHID = false;
  bool connectable = false;
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  unsigned long connectedTime = 0;    // Timestamp when BLE link was established
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  uint8_t lastHIDKeycode = 0x00;       // Track last keycode to detect press/release transitions
  unsigned long lastInjectionTime = 0; // Cooldown for button injection to prevent flooding
  uint8_t lastInjectedKeycode = 0x00;  // Track last injected key for smarter cooldown
  uint8_t activeInjectedButton = 0xFF; // Currently held virtual button, if any
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect
  bool hasSeenRelease = false;         // Ignore startup noise until a release frame is seen
  bool lastButtonState = false;        // Track button pressed state (from byte[0])
  const DeviceProfiles::DeviceProfile* profile = nullptr;  // Device-specific HID profile
  bool simpleFallbackEnabled = false;
  uint8_t simpleForwardKeycode = 0x00;
  uint8_t simpleBackKeycode = 0x00;
  bool descriptorHasConsumerPage = false;
  bool descriptorHasKeyboardPage = false;
  uint8_t descriptorSuggestedIndex = 0xFF;
  unsigned long lastNormalizedEventMs = 0;
  uint8_t lastNormalizedKeycode = 0x00;
  bool lastNormalizedPressed = false;
  uint8_t lastNormalizedDirection = 0xFF;  // 0x00=back, 0x01=forward, 0xFF=unknown
  uint16_t lastGameBrickCounter = 0xFFFF;  // For counter-freeze detection (button vs joystick)
  uint8_t lastGameBrickActiveKey = 0x00;   // Latched first key per freeze-window (prevents overshoot misfires)
  uint8_t gameBrickCenterPressFrames = 0;  // Centered horizontal active-frame streak (LEFT fallback)
  bool pendingGameBrickRelease = false;    // Delay short A/B release tails so one long hold stays merged
  unsigned long pendingGameBrickReleaseMs = 0;
  uint8_t pendingGameBrickKeycode = 0x00;
  uint8_t pendingGameBrickButton = 0xFF;
};

struct BluetoothReconnectStatus {
  enum class State : uint8_t {
    Idle,
    Running,
    Succeeded,
    Failed
  };

  State state = State::Idle;
  std::string message;
};

class BluetoothHIDManager {
public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const { return _enabled; }

  // Scanning
  void startScan(uint32_t durationMs = 10000);
  void stopScan();
  bool isScanning() const { return _scanning; }
  std::vector<BluetoothDevice> getDiscoveredDevices() const;

  // Connection
  bool connectToDevice(const std::string& address, uint32_t timeoutMs = 10000, uint8_t addressType = 0xFF,
                       const std::string& nameHint = "");
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  std::vector<std::string> getConnectedDevices() const;
  bool startPairNewRemote(uint32_t timeoutMs = 12000);
  bool startBondedReconnect(uint32_t timeoutMs = 6000, bool automatic = false);
  BluetoothReconnectStatus getReconnectStatus();
  bool consumeReconnectResult(bool& success, std::string& message);
  bool isBondedReconnectInProgress() const { return _reconnectJobRunning; }
  bool isPairingInProgress() const { return _reconnectJobRunning && _reconnectJobPairNew; }
  bool isAutoReconnectArmed() const { return _autoReconnectArmed && !_autoReconnectDisabledThisBoot; }

  // Input handling
  void processInputEvents();
  void setInputCallback(std::function<void(uint16_t keycode)> callback);
  void setLearnInputCallback(std::function<void(uint8_t keycode, uint8_t reportIndex)> callback);
  void setButtonInjector(std::function<void(uint8_t buttonIndex, bool pressed)> injector);
  void setReaderContextCallback(std::function<bool()> callback);
  void setButtonActivityNotifier(std::function<void(uint8_t buttonIndex)> notifier);
  void setDebugCaptureEnabled(bool enabled) { _debugCaptureEnabled = enabled; }
  bool isDebugCaptureEnabled() const { return _debugCaptureEnabled; }
  void setBondedDevice(const std::string& address, const std::string& name = "", uint8_t addressType = 0);
  std::string getBondedDeviceAddress() const { return _bondedDeviceAddress; }
  std::string getBondedDeviceName() const { return _bondedDeviceName; }
  uint8_t getBondedDeviceAddressType() const { return _bondedDeviceAddressType; }
  void updateActivity();  // Call periodically to check inactivity timeout
  void checkAutoReconnect(bool userInputDetected = false);  // Reconnect bonded device when disconnected
  void armAutoReconnectOnNextWake();  // Persist a one-shot reconnect request before intentional sleep
  
  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;
  bool hadRecentFree2Input(unsigned long windowMs = 1500) const;

  // State persistence
  void saveState();
  void loadState();

  std::string lastError;

  // BLE callbacks (public for NimBLE callbacks)
  void onScanResult(NimBLEAdvertisedDevice* advertisedDevice);
  void onClientDisconnect(const char* address, int reason);
  static void onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

private:
  BluetoothHIDManager();
  ~BluetoothHIDManager();
  BluetoothHIDManager(const BluetoothHIDManager&) = delete;
  BluetoothHIDManager& operator=(const BluetoothHIDManager&) = delete;

  void cleanup();
  void ensureReconnectMutex();
  void ensureScanResultsMutex() const;
  void ensureConnectedDevicesMutex() const;
  void clearDiscoveredDevices();
  size_t discoveredDeviceCount() const;
  void initializeAutoReconnectGuard();
  void setAutoReconnectGuard(bool active);
  void setAutoReconnectWakeRequest(bool active);
  bool consumeAutoReconnectWakeRequest();
  void armAutoReconnect(const char* reason, bool clearCrashGuard = false);
  void markIntentionalDisconnect(const std::string& address);
  bool consumeIntentionalDisconnect(const char* address);
  bool bondedDeviceLooksLikePageTurner() const;
  bool pageTurnerReconnectModeActive(unsigned long now);
  bool pageTurnerReconnectFastModeActive(unsigned long now);
  void enterPageTurnerReconnectMode(const char* reason, bool confirmedPageTurner = false);
  void ensureInputEventQueue();
  void ensureConnectionEventQueue();
  void queueInputReport(const char* address, const uint8_t* data, size_t length);
  void processConnectionEvents();
  void processQueuedHIDReport(const char* address, const uint8_t* data, size_t length);
  static void bondedReconnectTaskEntry(void* param);
  void runBondedReconnectTask();
  bool scanForBondedReconnectCandidate(BluetoothDevice& candidate, uint32_t scanMs);
  bool scanForPairingCandidate(BluetoothDevice& candidate, uint32_t scanMs);
  bool findBondedReconnectCandidate(BluetoothDevice& candidate) const;
  bool findPairingCandidate(BluetoothDevice& candidate);
  bool matchesBondedReconnectCandidate(const BluetoothDevice& device, const std::string& address,
                                       const std::string& name) const;
  bool isHighConfidencePairingCandidate(const BluetoothDevice& device) const;
  uint16_t parseHIDReport(uint8_t* data, size_t length);
  ConnectedDevice* findConnectedDeviceLocked(const std::string& address);
  uint8_t mapKeycodeToButton(uint8_t keycode, ConnectedDevice* device);

  bool _enabled = false;
  bool _scanning = false;
  std::vector<BluetoothDevice> _discoveredDevices;
  std::vector<ConnectedDevice> _connectedDevices;
  std::function<void(uint16_t)> _inputCallback;
  std::function<void(uint8_t, uint8_t)> _learnInputCallback;
  std::function<void(uint8_t, bool)> _buttonInjector;
  std::function<bool()> _readerContextCallback;
  std::function<void(uint8_t)> _buttonActivityNotifier;
  bool _debugCaptureEnabled = false;
  std::string _bondedDeviceAddress;
  std::string _bondedDeviceName;
  uint8_t _bondedDeviceAddressType = 0;
  unsigned long _scanStopTime = 0;
  unsigned long _lastReconnectCheck = 0;
  unsigned long _lastReconnectAttempt = 0;
  unsigned long _fastReconnectUntil = 0;
  uint8_t _reconnectFailureCount = 0;
  unsigned long _btEnabledAt = 0;
  bool _autoReconnectArmed = false;
  bool _autoReconnectDisabledThisBoot = false;
  bool _autoReconnectGuardInitialized = false;
  bool _autoReconnectGuardPresent = false;
  bool _autoReconnectWakeRequestedThisBoot = false;
  volatile bool _reconnectJobRunning = false;
  volatile bool _reconnectJobFinished = false;
  volatile bool _reconnectJobSuccess = false;
  volatile bool _reconnectScanMatched = false;
  std::string _reconnectJobMessage;
  uint32_t _reconnectJobTimeoutMs = 0;
  uint32_t _reconnectJobScanMs = 0;
  bool _reconnectJobAutomatic = false;
  bool _reconnectJobPairNew = false;
  TaskHandle_t _reconnectTaskHandle = nullptr;
  QueueHandle_t _inputEventQueue = nullptr;
  QueueHandle_t _connectionEventQueue = nullptr;
  SemaphoreHandle_t _reconnectMutex = nullptr;
  mutable SemaphoreHandle_t _scanResultsMutex = nullptr;
  mutable SemaphoreHandle_t _connectedDevicesMutex = nullptr;

  static constexpr size_t MAX_HID_REPORT_BYTES = 16;
  struct QueuedHIDReport {
    char address[18] = "";
    uint8_t data[MAX_HID_REPORT_BYTES] = {};
    uint8_t length = 0;
  };

  struct QueuedConnectionEvent {
    char address[18] = "";
    int reason = 0;
    bool suppressAutoReconnect = false;
  };

  char _intentionalDisconnectAddress[18] = "";
  unsigned long _intentionalDisconnectMarkedAt = 0;
  unsigned long _pageTurnerReconnectUntil = 0;
  unsigned long _pageTurnerReconnectFastUntil = 0;
  
  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 300000;  // 5 minutes
  unsigned long lastMaintenanceCheck = 0;
};
