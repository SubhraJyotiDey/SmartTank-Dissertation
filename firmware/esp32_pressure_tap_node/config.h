#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =========================================================================
// 1. PIN DEFINITIONS & HARDWARE MAPPING
// =========================================================================
// Analog pin connected to 0.2 MPa Pressure Sensor via Voltage Divider (Fallback Internal ADC)
// GPIO 34, 35, 36 (VP), or 39 (VN) are ideal ADC input-only pins on ESP32
#define PIN_PRESSURE_ADC        34 

// Built-in status LED pin (GPIO 2 on most ESP32 Dev Boards)
#define PIN_STATUS_LED          2

// =========================================================================
// 1B. PRECISION EXTERNAL ADC (ADS1115 16-BIT I2C) SETTINGS
// =========================================================================
// Set to true to enable high-resolution external 16-bit ADC (ADS1115)
// Firmware auto-detects ADS1115 at startup. If disconnected, automatically falls back to internal ADC (GPIO 34).
#define USE_EXTERNAL_ADS1115    true

// ESP32 Hardware I2C Pins for ADS1115:
#define PIN_ADS_SDA             21   // Default ESP32 I2C SDA
#define PIN_ADS_SCL             22   // Default ESP32 I2C SCL
#define ADS1115_I2C_ADDR        0x48 // I2C address (when ADDR pin is connected to GND)

// ADS1115 Channel connected to pressure sensor: 0 = A0, 1 = A1, 2 = A2, 3 = A3
#define ADS1115_PRESSURE_CH     0

// Wiring Mode for ADS1115:
// - If true (RECOMMENDED): ADS1115 VDD connected to 5V (VIN). Sensor Yellow wire connects DIRECTLY
//   to A0 (no voltage divider resistors needed!). PGA operates at +/- 6.144V with full 16-bit resolution.
// - If false: ADS1115 VDD connected to 3.3V. Sensor Yellow wire connects through voltage divider into A0.
#define ADS1115_DIRECT_5V       true

// =========================================================================
// 2. 0.2 MPa SENSOR SPECIFICATIONS & VOLTAGE DIVIDER
// =========================================================================
// Sensor: 0 to 0.2 MPa (0 to 200 kPa / ~20.4 meters H2O column)
// Supply Voltage: 5.0V (from VIN / 5V rail)
// Output Voltage: 0.5V @ 0 kPa  to  4.5V @ 200 kPa
#define SENSOR_MIN_V            0.50f   // Output voltage at 0 kPa
#define SENSOR_MAX_V            4.50f   // Output voltage at 200 kPa
#define SENSOR_MAX_KPA          200.0f  // 0.2 MPa = 200.0 kPa

// Voltage Divider Resistors (to scale 0-4.5V down to 0-1.05V for ESP32 ADC)
// Configured:  R1 = 33k Ohm (between sensor signal & GPIO34)
//              R2 = 10k Ohm (between GPIO34 & GND)
// Ratio = R2 / (R1 + R2) = 10 / 43 = 0.23256
#define VOLTAGE_DIVIDER_RATIO   (10.0f / (33.0f + 10.0f)) // ~0.23256

// ESP32 ADC Reference Voltage: With ADC_2_5db attenuation, input range is 0 - 1.50V
// Since our 33k/10k divider outputs 0 - 1.05V, ADC_2_5db provides 3x higher resolution!
#define ADC_REF_VOLTAGE         1.50f
#define ADC_RESOLUTION          4095.0f

// =========================================================================
// 3. DEFAULT CALIBRATION & TANK GEOMETRY (SAVED IN NVS/PREFERENCES)
// =========================================================================
// Default pressures (will be overridden when you calibrate via Web Server)
#define DEFAULT_P_EMPTY_KPA     25.0f   // Elevation head pressure when tank is empty
#define DEFAULT_P_FULL_KPA      40.0f   // Elevation head + full tank
#define DEFAULT_TANK_DEPTH_CM   122.0f  // Height of water in tank when 100% full (122 cm)
#define DEFAULT_TANK_CAPACITY_L 2000.0f // Overhead tank volume (Liters)

// =========================================================================
// 4. TAP OPEN / PRESSURE DROP COMPENSATION CONSTANTS
// =========================================================================
// Drop rate threshold to detect tap opening: (kPa per second)
#define TAP_DROP_THRESHOLD_KPA_S   1.5f 
#define TAP_RECOVER_THRESHOLD_KPA_S 1.2f

// Water Hammer / Settling delay in milliseconds after tap closure
#define SETTLING_DELAY_MS          3000

// ADC Oversampling count with Sort-and-Trim (trimmed mean) for noise rejection
#define ADC_OVERSAMPLE_COUNT       60

// Exponential Moving Average (EMA) smoothing factor (0.05 = super smooth)
#define PRESSURE_EMA_ALPHA         0.05f

// =========================================================================
// 5. WI-FI & NETWORK SETTINGS
// =========================================================================
#define DEFAULT_WIFI_SSID       "JioFiber-Technokind"
#define DEFAULT_WIFI_PASS       "05032006"

// Fallback SoftAP mode if Wi-Fi connection fails
#define AP_SSID                 "SmartTank_Pressure_AP"
#define AP_PASS                 "12345678"

// OTA (Over-The-Air) Remote Code Upload Settings
#define OTA_HOSTNAME            "esp32-tap-pressure"
#define OTA_PASSWORD            "admin123"
#define OTA_PORT                3232

// Web Server Port
#define WEB_SERVER_PORT         80

// =========================================================================
// 6. SYNCHRONIZED GOOGLE SHEETS & NTP CONFIGURATION
// =========================================================================
// Paste your Google Apps Script Web App Deployment URL here:
#define GOOGLE_SCRIPT_URL       "https://script.google.com/macros/s/AKfycbx7BzoSWxozJTqKUzqhG7wfK0P1MwTNogPdGqFiA2-jvY_G2s5sMhVWGPw2E-p2y0yU/exec"

// Synchronization interval: 1 minute for 24-hr dissertation monitoring experiment
#define LOG_INTERVAL_MINUTES    1

// NTP Server settings for precise clock synchronization
#define NTP_SERVER              "pool.ntp.org"
#define GMT_OFFSET_SEC          19800   // IST (India Standard Time) UTC+5:30 -> 5.5 * 3600 = 19800 sec
#define DAYLIGHT_OFFSET_SEC     0       // No daylight savings in IST (adjust if needed)

// =========================================================================
// 7. REAL-TIME 10 HZ MQTT STREAMING CONFIGURATION
// =========================================================================
// Free Global MQTT Broker (Zero account required, works with WebSockets)
#define MQTT_BROKER             "broker.hivemq.com"
#define MQTT_PORT               1883
// Unique topic for your tank setup
#define MQTT_TOPIC_TELEMETRY    "smarttank/dissertation/tap/live"
// =========================================================================
// 8. AUTOMATIC CLOUD OVER-THE-AIR (OTA) SELF-UPDATE (EVERY 15 MINUTES)
// =========================================================================
#define FIRMWARE_VERSION            "1.2.0"
#define AUTO_OTA_ENABLED            true
#define OTA_CHECK_INTERVAL_MIN      15      // Check for new updates every 15 minutes
#define OTA_NODE_NAME               "pressure_node"
#define OTA_BIN_FILENAME            "pressure_node_firmware.bin"

// GitHub Cloud Repository for Automatic Firmware Releases:
#define GITHUB_OWNER                "SubhraJyotiDey"
#define GITHUB_REPO                 "SmartTank-Dissertation"
#define GITHUB_RELEASE_TAG          "latest"

// Optional GitHub Personal Access Token (PAT) for private repo access.
// If empty, public release URLs are queried. Can also be set in Web Dashboard.
#define DEFAULT_GITHUB_OTA_TOKEN    "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"

#endif // CONFIG_H
