#include <nvs_flash.h>
#include "esp_mac.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include <tusb.h>

// --- PS/2 Pins ---------------------------------------------------------------
#define PS2_DATA  18
#define PS2_CLOCK 17

// --- PS/2 RX -----------------------------------------------------------------
#define PS2_BUF_SIZE 64
volatile uint8_t ps2_buffer[PS2_BUF_SIZE];
volatile uint8_t ps2_head = 0;
volatile uint8_t ps2_tail = 0;

volatile uint8_t ps2_bitCount  = 0;
volatile uint8_t ps2_incoming  = 0;
volatile bool    ledUpdatePending = false;
volatile uint8_t pendingLedStatus = 0;
volatile uint8_t localLedStatus = 0;
volatile unsigned long last_interrupt_time = 0;

// --- LED save debounce -------------------------------------------------------
bool ledSavePending = false;
unsigned long ledSaveTime = 0;
const unsigned long LED_SAVE_DELAY_MS = 5000; // Save to flash 5 seconds after last change

void IRAM_ATTR ps2interrupt() {
  unsigned long now = micros();
  if (now - last_interrupt_time > 2000) {
    ps2_bitCount = 0;
  }
  last_interrupt_time = now;

  bool bit = digitalRead(PS2_DATA);
  if (ps2_bitCount == 0) ps2_incoming = 0;
  if (ps2_bitCount > 0 && ps2_bitCount < 9)
    ps2_incoming |= (bit << (ps2_bitCount - 1));
  ps2_bitCount++;
  if (ps2_bitCount == 11) {
    uint8_t nextHead = (ps2_head + 1) % PS2_BUF_SIZE;
    if (nextHead != ps2_tail) {
      ps2_buffer[ps2_head] = ps2_incoming;
      ps2_head = nextHead;
    }
    ps2_bitCount  = 0;
    ps2_incoming  = 0;
  }
}

// --- Send data to PS/2 keyboard (Host -> Device) -----------------------------
bool ps2_write_raw(uint8_t data) {
  uint8_t parity = 1;
  for (uint8_t i = 0; i < 8; i++) {
    if (data & (1 << i)) {
      parity ^= 1;
    }
  }

  // Bits to send: 8 data bits + 1 parity + 1 stop bit (1)
  uint8_t bits[10];
  for (int i = 0; i < 8; i++) {
    bits[i] = (data >> i) & 1;
  }
  bits[8] = parity;
  bits[9] = 1;

  // 1. Inhibit communication (clock low for > 100 microseconds)
  pinMode(PS2_CLOCK, OUTPUT);
  digitalWrite(PS2_CLOCK, LOW);
  delayMicroseconds(120);

  // 2. Request-to-send (data low, release clock)
  pinMode(PS2_DATA, OUTPUT);
  digitalWrite(PS2_DATA, LOW); // Start bit (0)
  
  // Release the clock so the keyboard takes control
  pinMode(PS2_CLOCK, INPUT_PULLUP);

  // Wait for the keyboard to start the clock (pull clock line low) to read the start bit
  unsigned long start = millis();
  while (digitalRead(PS2_CLOCK) == HIGH) {
    if (millis() - start > 15) {
      pinMode(PS2_DATA, INPUT_PULLUP);
      return false;
    }
  }

  // Send the 10 bits (8 data + parity + stop)
  for (int i = 0; i < 10; i++) {
    // Wait for the clock to go high
    start = millis();
    while (digitalRead(PS2_CLOCK) == LOW) {
      if (millis() - start > 15) {
        pinMode(PS2_DATA, INPUT_PULLUP);
        return false;
      }
    }

    // Write the bit while the clock is HIGH
    if (bits[i]) {
      pinMode(PS2_DATA, INPUT_PULLUP);
    } else {
      pinMode(PS2_DATA, OUTPUT);
      digitalWrite(PS2_DATA, LOW);
    }

    // Wait for the clock to go low (keyboard reads the data)
    start = millis();
    while (digitalRead(PS2_CLOCK) == HIGH) {
      if (millis() - start > 15) {
        pinMode(PS2_DATA, INPUT_PULLUP);
        return false;
      }
    }
  }

  // Wait for the clock to go high again after the stop bit
  start = millis();
  while (digitalRead(PS2_CLOCK) == LOW) {
    if (millis() - start > 15) {
      pinMode(PS2_DATA, INPUT_PULLUP);
      return false;
    }
  }

  // Wait for the ACK from the keyboard (keyboard pulls DATA low)
  start = millis();
  while (digitalRead(PS2_DATA) == HIGH) {
    if (millis() - start > 15) {
      pinMode(PS2_DATA, INPUT_PULLUP);
      return false;
    }
  }

  // Wait for the keyboard to release clock (go HIGH) and DATA (go HIGH)
  start = millis();
  while (digitalRead(PS2_CLOCK) == LOW || digitalRead(PS2_DATA) == LOW) {
    if (millis() - start > 15) {
      pinMode(PS2_DATA, INPUT_PULLUP);
      return false;
    }
  }

  return true;
}

void setLeds(uint8_t ledStatus) {
  Serial.printf("setLeds: Sending command 0xED and LED data: 0x%02X to PS/2\n", ledStatus);
  // Detach interrupt during the entire send and ACK reception process
  detachInterrupt(digitalPinToInterrupt(PS2_CLOCK));

  if (ps2_write_raw(0xED)) {
    delay(3); // Wait for the keyboard to finish transmitting the ACK (0xFA)
    if (ps2_write_raw(ledStatus)) {
      delay(3); // Wait for the second ACK
    }
  }

  // Clear PS/2 receiver state to prevent bit misalignment and discard residual codes
  ps2_bitCount = 0;
  ps2_incoming = 0;
  ps2_head = 0;
  ps2_tail = 0;

  // Reattach the interrupt
  attachInterrupt(digitalPinToInterrupt(PS2_CLOCK), ps2interrupt, FALLING);
}

// --- USB and BLE Callbacks ---------------------------------------------------
static void usbHidEventCallback(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == ARDUINO_USB_HID_KEYBOARD_EVENTS) {
    if (event_id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
      auto* data = (arduino_usb_hid_keyboard_event_data_t*)event_data;
      Serial.printf("USB LED Event received: Num:%d Caps:%d Scroll:%d\n", data->numlock, data->capslock, data->scrolllock);
      uint8_t ledStatus = 0;
      if (data->scrolllock) ledStatus |= 0x01;
      if (data->numlock)    ledStatus |= 0x02;
      if (data->capslock)   ledStatus |= 0x04;
      localLedStatus = ledStatus;
      pendingLedStatus = ledStatus;
      ledUpdatePending = true;
      saveLedStatusDeferred(ledStatus);
    }
  }
}

class NimBLEConnInfo; // Forward declaration for NimBLE 1.x / 2.x compatibility

class BLEOutputCallbacks : public NimBLECharacteristicCallbacks {
public:
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    handleWrite(pCharacteristic);
  }
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    handleWrite(pCharacteristic);
  }
private:
  void handleWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      uint8_t hidLed = (uint8_t)value[0];
      uint8_t ledStatus = 0;
      if (hidLed & 0x01) ledStatus |= 0x02; // Num Lock (HID bit 0 -> PS/2 bit 1)
      if (hidLed & 0x02) ledStatus |= 0x04; // Caps Lock (HID bit 1 -> PS/2 bit 2)
      if (hidLed & 0x04) ledStatus |= 0x01; // Scroll Lock (HID bit 2 -> PS/2 bit 0)
      Serial.printf("BLE LED Event received. Raw: 0x%02X -> PS/2: 0x%02X\n", hidLed, ledStatus);
      localLedStatus = ledStatus;
      pendingLedStatus = ledStatus;
      ledUpdatePending = true;
      saveLedStatusDeferred(ledStatus);
    }
  }
};
static BLEOutputCallbacks bleOutputCallbacks;

// --- Modes -------------------------------------------------------------------
enum KeyboardMode { MODE_USB, MODE_BT1, MODE_BT2, MODE_BT3 };
KeyboardMode currentMode = MODE_USB;

// --- USB ---------------------------------------------------------------------
USBHIDKeyboard usbKeyboard;
USBHIDConsumerControl usbConsumer;

// --- BLE ---------------------------------------------------------------------
NimBLEServer*         bleServer = nullptr;
NimBLEHIDDevice*      bleHid    = nullptr;
NimBLECharacteristic* bleInput  = nullptr;
NimBLECharacteristic* bleConsumer = nullptr;
bool                  bleActive = false;

// Each BLE profile uses a different MAC address so the host does not confuse
// profiles with each other or use a cached name from a previous session.
// These are static local MACs (bit 6 of the first byte = 1, bit 7 = 1 -> 0xC0|...).
const uint8_t bleMacBase[3][6] = {
  { 0xCA, 0x11, 0x00, 0x00, 0x00, 0x01 },
  { 0xCA, 0x22, 0x00, 0x00, 0x00, 0x02 },
  { 0xCA, 0x33, 0x00, 0x00, 0x00, 0x03 },
};

const char* bleNames[3] = {
  "IBM KB-8926 BT1",
  "IBM KB-8926 BT2",
  "IBM KB-8926 BT3"
};

Preferences prefs;

void saveLedStatusNow(uint8_t status) {
  prefs.begin("keyboard", false);
  char key[8];
  sprintf(key, "led_%d", (int)currentMode);
  prefs.putUChar(key, status);
  prefs.end();
}

void saveLedStatusDeferred(uint8_t status) {
  ledSavePending = true;
  ledSaveTime = millis();
}

// --- PS/2 Set 2 -> HID Lookup Table -----------------------------------------
struct PS2toHID { uint8_t ps2; uint8_t hid; };

const PS2toHID ps2table[] = {
  {0x1C,0x04},{0x32,0x05},{0x21,0x06},{0x23,0x07},{0x24,0x08},{0x2B,0x09},
  {0x34,0x0A},{0x33,0x0B},{0x43,0x0C},{0x3B,0x0D},{0x42,0x0E},{0x4B,0x0F},
  {0x3A,0x10},{0x31,0x11},{0x44,0x12},{0x4D,0x13},{0x15,0x14},{0x2D,0x15},
  {0x1B,0x16},{0x2C,0x17},{0x3C,0x18},{0x2A,0x19},{0x1D,0x1A},{0x22,0x1B},
  {0x35,0x1C},{0x1A,0x1D},
  {0x16,0x1E},{0x1E,0x1F},{0x26,0x20},{0x25,0x21},{0x2E,0x22},{0x36,0x23},
  {0x3D,0x24},{0x3E,0x25},{0x46,0x26},{0x45,0x27},
  {0x5A,0x28},{0x76,0x29},{0x66,0x2A},{0x0D,0x2B},{0x29,0x2C},
  {0x4E,0x2D},{0x55,0x2E},{0x54,0x2F},{0x5B,0x30},{0x5D,0x31},
  {0x4C,0x33},{0x52,0x34},{0x0E,0x35},{0x41,0x36},{0x49,0x37},{0x4A,0x38},
  {0x58,0x39},{0x56,0x64},{0x61,0x64},
  {0x05,0x3A},{0x06,0x3B},{0x04,0x3C},{0x0C,0x3D},{0x03,0x3E},{0x0B,0x3F},
  {0x83,0x40},{0x0A,0x41},{0x01,0x42},{0x09,0x43},{0x78,0x44},{0x07,0x45},
  {0x12,0xE1},{0x59,0xE5},{0x14,0xE0},{0x11,0xE2},
  {0x77,0x53},{0x7C,0x55},{0x7B,0x56},{0x79,0x57},{0x71,0x63},
  {0x70,0x62},{0x69,0x59},{0x72,0x5A},{0x7A,0x5B},{0x6B,0x5C},
  {0x73,0x5D},{0x74,0x5E},{0x6C,0x5F},{0x75,0x60},{0x7D,0x61},
  {0x7E,0x47},
};
const int ps2tableSize = sizeof(ps2table) / sizeof(ps2table[0]);

// --- O(1) Direct Lookup Array ------------------------------------------------
uint8_t ps2ToHidMap[256];

void initPs2ToHidMap() {
  memset(ps2ToHidMap, 0, sizeof(ps2ToHidMap));
  for (int i = 0; i < ps2tableSize; i++) {
    ps2ToHidMap[ps2table[i].ps2] = ps2table[i].hid;
  }
}

// --- PS/2 State --------------------------------------------------------------
bool ps2_extended = false;
bool ps2_release  = false;
bool ps2_rightCtrl = false;
uint8_t pause_state = 0;

// --- HID State (6KRO) -------------------------------------------------------
uint8_t modifiers      = 0x00;
uint8_t pressedKeys[6] = {0,0,0,0,0,0};

void addKey(uint8_t hid) {
  for (int i = 0; i < 6; i++) {
    if (pressedKeys[i] == hid) return;
    if (pressedKeys[i] == 0x00) { pressedKeys[i] = hid; return; }
  }
  memmove(pressedKeys, pressedKeys + 1, 5);
  pressedKeys[5] = hid;
}

void removeKey(uint8_t hid) {
  for (int i = 0; i < 6; i++) {
    if (pressedKeys[i] == hid) {
      memmove(pressedKeys + i, pressedKeys + i + 1, 5 - i);
      pressedKeys[5] = 0x00;
      return;
    }
  }
}

// --- HID Report Map ----------------------------------------------------------
// Input Report ID 1:  modifiers (1 byte) + reserved (1 byte) + keys (6 bytes)
// Output Report ID 1: LEDs (5 bits) + padding (3 bits)
// A single Report ID for input+output is more compatible with Windows.
const uint8_t reportMap[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)

  // --- Input Report (modifiers) ---
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Key Codes)
  0x19, 0xE0,        //   Usage Minimum (224)
  0x29, 0xE7,        //   Usage Maximum (231)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)

  // --- Input Report (reserved) ---
  0x95, 0x01,
  0x75, 0x08,
  0x81, 0x01,        //   Input (Constant)

  // --- Input Report (keycodes) ---
  0x95, 0x06,
  0x75, 0x08,
  0x15, 0x00,
  0x25, 0x65,
  0x05, 0x07,
  0x19, 0x00,
  0x29, 0x65,
  0x81, 0x00,        //   Input (Data, Array)

  // --- Output Report (LEDs) ---
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01,        //   Usage Minimum (Num Lock)
  0x29, 0x05,        //   Usage Maximum (Kana)
  0x15, 0x00,
  0x25, 0x01,
  0x75, 0x01,
  0x95, 0x05,
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x95, 0x01,
  0x75, 0x03,
  0x91, 0x01,        //   Output (Constant) - padding

  0xC0,              // End Collection

  // --- Consumer Control Report ---
  0x05, 0x0C,        // Usage Page (Consumer Devices)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x02,        //   Report ID (2)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x02,        //   Report Count (2)
  0x09, 0xE9,        //   Usage (Volume Increment)
  0x09, 0xEA,        //   Usage (Volume Decrement)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x06,        //   Report Size (6)
  0x81, 0x01,        //   Input (Constant) - padding to 1 byte
  0xC0               // End Collection
};

// --- BLE Callbacks -----------------------------------------------------------
class BLECallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) {
    Serial.println("BLE: connected");
    // Stop advertising on connect (prevents other hosts from seeing the device
    // while it is in use)
    NimBLEDevice::stopAdvertising();
  }
  void onDisconnect(NimBLEServer* s) {
    Serial.println("BLE: disconnected - restarting advertising");
    NimBLEDevice::startAdvertising();
  }
};
static BLECallbacks bleCallbacksInstance;

void stopBLE() {
  if (!bleActive) return;
  if (bleServer) {
    std::vector<uint16_t> peerIds = bleServer->getPeerDevices();
    for (uint16_t id : peerIds) {
      bleServer->disconnect(id);
    }
    delay(300); // Wait for disconnection packets to be sent
  }
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(false); // false to preserve the bonding database in memory
  delay(200);
  bleServer   = nullptr;
  bleHid      = nullptr;
  bleInput    = nullptr;
  bleConsumer = nullptr;
  bleActive   = false;
  Serial.println("BLE stopped");
}

void startBLE(int profile) {
  stopBLE();

  // Change the Bluetooth MAC address directly before initializing the NimBLE stack.
  // We use ESP_MAC_BT instead of ESP_MAC_BASE to avoid incorrect derivations and ensure the change.
  esp_iface_mac_addr_set(bleMacBase[profile], ESP_MAC_BT); 

  NimBLEDevice::init(bleNames[profile]);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Security: bonding with Just Works (no PIN).
  // Required for the host to accept the device as a trusted keyboard.
  // We use direct numeric/boolean values to avoid macros that may fail during compilation.
  NimBLEDevice::setSecurityAuth(true, false, true); // bonding = true, mitm = false, sc = true
  NimBLEDevice::setSecurityIOCap(3);               // 3 = No Input, No Output (Just Works)

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&bleCallbacksInstance, true);

  bleHid      = new NimBLEHIDDevice(bleServer);
  bleInput    = bleHid->getInputReport(1);
  bleConsumer = bleHid->getInputReport(2);

  NimBLECharacteristic* bleOutput = bleHid->getOutputReport(1);
  if (bleOutput != nullptr) {
    bleOutput->setCallbacks(&bleOutputCallbacks);
  } else {
    Serial.println("WARNING: BLE getOutputReport(1) returned nullptr - LED callbacks not registered!");
  }

  bleHid->setManufacturer("IBM");
  bleHid->setPnp(0x01, 0x02D4, 0x0001, 0x0110);
  bleHid->setHidInfo(0x00, 0x03); // 0x03 = Normally Connectable + Remote Wake
  bleHid->setReportMap((uint8_t*)reportMap, sizeof(reportMap));
  bleHid->setBatteryLevel(100);
  bleHid->startServices();

  // Advertising: 20-40 ms interval recommended for HID (connects faster
  // on Windows and Linux; default NimBLE value is too slow).
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06); // 0x06 = General Discoverable + BR/EDR Not Supported
  advData.setAppearance(0x03C1);  // Keyboard
  advData.addServiceUUID(bleHid->getHidService()->getUUID());
  advData.addServiceUUID(bleHid->getBatteryService()->getUUID());
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanResponseData;
  scanResponseData.setName(bleNames[profile]);
  adv->setScanResponseData(scanResponseData);

  adv->setMinInterval(32);   // 32 * 0.625 ms = 20 ms
  adv->setMaxInterval(64);   // 64 * 0.625 ms = 40 ms
  adv->start();

  bleActive = true;
  Serial.printf("BLE active: %s\n", bleNames[profile]);
}

// --- Send Reports ------------------------------------------------------------
void bleSendReport() {
  if (!bleActive || !bleServer) return;
  if (bleServer->getConnectedCount() == 0) return;
  uint8_t report[8];
  report[0] = modifiers;
  report[1] = 0x00;
  memcpy(report + 2, pressedKeys, 6);
  bleInput->setValue(report, 8);
  bleInput->notify();
}

void usbSendReport() {
  KeyReport r;
  r.modifiers = modifiers;
  r.reserved  = 0x00;
  memcpy(r.keys, pressedKeys, 6);
  usbKeyboard.sendReport(&r);
}

void bleSendVolume(uint8_t volumeMask) {
  if (!bleActive || !bleServer || !bleConsumer) return;
  if (bleServer->getConnectedCount() == 0) return;
  uint8_t report = volumeMask; // bit 0 = Vol+, bit 1 = Vol-
  bleConsumer->setValue(&report, 1);
  bleConsumer->notify();
  delay(10);
  report = 0;
  bleConsumer->setValue(&report, 1);
  bleConsumer->notify();
}

void sendReport() {
  if (currentMode == MODE_USB) usbSendReport();
  else                         bleSendReport();
}

// --- Factory Reset -----------------------------------------------------------
void factoryReset() {
  Serial.println("FACTORY RESET");
  prefs.begin("keyboard", false);
  prefs.clear();
  prefs.end();
  nvs_flash_erase();
  nvs_flash_init();
  delay(1000);
  ESP.restart();
}

uint8_t ps2ToHid(uint8_t ps2) {
  return ps2ToHidMap[ps2];
}

// --- Process Scan Code -------------------------------------------------------
void processScanCode(uint8_t code) {
  if (pause_state > 0) {
    static const uint8_t pause_seq[7] = {0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77};
    if (code == pause_seq[pause_state - 1]) {
      pause_state++;
      if (pause_state == 8) {
        pause_state = 0;
        addKey(0x48); // Pause/Break in HID
        sendReport();
        removeKey(0x48);
        sendReport();
      }
    } else {
      pause_state = 0;
      if (code == 0xE1) {
        pause_state = 1;
      }
    }
    return;
  }

  if (code == 0xE1) {
    pause_state = 1;
    return;
  }

  if (code == 0xE0) { ps2_extended = true;  return; }
  if (code == 0xF0) { ps2_release  = true;  return; }

  bool ext = ps2_extended;
  bool rel = ps2_release;
  ps2_extended = false;
  ps2_release  = false;

  if (ext && code == 0x14) { ps2_rightCtrl = !rel; return; }

  if (ps2_rightCtrl) {
    if (code == 0x79 || code == 0x55) { // Keypad + or Standard +
      if (!rel) {
        if (currentMode == MODE_USB) {
          usbConsumer.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
          usbConsumer.release();
        } else {
          bleSendVolume(0x01);
        }
        Serial.println("Volume Up triggered");
      }
      return;
    }
    if (code == 0x7B || code == 0x4E) { // Keypad - or Standard -
      if (!rel) {
        if (currentMode == MODE_USB) {
          usbConsumer.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
          usbConsumer.release();
        } else {
          bleSendVolume(0x02);
        }
        Serial.println("Volume Down triggered");
      }
      return;
    }
  }

  static unsigned long reset_press_time = 0;
  static bool reset_pressed = false;

  if (ps2_rightCtrl && !rel) {
    int newProfile = -1;
    switch (code) {
      case 0x16: newProfile = 0; currentMode = MODE_BT1; break; // Right Ctrl + 1 = BT1
      case 0x1E: newProfile = 1; currentMode = MODE_BT2; break; // Right Ctrl + 2 = BT2
      case 0x26: newProfile = 2; currentMode = MODE_BT3; break; // Right Ctrl + 3 = BT3
      case 0x25: // Right Ctrl + 4 = USB
        prefs.begin("keyboard", false);
        prefs.putInt("mode", MODE_USB);
        prefs.end();
        Serial.println("Switching to USB... Restarting...");
        delay(200);
        ESP.restart();
        return;
      case 0x45: // Right Ctrl + 0 = Factory Reset (Requires holding 3 seconds)
        if (!reset_pressed) {
          reset_pressed = true;
          reset_press_time = millis();
          Serial.println("Factory Reset key combination detected. Keep holding for 3 seconds...");
        }
        break;
    }
    if (newProfile >= 0) {
      prefs.begin("keyboard", false);
      prefs.putInt("mode", currentMode);
      prefs.end();
      Serial.printf("Switching to BT%d... Restarting...\n", newProfile + 1);
      delay(200);
      ESP.restart();
      return;
    }
  }

  // Handle release of factory reset key (0x45 represents '0' on Keypad / standard key)
  if (code == 0x45 && rel) {
    reset_pressed = false;
    reset_press_time = 0;
  }

  // Check if reset key combination has been held for 3 seconds
  if (reset_pressed && (millis() - reset_press_time >= 3000)) {
    reset_pressed = false;
    reset_press_time = 0;
    factoryReset();
    return;
  }

  uint8_t hidCode = 0x00;

  if (ext) {
    switch (code) {
      case 0x11: hidCode = 0xE6; break; // Right Alt
      case 0x14: hidCode = 0xE4; break; // Right Ctrl
      case 0x1F: hidCode = 0xE3; break; // Left Win
      case 0x27: hidCode = 0xE7; break; // Right Win
      case 0x2F: hidCode = 0x65; break; // Menu
      case 0x70: hidCode = 0x49; break; // Insert
      case 0x71: hidCode = 0x4C; break; // Delete
      case 0x6B: hidCode = 0x50; break; // Left Arrow
      case 0x72: hidCode = 0x51; break; // Down Arrow
      case 0x74: hidCode = 0x4F; break; // Right Arrow
      case 0x75: hidCode = 0x52; break; // Up Arrow
      case 0x6C: hidCode = 0x4A; break; // Home
      case 0x69: hidCode = 0x4D; break; // End
      case 0x7D: hidCode = 0x4B; break; // Page Up
      case 0x7A: hidCode = 0x4E; break; // Page Down
      case 0x4A: hidCode = 0x54; break; // Numpad /
      case 0x5A: hidCode = 0x58; break; // Numpad Enter
      case 0x7C: hidCode = 0x46; break; // Print Screen
    }
  } else {
    hidCode = ps2ToHid(code);
  }

  if (hidCode == 0x00) return;

  if (hidCode >= 0xE0 && hidCode <= 0xE7) {
    uint8_t bit = 1 << (hidCode - 0xE0);
    if (!rel) modifiers |=  bit;
    else      modifiers &= ~bit;
    sendReport();
    return;
  }

  if (currentMode != MODE_USB && !rel) {
    if (hidCode == 0x39) { // Caps Lock
      localLedStatus ^= 0x04;
      pendingLedStatus = localLedStatus;
      ledUpdatePending = true;
      saveLedStatusDeferred(localLedStatus);
    } else if (hidCode == 0x53) { // Num Lock
      localLedStatus ^= 0x02;
      pendingLedStatus = localLedStatus;
      ledUpdatePending = true;
      saveLedStatusDeferred(localLedStatus);
    } else if (hidCode == 0x47) { // Scroll Lock
      localLedStatus ^= 0x01;
      pendingLedStatus = localLedStatus;
      ledUpdatePending = true;
      saveLedStatusDeferred(localLedStatus);
    }
  }

  if (!rel) addKey(hidCode);
  else      removeKey(hidCode);
  sendReport();
}



// --- Setup -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(10); // Reduced to speed up boot

  // Load the saved mode from Preferences.
  prefs.begin("keyboard", false);
  currentMode = (KeyboardMode)prefs.getInt("mode", MODE_USB);
  
  // Load the saved LED state for this profile
  char ledKey[8];
  sprintf(ledKey, "led_%d", (int)currentMode);
  localLedStatus = prefs.getUChar(ledKey, 0x02); // 0x02 = Num Lock active by default
  prefs.end();
  pendingLedStatus = localLedStatus;

  // Only initialize USB if we are in USB mode
  if (currentMode == MODE_USB) {
    pinMode(19, OUTPUT);
    pinMode(20, OUTPUT);
    digitalWrite(19, LOW);
    digitalWrite(20, LOW);
    delay(200); // Reduced from 500ms to 200ms for fast re-enumeration
    
    usbKeyboard.onEvent(usbHidEventCallback);
    usbKeyboard.begin();
    usbConsumer.begin();
    USB.begin();
    delay(100);  // Reduced from 2000ms to 100ms
  }

  // Initialize the O(1) PS/2 to HID lookup map
  initPs2ToHidMap();

  pinMode(PS2_DATA,  INPUT_PULLUP);
  pinMode(PS2_CLOCK, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PS2_CLOCK), ps2interrupt, FALLING);

  // Restore the saved LED state for this profile
  delay(100); // Reduced for faster initialization
  setLeds(localLedStatus);

  if (currentMode == MODE_USB) {
    Serial.println("IBM KB-8926 ready - USB mode");
  } else {
    int profile = 0;
    if (currentMode == MODE_BT2) profile = 1;
    if (currentMode == MODE_BT3) profile = 2;
    startBLE(profile);
    Serial.printf("IBM KB-8926 ready - BT%d mode\n", profile + 1);
  }

  Serial.println("Shortcuts: Right Ctrl + 1/2/3 = BT1/BT2/BT3 | Right Ctrl + 4 = USB | Right Ctrl + +/- = Vol Up/Down | Right Ctrl + 0 = Reset");
}

// --- Connection State --------------------------------------------------------
enum ConnectionState { STATE_DISCONNECTED, STATE_JUST_CONNECTED, STATE_CONNECTED };
ConnectionState connState = STATE_DISCONNECTED;
unsigned long connectionTime = 0;

// --- Loop --------------------------------------------------------------------
void loop() {
  while (ps2_tail != ps2_head) {
    uint8_t code = ps2_buffer[ps2_tail];
    ps2_tail = (ps2_tail + 1) % PS2_BUF_SIZE;
    processScanCode(code);
  }

  // --- Connection state and LED management ---
  bool currentlyConnected = false;
  if (currentMode == MODE_USB) {
    currentlyConnected = tud_mounted();
  } else {
    currentlyConnected = (bleActive && bleServer && bleServer->getConnectedCount() > 0);
  }

  static unsigned long lastBlink = 0;
  static bool blinkState = false;

  if (!currentlyConnected) {
    // If disconnected, return to STATE_DISCONNECTED
    if (connState != STATE_DISCONNECTED) {
      connState = STATE_DISCONNECTED;
      blinkState = false;
      lastBlink = 0;
    }
    
    // Blink all 3 LEDs (400ms on, 400ms off), but skip if shortcut is being used
    if (!ps2_rightCtrl && millis() - lastBlink > 400) {
      lastBlink = millis();
      blinkState = !blinkState;
      setLeds(blinkState ? 0x07 : 0x00);
    }
  } 
  else {
    // If was disconnected and now connected
    if (connState == STATE_DISCONNECTED) {
      connState = STATE_JUST_CONNECTED;
      connectionTime = millis();
      // Turn on all 3 LEDs
      setLeds(0x07);
    } 
    else if (connState == STATE_JUST_CONNECTED) {
      // Wait 1 second with all 3 LEDs on
      if (millis() - connectionTime > 1000) {
        connState = STATE_CONNECTED;
        // Restore saved LED state
        setLeds(localLedStatus);
      }
    } 
    else {
      // STATE_CONNECTED: Normal behavior
      if (ledUpdatePending) {
        ledUpdatePending = false;
        setLeds(pendingLedStatus);
      }
    }
  }

  // --- Debounced LED save to flash ---
  if (ledSavePending && (millis() - ledSaveTime >= LED_SAVE_DELAY_MS)) {
    ledSavePending = false;
    saveLedStatusNow(localLedStatus);
    Serial.println("LED status saved to flash (debounced)");
  }
}