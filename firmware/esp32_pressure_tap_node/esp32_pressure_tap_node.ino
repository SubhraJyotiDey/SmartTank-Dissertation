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
#include <esp_now.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <time.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "ads1115_driver.h"
#include "web_dashboard.h"
#include "cloud_ota_updater.h"
#include "cluster_protocol.h"

// External ADS1115 16-Bit Precision ADC Object
ADS1115_Lite ads(ADS1115_I2C_ADDR);
bool usingExternalAdc = false;

// Automatic Cloud OTA Self-Updater (every 15 minutes)
CloudOTAUpdater cloudOTA(FIRMWARE_VERSION, OTA_BIN_FILENAME, DEFAULT_GITHUB_OTA_TOKEN);

#include "highres_event_recorder.h"
// High-Resolution 10 Hz Event Recorder (Buffers in SPIFFS & Uploads in Chunks to Sheet 2)
HighResEventRecorder eventRecorder;

// =========================================================================
// ESP-NOW CLUSTER PEER DEFINITIONS & MULTI-ROLE DECENTRALIZED PROTOCOL
// =========================================================================
uint8_t pumpNodeMac[6]    = {0x88, 0x57, 0x21, 0x7A, 0xA9, 0x80}; // Node 3 (Pump Relay)
uint8_t roofNodeMac[6]    = {0xE0, 0x8C, 0xFE, 0x32, 0x98, 0x7C}; // Node 2 (Rooftop Ultrasonic)
uint8_t broadcastMac[6]   = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Cluster Broadcast
uint8_t myMac[6]          = {0};

bool espNowInitialized    = false;

// Dynamic Multi-Role / Gateway Election State
bool isElectedGateway     = true; // Node 1 default (evaluated dynamically via RSSI)
int8_t peerPumpRssi       = -127;
uint8_t peerPumpChannel   = 0;
unsigned long gatewayClaimDebounceStart = 0;

// Pump Safety Lockout & Status (Master Authority from Node 3)
bool pumpSafetyLockout        = false;
uint8_t pumpLockoutFaultCode  = 0;
unsigned long pumpLockoutUntil= 0;
String pumpLockoutReason      = "Normal";

// Automatic Level Control (Autonomous Master Role for Tank Level)
bool autoPumpControlEnabled   = false; // Safe default for lab/bench testing
float pumpAutoStartThreshold  = 20.0f; // Turn ON if level <= 20%
float pumpAutoStopThreshold   = 95.0f; // Turn OFF if level >= 95%
bool pumpActiveTracked        = false; // Tracked pump state
unsigned long lastPumpCmdTime = 0;

// Pending Bulk Chunk Buffer for Asynchronous Core 0 Forwarding
struct PendingBulkChunk {
    bool hasData;
    uint8_t senderMac[6];
    uint8_t senderId;
    uint16_t chunkId;
    uint8_t recordCount;
    char csvRows[160];
};
PendingBulkChunk pendingProxyChunk = {false, {0}, 0, 0, 0, ""};

// ESP-NOW Receiver Callback (Cluster Dispatcher)
void onClusterDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != sizeof(ClusterPacket)) return;

  ClusterPacket pkt;
  memcpy(&pkt, data, sizeof(ClusterPacket));
  if (pkt.magic != CLUSTER_MAGIC) return;

  unsigned long now = millis();

  switch (pkt.msgType) {
    // CASE 1: CLUSTER HEARTBEAT & GATEWAY ELECTION
    case MSG_CLUSTER_HEARTBEAT: {
      if (pkt.senderId == NODE_ID_PUMP_RELAY) {
        peerPumpRssi = pkt.wifiRssi;
        peerPumpChannel = pkt.wifiChannel;

        // Deterministic Gateway Election Tie-Breaker
        int8_t myRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
        bool shouldIBeGateway = evaluateGatewaySuperiority(myRssi, peerPumpRssi, myMac, pkt.senderMac);

        if (shouldIBeGateway != isElectedGateway) {
          if (gatewayClaimDebounceStart == 0) {
            gatewayClaimDebounceStart = now; // 15-second debounce
          } else if (now - gatewayClaimDebounceStart >= 15000UL) {
            isElectedGateway = shouldIBeGateway;
            gatewayClaimDebounceStart = 0;
            Serial.printf("[CLUSTER] Gateway Role Evaluated: %s (My RSSI: %d, Node 3 RSSI: %d)\n",
                          isElectedGateway ? "I AM GATEWAY" : "NODE 3 IS GATEWAY", myRssi, peerPumpRssi);
          }
        } else {
          gatewayClaimDebounceStart = 0;
        }
      }
      break;
    }

    // CASE 2: SAFETY OVERRIDE (NODE 3 MASTER AUTHORITY)
    case MSG_SAFETY_OVERRIDE: {
      if (pkt.senderId == NODE_ID_PUMP_RELAY) {
        pumpSafetyLockout = true;
        pumpLockoutFaultCode = pkt.payload.safety.faultCode;
        pumpLockoutUntil = now + (pkt.payload.safety.lockoutSec * 1000UL);
        pumpLockoutReason = (pkt.payload.safety.faultCode == 1) ? "Watchdog Timeout (>10s)" :
                            (pkt.payload.safety.faultCode == 2) ? "Max Continuous Runtime (35 min)" :
                            "Thermal/Dry-Run Cutoff";
        pumpActiveTracked = false;
        Serial.printf("[CLUSTER] 🚨 Node 3 SAFETY OVERRIDE ACTIVE! Fault %d (%s), lockout for %u s\n",
                      pumpLockoutFaultCode, pumpLockoutReason.c_str(), pkt.payload.safety.lockoutSec);
      }
      break;
    }

    // CASE 3: BULK DATA FORWARDING (WHEN NODE 1 IS ELECTED GATEWAY)
    case MSG_BULK_DATA_CHUNK: {
      if (isElectedGateway && (WiFi.status() == WL_CONNECTED) && !pendingProxyChunk.hasData) {
        pendingProxyChunk.hasData = true;
        memcpy(pendingProxyChunk.senderMac, mac, 6);
        pendingProxyChunk.senderId = pkt.senderId;
        pendingProxyChunk.chunkId = pkt.payload.bulkData.chunkId;
        pendingProxyChunk.recordCount = pkt.payload.bulkData.recordCount;
        strncpy(pendingProxyChunk.csvRows, pkt.payload.bulkData.csvRows, sizeof(pendingProxyChunk.csvRows) - 1);
        pendingProxyChunk.csvRows[sizeof(pendingProxyChunk.csvRows) - 1] = '\0';
        Serial.printf("[GATEWAY] Buffered Chunk #%u from Node %u (%u records) for background upload\n",
                      pendingProxyChunk.chunkId, pendingProxyChunk.senderId, pendingProxyChunk.recordCount);
      }
      break;
    }

    default:
      break;
  }
}

void initEspNowCluster() {
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[ESP-NOW] Init Failed on Node 1!"));
    return;
  }
  WiFi.macAddress(myMac);

  esp_now_register_recv_cb(esp_now_recv_cb_t(onClusterDataRecv));

  // Add Broadcast Peer (for cluster heartbeats)
  esp_now_peer_info_t bcastPeer = {};
  memcpy(bcastPeer.peer_addr, broadcastMac, 6);
  bcastPeer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;
  bcastPeer.encrypt = false;
  if (esp_now_is_peer_exist(broadcastMac)) esp_now_del_peer(broadcastMac);
  esp_now_add_peer(&bcastPeer);

  // Add Node 3 Dedicated Peer (Pump Relay)
  esp_now_peer_info_t pumpPeer = {};
  memcpy(pumpPeer.peer_addr, pumpNodeMac, 6);
  pumpPeer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;
  pumpPeer.encrypt = false;
  if (esp_now_is_peer_exist(pumpNodeMac)) esp_now_del_peer(pumpNodeMac);
  if (esp_now_add_peer(&pumpPeer) == ESP_OK) {
    espNowInitialized = true;
    Serial.println(F("[ESP-NOW] Cluster peers registered successfully!"));
  } else {
    Serial.println(F("[ESP-NOW] Failed to register Pump Node peer!"));
  }
}

inline void initEspNowSender() {
  initEspNowCluster();
}

bool sendEspNowPumpCommand(uint8_t cmd, float level) {
  if (!espNowInitialized) {
    initEspNowCluster();
    if (!espNowInitialized) return false;
  }

  unsigned long now = millis();

  // Safety Lockout Check: If Node 3 tripped, reject START command until cooldown expires
  if (cmd == 1 && pumpSafetyLockout) {
    if (now < pumpLockoutUntil) {
      unsigned long secLeft = (pumpLockoutUntil - now) / 1000UL;
      Serial.printf("[PUMP] Command rejected: Node 3 Safety Lockout active (%s, %lu s remaining)\n",
                    pumpLockoutReason.c_str(), secLeft);
      return false;
    } else {
      pumpSafetyLockout = false;
      pumpLockoutReason = "Normal (Cooldown Expired)";
    }
  }

  static uint16_t pumpCmdSeq = 1;
  ClusterPacket pkt;
  memset(&pkt, 0, sizeof(ClusterPacket));
  pkt.magic = CLUSTER_MAGIC;
  pkt.senderId = NODE_ID_TAP_PRESSURE;
  pkt.msgType = MSG_PUMP_COMMAND;
  pkt.seqNumber = pumpCmdSeq++;
  pkt.wifiChannel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;
  pkt.wifiRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
  pkt.isGateway = isElectedGateway ? 1 : 0;
  memcpy(pkt.senderMac, myMac, 6);

  pkt.payload.pump.command = cmd;
  pkt.payload.pump.tankLevelPct = level;
  pkt.payload.pump.fillBudgetSec = 2100; // 35 min max budget

  esp_err_t res = esp_now_send(pumpNodeMac, (uint8_t *)&pkt, sizeof(pkt));
  if (res == ESP_OK) {
    pumpActiveTracked = (cmd == 1);
    lastPumpCmdTime = now;
    return true;
  }
  return false;
}

void broadcastClusterHeartbeat() {
  if (!espNowInitialized) return;
  static uint16_t hbSeq = 1;
  ClusterPacket pkt;
  memset(&pkt, 0, sizeof(ClusterPacket));
  pkt.magic = CLUSTER_MAGIC;
  pkt.senderId = NODE_ID_TAP_PRESSURE;
  pkt.msgType = MSG_CLUSTER_HEARTBEAT;
  pkt.seqNumber = hbSeq++;
  pkt.wifiChannel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;
  pkt.wifiRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
  pkt.isGateway = isElectedGateway ? 1 : 0;
  memcpy(pkt.senderMac, myMac, 6);

  esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
}

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
float ultrasonicFullDistCm = DEFAULT_ULTRASONIC_FULL_DISTANCE_CM; // Air gap distance at 100% full (Tank Full Scenario)
String googleScriptUrl = GOOGLE_SCRIPT_URL;

// Live Telemetry
float rawAdcAvg        = 0.0f;
float sensorVoltage    = 0.0f;
float currentPressure  = 0.0f;
float filteredPressure = 0.0f;
float lastStaticPressure = 0.0f;
float smoothedStaticPressure = 0.0f;
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
// FUNCTION DECLARATIONS
// =========================================================================
void initWiFi();
void maintainWiFi();
void wifiMaintainerTask(void *pvParameters);
void initOTA();
void initNTP();
bool getResilientTime(struct tm* timeinfo);
void loadSettings();
void saveSettings();
void forwardConfigToNode2();
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
void handleOtaUpdateApi();
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

  // Start background rooftop node poller on Core 0
  eventRecorder.setScriptUrl(googleScriptUrl);
  eventRecorder.begin();

  // Initialize Over-The-Air (OTA) Remote Uploading
  initOTA();

  // Configure NTP time
  initNTP();

  // Initialize Web Server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/calibrate", HTTP_POST, handleCalibrateApi);
  server.on("/api/settings", HTTP_POST, handleSettingsApi);
  server.on("/api/log_sheets", HTTP_POST, handleManualLogApi);
  server.on("/api/ota_check", HTTP_POST, handleOtaCheckApi);
  server.on("/api/ota_update", HTTP_ANY, handleOtaUpdateApi);
  server.on("/api/event/start", HTTP_POST, handleEventStartApi);
  server.on("/api/event/stop", HTTP_POST, handleEventStopApi);
  server.on("/api/event/status", HTTP_GET, handleEventStatusApi);
  server.on("/api/test_espnow", HTTP_GET, []() {
    int cmd = server.hasArg("cmd") ? server.arg("cmd").toInt() : 1;
    bool ok = sendEspNowPumpCommand((uint8_t)cmd, waterLevelPct);
    String json = "{\"status\":\"" + String(ok ? "sent" : "rejected") + "\",\"cmd\":" + String(cmd) + ",\"level\":" + String(waterLevelPct, 1) + ",\"lockout\":" + String(pumpSafetyLockout ? "true" : "false") + ",\"reason\":\"" + pumpLockoutReason + "\",\"target_mac\":\"88:57:21:7A:A9:80\"}";
    server.send(ok ? 200 : 409, "application/json", json);
  });
  server.on("/api/pump", HTTP_GET, []() {
    int cmd = server.hasArg("state") ? server.arg("state").toInt() : (server.hasArg("cmd") ? server.arg("cmd").toInt() : 1);
    bool ok = sendEspNowPumpCommand((uint8_t)cmd, waterLevelPct);
    String json = "{\"status\":\"" + String(ok ? "sent" : "rejected") + "\",\"cmd\":" + String(cmd) + ",\"level\":" + String(waterLevelPct, 1) + ",\"lockout\":" + String(pumpSafetyLockout ? "true" : "false") + ",\"reason\":\"" + pumpLockoutReason + "\"}";
    server.send(ok ? 200 : 409, "application/json", json);
  });
  server.on("/api/pump_auto", HTTP_GET, []() {
    if (server.hasArg("enable")) {
      autoPumpControlEnabled = (server.arg("enable").toInt() == 1);
    }
    if (server.hasArg("start_pct")) {
      pumpAutoStartThreshold = server.arg("start_pct").toFloat();
    }
    if (server.hasArg("stop_pct")) {
      pumpAutoStopThreshold = server.arg("stop_pct").toFloat();
    }
    String json = "{\"status\":\"ok\",\"auto_enabled\":" + String(autoPumpControlEnabled ? "true" : "false") + ",\"start_threshold\":" + String(pumpAutoStartThreshold, 1) + ",\"stop_threshold\":" + String(pumpAutoStopThreshold, 1) + "}";
    server.send(200, "application/json", json);
  });
  server.begin();
  Serial.println(F("[HTTP] Embedded Web Server started on port 80"));

  // Initialize MQTT Client with Cloud Command Listener & Buffer Size
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessageReceived);
  mqttClient.setBufferSize(512);

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
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      String clientId = "ESP32Tap-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(F("[MQTT] ✅ Connected to Global HiveMQ Broker!"));
        mqttClient.subscribe(MQTT_TOPIC_COMMAND);
        Serial.printf("[MQTT] 🛰️ Subscribed to Cloud Command Topic: %s\n", MQTT_TOPIC_COMMAND);
        publishMqttStatus("ESP32 Online & Ready");
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
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"msg\":\"%s\",\"p\":%.2f,\"l\":%.1f,\"ver\":\"%s\"}",
           msg.c_str(), filteredPressure, waterLevelPct, FIRMWARE_VERSION);
  mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

// Inbound Cloud MQTT Command Listener
void onMqttMessageReceived(char* topic, byte* payload, unsigned int length) {
  if (length == 0 || length >= 512) return;
  char message[512];
  memcpy(message, payload, length);
  message[length] = '\0';
  String msgStr = String(message);
  Serial.printf("[MQTT-CMD] 📥 Received from Cloud: %s\n", msgStr.c_str());

  String cmd = getJsonField(msgStr, "cmd");

  if (cmd == "start_event") {
    String name = getJsonField(msgStr, "name");
    if (name.length() == 0) name = "Cloud Event";
    String durStr = getJsonField(msgStr, "duration");
    float dur = durStr.length() > 0 ? durStr.toFloat() : 30.0f;
    String colStr = getJsonField(msgStr, "color");
    int colorIdx = colStr.length() > 0 ? colStr.toInt() : 0;

    bool ok = eventRecorder.startEvent(name, dur, colorIdx);
    publishMqttStatus(ok ? ("Event Started: " + name) : "Event Busy");
  } 
  else if (cmd == "stop_event") {
    eventRecorder.stopEvent();
    publishMqttStatus("Event Stopped. Streaming to Sheet 2...");
  } 
  else if (cmd == "cal_empty") {
    pEmptyKPa = filteredPressure;
    saveSettings();
    Serial.printf("[MQTT] Calibrated EMPTY: %.2f kPa\n", pEmptyKPa);
    publishMqttStatus("Calibrated EMPTY: " + String(pEmptyKPa, 2) + " kPa");
  } 
  else if (cmd == "cal_full") {
    pFullKPa = filteredPressure;
    saveSettings();
    Serial.printf("[MQTT] Calibrated FULL: %.2f kPa\n", pFullKPa);
    publishMqttStatus("Calibrated FULL: " + String(pFullKPa, 2) + " kPa");
  } 
  else if (cmd == "manual_cal") {
    String empStr = getJsonField(msgStr, "p_empty");
    String fulStr = getJsonField(msgStr, "p_full");
    if (empStr.length() > 0 && fulStr.length() > 0) {
      float emp = empStr.toFloat();
      float ful = fulStr.toFloat();
      if (ful > emp) {
        pEmptyKPa = emp;
        pFullKPa = ful;
        saveSettings();
        publishMqttStatus("Cal Updated: " + String(pEmptyKPa, 1) + " to " + String(pFullKPa, 1) + " kPa");
      } else {
        publishMqttStatus("Cal Error: FULL must be > EMPTY");
      }
    }
  }
  else if (cmd == "save_settings") {
    String depthStr = getJsonField(msgStr, "depth");
    String capStr = getJsonField(msgStr, "cap");
    String urlStr = getJsonField(msgStr, "url");
    String usDistStr = getJsonField(msgStr, "us_full_dist");
    if (usDistStr.length() == 0) usDistStr = getJsonField(msgStr, "offset");
    if (depthStr.length() > 0) tankDepthCm = depthStr.toFloat();
    if (capStr.length() > 0) tankCapacityL = capStr.toFloat();
    if (urlStr.length() > 0) googleScriptUrl = urlStr;
    if (usDistStr.length() > 0) ultrasonicFullDistCm = usDistStr.toFloat();
    saveSettings();
    forwardConfigToNode2();
    publishMqttStatus("Settings Saved: Depth=" + String(tankDepthCm, 1) + "cm, Cap=" + String(tankCapacityL, 0) + "L, US Full Dist=" + String(ultrasonicFullDistCm, 1) + "cm");
  }
  else if (cmd == "log_sheets") {
    struct tm timeinfo;
    char timeBuf[30] = "2026-10-28 12:00:00";
    if (getLocalTime(&timeinfo)) {
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:00", &timeinfo);
    }
    bool ok = sendToGoogleSheets(String(timeBuf));
    publishMqttStatus(ok ? "Google Sheets Logged" : "Google Sheets Failed");
  } 
  else if (cmd == "check_ota") {
    cloudOTA.checkAndUpdate(false);
    publishMqttStatus("OTA: " + cloudOTA.getStatus());
  }
  else if (cmd == "update_ota_node1" || cmd == "update_ota") {
    publishMqttStatus("Node 1: Starting Immediate Cloud OTA...");
    Serial.println(F("[MQTT-CMD] ⚡ Starting Immediate Cloud OTA Update on Node 1..."));
    bool ok = cloudOTA.checkAndUpdate(true);
    publishMqttStatus("Node 1 OTA: " + cloudOTA.getStatus());
  }
  else if (cmd == "update_ota_node2") {
    publishMqttStatus("Forwarding immediate OTA to Node 2...");
    Serial.println(F("[MQTT-CMD] ⚡ Forwarding immediate OTA update to Node 2..."));
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    httpNode2.begin(clientNode2, "http://192.168.29.130/api/ota_update");
    httpNode2.setTimeout(4000);
    int code = httpNode2.GET();
    httpNode2.end();
    if (code > 0) {
      publishMqttStatus("Node 2 Immediate OTA Initiated!");
    } else {
      publishMqttStatus("Node 2 Unreachable (Offline or No Route)");
    }
  }
  else if (cmd == "update_ota_both") {
    publishMqttStatus("Starting Immediate Cloud OTA on Both Nodes...");
    Serial.println(F("[MQTT-CMD] ⚡ Starting Immediate Cloud OTA on Both Nodes..."));
    // 1. Trigger Node 2 first
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    httpNode2.begin(clientNode2, "http://192.168.29.130/api/ota_update");
    httpNode2.setTimeout(4000);
    int code2 = httpNode2.GET();
    httpNode2.end();
    if (code2 > 0) {
      publishMqttStatus("Node 2 OTA Initiated! Now flashing Node 1...");
    } else {
      publishMqttStatus("Node 2 Unreachable. Proceeding with Node 1 OTA...");
    }
    // 2. Trigger Node 1
    delay(500);
    cloudOTA.checkAndUpdate(true);
  }
  else if (cmd == "reboot") {
    publishMqttStatus("Rebooting Node 1...");
    delay(500);
    ESP.restart();
  }
  else if (cmd == "reboot_node2") {
    Serial.println(F("[MQTT-CMD] 🔄 Forwarding reboot command to Node 2 (Overhead Ultrasonic)..."));
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    httpNode2.begin(clientNode2, "http://192.168.29.130/api/reboot");
    httpNode2.setTimeout(2500);
    int code = httpNode2.GET();
    httpNode2.end();
    if (code > 0) {
      publishMqttStatus("Node 2 (Overhead Ultrasonic) Rebooting...");
    } else {
      publishMqttStatus("Node 2 Unreachable (Offline or No Route)");
    }
  }
  else if (cmd == "pump_on" || (cmd == "pump" && getJsonField(msgStr, "state") == "1")) {
    bool ok = sendEspNowPumpCommand(1, waterLevelPct);
    publishMqttStatus(ok ? "PUMP START Command Sent" : ("PUMP START Rejected: " + pumpLockoutReason));
  }
  else if (cmd == "pump_off" || (cmd == "pump" && getJsonField(msgStr, "state") == "0")) {
    bool ok = sendEspNowPumpCommand(0, waterLevelPct);
    publishMqttStatus(ok ? "PUMP STOP Command Sent" : "PUMP STOP Failed");
  }
  else if (cmd == "pump_auto") {
    String en = getJsonField(msgStr, "enable");
    if (en.length() > 0) autoPumpControlEnabled = (en.toInt() == 1);
    publishMqttStatus("Auto Pump: " + String(autoPumpControlEnabled ? "ENABLED" : "DISABLED"));
  }
}

// Publish 5 Hz Real-Time Telemetry to Global MQTT Broker
void publishMqttTelemetry() {
  if (!mqttClient.connected()) return;
  static unsigned long lastMqttPub = 0;
  if (millis() - lastMqttPub < 200) return; // 5 Hz rate limit (200 ms)
  lastMqttPub = millis();

  char payload[512];
  const char* st = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                   (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";

  snprintf(payload, sizeof(payload),
           "{\"p\":%.2f,\"l\":%.1f,\"v\":%.3f,\"d\":%.2f,\"s\":\"%s\","
           "\"h\":%.2f,\"vol\":%.0f,\"adc\":%d,"
           "\"pump\":%s,\"p_lock\":%s,"
           "\"rec\":%s,\"rec_s\":%.1f,\"dur\":%.0f,\"samples\":%d,\"evt\":\"%s\","
           "\"u_d\":%.1f,\"u_l\":%.1f,\"u_f\":%.1f,"
           "\"up_pct\":%d,"
           "\"p_emp\":%.1f,\"p_ful\":%.1f,"
           "\"depth\":%.1f,\"cap\":%.0f,"
           "\"ver\":\"%s\"}",
           filteredPressure, waterLevelPct, sensorVoltage, dPdt, st,
           waterHeadM, waterVolumeL, (int)rawAdcAvg,
           pumpActiveTracked ? "true" : "false", pumpSafetyLockout ? "true" : "false",
           eventRecorder.isRecording() ? "true" : "false",
           eventRecorder.getElapsedSec(), eventRecorder.getDurationSec(),
           eventRecorder.getSamplesCount(), eventRecorder.getEventName().c_str(),
           eventRecorder.getRooftopDist(), eventRecorder.getRooftopLevel(), ultrasonicFullDistCm,
           eventRecorder.getUploadProgressPct(),
           pEmptyKPa, pFullKPa,
           tankDepthCm, tankCapacityL,
           FIRMWARE_VERSION);

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

  // Note: maintainWiFi() runs asynchronously on Core 0 via wifiMaintainerTask!
  // Core 1 loop is completely decoupled and never stalls during Wi-Fi disconnects.

  server.handleClient();
  maintainMqtt();

  // Process chunked background upload of high-res event data to Sheet 2
  eventRecorder.processChunkedUpload(googleScriptUrl);

  unsigned long currentMillis = millis();

  // Sample sensor and update dynamic state machine at 10 Hz (every 100 ms)
  if (currentMillis - lastSampleTime >= 100) {
    float dt = (currentMillis - lastSampleTime) / 1000.0f;
    lastSampleTime += 100; // Exact phase accumulation (eliminates sampling jitter and sample drop)
    if (currentMillis - lastSampleTime > 500) {
      lastSampleTime = currentMillis; // Overflow guard
    }

    sampleSensors();
    updateFlowStateMachine();
    calculateWaterLevel();
    publishMqttTelemetry(); // Broadcast 10 Hz stream to global cloud

    // High-Resolution Event Capture at 10 Hz (100 ms precision)
    const char* stStr = (tapState == STATE_TAP_OPEN) ? "TAP_OPEN" :
                        (tapState == STATE_SETTLING) ? "SETTLING" : "STATIC";
    eventRecorder.captureSample(sensorVoltage, filteredPressure, dPdt, stStr, waterLevelPct, flowRateLps, totalDischargedLiters);
  }

  // Check NTP clock and synchronized 10-minute Google Sheets logging
  handleNtpAndLogging();
}

// =========================================================================
// SENSOR SAMPLING & DIRECT ADC READING (NO DIGITAL FILTERS)
// =========================================================================
void sampleSensors() {
  if (usingExternalAdc) {
    // -----------------------------------------------------------------------
    // A. 16-BIT PRECISION SAMPLING VIA EXTERNAL ADS1115 I2C ADC (DIRECT HARDWARE)
    // -----------------------------------------------------------------------
    float vRead = ads.readVoltage(ADS1115_PRESSURE_CH);
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
    // B. FALLBACK: 12-BIT INTERNAL ESP32 ADC (GPIO 34) DIRECT SAMPLING
    // -----------------------------------------------------------------------
    rawAdcAvg = (float)analogRead(PIN_PRESSURE_ADC);
    float vPin = (rawAdcAvg / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
    sensorVoltage = vPin / VOLTAGE_DIVIDER_RATIO;
  }

  // 5. Convert Sensor Voltage to Gauge Pressure (kPa)
  // Standard 0.2 MPa sensor: 0.5V = 0 kPa, 4.5V = 200 kPa
  if (sensorVoltage < SENSOR_MIN_V) {
    currentPressure = 0.0f;
  } else {
    currentPressure = (sensorVoltage - SENSOR_MIN_V) * (SENSOR_MAX_KPA / (SENSOR_MAX_V - SENSOR_MIN_V));
  }

  // Direct Gauge Output directly from ADC (No digital smoothing, EMA, or deadband)
  filteredPressure = currentPressure;
}

// =========================================================================
// ADVANCED DUAL-CONDITION HYDRAULIC FLOW & WATER HAMMER CLASSIFIER
// =========================================================================
void updateFlowStateMachine() {
  // 1. Instantaneous time derivative dP/dt in kPa/s (dt = 100 ms, direct from ADC)
  float instantDpH = (filteredPressure - prevPressure) / 0.10f;
  prevPressure = filteredPressure;

  // Direct instantaneous derivative from ADC
  dPdt = instantDpH;

  unsigned long now = millis();

  switch (tapState) {
    case STATE_STATIC: {
      // Evaluate pressure depression below baseline BEFORE baseline update
      float deltaP = lastStaticPressure - filteredPressure;

      // Condition A: Fast Dynamic Drop (sudden tap wrench/valve open)
      // Requires dP/dt < -2.2 kPa/s for >= 2 consecutive frames (200 ms)
      if (dPdt < -2.2f) {
        dropStreak++;
      } else {
        dropStreak = 0;
      }

      // Condition B: Slow Valve Turn (gentle knob turn over 1-2 seconds)
      // Absolute pressure depression > 2.5 kPa below baseline for >= 4 frames (400 ms)
      if (deltaP > 2.5f) {
        sustainedLowCount++;
      } else {
        sustainedLowCount = 0;
      }

      // Trigger Tap Open on either condition
      if (dropStreak >= 2 || sustainedLowCount >= 4) {
        tapState = STATE_TAP_OPEN;
        stateChangeTime = now;
        baselineAtDischarge = constrain(lastStaticPressure, pEmptyKPa, pFullKPa + 2.0f);
        frozenLevelPct = waterLevelPct;
        totalDischargedLiters = 0.0f;
        dropStreak = 0;
        sustainedLowCount = 0;
        digitalWrite(PIN_STATUS_LED, HIGH);
        Serial.printf("[FLOW-ENG] Dynamic Tap Discharge Confirmed! Baseline=%.2f kPa. Level=%.1f%%\n",
                      baselineAtDischarge, frozenLevelPct);
      } else {
        // Continuous equilibrium tracking: only adapt baseline when pressure is stable (quiescent)
        if (fabsf(dPdt) < 0.6f && deltaP < 1.5f && deltaP > -1.5f) {
          lastStaticPressure = 0.95f * lastStaticPressure + 0.05f * filteredPressure;
        }
      }
      break;
    }

    case STATE_TAP_OPEN: {
      // Dynamic Head Loss & Flow Rate Integration
      float dynamicHeadLoss = fmaxf(0.0f, baselineAtDischarge - filteredPressure);
      flowRateLps = 0.045f * sqrtf(fmaxf(0.0f, dynamicHeadLoss)); // ~8-12 L/min
      totalDischargedLiters += flowRateLps * 0.10f; // Integrated every 100 ms

      // Valve Closure Detection:
      // A. Acoustic Water Hammer Surge (dP/dt > 3.5 kPa/s after 300 ms)
      bool hammerShock = (dPdt > 3.5f) && (now - stateChangeTime > 300);
      // B. Pressure Returns near Baseline
      bool backToBaseline = (filteredPressure >= (baselineAtDischarge - 1.5f)) && (now - stateChangeTime > 600);
      // C. Pressure Quiescence: Fluid at rest (dP/dt ~ 0 for 1.5s)
      static int quiescentCount = 0;
      if (fabsf(dPdt) < 0.6f && (now - stateChangeTime > 1500)) {
        quiescentCount++;
      } else {
        quiescentCount = 0;
      }
      bool flowStopped = (quiescentCount >= 15); // 1.5 seconds of static pressure
      // D. Safety Max Duration: Continuous 45s without new negative drop
      bool timeoutExit = (now - stateChangeTime > 45000);

      if (hammerShock || backToBaseline || flowStopped || timeoutExit) {
        tapState = STATE_SETTLING;
        flowRateLps = 0.0f;
        stateChangeTime = now;
        peakHammerPressure = filteredPressure;
        quiescentCount = 0;
        Serial.printf("[FLOW-ENG] Valve Closed (Shock=%d, Recov=%d, Quiescent=%d). Entering Joukowsky Damping.\n",
                      hammerShock, backToBaseline, flowStopped);
      }
      break;
    }

    case STATE_SETTLING: {
      flowRateLps = 0.0f;
      if (filteredPressure > peakHammerPressure) {
        peakHammerPressure = filteredPressure;
      }

      // Hydraulic Settling Delay (3 seconds for acoustic waves to damp out)
      if (now - stateChangeTime >= SETTLING_DELAY_MS) {
        tapState = STATE_STATIC;
        hammerDeltaKPa = fmaxf(0.0f, peakHammerPressure - filteredPressure);
        lastStaticPressure = constrain(filteredPressure, pEmptyKPa, pFullKPa + 2.0f);
        smoothedStaticPressure = lastStaticPressure;
        digitalWrite(PIN_STATUS_LED, LOW);
        Serial.printf("[HAMMER-STUDY] Equilibrium Restored: P=%.2f kPa (Surge=+%.2f kPa). Resuming Static Hydrostatic Tracking.\n",
                      lastStaticPressure, hammerDeltaKPa);
      }
      break;
    }
  }
}

// =========================================================================
// HYDROSTATIC WATER LEVEL ESTIMATION (ULTRA-STABLE DUAL-PATH)
// =========================================================================
void calculateWaterLevel() {
  // Initialize smoothed static pressure on boot
  if (smoothedStaticPressure <= 0.0f && filteredPressure > 0.0f) {
    smoothedStaticPressure = filteredPressure;
  }

  // Path B: Low-Pass Exponential Moving Average (EMA, alpha = 0.08) for static water level display.
  // Filters out 92% of high-frequency electronic ADC jitter and fluid ripples per sample,
  // stabilizing the display without compromising fast-path transient detection.
  if (tapState == STATE_STATIC) {
    smoothedStaticPressure = 0.92f * smoothedStaticPressure + 0.08f * filteredPressure;
  } else {
    smoothedStaticPressure = baselineAtDischarge;
  }

  // Under STATIC conditions: use smoothed hydrostatic pressure
  // Under TAP_OPEN or SETTLING: use baselineAtDischarge to prevent false dynamic plunge & hammer spikes
  float pEval = (tapState == STATE_STATIC) ? smoothedStaticPressure : baselineAtDischarge;

  // Two-Point Hydrostatic Formulation:
  // Level % = (P_eval - P_empty) / (P_full - P_empty) * 100%
  float targetPct = 0.0f;
  if (pFullKPa > pEmptyKPa) {
    float rawPct = ((pEval - pEmptyKPa) / (pFullKPa - pEmptyKPa)) * 100.0f;
    targetPct = constrain(rawPct, 0.0f, 100.0f);
  }

  // Direct water level percentage: lock level under TAP_OPEN and SETTLING minus actual volumetric drawdown
  if (tapState == STATE_STATIC) {
    waterLevelPct = targetPct;
  } else {
    float drawdownPct = (tankCapacityL > 0.0f) ? ((totalDischargedLiters / tankCapacityL) * 100.0f) : 0.0f;
    waterLevelPct = constrain(frozenLevelPct - drawdownPct, 0.0f, 100.0f);
  }

  // Available Volume in Liters
  waterVolumeL = (waterLevelPct / 100.0f) * tankCapacityL;

  // Hydraulic head in meters: 1 kPa ~= 0.10197 m H2O
  waterHeadM = pEval * 0.10197f;

  // Dynamic Master Role: Autonomous Tank Level Pump Control
  if (autoPumpControlEnabled && tapState == STATE_STATIC) {
    unsigned long now = millis();
    // Start Pump: Water level dropped below start threshold and pump not tracked ON
    if (!pumpActiveTracked && waterLevelPct <= pumpAutoStartThreshold && (now - lastPumpCmdTime > 15000UL)) {
      Serial.printf("[AUTO-PUMP] Water level low (%.1f%% <= %.1f%%). Sending START command...\n",
                    waterLevelPct, pumpAutoStartThreshold);
      sendEspNowPumpCommand(1, waterLevelPct);
    }
    // Stop Pump: Tank filled to stop threshold and pump tracked ON
    else if (pumpActiveTracked && waterLevelPct >= pumpAutoStopThreshold && (now - lastPumpCmdTime > 15000UL)) {
      Serial.printf("[AUTO-PUMP] Tank full (%.1f%% >= %.1f%%). Sending STOP command...\n",
                    waterLevelPct, pumpAutoStopThreshold);
      sendEspNowPumpCommand(0, waterLevelPct);
    }
  }
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
  pEmptyKPa            = preferences.getFloat("p_empty", DEFAULT_P_EMPTY_KPA);
  pFullKPa             = preferences.getFloat("p_full", DEFAULT_P_FULL_KPA);
  tankDepthCm          = preferences.getFloat("tank_depth", DEFAULT_TANK_DEPTH_CM);
  if (tankDepthCm >= 149.0f && tankDepthCm <= 151.0f) {
    tankDepthCm = DEFAULT_TANK_DEPTH_CM; // Update legacy 150cm default to 122cm
  }
  tankCapacityL        = preferences.getFloat("tank_cap", DEFAULT_TANK_CAPACITY_L);
  if (tankCapacityL >= 1990.0f && tankCapacityL <= 2010.0f) {
    tankCapacityL = DEFAULT_TANK_CAPACITY_L; // Automatically migrate legacy 2000L flash NVS value to 1000L
  }
  ultrasonicFullDistCm = preferences.getFloat("us_full_dist", DEFAULT_ULTRASONIC_FULL_DISTANCE_CM);
  if (ultrasonicFullDistCm >= 10.5f && ultrasonicFullDistCm <= 11.5f) {
    ultrasonicFullDistCm = DEFAULT_ULTRASONIC_FULL_DISTANCE_CM; // Update legacy 11cm default to 6cm
  }
  googleScriptUrl      = preferences.getString("script_url", GOOGLE_SCRIPT_URL);
  String savedToken    = preferences.getString("gh_token", DEFAULT_GITHUB_OTA_TOKEN);
  cloudOTA.setToken(savedToken);
  preferences.end();

  Serial.println(F("[NVS] Configuration loaded from Flash:"));
  Serial.printf("      P_empty: %.2f kPa | P_full: %.2f kPa\n", pEmptyKPa, pFullKPa);
  Serial.printf("      Tank Depth: %.1f cm | Tank Capacity: %.0f L | US Full Dist: %.1f cm\n",
                tankDepthCm, tankCapacityL, ultrasonicFullDistCm);
}

void saveSettings() {
  preferences.begin("smart_tank", false); // Read-write mode
  preferences.putFloat("p_empty", pEmptyKPa);
  preferences.putFloat("p_full", pFullKPa);
  preferences.putFloat("tank_depth", tankDepthCm);
  preferences.putFloat("tank_cap", tankCapacityL);
  preferences.putFloat("us_full_dist", ultrasonicFullDistCm);
  preferences.putString("script_url", googleScriptUrl);
  preferences.end();

  Serial.println(F("[NVS] Configuration successfully saved to Flash NVS!"));
}

void forwardConfigToNode2() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    String node2Url = "http://192.168.29.130/api/settings?offset_cm=" + String(ultrasonicFullDistCm, 1) + "&depth_cm=" + String(tankDepthCm, 1);
    httpNode2.begin(clientNode2, node2Url);
    httpNode2.setTimeout(2500);
    int code = httpNode2.GET();
    httpNode2.end();
    Serial.printf("[SYNC-NODE2] Forwarded settings to Node 2 (HTTP %d: offset=%.1f cm, depth=%.1f cm)\n",
                  code, ultrasonicFullDistCm, tankDepthCm);
  }
}

// =========================================================================
// ASYNCHRONOUS AUTO-RECONNECTING WI-FI MANAGER (CORE 0 FREERTOS TASK)
// =========================================================================
TaskHandle_t wifiTaskHandle = NULL;

void wifiMaintainerTask(void *pvParameters) {
  unsigned long lastWifiCheck = 0;
  for (;;) {
    unsigned long now = millis();

    // 1. Maintain Wi-Fi Connection
    if (now - lastWifiCheck >= 5000UL) {
      lastWifiCheck = now;
      maintainWiFi();
    }

    // 2. Broadcast Cluster Heartbeat (feeds Node 3 10-second safety watchdog)
    broadcastClusterHeartbeat();

    // 3. Process Pending Gateway Cloud Forwarding for Offline Peers
    if (pendingProxyChunk.hasData && isElectedGateway && (WiFi.status() == WL_CONNECTED)) {
      Serial.printf("[GATEWAY Core0] Uploading Chunk #%u from Node %u (%u records) to Google Sheets...\n",
                    pendingProxyChunk.chunkId, pendingProxyChunk.senderId, pendingProxyChunk.recordCount);
      bool ok = postBulkJsonToGoogleSheets("{\"node\":\"proxy_" + String(pendingProxyChunk.senderId) + "\",\"raw\":\"" + String(pendingProxyChunk.csvRows) + "\"}");
      ClusterPacket ackPkt;
      memset(&ackPkt, 0, sizeof(ClusterPacket));
      ackPkt.magic = CLUSTER_MAGIC;
      ackPkt.senderId = NODE_ID_TAP_PRESSURE;
      ackPkt.msgType = MSG_BULK_DATA_ACK;
      ackPkt.payload.bulkAck.ackChunkId = pendingProxyChunk.chunkId;
      ackPkt.payload.bulkAck.httpStatus = ok ? 200 : 500;
      esp_now_send(pendingProxyChunk.senderMac, (uint8_t*)&ackPkt, sizeof(ackPkt));
      pendingProxyChunk.hasData = false;
      Serial.printf("[GATEWAY Core0] Chunk #%u forward result: %s\n", pendingProxyChunk.chunkId, ok ? "SUCCESS" : "FAILED");
    }

    vTaskDelay(pdMS_TO_TICKS(3000)); // 3-second cycle (well within 10s watchdog)
  }
}

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
    initEspNowSender();
  } else {
    Serial.println(F("\n[WIFI] Initial connect timed out. Continuous asynchronous background maintainWiFi() active on Core 0."));
  }

  // Spawn dedicated asynchronous Wi-Fi maintainer on Core 0
  // Completely isolates Core 1 (ADC sampling & FSM) from any network blocking latency!
  xTaskCreatePinnedToCore(
    wifiMaintainerTask,
    "WiFiMaintainer",
    4096,
    NULL,
    1,
    &wifiTaskHandle,
    0 // Pinned strictly to Core 0
  );
}

void maintainWiFi() {
  static unsigned long disconnectStart = 0;
  static bool wasConnected = false;
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      wasConnected = true;
      disconnectStart = 0;
      Serial.printf("\n[WIFI Core0] Wi-Fi Restored! IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER); // Resync NTP
      initEspNowSender(); // Re-sync ESP-NOW peer to current Wi-Fi channel
      uploadBufferedOfflineLogs(); // Immediately drain offline backlog!
    }
    return;
  }

  // Not connected
  if (wasConnected) {
    wasConnected = false;
    disconnectStart = now;
    Serial.println(F("\n[WIFI Core0] WARNING: Wi-Fi dropped! Storing telemetry in Flash memory..."));
  }

  unsigned long offlineSec = (disconnectStart > 0) ? ((now - disconnectStart) / 1000) : 0;
  Serial.printf("[WIFI Core0] Auto-reconnecting to '%s' (Offline: %lu s | Queued: %d records)...\n",
                DEFAULT_WIFI_SSID, offlineSec, getOfflineQueueCount());

  // If disconnected for > 3 minutes, perform a full hardware radio re-init
  if (offlineSec > 180) {
    Serial.println(F("[WIFI Core0] Performing clean Wi-Fi radio reset..."));
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));
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
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
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
  json += "\"us_full_dist\":" + String(ultrasonicFullDistCm, 1) + ",";
  json += "\"offset_cm\":" + String(ultrasonicFullDistCm, 1) + ",";
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
  json += "\"rooftop_level\":" + String(eventRecorder.getRooftopLevel(), 1) + ",";
  json += "\"rooftop_dist\":" + String(eventRecorder.getRooftopDist(), 1) + ",";
  json += "\"ota_status\":\"" + cloudOTA.getStatus() + "\",";
  json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"event_recording\":" + String(eventRecorder.isRecording() ? "true" : "false") + ",";
  json += "\"event_uploading\":" + String(eventRecorder.isUploading() ? "true" : "false") + ",";
  json += "\"event_name\":\"" + eventRecorder.getEventName() + "\",";
  json += "\"event_id\":\"" + eventRecorder.getEventId() + "\",";
  json += "\"event_elapsed\":" + String(eventRecorder.getElapsedSec(), 1) + ",";
  json += "\"event_duration\":" + String(eventRecorder.getDurationSec(), 1) + ",";
  json += "\"event_samples\":" + String(eventRecorder.getSamplesCount()) + ",";
  json += "\"event_upload_pct\":" + String(eventRecorder.getUploadProgressPct()) + ",";
  json += "\"event_upload_status\":\"" + eventRecorder.getUploadStatus() + "\",";
  json += "\"is_elected_gateway\":" + String(isElectedGateway ? "true" : "false") + ",";
  json += "\"peer_pump_rssi\":" + String(peerPumpRssi) + ",";
  json += "\"peer_pump_channel\":" + String(peerPumpChannel) + ",";
  json += "\"pump_active_tracked\":" + String(pumpActiveTracked ? "true" : "false") + ",";
  json += "\"pump_auto_enabled\":" + String(autoPumpControlEnabled ? "true" : "false") + ",";
  json += "\"pump_safety_lockout\":" + String(pumpSafetyLockout ? "true" : "false") + ",";
  json += "\"pump_lockout_reason\":\"" + pumpLockoutReason + "\"";
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
  if (server.hasArg("us_full_dist")) {
    ultrasonicFullDistCm = server.arg("us_full_dist").toFloat();
  } else if (server.hasArg("offset_cm")) {
    ultrasonicFullDistCm = server.arg("offset_cm").toFloat();
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
  forwardConfigToNode2();
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
  cloudOTA.checkAndUpdate(false);
  String json = "{\"status\":\"success\",\"message\":\"" + cloudOTA.getStatus() + "\"}";
  server.send(200, "application/json", json);
}

void handleOtaUpdateApi() {
  int target = server.hasArg("node") ? server.arg("node").toInt() : 0;
  Serial.printf("[HTTP] ⚡ Immediate Cloud OTA Update triggered (target node: %d)\n", target);

  if (target == 2) {
    // Forward to Node 2
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    httpNode2.begin(clientNode2, "http://192.168.29.130/api/ota_update");
    httpNode2.setTimeout(4000);
    int code = httpNode2.GET();
    httpNode2.end();
    if (code > 0) {
      server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Node 2 (Ultrasonic) Immediate Cloud OTA Initiated!\"}");
    } else {
      server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Node 2 Unreachable (Offline or No Route)\"}");
    }
  } else if (target == 0) {
    // Both nodes: Notify Node 2 first, then flash Node 1
    HTTPClient httpNode2;
    WiFiClient clientNode2;
    httpNode2.begin(clientNode2, "http://192.168.29.130/api/ota_update");
    httpNode2.setTimeout(4000);
    httpNode2.GET();
    httpNode2.end();

    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Immediate Cloud OTA Initiated on Both Nodes! Flashing in progress...\"}");
    delay(300);
    cloudOTA.checkAndUpdate(true);
  } else {
    // Node 1 only
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Node 1 (Pressure) Immediate Cloud OTA Initiated! Flashing in progress...\"}");
    delay(300);
    cloudOTA.checkAndUpdate(true);
  }
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

  eventRecorder.setScriptUrl(googleScriptUrl);
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
