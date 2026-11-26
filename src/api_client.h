#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>
#include <vector>
#include <esp_system.h>
#include <algorithm>
#include "config.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "logger.h"

// reset_reason for the heartbeat (§10.6). Computed once at boot.
static const char* resetReasonName(bool otaFlag) {
    if (otaFlag) return "ota";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power_on";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_SW:       return "software";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_DEEPSLEEP:return "deep_sleep";
        default:               return "unknown";
    }
}

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
    SemaphoreHandle_t _mutex = nullptr;   // one HTTP request on _wcs at a time
    SemaphoreHandle_t _cycle = nullptr;   // one sync cycle at a time (syncTask vs serial `sync`)
    String _caCert;
    WiFiClientSecure _wcs;
    uint32_t _eventSeq = 0;
    int _syncErrors = 0;
    const char* _resetReason = "unknown";
    // JSON documents are local to each call now — the old long-lived _doc
    // kept its pool between syncs and was touched outside the mutex.

    void loadCaCert() {
        if (!SdManager.isMounted() || !SD.exists("/data/ca.pem")) {
            Logger.log(LOG_WARN, "API", "No /data/ca.pem on SD — TLS cert validation disabled. "
                                        "Place server CA cert at /data/ca.pem to enable.");
            return;
        }
        File f = SD.open("/data/ca.pem", FILE_READ);
        if (!f) return;
        size_t len = f.size();
        _caCert = "";
        if (len > 0 && len < 16384 && _caCert.reserve(len)) {
            char buf[256];
            while (f.available()) {
                int rd = f.read((uint8_t*)buf, sizeof(buf) - 1);
                if (rd <= 0) break;
                buf[rd] = '\0';
                _caCert += buf;
            }
        }
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
    // Allow callers to configure a WiFiClientSecure for shared use across
    // multiple requests (e.g. the drain loop) to reduce heap fragmentation.
    void configureClient(WiFiClientSecure& c) { setupSecureClient(c); }

    // Set by sendHeartbeat() when the server includes OTA fields in its response.
    // Consumed and cleared by syncTask in main.cpp.
    String pendingOtaUrl;
    String pendingOtaVersion;
    String pendingOtaSha256;   // lowercase hex, "" = server gave none (can't verify)
    size_t pendingOtaSize = 0; // bytes, 0 = unknown

    // Camera firmware version — set by main.cpp after CamUart.requestVersion().
    // Included in heartbeat payload so the dashboard can track camera firmware.
    String camFirmwareVersion;

    // Camera OTA — set by sendHeartbeat(), consumed by syncTask in main.cpp.
    String pendingCamOtaUrl;
    String pendingCamOtaVersion;

    // Set by main.cpp — CamUart lives in a header included after this one.
    bool (*camOkFn)() = nullptr;

    // Heartbeat said the server's whitelist_version differs from ours (or
    // asked for a resync). Consumed by syncTask.
    volatile bool whitelistStale = false;

    // Serialises whole sync cycles: syncTask holds it for its 30 s pass, and
    // the serial `sync` command takes it too, so the two can't interleave
    // and send the same events twice.
    bool lockCycle(uint32_t ms) { return _cycle && xSemaphoreTakeRecursive(_cycle, pdMS_TO_TICKS(ms)) == pdTRUE; }
    void unlockCycle()          { if (_cycle) xSemaphoreGiveRecursive(_cycle); }

    // Close the TLS session at the end of a sync cycle. HTTPClient keeps it
    // open for reuse, which pinned 40–50 KB of heap for the whole 30 s gap
    // between syncs. A fresh handshake every 30 s is cheaper than that.
    void endCycle() {
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) return;
        _wcs.stop();
        xSemaphoreGiveRecursive(_mutex);
    }

    String nextEventId() {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lu-%lu", (unsigned long)Config.bootCount, (unsigned long)++_eventSeq);
        return String(buf);
    }

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
        _cycle = xSemaphoreCreateRecursiveMutex();
        _resetReason = resetReasonName(Config.takeOtaReboot());
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", Config.getPosixTz().c_str(), 1);
        tzset();
        Logger.log(LOG_INFO, "API", "NTP requested: pool.ntp.org time.nist.gov");
        loadCaCert();
        setupSecureClient(_wcs);
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
        // Pre-sync marker ("~123ms") is relied on downstream — sd_manager.h
        // skips events/photo filenames starting with '~' as not-yet-real
        // timestamps, so this guard has to stay even though the formatting
        // below changed.
        if (!isNtpSynced()) {
            char buf[24]; snprintf(buf, sizeof(buf), "~%lums", millis()); return buf;
        }
        time_t now; time(&now);
        struct tm t; gmtime_r(&now, &t);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
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
                           resp.reason,
                           nextEventId());
        if (resp.granted) {
            Logger.logf(LOG_INFO, "CARD", "GRANTED  uid=%s  name=%s  dir=%s",
                        uid.c_str(), resp.name.c_str(), dir == DIR_IN ? "IN" : "OUT");
        } else {
            Logger.logf(LOG_WARN, "CARD", "DENIED  uid=%s  reason=%s  dir=%s",
                        uid.c_str(), resp.reason.c_str(), dir == DIR_IN ? "IN" : "OUT");
        }
        return resp;
    }

    // POST /device/employees/batch (§10.3). HTTP 200 now carries both
    // halves: `ids` (saved) and `errors` (refused, per record). Only the
    // records named in `ids` are marked synced — the old code marked every
    // employee on the device synced after any 200.
    int syncEmployees() {
        if (!serverReachable()) return -1;
        String body;
        int count;
        {
            JsonDocument doc;
            JsonArray arr = doc["employees"].to<JsonArray>();
            if (!SdManager.getUnsyncedEmployees(arr)) return 0;
            count = (int)arr.size();
            serializeJson(doc, body);
        }
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Employees sync: mutex timeout");
            return -1;
        }
        HTTPClient http;
        http.begin(_wcs, Config.serverUrl + "/device/employees/batch");
        http.setTimeout(8000); auth(http);
        int code = http.POST(body);
        body = String();
        String respBody = code > 0 ? http.getString() : "";
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        if (code != 200 && code != 201) {
            Logger.log(LOG_ERROR, "SYNC", "Employees sync failed",
                       code > 0 ? serverErrMsg(code, respBody) : transportErrMsg(code));
            return -1;
        }
        JsonDocument r;
        if (deserializeJson(r, respBody)) {
            Logger.log(LOG_ERROR, "SYNC", "Employees sync: unreadable response", respBody.substring(0, 120));
            return -1;
        }
        JsonArrayConst ids  = r["ids"].as<JsonArrayConst>();
        JsonArrayConst errs = r["errors"].as<JsonArrayConst>();
        for (JsonObjectConst e : errs) {
            String fields; serializeJson(e["fields"], fields);
            String detail = String(e["local_id"] | "?") + " " + fields;
            Logger.log(LOG_WARN, "SYNC", "Employee refused by server — fix it in the panel to retry",
                       detail.substring(0, 200));
        }
        _syncErrors = SdManager.applyEmployeeSyncResult(ids, errs);
        int ok = (int)ids.size();
        Logger.logf(LOG_INFO, "SYNC", "Employees synced: %d of %d (refused: %d)",
                    ok, count, (int)errs.size());
        return ok;
    }

    // POST /device/events/batch (§10.4). Every event carries its event_id;
    // the answer lists accepted / duplicates / rejected by id, and exactly
    // those lines are marked synced. Rejected ones are logged and not resent.
    int syncEvents() {
        if (!serverReachable()) return -1;
        int n = SdManager.unsyncedCount();
        if (n == 0) return 0;

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        if (!SdManager.getUnsyncedEvents(arr, 15)) {
            int dropped = SdManager.dropUnsendableEvents();
            if (dropped > 0)
                Logger.logf(LOG_WARN, "SYNC", "Dropping %d event(s) recorded before the clock was set", dropped);
            return 0;
        }
        int batchSize = (int)arr.size();
        const char* boundary = "----ESP32Bnd9f2a";

        // ── Find photos for these events ───────────────────────────────────────
        struct PInfo { String path; size_t size = 0; bool ok = false; };
        PInfo photos[15];
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
        // Streaming from SD keeps the photo bytes out of RAM during the TLS
        // handshake. Field names go through a fixed buffer, not String +.
        const char* tmpPath = "/data/batch.tmp";
        {
            File tmp = SD.open(tmpPath, FILE_WRITE);
            if (!tmp) {
                Logger.log(LOG_ERROR, "SYNC", "Cannot create batch temp file");
                return -1;
            }
            char name[40];
            auto fld = [&](int i, const char* key, const char* val) {
                snprintf(name, sizeof(name), "events[%d][%s]", i, key);
                tmp.print("--"); tmp.print(boundary);
                tmp.print("\r\nContent-Disposition: form-data; name=\"");
                tmp.print(name); tmp.print("\"\r\n\r\n");
                tmp.print(val); tmp.print("\r\n");
            };
            bool toggle = (Config.directionMode == "toggle");
            for (int i = 0; i < batchSize; i++) {
                fld(i, "event_id",    arr[i]["event_id"]    | "");
                fld(i, "uid",         arr[i]["uid"]         | "");
                fld(i, "happened_at", arr[i]["happened_at"] | "");
                fld(i, "decision",    arr[i]["decision"]    | "denied");
                fld(i, "direction",   toggle ? "unknown" : (arr[i]["direction"] | "unknown"));
                const char* rsn = arr[i]["reason"] | "";
                if (*rsn) fld(i, "reason", rsn);
            }
            uint8_t chunk[512];
            for (int i = 0; i < batchSize; i++) {
                if (!photos[i].ok) continue;
                tmp.print("--"); tmp.print(boundary);
                tmp.print("\r\nContent-Disposition: form-data; name=\"photos[");
                tmp.print(i); tmp.print("]\"; filename=\"p.jpg\"\r\n"
                    "Content-Type: image/jpeg\r\n\r\n");
                File pf = SD.open(photos[i].path, FILE_READ);
                if (pf) {
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
        HTTPClient http;
        http.begin(_wcs, Config.serverUrl + "/device/events/batch");
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

        if (code != 200 && code != 201) {
            Logger.log(LOG_ERROR, "SYNC", "Events sync failed",
                       code > 0 ? serverErrMsg(code, respBody) : transportErrMsg(code));
            return -1;
        }

        // ── Which events did the server answer for? ────────────────────────────
        std::vector<String> done;
        int nAcc = 0, nDup = 0, nRej = 0;
        JsonDocument r;
        bool listed = !deserializeJson(r, respBody) && r["accepted"].is<JsonArray>();
        respBody = String();
        if (listed) {
            for (JsonVariantConst v : r["accepted"].as<JsonArrayConst>())   { done.push_back(v.as<String>()); nAcc++; }
            for (JsonVariantConst v : r["duplicates"].as<JsonArrayConst>()) { done.push_back(v.as<String>()); nDup++; }
            for (JsonObjectConst rj : r["rejected"].as<JsonArrayConst>()) {
                String id = rj["event_id"] | "";
                int idx = rj["index"] | -1;
                if (id.isEmpty() && idx >= 0 && idx < batchSize) id = arr[idx]["event_id"] | "";
                if (id.isEmpty()) continue;
                done.push_back(id); nRej++;
                Logger.log(LOG_WARN, "SYNC", "Event rejected by server — not resending",
                           id + ": " + String(rj["error"] | "?"));
            }
        } else {
            // Pre-v0.6.0 server: answers with counts only. A 200 means the
            // whole batch was taken (it validated the batch as one).
            for (int i = 0; i < batchSize; i++) done.push_back(arr[i]["event_id"] | "");
            nAcc = batchSize;
        }
        SdManager.markEventsSynced(done);

        int nPhotos = 0;
        for (int i = 0; i < batchSize; i++) {
            if (photos[i].path.isEmpty()) continue;
            String id = arr[i]["event_id"] | "";
            if (std::find(done.begin(), done.end(), id) == done.end()) continue;  // will be resent with it
            SD.remove(photos[i].path.c_str()); nPhotos++;
        }
        Logger.logf(LOG_INFO, "SYNC", "Events: %d sent, %d accepted, %d duplicate, %d rejected (of %d), photos: %d",
                    batchSize, nAcc, nDup, nRej, n, nPhotos);
        return (int)done.size();
    }

    // Whitelist sync. Delta by default (§10.2): GET /device/sync?since=<v>,
    // paged with an opaque cursor, applied page by page; the version is
    // stored only after the last page is on SD, and checked against the
    // server's checksum. A mismatch triggers one full resync.
    bool syncWhitelist() {
        int r = syncWhitelistPass(false);
        if (r == WL_MISMATCH) r = syncWhitelistPass(true);
        if (r == WL_OK || r == WL_MISMATCH) whitelistStale = false;
        return r != WL_FAIL;
    }

private:
    enum { WL_FAIL = 0, WL_OK = 1, WL_MISMATCH = 2 };

    void applyServerConfig(JsonDocument& d) {
        bool changed = false;
        if (!d["relay_ms"].isNull() && (int)d["relay_ms"] != Config.relayMs) {
            Config.relayMs = d["relay_ms"]; changed = true;
        }
        if (!d["config_version"].isNull() && (int)d["config_version"] != Config.configVersion) {
            Config.configVersion = d["config_version"]; changed = true;
        }
        if (!d["direction_mode"].isNull() && d["direction_mode"].as<String>() != Config.directionMode) {
            Config.directionMode = d["direction_mode"].as<String>(); changed = true;
        }
        // Only write NVS when something changed — this used to run every sync.
        if (changed) { Config.save(); SdManager.backupConfig(); }
    }

    // Fetch one GET into `payload`. Holds _mutex only for the request.
    int getJson(const String& url, String& payload) {
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_ERROR, "SYNC", "Whitelist sync: mutex timeout");
            return -1;
        }
        HTTPClient http;
        http.begin(_wcs, url);
        http.setTimeout(10000); auth(http);
        const char* dateHdr[] = {"Date"};
        http.collectHeaders(dateHdr, 1);
        int code = http.GET();
        if (code == 200 && !_ntpSynced && http.hasHeader("Date"))
            syncTimeFromHttpDate(http.header("Date"));
        payload = code > 0 ? http.getString() : "";
        http.end();
        xSemaphoreGiveRecursive(_mutex);
        return code;
    }

    int syncWhitelistPass(bool forceFull) {
        if (!serverReachable()) return WL_FAIL;
        long since = forceFull ? 0 : Config.whitelistVersion;
        String cursor;
        std::vector<int> seen;
        bool full = false;
        int nUp = 0, nDel = 0;
        // Small pages: the response is held in RAM twice (text + parsed), and
        // the server's own cap is ~28 KB. 50 records keeps it around 8 KB.
        for (int page = 0; page < 200; page++) {
            String url = Config.serverUrl + "/device/sync?limit=50&";
            url += cursor.length() ? ("cursor=" + cursor) : ("since=" + String(since));
            String payload;
            int code = getJson(url, payload);
            if (code != 200) {
                Logger.log(LOG_ERROR, "SYNC", "Whitelist fetch failed",
                           code > 0 ? serverErrMsg(code, payload) : transportErrMsg(code));
                return WL_FAIL;
            }
            JsonDocument d;
            DeserializationError err = deserializeJson(d, payload);
            if (err) {
                Logger.logf(LOG_ERROR, "SYNC", "Whitelist page unreadable: %s (%u bytes, maxBlock=%u)",
                            err.c_str(), payload.length(), ESP.getMaxAllocHeap());
                return WL_FAIL;
            }
            if (page == 0) applyServerConfig(d);

            if (!d["upserts"].is<JsonArray>()) {
                // Pre-v0.6.0 server ignored ?since= and sent the full legacy list.
                d.clear();
                bool ok = SdManager.updateWhitelist(payload);
                if (ok) Logger.log(LOG_INFO, "SYNC", "Whitelist updated (legacy full list)");
                else    Logger.log(LOG_ERROR, "SYNC", "Whitelist update failed — legacy payload not understood");
                return ok ? WL_OK : WL_FAIL;
            }
            payload = String();  // free the text before touching SD

            if (page == 0) full = d["full"] | false;
            if (!SdManager.applySyncPage(d.as<JsonObjectConst>(), full, seen, nUp, nDel))
                return WL_FAIL;

            if (d["has_more"] | false) {
                cursor = d["cursor"] | "";
                if (cursor.isEmpty()) {
                    Logger.log(LOG_ERROR, "SYNC", "Whitelist: has_more without cursor");
                    return WL_FAIL;
                }
                continue;
            }

            // ── Last page ─────────────────────────────────────────────────────
            if (full) SdManager.finishFullSync(seen);
            long version = d["version"] | 0L;
            int cnt = 0; String hash; bool comparable = false;
            if (!SdManager.rebuildWhitelist(version, cnt, hash, comparable)) {
                Logger.log(LOG_ERROR, "SYNC", "Whitelist: cannot rebuild whitelist.json");
                return WL_FAIL;
            }
            int  srvCnt  = d["checksum"]["cards"] | -1;
            String srvHash = d["checksum"]["hash"] | "";
            bool mismatch = comparable && srvCnt >= 0 && srvHash.length()
                          && (srvCnt != cnt || !srvHash.equalsIgnoreCase(hash));
            if (mismatch && !forceFull) {
                Logger.logf(LOG_WARN, "SYNC", "Whitelist checksum mismatch (server %d cards, local %d) — full resync",
                            srvCnt, cnt);
                Config.whitelistVersion = 0;  // if the full pass fails, the next attempt is full too
                return WL_MISMATCH;
            }
            if (mismatch)
                Logger.logf(LOG_ERROR, "SYNC", "Whitelist still differs after full sync (server %d cards, local %d)",
                            srvCnt, cnt);
            Config.whitelistVersion = version;
            if (full || nUp || nDel)
                Logger.logf(LOG_INFO, "SYNC", "Whitelist %s → v%ld (%d updated, %d removed, %d active cards)",
                            full ? "full" : "delta", version, nUp, nDel, cnt);
            return mismatch ? WL_MISMATCH : WL_OK;
        }
        Logger.log(LOG_ERROR, "SYNC", "Whitelist: too many pages — giving up this cycle");
        return WL_FAIL;
    }

public:
    void sendHeartbeat() {
        if (!serverReachable()) return;
        String body;
        {
            JsonDocument doc;
            doc["firmware"]        = FIRMWARE_VERSION;
            if (camFirmwareVersion.length() > 0)
                doc["cam_firmware"] = camFirmwareVersion;
            doc["ip"]              = WiFi.localIP().toString();
            doc["rssi"]            = WiFi.RSSI();
            doc["config_version"]  = Config.configVersion;
            doc["unsynced_events"] = SdManager.unsyncedCount();
            doc["sd_mounted"]      = SdManager.isMounted();
            doc["uptime_s"]        = millis()/1000;
            // §10.6 diagnostics — all optional on the server side.
            doc["whitelist_version"] = Config.whitelistVersion;
            doc["heap_free"]       = ESP.getFreeHeap();
            doc["heap_min"]        = ESP.getMinFreeHeap();
            doc["heap_max_block"]  = ESP.getMaxAllocHeap();
            doc["reset_reason"]    = _resetReason;
            doc["sync_errors"]     = _syncErrors;
            doc["nfc_in_ok"]       = NfcReader.isOk(READER_IN);
            doc["nfc_out_ok"]      = NfcReader.isOk(READER_OUT);
            if (camOkFn) doc["cam_ok"] = camOkFn();
            if (SdManager.isMounted())
                doc["sd_free_mb"]  = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
            serializeJson(doc, body);
        }
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(10000))) {
            Logger.log(LOG_WARN, "API", "Heartbeat: mutex timeout");
            return;
        }
        HTTPClient http;
        http.begin(_wcs, Config.serverUrl + "/device/heartbeat");
        http.setTimeout(5000); auth(http);
        int code = http.POST(body);
        String resp = code > 0 ? http.getString() : "";
        http.end();
        xSemaphoreGiveRecursive(_mutex);

        if (code != 200) {
            Logger.log(LOG_WARN, "API", "Heartbeat failed",
                       code > 0 ? serverErrMsg(code, resp) : transportErrMsg(code));
            return;
        }
        JsonDocument d;
        if (deserializeJson(d, resp)) return;
        if (d["resync"] | false) {
            Logger.log(LOG_INFO, "SYNC", "Server requested resync via heartbeat");
            whitelistStale = true;
        }
        if (!d["whitelist_version"].isNull()) {
            long srv = d["whitelist_version"] | 0L;
            if (srv != Config.whitelistVersion) whitelistStale = true;
        }
        const char* otaUrl = d["ota_url"];
        const char* otaVer = d["ota_version"];
        if (otaUrl && *otaUrl && otaVer && *otaVer) {
            pendingOtaUrl     = otaUrl;
            pendingOtaVersion = otaVer;
            pendingOtaSha256  = d["ota_sha256"] | "";
            pendingOtaSha256.toLowerCase();
            pendingOtaSize    = d["ota_size"] | 0UL;
        }
        const char* camUrl = d["cam_ota_url"];
        const char* camVer = d["cam_ota_version"];
        if (camUrl && *camUrl && camVer && *camVer) {
            pendingCamOtaUrl     = camUrl;
            pendingCamOtaVersion = camVer;
        }
    }

    int proxy(const String& method, const String& path,
              const String& body, String& out) {
        if (!serverReachable()) {
            out = "{\"error\":\"device_offline\",\"offline\":true}"; return 503;
        }
        // Own client, but taken under the same mutex as sync requests, so two
        // TLS sessions (40–50 KB each) are never alive at once — that overlap
        // is what exhausted the heap when the panel was open during a sync.
        if (!xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(15000))) {
            out = "{\"error\":\"busy\"}"; return 503;
        }
        struct Give { SemaphoreHandle_t m; ~Give() { xSemaphoreGiveRecursive(m); } } _give{_mutex};
        _wcs.stop();  // drop the idle sync session before opening a second one
        WiFiClientSecure wcs; setupSecureClient(wcs);
        HTTPClient http;
        http.begin(wcs, Config.serverUrl + path);
        http.setTimeout(10000); auth(http);
        int code;
        if      (method=="GET")    code = http.GET();
        else if (method=="POST")   code = http.POST(body);
        else if (method=="PUT")    code = http.PUT(body);
        else if (method=="DELETE") code = http.sendRequest("DELETE");
        else                       { out="{}"; return 405; }
        out = http.getString(); http.end();
        return code;
    }
} ApiClient;
