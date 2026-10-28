#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =========================================================================
// 1. PIN DEFINITIONS & ULTRASONIC SENSOR
// =========================================================================
// Supports HC-SR04 or waterproof JSN-SR04T (ideal for water tanks)
#define PIN_TRIG                5    // Trigger Output pin
#define PIN_ECHO                18   // Echo Input pin
#define PIN_STATUS_LED          2    // On-board LED

// =========================================================================
// 2. TANK GEOMETRY & SENSOR MOUNTING
// =========================================================================
// Total height of water column when tank is 100% full (122 cm)
#define DEFAULT_TANK_DEPTH_CM   122.0f

// Mounting offset: Distance from sensor face down to the 100% full water mark (cm)
// Calibrated: 11.0 cm measured from sensor face to water surface when tank is 100% full
#define DEFAULT_SENSOR_OFFSET_CM 11.0f

// Filter samples per measurement
#define ULTRASONIC_SAMPLES      7

// =========================================================================
// 3. WI-FI & NETWORK SETTINGS
// =========================================================================
#define DEFAULT_WIFI_SSID       "JioFiber-Technokind"
#define DEFAULT_WIFI_PASS       "05032006"

// OTA (Over-The-Air) Remote Code Upload Settings
#define OTA_HOSTNAME            "esp32-tank-ultrasonic"
#define OTA_PASSWORD            "admin123"
#define OTA_PORT                3232

// =========================================================================
// 4. SYNCHRONIZED GOOGLE SHEETS & NTP CONFIGURATION
// =========================================================================
// Paste the exact same Google Apps Script Web App Deployment URL here:
#define GOOGLE_SCRIPT_URL       "https://script.google.com/macros/s/AKfycbx7BzoSWxozJTqKUzqhG7wfK0P1MwTNogPdGqFiA2-jvY_G2s5sMhVWGPw2E-p2y0yU/exec"

// Synchronization interval: 1 minute for 24-hr dissertation monitoring experiment
#define LOG_INTERVAL_MINUTES    1

// NTP Server settings for clock synchronization
#define NTP_SERVER              "pool.ntp.org"
#define GMT_OFFSET_SEC          19800   // IST (India Standard Time) UTC+5:30 -> 19800 sec
#define DAYLIGHT_OFFSET_SEC     0
// =========================================================================
// 5. AUTOMATIC CLOUD OVER-THE-AIR (OTA) SELF-UPDATE (EVERY 15 MINUTES)
// =========================================================================
#define FIRMWARE_VERSION            "1.2.0"
#define AUTO_OTA_ENABLED            true
#define OTA_CHECK_INTERVAL_MIN      15      // Check for new updates every 15 minutes
#define OTA_NODE_NAME               "ultrasonic_node"
#define OTA_BIN_FILENAME            "ultrasonic_node_firmware.bin"

// GitHub Cloud Repository for Automatic Firmware Releases:
#define GITHUB_OWNER                "SubhraJyotiDey"
#define GITHUB_REPO                 "SmartTank-Dissertation"
#define GITHUB_RELEASE_TAG          "latest"

// Optional GitHub Personal Access Token (PAT) for private repo access:
#define DEFAULT_GITHUB_OTA_TOKEN    "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"

#endif // CONFIG_H
