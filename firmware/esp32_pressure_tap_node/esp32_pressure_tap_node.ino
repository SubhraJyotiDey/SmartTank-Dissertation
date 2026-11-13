/**
 * =========================================================================
 * Non-Invasive Inline Tap Pressure Sensor Node (ESP32)
 * Smart Overhead Tank Water Level Monitoring & Dynamic Tap Compensation
 * =========================================================================
 * 
 * Hardware:
 *   - ESP32 Dev Module
 *   - 0.2 MPa Piezoresistive Pressure Sensor (0.5V - 4.5V output)
 *   - Precision Voltage Divider (R1 = 10k, R2 = 20k) on GPIO 34
 * 
 * Features:
 *   - Two-point Empty & Full state calibration saved in NVS Preferences
 *   - Dynamic Tap Open / Flow Pressure Drop Compensation & Water Hammer Rejection
 *   - Built-in Modern Responsive Web Dashboard (Glassmorphism & Live Chart)
 *   - NTP Clock Timekeeping
 *   - Synchronized 10-minute interval logging to Google Sheets (HTTPS with 302 redirect follow)
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <time.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "cluster_protocol.h"
#include "ads1115_driver.h"
#include "web_dashboard.h"
#include "cloud_ota_updater.h"

// Wi-Fi Multi Client (Auto-selects strongest signal between Primary & Proxy Extender AP)
WiFiMulti wifiMulti;

// External ADS1115 16-Bit Precision ADC Object
ADS1115_Lite ads(ADS1115_I2C_ADDR);
bool usingExternalAdc = false;

// Automatic Cloud OTA Self-Updater (every 15 minutes)
CloudOTAUpdater cloudOTA(FIRMWARE_VERSION, OTA_BIN_FILENAME, DEFAULT_GITHUB_OTA_TOKEN);

#include "highres_event_recorder.h"
// High-Resolution 10 Hz Event Recorder (Buffers in SPIFFS & Uploads in Chunks to Sheet 2)
HighResEventRecorder eventRecorder;

// =========================================================================
// ENUMS & STATE STRUCTURES
// =========================================================================
enum TapFlowState {
  STATE_STATIC = 0,    // Normal hydrostatic equilibrium
  STATE_TAP_OPEN = 1,  // Dynamic flow active (level reading frozen at pre-flow baseline)
  STATE_SETTLING = 2   // Tap closed, water hammer damping period active
};

// =========================================================================
// GLOBAL OBJECTS & VARIABLES
// =========================================================================
WebServer server(WEB_SERVER_PORT);
Preferences preferences;

WiFiClient espMqttClient;
PubSubClient mqttClient(espMqttClient);
unsigned long lastMqttPublishTime = 0;

// Offline Persistent Storage in Flash SPIFFS
const char* OFFLINE_QUEUE_FILE = "/offline_pressure.csv";

// Persistent Settings
float pEmptyKPa       = DEFAULT_P_EMPTY_KPA;
float pFullKPa        = DEFAULT_P_FULL_KPA;
float tankDepthCm     = DEFAULT_TANK_DEPTH_CM;
float tankCapacityL   = DEFAULT_TANK_CAPACITY_L;
String googleScriptUrl = GOOGLE_SCRIPT_URL;

// Live Telemetry
float rawAdcAvg        = 0.0f;
float sensorVoltage    = 0.0f;
float currentPressure  = 0.0f;
float filteredPressure = 0.0f;
float lastStaticPressure = 0.0f;
float prevPressure     = 0.0f;
float dPdt             = 0.0f;

float waterLevelPct    = 0.0f;
float frozenLevelPct   = 0.0f;
float waterVolumeL     = 0.0f;
float waterHeadM       = 0.0f;
float flowRateLps      = 0.0f; // Instantaneous tap discharge flow rate (L/s)

TapFlowState tapState  = STATE_STATIC;
unsigned long stateChangeTime = 0;
unsigned long lastSampleTime  = 0;
unsigned long lastLogSlotMinute = 999;

// Advanced Hydraulic Transient & Water Hammer Tracking
int dropStreak = 0;           // Number of consecutive frames with sharp drop
int sustainedLowCount = 0;    // Number of frames with sustained drop > 3.0 kPa
float baselineAtDischarge = 0.0f;
float totalDischargedLiters = 0.0f;
float peakHammerPressure = 0.0f; // Maximum Joukowsky shockwave peak (kPa)
float hammerDeltaKPa = 0.0f;     // Overpressure surge above baseline (kPa)

// =========================================================================
// ESP-NOW MESH & SCENARIO-AWARE CLUSTER PROTOCOL
// =========================================================================
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6]        = {0};
bool espNowInitialized  = false;
uint8_t lastKnownChannel = 1;

// Node 2 (Rooftop Ultrasonic Ground Truth) Tracking
float peerRoofDistanceCm  = 0.0f;
float peerRoofLevelPct    = 0.0f;
float peerRoofHeightCm    = 0.0f;
int8_t peerRoofRssi       = -127;
unsigned long lastRoofPacketTime = 0;
bool peerRoofNeedsRelay   = false;
uint8_t activeScenario    = 1; // 1 = Dual Online, 2 = Gateway Relay, 3 = Offline Mesh

// =========================================================================
// FUNCTION DECLARATIONS
// =========================================================================
void initWiFi();
void maintainWiFi();
void initOTA();
void initNTP();
void initEspNowCluster();
void onClusterDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len);
bool sendUltrasonicProxyToGoogleSheets(const String& timestampStr, float distCm, float lvlPct);
bool getResilientTime(struct tm* timeinfo);
void loadSettings();
void saveSettings();
void sampleSensors();
void updateFlowStateMachine();
void calculateWaterLevel();
void handleNtpAndLogging();
bool sendToGoogleSheets(const String& timestampStr);
void saveOfflineRecord(const String& timestampStr, float v, float p, float lvl, float h, const String& st, float pk, float dlt);
void uploadBufferedOfflineLogs();
bool postBulkJsonToGoogleSheets(const String& jsonPayload);
int getOfflineQueueCount();
String getCurrentTimeString();
String getNextLogSlotString();

// Web Server Route Handlers
void handleRoot();
void handleStatusApi();
void handleCalibrateApi();
void handleSettingsApi();
void handleManualLogApi();
void handleOtaCheckApi();
void handleEventStartApi();
void handleEventStopApi();
void handleEventStatusApi();

// MQTT Cloud Handlers
void maintainMqtt();
void publishMqttTelemetry();
void publishMqttStatus(const String& msg);
void onMqttMessageReceived(char* topic, byte* payload, unsigned int length);
String getJsonField(const String& json, const String& key);

// =========================================================================
// ESP-NOW RECEIVE HANDLER & DUAL-RADIO SCENARIO COORDINATION
// =========================================================================
void onClusterDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
  if (data_len < (int)sizeof(ClusterPacket)) return;
  ClusterPacket pkt;
  memcpy(&pkt, data, sizeof(ClusterPacket));
  if (pkt.magic != CLUSTER_MAGIC) return;

  if (pkt.senderId == NODE_ID_ROOF_ULTRASONIC) {
    if (pkt.msgType == MSG_ROOF_TELEMETRY) {
      peerRoofDistanceCm = pkt.payload.roofTelemetry.distanceCm;
      peerRoofLevelPct = pkt.payload.roofTelemetry.waterLevelPct;
      peerRoofHeightCm = pkt.payload.roofTelemetry.waterHeightCm;
      peerRoofRssi = pkt.wifiRssi;
      lastRoofPacketTime = millis();

      // Feed high-res 10 Hz event recorder with synchronized ground truth
      eventRecorder.updateRooftopGroundTruth(peerRoofDistanceCm, peerRoofLevelPct);

      // Determine Scenario:
      if (WiFi.status() == WL_CONNECTED) {
        if (peerRoofRssi <= -100) {
          activeScenario = 2; // Scenario 2: Node 1 acts as Gateway Relay for Node 2
          peerRoofNeedsRelay = true;
        } else {
          activeScenario = 1; // Scenario 1: Dual Online (Both Node 1 and Node 2 on Wi-Fi STA)
          peerRoofNeedsRelay = false;
        }
      } else {
        activeScenario = 3; // Scenario 3: Complete Wi-Fi Outage (Offline Mesh & Flash Queue)
      }
    } else if (pkt.msgType == MSG_ROOF_OVERFLOW_STOP) {
      Serial.println(F("[SAFETY] 🚨 High-water overflow cutoff alert received from Node 2!"));
    }
  }
}

void initEspNowCluster() {
  WiFi.macAddress(myMac);
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[ESP-NOW] Init Failed on Node 1!"));
    return;
  }
  espNowInitialized = true;
  esp_now_register_recv_cb(esp_now_recv_cb_t(onClusterDataRecv));

  // Add Broadcast Peer on active channel
  esp_now_peer_info_t bcastPeer = {};
  memcpy(bcastPeer.peer_addr, broadcastMac, 6);
  bcastPeer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : lastKnownChannel;
  bcastPeer.encrypt = false;
  if (esp_now_is_peer_exist(broadcastMac)) esp_now_del_peer(broadcastMac);
  esp_now_add_peer(&bcastPeer);
  Serial.printf("[ESP-NOW] Node 1 Cluster initialized on Channel %d. Broadcast peer added.\n", bcastPeer.channel);
}

bool sendUltrasonicProxyToGoogleSheets(const String& timestampStr, float distCm, float lvlPct) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  String encodedTimestamp = timestampStr;
  encodedTimestamp.replace(" ", "%20");

  String url = googleScriptUrl;
  url.trim();
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += "node=ultrasonic";
  url += "&timestamp=" + encodedTimestamp;
  url += "&distance_cm=" + String(distCm, 1);
  url += "&level_pct=" + String(lvlPct, 1);
  url += "&tank_depth_cm=" + String(tankDepthCm, 1);
  url += "&relayed_by=node1";

  Serial.printf("[GATEWAY-RELAY] 🛰️ (Scenario 2) Relaying Node 2 Ground Truth to Google Sheets...\n");
  bool ok = false;
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code > 0) {
      ok = (code == 200 || code == 302);
      Serial.printf("[GATEWAY-RELAY] Result: %d\n", code);
    }
    http.end();
  }
  client.stop();
  return ok;
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n======================================================="));
  Serial.println(F(" Smart Tank: Non-Invasive Tap Pressure Node Starting "));
  Serial.println(F("======================================================="));

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Configure Internal ADC (Fallback): 12-bit resolution (0-4095) with 2.5dB attenuation (0-1.5V range)
  analogReadResolution(12);
  analogSetAttenuation(ADC_2_5db);

  #if USE_EXTERNAL_ADS1115
  Serial.println(F("[I2C] Probing for ADS1115 16-bit ADC (SDA=GPIO 21, SCL=GPIO 22)..."));
  if (ads.begin(PIN_ADS_SDA, PIN_ADS_SCL)) {
    #if ADS1115_DIRECT_5V
      ads.setGain(ADS1115_Lite::GAIN_TWOTHIRDS); // +/- 6.144V range (0.1875 mV / bit)
      Serial.println(F("[ADS1115] ✅ 16-bit ADC active! Mode: Direct 5V (No divider needed)"));
    #else
      ads.setGain(ADS1115_Lite::GAIN_ONE);       // +/- 4.096V range (0.1250 mV / bit)
      Serial.println(F("[ADS1115] ✅ 16-bit ADC active! Mode: 3.3V with Voltage Divider"));
    #endif
    usingExternalAdc = true;
  } else {
    Serial.println(F("[ADS1115] ⚠️ ADS1115 not detected on 0x48. Using internal ESP32 ADC (GPIO 34)."));
    usingExternalAdc = false;
  }
  #else
  usingExternalAdc = false;
  #endif

  // Load calibration & preferences from Flash NVS
  loadSettings();

  // Initialize Flash SPIFFS for offline data preservation
  if (!SPIFFS.begin(true)) {
    Serial.println(F("[SPIFFS] Flash filesystem mount failed!"));
  } else {
    Serial.printf("[SPIFFS] Mounted! Queued offline logs: %d\n", getOfflineQueueCount());
  }

  // Connect to Wi-Fi
  initWiFi();

  // Initialize Over-The-Air (OTA) Remote Uploading
  initOTA();

  // Configure NTP time
  initNTP();

  // Initialize ESP-NOW Mesh & Scenario Coordination
  initEspNowCluster();

  // Initialize Web Server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/calibrate", HTTP_POST, handleCalibrateApi);
  server.on("/api/settings", HTTP_POST, handleSettingsApi);
  server.on("/api/log_sheets", HTTP_POST, handleManualLogApi);
  server.on("/api/ota_check", HTTP_POST, handleOtaCheckApi);
  server.on("/api/event/start", HTTP_POST, handleEventStartApi);
  server.on("/api/event/stop", HTTP_POST, handleEventStopApi);
  server.on("/api/event/status", HTTP_GET, handleEventStatusApi);
  server.enableCORS(true);
  server.begin();
  Serial.println(F("[HTTP] Embedded Web Server started on port 80"));

  // Initialize MQTT Client with Cloud Command Listener & Buffer Size
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessageReceived);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(1);

  // Initial sensor warm-up read
  for (int i = 0; i < 20; i++) {
    sampleSensors();
    delay(20);
  }
  lastStaticPressure = filteredPressure;
  prevPressure = filteredPressure;
}

// Ensure MQTT connection and auto-subscribe to Cloud Command Topic
void maintainMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected()) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 8000) {
      lastReconnectAttempt = millis();
      String clientId = "ESP32Tap-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(F("[MQTT] ✅ Connected to HiveMQ Broker!"));
        mqttClient.subscribe(MQTT_TOPIC_COMMAND);
        Serial.printf("[MQTT] 🛰️ Subscribed to Cloud Command Topic: %s\n", MQTT_TOPIC_COMMAND);
        publishMqttStatus("ESP32 Online & Ready (v1.2.1)");
      }
    }
  } else {
    mqttClient.loop();
  }
}

// Helper: extract simple key-value from JSON string (zero-dependency)
String getJsonField(const String& json, const String& key) {
  String search = "\"" + key + "\":";
  int idx = json.indexOf(search);
  if (idx == -1) {
    search = "\"" + key + "\" :";
    idx = json.indexOf(search);
  }
  if (idx == -1) return "";
  int valStart = idx + search.length();
  while (valStart < json.length() && (json[valStart] == ' ' || json[valStart] == '\"')) valStart++;
  int valEnd = valStart;
  while (valEnd < json.length() && json[valEnd] != ',' && json[valEnd] != '}' && json[valEnd] != '\"') valEnd++;
  return json.substring(valStart, valEnd);
}

// Publish Status / Ack to Cloud Web Dashboard
void publishMqttStatus(const String& msg) {
  if (!mqttClient.connected()) return;
  String payload = "{\"msg\":\"" + msg + "\",\"p\":" + String(filteredPressure, 2) + ",\"l\":" + String(waterLevelPct, 1) + "}";
  mqttClient.publish(MQTT_TOPIC_STATUS, payload.c_str());
}

void onMqttMessageReceived(char* topic, byte* payload, unsigned int length) {
  char msgBuf[512];
  if (length >= sizeof(msgBuf)) length = sizeof(msgBuf) - 1;
  memcpy(msgBuf, payload, length);
  msgBuf[length] = '\0';
  String json = String(msgBuf);

  Serial.printf("[MQTT] Command received on %s: %s\n", topic, msgBuf);
  String cmd = getJsonField(json, "cmd");

  if (cmd == "start_event") {
    String name = getJsonField(json, "name");
    if (name.length() == 0) name = "Cloud Event";
    float durationSec = getJsonField(json, "duration").toFloat();
    if (durationSec <= 0.0f) durationSec = 30.0f;
    int colorIdx = getJsonField(json, "color").toInt();

    bool ok = eventRecorder.startEvent(name, durationSec, colorIdx);
    if (ok) {
      publishMqttStatus("High-res 10 Hz recording started: " + name);
    } else {
      publishMqttStatus("Event recording busy or already active");
    }
  } else if (cmd == "stop_event") {
    bool ok = eventRecorder.stopEvent();
    if (ok) {
      publishMqttStatus("Event stopped. Uploading samples to Sheet 2...");
    } else {
      publishMqttStatus("No active event to stop");
    }
  } else if (cmd == "cal_empty") {
    pEmptyKPa = filteredPressure;
    saveSettings();
    publishMqttStatus("Calibrated Empty Baseline: " + String(pEmptyKPa, 2) + " kPa");
  } else if (cmd == "cal_full") {
    pFullKPa = filteredPressure;
    saveSettings();
    publishMqttStatus("Calibrated Full Baseline: " + String(pFullKPa, 2) + " kPa");
  } else if (cmd == "manual_cal") {
    float emp = getJsonField(json, "p_empty").toFloat();
    float ful = getJsonField(json, "p_full").toFloat();
    if (ful > emp && emp > 0) {
      pEmptyKPa = emp;
      pFullKPa = ful;
      saveSettings();
      publishMqttStatus("Manual calibration saved: Empty=" + String(pEmptyKPa, 1) + ", Full=" + String(pFullKPa, 1));
    }
  } else if (cmd == "save_settings") {
    if (json.indexOf("\"depth\"") != -1) tankDepthCm = getJsonField(json, "depth").toFloat();
    if (json.indexOf("\"cap\"") != -1) tankCapacityL = getJsonField(json, "cap").toFloat();
    String newUrl = getJsonField(json, "url");
    if (newUrl.length() > 5) googleScriptUrl = newUrl;
    saveSettings();
    publishMqttStatus("Settings saved to ESP32 Flash");
  } else if (cmd == "log_sheets") {
    publishMqttStatus("Triggering immediate Google Sheets log...");
    String ts = getCurrentTimeString();
    sendToGoogleSheets(ts);
  } else if (cmd == "check_ota") {
    publishMqttStatus("Checking GitHub for firmware updates...");
    cloudOTA.checkAndUpdate(true);
  }
}

// Publish Real-Time Telemetry to Global MQTT Broker (5 Hz default / 10 Hz event)
void publishMqttTelemetry() {
  if (!mqttClient.connected()) return;

  char payload[256];
  const char* st = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                   (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";

  // Compact JSON payload with full event recorder feedback:
  // p: Pressure (kPa), l: Level (%), v: Voltage (V), d: dP/dt, s: State,
  // ud: Ultrasound Distance, ul: Ultrasound Level, sc: Scenario,
  // rec: Recording active bool, rec_s: Elapsed recording sec, dur: Target duration sec,
  // samples: Sample count, up: Uploading active bool, up_pct: Upload progress %
  snprintf(payload, sizeof(payload),
           "{\"p\":%.2f,\"l\":%.1f,\"v\":%.3f,\"d\":%.2f,\"s\":\"%s\",\"ud\":%.1f,\"ul\":%.1f,\"sc\":%u,"
           "\"rec\":%s,\"rec_s\":%.1f,\"dur\":%.1f,\"samples\":%d,\"up\":%s,\"up_pct\":%d}",
           filteredPressure, waterLevelPct, sensorVoltage, dPdt, st,
           peerRoofDistanceCm, peerRoofLevelPct, activeScenario,
           eventRecorder.isRecording() ? "true" : "false",
           eventRecorder.getElapsedSec(),
           eventRecorder.getDurationSec(),
           eventRecorder.getSamplesCount(),
           eventRecorder.isUploading() ? "true" : "false",
           eventRecorder.getUploadProgressPct());

  mqttClient.publish(MQTT_TOPIC_TELEMETRY, payload);
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  // Handle Over-The-Air Remote Code Upload requests
  ArduinoOTA.handle();

  // Handle Automatic Cloud OTA updates (checks every 15 minutes)
  cloudOTA.handleLoop();

  // Continuous auto-reconnect and offline backlog drainer
  maintainWiFi();

  server.handleClient();
  maintainMqtt();

  // Process chunked background upload of high-res event data to Sheet 2
  eventRecorder.processChunkedUpload(googleScriptUrl);

  unsigned long currentMillis = millis();

  // Sampling & Telemetry interval:
  // Default is 5 Hz (200 ms). When High-Res Event is actively recording, switches to 10 Hz (100 ms)!
  unsigned long sampleInterval = eventRecorder.isRecording() ? 100 : 200;

  if (currentMillis - lastSampleTime >= sampleInterval) {
    float dt = (currentMillis - lastSampleTime) / 1000.0f;
    lastSampleTime = currentMillis;

    sampleSensors();
    updateFlowStateMachine();
    calculateWaterLevel();
    publishMqttTelemetry(); // Broadcast 5 Hz default / 10 Hz burst stream to global cloud

    // High-Resolution Event Capture at 10 Hz (100 ms precision)
    if (eventRecorder.isRecording()) {
      const char* stStr = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                          (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";
      eventRecorder.captureSample(sensorVoltage, filteredPressure, dPdt, stStr, waterLevelPct, flowRateLps, totalDischargedLiters);
    }
  }

  // Check NTP clock and synchronized 10-minute Google Sheets logging
  handleNtpAndLogging();
}

// =========================================================================
// SENSOR SAMPLING & RESPONSIVE NOISE REJECTION (STABLE 5 HZ / 10 HZ STREAM)
// =========================================================================
void sampleSensors() {
  if (usingExternalAdc) {
    // -----------------------------------------------------------------------
    // A. 16-BIT PRECISION SAMPLING VIA EXTERNAL ADS1115 I2C ADC (4-SAMPLE BURST)
    // -----------------------------------------------------------------------
    float vRead = ads.readVoltageAveraged(ADS1115_PRESSURE_CH, 4);
    if (vRead >= 0.0f) {
      #if ADS1115_DIRECT_5V
        // Direct Mode: Sensor 0.5V - 4.5V output read directly on A0
        sensorVoltage = vRead;
      #else
        // Voltage Divider Mode: Attenuated input scaled back up
        sensorVoltage = vRead / VOLTAGE_DIVIDER_RATIO;
      #endif
      rawAdcAvg = vRead / ads.getLsbVoltage();
    }
  } else {
    // -----------------------------------------------------------------------
    // B. FALLBACK: 12-BIT INTERNAL ESP32 ADC WITH 32-SAMPLE TRIMMED MEAN
    // Discards top 20% and bottom 20% outlier noise spikes to stop random jumps
    // -----------------------------------------------------------------------
    int samples[ADC_OVERSAMPLE_COUNT];
    for (int i = 0; i < ADC_OVERSAMPLE_COUNT; i++) {
      samples[i] = analogRead(PIN_PRESSURE_ADC);
      delayMicroseconds(40);
    }

    // Insertion sort to extract median band
    for (int i = 1; i < ADC_OVERSAMPLE_COUNT; i++) {
      int key = samples[i];
      int j = i - 1;
      while (j >= 0 && samples[j] > key) {
        samples[j + 1] = samples[j];
        j--;
      }
      samples[j + 1] = key;
    }

    // Average the middle 60% of samples (discards any high/low outlier spikes)
    int trimCount = ADC_OVERSAMPLE_COUNT / 5; // 6 samples trimmed on each side
    long middleSum = 0;
    int validCount = 0;
    for (int i = trimCount; i < (ADC_OVERSAMPLE_COUNT - trimCount); i++) {
      middleSum += samples[i];
      validCount++;
    }
    rawAdcAvg = (float)middleSum / (float)validCount;

    float vPin = (rawAdcAvg / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
    sensorVoltage = vPin / VOLTAGE_DIVIDER_RATIO;
  }

  // Convert Sensor Voltage to Gauge Pressure (kPa)
  // Standard 0.2 MPa sensor: 0.5V = 0 kPa, 4.5V = 200 kPa
  if (sensorVoltage < SENSOR_MIN_V) {
    currentPressure = 0.0f;
  } else {
    currentPressure = (sensorVoltage - SENSOR_MIN_V) * (SENSOR_MAX_KPA / (SENSOR_MAX_V - SENSOR_MIN_V));
  }

  // Adaptive Dynamic Noise Rejection:
  // Clamps micro-noise jitter (< 0.25 kPa) while instantly tracking tap transients (> 2.0 kPa)
  if (filteredPressure <= 0.001f) {
    filteredPressure = currentPressure;
  } else {
    float diff = currentPressure - filteredPressure;
    if (fabs(diff) < 0.25f) {
      // Clamped micro-noise deadband: gentle slow creep to track real tank drain without jitter
      filteredPressure += 0.02f * diff;
    } else if (fabs(diff) > 2.0f) {
      // Fast physical transient (tap open / close / hammer): snap immediately!
      filteredPressure = (0.75f * currentPressure) + (0.25f * filteredPressure);
    } else {
      // Moderate step: smooth EMA
      filteredPressure = (PRESSURE_EMA_ALPHA * currentPressure) + ((1.0f - PRESSURE_EMA_ALPHA) * filteredPressure);
    }
  }
}

// =========================================================================
// ADVANCED DUAL-CONDITION HYDRAULIC FLOW & WATER HAMMER CLASSIFIER
// =========================================================================
void updateFlowStateMachine() {
  // 1. Instantaneous time derivative dP/dt in kPa/s (dt = 100 ms)
  float instantDpH = (filteredPressure - prevPressure) / 0.10f;
  prevPressure = filteredPressure;

  // 2. Smooth derivative to eliminate 1-frame ADC glitches
  dPdt = (0.35f * instantDpH) + (0.65f * dPdt);

  unsigned long now = millis();

  switch (tapState) {
    case STATE_STATIC: {
      // Normal equilibrium: continuous baseline tracking
      lastStaticPressure = filteredPressure;

      // Condition A: Fast Dynamic Drop (sudden tap wrench/valve open)
      // Requires dP/dt < -2.2 kPa/s for >= 2 consecutive frames (200 ms)
      if (dPdt < -2.2f) {
        dropStreak++;
      } else {
        dropStreak = 0;
      }

      // Condition B: Slow Valve Turn (gentle knob turn over 2 seconds)
      // Absolute pressure depression > 3.0 kPa below baseline for >= 500 ms
      float deltaP = lastStaticPressure - filteredPressure;
      if (deltaP > 3.0f) {
        sustainedLowCount++;
      } else {
        sustainedLowCount = 0;
      }

      // Trigger Tap Open on either condition
      if (dropStreak >= 2 || sustainedLowCount >= 5) {
        tapState = STATE_TAP_OPEN;
        stateChangeTime = now;
        baselineAtDischarge = lastStaticPressure;
        frozenLevelPct = waterLevelPct;
        totalDischargedLiters = 0.0f;
        dropStreak = 0;
        sustainedLowCount = 0;
        digitalWrite(PIN_STATUS_LED, HIGH);
        Serial.printf("[FLOW-ENG] Dynamic Tap Discharge Confirmed! Baseline=%.2f kPa. Level=%.1f%%\n",
                      baselineAtDischarge, frozenLevelPct);
      }
      break;
    }

    case STATE_TAP_OPEN: {
      // Water is discharging:
      // Real flow integration: Approximate dynamic discharge flow rate (Q ~= 8 - 14 L/min)
      // to realistically deduct water if tap runs for prolonged periods:
      float dynamicHeadLoss = fmaxf(0.0f, baselineAtDischarge - filteredPressure);
      flowRateLps = 0.045f * sqrtf(fmaxf(0.0f, dynamicHeadLoss)); // ~8-12 L/min
      totalDischargedLiters += flowRateLps * 0.10f; // Integrated every 100 ms

      // Recovery Trigger:
      // Valve closure produces a sharp positive hydraulic surge (dP/dt > +1.8 kPa/s)
      // OR pressure climbs back within 1.0 kPa of baseline
      bool closureSurge = (dPdt > 1.8f) && (now - stateChangeTime > 800);
      bool backToBaseline = (filteredPressure >= (baselineAtDischarge - 1.0f)) && (now - stateChangeTime > 1500);

      if (closureSurge || backToBaseline) {
        tapState = STATE_SETTLING;
        flowRateLps = 0.0f;
        stateChangeTime = now;
        peakHammerPressure = currentPressure; // Begin tracking Joukowsky peak
        Serial.printf("[FLOW-ENG] Valve Closure Shock Detected! (dP/dt=%.2f kPa/s). Entering Joukowsky Damping.\n", dPdt);
      }
      break;
    }

    case STATE_SETTLING: {
      flowRateLps = 0.0f;
      // Continuously record the highest overpressure shockwave peak
      if (currentPressure > peakHammerPressure) {
        peakHammerPressure = currentPressure;
      }

      // Hydraulic Water Hammer Damping:
      // Bouncing acoustic pressure waves take 2.5 to 3.5 seconds to damp out.
      if (now - stateChangeTime >= SETTLING_DELAY_MS) {
        tapState = STATE_STATIC;
        // Calculate Joukowsky overpressure surge above settled baseline
        hammerDeltaKPa = fmaxf(0.0f, peakHammerPressure - filteredPressure);
        lastStaticPressure = filteredPressure;
        digitalWrite(PIN_STATUS_LED, LOW);
        Serial.printf("[HAMMER-STUDY] Acoustic waves damped. Equilibrium=%.2f kPa | Joukowsky Peak=%.2f kPa (+%.2f kPa surge)\n",
                      lastStaticPressure, peakHammerPressure, hammerDeltaKPa);
      }
      break;
    }
  }
}

// =========================================================================
// HYDROSTATIC WATER LEVEL ESTIMATION (ULTRA-STABLE)
// =========================================================================
void calculateWaterLevel() {
  float pEval = (tapState == STATE_TAP_OPEN) ? lastStaticPressure : filteredPressure;

  // Two-Point Hydrostatic Formulation:
  // Level % = (P_eval - P_empty) / (P_full - P_empty) * 100%
  float targetPct = 0.0f;
  if (pFullKPa > pEmptyKPa) {
    float rawPct = ((pEval - pEmptyKPa) / (pFullKPa - pEmptyKPa)) * 100.0f;
    targetPct = constrain(rawPct, 0.0f, 100.0f);
  }

  // Direct water level percentage from ADC without digital smoothing filter
  waterLevelPct = (tapState == STATE_TAP_OPEN) ? frozenLevelPct : targetPct;

  // Available Volume in Liters
  waterVolumeL = (waterLevelPct / 100.0f) * tankCapacityL;

  // Hydraulic head in meters: 1 kPa ~= 0.10197 m H2O
  waterHeadM = pEval * 0.10197f;
}

// =========================================================================
// RESILIENT TIMEKEEPING (RTC + NTP SYNCHRONIZATION)
// =========================================================================
bool getResilientTime(struct tm* timeinfo) {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) { // Uninitialized (before late 2023)
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

void saveOfflineRecord(const String& timestampStr, float v, float p, float lvl, float h, const String& st, float pk, float dlt) {
  File f = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_APPEND);
  if (f) {
    f.printf("%s,%.3f,%.2f,%.1f,%.2f,%s,%.2f,%.2f\n",
             timestampStr.c_str(), v, p, lvl, h, st.c_str(), pk, dlt);
    f.close();
    Serial.printf("[OFFLINE] Stored in Flash: %s | P=%.2f kPa, L=%.1f%% (Queue count: %d)\n",
                  timestampStr.c_str(), p, lvl, getOfflineQueueCount());
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
  client.setInsecure(); // Skip certificate verification for Google Apps Script

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(25000); // 25s timeout for batch bulk ingestion

  bool success = false;
  if (http.begin(client, googleScriptUrl)) {
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

    String json = "{\"node\":\"pressure\",\"bulk\":[";
    int validRecords = 0;
    for (int i = 0; i < count; i++) {
      String l = lines[i];
      int c1 = l.indexOf(',');
      int c2 = l.indexOf(',', c1 + 1);
      int c3 = l.indexOf(',', c2 + 1);
      int c4 = l.indexOf(',', c3 + 1);
      int c5 = l.indexOf(',', c4 + 1);
      int c6 = l.indexOf(',', c5 + 1);
      int c7 = l.indexOf(',', c6 + 1);

      if (c1 > 0 && c2 > 0 && c3 > 0) {
        String ts  = l.substring(0, c1);
        String v   = l.substring(c1 + 1, c2);
        String p   = l.substring(c2 + 1, c3);
        String lvl = l.substring(c3 + 1, c4);
        String h   = (c5 > 0) ? l.substring(c4 + 1, c5) : "0";
        String st  = (c6 > 0) ? l.substring(c5 + 1, c6) : "STATIC";
        String pk  = (c7 > 0) ? l.substring(c6 + 1, c7) : "0";
        String dlt = (c7 > 0) ? l.substring(c7 + 1) : "0";

        if (validRecords > 0) json += ",";
        json += "{\"timestamp\":\"" + ts + "\",\"voltage\":" + v +
                ",\"pressure_kpa\":" + p + ",\"level_pct\":" + lvl +
                ",\"head_m\":" + h + ",\"tap_state\":\"" + st +
                "\",\"peak_hammer_kpa\":" + pk + ",\"hammer_delta_kpa\":" + dlt + "}";
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
    // Preserve remaining unsent lines
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
    return; // Clock not yet seeded
  }

  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;

  // Trigger at exact 1-minute slot boundary (:00, :01, :02, ...)
  if ((currentMinute % LOG_INTERVAL_MINUTES == 0) && (currentSecond <= 5)) {
    if (lastLogSlotMinute != currentMinute) {
      lastLogSlotMinute = currentMinute; // Lock to prevent duplicate triggers

      char timeBuf[30];
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:00", &timeinfo);
      String timestampStr = String(timeBuf);

      String stateStr = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                        (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";
      float logP = filteredPressure; // Real gauge output directly from ADC

      Serial.printf("[SYNC] 1-Minute boundary reached: %s. Uploading...\n", timeBuf);

      bool sent = false;
      if (WiFi.status() == WL_CONNECTED) {
        sent = sendToGoogleSheets(timestampStr);

        // Scenario 2: Relay Node 2 Ground Truth to Google Sheets if Node 2 has no Wi-Fi
        if (peerRoofNeedsRelay && (millis() - lastRoofPacketTime < 15000)) {
          sendUltrasonicProxyToGoogleSheets(timestampStr, peerRoofDistanceCm, peerRoofLevelPct);
        }
      }

      if (!sent) {
        // Wi-Fi down or Google Sheets unreachable -> Persist to Flash memory!
        saveOfflineRecord(timestampStr, sensorVoltage, logP, waterLevelPct, waterHeadM, stateStr, peakHammerPressure, hammerDeltaKPa);
      } else {
        // If live upload succeeded and there are older offline logs pending, drain them!
        uploadBufferedOfflineLogs();
      }
    }
  }
}

// =========================================================================
// HTTPS CLIENT FOR GOOGLE APPS SCRIPT (LIVE 1-MIN LOGGING)
// =========================================================================
bool sendToGoogleSheets(const String& timestampStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[SHEETS] Wi-Fi not connected. Switching to offline Flash queue."));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Follow Google 302 redirection!
  http.setTimeout(15000);

  String stateStr = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                    (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";
  float logP = filteredPressure; // Real gauge output directly from ADC
  float logLevel = waterLevelPct;

  String encodedTimestamp = timestampStr;
  encodedTimestamp.replace(" ", "%20");

  String url = googleScriptUrl;
  url.trim();
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += "node=pressure";
  url += "&timestamp=" + encodedTimestamp;
  url += "&voltage=" + String(sensorVoltage, 3);
  url += "&pressure_kpa=" + String(logP, 2);
  url += "&level_pct=" + String(logLevel, 1);
  url += "&head_m=" + String(waterHeadM, 2);
  url += "&tap_state=" + stateStr;
  url += "&tank_depth_cm=" + String(tankDepthCm, 1);
  url += "&peak_hammer_kpa=" + String(peakHammerPressure, 2);
  url += "&hammer_delta_kpa=" + String(hammerDeltaKPa, 2);

  Serial.printf("[SHEETS] Sending GET request to: %s\n", url.c_str());
  bool success = false;
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    Serial.printf("[SHEETS] HTTP Response Code: %d\n", httpCode);

    if (httpCode > 0) {
      String response = http.getString();
      Serial.printf("[SHEETS] Response Payload: %s\n", response.c_str());
      success = (httpCode == 200 || httpCode == 302);
    } else {
      Serial.printf("[SHEETS] HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println(F("[SHEETS] Unable to connect to host"));
  }

  client.stop(); // Explicitly close SSL socket to free TLS buffers
  return success;
}

// =========================================================================
// SETTINGS / PREFERENCES PERSISTENCE (NVS)
// =========================================================================
void loadSettings() {
  preferences.begin("smart_tank", true); // Read-only mode
  pEmptyKPa       = preferences.getFloat("p_empty", DEFAULT_P_EMPTY_KPA);
  pFullKPa        = preferences.getFloat("p_full", DEFAULT_P_FULL_KPA);
  tankDepthCm     = preferences.getFloat("tank_depth", DEFAULT_TANK_DEPTH_CM);
  if (tankDepthCm >= 149.0f && tankDepthCm <= 151.0f) {
    tankDepthCm = DEFAULT_TANK_DEPTH_CM; // Update legacy 150cm default to 122cm
  }
  tankCapacityL   = preferences.getFloat("tank_cap", DEFAULT_TANK_CAPACITY_L);
  googleScriptUrl = preferences.getString("script_url", GOOGLE_SCRIPT_URL);
  String savedToken = preferences.getString("gh_token", DEFAULT_GITHUB_OTA_TOKEN);
  cloudOTA.setToken(savedToken);
  preferences.end();

  Serial.println(F("[NVS] Configuration loaded from Flash:"));
  Serial.printf("      P_empty: %.2f kPa | P_full: %.2f kPa\n", pEmptyKPa, pFullKPa);
  Serial.printf("      Tank Depth: %.1f cm | Tank Capacity: %.0f L\n", tankDepthCm, tankCapacityL);
}

void saveSettings() {
  preferences.begin("smart_tank", false); // Read-write mode
  preferences.putFloat("p_empty", pEmptyKPa);
  preferences.putFloat("p_full", pFullKPa);
  preferences.putFloat("tank_depth", tankDepthCm);
  preferences.putFloat("tank_cap", tankCapacityL);
  preferences.putString("script_url", googleScriptUrl);
  preferences.end();

  Serial.println(F("[NVS] Configuration successfully saved to Flash NVS!"));
}

// =========================================================================
// ROBUST AUTO-RECONNECTING WI-FI MANAGER (PRIMARY & PROXY EXTENDER)
// =========================================================================
void initWiFi() {
  Serial.printf("[WIFI] Initializing Station connection (Primary: %s | Proxy: %s)...\n",
                DEFAULT_WIFI_SSID, PROXY_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Register both Primary Router and Proxy Extender with WiFiMulti
  wifiMulti.addAP(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
  wifiMulti.addAP(PROXY_WIFI_SSID, PROXY_WIFI_PASS);

  int attempts = 0;
  while (wifiMulti.run(4000) != WL_CONNECTED && attempts < 15) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lastKnownChannel = WiFi.channel();
    Serial.printf("\n[WIFI] Connected to '%s'! IP: %s | RSSI: %d dBm | Channel: %d\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), lastKnownChannel);
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
    lastKnownChannel = WiFi.channel();
    if (!wasConnected) {
      wasConnected = true;
      disconnectStart = 0;
      Serial.printf("\n[WIFI] Wi-Fi Restored on '%s'! IP: %s | RSSI: %d dBm | Channel: %d\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), lastKnownChannel);
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER); // Resync NTP

      // Update ESP-NOW broadcast peer channel
      if (espNowInitialized) {
        esp_now_peer_info_t bcastPeer = {};
        memcpy(bcastPeer.peer_addr, broadcastMac, 6);
        bcastPeer.channel = lastKnownChannel;
        bcastPeer.encrypt = false;
        if (esp_now_is_peer_exist(broadcastMac)) esp_now_del_peer(broadcastMac);
        esp_now_add_peer(&bcastPeer);
      }

      uploadBufferedOfflineLogs(); // Immediately drain offline backlog!
    }
    return;
  }

  // Not connected
  if (wasConnected) {
    wasConnected = false;
    disconnectStart = now;
    activeScenario = 3;
    Serial.println(F("\n[WIFI] WARNING: Wi-Fi dropped! Storing telemetry in Flash memory..."));
  }

  // Keep ESP-NOW locked to last known channel even while disconnected
  if (lastKnownChannel > 0 && lastKnownChannel <= 14) {
    esp_wifi_set_channel(lastKnownChannel, WIFI_SECOND_CHAN_NONE);
  }

  unsigned long offlineSec = (disconnectStart > 0) ? ((now - disconnectStart) / 1000) : 0;
  Serial.printf("[WIFI] Auto-reconnecting to best AP ('%s' or '%s') (Offline: %lu s | Queued: %d records)...\n",
                DEFAULT_WIFI_SSID, PROXY_WIFI_SSID, offlineSec, getOfflineQueueCount());

  // Use WiFiMulti to auto-select and connect to whichever AP is currently in range / strongest
  wifiMulti.run(3000);
}

void initOTA() {
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA
    .onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("[OTA] Remote code upload starting: " + type);
    })
    .onEnd([]() {
      Serial.println("\n[OTA] Remote code upload finished successfully!");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  Serial.printf("[OTA] Remote Code Upload ready on hostname '%s', Port %d\n", OTA_HOSTNAME, OTA_PORT);
}

void initNTP() {
  Serial.println(F("[NTP] Initializing time synchronization..."));
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getResilientTime(&timeinfo)) {
    return "--:--:--";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

String getNextLogSlotString() {
  struct tm timeinfo;
  if (!getResilientTime(&timeinfo)) {
    return "--";
  }
  int nextMin = timeinfo.tm_min + 1;
  int hour = timeinfo.tm_hour;
  if (nextMin >= 60) {
    nextMin = 0;
    hour = (hour + 1) % 24;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:00", hour, nextMin);
  return String(buf);
}

// =========================================================================
// WEB SERVER HANDLERS
// =========================================================================
void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatusApi() {
  String stateStr = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                    (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";

  String json = "{";
  json += "\"level_pct\":" + String(waterLevelPct, 1) + ",";
  json += "\"frozen_pct\":" + String(frozenLevelPct, 1) + ",";
  json += "\"volume_liters\":" + String(waterVolumeL, 0) + ",";
  json += "\"tank_capacity\":" + String(tankCapacityL, 0) + ",";
  json += "\"tank_depth\":" + String(tankDepthCm, 0) + ",";
  json += "\"pressure_kpa\":" + String(filteredPressure, 2) + ",";
  json += "\"head_m\":" + String(waterHeadM, 2) + ",";
  json += "\"voltage\":" + String(sensorVoltage, 3) + ",";
  json += "\"raw_adc\":" + String((int)rawAdcAvg) + ",";
  json += "\"dp_dt\":" + String(dPdt, 2) + ",";
  json += "\"tap_state\":\"" + stateStr + "\",";
  json += "\"p_empty\":" + String(pEmptyKPa, 1) + ",";
  json += "\"p_full\":" + String(pFullKPa, 1) + ",";
  json += "\"time_str\":\"" + getCurrentTimeString() + "\",";
  json += "\"next_log_slot\":\"" + getNextLogSlotString() + "\",";
  json += "\"offline_queue\":" + String(getOfflineQueueCount()) + ",";
  json += "\"wifi_connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"adc_mode\":\"" + String(usingExternalAdc ? "ADS1115 (16-bit)" : "ESP32 Internal (12-bit)") + "\",";
  json += "\"ota_status\":\"" + cloudOTA.getStatus() + "\",";
  json += "\"event_recording\":" + String(eventRecorder.isRecording() ? "true" : "false") + ",";
  json += "\"event_uploading\":" + String(eventRecorder.isUploading() ? "true" : "false") + ",";
  json += "\"event_name\":\"" + eventRecorder.getEventName() + "\",";
  json += "\"event_elapsed\":" + String(eventRecorder.getElapsedSec(), 1) + ",";
  json += "\"event_duration\":" + String(eventRecorder.getDurationSec(), 1) + ",";
  json += "\"event_samples\":" + String(eventRecorder.getSamplesCount()) + ",";
  json += "\"event_upload_pct\":" + String(eventRecorder.getUploadProgressPct()) + ",";
  json += "\"scenario\":" + String(activeScenario) + ",";
  json += "\"roof_distance\":" + String(peerRoofDistanceCm, 1) + ",";
  json += "\"roof_level\":" + String(peerRoofLevelPct, 1) + ",";
  json += "\"roof_rssi\":" + String(peerRoofRssi) + ",";
  json += "\"roof_age_ms\":" + String((lastRoofPacketTime > 0) ? (millis() - lastRoofPacketTime) : 999999) + ",";
  json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleCalibrateApi() {
  if (!server.hasArg("action")) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing action param\"}");
    return;
  }

  String action = server.arg("action");
  if (action == "empty") {
    pEmptyKPa = filteredPressure;
    saveSettings();
    Serial.printf("[CALIB] Calibrated EMPTY state to: %.2f kPa\n", pEmptyKPa);
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Calibrated EMPTY state successfully!\"}");
  } 
  else if (action == "full") {
    pFullKPa = filteredPressure;
    saveSettings();
    Serial.printf("[CALIB] Calibrated FULL state to: %.2f kPa\n", pFullKPa);
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Calibrated FULL state successfully!\"}");
  }
  else if (action == "manual") {
    if (server.hasArg("p_empty") && server.hasArg("p_full")) {
      pEmptyKPa = server.arg("p_empty").toFloat();
      pFullKPa  = server.arg("p_full").toFloat();
      saveSettings();
      Serial.printf("[CALIB] Manual calibration updated: Empty=%.2f, Full=%.2f\n", pEmptyKPa, pFullKPa);
      server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Manual calibration saved!\"}");
    } else {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing p_empty or p_full\"}");
    }
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Unknown calibration action\"}");
  }
}

void handleSettingsApi() {
  if (server.hasArg("tank_depth")) {
    tankDepthCm = server.arg("tank_depth").toFloat();
  }
  if (server.hasArg("tank_cap")) {
    tankCapacityL = server.arg("tank_cap").toFloat();
  }
  if (server.hasArg("script_url") && server.arg("script_url").length() > 5) {
    googleScriptUrl = server.arg("script_url");
  }
  if (server.hasArg("gh_token")) {
    String tokenVal = server.arg("gh_token");
    tokenVal.trim();
    preferences.begin("smart_tank", false);
    preferences.putString("gh_token", tokenVal);
    preferences.end();
    cloudOTA.setToken(tokenVal);
    Serial.println(F("[NVS] Updated GitHub OTA Token saved to flash."));
  }

  saveSettings();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Settings updated successfully!\"}");
}

void handleManualLogApi() {
  struct tm timeinfo;
  char timeBuf[30] = "2026-10-24 12:00:00";
  if (getLocalTime(&timeinfo)) {
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:00", &timeinfo);
  }

  bool ok = sendToGoogleSheets(String(timeBuf));
  if (ok) {
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Logged to Google Sheets successfully!\"}");
  } else {
    server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to send data to Google Sheets\"}");
  }
}

void handleOtaCheckApi() {
  Serial.println(F("[WEB] Manual Cloud OTA check triggered via Web Dashboard"));
  cloudOTA.checkAndUpdate(true);
  String json = "{\"status\":\"success\",\"message\":\"" + cloudOTA.getStatus() + "\"}";
  server.send(200, "application/json", json);
}

// =========================================================================
// HIGH-RESOLUTION EVENT REST API HANDLERS
// =========================================================================
void handleEventStartApi() {
  String name = server.hasArg("name") ? server.arg("name") : "Tap Event";
  float durationSec = 30.0f;
  if (server.hasArg("duration_sec")) {
    durationSec = server.arg("duration_sec").toFloat();
  } else if (server.hasArg("duration_min")) {
    durationSec = server.arg("duration_min").toFloat() * 60.0f;
  }
  if (durationSec < 5.0f) durationSec = 5.0f;
  if (durationSec > 600.0f) durationSec = 600.0f; // Max 10 min

  int colorIdx = server.hasArg("color_idx") ? server.arg("color_idx").toInt() : (int)(millis() % 8);

  bool ok = eventRecorder.startEvent(name, durationSec, colorIdx);
  if (ok) {
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"High-res 10 Hz recording started!\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Recorder busy or upload in progress\"}");
  }
}

void handleEventStopApi() {
  eventRecorder.stopEvent();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Recording stopped. Streaming chunks to Sheet 2...\"}");
}

void handleEventStatusApi() {
  String json = "{";
  json += "\"recording\":" + String(eventRecorder.isRecording() ? "true" : "false") + ",";
  json += "\"name\":\"" + eventRecorder.getEventName() + "\",";
  json += "\"event_id\":\"" + eventRecorder.getEventId() + "\",";
  json += "\"elapsed_sec\":" + String(eventRecorder.getElapsedSec(), 1) + ",";
  json += "\"duration_sec\":" + String(eventRecorder.getDurationSec(), 1) + ",";
  json += "\"samples\":" + String(eventRecorder.getSamplesCount()) + ",";
  json += "\"uploading\":" + String(eventRecorder.isUploading() ? "true" : "false") + ",";
  json += "\"upload_pct\":" + String(eventRecorder.getUploadProgressPct()) + ",";
  json += "\"upload_status\":\"" + eventRecorder.getUploadStatus() + "\",";
  json += "\"rooftop_dist\":" + String(eventRecorder.getRooftopDist(), 1) + ",";
  json += "\"rooftop_level\":" + String(eventRecorder.getRooftopLevel(), 1);
  json += "}";
  server.send(200, "application/json", json);
}
