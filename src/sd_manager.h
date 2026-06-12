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
#include "config.h"

#define EMPLOYEES_FILE "/data/employees.json"
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
            Serial.println("[SD] Mount failed");
            _mounted = false; return false;
        }
        Serial.printf("[SD] OK  %lluMB  type=%d\n",
                      SD.cardSize()/(1024*1024), SD.cardType());
        mkdirP("/data"); mkdirP("/www"); mkdirP("/photos");
        if (!SD.exists(EMPLOYEES_FILE)) {
            File f = SD.open(EMPLOYEES_FILE, FILE_WRITE);
            if (f) { f.print("{\"employees\":[],\"updated_at\":0}"); f.close(); }
        }
        _mounted = true;
        return true;
    }

    bool isMounted()   { return _mounted; }
    uint64_t totalMB() { return _mounted ? SD.cardSize()/(1024*1024) : 0; }
    uint64_t usedMB()  { return _mounted ? SD.usedBytes()/(1024*1024) : 0; }

    String lookupUid(const String& uid) {
        if (!_mounted) return "";
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return "";
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) return "";
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (String(emp["status"] | "active") != "active") continue;
            for (JsonObject card : emp["cards"].as<JsonArray>())
                if (String(card["uid"] | "") == uid &&
                    String(card["status"] | "active") == "active") {
                    String name = String(emp["first_name"]|"") + " " + String(emp["last_name"]|"");
                    name.trim(); return name;
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
            if (f) { deserializeJson(doc, f); f.close(); }
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
        if (!f) return "";
        serializeJson(doc, f); f.close();
        return localId;
    }

    bool addCard(const String& localId, const String& uid,
                 const String& label,   const String& type) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        if (deserializeJson(doc, f)) { f.close(); return false; }
        f.close();
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
                if (!out) return false;
                serializeJson(doc, out); out.close(); return true;
            }
        }
        return false;
    }

    bool updateEmployee(const String& localId, const JsonObject& updates) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        if (deserializeJson(doc, f)) { f.close(); return false; }
        f.close();
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
        if (!found) return false;
        File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!out) return false;
        serializeJson(doc, out); out.close();
        Serial.printf("[SD] Employee updated: %s\n", localId.c_str());
        return true;
    }

    bool deleteEmployee(const String& localId) {
        if (!_mounted) return false;
        JsonDocument doc;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        if (deserializeJson(doc, f)) { f.close(); return false; }
        f.close();
        JsonDocument newDoc;
        JsonArray newArr = newDoc["employees"].to<JsonArray>();
        newDoc["updated_at"] = doc["updated_at"];
        bool found = false;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (String(emp["local_id"]|"") == localId) { found = true; continue; }
            newArr.add(emp);
        }
        if (!found) return false;
        File out = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!out) return false;
        serializeJson(newDoc, out); out.close();
        Serial.printf("[SD] Employee deleted: %s\n", localId.c_str());
        return true;
    }

    bool getUnsyncedEmployees(JsonArray& out) {
        if (!_mounted || !SD.exists(EMPLOYEES_FILE)) return false;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); return false; }
        f.close();
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
        JsonDocument existing;
        if (SD.exists(EMPLOYEES_FILE)) {
            File f = SD.open(EMPLOYEES_FILE, FILE_READ);
            if (f) { deserializeJson(existing, f); f.close(); }
        }
        if (existing["employees"].is<JsonArray>() && doc["employees"].is<JsonArray>()) {
            for (JsonObject local : existing["employees"].as<JsonArray>()) {
                if (local["synced"]|true) continue;
                bool found = false;
                for (JsonObject srv : doc["employees"].as<JsonArray>())
                    if (String(srv["local_id"]|"") == String(local["local_id"]|"")) { found=true; break; }
                if (!found) doc["employees"].add(local);
            }
        }
        File f = SD.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!f) return false;
        serializeJson(doc, f); f.close(); return true;
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
                  const String& dir, const String& decision, const String& ts) {
        if (!_mounted) return;
        File f = SD.open(EVENTS_LOG, FILE_APPEND);
        if (!f) return;
        JsonDocument doc;
        doc["uid"]=uid; doc["name"]=name; doc["dir"]=dir;
        doc["decision"]=decision; doc["ts"]=ts; doc["synced"]=false;
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
            if (!deserializeJson(tmp, l)) { out.add(tmp.as<JsonObject>()); n++; }
        }
        f.close(); return n > 0;
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

    // Save a JPEG from ESP32-CAM; path: /photos/<uid>_<YYYYMMDD_HHMMSS>.jpg
    bool savePhoto(const String& uid, const uint8_t* data, size_t len) {
        if (!_mounted || len == 0) return false;
        char path[72];
        struct tm t;
        if (getLocalTime(&t, 50))
            snprintf(path, sizeof(path), "/photos/%s_%04d%02d%02d_%02d%02d%02d.jpg",
                     uid.c_str(),
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        else
            snprintf(path, sizeof(path), "/photos/%s_%lu.jpg", uid.c_str(), millis());

        File f = SD.open(path, FILE_WRITE);
        if (!f) return false;
        size_t written = f.write(data, len);
        f.close();
        Serial.printf("[SD] Photo: %s  (%u bytes)\n", path, written);
        return written == len;
    }

    String resolveAdminPath(const String& uri) {
        if (uri == "/" || uri == "/admin" || uri == "/admin/") return "/www/index.html";
        if (uri.startsWith("/admin/")) return "/www" + uri.substring(6);
        return "/www" + uri;
    }
} SdManager;
