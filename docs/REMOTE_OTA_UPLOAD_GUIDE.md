# Remote Code Upload (OTA) & Multi-Node Deployment Plan

This guide details how **Over-The-Air (OTA) Remote Code Upload** works for both ESP32 nodes connected to your Wi-Fi network (`JioFiber-Technokind`). 

Once the initial sketch is flashed once over USB, **you will never need to connect a USB cable again**. You can wirelessly flash updates directly from your computer over Wi-Fi while both sensors are permanently installed at the tap head and rooftop tank!

---

## 1. Network & OTA Identifiers

Both devices connect to your Wi-Fi with distinct network hostnames:

| Device | Role | Wi-Fi SSID | mDNS / Hostname | OTA Port | OTA Password |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **ESP32 Node 1** | Tap Head Pressure Node | `JioFiber-Technokind` | `esp32-tap-pressure.local` | `3232` | `admin123` |
| **ESP32 Node 2** | Overhead Tank Ultrasonic Node | `JioFiber-Technokind` | `esp32-tank-ultrasonic.local` | `3232` | `admin123` |

---

## 2. Step-by-Step Implementation Plan

### Phase 1: Initial Flash (One-Time via USB)
1. Connect **ESP32 Node 1** to your PC via USB cable.
2. In Arduino IDE or PlatformIO, select your COM Port and upload `esp32_pressure_tap_node.ino`.
3. Open Serial Monitor at `115200` baud:
   * Verify it connects to `JioFiber-Technokind`.
   * Note the IP address displayed (e.g. `192.168.29.X`).
   * Confirm the line: `[OTA] Remote Code Upload ready on hostname 'esp32-tap-pressure'`.
4. Disconnect Node 1, plug in **ESP32 Node 2**, and upload `esp32_ultrasonic_tank_node.ino`.
5. Verify its connection and IP address in Serial Monitor.

*(From this point onward, USB is no longer needed!)*

---

### Phase 2: Remote Wireless Upload Methods

You can upload firmware wirelessly using either **Arduino IDE**, **PlatformIO (VS Code)**, or the command line via **`espota.py`**.

#### Method A: Using Arduino IDE
1. Ensure your PC is connected to the same Wi-Fi: `JioFiber-Technokind`.
2. In Arduino IDE, open **Tools > Port**.
3. Under the **Network Ports** section, you will see your ESP32s listed:
   * `esp32-tap-pressure at 192.168.29.X`
   * `esp32-tank-ultrasonic at 192.168.29.Y`
4. Select the device you wish to update.
5. Click **Upload** (arrow icon).
6. When prompted for password, enter: `admin123`.
7. The code will compile and transmit over Wi-Fi into flash memory!

*(Note: If Network Ports do not appear immediately on Windows, ensure Apple Bonjour service is installed, or use Method B / C).*

---

#### Method B: Using PlatformIO (VS Code)
In each node's `platformio.ini`, you can specify the upload protocol and target IP:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

; Remote Wireless Upload Settings
upload_protocol = espota
upload_port = 192.168.29.X     ; Or esp32-tap-pressure.local
upload_flags =
    --port=3232
    --auth=admin123
```

Click PlatformIO **Upload**; it will push the binary directly over Wi-Fi.

---

#### Method C: Direct Command-Line Upload (`espota.py`)
Arduino and ESP-IDF come with `espota.py`. You can flash any compiled `.bin` file with:

```powershell
python $env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\espota.py -i 192.168.29.X -p 3232 -a admin123 -f build/firmware.bin
```

---

## 3. Deployment Safety Features Built into Firmware

1. **Non-Blocking OTA Polling:** `ArduinoOTA.handle()` runs on every loop iteration without causing pauses or stutter in sensor sampling.
2. **Safe Rollback Protection:** If an OTA upload is interrupted halfway, the ESP32 retains the previous working firmware partition.
3. **Password Authentication:** Protected with password `admin123` so unauthorized users on the Wi-Fi cannot overwrite your firmware.
