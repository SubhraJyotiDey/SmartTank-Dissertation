#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "cluster_protocol.h"

// =========================================================================
// PEER HARDWARE ADDRESSES
// =========================================================================
// Node 1 (Tap Pressure Node): E8:6B:EA:D4:57:68
uint8_t tapNodeMac[6] = {0xE8, 0x6B, 0xEA, 0xD4, 0x57, 0x68};
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// =========================================================================
// STATE VARIABLES
// =========================================================================
bool pumpActive = false;
unsigned long pumpStartTime = 0;
unsigned long pumpStopTime = 0;
unsigned long lastStateChangeTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastBroadcastTime = 0;

uint16_t lastExecutedSeq = 0;
float lastReportedLevel = 0.0f;
String lastShutdownReason = "Startup Initialized";

// Anti-Chatter Deadband: Minimum 10 seconds before opposing relay toggle allowed
const unsigned long ANTI_CHATTER_DEADBAND_MS = 10000UL;

// Dynamic Multi-Role / Gateway Election State
bool isElectedGateway = false;
int8_t peerTapRssi = -127;
unsigned long gatewayClaimDebounceStart = 0;
uint8_t myMac[6];

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

// =========================================================================
// RELAY HARDWARE DRIVER (D965 NPN ACTIVE-HIGH)
// =========================================================================
void setRelay(bool state, const String& reason) {
    if (pumpActive == state) return;

    // Enforce 10-second anti-chatter deadband (unless it's an emergency safety shutoff)
    unsigned long now = millis();
    if (!reason.startsWith("SAFETY") && (now - lastStateChangeTime < ANTI_CHATTER_DEADBAND_MS) && lastStateChangeTime != 0) {
        Serial.printf("[RELAY] Toggle rejected: Anti-chatter deadband active (%lu ms left)\n",
                      ANTI_CHATTER_DEADBAND_MS - (now - lastStateChangeTime));
        return;
    }

    pumpActive = state;
    lastStateChangeTime = now;

    if (state) {
        digitalWrite(PIN_RELAY, RELAY_ACTIVE_LEVEL);
        digitalWrite(PIN_STATUS_LED, HIGH);
        pumpStartTime = now;
        lastHeartbeatTime = now;
        Serial.printf("[PUMP] >>> RELAY ENERGIZED (ON) <<< Reason: %s\n", reason.c_str());
    } else {
        digitalWrite(PIN_RELAY, (RELAY_ACTIVE_LEVEL == LOW) ? HIGH : LOW);
        digitalWrite(PIN_STATUS_LED, LOW);
        pumpStopTime = now;
        lastShutdownReason = reason;
        unsigned long runtimeSec = (now - pumpStartTime) / 1000;
        Serial.printf("[PUMP] --- RELAY DE-ENERGIZED (OFF) --- Reason: %s (Ran for %lu s)\n", 
                      reason.c_str(), runtimeSec);
    }
}

// =========================================================================
// ESP-NOW RECEIVER CALLBACK (ROLE-BASED COOPERATIVE DISPATCHER)
// =========================================================================
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(ClusterPacket)) return;

    ClusterPacket pkt;
    memcpy(&pkt, data, sizeof(ClusterPacket));

    // 1. Sanity Check Magic Number
    if (pkt.magic != CLUSTER_MAGIC) return;

    unsigned long now = millis();

    switch (pkt.msgType) {

        // CASE 1: PUMP COMMAND FROM NODE 1 (SLAVE ROLE)
        case MSG_PUMP_COMMAND: {
            // Anti-Stale / Out-of-Order Check: Reject older sequence IDs
            if (pkt.seqNumber <= lastExecutedSeq && (lastExecutedSeq - pkt.seqNumber < 30000)) {
                return; // Drop duplicate/stale packet
            }
            lastExecutedSeq = pkt.seqNumber;
            lastReportedLevel = pkt.payload.pump.tankLevelPct;
            lastHeartbeatTime = now; // Reset watchdog timer

            if (pkt.payload.pump.command == 1) {
                // Command: START PUMP
                if (!pumpActive && (now - pumpStopTime < MIN_COOLDOWN_MS) && pumpStopTime != 0) {
                    unsigned long waitLeft = (MIN_COOLDOWN_MS - (now - pumpStopTime)) / 1000;
                    Serial.printf("[PUMP] Start rejected: Motor cooling down (%lu s left)\n", waitLeft);
                    return;
                }
                setRelay(true, "ESP-NOW Command from Node 1 (Level: " + String(lastReportedLevel, 1) + "%)");
            } else {
                // Command: STOP PUMP (Tank Full / Routine cutoff)
                setRelay(false, "ESP-NOW Routine Stop from Node 1");
            }
            break;
        }

        // CASE 2: ROOFTOP DIRECT OVERFLOW STOP FROM NODE 2 (SLAVE ROLE)
        case MSG_ROOF_OVERFLOW_STOP: {
            Serial.println("[PUMP] DIRECT OVERFLOW INTERRUPT FROM NODE 2 (ROOFTOP)!");
            setRelay(false, "SAFETY: Node 2 Rooftop Overflow Cutoff");
            break;
        }

        // CASE 3: PEER HEARTBEAT & GATEWAY ELECTION (COOPERATIVE ROLE)
        case MSG_CLUSTER_HEARTBEAT: {
            if (pkt.senderId == NODE_ID_TAP_PRESSURE) {
                lastHeartbeatTime = now; // Reset watchdog timer on heartbeat from Node 1
                peerTapRssi = pkt.wifiRssi;

                // Evaluate Gateway role using deterministic tie-breaker
                int8_t myRssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
                bool shouldIBeGateway = evaluateGatewaySuperiority(myRssi, peerTapRssi, myMac, pkt.senderMac);

                if (shouldIBeGateway != isElectedGateway) {
                    if (gatewayClaimDebounceStart == 0) {
                        gatewayClaimDebounceStart = now; // Start 15s hysteresis debounce
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

        // CASE 4: BULK DATA FORWARDING (MASTER GATEWAY ROLE)
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
    pkt.wifiChannel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;
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
// WEB SERVER API
// =========================================================================
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
    json += "\"wifi_channel\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0) + ",";
    json += "\"wifi_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Offline") + "\",";
    json += "\"mac_address\":\"" + WiFi.macAddress() + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

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
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
        delay(400);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    WiFi.macAddress(myMac);
    Serial.printf("[PUMP NODE] MAC: %s | IP: %s | Channel: %d\n",
                  WiFi.macAddress().c_str(), 
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Offline",
                  WiFi.channel());

    // 3. ESP-NOW Mesh Protocol Init
    if (esp_now_init() == ESP_OK) {
        Serial.println("[ESP-NOW] Decentralized Protocol Initialized.");
        esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

        // Add Broadcast Peer
        esp_now_peer_info_t bcastPeer = {};
        memcpy(bcastPeer.peer_addr, broadcastMac, 6);
        bcastPeer.channel = WiFi.channel();
        bcastPeer.encrypt = false;
        esp_now_add_peer(&bcastPeer);

        // Add Node 1 Dedicated Peer
        esp_now_peer_info_t tapPeer = {};
        memcpy(tapPeer.peer_addr, tapNodeMac, 6);
        tapPeer.channel = WiFi.channel();
        tapPeer.encrypt = false;
        esp_now_add_peer(&tapPeer);
    }

    // 4. Over-The-Air Setup
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();

    // 5. Web Server Routes
    server.on("/api/status", HTTP_GET, handleStatusApi);
    server.on("/api/pump", HTTP_GET, []() {
        if (server.hasArg("state")) {
            setRelay(server.arg("state").toInt() == 1, "Web UI Manual Override");
            server.send(200, "application/json", "{\"status\":\"ok\"}");
        }
    });
    server.begin();
}

void loop() {
    ArduinoOTA.handle();
    server.handleClient();

    unsigned long now = millis();

    // --- AUTONOMOUS SAFETY BOSS (MASTER AUTHORITY OVERRIDES) ---
    if (pumpActive) {
        // Watchdog 1: Lost heartbeat from Node 1 (> 10s)
        if (now - lastHeartbeatTime >= WATCHDOG_TIMEOUT_MS) {
            setRelay(false, "SAFETY WATCHDOG: Lost ESP-NOW heartbeat (> 10s)");
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

    delay(20);
}
