# GitHub Continuous Integration & Wireless OTA Flashing Guide

This repository is configured with **GitHub Actions CI/CD** to automatically compile your ESP32 code on every Git push, eliminating the need to compile locally or attach USB cables!

---

## 1. Repository Details
* **Repository:** [https://github.com/SubhraJyotiDey/SmartTank-Dissertation](https://github.com/SubhraJyotiDey/SmartTank-Dissertation)
* **Visibility:** **Private**
* **Branch:** `main`

---

## 2. How the Automated Workflow Operates

```text
  [You Edit Code on PC] 
          │
          ▼
   `git commit & push`
          │
          ▼
 [GitHub Actions Cloud Runner] 
          │ (Builds on Ubuntu in ~60s using PlatformIO)
          ▼
 [Compiled Binaries Produced]
    ├── pressure_node_firmware.bin
    └── ultrasonic_node_firmware.bin
          │
          ▼
 [Saved as Downloadable Build Artifacts & Releases]
```

Every time you modify code in `firmware/` and push to GitHub:
1. GitHub Actions spins up a clean cloud build environment.
2. It compiles both `esp32_pressure_tap_node` and `esp32_ultrasonic_tank_node`.
3. If there are any syntax or compilation errors, GitHub notifies you immediately with exact line numbers.
4. If successful, it publishes the ready-to-flash binary files (`.bin`).

---

## 3. How to Update ESP32 Wirelessly in 1-Click (No USB Ever Needed)

Once your ESP32 is powered on and connected to your Wi-Fi (`JioFiber-Technokind`):

### Option A: From Web Browser (Web OTA Upload)
Both ESP32s run embedded web dashboards on your local network. You can update firmware through their browser interface without Arduino IDE or PlatformIO:
1. Download the latest `pressure_node_firmware.bin` from the GitHub Actions tab.
2. Open `http://esp32-tap-pressure.local/` (or its IP address).
3. Navigate to the firmware update upload field, select the `.bin` file, and click **Update**.
4. The ESP32 flashes in ~10 seconds and reboots automatically!

---

### Option B: Direct Wi-Fi OTA from Terminal (`espota.py`)
Run this single command from your terminal over Wi-Fi:
```powershell
# For Node 1 (Tap Pressure):
python C:\Users\subhr\.platformio\packages\framework-arduinoespressif32\tools\espota.py -i esp32-tap-pressure.local -p 3232 -a admin123 -f pressure_node_firmware.bin

# For Node 2 (Overhead Ultrasonic):
python C:\Users\subhr\.platformio\packages\framework-arduinoespressif32\tools\espota.py -i esp32-tank-ultrasonic.local -p 3232 -a admin123 -f ultrasonic_node_firmware.bin
```

---

## 4. Daily Workflow Summary

```powershell
# 1. Check status of local changes
git status

# 2. Add changes
git add .

# 3. Commit with a message
git commit -m "Calibrated full mark and tuned filter"

# 4. Push to cloud
git push
```
GitHub compiles the code automatically in the cloud!
