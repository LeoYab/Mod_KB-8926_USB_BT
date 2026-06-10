# IBM KB-8926 PS/2 to USB & Bluetooth Converter

Firmware for the **ESP32-S3-N16R8** that converts an IBM KB-8926 PS/2 keyboard into a modern USB and Bluetooth Low Energy (BLE) HID device. Supports up to **3 independent Bluetooth profiles** and **1 USB profile**, with hot-switching between them using keyboard shortcuts.

---

## Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Pin Configuration](#pin-configuration)
- [Wiring Diagram](#wiring-diagram)
- [Software Requirements](#software-requirements)
- [Arduino IDE Board Configuration](#arduino-ide-board-configuration)
- [Installation](#installation)
- [Usage](#usage)
  - [Mode Switching Shortcuts](#mode-switching-shortcuts)
  - [LED Indicators](#led-indicators)
  - [Factory Reset](#factory-reset)
- [BLE Profiles](#ble-profiles)
- [Technical Details](#technical-details)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Features

- Full PS/2 Set 2 scancode to USB HID keycode translation
- Native USB HID keyboard (no external USB host chip required)
- Bluetooth Low Energy HID with 3 independent profiles
- Bidirectional LED synchronization (Caps Lock, Num Lock, Scroll Lock) across USB, BLE and the physical keyboard
- Persistent mode and LED state across power cycles (stored in NVS flash)
- Connection status feedback through keyboard LED blinking patterns
- Hot-switching between USB and 3 BLE profiles via keyboard shortcuts
- Factory reset to clear all stored settings and bonding data
- Support for all standard keys including Pause/Break and Print Screen
- 6-Key Rollover (6KRO) HID reports
- Fast BLE advertising interval (20-40 ms) for quick connections
- Secure BLE bonding with Just Works pairing (no PIN required)

---

## Hardware Requirements

| Component | Specification |
|---|---|
| **Microcontroller** | ESP32-S3-N16R8 (16 MB Flash, 8 MB PSRAM, native USB) |
| **Keyboard** | IBM KB-8926 (or any PS/2 keyboard using Scan Code Set 2) |
| **Connector** | PS/2 Mini-DIN 6-pin female connector or direct wiring |
| **Power** | USB-C (from ESP32-S3 dev board) |

> The ESP32-S3 is required specifically because it has a native USB peripheral (USB OTG), which is used to implement the USB HID device. Other ESP32 variants (ESP32, ESP32-C3, etc.) do not have this capability.

---

## Pin Configuration

### ESP32-S3-N16R8 Pins Used

| Function | GPIO | Direction | Description |
|---|---|---|---|
| PS/2 Data | **GPIO 18** | Bidirectional | PS/2 keyboard data line |
| PS/2 Clock | **GPIO 17** | Bidirectional | PS/2 keyboard clock line (interrupt-driven) |
| USB D- | **GPIO 19** | Output | Native USB data minus (directly on the ESP32-S3-N16R8 dev board USB connector) |
| USB D+ | **GPIO 20** | Output | Native USB data plus (directly on the ESP32-S3-N16R8 dev board USB connector) |

### PS/2 Connector Pinout (Mini-DIN 6-pin)

```
    Pin Layout (looking at the connector face)

          6   5
        4       3
          2   1

    Pin 1: Data    --> GPIO 18
    Pin 2: N/C
    Pin 3: GND     --> GND
    Pin 4: VCC     --> 5V
    Pin 5: Clock   --> GPIO 17
    Pin 6: N/C
```

> Both PS/2 Data and Clock lines use the internal pull-up resistors of the ESP32-S3. No external pull-up resistors are needed.

---

## Wiring Diagram

## ESP32-S3 N16R8 Generic Boards

Some generic ESP32-S3 N16R8 development boards do **not** connect the USB input voltage to the 5V pin by default.

To make **5V available on the 5V pin**, you must bridge the **IN-OUT** solder pads on the back/front of the board (depending on the board revision).

After bridging these pads, the board's USB 5V supply will be available on the 5V pin and can be used to power the PS/2 keyboard.

![ESP32-S3 N16R8 IN-OUT Pads](/Images/esp32_s3_in_out.png)

---

```
    IBM KB-8926 (PS/2)                    ESP32-S3-N16R8
    ==================                    ==============

    PS/2 Pin 1 (Data)  ───────────────>   GPIO 18
    PS/2 Pin 5 (Clock) ───────────────>   GPIO 17
    PS/2 Pin 3 (GND)   ───────────────>   GND
    PS/2 Pin 4 (VCC)   ───────────────>   5V


    ESP32-S3 USB-C Port
    ====================
    Connect directly to the host PC for USB HID mode.
    GPIO 19 (D-) and GPIO 20 (D+) are routed through
    the dev board's USB-C connector internally.
```

---

## Software Requirements

| Software | Version |
|---|---|
| **Arduino IDE** | 2.x or later |
| **ESP32 Board Package** | 3.x (via Boards Manager) |
| **NimBLE-Arduino** | 2.x (via Library Manager) |

### Required Libraries

Install these through the Arduino IDE Library Manager:

1. **NimBLE-Arduino** by h2zero -- BLE stack implementation for ESP32

The following libraries are included with the ESP32 board package and do not need separate installation:

- `USB.h` / `USBHIDKeyboard.h` -- Native USB HID
- `Preferences.h` -- NVS flash persistent storage
- `nvs_flash.h` -- NVS low-level operations
- `tusb.h` -- TinyUSB (bundled with ESP32-S3 Arduino core)

---

## Arduino IDE Board Configuration

Select the following settings under **Tools** menu:

| Setting | Value |
|---|---|
| **Board** | ESP32S3 Dev Module |
| **USB CDC On Boot** | Enabled |
| **USB Mode** | USB-OTG (TinyUSB) |
| **Flash Size** | 16MB (128Mb) |
| **PSRAM** | OPI PSRAM |
| **Partition Scheme** | Default 4MB with spiffs (or larger) |
| **Upload Speed** | 921600 |

> USB Mode **must** be set to `USB-OTG (TinyUSB)` for the native USB HID keyboard to work. The default `Hardware CDC and JTAG` mode will not expose the HID device.

---

## Installation

1. Clone this repository:
   ```bash
   git clone https://github.com/LeoYab/Mod_KB-8926_USB_BT
   ```

2. Open `Mod_KB8926_USB_BT.ino` in the Arduino IDE.

3. Install the **NimBLE-Arduino** library through the Library Manager (Sketch > Include Library > Manage Libraries).

4. Select the board and configure settings as shown in [Arduino IDE Board Configuration](#arduino-ide-board-configuration).

5. Wire the PS/2 keyboard to the ESP32-S3 as shown in [Wiring Diagram](#wiring-diagram).

6. Connect the ESP32-S3 to your computer via USB-C.

7. Upload the firmware.

---

## Usage

### Mode Switching Shortcuts

All shortcuts use the **Right Ctrl** key as the modifier. Press and hold Right Ctrl, then press the number key:

| Shortcut | Action | Description |
|---|---|---|
| `Right Ctrl + 1` | Switch to BT1 | Activates Bluetooth profile 1. Device restarts. |
| `Right Ctrl + 2` | Switch to BT2 | Activates Bluetooth profile 2. Device restarts. |
| `Right Ctrl + 3` | Switch to BT3 | Activates Bluetooth profile 3. Device restarts. |
| `Right Ctrl + 4` | Switch to USB | Activates USB HID mode. Device restarts. |
| `Right Ctrl + +` (Numpad or Main) | Volume Up | Increases system volume. |
| `Right Ctrl + -` (Numpad or Main) | Volume Down | Decreases system volume. |
| `Right Ctrl + 0 (hold 3 seconds)` | Factory Reset | Hold Right Ctrl + 0 for 3 seconds to erase all settings, bonding data, and LED states. Device restarts. |

> The selected mode is saved to flash memory and persists across power cycles. The device will boot into the last selected mode.

### LED Indicators

The three keyboard LEDs (Num Lock, Caps Lock, Scroll Lock) provide visual feedback about the connection state:

| LED Pattern | Meaning |
|---|---|
| All 3 LEDs blinking (400ms interval) | Disconnected / Waiting for host connection |
| All 3 LEDs solid on (1 second) | Just connected to host |
| LEDs reflect actual state | Normal operation (connected) |

### Factory Reset

Press and hold `Right Ctrl + 0` for **3 seconds** to perform a factory reset. This will:

- Clear all saved mode preferences
- Erase all BLE bonding/pairing data from NVS
- Reset LED states to defaults (Num Lock on)
- Restart the device in USB mode

---

## BLE Profiles

Each Bluetooth profile advertises with a unique name and MAC address to prevent host-side caching conflicts:

| Profile | BLE Device Name | MAC Address |
|---|---|---|
| BT1 | `IBM KB-8926 BT1` | `CA:11:00:00:00:01` |
| BT2 | `IBM KB-8926 BT2` | `CA:22:00:00:00:02` |
| BT3 | `IBM KB-8926 BT3` | `CA:33:00:00:00:03` |

### BLE Security

- Pairing method: **Just Works** (no PIN entry required)
- Bonding is enabled, so paired devices reconnect automatically
- Secure Connections (SC) enabled
- MITM protection disabled (trade-off for compatibility with Just Works)

### BLE Advertising

- Interval: 20-40 ms (optimized for fast discovery)
- Flags: General Discoverable + BR/EDR Not Supported
- Appearance: Keyboard (0x03C1)
- Advertising stops when a device connects and resumes on disconnection (single-host per profile)

---

## Technical Details

### PS/2 Protocol Implementation

- **Reception**: Interrupt-driven on the clock falling edge. Each PS/2 frame is 11 bits (1 start + 8 data + 1 parity + 1 stop). A 2000 microsecond timeout resets the bit counter to handle noise and partial frames.
- **Transmission** (Host to Device): Used exclusively for setting keyboard LEDs via the `0xED` command. The host inhibits the clock, sends a request-to-send, and clocks out 10 bits. The keyboard ACKs with `0xFA`.
- **Buffer**: 64-byte circular buffer for received scancodes, processed in the main loop.

### HID Report Format

The firmware uses a standard 8-byte HID keyboard report:

```
Byte 0: Modifier keys (bitmap)
         Bit 0: Left Ctrl     Bit 4: Right Ctrl
         Bit 1: Left Shift    Bit 5: Right Shift
         Bit 2: Left Alt      Bit 6: Right Alt
         Bit 3: Left GUI      Bit 7: Right GUI
Byte 1: Reserved (0x00)
Bytes 2-7: Up to 6 simultaneous keycodes (6KRO)
```

### LED Synchronization

LED state is managed locally on the ESP32 to maintain consistency across mode switches:

1. **USB mode**: The host sends LED state via USB HID Output Reports. The firmware updates the PS/2 keyboard LEDs accordingly.
2. **BLE mode**: The host sends LED state via BLE HID Output Reports. The bit mapping is translated from HID convention to PS/2 convention.
3. **Toggle keys** (Caps/Num/Scroll Lock): The firmware tracks the toggle state locally and immediately updates the PS/2 LEDs, without waiting for the host report.
4. **Persistence**: LED state per profile is saved to NVS flash and restored on boot.

### PS/2 to HID Bit Mapping for LEDs

| LED | HID Bit | PS/2 Bit |
|---|---|---|
| Num Lock | Bit 0 | Bit 1 |
| Caps Lock | Bit 1 | Bit 2 |
| Scroll Lock | Bit 2 | Bit 0 |

### Memory Usage

- **Flash**: Mode and LED states stored in NVS (Preferences library)
- **BLE bonding data**: Stored in NVS, preserved across `stopBLE()`/`startBLE()` cycles, erased only on factory reset

---

## Troubleshooting

| Problem | Solution |
|---|---|
| USB device not recognized | Ensure USB Mode is set to `USB-OTG (TinyUSB)` in the Arduino IDE board settings. Check that GPIO 19/20 are not used by other peripherals. |
| BLE device not appearing | Verify you are scanning for BLE (not classic Bluetooth). The device name will be `IBM KB-8926 BT1/BT2/BT3` depending on the active profile. |
| Keys not working | Check PS/2 wiring. Data must be on GPIO 18 and Clock on GPIO 17. Verify pull-ups are active (the firmware enables internal pull-ups). |
| LEDs not updating | The PS/2 LED command may fail silently. Check Serial Monitor output at 115200 baud for `setLeds` messages. |
| Cannot switch modes | Ensure you are using the **Right Ctrl** key (not Left Ctrl). The shortcut requires holding Right Ctrl and pressing 1-4 or 0. |
| BLE not reconnecting | Perform a factory reset (`Right Ctrl + 0`) and re-pair the device. Old bonding data may be stale. |
| Multiple keypresses or ghost keys | This can indicate noise on the PS/2 lines. Keep wires short and away from power lines. |

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
