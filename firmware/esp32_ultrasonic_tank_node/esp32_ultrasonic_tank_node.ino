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
#include <time.h>
#include <ArduinoOTA.h>
#include "config.h"

// =========================================================================
// GLOBAL VARIABLES
// =========================================================================
float tankDepthCm      = DEFAULT_TANK_DEPTH_CM;
float sensorOffsetCm    = DEFAULT_SENSOR_OFFSET_CM;

float measuredDistanceCm = 0.0f;
float waterHeightCm      = 0.0f;
float waterLevelPct      = 0.0f;

unsigned long lastSampleTime    = 0;
unsigned long lastLogSlotMinute = 999;

// Persistent Offline Queue in Flash SPIFFS
const char* OFFLINE_QUEUE_FILE = "/offline_ultrasonic.csv";

// =========================================================================
// FUNCTION DECLARATIONS
// =========================================================================
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
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n==========================================================="));
  Serial.println(F(" Smart Tank: Ultrasonic Ground Truth Node (ESP32 #2)"));
  Serial.println(F("==========================================================="));

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

  Serial.println(F("[SYSTEM] Ultrasonic Ground Truth Node initialized."));
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  // Handle Over-The-Air Remote Code Upload requests
  ArduinoOTA.handle();

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
// ULTRASONIC MEASUREMENT WITH MULTI-SAMPLE MEDIAN FILTER
// =========================================================================
float measureDistanceSingle() {
  // Send 10 microsecond pulse to TRIG pin
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Measure ECHO pulse duration in microseconds (timeout 30 ms ~ 5 meters max)
  long duration = pulseIn(PIN_ECHO, HIGH, 30000);

  if (duration == 0) {
    return -1.0f; // Timeout or sensor reading failed
  }

  // Speed of sound in air ~= 343 m/s = 0.0343 cm/us
  // Distance = (Duration * 0.0343) / 2
  float dist = (duration * 0.0343f) / 2.0f;
  return dist;
}

float measureDistanceMedian() {
  float readings[ULTRASONIC_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
    float d = measureDistanceSingle();
    if (d > 2.0f && d < 450.0f) { // Valid range check
      readings[validCount++] = d;
    }
    delay(30);
  }

  if (validCount == 0) {
    return -1.0f;
  }

  // Sort array (Bubble sort for small array)
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = 0; j < validCount - i - 1; j++) {
      if (readings[j] > readings[j + 1]) {
        float tmp = readings[j];
        readings[j] = readings[j + 1];
        readings[j + 1] = tmp;
      }
    }
  }

  // Return median value to reject outlier ripples/splashes
  return readings[validCount / 2];
}

void calculateGroundTruth() {
  float dist = measureDistanceMedian();
  if (dist > 0.0f) {
    measuredDistanceCm = dist;

    // Water level height calculation:
    // When tank is 100% full, distance to water = sensorOffsetCm
    // Effective water height = tankDepthCm - (measuredDistanceCm - sensorOffsetCm)
    float rawHeight = tankDepthCm - (measuredDistanceCm - sensorOffsetCm);
    waterHeightCm = constrain(rawHeight, 0.0f, tankDepthCm);

    // Percentage
    waterLevelPct = (waterHeightCm / tankDepthCm) * 100.0f;

    Serial.printf("[ULTRASONIC] Dist: %.1f cm | Height: %.1f cm | Ground Truth Level: %.1f%%\n",
                  measuredDistanceCm, waterHeightCm, waterLevelPct);
  } else {
    Serial.println(F("[ULTRASONIC] Sensor read timeout/echo missed!"));
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
  http.setTimeout(25000); // 25s timeout for batch

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
  if (!f || f.size() == 0) {
    if (f) f.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    return;
  }

  Serial.printf("\n[OFFLINE] Stored offline logs found (%u bytes). Starting bulk upload to Google Sheets...\n", (unsigned int)f.size());

  const int BATCH_SIZE = 25;
  int totalSynced = 0;
  bool failed = false;

  while (f.available() && !failed) {
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
      int c2 = l.indexOf(',', c1 + 1);
      int c3 = l.indexOf(',', c2 + 1);

      if (c1 > 0 && c2 > 0) {
        String ts   = l.substring(0, c1);
        String dist = l.substring(c1 + 1, c2);
        String lvl  = (c3 > 0) ? l.substring(c2 + 1, c3) : l.substring(c2 + 1);
        String dpt  = (c3 > 0) ? l.substring(c3 + 1) : String(DEFAULT_TANK_DEPTH_CM, 1);

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
        totalSynced += validRecords;
        Serial.printf("[OFFLINE] Synced batch of %d records (Total: %d)...\n", validRecords, totalSynced);
      } else {
        Serial.println(F("[OFFLINE] Batch upload failed! Halting drain for retry."));
        failed = true;
        break;
      }
    }
    delay(200);
  }

  if (!failed && !f.available()) {
    f.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    Serial.printf("[OFFLINE] SUCCESS: All %d offline records synchronized! Queue cleared.\n\n", totalSynced);
  } else {
    String rem = "";
    while (f.available()) {
      rem += f.readStringUntil('\n') + "\n";
    }
    f.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    if (rem.length() > 0) {
      File rf = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_WRITE);
      if (rf) {
        rf.print(rem);
        rf.close();
      }
    }
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
      uploadBufferedOfflineLogs(); // Immediately drain offline backlog!
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
