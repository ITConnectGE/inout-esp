#pragma once
/**
 * sd_manager.h — SD card on dedicated HSPI (SCK=17 MISO=16 MOSI=33 CS=5)
 * PN532 readers are on VSPI — completely separate, zero conflict.
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <vector>
#include "config.h"
#include "logger.h"

#define EMPLOYEES_FILE "/data/employees.json"
#define WHITELIST_FILE "/data/whitelist.json"
#define EVENTS_LOG     "/data/events.log"
#define ADMINS_FILE    "/data/admins.json"
#define CONFIG_BACKUP  "/data/config.json"
#define MAX_LOG_LINES  2000

extern SPIClass spiHSPI;

struct SdManagerClass {
private:
    bool _mounted = false;

    String mime(const String& p) {
        if (p.endsWith(".html")||p.endsWith(".htm")) return "text/html";
        if (p.endsWith(".css"))   return "text/css";
        if (p.endsWith(".js"))    return "application/javascript";
        if (p.endsWith(".json"))  return "application/json";
        if (p.endsWith(".png"))   return "image/png";
        if (p.endsWith(".jpg"))   return "image/jpeg";
        if (p.endsWith(".svg"))   return "image/svg+xml";
        if (p.endsWith(".ico"))   return "image/x-icon";
        return "application/octet-stream";
    }

    void mkdirP(const char* path) { if (!SD.exists(path)) SD.mkdir(path); }

    String makeLocalId() {
        char buf[24];
        snprintf(buf, sizeof(buf), "loc_%lu_%04x", millis(), (unsigned)random(0xFFFF));
        return String(buf);
    }

public:
    bool begin() {
        // Dedicated HSPI bus — start it here
        spiHSPI.begin(PIN_HSPI_SCK, PIN_HSPI_MISO, PIN_HSPI_MOSI);
        delay(100);

        pinMode(Config.csPin_SD, OUTPUT);
        digitalWrite(Config.csPin_SD, HIGH);

        // 80 clock pulses with CS HIGH per SD spec
        spiHSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
        for (int i = 0; i < 10; i++) spiHSPI.transfer(0xFF);
        spiHSPI.endTransaction();
        delay(10);

        if (!SD.begin(Config.csPin_SD, spiHSPI, 4000000)) {
            Logger.log(LOG_ERROR, "SD", "Mount failed — check card and CS pin",
                       "cs=" + String(Config.csPin_SD));
            _mounted = false; return false;
        }
        Logger.logf(LOG_INFO, "SD", "Mounted: %lluMB  type=%d",
                    SD.cardSize()/(1024*1024), SD.cardType());
        mkdirP("/data"); mkdirP("/www"); mkdirP("/photos");
        if (!SD.exists(EMPLOYEES_FILE)) {
            File f = SD.open(EMPLOYEES_FILE, FILE_WRITE);
            if (f) { f.print("{\"employees\":[],\"updated_at\":0}"); f.close(); }
            else Logger.log(LOG_ERROR, "SD", "Failed to create employees file");
        }
        if (!SD.exists(WHITELIST_FILE)) {
            File f = SD.open(WHITELIST_FILE, FILE_WRITE);
            if (f) { f.print("{\"uids\":[]}"); f.close(); }
            else Logger.log(LOG_ERROR, "SD", "Failed to create whitelist file");
        }
        _mounted = true;
        return true;
    }

    bool isMounted()   { return _mounted; }
    uint64_t totalMB() { return _mounted ? SD.cardSize()/(1024*1024) : 0; }
    uint64_t usedMB()  { return _mounted ? SD.usedBytes()/(1024*1024) : 0; }

    String lookupUid(const String& uid) {
        if (!_mounted) return "";
        // Check locally-added employees first (have full names)
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f); f.close();
            if (!err) {
                for (JsonObject emp : doc["employees"].as<JsonArray>()) {
                    if (String(emp["status"] | "active") != "active") continue;
                    for (JsonObject card : emp["cards"].as<JsonArray>())
                        if (String(card["uid"] | "") == uid &&
                            String(card["status"] | "active") == "active") {
                            String name = String(emp["first_name"]|"") + " " + String(emp["last_name"]|"");
                            name.trim(); return name;
                        }
                }
            } else {
                Logger.logf(LOG_ERROR, "SD", "employees.json parse error: %s", err.c_str());
            }
        } else {
            Logger.log(LOG_ERROR, "SD", "Cannot open employees.json for UID lookup");
        }
        // Fall back to server-synced flat whitelist
        File wf = SD.open(WHITELIST_FILE, FILE_READ);
        if (wf) {
            JsonDocument wdoc;
            DeserializationError werr = deserializeJson(wdoc, wf); wf.close();
            if (!werr) {
                for (JsonVariant u : wdoc["uids"].as<JsonArray>())
                    if (String(u | "") == uid) return "Authorized";
            } else {
                Logger.logf(LOG_ERROR, "SD", "whitelist.json parse error: %s", werr.c_str());
            }
        }
        return "";
    }

    String addEmployee(const String& firstName, const String& lastName,
                       const String& position,  const String& department,
                       const String& phone,     const String& email,
                       const String& externalId) {
        if (!_mounted) return "";
        JsonDocument doc;
        if (SD.exists(EMPLOYEES_FILE)) {
            File f = SD.open(EMPLOYEES_FILE, FILE_READ);
            if (f) {
                DeserializationError err = deserializeJson(doc, f); f.close();
                if (err) Logger.logf(LOG_WARN, "SD", "employees.json parse error on add: %s", err.c_str());
            }
        }
        if (!doc["employees"].is<JsonArray>()) doc["employees"].to<JsonArray>();
        String localId = makeLocalId();
        JsonObject emp = doc["employees"].add<JsonObject>();
        emp["id"] = 0; emp["local_id"] = localId;
        emp["first_name"] = firstName; emp["last_name"] = lastName;
        emp["position"] = position; emp["department"] = department;
        emp["phone"] = phone; emp["email"] = email;
        emp["external_id"] = externalId; emp["status"] = "active";
        emp["synced"] = false; emp["created_at"] = (long)(millis()/1000);
        emp["cards"].to<JsonArray>();
        File f = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json — addEmployee failed",
                       firstName + " " + lastName);
            return "";
        }
        serializeJson(doc, f); f.close();
        Logger.logf(LOG_INFO, "SD", "Employee added: %s %s  id=%s",
                    firstName.c_str(), lastName.c_str(), localId.c_str());
        return localId;
    }

    bool addCard(const String& localId, const String& uid,
                 const String& label,   const String& type) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot open employees.json — addCard failed",
                       "uid=" + uid + " localId=" + localId);
            return false;
        }
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) {
            Logger.logf(LOG_ERROR, "SD", "employees.json parse error on addCard: %s", err.c_str());
            return false;
        }
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (String(emp["local_id"]|"") == localId) {
                JsonObject card = emp["cards"].add<JsonObject>();
                card["uid"]    = uid;
                card["label"]  = label.length() > 0 ? label : ("Card #" + uid.substring(0,4));
                card["type"]   = type.length() > 0 ? type : "mifare_uid";
                card["status"] = "active";
                card["issued_at"] = (long)(millis()/1000);
                emp["synced"] = false;
                File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
                if (!out) {
                    Logger.log(LOG_ERROR, "SD", "Cannot write employees.json — addCard failed",
                               "uid=" + uid + " localId=" + localId);
                    return false;
                }
                serializeJson(doc, out); out.close();
                Logger.logf(LOG_INFO, "SD", "Card assigned: uid=%s to localId=%s",
                            uid.c_str(), localId.c_str());
                return true;
            }
        }
        Logger.log(LOG_WARN, "SD", "addCard: employee not found", "localId=" + localId);
        return false;
    }

    bool updateEmployee(const String& localId, const JsonObject& updates) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot open employees.json — update failed",
                       "localId=" + localId);
            return false;
        }
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) {
            Logger.logf(LOG_ERROR, "SD", "employees.json parse error on update: %s", err.c_str());
            return false;
        }
        bool found = false;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (String(emp["local_id"]|"") == localId) {
                if (!updates["first_name"].isNull()) emp["first_name"] = updates["first_name"];
                if (!updates["last_name"].isNull())  emp["last_name"]  = updates["last_name"];
                if (!updates["department"].isNull()) emp["department"] = updates["department"];
                if (!updates["position"].isNull())   emp["position"]   = updates["position"];
                if (!updates["phone"].isNull())      emp["phone"]      = updates["phone"];
                if (!updates["email"].isNull())      emp["email"]      = updates["email"];
                if (!updates["status"].isNull())     emp["status"]     = updates["status"];
                emp["synced"] = false;
                found = true; break;
            }
        }
        if (!found) {
            Logger.log(LOG_WARN, "SD", "updateEmployee: employee not found", "localId=" + localId);
            return false;
        }
        File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!out) {
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json — update failed",
                       "localId=" + localId);
            return false;
        }
        serializeJson(doc, out); out.close();
        Logger.log(LOG_INFO, "SD", "Employee updated", "localId=" + localId);
        return true;
    }

    bool deleteEmployee(const String& localId) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot open employees.json — delete failed",
                       "localId=" + localId);
            return false;
        }
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) {
            Logger.logf(LOG_ERROR, "SD", "employees.json parse error on delete: %s", err.c_str());
            return false;
        }
        JsonDocument newDoc;
        JsonArray newArr = newDoc["employees"].to<JsonArray>();
        newDoc["updated_at"] = doc["updated_at"];
        bool found = false;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (String(emp["local_id"]|"") == localId) { found = true; continue; }
            newArr.add(emp);
        }
        if (!found) {
            Logger.log(LOG_WARN, "SD", "deleteEmployee: employee not found", "localId=" + localId);
            return false;
        }
        File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!out) {
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json — delete failed",
                       "localId=" + localId);
            return false;
        }
        serializeJson(newDoc, out); out.close();
        Logger.log(LOG_INFO, "SD", "Employee deleted", "localId=" + localId);
        return true;
    }

    bool getUnsyncedEmployees(JsonArray& out) {
        if (!_mounted || !SD.exists(EMPLOYEES_FILE)) return false;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot open employees.json for sync read");
            return false;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) {
            Logger.logf(LOG_ERROR, "SD", "employees.json parse error on sync: %s", err.c_str());
            return false;
        }
        int n = 0;
        for (JsonObject emp : doc["employees"].as<JsonArray>())
            if (!(emp["synced"]|false)) { out.add(emp); n++; }
        return n > 0;
    }

    void markEmployeesSynced(const JsonArray& serverIds) {
        if (!_mounted || !SD.exists(EMPLOYEES_FILE)) return;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return;
        deserializeJson(doc, f); f.close();
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            emp["synced"] = true;
            for (JsonObject sid : serverIds)
                if (String(sid["local_id"]|"") == String(emp["local_id"]|""))
                    emp["id"] = sid["server_id"]|0;
        }
        File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (out) { serializeJson(doc, out); out.close(); }
    }

    bool updateWhitelist(const String& json) {
        if (!_mounted) return false;
        JsonDocument doc;
        if (deserializeJson(doc, json)) return false;
        if (!doc["whitelist"].is<JsonArray>()) return false;

        // Step 1: save flat UID whitelist (scoped so wdoc is freed before step 2)
        {
            JsonDocument wdoc;
            JsonArray uids = wdoc["uids"].to<JsonArray>();
            for (JsonVariant u : doc["whitelist"].as<JsonArray>())
                uids.add(u.as<String>());
            File wf = SD.open(WHITELIST_FILE, FILE_WRITE);
            if (!wf) return false;
            serializeJson(wdoc, wf); wf.close();
        }

        if (doc["employees"].is<JsonArray>()) {
            // Step 2a: collect unsynced local employees before overwriting
            // (scoped so `existing` doc is freed before building empDoc)
            std::vector<String> locals;
            {
                File f = SD.open(EMPLOYEES_FILE, FILE_READ);
                if (f) {
                    JsonDocument existing;
                    if (!deserializeJson(existing, f)) {
                        for (JsonObject e : existing["employees"].as<JsonArray>()) {
                            if (!(e["synced"] | true)) {
                                String s; serializeJson(e, s);
                                locals.push_back(s);
                            }
                        }
                    }
                    f.close();
                }
            }

            // Step 2b: rebuild employees.json from server data + unsynced locals.
            // Build a set of server IDs already covered by a local unsynced edit so
            // we don't create a duplicate entry for the same person.
            std::vector<int> localServerIds;
            for (const String& line : locals) {
                JsonDocument tmp;
                if (!deserializeJson(tmp, line)) {
                    int sid = tmp["id"] | 0;
                    if (sid > 0) localServerIds.push_back(sid);
                }
            }

            JsonDocument empDoc;
            JsonArray arr = empDoc["employees"].to<JsonArray>();
            for (JsonObject srv : doc["employees"].as<JsonArray>()) {
                int srvId = srv["id"] | 0;
                bool hasLocalEdit = false;
                for (int lid : localServerIds)
                    if (lid == srvId) { hasLocalEdit = true; break; }
                if (hasLocalEdit) continue; // local version will be appended below

                JsonObject emp = arr.add<JsonObject>();
                emp["id"]         = srvId;
                emp["local_id"]   = String("srv_") + String(srvId);
                emp["first_name"] = srv["first_name"] | "";
                emp["last_name"]  = srv["last_name"]  | "";
                emp["department"] = srv["department"] | "";
                emp["position"]   = srv["position"]   | "";
                emp["phone"]      = srv["phone"]      | "";
                emp["email"]      = srv["email"]      | "";
                emp["status"]     = "active";
                emp["synced"]     = true;
                JsonArray cards   = emp["cards"].to<JsonArray>();
                for (JsonObject c : srv["cards"].as<JsonArray>()) {
                    JsonObject card  = cards.add<JsonObject>();
                    card["uid"]    = c["uid"]    | "";
                    card["status"] = c["status"] | "active";
                }
            }
            for (const String& line : locals) {
                JsonDocument tmp;
                if (!deserializeJson(tmp, line)) arr.add(tmp.as<JsonObject>());
            }
            empDoc["updated_at"] = (long)(millis() / 1000);
            File ef = SD.open(EMPLOYEES_FILE, FILE_WRITE);
            if (!ef) return false;
            serializeJson(empDoc, ef); ef.close();
        } else {
            // No employees in response — just bump updated_at
            JsonDocument empDoc;
            if (SD.exists(EMPLOYEES_FILE)) {
                File f = SD.open(EMPLOYEES_FILE, FILE_READ);
                if (f) { deserializeJson(empDoc, f); f.close(); }
            }
            if (!empDoc["employees"].is<JsonArray>()) empDoc["employees"].to<JsonArray>();
            empDoc["updated_at"] = (long)(millis() / 1000);
            File ef = SD.open(EMPLOYEES_FILE, FILE_WRITE);
            if (!ef) return false;
            serializeJson(empDoc, ef); ef.close();
        }

        return true;
    }

    long whitelistUpdatedAt() {
        if (!_mounted || !SD.exists(EMPLOYEES_FILE)) return 0;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return 0;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        return err ? 0 : (long)(doc["updated_at"]|0);
    }

    int employeeCount() {
        if (!_mounted || !SD.exists(EMPLOYEES_FILE)) return 0;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return 0;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        return err ? 0 : (int)doc["employees"].as<JsonArray>().size();
    }

    void logEvent(const String& uid, const String& name,
                  const String& direction, const String& decision,
                  const String& happened_at, const String& reason) {
        if (!_mounted) return;
        File f = SD.open(EVENTS_LOG, FILE_APPEND);
        if (!f) {
            // Can't use Logger here (different log file) — fall back to Serial
            Serial.printf("[ERROR][SD] Cannot append to events.log  uid=%s\n", uid.c_str());
            return;
        }
        JsonDocument doc;
        // Keep "dir" and "ts" for local panel compatibility; "reason" is new
        doc["uid"]=uid; doc["name"]=name; doc["dir"]=direction;
        doc["decision"]=decision; doc["ts"]=happened_at;
        doc["reason"]=reason; doc["synced"]=false;
        serializeJson(doc, f); f.println(); f.close();
    }

    int unsyncedCount() {
        if (!_mounted || !SD.exists(EVENTS_LOG)) return 0;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return 0;
        int n = 0;
        while (f.available()) {
            String l = f.readStringUntil('\n'); l.trim();
            if (l.length() > 5 && l.indexOf("\"synced\":false") >= 0) n++;
        }
        f.close(); return n;
    }

    bool getUnsyncedEvents(JsonArray& out, int limit = 50) {
        if (!_mounted || !SD.exists(EVENTS_LOG)) return false;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return false;
        int n = 0;
        while (f.available() && n < limit) {
            String l = f.readStringUntil('\n'); l.trim();
            if (l.length() < 5 || l.indexOf("\"synced\":false") < 0) continue;
            JsonDocument tmp;
            if (deserializeJson(tmp, l)) continue;
            JsonObject src = tmp.as<JsonObject>();
            // Support both log formats:
            //   current:      "dir" + "ts"
            //   transitional: "direction" + "happened_at"  (logged between firmware edits)
            String ts_val  = src["ts"].isNull()
                ? String(src["happened_at"] | "")
                : String(src["ts"]          | "");
            String dir_val = src["dir"].isNull()
                ? String(src["direction"] | "unknown")
                : String(src["dir"]       | "unknown");
            // Skip events whose timestamp is empty or the pre-NTP fallback ("~NNNms").
            // markAllSynced() will clear them once the valid batch succeeds.
            if (ts_val.length() == 0 || ts_val[0] == '~') continue;
            JsonObject dst = out.add<JsonObject>();
            dst["uid"]         = src["uid"]      | "";
            dst["direction"]   = dir_val;
            dst["decision"]    = src["decision"] | "";
            dst["happened_at"] = ts_val;
            String rsn = src["reason"] | "";
            if (rsn.length() > 0) dst["reason"] = rsn;
            n++;
        }
        f.close(); return n > 0;
    }

    // Return the SD path of a photo that matches uid + ISO timestamp, or "" if not found.
    // Photo filename format: /photos/<uid>_<YYYYMMDD>_<HHMMSS>.jpg
    String getPhotoForEvent(const String& uid, const String& ts) {
        if (!_mounted || uid.isEmpty() || ts.length() < 19) return "";
        String dateStr = ts.substring(0,4) + ts.substring(5,7) + ts.substring(8,10);
        String timeStr = ts.substring(11,13) + ts.substring(14,16) + ts.substring(17,19);
        String path = "/photos/" + uid + "_" + dateStr + "_" + timeStr + ".jpg";
        return SD.exists(path) ? path : "";
    }

    // Mark the first n unsynced events in events.log as synced (in file order).
    void markNEventsSynced(int n) {
        if (!_mounted || !SD.exists(EVENTS_LOG) || n <= 0) return;
        String tmp = "/data/ev.tmp";
        File src = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!src || !dst) { if(src)src.close(); if(dst)dst.close(); return; }
        int marked = 0;
        while (src.available()) {
            String l = src.readStringUntil('\n'); l.trim();
            if (l.length() < 5) continue;
            if (marked < n && l.indexOf("\"synced\":false") >= 0) {
                l.replace("\"synced\":false", "\"synced\":true");
                marked++;
            }
            dst.println(l);
        }
        src.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
    }

    void markAllSynced() {
        if (!_mounted || !SD.exists(EVENTS_LOG)) return;
        String tmp = "/data/ev.tmp";
        File src = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!src || !dst) { if(src)src.close(); if(dst)dst.close(); return; }
        while (src.available()) {
            String l = src.readStringUntil('\n'); l.trim();
            if (l.length() < 5) continue;
            l.replace("\"synced\":false", "\"synced\":true");
            dst.println(l);
        }
        src.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
    }

    void trimLog() {
        if (!_mounted || !SD.exists(EVENTS_LOG)) return;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return;
        int total = 0;
        while (f.available()) { f.readStringUntil('\n'); total++; }
        f.close();
        if (total <= MAX_LOG_LINES) return;
        int skip = total - MAX_LOG_LINES;
        String tmp = "/data/ev.tmp";
        f = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!f || !dst) { if(f)f.close(); if(dst)dst.close(); return; }
        int i = 0;
        while (f.available()) { String l = f.readStringUntil('\n'); if(i++>=skip) dst.println(l); }
        f.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
    }

    void backupConfig() {
        if (!_mounted) return;
        File f = SD.open(CONFIG_BACKUP, FILE_WRITE);
        if (!f) return;
        JsonDocument doc;
        doc["server_url"] = Config.serverUrl;
        doc["identifier"] = Config.identifier;
        doc["config_version"] = Config.configVersion;
        doc["relay_ms"] = Config.relayMs;
        doc["saved_at"] = (long)(millis()/1000);
        serializeJson(doc, f); f.close();
    }

    bool serveFile(WebServer& server, const String& path) {
        if (!_mounted || !SD.exists(path)) return false;
        File f = SD.open(path, FILE_READ);
        if (!f || f.isDirectory()) { if(f)f.close(); return false; }
        server.sendHeader("Cache-Control", "max-age=60");
        server.streamFile(f, mime(path)); f.close(); return true;
    }

    // Save a JPEG from ESP32-CAM; path: /photos/<uid>_<YYYYMMDD>_<HHMMSS>.jpg
    // happenedAt: ISO timestamp from the event log (e.g. "2026-08-04T14:30:22Z").
    // When provided, the photo filename is derived from it so it matches exactly.
    bool savePhoto(const String& uid, const uint8_t* data, size_t len,
                   const String& happenedAt = "") {
        if (!_mounted || len == 0) return false;
        char path[72];
        if (happenedAt.length() >= 19) {
            // Derive filename from the same timestamp used in the event log.
            String d = happenedAt.substring(0,4)  + happenedAt.substring(5,7)
                     + happenedAt.substring(8,10);                    // YYYYMMDD
            String t = happenedAt.substring(11,13) + happenedAt.substring(14,16)
                     + happenedAt.substring(17,19);                   // HHMMSS
            snprintf(path, sizeof(path), "/photos/%s_%s_%s.jpg",
                     uid.c_str(), d.c_str(), t.c_str());
        } else {
            struct tm t;
            if (getLocalTime(&t, 50))
                snprintf(path, sizeof(path), "/photos/%s_%04d%02d%02d_%02d%02d%02d.jpg",
                         uid.c_str(),
                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec);
            else
                snprintf(path, sizeof(path), "/photos/%s_%lu.jpg", uid.c_str(), millis());
        }

        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Logger.log(LOG_ERROR, "SD", "Cannot create photo file", String(path));
            return false;
        }
        size_t written = f.write(data, len);
        f.close();
        if (written != len) {
            Logger.logf(LOG_ERROR, "SD", "Photo write incomplete: %u/%u bytes", written, len);
            return false;
        }
        Logger.logf(LOG_INFO, "SD", "Photo saved: %s  (%u bytes)", path, written);
        return true;
    }

    String resolveAdminPath(const String& uri) {
        if (uri == "/" || uri == "/admin" || uri == "/admin/") return "/www/index.html";
        if (uri.startsWith("/admin/")) return "/www" + uri.substring(6);
        return "/www" + uri;
    }

    void clearEvents() {
        if (!_mounted) return;
        SD.remove(EVENTS_LOG);
    }

    // Count visual columns in a UTF-8 string (Georgian = 1 col, 3 bytes).
    static int utf8cols(const String& s) {
        int cols = 0;
        for (int i = 0; i < (int)s.length(); ) {
            unsigned char c = (unsigned char)s[i];
            if      (c < 0x80) i += 1;
            else if (c < 0xE0) i += 2;
            else if (c < 0xF0) i += 3;
            else               i += 4;
            cols++;
        }
        return cols;
    }

    // Truncate to at most maxCols visual columns, appending ">" if cut.
    static String utf8trunc(const String& s, int maxCols) {
        if (utf8cols(s) <= maxCols) return s;
        int bytePos = 0, cols = 0;
        while (bytePos < (int)s.length() && cols < maxCols - 1) {
            unsigned char c = (unsigned char)s[bytePos];
            if      (c < 0x80) bytePos += 1;
            else if (c < 0xE0) bytePos += 2;
            else if (c < 0xF0) bytePos += 3;
            else               bytePos += 4;
            cols++;
        }
        return s.substring(0, bytePos) + ">";
    }

    // Pad string to exactly width visual columns with trailing spaces.
    static String utf8pad(const String& s, int width) {
        String r = s;
        int need = width - utf8cols(s);
        for (int i = 0; i < need; i++) r += ' ';
        return r;
    }

    // Print a formatted table of all employees to Serial.
    // Employees are numbered 1..N — the same numbering used by deleteEmployeeByIndex().
    void printEmployees() {
        if (!_mounted) { Serial.println("  SD not mounted"); return; }
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) { Serial.println("  Cannot open employees file"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); Serial.println("  Parse error"); return; }
        f.close();
        JsonArray arr = doc["employees"].as<JsonArray>();
        int n = (int)arr.size();
        if (n == 0) { Serial.println("  No employees on device."); return; }
        Serial.printf("\n  %d employee(s):\n", n);
        const char* sep = "  +----+------------------------+--------------------+--------+-------+--------+";
        Serial.println(sep);
        Serial.println("  | #  | Name                   | Local ID           | Status | Cards | Synced |");
        Serial.println(sep);
        int i = 1;
        for (JsonObject emp : arr) {
            String name = String(emp["first_name"] | "") + " " + String(emp["last_name"] | "");
            name.trim();
            name = utf8pad(utf8trunc(name, 22), 22);
            String lid  = utf8pad(utf8trunc(String(emp["local_id"] | ""), 18), 18);
            String stat = utf8pad(String(emp["status"] | "active").substring(0, 6), 6);
            int   cards = (int)emp["cards"].as<JsonArray>().size();
            bool synced = emp["synced"] | true;
            Serial.printf("  | %-2d | %s | %s | %s | %5d | %-6s |\n",
                          i++, name.c_str(), lid.c_str(),
                          stat.c_str(), cards, synced ? "yes" : "no");
        }
        Serial.println(sep);
        Serial.println("  Use:  remove <#>  (e.g. remove 1)");
        Serial.println();
    }

    // Delete the employee at 1-based position `index` (as shown by printEmployees).
    // Returns true if found and deleted.
    bool deleteEmployeeByIndex(int index) {
        if (!_mounted || index < 1) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) return false;
        JsonArray arr = doc["employees"].as<JsonArray>();
        if (index > (int)arr.size()) return false;
        String localId = arr[index - 1]["local_id"] | "";
        if (localId.isEmpty()) return false;
        String name = String(arr[index - 1]["first_name"] | "")
                    + " " + String(arr[index - 1]["last_name"] | "");
        name.trim();
        Serial.printf("[CMD] Removing #%d: %s  (id=%s)\n", index, name.c_str(), localId.c_str());
        return deleteEmployee(localId);
    }

    // Delete every file in /photos — used by factory reset
    void deleteAllPhotos() {
        if (!_mounted) return;
        File dir = SD.open("/photos");
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
        std::vector<String> paths;
        File entry;
        while ((entry = dir.openNextFile())) {
            if (!entry.isDirectory()) paths.push_back(String(entry.path()));
            entry.close();
        }
        dir.close();
        int deleted = 0;
        for (const String& p : paths) { if (SD.remove(p)) deleted++; }
        Logger.logf(LOG_INFO, "SD", "Deleted %d/%u photos (factory reset)", deleted, (unsigned)paths.size());
    }

    // Delete all photos whose filename UID prefix matches any card of the given employee
    void deletePhotosForEmployee(const String& localId) {
        if (!_mounted) return;
        // Collect UIDs for this employee
        std::vector<String> uids;
        {
            File f = SD.open(EMPLOYEES_FILE, FILE_READ);
            if (f) {
                JsonDocument doc;
                if (!deserializeJson(doc, f)) {
                    for (JsonObject emp : doc["employees"].as<JsonArray>()) {
                        if (String(emp["local_id"]|"") != localId) continue;
                        for (JsonObject card : emp["cards"].as<JsonArray>()) {
                            String uid = String(card["uid"] | "");
                            uid.toUpperCase();
                            if (uid.length()) uids.push_back(uid);
                        }
                        break;
                    }
                }
                f.close();
            }
        }
        if (uids.empty()) return;
        // Collect matching photo paths, then delete
        std::vector<String> toDelete;
        File dir = SD.open("/photos");
        if (dir && dir.isDirectory()) {
            File entry;
            while ((entry = dir.openNextFile())) {
                if (entry.isDirectory()) { entry.close(); continue; }
                String fpath = String(entry.path());
                entry.close();
                int slash = fpath.lastIndexOf('/');
                String fname = slash >= 0 ? fpath.substring(slash + 1) : fpath;
                int us = fname.indexOf('_');
                String fuid = us > 0 ? fname.substring(0, us) : fname;
                fuid.toUpperCase();
                for (const String& uid : uids) {
                    if (fuid == uid) { toDelete.push_back(fpath); break; }
                }
            }
            dir.close();
        }
        for (const String& p : toDelete) {
            if (SD.remove(p)) Serial.printf("[SD] Deleted employee photo: %s\n", p.c_str());
            else              Serial.printf("[SD] Failed to delete employee photo: %s\n", p.c_str());
        }
    }
} SdManager;
