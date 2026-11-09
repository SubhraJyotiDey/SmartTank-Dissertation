#ifndef CLUSTER_PROTOCOL_H
#define CLUSTER_PROTOCOL_H

#include <Arduino.h>

#define CLUSTER_MAGIC             0x544E // 'TN' (Tank Network)
#define PROTOCOL_VERSION          2

// Node Identifiers
#define NODE_ID_TAP_PRESSURE      1
#define NODE_ID_ROOF_ULTRASONIC   2
#define NODE_ID_PUMP_RELAY        3

// Message Types
enum ClusterMsgType : uint8_t {
    MSG_CLUSTER_HEARTBEAT     = 0x10, // Periodic presence, RSSI & Channel beacon
    MSG_ROOF_TELEMETRY        = 0x12, // Node 2 -> Node 1 & Node 3: Ground truth telemetry
    MSG_PUMP_COMMAND          = 0x20, // Node 1 -> Node 3: PUMP_ON / PUMP_OFF
    MSG_SAFETY_OVERRIDE       = 0x30, // Node 3 -> All: Emergency kill & lockout broadcast
    MSG_ROOF_OVERFLOW_STOP    = 0x40, // Node 2 -> Node 3: Direct overflow kill
    MSG_BULK_DATA_CHUNK       = 0x50, // Offline client -> Gateway: Forwarded telemetry
    MSG_BULK_DATA_ACK         = 0x51  // Gateway -> Offline client: Confirmed uploaded
};

#pragma pack(push, 1)
struct ClusterPacket {
    uint16_t magic;          // CLUSTER_MAGIC (0x544E)
    uint8_t  senderId;       // 1, 2, or 3
    uint8_t  msgType;        // ClusterMsgType
    uint16_t seqNumber;      // Monotonic sequence counter
    uint8_t  wifiChannel;    // Current active Wi-Fi channel
    int8_t   wifiRssi;       // -127 if offline, else RSSI in dBm
    uint8_t  isGateway;      // 1 if sender is currently the active Internet Gateway
    uint8_t  senderMac[6];   // Full hardware MAC address for deterministic tie-breaking

    union {
        // MSG_ROOF_TELEMETRY (Node 2 -> Node 1 & Node 3)
        struct {
            float    distanceCm;
            float    waterLevelPct;
            float    waterHeightCm;
        } roofTelemetry;

        // MSG_PUMP_COMMAND (Node 1 -> Node 3)
        struct {
            uint8_t  command;         // 1 = PUMP_ON, 0 = PUMP_OFF
            float    tankLevelPct;    // Calculated water level %
            uint32_t fillBudgetSec;   // Maximum allotted runtime for this cycle
        } pump;

        // MSG_SAFETY_OVERRIDE (Node 3 -> All)
        struct {
            uint8_t  faultCode;       // 1 = Watchdog Timeout, 2 = Max Runtime, 3 = Thermal/Dry-Run
            uint32_t lockoutSec;      // Cooldown seconds before next start is allowed
        } safety;

        // MSG_BULK_DATA_CHUNK (Offline Client -> Gateway)
        struct {
            uint16_t chunkId;         // Incremental chunk index
            uint8_t  recordCount;     // Number of records packed
            char     csvRows[160];    // Raw compact CSV lines
        } bulkData;

        // MSG_BULK_DATA_ACK (Gateway -> Client)
        struct {
            uint16_t ackChunkId;      // The chunk ID that was successfully POSTed
            uint8_t  httpStatus;      // 200 = Success, else error
        } bulkAck;
    } payload;
};
#pragma pack(pop)

// Deterministic Gateway Tie-Breaker Logic (Eliminates Split-Brain)
inline bool evaluateGatewaySuperiority(int8_t myRssi, int8_t peerRssi, 
                                       const uint8_t* myMac, const uint8_t* peerMac) {
    if (myRssi <= -100) return false; // I have no viable Wi-Fi
    if (peerRssi <= -100) return true; // Peer has no viable Wi-Fi

    // 1. If RSSI difference is significant (> 6 dBm), the stronger signal wins
    if (abs(myRssi - peerRssi) > 6) {
        return (myRssi > peerRssi);
    }

    // 2. Tie-Breaker: Lower MAC address wins deterministically
    return (memcmp(myMac, peerMac, 6) < 0);
}

#endif // CLUSTER_PROTOCOL_H