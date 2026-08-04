#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>
#include <vector>
#include "config.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "logger.h"

struct ApiResponse {
    bool   granted = false;
    int    openMs  = 3000;
    String name;
    String reason;
};

struct ApiClientClass {
private:
    bool _ntpSynced = false;
    SemaphoreHandle_t _mutex = nullptr;

    bool serverReachable() {
        return WiFi.status() == WL_CONNECTED
            && Config.serverUrl.length() > 0
            && Config.deviceToken.length() > 0;
    }

    void auth(HTTPClient& http) {
        http.addHeader("Authorization", "Bearer " + Config.deviceToken);
        http.addHeader("Content-Type",  "application/json");
        http.addHeader("Accept",        "application/json");
        http.addHeader("X-Device-ID",   Config.identifier);
    }

    // Extract a human-readable error string from an HTTP response body.
    // ArduinoJson decodes \uXXXX Unicode escapes to UTF-8 when parsing, so
    // Georgian characters stored as \u10XX in JSON are printed correctly.
    String serverErrMsg(int code, const String& body) {
        String prefix = "HTTP " + String(code) + " — ";
        JsonDocument doc;
        if (body.length() > 0 && !deserializeJson(doc, body)) {
            // Prefer "message", fall back to "error", then raw body
            const char* msg = nullptr;
            if (!doc["message"].isNull()) msg = doc["message"];
            else if (!doc["error"].isNull())   msg = doc["error"];
            if (msg) return prefix + String(msg).substring(0, 160);
        }
        return prefix + body.substring(0, 120);
    }

    // Parse uid and ISO happened_at from a photo base-name (no path, no extension).
    // Expects format: <uid>_<YYYYMMDD>_<HHMMSS>  e.g. "04A2B3C4_20260804_143022"
    // Returns false for millis-only names like "04A2B3C4_1234567".
    bool parsePhotoFilename(const String& base, String& uid, String& happenedAt) {
        int last = base.lastIndexOf('_');
        int prev = last > 0 ? base.lastIndexOf('_', last - 1) : -1;
        if (prev < 0 || prev == last) return false;
        String timeStr = base.substring(last + 1); // "143022"
        String dateStr = base.substring(prev + 1, last); // "20260804"
        if (dateStr.length() != 8 || timeStr.length() != 6) return false;
        for (char c : dateStr) if (!isdigit(c)) return false;
        for (char c : timeStr) if (!isdigit(c)) return false;
        uid = base.substring(0, prev);
        happenedAt = dateStr.substring(0,4) + "-" + dateStr.substring(4,6) + "-" + dateStr.substring(6,8)
                   + "T" + timeStr.substring(0,2) + ":" + timeStr.substring(2,4) + ":" + timeStr.substring(4,6) + "Z";
        return true;
    }

    // Read JPEG from SD and POST as multipart/form-data to /device/events.
    // Returns true on HTTP 200/201.
    bool uploadPhoto(const String& path) {
        String fullName = path.substring(path.lastIndexOf('/') + 1);
        String base = fullName.endsWith(".jpg")
                    ? fullName.substring(0, fullName.length() - 4)
                    : fullName;
        String uid, happenedAt;
        if (!parsePhotoFilename(base, uid, happenedAt)) {
            Logger.log(LOG_WARN, "PHOTO", "Skipping (millis-only filename)", path);
            return false;
        }

        File f = SD.open(path, FILE_READ);
        if (!f) { Logger.log(LOG_WARN, "PHOTO", "Cannot open for upload", path); return false; }
        size_t fileSize = f.size();
        if (fileSize == 0 || fileSize > 200000) {
            f.close();
            Logger.logf(LOG_WARN, "PHOTO", "Skipping (size=%u)", (unsigned)fileSize);
            return false;
        }

        const char* boundary = "----ESP32Bnd7a3f";
        String preamble = "--"; preamble += boundary; preamble += "\r\n"
            "Content-Disposition: form-data; name=\"uid\"\r\n\r\n" + uid + "\r\n"
            "--"; preamble += boundary; preamble += "\r\n"
            "Content-Disposition: form-data; name=\"happened_at\"\r\n\r\n" + happenedAt + "\r\n"
            "--"; preamble += boundary; preamble += "\r\n"
            "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n";
        String postamble = "\r\n--"; postamble += boundary; postamble += "--\r\n";

        size_t totalLen = preamble.length() + fileSize + postamble.length();
        uint8_t* body = (uint8_t*)malloc(totalLen);
        if (!body) {
            f.close();
            Logger.logf(LOG_WARN, "PHOTO", "OOM (%u bytes needed)", (unsigned)totalLen);
            return false;
        }
        memcpy(body, preamble.c_str(), preamble.length());
        size_t got = f.read(body + preamble.length(), fileSize);
        f.close();
        if (got != fileSize) {
            free(body);
            Logger.logf(LOG_WARN, "PHOTO", "Short read %u/%u", (unsigned)got, (unsigned)fileSize);
            return false;
        }
        memcpy(body + preamble.length() + fileSize, postamble.c_str(), postamble.length());

        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) { free(body); return false; }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/events");
        http.setTimeout(20000);
        auth(http);
        String ct = "multipart/form-data; boundary="; ct += boundary;
        http.addHeader("Content-Type", ct);
        int code = http.POST(body, totalLen);
        String respBody = http.getString();
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        free(body);

        if (code == 200 || code == 201) {
            Logger.logf(LOG_INFO, "PHOTO", "Uploaded %s (%u bytes)", path.c_str(), (unsigned)fileSize);
            return true;
        }
        Logger.log(LOG_WARN, "PHOTO", "Upload failed: " + path, serverErrMsg(code, respBody));
        return false;
    }

public:
    void begin() {
        _mutex = xSemaphoreCreateRecursiveMutex();
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", Config.getPosixTz().c_str(), 1);
        tzset();
        Logger.log(LOG_INFO, "API", "NTP requested: pool.ntp.org time.nist.gov");
    }

    bool isNtpSynced() {
        if (_ntpSynced) return true;
        struct tm t;
        bool synced = getLocalTime(&t, 500) && t.tm_year > 100;
        if (synced) {
            setenv("TZ", Config.getPosixTz().c_str(), 1);
            tzset();
            _ntpSynced = true;
            Logger.logf(LOG_INFO, "NTP", "Synced: %02d:%02d  TZ=%s",
                        t.tm_hour, t.tm_min, Config.timezone.c_str());
        }
        return _ntpSynced;
    }

    String nowIso() {
        struct tm t;
        if (!getLocalTime(&t, 100)) {
            char buf[24]; snprintf(buf, sizeof(buf), "~%lums", millis()); return buf;
        }
        char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
        return buf;
    }

    ApiResponse processCard(const String& uid, CardDirection dir) {
        ApiResponse resp;
        resp.name    = SdManager.lookupUid(uid);
        resp.granted = resp.name.length() > 0;
        resp.openMs  = Config.relayMs;
        resp.reason  = resp.granted ? "ok" : "card_unknown";
        SdManager.logEvent(uid, resp.name,
                           dir == DIR_IN ? "in" : "out",
                           resp.granted ? "granted" : "denied",
                           nowIso(),
                           resp.reason);
        if (resp.granted) {
            Logger.logf(LOG_INFO, "CARD", "GRANTED  uid=%s  name=%s  dir=%s",
                        uid.c_str(), resp.name.c_str(), dir == DIR_IN ? "IN" : "OUT");
        } else {
            Logger.logf(LOG_WARN, "CARD", "DENIED  uid=%s  reason=%s  dir=%s",
                        uid.c_str(), resp.reason.c_str(), dir == DIR_IN ? "IN" : "OUT");
        }
        return resp;
    }

    int syncEmployees() {
        if (!serverReachable()) return -1;
        JsonDocument doc;
        JsonArray arr = doc["employees"].to<JsonArray>();
        if (!SdManager.getUnsyncedEmployees(arr)) return 0;
        int count = (int)arr.size();
        String body; serializeJson(doc, body);
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Employees sync: mutex timeout");
            return -1;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/employees/batch");
        http.setTimeout(8000); auth(http);
        int code = http.POST(body);
        String respBody = http.getString(); http.end();
        xSemaphoreGiveRecursive(_mutex);
        if (code == 200 || code == 201) {
            JsonDocument resp;
            if (!deserializeJson(resp, respBody)) {
                JsonArray ids = resp["ids"].as<JsonArray>();
                SdManager.markEmployeesSynced(ids);
            }
            Logger.logf(LOG_INFO, "SYNC", "Employees synced: %d record(s)", count);
            return count;
        }
        Logger.log(LOG_ERROR, "SYNC", "Employees sync failed", serverErrMsg(code, respBody));
        return -1;
    }

    int syncEvents() {
        if (!serverReachable()) return -1;
        int n = SdManager.unsyncedCount();
        if (n == 0) return 0;
        JsonDocument doc;
        JsonArray arr = doc["events"].to<JsonArray>();
        if (!SdManager.getUnsyncedEvents(arr, 50)) {
            // All pending events have bad timestamps — drop to unblock future syncs.
            Logger.logf(LOG_WARN, "SYNC", "Dropping %d unsendable events (bad timestamps)", n);
            SdManager.markAllSynced();
            return 0;
        }
        int batchSize = (int)arr.size();
        String body; serializeJson(doc, body);
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Events sync: mutex timeout");
            return -1;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/events/batch");
        http.setTimeout(8000); auth(http);
        int code = http.POST(body);
        String respBody = http.getString(); http.end();
        xSemaphoreGiveRecursive(_mutex);
        if (code == 200 || code == 201) {
            SdManager.markAllSynced();
            Logger.logf(LOG_INFO, "SYNC", "Events synced: %d of %d record(s)", batchSize, n);
            return n;
        }
        Logger.log(LOG_ERROR, "SYNC", "Events sync failed", serverErrMsg(code, respBody));
        return -1;
    }

    bool syncWhitelist() {
        if (!serverReachable()) return false;
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Whitelist sync: mutex timeout");
            return false;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/sync");
        http.setTimeout(8000); auth(http);
        int code = http.GET();
        if (code != 200) {
            String errBody = code > 0 ? http.getString() : "";
            http.end(); xSemaphoreGiveRecursive(_mutex);
            Logger.log(LOG_ERROR, "SYNC", "Whitelist fetch failed", serverErrMsg(code, errBody));
            return false;
        }
        String payload = http.getString(); http.end();
        xSemaphoreGiveRecursive(_mutex);
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            if (!doc["relay_ms"].isNull())       Config.relayMs       = doc["relay_ms"];
            if (!doc["config_version"].isNull()) Config.configVersion = doc["config_version"];
            Config.save();
        }
        bool ok = SdManager.updateWhitelist(payload);
        SdManager.backupConfig();
        if (ok) Logger.log(LOG_INFO, "SYNC", "Whitelist updated");
        else    Logger.log(LOG_ERROR, "SYNC", "Whitelist update failed — bad payload format");
        return ok;
    }

    void sendHeartbeat() {
        if (!serverReachable()) return;
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_WARN, "API", "Heartbeat: mutex timeout");
            return;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/heartbeat");
        http.setTimeout(5000); auth(http);
        JsonDocument doc;
        doc["firmware"]        = "0.3.2";
        doc["ip"]              = WiFi.localIP().toString();
        doc["rssi"]            = WiFi.RSSI();
        doc["config_version"]  = Config.configVersion;
        doc["unsynced_events"] = SdManager.unsyncedCount();
        doc["sd_mounted"]      = SdManager.isMounted();
        doc["uptime_s"]        = millis()/1000;
        String body; serializeJson(doc, body);
        int code = http.POST(body);
        bool resync = false;
        if (code == 200) {
            JsonDocument res;
            if (!deserializeJson(res, http.getString()))
                resync = res["resync"] | false;
        } else if (code > 0) {
            Logger.log(LOG_WARN, "API", "Heartbeat failed",
                       serverErrMsg(code, http.getString()));
        } else {
            Logger.log(LOG_WARN, "API", "Heartbeat: no response (connection failed)");
        }
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        if (resync) {
            Logger.log(LOG_INFO, "SYNC", "Server requested resync via heartbeat");
            syncWhitelist();
        }
    }

    void uploadPendingPhotos() {
        if (!serverReachable() || !SdManager.isMounted()) return;
        File dir = SD.open("/photos");
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

        std::vector<String> paths;
        File entry;
        while ((entry = dir.openNextFile())) {
            if (!entry.isDirectory()) {
                String n = String(entry.name());
                // entry.name() may return just the filename or full path depending on SD lib version
                String p = n.startsWith("/") ? n : "/photos/" + n;
                if (p.endsWith(".jpg")) paths.push_back(p);
            }
            entry.close();
        }
        dir.close();

        if (paths.empty()) return;
        Logger.logf(LOG_INFO, "PHOTO", "Uploading %d pending photo(s)", (int)paths.size());

        int ok = 0, fail = 0;
        for (const String& p : paths) {
            if (uploadPhoto(p)) { SD.remove(p.c_str()); ok++; }
            else fail++;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        Logger.logf(LOG_INFO, "PHOTO", "Done: %d uploaded, %d failed", ok, fail);
    }

    int proxy(const String& method, const String& path,
              const String& body, String& out) {
        if (!serverReachable()) {
            out = "{\"error\":\"device_offline\",\"offline\":true}"; return 503;
        }
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            out = "{\"error\":\"busy\"}"; return 503;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + path);
        http.setTimeout(10000); auth(http);
        int code;
        if      (method=="GET")    code = http.GET();
        else if (method=="POST")   code = http.POST(body);
        else if (method=="PUT")    code = http.PUT(body);
        else if (method=="DELETE") code = http.sendRequest("DELETE");
        else                       { xSemaphoreGiveRecursive(_mutex); out="{}"; return 405; }
        out = http.getString(); http.end();
        xSemaphoreGiveRecursive(_mutex);
        return code;
    }
} ApiClient;
