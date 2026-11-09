/**
 * =========================================================================
 * High-Resolution Event Capture & Chunked Upload Engine
 * Records dual-sensor telemetry at 10 Hz (every 100 ms) into SPIFFS Flash
 * Streams completed events in chunks to Google Sheets (Sheet 2) asynchronously
 * using a dedicated FreeRTOS background task on Core 0.
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
    unsigned long durationMillis = 15000; // 15 sec default
    int targetSamples = 0;                 // Exact calculated sample count
    int samplesCaptured = 0;

    // Rooftop Node 2 Cache (-1.0f indicates offline / disconnected)
    float cachedRooftopDist = -1.0f;
    float cachedRooftopLevel = -1.0f;
    bool rooftopOnline = false;
    const char* rooftopApiUrl = "http://192.168.29.130/api/reading";

    // Buffer File
    const char* bufferPath = "/event_highres.csv";
    File writeBuffer;
    File readBuffer;

    // Upload Progress & Settings
    String savedScriptUrl = GOOGLE_SCRIPT_URL;
    int currentUploadChunk = 0;
    int totalUploadChunks = 0;
    String uploadStatusText = "Idle";
    TaskHandle_t rooftopTaskHandle = NULL;
    TaskHandle_t uploadTaskHandle = NULL;

    // Rooftop background poller task running on Core 0 (Zero impact on Core 1 sampling)
    static void rooftopWorkerTask(void* pvParameters) {
        HighResEventRecorder* self = (HighResEventRecorder*)pvParameters;
        int failedPolls = 0;
        while (true) {
            // Pause rooftop polling while high-res background upload is streaming chunks
            if (self->uploadingActive) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (WiFi.status() == WL_CONNECTED) {
                WiFiClient client;
                client.setTimeout(1000);
                HTTPClient http;
                http.setTimeout(1000);
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
                        self->rooftopOnline = true;
                        failedPolls = 0; // Reset fail counter on successful poll
                    } else {
                        failedPolls++;
                    }
                    http.end();
                } else {
                    failedPolls++;
                }
            } else {
                failedPolls++;
            }

            // Only declare offline after 5 consecutive failed seconds
            if (failedPolls >= 5) {
                self->cachedRooftopDist = -1.0f;
                self->cachedRooftopLevel = -1.0f;
                self->rooftopOnline = false;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Static FreeRTOS wrapper for background chunked uploader
    static void uploadWorkerTask(void* pvParameters) {
        HighResEventRecorder* self = (HighResEventRecorder*)pvParameters;
        self->executeBackgroundUpload();
        self->uploadTaskHandle = NULL;
        vTaskDelete(NULL);
    }

    // Asynchronous background upload engine running on Core 0
    void executeBackgroundUpload() {
        if (WiFi.status() != WL_CONNECTED) {
            uploadStatusText = "Waiting for WiFi...";
            int retryWifi = 0;
            while (WiFi.status() != WL_CONNECTED && retryWifi++ < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (WiFi.status() != WL_CONNECTED) {
                uploadStatusText = "Upload failed: No WiFi";
                uploadingActive = false;
                return;
            }
        }

        readBuffer = SPIFFS.open(bufferPath, FILE_READ);
        if (!readBuffer) {
            Serial.println(F("[HIGH-RES] ❌ Failed to open SPIFFS buffer for reading!"));
            uploadStatusText = "Buffer read error";
            uploadingActive = false;
            return;
        }

        currentUploadChunk = 0;
        Serial.printf("\n[HIGH-RES] 🚀 Starting Background Upload of %d chunks (%d samples) to Sheet 2...\n",
                      totalUploadChunks, samplesCaptured);

        while (readBuffer.available() && currentUploadChunk < totalUploadChunks) {
            // Heap Safety: Ensure sufficient contiguous heap for MbedTLS TLS handshake (~40 KB)
            int waitMem = 0;
            while (ESP.getFreeHeap() < 35000 && waitMem++ < 10) {
                Serial.printf("[HIGH-RES] Low heap (%d bytes). Pausing 500ms for memory coalescing...\n", ESP.getFreeHeap());
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            size_t chunkStartPos = readBuffer.position();
            int chunkIdx = currentUploadChunk + 1; // 1-based chunk number for display

            // Build JSON chunk payload (up to 50 rows)
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

            if (rowsInChunk == 0) break;

            int progressPct = (chunkIdx * 100) / totalUploadChunks;
            uploadStatusText = "Uploading chunk " + String(chunkIdx) + "/" + String(totalUploadChunks) + " (" + String(progressPct) + "%)...";
            Serial.printf("[HIGH-RES] [Chunk %d/%d] Transmitting %d rows (Free Heap: %d)...\n",
                          chunkIdx, totalUploadChunks, rowsInChunk, ESP.getFreeHeap());

            // Robust 3-Attempt Delivery Loop with SPIFFS Seek Rewind on Failure
            bool chunkSuccess = false;
            for (int attempt = 1; attempt <= 3; attempt++) {
                WiFiClientSecure client;
                client.setInsecure();
                client.setTimeout(15000);

                HTTPClient http;
                http.setTimeout(15000);
                http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

                if (http.begin(client, savedScriptUrl)) {
                    http.addHeader("Content-Type", "application/json");
                    int httpCode = http.POST(jsonPayload);
                    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
                        chunkSuccess = true;
                        Serial.printf("[HIGH-RES] ✅ Chunk %d/%d delivered successfully on attempt %d!\n",
                                      chunkIdx, totalUploadChunks, attempt);
                    } else {
                        Serial.printf("[HIGH-RES] ⚠️ Chunk %d attempt %d returned HTTP %d: %s\n",
                                      chunkIdx, attempt, httpCode, http.errorToString(httpCode).c_str());
                    }
                    http.end();
                } else {
                    Serial.printf("[HIGH-RES] ⚠️ HTTP client begin failed on chunk %d attempt %d\n", chunkIdx, attempt);
                }
                client.stop();

                if (chunkSuccess) break;

                // Backoff delay before retry
                if (attempt < 3) {
                    uploadStatusText = "Retrying chunk " + String(chunkIdx) + " (attempt " + String(attempt + 1) + "/3)...";
                    Serial.printf("[HIGH-RES] Pausing 3s before retry %d...\n", attempt + 1);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
            }

            if (chunkSuccess) {
                currentUploadChunk = chunkIdx;
                uploadStatusText = "Uploaded chunk " + String(currentUploadChunk) + "/" + String(totalUploadChunks) + " (" + String(getUploadProgressPct()) + "%)";
                // Enforce 3.5s cooldown delay:
                // 1) Allows lwIP TCP PCB to exit TIME_WAIT state
                // 2) Allows FreeRTOS dynamic heap to deallocate and coalesce MbedTLS buffers
                // 3) Gives Google Apps Script time to finish batch row formatting and release lock
                vTaskDelay(pdMS_TO_TICKS(3500));
            } else {
                // Rewind file pointer so data isn't dropped, but break to prevent infinite hang
                readBuffer.seek(chunkStartPos);
                Serial.printf("[HIGH-RES] ❌ Failed to deliver chunk %d after 3 attempts. Aborting upload.\n", chunkIdx);
                uploadStatusText = "Upload failed on chunk " + String(chunkIdx);
                break;
            }
        }

        readBuffer.close();

        if (currentUploadChunk >= totalUploadChunks) {
            SPIFFS.remove(bufferPath);
            uploadingActive = false;
            uploadStatusText = "✅ Success: All " + String(samplesCaptured) + " samples logged to Sheet 2!";
            Serial.printf("[HIGH-RES] ✨ ALL %d SAMPLES SUCCESSFULLY DELIVERED TO SHEET 2!\n\n", samplesCaptured);
        } else {
            uploadingActive = false;
            uploadStatusText = "Partial upload (" + String(currentUploadChunk) + "/" + String(totalUploadChunks) + " chunks)";
        }
    }

public:
    HighResEventRecorder() {}

    void begin() {
        xTaskCreatePinnedToCore(rooftopWorkerTask, "RooftopPoller", 4096, this, 1, &rooftopTaskHandle, 0);
    }

    void suspendTasks() {
        if (rooftopTaskHandle != NULL) vTaskSuspend(rooftopTaskHandle);
        if (uploadTaskHandle != NULL) vTaskSuspend(uploadTaskHandle);
    }

    void resumeTasks() {
        if (rooftopTaskHandle != NULL) vTaskResume(rooftopTaskHandle);
        if (uploadTaskHandle != NULL) vTaskResume(uploadTaskHandle);
    }

    void setScriptUrl(const String& url) {
        if (url.length() > 5) savedScriptUrl = url;
    }

    bool isRecording() const { return recordingActive; }
    bool isUploading() const { return uploadingActive; }
    String getEventName() const { return eventName; }
    String getEventId() const { return eventId; }
    int getSamplesCount() const { return samplesCaptured; }
    int getTargetSamples() const { return targetSamples; }
    String getUploadStatus() const { return uploadStatusText; }
    float getRooftopDist() const { return cachedRooftopDist; }
    float getRooftopLevel() const { return cachedRooftopLevel; }
    bool isRooftopOnline() const { return rooftopOnline; }

    void setRooftopData(float dist, float lvl) {
        if (dist > 0.0f && dist < 400.0f) cachedRooftopDist = dist;
        if (lvl >= 0.0f && lvl <= 100.0f) cachedRooftopLevel = lvl;
        rooftopOnline = true;
    }

    float getElapsedSec() const {
        if (!recordingActive) return 0.0f;
        return (millis() - startMillis) / 1000.0f;
    }

    float getDurationSec() const {
        return durationMillis / 1000.0f;
    }

    int getUploadProgressPct() const {
        if (totalUploadChunks <= 0) return 0;
        int pct = (currentUploadChunk * 100) / totalUploadChunks;
        return (pct > 100) ? 100 : pct;
    }

    bool startEvent(const String& name, float durationSec, int colorIdx = 0) {
        if (recordingActive || uploadingActive) return false;

        eventName = name.length() > 0 ? name : "Manual Tap Event";
        colorIndex = colorIdx;
        durationMillis = (unsigned long)(durationSec * 1000.0f);
        // Explicit Target Sample Calculation: Exactly durationSec * 10 Hz + 1 initial sample (t=0)
        targetSamples = (int)round(durationSec * 10.0f) + 1;
        startMillis = millis();
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
        Serial.printf(" Duration: %.1f sec | Target Samples: %d | Target: Sheet 2\n", durationSec, targetSamples);
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

        // Auto-stop when target sample count is reached OR duration safety timeout
        if (samplesCaptured >= targetSamples || (now - startMillis >= durationMillis + 2500)) {
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

        Serial.printf("\n[HIGH-RES] ⏹️ RECORDING FINISHED. Total samples: %d (Target: %d)\n",
                      samplesCaptured, targetSamples);

        if (samplesCaptured > 0) {
            totalUploadChunks = (samplesCaptured + 49) / 50; // 50 samples per chunk
            currentUploadChunk = 0;
            uploadingActive = true;
            uploadStatusText = "Preparing background upload to Sheet 2 (0/" + String(totalUploadChunks) + ")...";

            // Spawn background uploader task on Core 0 with 8KB stack (Zero blocking of Core 1 web server)
            xTaskCreatePinnedToCore(uploadWorkerTask, "HighResUploader", 8192, this, 1, &uploadTaskHandle, 0);
        } else {
            uploadStatusText = "No samples captured";
        }
    }

    // Backwards compatibility no-op (upload is now managed asynchronously in background FreeRTOS task)
    void processChunkedUpload(const String& googleScriptUrl) {
        // Intentionally empty: all uploads run non-blockingly on Core 0
    }
};

#endif // HIGHRES_EVENT_RECORDER_H
