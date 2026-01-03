# ESP32 IR Web Remote (Wi-Fi Setup Portal + HTTP API + Web UI)

This project turns an **ESP32** into a small **IR transmitter** that you can control from a browser or via HTTP API calls.

Features:
- Wi-Fi setup portal (AP page on first setup / when Wi-Fi fails)
- Runs an HTTP server when connected to your Wi-Fi
- Web UI with:
  - configurable NEC **address** + **repeats**
  - preset buttons + custom command send
  - **shows last received IR code**
  - live log/status
  - device settings page to update SSID/password/device name
- **IR receiver + relay (re-transmit received IR):**
  - receives on `IR_RECEIVE_PIN`
  - relays via the transmitter on `IR_SEND_PIN`
  - relay can be toggled via API
- mDNS support (unique hostname per device): `http://<name>-<suffix>.local/`
- Designed for running multiple devices (unique suffix prevents name collisions)

---

## Tested hardware

✅ Tested with:
- **AZ-Delivery KY-005 IR infrared transmitter transceiver module**
- **AZDelivery KY-022 IR Receiver**
- **AZ-Delivery ESP32 Dev Kit C V4 NodeMCU WLAN WiFi Development Board** (USB-C)

---

## IDE

Install **"esp32 by Espressif Systems"** in Arduino IDE.

In **Tools → Board → esp32**, choose:

- **ESP32 Dev Module**

---

## Libraries

- **Arduino-IRremote** (Armin Joachimsmeyer)
- ESP32 Arduino core libraries:
  - `WiFi.h`
  - `WebServer.h`
  - `Preferences.h`
  - `ESPmDNS.h`

Install Arduino-IRremote from **Arduino IDE → Library Manager**.

---

## Wiring

![Diagram](https://github.com/dockdv/esp32-ir-repeater/blob/main/docs/images/IR%20Repeater.png)

Tested wiring:
- **KY-005 (IR transmitter)**: signal → `IR_SEND_PIN` (default `GPIO2`), VCC/GND per module specs
- **KY-022 (IR receiver)**: OUT → `IR_RECEIVE_PIN` (default `GPIO27`), VCC (3.3V), GND

> If you change pins, update `IR_SEND_PIN` / `IR_RECEIVE_PIN` in the sketch.

---

## Setup flow

1. Boot device:
   - It tries to connect to the previously saved Wi-Fi.
2. If it cannot connect (or BOOT button held at boot):
   - It starts an **open** AP with an SSID like:  
     `ESP32-Setup-<suffix>`
   - Connect to it and open:  
     `http://192.168.4.1`
   - Enter:
     - SSID
     - password (optional)
     - device name (optional)
3. Device reboots and connects to your Wi-Fi.
4. Open the main UI:
   - via IP shown in serial log, **or**
   - via mDNS:  
     `http://<device-name>-<suffix>.local/`

---

## Web UI

When connected to Wi-Fi, open the device in your browser:
- `http://<device-ip>/`  
or
- `http://<mdns-name>.local/`

The UI provides:
- Device Settings:
  - change SSID/password/device name (reboots after saving)
- IR Settings:
  - NEC Address
  - repeats
- Send Presets + Send Custom command
- **Last received IR** (live)
- Live log/status

---

## HTTP API

### Send NEC command (address selectable)

Send an NEC command (values can be decimal or hex like `0x1B`):

```
GET /api/ir/send?addr=0x01&cmd=0x1B&repeats=0
```

Parameters:
- `cmd` (required): 0–255
- `addr` (optional, default is firmware’s `NEC_ADDR_DEFAULT`): 0–255
- `repeats` (optional, default is firmware’s `NEC_REPEATS_DEFAULT`): 0–255

Examples:
```
/api/ir/send?addr=1&cmd=27
/api/ir/send?addr=0x01&cmd=0x1B&repeats=3
```

### IR relay toggle

Enable/disable IR relaying (receive → retransmit):

```
GET /api/ir/relay?enable=1
GET /api/ir/relay?enable=0
```

### Status / logs

```
GET /api/status
```

Returns (among other fields):
- `lastRx`: last received IR decode line shown in the UI
- `relayEnabled`: current relay state
- `logs`: recent log lines

### Update Wi-Fi + device name (STA mode)

```
POST /api/wifi/set
Content-Type: application/x-www-form-urlencoded

ssid=<...>&pass=<...>&name=<...>
```

### Forget Wi-Fi and reboot to portal

```
GET /api/wifi/forget
```

---

## Notes

- Credentials are stored in ESP32 flash (NVS) using `Preferences`, so they persist across resets and firmware uploads.
- mDNS behavior depends on your OS:
  - works well on iOS/macOS
  - on Windows it may depend on installed mDNS/Bonjour support
- If you don’t see the setup AP SSID:
  - the device may have successfully connected to your saved Wi-Fi (then it won’t start AP),
  - or hold **BOOT** during power-on to force portal mode.

---

## License

MIT License.
