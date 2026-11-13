#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "config.h"
#include "cluster_protocol.h"

// =========================================================================
// MQTT GLOBAL CLOUD MESSAGING (FOR GITHUB PAGES SECURE WEBSOCKET CONTROL)
// =========================================================================
WiFiClient espMqttClient;
PubSubClient mqttClient(espMqttClient);

const char* MQTT_BROKER            = "broker.hivemq.com";
const uint16_t MQTT_PORT           = 1883;
const char* MQTT_TOPIC_COMMAND     = "smarttank/dissertation/tap/cmd";
const char* MQTT_TOPIC_PUMP_CMD    = "smarttank/dissertation/pump/cmd";
const char* MQTT_TOPIC_STATUS      = "smarttank/dissertation/tap/status";
const char* MQTT_TOPIC_PUMP_STATUS = "smarttank/dissertation/pump/status";
unsigned long lastMqttRetry        = 0;
unsigned long lastMqttStatusPub    = 0;

void publishMqttPumpStatus(const String& reason = "");
void onMqttPumpMessage(char* topic, byte* payload, unsigned int length);
void maintainMqtt();

// =========================================================================
// PEER HARDWARE ADDRESSES & DYNAMIC DISCOVERY
// =========================================================================
// Default fallback MACs (auto-updated dynamically on first received packet)
uint8_t tapNodeMac[6]   = {0x28, 0x05, 0xA5, 0x25, 0x9D, 0x5C}; // Node 1 (Tap Pressure Node)
uint8_t roofNodeMac[6]  = {0xE0, 0x8C, 0xFE, 0x32, 0x98, 0x7C}; // Node 2 (Rooftop Ultrasonic)
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6]        = {0};

// =========================================================================
// STATE VARIABLES
// =========================================================================
bool pumpActive = false;
unsigned long pumpStartTime = 0;
unsigned long pumpStopTime = 0;
unsigned long lastStateChangeTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastBroadcastTime = 0;
unsigned long lastWifiCheck = 0;
unsigned long wifiDisconnectStart = 0;
uint8_t lastKnownWifiChannel = 1;

uint16_t lastExecutedSeq = 0;
float lastReportedLevel = 0.0f;
String lastShutdownReason = "Startup Initialized";

// Anti-Chatter Deadband: Minimum 10 seconds before opposing relay toggle allowed
const unsigned long ANTI_CHATTER_DEADBAND_MS = 10000UL;

// Dynamic Multi-Role / Gateway Election State
bool isElectedGateway = false;
int8_t peerTapRssi = -127;
unsigned long gatewayClaimDebounceStart = 0;

struct PendingBulkChunk {
    bool hasData;
    uint8_t senderMac[6];
    uint8_t senderId;
    uint16_t chunkId;
    uint8_t recordCount;
    char csvRows[160];
};
PendingBulkChunk pendingProxyChunk = {false, {0}, 0, 0, 0, ""};

WebServer server(80);

// =========================================================================
// FUNCTION DECLARATIONS
// =========================================================================
void setRelay(bool state, const String& reason);
void broadcastHeartbeat();
void broadcastSafetyOverride(uint8_t faultCode);
bool forwardChunkToGoogleSheets(const String& csvPayload);
void ensurePeerExists(const uint8_t* peerMac);
void updateStatusLed();
void maintainWiFi();
void handleRootHtml();
void handleStatusApi();

// =========================================================================
// RELAY HARDWARE DRIVER & ACTIVE STATUS SIGNALING
// =========================================================================
void setRelay(bool state, const String& reason) {
    if (pumpActive == state) return;

    unsigned long now = millis();
    // Enforce anti-chatter restart cooldown: NEVER delay or reject a STOP command!
    if (state && (now - lastStateChangeTime < ANTI_CHATTER_DEADBAND_MS) && lastStateChangeTime != 0) {
        Serial.printf("[RELAY] Turn-on rejected: Anti-chatter cooldown active (%lu ms left)\n",
                      ANTI_CHATTER_DEADBAND_MS - (now - lastStateChangeTime));
        return;
    }

    pumpActive = state;
    lastStateChangeTime = now;

    if (state) {
        digitalWrite(PIN_RELAY, RELAY_ACTIVE_LEVEL);
        pumpStartTime = now;
        lastHeartbeatTime = now;
        Serial.printf("[PUMP] >>> RELAY ENERGIZED (ON) <<< Reason: %s\n", reason.c_str());
    } else {
        digitalWrite(PIN_RELAY, (RELAY_ACTIVE_LEVEL == LOW) ? HIGH : LOW);
        pumpStopTime = now;
        lastShutdownReason = reason;
        unsigned long runtimeSec = (now - pumpStartTime) / 1000;
        Serial.printf("[PUMP] --- RELAY DE-ENERGIZED (OFF) --- Reason: %s (Ran for %lu s)\n", 
                      reason.c_str(), runtimeSec);
    }
    publishMqttPumpStatus(reason);
}

// Built-in Status LED Active Signaling Routine
void updateStatusLed() {
    static unsigned long lastToggle = 0;
    static bool ledState = false;
    unsigned long now = millis();

    // 1. Pump Active -> Continuous warning signaling cadence (250ms ON / 250ms OFF)
    if (pumpActive) {
        if (now - lastToggle >= 250) {
            lastToggle = now;
            ledState = !ledState;
            digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
        }
        return;
    }

    // 2. Cooldown Active after shutoff -> Fast warning strobe (100ms ON / 100ms OFF)
    if (now - pumpStopTime < MIN_COOLDOWN_MS && pumpStopTime != 0) {
        if (now - lastToggle >= 100) {
            lastToggle = now;
            ledState = !ledState;
            digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
        }
        return;
    }

    // 3. Normal Standby / Healthy -> Calm heartbeat flash (60ms pulse every 3 seconds)
    unsigned long cycle = now % 3000;
    if (cycle < 60) {
        digitalWrite(PIN_STATUS_LED, HIGH);
    } else {
        digitalWrite(PIN_STATUS_LED, LOW);
    }
}

// =========================================================================
// ESP-NOW PEER MANAGEMENT
// =========================================================================
void ensurePeerExists(const uint8_t* peerMac) {
    if (!esp_now_is_peer_exist(peerMac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, peerMac, 6);
        peer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : lastKnownWifiChannel;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) == ESP_OK) {
            Serial.printf("[ESP-NOW] Auto-registered peer: %02X:%02X:%02X:%02X:%02X:%02X on Channel %d\n",
                          peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5], peer.channel);
        }
    }
}

// =========================================================================
// ESP-NOW RECEIVER CALLBACK (ROLE-BASED COOPERATIVE DISPATCHER)
// =========================================================================
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(ClusterPacket)) return;

    ClusterPacket pkt;
    memcpy(&pkt, data, sizeof(ClusterPacket));
    if (pkt.magic != CLUSTER_MAGIC) return;

    ensurePeerExists(mac);
    unsigned long now = millis();

    switch (pkt.msgType) {

        // CASE 1: PUMP COMMAND FROM NODE 1 (SLAVE ROLE)
        case MSG_PUMP_COMMAND: {
            if (memcmp(tapNodeMac, mac, 6) != 0) {
                memcpy(tapNodeMac, mac, 6);
            }

            if (pkt.seqNumber <= lastExecutedSeq && (lastExecutedSeq - pkt.seqNumber < 30000)) {
                return; // Drop duplicate/stale packet
            }
            lastExecutedSeq = pkt.seqNumber;
            lastReportedLevel = pkt.payload.pump.tankLevelPct;
            lastHeartbeatTime = now; // Reset watchdog timer

            if (pkt.payload.pump.command == 1) {
                if (!pumpActive && (now - pumpStopTime < MIN_COOLDOWN_MS) && pumpStopTime != 0) {
                    unsigned long waitLeft = (MIN_COOLDOWN_MS - (now - pumpStopTime)) / 1000;
                    Serial.printf("[PUMP] Start rejected: Motor cooling down (%lu s left)\n", waitLeft);
                    return;
                }
                setRelay(true, "ESP-NOW Command from Node 1 (Level: " + String(lastReportedLevel, 1) + "%)");
            } else {
                setRelay(false, "ESP-NOW Routine Stop from Node 1");
            }
            break;
        }

        // CASE 2: ROOFTOP DIRECT OVERFLOW STOP FROM NODE 2
        case MSG_ROOF_OVERFLOW_STOP: {
            Serial.println("[PUMP] 🚨 DIRECT OVERFLOW INTERRUPT FROM NODE 2 (ROOFTOP)!");
            setRelay(false, "SAFETY: Node 2 Rooftop Overflow Cutoff");
            break;
        }

        // CASE 3: ROOFTOP GROUND TRUTH TELEMETRY FROM NODE 2
        case MSG_ROOF_TELEMETRY: {
            if (memcmp(roofNodeMac, mac, 6) != 0) {
                memcpy(roofNodeMac, mac, 6);
            }
            lastReportedLevel = pkt.payload.roofTelemetry.waterLevelPct;
            if (lastReportedLevel >= 98.0f && pumpActive) {
                setRelay(false, "SAFETY: Water Level >= 98% (Node 2 Ultrasonic)");
            }
            break;
        }

        // CASE 4: PEER HEARTBEAT & GATEWAY ELECTION
        case MSG_CLUSTER_HEARTBEAT: {
            if (pkt.senderId == NODE_ID_TAP_PRESSURE) {
                if (memcmp(tapNodeMac, mac, 6) != 0) {
                    memcpy(tapNodeMac, mac, 6);
                }
                lastHeartbeatTime = now; // Reset watchdog timer
                peerTapRssi = pkt.wifiRssi;

                int8_t myRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
                bool shouldIBeGateway = evaluateGatewaySuperiority(myRssi, peerTapRssi, myMac, pkt.senderMac);

                if (shouldIBeGateway != isElectedGateway) {
                    if (gatewayClaimDebounceStart == 0) {
                        gatewayClaimDebounceStart = now;
                    } else if (now - gatewayClaimDebounceStart >= 15000UL) {
                        isElectedGateway = shouldIBeGateway;
                        gatewayClaimDebounceStart = 0;
                        Serial.printf("[CLUSTER] Gateway Role Changed: %s (My RSSI: %d, Peer: %d)\n",
                                      isElectedGateway ? "I AM GATEWAY" : "PEER IS GATEWAY", myRssi, peerTapRssi);
                    }
                } else {
                    gatewayClaimDebounceStart = 0;
                }
            }
            break;
        }

        // CASE 5: BULK DATA FORWARDING (MASTER GATEWAY ROLE)
        case MSG_BULK_DATA_CHUNK: {
            if (isElectedGateway && WiFi.status() == WL_CONNECTED && !pendingProxyChunk.hasData) {
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

// =========================================================================
// CLOUD FORWARDER FOR BULK CHUNKS (HTTPS GOOGLE SHEETS)
// =========================================================================
bool forwardChunkToGoogleSheets(const String& csvPayload) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    bool success = false;
    if (http.begin(client, GOOGLE_SCRIPT_URL)) {
        http.addHeader("Content-Type", "application/json");
        String json = "{\"node\":\"pump_gateway\",\"proxy\":true,\"data\":\"" + csvPayload + "\"}";
        int code = http.POST(json);
        success = (code == 200 || code == 302);
        http.end();
    }
    client.stop();
    return success;
}

// =========================================================================
// BROADCAST FUNCTIONS
// =========================================================================
void broadcastHeartbeat() {
    static uint16_t hbSeq = 1;
    ClusterPacket pkt;
    memset(&pkt, 0, sizeof(ClusterPacket));
    pkt.magic = CLUSTER_MAGIC;
    pkt.senderId = NODE_ID_PUMP_RELAY;
    pkt.msgType = MSG_CLUSTER_HEARTBEAT;
    pkt.seqNumber = hbSeq++;
    pkt.wifiChannel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : lastKnownWifiChannel;
    pkt.wifiRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
    pkt.isGateway = isElectedGateway ? 1 : 0;
    memcpy(pkt.senderMac, myMac, 6);

    esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
}

void broadcastSafetyOverride(uint8_t faultCode) {
    ClusterPacket pkt;
    memset(&pkt, 0, sizeof(ClusterPacket));
    pkt.magic = CLUSTER_MAGIC;
    pkt.senderId = NODE_ID_PUMP_RELAY;
    pkt.msgType = MSG_SAFETY_OVERRIDE;
    pkt.payload.safety.faultCode = faultCode;
    pkt.payload.safety.lockoutSec = MIN_COOLDOWN_MS / 1000;
    memcpy(pkt.senderMac, myMac, 6);

    esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("[SAFETY MASTER] Broadcasted Emergency Override! Fault Code: %d\n", faultCode);
}

// =========================================================================
// WI-FI CONNECTION MAINTENANCE & CHANNEL LOCKING
// =========================================================================
void maintainWiFi() {
    unsigned long now = millis();
    if (now - lastWifiCheck < 5000) return;
    lastWifiCheck = now;

    if (WiFi.status() == WL_CONNECTED) {
        lastKnownWifiChannel = WiFi.channel();
        wifiDisconnectStart = 0;
        return;
    }

    if (wifiDisconnectStart == 0) wifiDisconnectStart = now;
    unsigned long offlineSec = (now - wifiDisconnectStart) / 1000;

    Serial.printf("[WIFI] Node 3 Auto-reconnecting to '%s' (Offline: %lu s)...\n", DEFAULT_WIFI_SSID, offlineSec);

    if (offlineSec > 180) {
        Serial.println(F("[WIFI] Clean Wi-Fi radio reset..."));
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
        wifiDisconnectStart = now;
    } else {
        WiFi.reconnect();
    }

    // Keep ESP-NOW locked to last known channel even while disconnected
    if (lastKnownWifiChannel > 0 && lastKnownWifiChannel <= 14) {
        esp_wifi_set_channel(lastKnownWifiChannel, WIFI_SECOND_CHAN_NONE);
    }
}

// =========================================================================
// EMBEDDED WEB DASHBOARD & HTTP API
// =========================================================================
void handleRootHtml() {
    unsigned long now = millis();
    unsigned long runSec = pumpActive ? ((now - pumpStartTime) / 1000) : 0;
    float watchdogSecLeft = pumpActive ? max(0.0f, (float)(WATCHDOG_TIMEOUT_MS - (now - lastHeartbeatTime)) / 1000.0f) : 0.0f;
    unsigned long cooldownLeft = (!pumpActive && (now - pumpStopTime < MIN_COOLDOWN_MS) && pumpStopTime != 0) 
                                 ? ((MIN_COOLDOWN_MS - (now - pumpStopTime)) / 1000) : 0;

    String html = F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
                    "<title>Node 3 - Pump Relay Controller</title>"
                    "<style>"
                    ":root{--bg:#0f172a;--card:#1e293b;--text:#f8fafc;--primary:#3b82f6;--danger:#ef4444;--success:#10b981;--warn:#f59e0b;}"
                    "*{margin:0;padding:0;box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;}"
                    "body{background:var(--bg);color:var(--text);padding:1.5rem;display:flex;justify-content:center;}"
                    ".container{max-width:540px;width:100%;display:flex;flex-direction:column;gap:1.25rem;}"
                    ".card{background:var(--card);border-radius:1rem;padding:1.5rem;border:1px solid #334155;box-shadow:0 10px 15px -3px rgba(0,0,0,0.5);}"
                    ".header{display:flex;justify-content:space-between;align-items:center;}"
                    ".title{font-size:1.25rem;font-weight:700;letter-spacing:-0.025em;}"
                    ".badge{padding:0.25rem 0.75rem;border-radius:9999px;font-size:0.875rem;font-weight:700;text-transform:uppercase;}"
                    ".badge-on{background:rgba(239,68,68,0.2);color:var(--danger);border:1px solid var(--danger);box-shadow:0 0 12px rgba(239,68,68,0.4);}"
                    ".badge-off{background:rgba(16,185,129,0.2);color:var(--success);border:1px solid var(--success);}"
                    ".led-box{display:flex;align-items:center;gap:0.75rem;margin:1rem 0;padding:0.75rem 1rem;background:#0b1120;border-radius:0.75rem;border:1px solid #1e293b;}"
                    ".led-dot{width:16px;height:16px;border-radius:50%;background:#475569;transition:all 0.2s;}"
                    ".led-dot.pulsing{background:#3b82f6;box-shadow:0 0 15px #3b82f6;animation:blink 0.5s infinite;}"
                    ".led-dot.idle{background:#10b981;box-shadow:0 0 8px #10b981;}"
                    "@keyframes blink{0%,100%{opacity:1;transform:scale(1.1);}50%{opacity:0.3;transform:scale(0.9);}}"
                    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:1rem;margin:1rem 0;}"
                    ".stat-item{background:#0b1120;padding:1rem;border-radius:0.75rem;border:1px solid #1e293b;}"
                    ".stat-label{font-size:0.75rem;color:#94a3b8;font-weight:600;text-transform:uppercase;}"
                    ".stat-value{font-size:1.35rem;font-weight:700;margin-top:0.25rem;}"
                    ".btn{display:block;width:100%;padding:0.85rem;border:none;border-radius:0.75rem;font-size:1rem;font-weight:700;cursor:pointer;transition:background 0.2s;margin-top:0.75rem;}"
                    ".btn-danger{background:var(--danger);color:#fff;}"
                    ".btn-danger:hover{background:#dc2626;}"
                    ".btn-success{background:var(--success);color:#fff;}"
                    ".btn-success:hover{background:#059669;}"
                    ".footer{font-size:0.75rem;color:#64748b;text-align:center;line-height:1.4;}"
                    "</style></head><body><div class='container'><div class='card'>"
                    "<div class='header'><div class='title'>Node 3: Pump Controller</div>");

    if (pumpActive) {
        html += F("<div class='badge badge-on'>⚡ PUMP ON</div>");
    } else {
        html += F("<div class='badge badge-off'>💤 PUMP OFF</div>");
    }

    html += F("</div><div class='led-box'><div class='led-dot ");
    if (pumpActive) html += F("pulsing");
    else html += F("idle");
    html += F("'></div><div style='font-size:0.85rem;'><b>Relay Built-in LED:</b> ");
    if (pumpActive) html += F("<span style='color:var(--primary);'>Active Pulsing (2 Hz Alert Signal)</span>");
    else html += F("<span style='color:var(--success);'>Standby Heartbeat (3s Pulse)</span>");

    html += F("</div></div><div class='grid'>"
              "<div class='stat-item'><div class='stat-label'>Tank Level</div><div class='stat-value' style='color:#38bdf8;'>");
    html += String(lastReportedLevel, 1) + F("%</div></div>"
              "<div class='stat-item'><div class='stat-label'>Safety Watchdog</div><div class='stat-value' style='color:");
    html += (watchdogSecLeft < 5.0f && pumpActive) ? F("var(--danger)") : F("var(--text)");
    html += F(";'>");
    html += pumpActive ? (String(watchdogSecLeft, 1) + "s") : String("Armed");
    html += F("</div></div><div class='stat-item'><div class='stat-label'>Runtime</div><div class='stat-value'>");
    html += String(runSec / 60) + F("m ") + String(runSec % 60) + F("s</div></div>"
              "<div class='stat-item'><div class='stat-label'>Cooldown</div><div class='stat-value'>");
    html += (cooldownLeft > 0) ? (String(cooldownLeft) + "s") : String("Ready");
    html += F("</div></div></div>"
              "<div style='font-size:0.8rem;color:#94a3b8;margin-bottom:0.5rem;'><b>Last Shutoff:</b> ");
    html += lastShutdownReason;
    html += F("</div>");

    if (pumpActive) {
        html += F("<button class='btn btn-danger' onclick='controlPump(0)'>🛑 STOP PUMP NOW</button>");
    } else {
        html += F("<button class='btn btn-success' onclick='controlPump(1)'>⚡ START PUMP (MANUAL)</button>");
    }

    html += F("</div><div class='card' style='font-size:0.8rem;color:#94a3b8;'>"
              "<div style='font-weight:700;color:#f8fafc;margin-bottom:0.5rem;'>📡 System & ESP-NOW Telemetry</div>"
              "<div><b>MAC Address:</b> ");
    html += WiFi.macAddress();
    html += F("<br><b>Wi-Fi IP:</b> ");
    html += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : F("Offline");
    html += F("<br><b>Wi-Fi Channel:</b> ");
    html += String((WiFi.status() == WL_CONNECTED) ? WiFi.channel() : lastKnownWifiChannel);
    html += F("<br><b>Node 1 Tap RSSI:</b> ");
    html += String(peerTapRssi) + F(" dBm"
              "<br><b>Firmware Version:</b> v") + String(FIRMWARE_VERSION) + F("</div></div>"
              "<div class='footer'>Smart Tank Automation • Google Antigravity Decentralized Architecture</div>"
              "</div><script>"
              "function controlPump(state){"
              "if(!confirm(state ? 'Are you sure you want to START the high-power pump?' : 'STOP the pump?')) return;"
              "fetch('/api/pump?state=' + state).then(r=>r.json()).then(d=>{location.reload();});"
              "}"
              "setTimeout(()=>{location.reload();}, 4000);"
              "</script></body></html>");

    server.send(200, "text/html", html);
}

void handleStatusApi() {
    unsigned long now = millis();
    unsigned long runSec = pumpActive ? ((now - pumpStartTime) / 1000) : 0;
    float watchdogSecLeft = pumpActive ? max(0.0f, (float)(WATCHDOG_TIMEOUT_MS - (now - lastHeartbeatTime)) / 1000.0f) : 0.0f;
    unsigned long cooldownLeft = (!pumpActive && (now - pumpStopTime < MIN_COOLDOWN_MS) && pumpStopTime != 0) 
                                 ? ((MIN_COOLDOWN_MS - (now - pumpStopTime)) / 1000) : 0;

    String json = "{";
    json += "\"pump_state\":\"" + String(pumpActive ? "ON" : "OFF") + "\",";
    json += "\"water_level_pct\":" + String(lastReportedLevel, 1) + ",";
    json += "\"run_duration_sec\":" + String(runSec) + ",";
    json += "\"watchdog_sec_left\":" + String(watchdogSecLeft, 1) + ",";
    json += "\"cooldown_sec_left\":" + String(cooldownLeft) + ",";
    json += "\"last_shutdown_reason\":\"" + lastShutdownReason + "\",";
    json += "\"is_elected_gateway\":" + String(isElectedGateway ? "true" : "false") + ",";
    json += "\"peer_tap_rssi\":" + String(peerTapRssi) + ",";
    json += "\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127) + ",";
    json += "\"wifi_channel\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.channel() : lastKnownWifiChannel) + ",";
    json += "\"wifi_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Offline") + "\",";
    json += "\"mac_address\":\"" + WiFi.macAddress() + "\",";
    json += "\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false") + ",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// =========================================================================
// MQTT IMPLEMENTATION (DIRECT HIGH-SPEED WEBSOCKET CONTROL FROM GITHUB PAGES)
// =========================================================================
void publishMqttPumpStatus(const String& reason) {
    if (!mqttClient.connected()) return;
    unsigned long now = millis();
    unsigned long runSec = pumpActive ? ((now - pumpStartTime) / 1000) : 0;
    float watchdogSecLeft = pumpActive ? max(0.0f, (float)(WATCHDOG_TIMEOUT_MS - (now - lastHeartbeatTime)) / 1000.0f) : 0.0f;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"pump\":%s,\"p_lock\":false,\"pump_state\":\"%s\",\"run_sec\":%lu,\"watchdog_left\":%.1f,\"reason\":\"%s\"}",
        pumpActive ? "true" : "false", pumpActive ? "ON" : "OFF", runSec, watchdogSecLeft,
        reason.length() > 0 ? reason.c_str() : lastShutdownReason.c_str());
    mqttClient.publish(MQTT_TOPIC_PUMP_STATUS, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

void onMqttPumpMessage(char* topic, byte* payload, unsigned int length) {
    if (length == 0 || length >= 256) return;
    char msg[256];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    String msgStr = String(msg);
    Serial.printf("[MQTT] Received command on %s: %s\n", topic, msgStr.c_str());

    int cmdIdx = msgStr.indexOf("\"cmd\":");
    if (cmdIdx == -1) cmdIdx = msgStr.indexOf("\"cmd\" :");
    String cmd = "";
    if (cmdIdx != -1) {
        int q1 = msgStr.indexOf('"', cmdIdx + 6);
        int q2 = msgStr.indexOf('"', q1 + 1);
        if (q1 != -1 && q2 != -1) cmd = msgStr.substring(q1 + 1, q2);
    }

    int stateIdx = msgStr.indexOf("\"state\":");
    if (stateIdx == -1) stateIdx = msgStr.indexOf("\"state\" :");
    int state = -1;
    if (stateIdx != -1) {
        state = msgStr.substring(stateIdx + 8).toInt();
    }

    if (cmd == "pump" || cmd == "pump_on" || cmd == "pump_off") {
        bool targetOn = (cmd == "pump_on") || (cmd == "pump" && state == 1);
        lastHeartbeatTime = millis(); // Reset safety watchdog
        setRelay(targetOn, "Cloud MQTT Command");
    }
}

void maintainMqtt() {
    if (WiFi.status() != WL_CONNECTED) return;
    unsigned long now = millis();
    if (!mqttClient.connected()) {
        if (now - lastMqttRetry >= 5000) {
            lastMqttRetry = now;
            String clientId = "ESP32Pump-" + String((uint32_t)ESP.getEfuseMac(), HEX);
            espMqttClient.setTimeout(3000); // 3 seconds socket timeout
            if (mqttClient.connect(clientId.c_str())) {
                Serial.println(F("[MQTT] ✅ Node 3 Connected to Global HiveMQ Broker!"));
                mqttClient.subscribe(MQTT_TOPIC_COMMAND);
                mqttClient.subscribe(MQTT_TOPIC_PUMP_CMD);
                publishMqttPumpStatus("Node 3 Online & Armed");
            } else {
                Serial.printf("[MQTT] Node 3 HiveMQ connect failed (rc=%d)\n", mqttClient.state());
            }
        }
    } else {
        mqttClient.loop();
        if (pumpActive && (now - lastMqttStatusPub >= 3000)) {
            lastMqttStatusPub = now;
            publishMqttPumpStatus("Telemetry Sync");
        }
    }
}

// =========================================================================
// SETUP & INITIALIZATION
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(200);

    // 1. Safe Relay Initialization (Starts OFF)
    pinMode(PIN_RELAY, OUTPUT);
    digitalWrite(PIN_RELAY, (RELAY_ACTIVE_LEVEL == LOW) ? HIGH : LOW);
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // 2. Wi-Fi Initialization
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);

    Serial.print("[WIFI] Connecting to " DEFAULT_WIFI_SSID);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 25) {
        delay(300);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        lastKnownWifiChannel = WiFi.channel();
        Serial.printf("[WIFI] Connected! IP: %s | RSSI: %d dBm | Channel: %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(), lastKnownWifiChannel);
    } else {
        Serial.println("[WIFI] Initial connect timed out. Continuous background maintainWiFi() active.");
    }

    WiFi.macAddress(myMac);
    Serial.printf("[PUMP NODE] MAC: %s\n", WiFi.macAddress().c_str());

    // 3. ESP-NOW Mesh Protocol Init
    if (esp_now_init() == ESP_OK) {
        Serial.println("[ESP-NOW] Decentralized Protocol Initialized.");
        esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

        // Add Broadcast Peer
        esp_now_peer_info_t bcastPeer = {};
        memcpy(bcastPeer.peer_addr, broadcastMac, 6);
        bcastPeer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : lastKnownWifiChannel;
        bcastPeer.encrypt = false;
        esp_now_add_peer(&bcastPeer);

        // Add Node 1 Peer (Initial registration)
        ensurePeerExists(tapNodeMac);
    }

    // 4. Over-The-Air Setup
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();

    // 5. Web Server Routes
    server.on("/", HTTP_GET, handleRootHtml);
    server.on("/api/status", HTTP_GET, handleStatusApi);
    server.on("/api/pump", HTTP_GET, []() {
        if (server.hasArg("state")) {
            setRelay(server.arg("state").toInt() == 1, "Web UI Manual Override");
            server.send(200, "application/json", "{\"status\":\"ok\",\"pump_state\":\"" + String(pumpActive ? "ON" : "OFF") + "\"}");
        } else if (server.hasArg("keepalive")) {
            lastHeartbeatTime = millis();
            server.send(200, "application/json", "{\"status\":\"ok\",\"watchdog_fed\":true,\"pump_state\":\"" + String(pumpActive ? "ON" : "OFF") + "\"}");
        } else {
            server.send(200, "application/json", "{\"pump_state\":\"" + String(pumpActive ? "ON" : "OFF") + "\"}");
        }
    });
    server.enableCORS(true);
    server.begin();
    Serial.println("[HTTP] Node 3 Web Server started on port 80");

    // 6. Global MQTT Client for Direct GitHub Pages Web Control
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(onMqttPumpMessage);
    mqttClient.setBufferSize(512);
}

// =========================================================================
// MAIN RUNTIME LOOP
// =========================================================================
void loop() {
    ArduinoOTA.handle();
    server.handleClient();

    // Active Status LED Signaling Routine (2 Hz when ON, 3s heartbeat when idle)
    updateStatusLed();

    // Background Wi-Fi Auto-Reconnect & Channel Locking
    maintainWiFi();

    // Global MQTT Cloud Message Service (Bidirectional HiveMQ Broker)
    maintainMqtt();

    unsigned long now = millis();

    // --- AUTONOMOUS SAFETY BOSS (MASTER AUTHORITY OVERRIDES) ---
    if (pumpActive) {
        // Watchdog 1: Lost heartbeat from Node 1 (> 15s)
        if (now - lastHeartbeatTime >= WATCHDOG_TIMEOUT_MS) {
            setRelay(false, "SAFETY WATCHDOG: Lost ESP-NOW heartbeat (> 15s)");
            broadcastSafetyOverride(1); // Inform cluster that pump was tripped by watchdog
        }

        // Watchdog 2: Continuous runtime safety limit (> 35 min)
        if (now - pumpStartTime >= MAX_RUN_TIME_MS) {
            setRelay(false, "SAFETY TIMEOUT: Exceeded max runtime (35 min)");
            broadcastSafetyOverride(2); // Inform cluster that pump timed out
        }
    }

    // Periodic Heartbeat Broadcast every 5s
    if (now - lastBroadcastTime >= 5000UL) {
        lastBroadcastTime = now;
        broadcastHeartbeat();
    }

    // Background Processing: Upload forwarded bulk data chunks to Google Sheets
    if (pendingProxyChunk.hasData && isElectedGateway && (WiFi.status() == WL_CONNECTED)) {
        Serial.printf("[GATEWAY] Uploading Chunk #%u from Node %u (%u records) to Google Sheets...\n",
                      pendingProxyChunk.chunkId, pendingProxyChunk.senderId, pendingProxyChunk.recordCount);
        bool ok = forwardChunkToGoogleSheets(String(pendingProxyChunk.csvRows));
        ClusterPacket ackPkt;
        memset(&ackPkt, 0, sizeof(ClusterPacket));
        ackPkt.magic = CLUSTER_MAGIC;
        ackPkt.senderId = NODE_ID_PUMP_RELAY;
        ackPkt.msgType = MSG_BULK_DATA_ACK;
        ackPkt.payload.bulkAck.ackChunkId = pendingProxyChunk.chunkId;
        ackPkt.payload.bulkAck.httpStatus = ok ? 200 : 500;
        esp_now_send(pendingProxyChunk.senderMac, (uint8_t*)&ackPkt, sizeof(ackPkt));
        pendingProxyChunk.hasData = false;
        Serial.printf("[GATEWAY] Chunk #%u forward result: %s\n", pendingProxyChunk.chunkId, ok ? "SUCCESS" : "FAILED");
    }

    delay(10);
}
