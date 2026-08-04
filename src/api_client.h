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
        char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
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

        // ── Build text fields for all events ───────────────────────────────────
        String textPart;
        textPart.reserve(batchSize * 320);
        for (int i = 0; i < batchSize; i++) {
            JsonObject ev = arr[i];
            String uid = ev["uid"] | "";
            String ts  = ev["happened_at"] | "";
            String dec = ev["decision"] | "denied";
            String dir = (Config.directionMode == "toggle")
                         ? "unknown" : String(ev["direction"] | "unknown");
            String rsn = ev["reason"] | "";
            auto fld = [&](const String& nm, const String& val) {
                textPart += "--"; textPart += boundary; textPart += "\r\n"
                    "Content-Disposition: form-data; name=\""; textPart += nm;
                textPart += "\"\r\n\r\n"; textPart += val; textPart += "\r\n";
            };
            fld("events[" + String(i) + "][uid]", uid);
            fld("events[" + String(i) + "][happened_at]", ts);
            fld("events[" + String(i) + "][decision]", dec);
            fld("events[" + String(i) + "][direction]", dir);
            if (rsn.length()) fld("events[" + String(i) + "][reason]", rsn);
        }

        // ── Find matching photos; include those that fit in free heap ──────────
        struct PInfo { String path; String hdr; size_t size; bool ok = false; };
        PInfo photos[10];
        size_t photoByteTotal = 0;
        // Reserve 80 KB headroom for stack, HTTPClient, and JSON buffers.
        size_t budget = ESP.getFreeHeap() > 80000 ? ESP.getFreeHeap() - 80000 : 0;

        for (int i = 0; i < batchSize; i++) {
            String uid = arr[i]["uid"] | "";
            String ts  = arr[i]["happened_at"] | "";
            photos[i].path = SdManager.getPhotoForEvent(uid, ts);
            if (photos[i].path.isEmpty()) continue;
            File pf = SD.open(photos[i].path, FILE_READ);
            if (!pf) { photos[i].path = ""; continue; }
            photos[i].size = pf.size(); pf.close();
            if (photos[i].size == 0 || photos[i].size > 250000)
                { photos[i].path = ""; continue; }
            photos[i].hdr  = "--"; photos[i].hdr += boundary;
            photos[i].hdr += "\r\nContent-Disposition: form-data; name=\"photos[";
            photos[i].hdr += String(i);
            photos[i].hdr += "]\"; filename=\"p.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
            size_t needed = photos[i].hdr.length() + photos[i].size + 2; // +2 \r\n
            if (textPart.length() + photoByteTotal + needed < budget) {
                photos[i].ok = true;
                photoByteTotal += needed;
            }
        }

        // ── Allocate and fill multipart body ───────────────────────────────────
        String tail = "--"; tail += boundary; tail += "--\r\n";
        size_t totalLen = textPart.length() + photoByteTotal + tail.length();
        uint8_t* body = (uint8_t*)malloc(totalLen);
        if (!body) {
            // Not enough contiguous heap — send without photos.
            Logger.logf(LOG_WARN, "SYNC", "OOM (%u B) — sending events without photos",
                        (unsigned)totalLen);
            for (int i = 0; i < batchSize; i++) photos[i].ok = false;
            photoByteTotal = 0;
            totalLen = textPart.length() + tail.length();
            body = (uint8_t*)malloc(totalLen);
            if (!body) {
                Logger.log(LOG_ERROR, "SYNC", "Events sync: OOM, skipping");
                return -1;
            }
        }

        size_t pos = 0;
        memcpy(body + pos, textPart.c_str(), textPart.length()); pos += textPart.length();
        for (int i = 0; i < batchSize; i++) {
            if (!photos[i].ok) continue;
            memcpy(body + pos, photos[i].hdr.c_str(), photos[i].hdr.length());
            pos += photos[i].hdr.length();
            File pf = SD.open(photos[i].path, FILE_READ);
            if (pf) { pos += pf.read(body + pos, photos[i].size); pf.close(); }
            body[pos++] = '\r'; body[pos++] = '\n';
        }
        memcpy(body + pos, tail.c_str(), tail.length()); pos += tail.length();

        // ── POST ───────────────────────────────────────────────────────────────
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) { free(body); return -1; }
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/events/batch");
        http.setTimeout(30000);
        http.addHeader("Authorization", "Bearer " + Config.deviceToken);
        http.addHeader("Accept",        "application/json");
        http.addHeader("X-Device-ID",   Config.identifier);
        String ct = "multipart/form-data; boundary="; ct += boundary;
        http.addHeader("Content-Type", ct);
        int code = http.POST(body, pos);
        String respBody = code > 0 ? http.getString() : "";
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        free(body);

        if (code == 200 || code == 201) {
            SdManager.markNEventsSynced(batchSize);
            int nPhotos = 0;
            for (int i = 0; i < batchSize; i++) {
                // Delete photos for this batch regardless of whether they were
                // included — events are now synced so photos would be orphaned.
                if (!photos[i].path.isEmpty()) { SD.remove(photos[i].path.c_str()); nPhotos++; }
            }
            Logger.logf(LOG_INFO, "SYNC", "Events synced: %d (of %d), photos: %d",
                        batchSize, n, nPhotos);
            return batchSize;
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
        const char* dateHdr[] = {"Date"};
        http.collectHeaders(dateHdr, 1);
        int code = http.GET();
        if (code != 200) {
            String errBody = code > 0 ? http.getString() : "";
            http.end(); xSemaphoreGiveRecursive(_mutex);
            Logger.log(LOG_ERROR, "SYNC", "Whitelist fetch failed", serverErrMsg(code, errBody));
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
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/heartbeat");
        http.setTimeout(5000); auth(http);
        JsonDocument doc;
        doc["firmware"]        = "0.4.0";
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
