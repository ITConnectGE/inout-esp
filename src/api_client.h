#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
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
        // Truncate body to 120 chars to keep the log readable
        String detail = "HTTP " + String(code) + " — " + respBody.substring(0, 120);
        Logger.log(LOG_ERROR, "SYNC", "Employees sync failed", detail);
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
        String detail = "HTTP " + String(code) + " — " + respBody.substring(0, 120);
        Logger.log(LOG_ERROR, "SYNC", "Events sync failed", detail);
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
            String detail = "HTTP " + String(code);
            if (code > 0) detail += " — " + http.getString().substring(0, 80);
            http.end(); xSemaphoreGiveRecursive(_mutex);
            Logger.log(LOG_ERROR, "SYNC", "Whitelist fetch failed", detail);
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
        } else if (code != -1) {
            Logger.logf(LOG_WARN, "API", "Heartbeat HTTP %d", code);
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
