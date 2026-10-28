/**
 * =========================================================================
 * High-Resolution Event Capture & Chunked Upload Engine
 * Records dual-sensor telemetry at 10 Hz (every 100 ms) into SPIFFS Flash
 * Streams completed events in chunks to Google Sheets (Sheet 2)
 * =========================================================================
 */

#ifndef HIGHRES_EVENT_RECORDER_H
#define HIGHRES_EVENT_RECORDER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include "config.h"

class HighResEventRecorder {
private:
    bool recordingActive = false;
    bool uploadingActive = false;
    String eventName = "Event";
    String eventId = "EVT_1";
    int colorIndex = 0;
    unsigned long startMillis = 0;
    unsigned long durationMillis = 30000; // 30 sec default
    unsigned long lastSampleMillis = 0;
    unsigned long lastRooftopFetch = 0;
    int samplesCaptured = 0;

    // Rooftop Node 2 Cache
    float cachedRooftopDist = 60.0f;
    float cachedRooftopLevel = 50.0f;
    const char* rooftopApiUrl = "http://192.168.29.130/api/reading";

    // Buffer File
    const char* bufferPath = "/event_highres.csv";
    File writeBuffer;
    File readBuffer;

    // Upload Progress Tracking
    int currentUploadChunk = 0;
    int totalUploadChunks = 0;
    String uploadStatusText = "Idle";
    unsigned long lastChunkPostTime = 0;

    // Rooftop background poller task running on Core 0 (Zero impact on Core 1 sampling)
    static void rooftopWorkerTask(void* pvParameters) {
        HighResEventRecorder* self = (HighResEventRecorder*)pvParameters;
        while (true) {
            if (WiFi.status() == WL_CONNECTED) {
                WiFiClient client;
                client.setTimeout(250);
                HTTPClient http;
                http.setTimeout(250);
                if (http.begin(client, self->rooftopApiUrl)) {
                    int code = http.GET();
                    if (code == HTTP_CODE_OK) {
                        String payload = http.getString();
                        int distIdx = payload.indexOf("\"distance_cm\":");
                        if (distIdx != -1) {
                            float d = payload.substring(distIdx + 14).toFloat();
                            if (d > 0.0f && d < 400.0f) self->cachedRooftopDist = d;
                        }
                        int lvlIdx = payload.indexOf("\"level_pct\":");
                        if (lvlIdx != -1) {
                            float l = payload.substring(lvlIdx + 12).toFloat();
                            if (l >= 0.0f && l <= 100.0f) self->cachedRooftopLevel = l;
                        }
                    }
                    http.end();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

public:
    HighResEventRecorder() {}

    void begin() {
        xTaskCreatePinnedToCore(rooftopWorkerTask, "RooftopPoller", 4096, this, 1, NULL, 0);
    }

    bool isRecording() const { return recordingActive; }
    bool isUploading() const { return uploadingActive; }
    String getEventName() const { return eventName; }
    String getEventId() const { return eventId; }
    int getSamplesCount() const { return samplesCaptured; }
    String getUploadStatus() const { return uploadStatusText; }
    float getRooftopDist() const { return cachedRooftopDist; }
    float getRooftopLevel() const { return cachedRooftopLevel; }

    float getElapsedSec() const {
        if (!recordingActive) return 0.0f;
        return (millis() - startMillis) / 1000.0f;
    }

    float getDurationSec() const {
        return durationMillis / 1000.0f;
    }

    int getUploadProgressPct() const {
        if (totalUploadChunks <= 0) return 0;
        return (currentUploadChunk * 100) / totalUploadChunks;
    }

    bool startEvent(const String& name, float durationSec, int colorIdx = 0) {
        if (recordingActive || uploadingActive) return false;

        eventName = name.length() > 0 ? name : "Manual Tap Event";
        colorIndex = colorIdx;
        durationMillis = (unsigned long)(durationSec * 1000.0f);
        startMillis = millis();
        lastSampleMillis = 0;
        samplesCaptured = 0;
        currentUploadChunk = 0;
        totalUploadChunks = 0;

        // Generate clean unique ID: EVT_<timestamp>
        eventId = "EVT_" + String((uint32_t)(millis() / 1000));

        // Delete any old buffer and open clean file in SPIFFS
        if (SPIFFS.exists(bufferPath)) {
            SPIFFS.remove(bufferPath);
        }

        writeBuffer = SPIFFS.open(bufferPath, FILE_WRITE);
        if (!writeBuffer) {
            Serial.println(F("[HIGH-RES] ❌ Failed to open SPIFFS event buffer for writing!"));
            uploadStatusText = "Buffer open failed";
            return false;
        }

        recordingActive = true;
        uploadStatusText = "Recording at 10 Hz...";

        Serial.printf("\n===========================================================\n");
        Serial.printf(" [HIGH-RES] 🔴 STARTED EVENT RECORDING: '%s' (ID: %s)\n", eventName.c_str(), eventId.c_str());
        Serial.printf(" Duration: %.1f sec | Sampling: 10 Hz (100 ms) | Target: Sheet 2\n", durationSec);
        Serial.printf("===========================================================\n");
        return true;
    }

    // Called inside 10 Hz loop (every 100 ms)
    void captureSample(float voltage, float pressureKpa, float dpDt, const char* stateStr,
                       float predLevelPct, float flowRateLps, float cumulativeLiters) {
        if (!recordingActive) return;

        unsigned long now = millis();
        float elapsedSec = (now - startMillis) / 1000.0f;

        // Append line to SPIFFS: s,v,p,d,st,ud,ul,pl,fl,vl
        if (writeBuffer) {
            writeBuffer.printf("%.2f,%.3f,%.2f,%.2f,%s,%.1f,%.1f,%.1f,%.3f,%.3f\n",
                               elapsedSec, voltage, pressureKpa, dpDt, stateStr,
                               cachedRooftopDist, cachedRooftopLevel, predLevelPct,
                               flowRateLps, cumulativeLiters);
            samplesCaptured++;
        }

        // Auto-stop when duration is reached
        if (now - startMillis >= durationMillis) {
            stopEvent();
        }
    }

    void stopEvent() {
        if (!recordingActive) return;

        recordingActive = false;
        if (writeBuffer) {
            writeBuffer.flush();
            writeBuffer.close();
        }

        Serial.printf("\n[HIGH-RES] ⏹️ RECORDING FINISHED. Total samples: %d\n", samplesCaptured);
        if (samplesCaptured > 0) {
            totalUploadChunks = (samplesCaptured + 49) / 50; // 50 samples per chunk
            currentUploadChunk = 0;
            uploadingActive = true;
            uploadStatusText = "Preparing chunked upload to Sheet 2 (0/" + String(totalUploadChunks) + ")...";

            readBuffer = SPIFFS.open(bufferPath, FILE_READ);
            if (!readBuffer) {
                Serial.println(F("[HIGH-RES] Failed to open buffer for reading!"));
                uploadingActive = false;
                uploadStatusText = "Read error";
            }
        } else {
            uploadStatusText = "No samples captured";
        }
    }

    // Runs in loop(): uploads 1 chunk of 50 samples every 1.2s to prevent Google rate limits
    void processChunkedUpload(const String& googleScriptUrl) {
        if (!uploadingActive) return;
        if (WiFi.status() != WL_CONNECTED) {
            uploadStatusText = "Waiting for WiFi...";
            return;
        }

        unsigned long now = millis();
        if (now - lastChunkPostTime < 1200) return; // 1.2s cadence between chunks
        lastChunkPostTime = now;

        if (!readBuffer || !readBuffer.available()) {
            // All chunks completed!
            if (readBuffer) readBuffer.close();
            uploadingActive = false;
            SPIFFS.remove(bufferPath);
            uploadStatusText = "✅ Success: All " + String(samplesCaptured) + " samples logged to Sheet 2 ('HighRes_Event_Logs')!";
            Serial.printf("[HIGH-RES] ✨ ALL %d SAMPLES UPLOADED TO SHEET 2 WITH EVENT COLORING!\n\n", samplesCaptured);
            return;
        }

        // Read up to 50 rows from buffer
        String jsonPayload = "{\"highres_upload\":true,\"event_id\":\"" + eventId + "\",\"event_name\":\"" + eventName + "\"";
        jsonPayload += ",\"color_index\":" + String(colorIndex);
        jsonPayload += ",\"chunk_index\":" + String(currentUploadChunk);
        jsonPayload += ",\"total_chunks\":" + String(totalUploadChunks);
        jsonPayload += ",\"records\":[";

        int rowsInChunk = 0;
        while (readBuffer.available() && rowsInChunk < 50) {
            String line = readBuffer.readStringUntil('\n');
            line.trim();
            if (line.length() < 5) continue;

            // Parse CSV: s,v,p,d,st,ud,ul,pl,fl,vl
            int p1 = line.indexOf(',');
            int p2 = line.indexOf(',', p1 + 1);
            int p3 = line.indexOf(',', p2 + 1);
            int p4 = line.indexOf(',', p3 + 1);
            int p5 = line.indexOf(',', p4 + 1);
            int p6 = line.indexOf(',', p5 + 1);
            int p7 = line.indexOf(',', p6 + 1);
            int p8 = line.indexOf(',', p7 + 1);
            int p9 = line.indexOf(',', p8 + 1);

            if (p9 == -1) continue;

            String s = line.substring(0, p1);
            String v = line.substring(p1 + 1, p2);
            String p = line.substring(p2 + 1, p3);
            String d = line.substring(p3 + 1, p4);
            String st = line.substring(p4 + 1, p5);
            String ud = line.substring(p5 + 1, p6);
            String ul = line.substring(p6 + 1, p7);
            String pl = line.substring(p7 + 1, p8);
            String fl = line.substring(p8 + 1, p9);
            String vl = line.substring(p9 + 1);

            if (rowsInChunk > 0) jsonPayload += ",";
            jsonPayload += "{\"s\":" + s + ",\"v\":" + v + ",\"p\":" + p + ",\"d\":" + d;
            jsonPayload += ",\"st\":\"" + st + "\",\"ud\":" + ud + ",\"ul\":" + ul;
            jsonPayload += ",\"pl\":" + pl + ",\"fl\":" + fl + ",\"vl\":" + vl + "}";

            rowsInChunk++;
        }
        jsonPayload += "]}";

        if (rowsInChunk == 0) {
            if (readBuffer) readBuffer.close();
            uploadingActive = false;
            SPIFFS.remove(bufferPath);
            uploadStatusText = "Upload complete";
            return;
        }

        currentUploadChunk++;
        uploadStatusText = "Uploading chunk " + String(currentUploadChunk) + "/" + String(totalUploadChunks) + " (" + String(getUploadProgressPct()) + "%)...";
        Serial.printf("[HIGH-RES] Uploading chunk %d/%d (%d rows) to Google Sheets...\n", currentUploadChunk, totalUploadChunks, rowsInChunk);

        // Send HTTPS POST to Google Apps Script
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(12000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (http.begin(client, googleScriptUrl)) {
            http.addHeader("Content-Type", "application/json");
            int httpCode = http.POST(jsonPayload);
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
                Serial.printf("[HIGH-RES] Chunk %d/%d delivered successfully!\n", currentUploadChunk, totalUploadChunks);
            } else {
                Serial.printf("[HIGH-RES] Chunk %d upload returned code %d. Continuing...\n", currentUploadChunk, httpCode);
            }
            http.end();
        }
        client.stop();
    }
};

#endif // HIGHRES_EVENT_RECORDER_H
