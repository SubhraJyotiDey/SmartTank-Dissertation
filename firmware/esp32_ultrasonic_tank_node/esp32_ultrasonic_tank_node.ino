/**
 * =========================================================================
 * Overhead Tank Ultrasonic Ground Truth Sensor Node (ESP32 #2)
 * Smart Water Level Verification & Synchronized Google Sheets Logging
 * =========================================================================
 * 
 * Hardware:
 *   - ESP32 Dev Module
 *   - Ultrasonic Sensor (HC-SR04 or waterproof JSN-SR04T)
 *     * TRIG -> GPIO 5
 *     * ECHO -> GPIO 18 (use 1k/2k voltage divider if 5V sensor output)
 * 
 * Purpose:
 *   - Mounted at the top of the overhead tank facing down at water surface.
 *   - Measures true physical water surface level (Ground Reality benchmark).
 *   - Synchronized NTP timekeeping: Logs ground truth values in fixed 10-minute
 *     intervals simultaneously with Node 1 to the exact same Google Sheet row.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include "config.h"
#include "cloud_ota_updater.h"

// WebServer for local high-speed telemetry query by Pressure Node
WebServer server(80);

// NVS Persistent Storage
Preferences preferences;

// Automatic Cloud OTA Self-Updater (every 15 minutes)
CloudOTAUpdater cloudOTA(FIRMWARE_VERSION, OTA_BIN_FILENAME, DEFAULT_GITHUB_OTA_TOKEN);

// =========================================================================
// GLOBAL VARIABLES
// =========================================================================
float tankDepthCm        = DEFAULT_TANK_DEPTH_CM;
float sensorOffsetCm     = DEFAULT_SENSOR_OFFSET_CM; // Air gap distance at 100% full (Tank Full Scenario)

float measuredDistanceCm = 0.0f;
float smoothedDistanceCm = -1.0f;   // Exponential Moving Average filtered distance
float lastValidCandidate = -1.0f;   // Candidate for large jump validation
int   consecutiveMisses  = 0;       // Counter for persistent echo timeout detection
float waterHeightCm      = 0.0f;
float waterLevelPct      = 0.0f;

unsigned long lastSampleTime    = 0;
unsigned long lastLogSlotMinute = 999;

// Persistent Offline Queue in Flash SPIFFS
const char* OFFLINE_QUEUE_FILE = "/offline_ultrasonic.csv";

// =========================================================================
// FUNCTION DECLARATIONS
// =========================================================================
void loadSettings();
void saveSettings();
void initWiFi();
void maintainWiFi();
void initOTA();
void initNTP();
bool getResilientTime(struct tm* timeinfo);
float measureDistanceSingle();
float measureDistanceMedian();
void calculateGroundTruth();
void handleNtpAndLogging();
bool sendToGoogleSheets(const String& timestampStr);
void saveOfflineRecord(const String& timestampStr, float dist, float lvl, float depth);
void uploadBufferedOfflineLogs();
bool postBulkJsonToGoogleSheets(const String& jsonPayload);
int getOfflineQueueCount();

// =========================================================================
// SETTINGS PERSISTENCE (NVS)
// =========================================================================
void loadSettings() {
  preferences.begin("smart_tank_us", true); // Read-only
  sensorOffsetCm = preferences.getFloat("sensor_offset", DEFAULT_SENSOR_OFFSET_CM);
  if (sensorOffsetCm >= 10.5f && sensorOffsetCm <= 11.5f) {
    sensorOffsetCm = DEFAULT_SENSOR_OFFSET_CM; // Update legacy 11cm default to 6cm
  }
  tankDepthCm    = preferences.getFloat("tank_depth", DEFAULT_TANK_DEPTH_CM);
  preferences.end();
  Serial.printf("[NVS] Loaded settings: Tank Full Dist (Offset) = %.1f cm | Tank Depth = %.1f cm\n",
                sensorOffsetCm, tankDepthCm);
}

void saveSettings() {
  preferences.begin("smart_tank_us", false); // Read-write
  preferences.putFloat("sensor_offset", sensorOffsetCm);
  preferences.putFloat("tank_depth", tankDepthCm);
  preferences.end();
  Serial.printf("[NVS] Settings saved: Tank Full Dist (Offset) = %.1f cm | Tank Depth = %.1f cm\n",
                sensorOffsetCm, tankDepthCm);
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n==========================================================="));
  Serial.println(F(" Smart Tank: Ultrasonic Ground Truth Node (ESP32 #2)"));
  Serial.println(F("==========================================================="));

  // Load persistent calibration and geometry parameters from Flash NVS
  loadSettings();

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Mount SPIFFS for power-fail-safe offline queue storage
  if (!SPIFFS.begin(true)) {
    Serial.println(F("[SPIFFS] Flash filesystem mount failed!"));
  } else {
    Serial.printf("[SPIFFS] Flash filesystem mounted! Queued offline logs: %d\n", getOfflineQueueCount());
  }

  initWiFi();
  initOTA();
  initNTP();

  // Local API for Node 1 to fetch live ultrasonic ground truth during high-res events
  server.on("/api/reading", HTTP_GET, []() {
    String json = "{";
    json += "\"distance_cm\":" + String(measuredDistanceCm, 1) + ",";
    json += "\"water_height_cm\":" + String(waterHeightCm, 1) + ",";
    json += "\"level_pct\":" + String(waterLevelPct, 1) + ",";
    json += "\"offset_cm\":" + String(sensorOffsetCm, 1) + ",";
    json += "\"us_full_dist\":" + String(sensorOffsetCm, 1) + ",";
    json += "\"depth_cm\":" + String(tankDepthCm, 1);
    json += "}";
    server.send(200, "application/json", json);
  });

  // Settings configuration endpoint (allows setting tank full distance / offset and tank depth)
  server.on("/api/settings", HTTP_ANY, []() {
    bool changed = false;
    if (server.hasArg("offset_cm")) {
      sensorOffsetCm = server.arg("offset_cm").toFloat();
      changed = true;
    } else if (server.hasArg("us_full_dist")) {
      sensorOffsetCm = server.arg("us_full_dist").toFloat();
      changed = true;
    }
    if (server.hasArg("depth_cm")) {
      tankDepthCm = server.arg("depth_cm").toFloat();
      changed = true;
    }
    if (changed) {
      saveSettings();
      calculateGroundTruth();
    }
    String json = "{\"status\":\"ok\",\"offset_cm\":" + String(sensorOffsetCm, 1) +
                  ",\"us_full_dist\":" + String(sensorOffsetCm, 1) +
                  ",\"depth_cm\":" + String(tankDepthCm, 1) + "}";
    server.send(200, "application/json", json);
  });

  // Remote reboot endpoint
  server.on("/api/reboot", HTTP_ANY, []() {
    server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Rebooting Node 2...\"}");
    delay(500);
    ESP.restart();
  });

  // Remote Immediate Cloud OTA Update endpoint
  server.on("/api/ota_update", HTTP_ANY, []() {
    Serial.println(F("[HTTP] ⚡ Immediate Cloud OTA Update triggered on Node 2 via Web/API"));
    server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Starting Immediate Cloud OTA on Node 2...\"}");
    delay(250);
    cloudOTA.checkAndUpdate(true); // Force download & flash from GitHub
  });

  server.begin();
  Serial.println(F("[HTTP] Local telemetry API started on port 80"));

  Serial.println(F("[SYSTEM] Ultrasonic Ground Truth Node initialized."));
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  server.handleClient();

  // Handle Over-The-Air Remote Code Upload requests
  ArduinoOTA.handle();

  // Handle Automatic Cloud OTA updates (checks every 15 minutes)
  cloudOTA.handleLoop();

  // Continuous auto-reconnect and offline backlog drainer
  maintainWiFi();

  unsigned long currentMillis = millis();

  // Measure ground truth water level every 2 seconds
  if (currentMillis - lastSampleTime >= 2000) {
    lastSampleTime = currentMillis;
    calculateGroundTruth();
  }

  // Check NTP clock and execute synchronized 1-minute Google Sheets log
  handleNtpAndLogging();
}

// =========================================================================
// ULTRASONIC MEASUREMENT WITH MULTI-SAMPLE MEDIAN & TEMPORAL EMA FILTER
// =========================================================================
float measureDistanceSingle() {
  // 1. Wait up to 1000 us for ECHO line to be LOW if it was still active/ringing
  unsigned long tWait = micros();
  while (digitalRead(PIN_ECHO) == HIGH && (micros() - tWait) < 1000) {
    delayMicroseconds(10);
  }

  // 2. Ensure TRIG line is clean and LOW
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);

  // 3. Send 20 microsecond pulse to TRIG pin (required for full transducer chirp energy on JSN-SR04T / AJ-SR04M)
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(20);
  digitalWrite(PIN_TRIG, LOW);

  // 4. Measure ECHO pulse duration in microseconds (25 ms timeout ~ 4.3 meters max)
  long duration = pulseIn(PIN_ECHO, HIGH, 25000);

  if (duration <= 0) {
    return -1.0f; // Timeout or missed echo
  }

  // Speed of sound in air ~= 343 m/s = 0.0343 cm/us
  // Distance = (Duration * 0.0343) / 2
  float dist = (duration * 0.0343f) / 2.0f;
  return dist;
}

float measureDistanceMedian() {
  const int SAMPLES = 9;
  float readings[SAMPLES];
  int validCount = 0;

  for (int i = 0; i < SAMPLES; i++) {
    float d = measureDistanceSingle();
    // Valid physical range check (exclude near-field blind-zone ringing < 8cm and out-of-tank > 350cm)
    if (d >= 8.0f && d <= 350.0f) {
      readings[validCount++] = d;
    }
    // 50 ms delay between pings: allows acoustic reverberations in tank to decay
    // Also service HTTP client so local queries from Node 1 never block or time out
    unsigned long dStart = millis();
    while (millis() - dStart < 50) {
      server.handleClient();
      delay(2);
    }
  }

  // Quorum requirement: Require at least 3 valid readings out of 9
  // (Prevents a single random noise spike from being accepted when echoes are weak)
  if (validCount < 3) {
    return -1.0f;
  }

  // Sort array (Bubble sort)
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = 0; j < validCount - i - 1; j++) {
      if (readings[j] > readings[j + 1]) {
        float tmp = readings[j];
        readings[j] = readings[j + 1];
        readings[j + 1] = tmp;
      }
    }
  }

  // Trimmed median: return the true center reading of the sorted cluster
  return readings[validCount / 2];
}

void calculateGroundTruth() {
  float rawDist = measureDistanceMedian();

  if (rawDist > 0.0f) {
    consecutiveMisses = 0;

    // Outlier jump protection:
    // Water level cannot physically move > 15 cm in 2 seconds.
    // If a sudden jump occurs, require 2 consecutive cycles to confirm before accepting
    if (smoothedDistanceCm > 0.0f && fabs(rawDist - smoothedDistanceCm) > 15.0f) {
      if (lastValidCandidate > 0.0f && fabs(rawDist - lastValidCandidate) < 5.0f) {
        // Confirmed second reading: accept new level step (e.g. rapid tank filling)
        smoothedDistanceCm = rawDist;
        lastValidCandidate = -1.0f;
      } else {
        // Suspected ripple / transient reflection: hold candidate for next cycle
        lastValidCandidate = rawDist;
        rawDist = smoothedDistanceCm; // Suppress spike for this cycle
      }
    } else {
      lastValidCandidate = -1.0f;
    }

    // Exponential Moving Average (EMA) Temporal Smoothing (alpha = 0.30):
    // Drastically suppresses acoustic jitter and surface ripple fluctuations
    if (smoothedDistanceCm <= 0.0f) {
      smoothedDistanceCm = rawDist;
    } else {
      smoothedDistanceCm = (0.30f * rawDist) + (0.70f * smoothedDistanceCm);
    }

    measuredDistanceCm = smoothedDistanceCm;

    // Water level height calculation:
    // When tank is 100% full, distance to water = sensorOffsetCm
    // Effective water height = tankDepthCm - (measuredDistanceCm - sensorOffsetCm)
    float rawHeight = tankDepthCm - (measuredDistanceCm - sensorOffsetCm);
    waterHeightCm = constrain(rawHeight, 0.0f, tankDepthCm);

    // Percentage
    waterLevelPct = (waterHeightCm / tankDepthCm) * 100.0f;

    Serial.printf("[ULTRASONIC] Raw: %.1f cm | Smooth Dist: %.1f cm | Height: %.1f cm | Level: %.1f%%\n",
                  rawDist, measuredDistanceCm, waterHeightCm, waterLevelPct);
  } else {
    consecutiveMisses++;
    Serial.printf("[ULTRASONIC] Sensor read timeout/echo missed! (Consecutive misses: %d)\n", consecutiveMisses);

    // If echo is lost for > 6 consecutive cycles (12 seconds), flag as offline / out of range
    // rather than staying silently frozen on a stale value
    if (consecutiveMisses >= 6) {
      waterLevelPct = -1.0f;
    }
  }
}

// =========================================================================
// RESILIENT TIMEKEEPING (RTC + NTP SYNCHRONIZATION)
// =========================================================================
bool getResilientTime(struct tm* timeinfo) {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) { // Uninitialized clock
    if (WiFi.status() == WL_CONNECTED) {
      struct tm dummy;
      if (getLocalTime(&dummy, 2000)) {
        nowSec = time(nullptr);
      } else {
        return false;
      }
    } else {
      return false; // Still waiting for initial clock synchronization
    }
  }
  localtime_r(&nowSec, timeinfo);
  return true;
}

// =========================================================================
// OFFLINE STORAGE & QUEUE MANAGEMENT IN FLASH (SPIFFS)
// =========================================================================
int getOfflineQueueCount() {
  if (!SPIFFS.exists(OFFLINE_QUEUE_FILE)) return 0;
  File f = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    if (f.read() == '\n') count++;
  }
  f.close();
  return count;
}

void saveOfflineRecord(const String& timestampStr, float dist, float lvl, float depth) {
  File f = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_APPEND);
  if (f) {
    f.printf("%s,%.1f,%.1f,%.1f\n", timestampStr.c_str(), dist, lvl, depth);
    f.close();
    Serial.printf("[OFFLINE] Stored Ground Truth in Flash: %s | Dist=%.1f cm, Level=%.1f%% (Queue count: %d)\n",
                  timestampStr.c_str(), dist, lvl, getOfflineQueueCount());
  } else {
    Serial.println(F("[OFFLINE] ERROR: Failed to open SPIFFS queue file for writing!"));
  }
}

// =========================================================================
// BULK HTTPS UPLOAD TO GOOGLE SHEETS
// =========================================================================
bool postBulkJsonToGoogleSheets(const String& jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(35000); // 35s timeout for batch bulk ingestion

  bool success = false;
  if (http.begin(client, GOOGLE_SCRIPT_URL)) {
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(jsonPayload);
    Serial.printf("[SHEETS-BULK] HTTP POST Response Code: %d\n", httpCode);

    if (httpCode > 0) {
      String response = http.getString();
      Serial.printf("[SHEETS-BULK] Server Response: %s\n", response.c_str());
      success = (httpCode == 200 || httpCode == 302);
    } else {
      Serial.printf("[SHEETS-BULK] Transmission failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
  client.stop(); // Free SSL buffer
  return success;
}

void uploadBufferedOfflineLogs() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!SPIFFS.exists(OFFLINE_QUEUE_FILE)) return;

  File f = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
  if (!f) return;
  if (f.size() == 0) {
    f.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    return;
  }

  // Count total records in the queue file
  int totalLines = 0;
  while (f.available()) {
    if (f.read() == '\n') totalLines++;
  }
  f.seek(0); // Rewind to start of file

  if (totalLines == 0) {
    f.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    return;
  }

  Serial.printf("\n[OFFLINE] Stored ground truth logs found: %d records (%u bytes). Starting bulk upload to Google Sheets...\n",
                totalLines, (unsigned int)f.size());

  const int BATCH_SIZE = 10; // 10 records per batch prevents Google Sheets execution timeouts
  int syncedLineCount = 0;
  bool uploadFailed = false;

  while (f.available() && !uploadFailed) {
    String lines[BATCH_SIZE];
    int count = 0;

    while (f.available() && count < BATCH_SIZE) {
      String l = f.readStringUntil('\n');
      l.trim();
      if (l.length() > 0) {
        lines[count++] = l;
      }
    }

    if (count == 0) break;

    String json = "{\"node\":\"ultrasonic\",\"bulk\":[";
    int validRecords = 0;
    for (int i = 0; i < count; i++) {
      String l = lines[i];
      // Format: timestamp,distance_cm,level_pct,tank_depth_cm
      int c1 = l.indexOf(',');
      int c2 = (c1 > 0) ? l.indexOf(',', c1 + 1) : -1;
      int c3 = (c2 > 0) ? l.indexOf(',', c2 + 1) : -1;

      if (c1 > 0 && c2 > 0) {
        String ts   = l.substring(0, c1);
        String dist = l.substring(c1 + 1, c2);
        String lvl  = (c3 > 0) ? l.substring(c2 + 1, c3) : l.substring(c2 + 1);
        String dpt  = (c3 > 0) ? l.substring(c3 + 1) : String(DEFAULT_TANK_DEPTH_CM, 1);

        ts.trim(); dist.trim(); lvl.trim(); dpt.trim();
        if (dist.length() == 0) dist = "0.0";
        if (lvl.length() == 0) lvl = "0.0";
        if (dpt.length() == 0) dpt = String(DEFAULT_TANK_DEPTH_CM, 1);

        if (validRecords > 0) json += ",";
        json += "{\"timestamp\":\"" + ts + "\",\"distance_cm\":" + dist +
                ",\"level_pct\":" + lvl + ",\"tank_depth_cm\":" + dpt + "}";
        validRecords++;
      }
    }
    json += "]}";

    if (validRecords > 0) {
      bool ok = postBulkJsonToGoogleSheets(json);
      if (ok) {
        syncedLineCount += count;
        Serial.printf("[OFFLINE] Synced batch of %d records (Total: %d/%d)...\n", count, syncedLineCount, totalLines);
      } else {
        Serial.printf("[OFFLINE] Batch upload failed at record index %d! Halting drain without losing data.\n", syncedLineCount);
        uploadFailed = true;
        break;
      }
    } else {
      // Corrupt line encountered, advance past it
      syncedLineCount += count;
    }
    delay(200);
  }
  f.close();

  // Bulletproof queue maintenance: ZERO DATA LOSS
  if (!uploadFailed && syncedLineCount >= totalLines) {
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    Serial.printf("[OFFLINE] SUCCESS: All %d offline records synchronized! Queue cleared.\n\n", syncedLineCount);
  } else if (syncedLineCount > 0) {
    // Partial success: Only remove the lines that were confirmed uploaded!
    File rf = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
    if (rf) {
      int skipped = 0;
      while (rf.available() && skipped < syncedLineCount) {
        if (rf.read() == '\n') skipped++;
      }
      String remainingData = "";
      while (rf.available()) {
        remainingData += rf.readStringUntil('\n') + "\n";
      }
      rf.close();

      SPIFFS.remove(OFFLINE_QUEUE_FILE);
      if (remainingData.length() > 0) {
        File wf = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_WRITE);
        if (wf) {
          wf.print(remainingData);
          wf.close();
        }
      }
      Serial.printf("[OFFLINE] Retained %d unuploaded records in queue for next retry.\n", totalLines - syncedLineCount);
    }
  } else {
    // Zero records were uploaded: Leave the file 100% untouched!
    Serial.printf("[OFFLINE] Upload failed: All %d records safely preserved in Flash memory.\n", totalLines);
  }
}

// =========================================================================
// SYNCHRONIZED 1-MINUTE LOGGING ENGINE
// =========================================================================
void handleNtpAndLogging() {
  struct tm timeinfo;
  if (!getResilientTime(&timeinfo)) {
    return;
  }

  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;

  // Trigger at exact 1-minute slot boundary (:00, :01, :02, ...)
  if ((currentMinute % LOG_INTERVAL_MINUTES == 0) && (currentSecond <= 5)) {
    if (lastLogSlotMinute != currentMinute) {
      lastLogSlotMinute = currentMinute; // Lock to prevent duplicate fires

      char timeBuf[30];
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:00", &timeinfo);
      String timestampStr = String(timeBuf);

      Serial.printf("[SYNC] 1-Minute boundary reached: %s. Logging Ground Truth...\n", timeBuf);

      bool sent = false;
      if (WiFi.status() == WL_CONNECTED) {
        sent = sendToGoogleSheets(timestampStr);
      }

      if (!sent) {
        // Wi-Fi down or Google Sheets unreachable -> Persist to Flash memory!
        saveOfflineRecord(timestampStr, measuredDistanceCm, waterLevelPct, tankDepthCm);
      } else {
        // If live upload succeeded and there are older offline logs pending, drain them!
        uploadBufferedOfflineLogs();
      }
    }
  }
}

// =========================================================================
// HTTPS CLIENT FOR GOOGLE APPS SCRIPT
// =========================================================================
bool sendToGoogleSheets(const String& timestampStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[SHEETS] Wi-Fi not connected. Switching to offline Flash queue."));
    return false;
  }

  digitalWrite(PIN_STATUS_LED, HIGH);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  String encodedTimestamp = timestampStr;
  encodedTimestamp.replace(" ", "%20");

  String url = GOOGLE_SCRIPT_URL;
  url.trim();
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += "node=ultrasonic";
  url += "&timestamp=" + encodedTimestamp;
  url += "&distance_cm=" + String(measuredDistanceCm, 1);
  url += "&level_pct=" + String(waterLevelPct, 1);
  url += "&tank_depth_cm=" + String(tankDepthCm, 1);
  url += "&sensor_offset_cm=" + String(sensorOffsetCm, 1);

  Serial.printf("[SHEETS] Uploading Ground Truth to: %s\n", url.c_str());
  bool success = false;
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    Serial.printf("[SHEETS] HTTP Response Code: %d\n", httpCode);

    if (httpCode > 0) {
      String response = http.getString();
      Serial.printf("[SHEETS] Server Response: %s\n", response.c_str());
      success = (httpCode == 200 || httpCode == 302);
    } else {
      Serial.printf("[SHEETS] Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println(F("[SHEETS] Unable to connect to host"));
  }

  client.stop(); // Free SSL buffer
  digitalWrite(PIN_STATUS_LED, LOW);
  return success;
}

// =========================================================================
// ROBUST AUTO-RECONNECTING WI-FI MANAGER
// =========================================================================
void initWiFi() {
  Serial.printf("[WIFI] Initializing Station connection to: %s\n", DEFAULT_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true); // Low-level SDK auto-reconnect
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP Address: %s | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println(F("\n[WIFI] Initial connect timed out. Continuous background maintainWiFi() active."));
  }
}

void maintainWiFi() {
  static unsigned long lastCheck = 0;
  static unsigned long disconnectStart = 0;
  static bool wasConnected = false;
  unsigned long now = millis();

  if (now - lastCheck < 5000) return; // Check every 5 seconds
  lastCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      wasConnected = true;
      disconnectStart = 0;
      Serial.printf("\n[WIFI] Wi-Fi Restored! IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER); // Resync NTP
      delay(2000); // 2-second stabilization delay for DHCP & DNS resolver
      uploadBufferedOfflineLogs(); // Drain offline backlog!
    }
    return;
  }

  // Not connected
  if (wasConnected) {
    wasConnected = false;
    disconnectStart = now;
    Serial.println(F("\n[WIFI] WARNING: Wi-Fi dropped! Storing ground truth in Flash memory..."));
  }

  unsigned long offlineSec = (disconnectStart > 0) ? ((now - disconnectStart) / 1000) : 0;
  Serial.printf("[WIFI] Auto-reconnecting to '%s' (Offline: %lu s | Queued: %d records)...\n",
                DEFAULT_WIFI_SSID, offlineSec, getOfflineQueueCount());

  // If disconnected for > 3 minutes, perform a full hardware radio re-init
  if (offlineSec > 180) {
    Serial.println(F("[WIFI] Performing clean Wi-Fi radio reset..."));
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    disconnectStart = now;
  } else {
    WiFi.reconnect();
  }
}

void initOTA() {
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA
    .onStart([]() {
      Serial.println(F("[OTA] Remote code upload starting..."));
    })
    .onEnd([]() {
      Serial.println(F("\n[OTA] Remote code upload completed successfully!"));
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u]\n", error);
    });

  ArduinoOTA.begin();
  Serial.printf("[OTA] Remote Code Upload ready on hostname '%s', Port %d\n", OTA_HOSTNAME, OTA_PORT);
}

void initNTP() {
  Serial.println(F("[NTP] Initializing time synchronization..."));
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}
