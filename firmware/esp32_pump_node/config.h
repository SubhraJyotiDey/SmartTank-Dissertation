#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =========================================================================
// 1. PIN DEFINITIONS & RELAY HARDWARE MAPPING
// =========================================================================
// Safe boot GPIOs on ESP32: GPIO 16, 4, 17, 18, 19
#define PIN_RELAY               16      // GPIO connected to D965 Base (via 1k resistor)
#define RELAY_ACTIVE_LEVEL      HIGH    // HIGH for NPN Transistor (D965) driving JQX-15F (T90) relay

#define PIN_STATUS_LED          2       // Built-in status LED (GPIO 2)

// =========================================================================
// 2. WI-FI & OVER-THE-AIR (OTA) SETTINGS
// =========================================================================
#define DEFAULT_WIFI_SSID       "JioFiber-Technokind"
#define DEFAULT_WIFI_PASS       "05032006"

#define GOOGLE_SCRIPT_URL       "https://script.google.com/macros/s/AKfycbx7BzoSWxozJTqKUzqhG7wfK0P1MwTNogPdGqFiA2-jvY_G2s5sMhVWGPw2E-p2y0yU/exec"

#define OTA_HOSTNAME            "esp32-pump-node"
#define OTA_PASSWORD            "admin123"
#define OTA_PORT                3232

// =========================================================================
// 3. SAFETY WATCHDOG & MOTOR TIMEOUTS
// =========================================================================
// Fail-safe: If Node 1 stops sending heartbeats during pumping, cut relay after 10s
#define WATCHDOG_TIMEOUT_MS     10000UL // 10 seconds

// Absolute safety cutoff: Max allowable continuous runtime (protects against empty sump / pipe burst)
#define MAX_RUN_TIME_MS         2100000UL // 35 minutes

// Minimum cooldown before pump can be started again (protects motor windings)
// Set to 5s for rapid bench testing (Change to 300000UL / 5 min for permanent production deployment)
#define MIN_COOLDOWN_MS         5000UL

#define FIRMWARE_VERSION        "1.0.0"

#endif
