#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "nfc_reader.h"
#include "sd_manager.h"

struct ApiResponse {
    bool   granted = false;
    int    openMs  = 3000;
    String name;
    String reason;
};

struct ApiClientClass {
private:
    bool _ntpSynced = false;

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
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", Config.getPosixTz().c_str(), 1);
        tzset();
        Serial.println("[API] NTP requested");
    }

    bool isNtpSynced() {
        if (_ntpSynced) return true;
        struct tm t;
        bool synced = getLocalTime(&t, 500) && t.tm_year > 100;
        if (synced) {
            // Re-apply timezone once NTP confirms sync
            setenv("TZ", Config.getPosixTz().c_str(), 1);
            tzset();
            _ntpSynced = true;
            Serial.printf("[NTP] Synced. Local time: %02d:%02d  TZ=%s\n",
                          t.tm_hour, t.tm_min, Config.timezone.c_str());
        }
        return _ntpSynced;
    }

    // Returns ISO8601 UTC string, or millis fallback
    String nowIso() {
        struct tm t;
        if (!getLocalTime(&t, 100)) {
            char buf[24]; snprintf(buf, sizeof(buf), "~%lums", millis()); return buf;
        }
        char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
        return buf;
    }

    // Offline-first tap — SD whitelist + log
    ApiResponse processCard(const String& uid, CardDirection dir) {
        ApiResponse resp;
        resp.name    = SdManager.lookupUid(uid);
        resp.granted = resp.name.length() > 0;
        resp.openMs  = Config.relayMs;
        resp.reason  = resp.granted ? "ok" : "card_unknown";
        SdManager.logEvent(uid, resp.name,
                           dir == DIR_IN ? "in" : "out",
                           resp.granted ? "granted" : "denied",
                           nowIso());
        Serial.printf("[Card] %s → %s (%s)\n",
                      uid.c_str(),
                      resp.granted ? "GRANTED" : "DENIED",
                      resp.name.c_str());
        return resp;
    }

    // Push unsynced events
    int syncEvents() {
        if (!serverReachable()) return -1;
        int n = SdManager.unsyncedCount();
        if (n == 0) return 0;
        JsonDocument doc;
        JsonArray arr = doc["events"].to<JsonArray>();
        if (!SdManager.getUnsyncedEvents(arr, 50)) return 0;
        String body; serializeJson(doc, body);
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/events/batch");
        http.setTimeout(15000); auth(http);
        int code = http.POST(body); http.end();
        if (code == 200 || code == 201) { SdManager.markAllSynced(); return n; }
        Serial.printf("[Sync] Events HTTP %d\n", code); return -1;
    }

    // Push unsynced employees to server
    int syncEmployees() {
        if (!serverReachable()) return -1;
        JsonDocument doc;
        JsonArray arr = doc["employees"].to<JsonArray>();
        if (!SdManager.getUnsyncedEmployees(arr)) return 0;
        String body; serializeJson(doc, body);
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/employees/batch");
        http.setTimeout(15000); auth(http);
        int code = http.POST(body);
        if (code == 200 || code == 201) {
            // Parse server-assigned IDs from response
            JsonDocument res;
            if (!deserializeJson(res, http.getString()))
                if (res["ids"].is<JsonArray>())
                    SdManager.markEmployeesSynced(res["ids"].as<JsonArray>());
            http.end();
            Serial.println("[Sync] Employees synced");
            return arr.size();
        }
        Serial.printf("[Sync] Employees HTTP %d\n", code);
        http.end(); return -1;
    }

    // Pull whitelist + config from server
    bool syncWhitelist() {
        if (!serverReachable()) return false;
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/sync");
        http.setTimeout(15000); auth(http);
        int code = http.GET();
        if (code != 200) { http.end(); return false; }
        String payload = http.getString(); http.end();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            if (!doc["relay_ms"].isNull())       Config.relayMs       = doc["relay_ms"];
            if (!doc["config_version"].isNull()) Config.configVersion = doc["config_version"];
            Config.save();
        }
        bool ok = SdManager.updateWhitelist(payload);
        SdManager.backupConfig();
        return ok;
    }

    void sendHeartbeat() {
        if (!serverReachable()) return;
        HTTPClient http;
        http.begin(Config.serverUrl + "/device/heartbeat");
        http.setTimeout(5000); auth(http);
        JsonDocument doc;
        doc["firmware"]        = "0.3.0";
        doc["ip"]              = WiFi.localIP().toString();
        doc["rssi"]            = WiFi.RSSI();
        doc["config_version"]  = Config.configVersion;
        doc["unsynced_events"] = SdManager.unsyncedCount();
        doc["sd_mounted"]      = SdManager.isMounted();
        doc["uptime_s"]        = millis()/1000;
        String body; serializeJson(doc, body);
        int code = http.POST(body);
        if (code == 200) {
            JsonDocument res;
            if (!deserializeJson(res, http.getString()))
                if (res["resync"] | false) syncWhitelist();
        }
        http.end();
    }

    int proxy(const String& method, const String& path,
              const String& body, String& out) {
        if (!serverReachable()) {
            out = "{\"error\":\"device_offline\",\"offline\":true}"; return 503;
        }
        HTTPClient http;
        http.begin(Config.serverUrl + path);
        http.setTimeout(10000); auth(http);
        int code;
        if      (method=="GET")    code = http.GET();
        else if (method=="POST")   code = http.POST(body);
        else if (method=="PUT")    code = http.PUT(body);
        else if (method=="DELETE") code = http.sendRequest("DELETE");
        else                       { out="{}"; return 405; }
        out = http.getString(); http.end(); return code;
    }
} ApiClient;
