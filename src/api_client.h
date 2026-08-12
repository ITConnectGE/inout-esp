#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>
#include <vector>
#include "config.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "logger.h"

struct ApiResponse {
    bool   granted    = false;
    int    openMs     = 3000;
    String name;
    String reason;
    String happenedAt; // ISO timestamp recorded at tap time — shared with photo filename
};

struct ApiClientClass {
private:
    bool _ntpSynced = false;
    SemaphoreHandle_t _mutex = nullptr;
    String _caCert;

    void loadCaCert() {
        if (!SdManager.isMounted() || !SD.exists("/data/ca.pem")) {
            Logger.log(LOG_WARN, "API", "No /data/ca.pem on SD — TLS cert validation disabled. "
                                        "Place server CA cert at /data/ca.pem to enable.");
            return;
        }
        File f = SD.open("/data/ca.pem", FILE_READ);
        if (!f) return;
        _caCert = "";
        while (f.available()) _caCert += (char)f.read();
        f.close();
        Logger.logf(LOG_INFO, "API", "CA cert loaded from /data/ca.pem (%d bytes)", _caCert.length());
    }

    void setupSecureClient(WiFiClientSecure& c) {
        if (_caCert.length() > 0)
            c.setCACert(_caCert.c_str());
        else
            c.setInsecure();
    }

    static bool setTimeFromEpoch(time_t epoch) {
        if (epoch < 1700000000L) return false; // sanity: after 2023-11
        struct timeval tv = { epoch, 0 };
        return settimeofday(&tv, nullptr) == 0;
    }

    // Parse RFC 1123 Date header ("Mon, 04 Aug 2026 14:30:00 GMT") and set
    // device clock. HTTP Date is always UTC, so we switch TZ temporarily.
    void syncTimeFromHttpDate(const String& dateStr) {
        if (dateStr.isEmpty()) return;
        char mon[4] = {};
        int day, year, h, mi, s;
        if (sscanf(dateStr.c_str(), "%*s %d %3s %d %d:%d:%d",
                   &day, mon, &year, &h, &mi, &s) != 6) return;
        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
        };
        int mo = -1;
        for (int i = 0; i < 12; i++)
            if (strncmp(mon, months[i], 3) == 0) { mo = i; break; }
        if (mo < 0 || year < 2024) return;
        struct tm t = {};
        t.tm_year = year - 1900; t.tm_mon = mo; t.tm_mday = day;
        t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = 0;
        setenv("TZ", "UTC0", 1); tzset();
        time_t epoch = mktime(&t);
        setenv("TZ", Config.getPosixTz().c_str(), 1); tzset();
        if (setTimeFromEpoch(epoch))
            Logger.logf(LOG_INFO, "TIME", "Set from server Date header: %s", dateStr.c_str());
    }

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

    // Transport-level failure (code <= 0 means no HTTP response was ever
    // received — DNS, connect, or TLS handshake failed before the server
    // could reply). HTTPClient::errorToString() turns opaque codes like
    // -32512 into "SSL - Memory allocation failed" instead of a blank body,
    // and the heap stats make a fragmentation-caused failure (small
    // maxBlock despite plenty of free heap) visibly distinct from a real
    // network outage right in the log line, instead of needing a separate
    // heap dump correlated by hand afterward.
    String transportErrMsg(int code) {
        return "HTTP " + String(code) + " — " + HTTPClient::errorToString(code)
             + " | heap=" + String(ESP.getFreeHeap())
             + " min=" + String(ESP.getMinFreeHeap())
             + " maxBlock=" + String(ESP.getMaxAllocHeap());
    }

public:
    // Persist UTC epoch to SD so reboots start with an approximate clock.
    void saveTimeToSD() {
        if (!SdManager.isMounted()) return;
        time_t epoch = time(nullptr);
        if (epoch < 1700000000L) return;
        File f = SD.open("/data/time.json", FILE_WRITE);
        if (!f) return;
        JsonDocument doc;
        doc["epoch"] = (long)epoch;
        serializeJson(doc, f);
        f.close();
    }

    // Load epoch saved by saveTimeToSD() and set the system clock.
    // Call right after SD mounts, before WiFi connects.
    void restoreTimeFromSD() {
        if (!SdManager.isMounted() || !SD.exists("/data/time.json")) return;
        File f = SD.open("/data/time.json", FILE_READ);
        if (!f) return;
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); return; }
        f.close();
        long epoch = doc["epoch"] | 0L;
        if (setTimeFromEpoch((time_t)epoch))
            Logger.logf(LOG_INFO, "TIME", "Clock restored from SD (epoch=%ld)", epoch);
    }

    void begin() {
        _mutex = xSemaphoreCreateRecursiveMutex();
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", Config.getPosixTz().c_str(), 1);
        tzset();
        Logger.log(LOG_INFO, "API", "NTP requested: pool.ntp.org time.nist.gov");
        loadCaCert();
    }

    bool isNtpSynced() {
        if (_ntpSynced) return true;
        struct tm t;
        bool synced = getLocalTime(&t, 500) && t.tm_year > 100;
        if (synced) {
            setenv("TZ", Config.getPosixTz().c_str(), 1);
            tzset();
            _ntpSynced = true;
            saveTimeToSD();
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
        // getLocalTime() returns local (TZ-adjusted) time, not UTC — append
        // the real offset (%z) instead of a literal "Z", otherwise the
        // backend stores local time labelled as UTC and every timestamp
        // ends up skewed by the TZ offset once converted back for display.
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
        return buf;
    }

    ApiResponse processCard(const String& uid, CardDirection dir) {
        ApiResponse resp;
        resp.name       = SdManager.lookupUid(uid);
        resp.granted    = resp.name.length() > 0;
        resp.openMs     = Config.relayMs;
        resp.reason     = resp.granted ? "ok" : "card_unknown";
        resp.happenedAt = nowIso(); // capture once — reused for both event log and photo filename
        String logDir   = (Config.directionMode == "toggle")
                          ? "unknown" : (dir == DIR_IN ? "in" : "out");
        SdManager.logEvent(uid, resp.name,
                           logDir,
                           resp.granted ? "granted" : "denied",
                           resp.happenedAt,
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
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + "/device/employees/batch");
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
        Logger.log(LOG_ERROR, "SYNC", "Employees sync failed",
                   code > 0 ? serverErrMsg(code, respBody) : transportErrMsg(code));
        return -1;
    }

    int syncEvents() {
        if (!serverReachable()) return -1;
        int n = SdManager.unsyncedCount();
        if (n == 0) return 0;

        // Collect up to 10 events — smaller batch leaves heap headroom for photos.
        JsonDocument evDoc;
        JsonArray arr = evDoc["events"].to<JsonArray>();
        if (!SdManager.getUnsyncedEvents(arr, 10)) {
            Logger.logf(LOG_WARN, "SYNC", "Dropping %d unsendable events (bad timestamps)", n);
            SdManager.markAllSynced();
            return 0;
        }
        int batchSize = (int)arr.size();
        const char* boundary = "----ESP32Bnd9f2a";

        // ── Find photos for these events ───────────────────────────────────────
        struct PInfo { String path; size_t size; bool ok = false; };
        PInfo photos[10];
        for (int i = 0; i < batchSize; i++) {
            photos[i].path = SdManager.getPhotoForEvent(
                String(arr[i]["uid"] | ""), String(arr[i]["happened_at"] | ""));
            if (photos[i].path.isEmpty()) continue;
            File pf = SD.open(photos[i].path, FILE_READ);
            if (!pf) { photos[i].path = ""; continue; }
            photos[i].size = pf.size(); pf.close();
            if (photos[i].size == 0 || photos[i].size > 250000)
                { photos[i].path = ""; continue; }
            photos[i].ok = true;
        }

        // ── Write multipart body to SD temp file ───────────────────────────────
        // Avoids holding a large RAM buffer (photo bytes) during the SSL
        // handshake which needs ~80 KB of its own — the two together caused
        // "SSL alloc failed" OOM errors. Streaming from SD keeps peak RAM low.
        const char* tmpPath = "/data/batch.tmp";
        {
            File tmp = SD.open(tmpPath, FILE_WRITE);
            if (!tmp) {
                Logger.log(LOG_ERROR, "SYNC", "Cannot create batch temp file");
                return -1;
            }
            for (int i = 0; i < batchSize; i++) {
                String uid = arr[i]["uid"] | "";
                String ts  = arr[i]["happened_at"] | "";
                String dec = arr[i]["decision"] | "denied";
                String dir = (Config.directionMode == "toggle")
                             ? "unknown" : String(arr[i]["direction"] | "unknown");
                String rsn = arr[i]["reason"] | "";
                auto fld = [&](const String& nm, const String& val) {
                    tmp.print("--"); tmp.print(boundary);
                    tmp.print("\r\nContent-Disposition: form-data; name=\"");
                    tmp.print(nm); tmp.print("\"\r\n\r\n");
                    tmp.print(val); tmp.print("\r\n");
                };
                fld("events[" + String(i) + "][uid]", uid);
                fld("events[" + String(i) + "][happened_at]", ts);
                fld("events[" + String(i) + "][decision]", dec);
                fld("events[" + String(i) + "][direction]", dir);
                if (rsn.length()) fld("events[" + String(i) + "][reason]", rsn);
            }
            for (int i = 0; i < batchSize; i++) {
                if (!photos[i].ok) continue;
                tmp.print("--"); tmp.print(boundary);
                tmp.print("\r\nContent-Disposition: form-data; name=\"photos[");
                tmp.print(i); tmp.print("]\"; filename=\"p.jpg\"\r\n"
                    "Content-Type: image/jpeg\r\n\r\n");
                File pf = SD.open(photos[i].path, FILE_READ);
                if (pf) {
                    uint8_t chunk[512];
                    while (pf.available()) {
                        int rd = pf.read(chunk, sizeof(chunk));
                        if (rd > 0) tmp.write(chunk, rd);
                    }
                    pf.close();
                }
                tmp.print("\r\n");
            }
            tmp.print("--"); tmp.print(boundary); tmp.print("--\r\n");
            tmp.close();
        }

        // ── Stream from SD to server — peak RAM is SSL context only ───────────
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            SD.remove(tmpPath); return -1;
        }
        File bodyFile = SD.open(tmpPath, FILE_READ);
        if (!bodyFile) {
            xSemaphoreGiveRecursive(_mutex);
            Logger.log(LOG_ERROR, "SYNC", "Cannot reopen batch temp file");
            SD.remove(tmpPath);
            return -1;
        }
        size_t bodyLen = bodyFile.size();
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + "/device/events/batch");
        http.setTimeout(30000);
        http.addHeader("Authorization", "Bearer " + Config.deviceToken);
        http.addHeader("Accept",        "application/json");
        http.addHeader("X-Device-ID",   Config.identifier);
        String ct = "multipart/form-data; boundary="; ct += boundary;
        http.addHeader("Content-Type", ct);
        int code = http.sendRequest("POST", &bodyFile, bodyLen);
        String respBody = code > 0 ? http.getString() : "";
        http.end();
        bodyFile.close();
        xSemaphoreGiveRecursive(_mutex);
        SD.remove(tmpPath);

        if (code == 200 || code == 201) {
            SdManager.markNEventsSynced(batchSize);
            int nPhotos = 0;
            for (int i = 0; i < batchSize; i++) {
                // Delete all photos for this batch: events are synced so any
                // unincluded photo would be orphaned on SD with no way to upload.
                if (!photos[i].path.isEmpty()) { SD.remove(photos[i].path.c_str()); nPhotos++; }
            }
            Logger.logf(LOG_INFO, "SYNC", "Events synced: %d (of %d), photos: %d",
                        batchSize, n, nPhotos);
            return batchSize;
        }
        Logger.log(LOG_ERROR, "SYNC", "Events sync failed",
                   code > 0 ? serverErrMsg(code, respBody) : transportErrMsg(code));
        return -1;
    }

    bool syncWhitelist() {
        if (!serverReachable()) return false;
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Whitelist sync: mutex timeout");
            return false;
        }
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + "/device/sync");
        http.setTimeout(8000); auth(http);
        const char* dateHdr[] = {"Date"};
        http.collectHeaders(dateHdr, 1);
        int code = http.GET();
        if (code != 200) {
            String errBody = code > 0 ? http.getString() : "";
            http.end(); xSemaphoreGiveRecursive(_mutex);
            Logger.log(LOG_ERROR, "SYNC", "Whitelist fetch failed",
                       code > 0 ? serverErrMsg(code, errBody) : transportErrMsg(code));
            return false;
        }
        // Use server's Date header to set clock when NTP hasn't resolved yet.
        if (!_ntpSynced && http.hasHeader("Date"))
            syncTimeFromHttpDate(http.header("Date"));
        String payload = http.getString(); http.end();
        xSemaphoreGiveRecursive(_mutex);
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            if (!doc["relay_ms"].isNull())       Config.relayMs       = doc["relay_ms"];
            if (!doc["config_version"].isNull()) Config.configVersion = doc["config_version"];
            if (!doc["direction_mode"].isNull()) Config.directionMode = doc["direction_mode"].as<String>();
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
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + "/device/heartbeat");
        http.setTimeout(5000); auth(http);
        JsonDocument doc;
        doc["firmware"]        = "0.4.3";
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
            Logger.log(LOG_WARN, "API", "Heartbeat failed", transportErrMsg(code));
        }
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        if (resync) {
            Logger.log(LOG_INFO, "SYNC", "Server requested resync via heartbeat");
            syncWhitelist();
        }
    }

    int proxy(const String& method, const String& path,
              const String& body, String& out) {
        if (!serverReachable()) {
            out = "{\"error\":\"device_offline\",\"offline\":true}"; return 503;
        }
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            out = "{\"error\":\"busy\"}"; return 503;
        }
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + path);
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
