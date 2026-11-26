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
#include <algorithm>
#include <mbedtls/sha256.h>
#include "config.h"
#include "logger.h"

#define EMPLOYEES_FILE "/data/employees.json"
#define WHITELIST_FILE "/data/whitelist.json"
#define EVENTS_LOG     "/data/events.log"
#define ADMINS_FILE    "/data/admins.json"
#define CONFIG_BACKUP  "/data/config.json"
#define MAX_LOG_LINES  2000

extern SPIClass spiHSPI;

// RAII guard for SdManagerClass's recursive mutex — used internally by every
// SD-touching method so early returns can't accidentally skip the unlock.
// Recursive: a guarded method calling another guarded method on the same
// object, from the same task, just re-enters (no deadlock).
struct SdIoLock {
    SemaphoreHandle_t h; bool held = false;
    SdIoLock(SemaphoreHandle_t handle, uint32_t timeoutMs = 3000) : h(handle) {
        held = h && xSemaphoreTakeRecursive(h, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }
    ~SdIoLock() { if (held) xSemaphoreGiveRecursive(h); }
};

struct SdManagerClass {
private:
    bool _mounted = false;
    SemaphoreHandle_t _ioMutex = nullptr;

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

    // Read one '\n'-terminated line into a fixed buffer, trimming trailing
    // whitespace. Replaces readStringUntil('\n'), which allocated a new String
    // per line — thousands of alloc/free cycles per sync pass over events.log
    // were the main source of heap fragmentation. Overlong lines are
    // truncated (the remainder is consumed and dropped). Returns length, or
    // -1 at EOF with nothing read.
    static int readLine(File& f, char* buf, size_t cap) {
        size_t n = 0; bool any = false;
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            any = true;
            if (c == '\n') break;
            if (n + 1 < cap) buf[n++] = (char)c;
        }
        while (n > 0 && (buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t')) n--;
        buf[n] = '\0';
        return any ? (int)n : -1;
    }

    static constexpr size_t LINE_CAP = 512;
    static bool isUnsyncedLine(const char* l) { return strstr(l, "\"synced\":false") != nullptr; }

    // event_id of a log line. v0.6.0+ lines carry "eid"; older lines get a
    // stable id derived from uid + timestamp, so they can be sent, matched
    // in the server's answer, and marked like any other line.
    static String eventIdOf(JsonObjectConst src) {
        const char* eid = src["eid"] | "";
        if (*eid) return String(eid);
        String ts = src["ts"].isNull() ? String(src["happened_at"] | "") : String(src["ts"] | "");
        String id = "x-"; id += (const char*)(src["uid"] | ""); id += "-";
        for (size_t i = 0; i < ts.length(); i++) if (isDigit(ts[i])) id += ts[i];
        return id.substring(0, 64);
    }

    static String sha256Hex(const String& in) {
        uint8_t h[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t*)in.c_str(), in.length());
        mbedtls_sha256_finish(&ctx, h);
        mbedtls_sha256_free(&ctx);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", h[i]);
        return String(hex);
    }

    bool loadEmployees(JsonDocument& doc) {
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return false;
        DeserializationError err = deserializeJson(doc, f); f.close();
        if (err) {
            Logger.logf(LOG_ERROR, "SD", "employees.json parse error: %s", err.c_str());
            return false;
        }
        if (!doc["employees"].is<JsonArray>()) doc["employees"].to<JsonArray>();
        return true;
    }

    // Write to a temp file first, then swap — a power cut mid-write leaves
    // the old employees.json intact instead of a truncated one.
    bool saveEmployees(JsonDocument& doc) {
        const char* tmp = "/data/emp.tmp";
        File out = SD.open(tmp, FILE_WRITE);
        if (!out) return false;
        size_t n = serializeJson(doc, out); out.close();
        if (n == 0) { SD.remove(tmp); return false; }
        SD.remove(EMPLOYEES_FILE);
        return SD.rename(tmp, EMPLOYEES_FILE);
    }

    // Active UIDs (active employee + active card), uppercased and sorted —
    // the exact set the server's sync checksum is computed over (§10.2).
    void activeUids(JsonDocument& doc, std::vector<String>& uids, bool& anyUnsynced) {
        anyUnsynced = false;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (!(emp["synced"] | true)) anyUnsynced = true;
            if (String(emp["status"] | "active") != "active") continue;
            for (JsonObject c : emp["cards"].as<JsonArray>()) {
                if (String(c["status"] | "active") != "active") continue;
                String u = c["uid"] | "";
                u.toUpperCase();
                if (u.length()) uids.push_back(u);
            }
        }
        std::sort(uids.begin(), uids.end(),
                  [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
        uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
    }

    String makeLocalId() {
        char buf[24];
        snprintf(buf, sizeof(buf), "loc_%lu_%04x", millis(), (unsigned)random(0xFFFF));
        return String(buf);
    }

public:
    // Held across a whole span of calls (e.g. CamUart's multi-chunk photo
    // write) so no other task's SD access can interleave with it. Internal
    // methods take/give the same recursive mutex too, so calling a guarded
    // method while already holding this lock is safe (re-entrant).
    bool lock(uint32_t timeoutMs = 3000) {
        return _ioMutex && xSemaphoreTakeRecursive(_ioMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }
    void unlock() { if (_ioMutex) xSemaphoreGiveRecursive(_ioMutex); }

    bool begin() {
        if (!_ioMutex) _ioMutex = xSemaphoreCreateRecursiveMutex();

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
        SdIoLock _lk(_ioMutex);
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
        SdIoLock _lk(_ioMutex);
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
        SdIoLock _lk(_ioMutex);
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
                emp.remove("sync_error");  // an edit is the retry
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
        SdIoLock _lk(_ioMutex);
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
                emp.remove("sync_error");  // an edit is the retry
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
        SdIoLock _lk(_ioMutex);
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

    // Employees waiting to be pushed. Records the server refused
    // (sync_error) are held back until someone edits them — resending an
    // unchanged record only gets the same refusal. Capped per batch; the
    // server takes at most 100.
    bool getUnsyncedEmployees(JsonArray& out, int limit = 50) {
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EMPLOYEES_FILE)) return false;
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
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            if (n >= limit) break;
            if (emp["synced"] | false) continue;
            if (!emp["sync_error"].isNull()) continue;
            JsonObject o = out.add<JsonObject>();
            o.set(emp);
            o.remove("synced");
            n++;
        }
        return n > 0;
    }

    // Apply the server's per-record answer (§10.3): records in `ids` become
    // synced and learn their server id; records in `errors` keep synced=false
    // and get a sync_error message so they stop being resent until edited.
    // Anything not mentioned in either list is left as it was. Returns the
    // number of employees now stuck in sync_error.
    int applyEmployeeSyncResult(JsonArrayConst ids, JsonArrayConst errors) {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        JsonDocument doc;
        if (!loadEmployees(doc)) return 0;
        int stuck = 0;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            String lid = emp["local_id"] | "";
            for (JsonObjectConst r : ids) {
                if (lid != String(r["local_id"] | "")) continue;
                int sid = r["server_id"] | 0;
                if (sid > 0) emp["id"] = sid;
                emp["synced"] = true;
                emp.remove("sync_error");
                break;
            }
            for (JsonObjectConst e : errors) {
                if (lid != String(e["local_id"] | "")) continue;
                // First message of the first failing field, e.g.
                // "email: The email must be a valid email address."
                String msg = e["error"] | "";
                for (JsonPairConst kv : e["fields"].as<JsonObjectConst>()) {
                    msg = String(kv.key().c_str()) + ": " + String(kv.value()[0] | "");
                    break;
                }
                if (msg.isEmpty()) msg = "rejected by server";
                emp["sync_error"] = msg.substring(0, 160);
                break;
            }
            if (!emp["sync_error"].isNull()) stuck++;
        }
        if (!saveEmployees(doc))
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json after employee sync");
        return stuck;
    }

    int syncErrorCount() {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        JsonDocument doc;
        if (!loadEmployees(doc)) return 0;
        int n = 0;
        for (JsonObject emp : doc["employees"].as<JsonArray>())
            if (!emp["sync_error"].isNull()) n++;
        return n;
    }

    // ── Delta whitelist sync (§10.2) ─────────────────────────────────────────
    // Apply one page of GET /device/sync?since=. Upserts replace the whole
    // server record (fields + active cards), except where a local edit is
    // still waiting to be pushed — the local version wins until it syncs, as
    // before. On a full page every server id is collected in `seen` so
    // finishFullSync() can drop the ones the server no longer has.
    bool applySyncPage(JsonObjectConst page, bool full, std::vector<int>& seen,
                       int& nUpserts, int& nDeletes) {
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
        JsonDocument doc;
        if (SD.exists(EMPLOYEES_FILE) && !loadEmployees(doc)) return false;
        JsonArray arr = doc["employees"].is<JsonArray>()
                      ? doc["employees"].as<JsonArray>()
                      : doc["employees"].to<JsonArray>();

        for (JsonObjectConst up : page["upserts"].as<JsonArrayConst>()) {
            int sid = up["id"] | 0;
            if (sid <= 0) continue;
            if (full) seen.push_back(sid);
            JsonObject target;
            for (JsonObject emp : arr)
                if ((emp["id"] | 0) == sid) { target = emp; break; }
            if (!target.isNull() && !(target["synced"] | true)) continue;  // local edit pending
            if (target.isNull()) {
                target = arr.add<JsonObject>();
                target["local_id"] = String("srv_") + String(sid);
                target["phone"] = ""; target["email"] = "";
            }
            target["id"]         = sid;
            target["first_name"] = up["first_name"] | "";
            target["last_name"]  = up["last_name"]  | "";
            target["department"] = up["department"] | "";
            target["position"]   = up["position"]   | "";
            target["status"]     = up["status"]     | "active";
            target["synced"]     = true;
            target.remove("sync_error");
            JsonArray cards = target["cards"].to<JsonArray>();
            for (JsonObjectConst c : up["cards"].as<JsonArrayConst>()) {
                JsonObject card = cards.add<JsonObject>();
                card["uid"]    = c["uid"]    | "";
                card["status"] = c["status"] | "active";
            }
            nUpserts++;
        }

        // Deleted / disabled employees — the server is the authority on
        // removal, so this applies even over a pending local edit.
        for (JsonVariantConst v : page["deletes"]["employees"].as<JsonArrayConst>()) {
            int sid = v | 0;
            if (sid <= 0) continue;
            for (int i = (int)arr.size() - 1; i >= 0; i--)
                if ((arr[i]["id"] | 0) == sid) { arr.remove(i); nDeletes++; }
        }
        // Revoked cards leave every employee, local ones included: a card the
        // server took away must stop opening the door immediately.
        for (JsonVariantConst v : page["deletes"]["cards"].as<JsonArrayConst>()) {
            String uid = v | "";
            if (uid.isEmpty()) continue;
            for (JsonObject emp : arr) {
                JsonArray cards = emp["cards"].as<JsonArray>();
                for (int i = (int)cards.size() - 1; i >= 0; i--)
                    if (uid.equalsIgnoreCase(String(cards[i]["uid"] | ""))) { cards.remove(i); nDeletes++; }
            }
        }

        doc["updated_at"] = (long)(millis() / 1000);
        if (!saveEmployees(doc)) {
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json during whitelist sync");
            return false;
        }
        return true;
    }

    // End of a full sync: drop server-origin employees the server didn't send.
    void finishFullSync(const std::vector<int>& seen) {
        if (!_mounted) return;
        SdIoLock _lk(_ioMutex);
        JsonDocument doc;
        if (!loadEmployees(doc)) return;
        JsonArray arr = doc["employees"].as<JsonArray>();
        int removed = 0;
        for (int i = (int)arr.size() - 1; i >= 0; i--) {
            int sid = arr[i]["id"] | 0;
            bool synced = arr[i]["synced"] | true;
            if (sid > 0 && synced && std::find(seen.begin(), seen.end(), sid) == seen.end()) {
                arr.remove(i); removed++;
            }
        }
        if (removed && !saveEmployees(doc))
            Logger.log(LOG_ERROR, "SD", "Cannot write employees.json after full sync");
        if (removed) Logger.logf(LOG_INFO, "SYNC", "Full sync removed %d stale employee(s)", removed);
    }

    // Regenerate the flat whitelist.json fallback from employees.json, and
    // compute the checksum the server sends on the last sync page. Written
    // with print() rather than a JsonDocument — one UID at a time.
    // `comparable` is false while local edits are unsynced: their cards
    // aren't on the server yet, so a mismatch would mean nothing.
    bool rebuildWhitelist(long version, int& count, String& hash, bool& comparable) {
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
        std::vector<String> uids;
        bool anyUnsynced = false;
        {
            JsonDocument doc;
            if (!loadEmployees(doc)) return false;
            activeUids(doc, uids, anyUnsynced);
        }
        comparable = !anyUnsynced;
        count = (int)uids.size();
        String joined;
        joined.reserve(uids.size() * 15);
        for (size_t i = 0; i < uids.size(); i++) { if (i) joined += ','; joined += uids[i]; }
        hash = sha256Hex(joined);

        File wf = SD.open(WHITELIST_FILE, FILE_WRITE);
        if (!wf) return false;
        wf.print("{\"uids\":[");
        for (size_t i = 0; i < uids.size(); i++) {
            if (i) wf.print(',');
            wf.print('"'); wf.print(uids[i]); wf.print('"');
        }
        wf.print("],\"version\":"); wf.print(version); wf.print('}');
        wf.close();
        return true;
    }

    // Version stored by rebuildWhitelist(); 0 when the file is missing, is
    // from older firmware, or doesn't parse — all of which mean "full sync".
    long readWhitelistVersion() {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        File f = SD.open(WHITELIST_FILE, FILE_READ);
        if (!f) return 0;
        JsonDocument filter; filter["version"] = true;
        JsonDocument d;
        DeserializationError err = deserializeJson(d, f, DeserializationOption::Filter(filter));
        f.close();
        return err ? 0 : (long)(d["version"] | 0L);
    }

    bool updateWhitelist(const String& json) {
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
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
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EMPLOYEES_FILE)) return 0;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return 0;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        return err ? 0 : (long)(doc["updated_at"]|0);
    }

    int employeeCount() {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EMPLOYEES_FILE)) return 0;
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) return 0;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f); f.close();
        return err ? 0 : (int)doc["employees"].as<JsonArray>().size();
    }

    void logEvent(const String& uid, const String& name,
                  const String& direction, const String& decision,
                  const String& happened_at, const String& reason,
                  const String& eventId) {
        if (!_mounted) return;
        SdIoLock _lk(_ioMutex);
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
        doc["reason"]=reason; doc["eid"]=eventId; doc["synced"]=false;
        serializeJson(doc, f); f.println(); f.close();
    }

    int unsyncedCount() {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EVENTS_LOG)) return 0;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return 0;
        int n = 0;
        char l[LINE_CAP];
        while (readLine(f, l, sizeof(l)) >= 0)
            if (strlen(l) > 5 && isUnsyncedLine(l)) n++;
        f.close(); return n;
    }

    bool getUnsyncedEvents(JsonArray& out, int limit = 50) {
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EVENTS_LOG)) return false;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return false;
        int n = 0;
        char l[LINE_CAP];
        while (n < limit && readLine(f, l, sizeof(l)) >= 0) {
            if (strlen(l) < 5 || !isUnsyncedLine(l)) continue;
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
            // dropUnsendableEvents() clears them once no valid event is left.
            if (ts_val.length() == 0 || ts_val[0] == '~') continue;
            JsonObject dst = out.add<JsonObject>();
            dst["event_id"]    = eventIdOf(src);
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
        SdIoLock _lk(_ioMutex);
        String dateStr = ts.substring(0,4) + ts.substring(5,7) + ts.substring(8,10);
        String timeStr = ts.substring(11,13) + ts.substring(14,16) + ts.substring(17,19);
        String path = "/photos/" + uid + "_" + dateStr + "_" + timeStr + ".jpg";
        return SD.exists(path) ? path : "";
    }

    // Mark exactly the events the server answered for (accepted, duplicate
    // or rejected) as synced, matched by event_id. Replaces the old
    // "mark the first N unsynced lines", which went wrong whenever a skipped
    // pre-NTP line sat among the ones that were sent.
    int markEventsSynced(const std::vector<String>& ids) {
        if (!_mounted || ids.empty()) return 0;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EVENTS_LOG)) return 0;
        const char* tmp = "/data/ev.tmp";
        File src = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!src || !dst) { if(src)src.close(); if(dst)dst.close(); return 0; }
        int marked = 0;
        char l[LINE_CAP];
        while (readLine(src, l, sizeof(l)) >= 0) {
            if (strlen(l) < 5) continue;
            if (isUnsyncedLine(l)) {
                JsonDocument d;
                if (!deserializeJson(d, l)) {
                    String id = eventIdOf(d.as<JsonObjectConst>());
                    if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
                        String s(l);
                        s.replace("\"synced\":false", "\"synced\":true");
                        dst.println(s);
                        marked++;
                        continue;
                    }
                }
            }
            dst.println(l);
        }
        src.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
        return marked;
    }

    // Events recorded before the clock was set ("~NNNms" or empty timestamp)
    // can never be sent — the server needs a real time. Mark only those, and
    // only once nothing sendable is left in front of them.
    int dropUnsendableEvents() {
        if (!_mounted) return 0;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EVENTS_LOG)) return 0;
        const char* tmp = "/data/ev.tmp";
        File src = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!src || !dst) { if(src)src.close(); if(dst)dst.close(); return 0; }
        int dropped = 0;
        char l[LINE_CAP];
        while (readLine(src, l, sizeof(l)) >= 0) {
            if (strlen(l) < 5) continue;
            if (isUnsyncedLine(l)) {
                JsonDocument d;
                bool bad = true;
                if (!deserializeJson(d, l)) {
                    const char* ts = d["ts"].isNull() ? (d["happened_at"] | "") : (d["ts"] | "");
                    bad = (*ts == '\0' || *ts == '~');
                }
                if (bad) {
                    String s(l);
                    s.replace("\"synced\":false", "\"synced\":true");
                    dst.println(s);
                    dropped++;
                    continue;
                }
            }
            dst.println(l);
        }
        src.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
        return dropped;
    }

    void trimLog() {
        if (!_mounted) return;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(EVENTS_LOG)) return;
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) return;
        int total = 0;
        char l[LINE_CAP];
        while (readLine(f, l, sizeof(l)) >= 0) total++;
        f.close();
        if (total <= MAX_LOG_LINES) return;
        int skip = total - MAX_LOG_LINES;
        String tmp = "/data/ev.tmp";
        f = SD.open(EVENTS_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!f || !dst) { if(f)f.close(); if(dst)dst.close(); return; }
        int i = 0;
        while (readLine(f, l, sizeof(l)) >= 0) { if (i++ >= skip) dst.println(l); }
        f.close(); dst.close();
        SD.remove(EVENTS_LOG); SD.rename(tmp, EVENTS_LOG);
    }

    void backupConfig() {
        if (!_mounted) return;
        SdIoLock _lk(_ioMutex);
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
        if (!_mounted) return false;
        SdIoLock _lk(_ioMutex);
        if (!SD.exists(path)) return false;
        File f = SD.open(path, FILE_READ);
        if (!f || f.isDirectory()) { if(f)f.close(); return false; }
        server.sendHeader("Cache-Control", "max-age=60");
        server.sendHeader("Connection", "close");
        server.streamFile(f, mime(path)); f.close(); return true;
    }

    // Builds /photos/<uid>_<YYYYMMDD>_<HHMMSS>.jpg from happenedAt (ISO
    // timestamp from the event log) or the live clock as a fallback.
    String photoPath(const String& uid, const String& happenedAt) {
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
        return String(path);
    }

    // Open a new photo file for streaming writes — caller (CamUart) writes
    // chunks to the returned File as they arrive over UART, instead of
    // buffering the whole JPEG in RAM first. outPath is filled in for
    // logging and for abortPhoto() if the transfer fails partway.
    File openPhotoFile(const String& uid, const String& happenedAt, String& outPath) {
        if (!_mounted) return File();
        SdIoLock _lk(_ioMutex);
        outPath = photoPath(uid, happenedAt);
        File f = SD.open(outPath, FILE_WRITE);
        if (!f) Logger.log(LOG_ERROR, "SD", "Cannot create photo file", outPath);
        return f;
    }

    // Discard a photo file left partially written by a failed capture.
    void abortPhoto(File& f, const String& path) {
        SdIoLock _lk(_ioMutex);
        if (f) f.close();
        if (path.length()) SD.remove(path);
    }

    String resolveAdminPath(const String& uri) {
        if (uri == "/" || uri == "/admin" || uri == "/admin/") return "/www/index.html";
        if (uri.startsWith("/admin/")) return "/www" + uri.substring(6);
        return "/www" + uri;
    }

    void clearEvents() {
        if (!_mounted) return;
        SdIoLock _lk(_ioMutex);
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
        SdIoLock _lk(_ioMutex);
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

    // ── Serial console: pretty-print all registered cards ───────────────────────
    void printCards() {
        if (!_mounted) { Serial.println("  SD not mounted"); return; }
        SdIoLock _lk(_ioMutex);
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) { Serial.println("  Cannot open employees file"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); Serial.println("  Parse error"); return; }
        f.close();

        struct Row { String uid, type, owner, status; };
        std::vector<Row> rows;
        for (JsonObject emp : doc["employees"].as<JsonArray>()) {
            String name = String(emp["first_name"] | "") + " " + String(emp["last_name"] | "");
            name.trim();
            for (JsonObject c : emp["cards"].as<JsonArray>())
                rows.push_back({ String(c["uid"] | ""), String(c["type"] | ""),
                                  name, String(c["status"] | "active") });
        }
        if (rows.empty()) { Serial.println("  No cards registered."); return; }

        Serial.printf("\n  %d card(s):\n", (int)rows.size());
        const char* sep = "  +----+------------------+--------+------------------------+--------+";
        Serial.println(sep);
        Serial.println("  | #  | UID              | Type   | Owner                  | Status |");
        Serial.println(sep);
        int i = 1;
        for (auto& r : rows) {
            String uid  = utf8pad(utf8trunc(r.uid,    16), 16);
            String type = utf8pad(utf8trunc(r.type,    6),  6);
            String own  = utf8pad(utf8trunc(r.owner,  22), 22);
            String st   = utf8pad(utf8trunc(r.status,  6),  6);
            Serial.printf("  | %-2d | %s | %s | %s | %s |\n",
                          i++, uid.c_str(), type.c_str(), own.c_str(), st.c_str());
        }
        Serial.println(sep);
        Serial.println();
    }

    // Delete the employee at 1-based position `index` (as shown by printEmployees).
    // Returns true if found and deleted.
    bool deleteEmployeeByIndex(int index) {
        if (!_mounted || index < 1) return false;
        SdIoLock _lk(_ioMutex);
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
        SdIoLock _lk(_ioMutex);
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
        SdIoLock _lk(_ioMutex);
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

// RAII guard for callers outside SdManagerClass that need the lock held
// across several calls plus raw File I/O of their own (e.g. CamUart
// streaming photo chunks directly to a File — the individual write() calls
// aren't SdManager methods, so they need an explicit outer hold).
struct SdManagerLock {
    bool held;
    SdManagerLock(uint32_t timeoutMs = 3000) : held(SdManager.lock(timeoutMs)) {}
    ~SdManagerLock() { if (held) SdManager.unlock(); }
};
