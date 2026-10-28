/**
 * =========================================================================
 * Cloud OTA Auto-Updater for ESP32 Smart Tank System
 * Checks GitHub for new firmware releases every 15 minutes (non-blocking)
 * =========================================================================
 */

#ifndef CLOUD_OTA_UPDATER_H
#define CLOUD_OTA_UPDATER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include "config.h"

class CloudOTAUpdater {
private:
    unsigned long lastCheckMillis = 0;
    bool initialCheckPending = true;
    String currentVersion;
    String nodeBinName;
    String token;
    String lastStatus = "Initialized. Next check in 30s.";
    unsigned long lastCheckTimeSec = 0;
    bool updateInProgress = false;

    // Helper: Simple JSON string field extractor (zero external dependencies)
    String extractJsonString(const String& json, const String& key) {
        String searchKey = "\"" + key + "\"";
        int keyPos = json.indexOf(searchKey);
        if (keyPos == -1) return "";
        int colonPos = json.indexOf(':', keyPos + searchKey.length());
        if (colonPos == -1) return "";
        int quoteStart = json.indexOf('"', colonPos);
        if (quoteStart == -1) return "";
        int quoteEnd = json.indexOf('"', quoteStart + 1);
        if (quoteEnd == -1) return "";
        return json.substring(quoteStart + 1, quoteEnd);
    }

    // Helper: Extract asset download URL or asset ID for target binary from GitHub release JSON
    String findAssetDownloadUrl(const String& json, const String& targetFileName, String& outAssetApiUrl) {
        int filePos = json.indexOf(targetFileName);
        if (filePos == -1) return "";

        // Look backwards from file name for asset block to extract "url": "https://api.github.com/.../assets/12345"
        int assetBlockStart = json.lastIndexOf("{\"url\":", filePos);
        if (assetBlockStart != -1) {
            int urlStart = json.indexOf("https://api.github.com/repos/", assetBlockStart);
            if (urlStart != -1 && urlStart < filePos) {
                int urlEnd = json.indexOf('"', urlStart);
                if (urlEnd != -1) {
                    outAssetApiUrl = json.substring(urlStart, urlEnd);
                }
            }
        }

        // Look forwards for browser_download_url
        int browserUrlPos = json.indexOf("\"browser_download_url\":", filePos);
        if (browserUrlPos != -1 && browserUrlPos < filePos + 800) {
            int urlStart = json.indexOf("http", browserUrlPos);
            if (urlStart != -1) {
                int urlEnd = json.indexOf('"', urlStart);
                if (urlEnd != -1) {
                    return json.substring(urlStart, urlEnd);
                }
            }
        }
        return "";
    }

public:
    CloudOTAUpdater(const String& version, const String& binName, const String& githubToken = "") {
        currentVersion = version;
        nodeBinName = binName;
        token = githubToken;
    }

    void setToken(const String& newToken) {
        token = newToken;
    }

    String getStatus() const {
        return lastStatus;
    }

    bool isUpdating() const {
        return updateInProgress;
    }

    // Call this inside Arduino loop() regularly (Non-blocking)
    void handleLoop() {
        #if !AUTO_OTA_ENABLED
        return;
        #endif

        unsigned long currentMillis = millis();

        // 1. Initial check ~30 seconds after boot (allows WiFi and sensors to stabilize)
        if (initialCheckPending) {
            if (currentMillis >= 30000) {
                initialCheckPending = false;
                lastCheckMillis = currentMillis;
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.println(F("\n[AUTO-OTA] Executing initial boot firmware check..."));
                    checkAndUpdate(false);
                }
            }
            return;
        }

        // 2. Periodic check every 15 minutes (OTA_CHECK_INTERVAL_MIN * 60 * 1000)
        unsigned long intervalMs = (unsigned long)OTA_CHECK_INTERVAL_MIN * 60UL * 1000UL;
        if (currentMillis - lastCheckMillis >= intervalMs) {
            lastCheckMillis = currentMillis;
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("\n[AUTO-OTA] 15-minute periodic update check interval reached (v%s)...\n", currentVersion.c_str());
                checkAndUpdate(false);
            } else {
                lastStatus = "Skipped: WiFi disconnected";
            }
        }
    }

    // Query GitHub, compare versions, and download/flash if newer
    bool checkAndUpdate(bool forceUpdate = false) {
        if (WiFi.status() != WL_CONNECTED) {
            lastStatus = "Error: WiFi not connected";
            return false;
        }

        lastStatus = "Checking GitHub...";
        Serial.printf("[AUTO-OTA] Contacting GitHub (%s/%s) for release: %s\n", GITHUB_OWNER, GITHUB_REPO, GITHUB_RELEASE_TAG);

        WiFiClientSecure client;
        client.setInsecure(); // GitHub uses valid SSL, but root certs expire/bloat flash. Encrypted connection maintained.

        HTTPClient http;
        http.setTimeout(15000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        // 1. Check version manifest first (version.json from raw contents or release)
        String versionApiUrl = "https://api.github.com/repos/" + String(GITHUB_OWNER) + "/" + String(GITHUB_REPO) + "/contents/firmware/version.json";
        http.begin(client, versionApiUrl);
        http.addHeader("User-Agent", "ESP32-AutoOTA-v1.1");
        http.addHeader("Accept", "application/vnd.github.v3.raw");
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }

        int httpCode = http.GET();
        String latestVersion = "";
        if (httpCode == HTTP_CODE_OK) {
            String manifestJson = http.getString();
            latestVersion = extractJsonString(manifestJson, "firmware_version");
            Serial.printf("[AUTO-OTA] GitHub Version Manifest parsed: latest v%s (Current: v%s)\n", latestVersion.c_str(), currentVersion.c_str());
        } else {
            Serial.printf("[AUTO-OTA] Manifest query code: %d. Falling back to Release API...\n", httpCode);
        }
        http.end();

        // If manifest was not found or empty, query Release API directly
        String releaseApiUrl = "https://api.github.com/repos/" + String(GITHUB_OWNER) + "/" + String(GITHUB_REPO) + "/releases/tags/" + String(GITHUB_RELEASE_TAG);
        http.begin(client, releaseApiUrl);
        http.addHeader("User-Agent", "ESP32-AutoOTA-v1.1");
        http.addHeader("Accept", "application/vnd.github.v3+json");
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }

        httpCode = http.GET();
        String downloadUrl = "";
        String assetApiUrl = "";

        if (httpCode == HTTP_CODE_OK) {
            String releaseJson = http.getString();
            if (latestVersion.length() == 0) {
                latestVersion = extractJsonString(releaseJson, "tag_name");
            }
            downloadUrl = findAssetDownloadUrl(releaseJson, nodeBinName, assetApiUrl);
        } else if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
            // Redirect handled
        } else {
            Serial.printf("[AUTO-OTA] Release API check failed (HTTP %d). Will retry in 15 mins.\n", httpCode);
            lastStatus = "Check failed (HTTP " + String(httpCode) + ")";
            http.end();
            return false;
        }
        http.end();

        // 2. Version comparison
        if (!forceUpdate && latestVersion.length() > 0 && latestVersion == currentVersion) {
            Serial.printf("[AUTO-OTA] ✅ System is up to date (Running v%s). Next check in 15 mins.\n", currentVersion.c_str());
            lastStatus = "Up-to-date (v" + currentVersion + ")";
            return true;
        }

        Serial.printf("[AUTO-OTA] 🚀 New firmware detected! Local: v%s -> Cloud: v%s\n", currentVersion.c_str(), latestVersion.c_str());

        // 3. Determine best download endpoint
        // If private repository and we have assetApiUrl, fetch using API endpoint with Bearer token
        String finalFirmwareUrl = "";
        bool useAssetApi = false;

        if (token.length() > 0 && assetApiUrl.length() > 0) {
            finalFirmwareUrl = assetApiUrl;
            useAssetApi = true;
        } else if (downloadUrl.length() > 0) {
            finalFirmwareUrl = downloadUrl;
        } else {
            // Direct release fallback
            finalFirmwareUrl = "https://github.com/" + String(GITHUB_OWNER) + "/" + String(GITHUB_REPO) + "/releases/download/" + String(GITHUB_RELEASE_TAG) + "/" + nodeBinName;
        }

        Serial.printf("[AUTO-OTA] Downloading binary from: %s\n", finalFirmwareUrl.c_str());
        return flashFirmwareFromUrl(finalFirmwareUrl, useAssetApi);
    }

    // Download stream and write directly to ESP32 flash partition
    bool flashFirmwareFromUrl(const String& url, bool useAssetApiHeader) {
        updateInProgress = true;
        lastStatus = "Downloading & Flashing...";

        WiFiClientSecure downloadClient;
        downloadClient.setInsecure();

        HTTPClient http;
        http.setTimeout(45000); // 45 seconds for firmware stream
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(downloadClient, url)) {
            Serial.println(F("[AUTO-OTA] Failed to begin HTTP download client"));
            lastStatus = "Download connection error";
            updateInProgress = false;
            return false;
        }

        http.addHeader("User-Agent", "ESP32-AutoOTA-v1.1");
        if (useAssetApiHeader) {
            http.addHeader("Accept", "application/octet-stream");
        }
        if (token.length() > 0) {
            http.addHeader("Authorization", "Bearer " + token);
        }

        int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            Serial.printf("[AUTO-OTA] Download HTTP request failed with code %d\n", httpCode);
            lastStatus = "Download failed (HTTP " + String(httpCode) + ")";
            http.end();
            updateInProgress = false;
            return false;
        }

        int contentLength = http.getSize();
        Serial.printf("[AUTO-OTA] Firmware binary size: %d bytes (%.2f MB)\n", contentLength, contentLength / (1024.0f * 1024.0f));

        if (contentLength <= 0) {
            Serial.println(F("[AUTO-OTA] Invalid binary content length"));
            lastStatus = "Invalid firmware size";
            http.end();
            updateInProgress = false;
            return false;
        }

        // Initialize ESP32 Flash Update library
        bool canBegin = Update.begin(contentLength);
        if (!canBegin) {
            Serial.printf("[AUTO-OTA] Flash partition error: Not enough space or wrong partition! Error #%d\n", Update.getError());
            lastStatus = "Flash space error";
            http.end();
            updateInProgress = false;
            return false;
        }

        Serial.println(F("[AUTO-OTA] Flashing firmware to secondary partition..."));

        WiFiClient* stream = http.getStreamPtr();
        size_t written = 0;
        uint8_t buff[2048];
        int lastPct = -1;

        while (http.connected() && (written < (size_t)contentLength)) {
            size_t avail = stream->available();
            if (avail > 0) {
                int readBytes = stream->readBytes(buff, min(avail, sizeof(buff)));
                if (readBytes > 0) {
                    Update.write(buff, readBytes);
                    written += readBytes;

                    int pct = (written * 100) / contentLength;
                    if (pct % 15 == 0 && pct != lastPct) {
                        Serial.printf("[AUTO-OTA] Progress: %d%% (%d / %d bytes)\n", pct, written, contentLength);
                        lastPct = pct;
                        lastStatus = "Flashing: " + String(pct) + "%";
                    }
                }
            } else {
                delay(2);
            }
        }

        http.end();

        if (Update.end()) {
            if (Update.isFinished()) {
                Serial.println(F("\n==========================================================="));
                Serial.println(F(" [AUTO-OTA] ✨ FIRMWARE UPDATE SUCCESSFUL!"));
                Serial.printf(F(" Written: %d bytes. System will reboot in 2 seconds...\n"), written);
                Serial.println(F("===========================================================\n"));
                lastStatus = "Success! Rebooting...";
                delay(2000);
                ESP.restart();
                return true;
            } else {
                Serial.println(F("[AUTO-OTA] Update not finished. Firmware could not verify."));
                lastStatus = "Verification failed";
            }
        } else {
            Serial.printf("[AUTO-OTA] Update error occurred: #%d\n", Update.getError());
            lastStatus = "Update error #" + String(Update.getError());
        }

        updateInProgress = false;
        return false;
    }
};

#endif // CLOUD_OTA_UPDATER_H
