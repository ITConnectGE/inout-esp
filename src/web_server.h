#pragma once
/**
 * web_server.h — HTTP :80
 *
 * All routes (except /api/auth/login) require valid session token.
 * Token passed as header: X-Session-Token: <token>
 * Or query param: ?token=<token>
 *
 * Auth routes:
 *   POST /api/auth/login           { username, password } → { token, role, must_change_password }
 *   POST /api/auth/logout          clears session
 *   GET  /api/auth/me              returns current user info
 *   POST /api/auth/change-password { old_password, new_password }
 *
 * Super admin only:
 *   GET  /api/admins               list all admins
 *   POST /api/admins               create admin { username, password, role }
 *   DELETE /api/admins/:username   delete admin
 *   POST /api/admins/:username/reset-password { new_password }
 *
 * All roles:
 *   GET  /api/status
 *   GET  /api/local/employees
 *   GET  /api/local/events
 *   POST /api/employees
 *   POST /api/employees/card
 *   GET  /api/employees/pending
 *   ANY  /api/proxy/*
 *
 * Super admin only:
 *   POST /api/config
 *   POST /api/open
 *   POST /api/reset
 *
 * Pages:
 *   GET  /          → SD index.html (combined login+admin page)
 *   GET  /admin     → same
 */

#include <Arduino.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <WiFiManager.h>
#include <vector>
#include "config.h"
#include "relay.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "api_client.h"
#include "auth_manager.h"
#include "page_html.h"

WebServer _server(80);
extern bool _lcdFound;

// ── Card tap capture ──────────────────────────────────────────────────────────
struct PendingTap {
    String  localId, label, type;
    uint32_t expiresAt;
    bool    captured;
    String  capturedUid;
};
static PendingTap _pendingTap = {"","","",0,false,""};

void onRegistrationTap(const String& uid) {
    if (_pendingTap.localId.length() == 0) return;
    if (millis() > _pendingTap.expiresAt) { _pendingTap.localId = ""; return; }
    _pendingTap.captured    = true;
    _pendingTap.capturedUid = uid;
    // "__scan__" = scan-only, just capture UID — don't write to SD yet
    if (_pendingTap.localId != "__scan__") {
        SdManager.addCard(_pendingTap.localId, uid, _pendingTap.label, _pendingTap.type);
    }
    Serial.printf("[Reg] Card %s captured (mode=%s)\n", uid.c_str(), _pendingTap.localId.c_str());
    _pendingTap.localId = "";
}

bool hasPendingTap() {
    if (_pendingTap.localId.length() == 0) return false;
    if (millis() > _pendingTap.expiresAt) { _pendingTap.localId = ""; return false; }
    return true;
}

struct WebServerClass {
private:
    // ── Auth helpers ──────────────────────────────────────────────────────────
    String getToken() {
        if (_server.hasHeader("X-Session-Token"))
            return _server.header("X-Session-Token");
        if (_server.hasArg("token"))
            return _server.arg("token");
        return "";
    }

    bool requireAuth(Session& session) {
        String token = getToken();
        if (token.length() == 0 || !AuthManager.validate(token, session)) {
            cors(); _server.send(401, "application/json",
                "{\"error\":\"unauthorized\",\"code\":401}");
            return false;
        }
        return true;
    }

    bool requireSuperAdmin() {
        Session s;
        if (!requireAuth(s)) return false;
        if (s.role != "super_admin") {
            cors(); _server.send(403, "application/json",
                "{\"error\":\"forbidden\",\"code\":403}");
            return false;
        }
        return true;
    }

    void cors() {
        _server.sendHeader("Access-Control-Allow-Origin",  "*");
        _server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
        _server.sendHeader("Access-Control-Allow-Headers",
                           "Content-Type,Authorization,X-Session-Token");
    }

    void jsend(int code, const String& body) {
        cors(); _server.send(code, "application/json", body);
    }

    // ── POST /api/auth/login ──────────────────────────────────────────────────
    void handleLogin() {
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }

        String username = doc["username"] | "";
        String password = doc["password"] | "";
        String role; bool mustChange = false;

        String token = AuthManager.login(username, password, role, mustChange);
        if (token.length() == 0) {
            jsend(401, "{\"error\":\"invalid credentials\"}"); return;
        }

        JsonDocument resp;
        resp["token"]                 = token;
        resp["username"]              = username;
        resp["role"]                  = role;
        resp["must_change_password"]  = mustChange;
        String body; serializeJson(resp, body); jsend(200, body);
    }

    // ── POST /api/auth/logout ─────────────────────────────────────────────────
    void handleLogout() {
        AuthManager.logout(getToken());
        jsend(200, "{\"ok\":true}");
    }

    // ── GET /api/auth/me ──────────────────────────────────────────────────────
    void handleMe() {
        Session s;
        if (!requireAuth(s)) return;
        JsonDocument resp;
        resp["username"] = s.username;
        resp["role"]     = s.role;
        String body; serializeJson(resp, body); jsend(200, body);
    }

    // ── POST /api/auth/change-password ────────────────────────────────────────
    void handleChangePassword() {
        Session s;
        if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }

        String oldPass = doc["old_password"] | "";
        String newPass = doc["new_password"] | "";
        if (newPass.length() < 8) { jsend(400,"{\"error\":\"Password min 8 chars\"}"); return; }

        if (AuthManager.changePassword(s.username, oldPass, newPass))
            jsend(200, "{\"ok\":true}");
        else
            jsend(400, "{\"error\":\"Current password incorrect\"}");
    }

    // ── GET /api/admins ───────────────────────────────────────────────────────
    void handleListAdmins() {
        if (!requireSuperAdmin()) return;
        jsend(200, AuthManager.getAdminsList());
    }

    // ── POST /api/admins ──────────────────────────────────────────────────────
    void handleCreateAdmin() {
        if (!requireSuperAdmin()) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }

        String username = doc["username"] | "";
        String password = doc["password"] | "";
        String role     = doc["role"]     | "admin";

        if (username.length() < 3) { jsend(400,"{\"error\":\"Username min 3 chars\"}"); return; }
        if (password.length() < 8) { jsend(400,"{\"error\":\"Password min 8 chars\"}"); return; }
        if (role != "admin" && role != "super_admin") role = "admin";

        if (AuthManager.addAdmin(username, password, role))
            jsend(201, "{\"ok\":true,\"message\":\"Admin created. User must change password on first login.\"}");
        else
            jsend(409, "{\"error\":\"Username already exists\"}");
    }

    // ── DELETE /api/admins ────────────────────────────────────────────────────
    void handleDeleteAdmin() {
        if (!requireSuperAdmin()) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String username = doc["username"] | "";
        if (AuthManager.deleteAdmin(username))
            jsend(200, "{\"ok\":true}");
        else
            jsend(400, "{\"error\":\"Cannot delete — last super admin or not found\"}");
    }

    // ── POST /api/admins/reset-password ───────────────────────────────────────
    void handleResetAdminPassword() {
        if (!requireSuperAdmin()) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String username = doc["username"] | "";
        String newPass  = doc["new_password"] | "";
        if (newPass.length() < 8) { jsend(400,"{\"error\":\"Password min 8 chars\"}"); return; }
        if (AuthManager.setPassword(username, newPass))
            jsend(200, "{\"ok\":true}");
        else
            jsend(404, "{\"error\":\"Admin not found\"}");
    }

    // ── GET /api/status ───────────────────────────────────────────────────────
    void handleStatus() {
        // Public fields available without auth (for login page health check)
        // Full status requires auth
        Session s;
        bool authed = AuthManager.validate(getToken(), s);
        JsonDocument doc;
        doc["firmware"]          = "0.3.2";
        doc["identifier"]        = Config.identifier;
        doc["ip"]                = WiFi.localIP().toString();
        doc["rssi"]              = WiFi.RSSI();
        doc["server_url"]        = Config.serverUrl;
        doc["token_set"]         = Config.deviceToken.length() > 0;
        doc["config_version"]    = Config.configVersion;
        doc["relay_ms"]          = Config.relayMs;
        doc["relay_pin"]         = Config.relayPin;
        doc["cs_in"]             = Config.csPin_IN;
        doc["cs_out"]            = Config.csPin_OUT;
        doc["cs_sd"]             = Config.csPin_SD;
        doc["uptime_s"]          = millis()/1000;
        doc["ntp_synced"]        = ApiClient.isNtpSynced();
        doc["current_time"]      = ApiClient.nowIso();
        doc["sd_mounted"]        = SdManager.isMounted();
        doc["sd_total_mb"]       = (int)SdManager.totalMB();
        doc["sd_used_mb"]        = (int)SdManager.usedMB();
        doc["unsynced_events"]   = SdManager.unsyncedCount();
        doc["employee_count"]    = SdManager.employeeCount();
        doc["whitelist_age_s"]   = (long)(millis()/1000) - SdManager.whitelistUpdatedAt();
        doc["lcd_found"]         = _lcdFound;
        doc["timezone"]          = Config.timezone;
        doc["pending_tap"]       = hasPendingTap();
        if (authed) doc["role"]  = s.role;
        String body; serializeJson(doc, body); jsend(200, body);
    }

    // ── POST /api/config (super admin only) ───────────────────────────────────
    void handleConfig() {
        if (!requireSuperAdmin()) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        Config.applyFromJson(doc.as<JsonObject>());
        jsend(200, "{\"ok\":true}");
    }

    // ── POST /api/open (super admin only) ─────────────────────────────────────
    void handleOpen() {
        if (!requireSuperAdmin()) return;
        Relay.open(Config.relayMs); jsend(200, "{\"ok\":true}");
    }

    // ── POST /api/reset (super admin only) ────────────────────────────────────
    void handleReset() {
        if (!requireSuperAdmin()) return;
        String type = _server.hasArg("type") ? _server.arg("type") : "wifi";
        jsend(200, "{\"ok\":true}");
        delay(300);
        if (type == "all") {
            // 1. Clear NVS config
            Preferences p; p.begin("cp", false); p.clear(); p.end();
            // 2. Wipe all SD data including photos
            if (SdManager.isMounted()) {
                SD.remove(EMPLOYEES_FILE);
                SD.remove(EVENTS_LOG);
                SD.remove(ADMINS_FILE);
                SD.remove(CONFIG_BACKUP);
                SdManager.deleteAllPhotos();
                Serial.println("[Reset] SD data wiped");
            }
            // 3. Recreate default admin (admin / 12345678)
            AuthManager.begin();
            Serial.println("[Reset] Default admin restored");
        }
        WiFiManager wm; wm.resetSettings();
        ESP.restart();
    }

    // ── POST /api/employees ───────────────────────────────────────────────────
    void handleCreateEmployee() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }

        String firstName = doc["first_name"] | "";
        String lastName  = doc["last_name"]  | "";
        if (firstName.length()==0 && lastName.length()==0) {
            jsend(400,"{\"error\":\"first_name or last_name required\"}"); return;
        }
        String localId = SdManager.addEmployee(
            firstName, lastName,
            doc["position"]    | "",
            doc["department"]  | "",
            doc["phone"]       | "",
            doc["email"]       | "",
            doc["external_id"] | ""
        );
        if (localId.length() == 0) { jsend(500,"{\"error\":\"save failed\"}"); return; }

        // If card_uid provided, assign card immediately without needing tap
        String cardUid = doc["card_uid"] | "";
        cardUid.toUpperCase();
        if (cardUid.length() > 0) {
            SdManager.addCard(localId, cardUid,
                              doc["card_label"] | "",
                              doc["card_type"]  | "mifare_uid");
        }

        JsonDocument resp;
        resp["ok"]       = true;
        resp["local_id"] = localId;
        String body; serializeJson(resp, body); jsend(201, body);
    }

    // ── POST /api/employees/update ──────────────────────────────────────────────
    void handleUpdateEmployee() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String localId = doc["local_id"] | "";
        if (localId.length() == 0) { jsend(400,"{\"error\":\"local_id required\"}"); return; }
        if (SdManager.updateEmployee(localId, doc.as<JsonObject>()))
            jsend(200, "{\"ok\":true}");
        else
            jsend(500, "{\"error\":\"save failed\"}");
    }

    // ── POST /api/employees/delete ────────────────────────────────────────────
    void handleDeleteEmployee() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String localId = doc["local_id"] | "";
        if (localId.length() == 0) { jsend(400,"{\"error\":\"local_id required\"}"); return; }
        SdManager.deletePhotosForEmployee(localId);
        if (SdManager.deleteEmployee(localId))
            jsend(200, "{\"ok\":true}");
        else
            jsend(404, "{\"error\":\"employee not found\"}");
    }

    // ── POST /api/photos/delete ───────────────────────────────────────────────
    void handleDeletePhoto() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String filename = doc["file"] | "";
        // Prevent path traversal
        if (filename.length() == 0 || filename.indexOf('/') >= 0 || filename.indexOf("..") >= 0) {
            jsend(400, "{\"error\":\"invalid filename\"}"); return;
        }
        String path = "/photos/" + filename;
        if (!SdManager.isMounted() || !SD.exists(path)) {
            jsend(404, "{\"error\":\"not found\"}"); return;
        }
        SD.remove(path);
        jsend(200, "{\"ok\":true}");
    }

    // ── POST /api/employees/card ──────────────────────────────────────────────
    void handleAssignCard() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String localId = doc["local_id"] | "";
        if (localId.length() == 0) { jsend(400,"{\"error\":\"local_id required\"}"); return; }
        // "__scan__" = scan-only mode: capture UID but don't assign to employee yet
        bool scanOnly = (localId == "__scan__");
        _pendingTap = { localId, doc["label"]|"", doc["type"]|"mifare_uid",
                        millis() + (scanOnly ? 30000 : 15000), false, "" };
        jsend(200, "{\"ok\":true,\"timeout_ms\":30000}");
    }

    // ── POST /api/employees/card/assign — assign known UID directly ────────────
    // Used when UID is already known (scanned during step 1 of add-employee flow)
    void handleDirectAssignCard() {
        Session s; if (!requireAuth(s)) return;
        if (!_server.hasArg("plain")) { jsend(400,"{\"error\":\"no body\"}"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain"))) { jsend(400,"{\"error\":\"bad json\"}"); return; }
        String localId = doc["local_id"] | "";
        String uid     = doc["uid"]      | "";
        uid.toUpperCase();
        if (localId.length() == 0 || uid.length() == 0) {
            jsend(400, "{\"error\":\"local_id and uid required\"}"); return;
        }
        if (SdManager.addCard(localId, uid, doc["label"]|"", doc["type"]|"mifare_uid"))
            jsend(200, "{\"ok\":true}");
        else
            jsend(500, "{\"error\":\"save failed\"}");
    }

        // ── GET /api/employees/pending ────────────────────────────────────────────
    void handlePendingTap() {
        Session s; if (!requireAuth(s)) return;
        JsonDocument resp;
        if (_pendingTap.captured) {
            resp["pending"]  = false;
            resp["captured"] = true;
            resp["uid"]      = _pendingTap.capturedUid;
        } else if (hasPendingTap()) {
            resp["pending"]     = true;
            resp["captured"]    = false;
            resp["expires_in"]  = (int)((_pendingTap.expiresAt - millis()) / 1000);
        } else {
            resp["pending"]  = false;
            resp["captured"] = false;
            resp["expired"]  = true;
        }
        String body; serializeJson(resp, body); jsend(200, body);
    }

    // ── GET /api/photos ───────────────────────────────────────────────────────
    void handleListPhotos() {
        Session s; if (!requireAuth(s)) return;
        if (!SdManager.isMounted()) { jsend(200, "{\"photos\":[]}"); return; }

        // Build UID→name map from employees (read once)
        JsonDocument uidMap;
        if (SD.exists(EMPLOYEES_FILE)) {
            File ef = SD.open(EMPLOYEES_FILE, FILE_READ);
            if (ef) {
                JsonDocument empDoc;
                if (!deserializeJson(empDoc, ef)) {
                    for (JsonObject emp : empDoc["employees"].as<JsonArray>()) {
                        String name = String(emp["first_name"]|"") + " " + String(emp["last_name"]|"");
                        name.trim();
                        for (JsonObject card : emp["cards"].as<JsonArray>()) {
                            String uid = String(card["uid"] | "");
                            uid.toUpperCase();
                            if (uid.length()) uidMap[uid] = name;
                        }
                    }
                }
                ef.close();
            }
        }

        String out = "{\"photos\":[";
        bool first = true;
        File dir = SD.open("/photos");
        if (dir && dir.isDirectory()) {
            File entry;
            while ((entry = dir.openNextFile())) {
                if (entry.isDirectory()) { entry.close(); continue; }
                String fname = String(entry.name());
                entry.close();
                int slash = fname.lastIndexOf('/');
                if (slash >= 0) fname = fname.substring(slash + 1);
                if (!fname.endsWith(".jpg")) continue;

                // Filename: <UID>_<YYYYMMDD>_<HHMMSS>.jpg
                int us = fname.indexOf('_');
                String uid = us > 0 ? fname.substring(0, us) : fname;
                String name = uidMap[uid].as<String>();

                // Rebuild human timestamp from filename parts
                String ts = "";
                if (us > 0 && (int)fname.length() >= us + 16) {
                    String rest = fname.substring(us + 1);
                    if (rest.length() >= 15)
                        ts = rest.substring(0,4)+"-"+rest.substring(4,6)+"-"+rest.substring(6,8)
                           +" "+rest.substring(9,11)+":"+rest.substring(11,13)+":"+rest.substring(13,15);
                }

                if (!first) out += ",";
                first = false;
                String safeName = name; safeName.replace("\"", "\\\"");
                out += "{\"file\":\""+fname+"\",\"uid\":\""+uid+"\",\"name\":\""+safeName+"\",\"ts\":\""+ts+"\"}";
            }
            dir.close();
        }
        out += "]}";
        jsend(200, out);
    }

    // ── GET / and /admin ──────────────────────────────────────────────────────
    void handleRoot() {
        // Try SD first (combined login+admin page)
        if (SdManager.isMounted() && SD.exists("/www/index.html")) {
            File f = SD.open("/www/index.html", FILE_READ);
            if (f && f.size() > 0) {
                Serial.printf("[Web] Serving / from SD (%u bytes)\n", f.size());
                _server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
                _server.sendHeader("Pragma", "no-cache");
                _server.streamFile(f, "text/html");
                f.close(); return;
            }
            if (f) f.close();
            Serial.println("[Web] SD index.html empty or unreadable");
        } else {
            Serial.printf("[Web] SD mounted=%d exists=%d\n",
                          SdManager.isMounted(),
                          SdManager.isMounted() ? SD.exists("/www/index.html") : 0);
        }
        // PROGMEM fallback — basic setup page when SD not available
        Serial.println("[Web] Serving / from PROGMEM (SD not mounted or file missing)");
        _server.send_P(200, "text/html", PAGE_HTML);
    }

    // ── GET /api/local/employees ──────────────────────────────────────────────
    void handleLocalEmployees() {
        Session s; if (!requireAuth(s)) return;
        if (!SdManager.isMounted() || !SD.exists(EMPLOYEES_FILE)) {
            jsend(200,"{\"employees\":[],\"offline\":true}"); return;
        }
        File f = SD.open(EMPLOYEES_FILE, FILE_READ);
        if (!f) { jsend(500,"{\"error\":\"read_failed\"}"); return; }
        cors(); _server.streamFile(f,"application/json"); f.close();
    }

    // ── GET /api/local/events ─────────────────────────────────────────────────
    void handleLocalEvents() {
        Session s; if (!requireAuth(s)) return;
        int limit = _server.hasArg("limit")
            ? (int)min(_server.arg("limit").toInt(), 500L) : 100;
        if (!SdManager.isMounted() || !SD.exists(EVENTS_LOG)) {
            jsend(200,"{\"events\":[],\"offline\":true}"); return;
        }
        File f = SD.open(EVENTS_LOG, FILE_READ);
        if (!f) { jsend(500,"{\"error\":\"read_failed\"}"); return; }
        std::vector<String> lines; lines.reserve(limit+1);
        while (f.available()) {
            String l = f.readStringUntil('\n'); l.trim();
            if (l.length() < 5) continue;
            lines.push_back(l);
            if ((int)lines.size() > limit) lines.erase(lines.begin());
        }
        f.close();
        String out = "{\"events\":[";
        for (int i=(int)lines.size()-1; i>=0; i--) {
            if (i<(int)lines.size()-1) out += ",";
            out += lines[i];
        }
        out += "]}";
        jsend(200, out);
    }

    // ── ANY /api/proxy/* ──────────────────────────────────────────────────────
    void handleProxy() {
        Session s; if (!requireAuth(s)) return;
        String path = _server.uri().substring(10);
        String body = _server.hasArg("plain") ? _server.arg("plain") : "";
        bool first = true;
        for (int i=0; i<_server.args(); i++) {
            if (_server.argName(i)=="plain"||_server.argName(i)=="token") continue;
            path += (first?"?":"&"); first=false;
            path += _server.argName(i) + "=" + _server.arg(i);
        }
        String method;
        switch (_server.method()) {
            case HTTP_GET:    method="GET";    break;
            case HTTP_POST:   method="POST";   break;
            case HTTP_PUT:    method="PUT";    break;
            case HTTP_DELETE: method="DELETE"; break;
            default:          method="GET";
        }
        String out; int code = ApiClient.proxy(method, path, body, out);
        jsend(code, out);
    }

public:
    void begin() {
        const char* headers[] = {"X-Session-Token"};
        _server.collectHeaders(headers, 1);

        // SPIFFS not used — SD card serves all web content

        // Auth (no token required)
        _server.on("/api/auth/login",           HTTP_POST, [this]{ handleLogin(); });
        _server.on("/api/auth/logout",          HTTP_POST, [this]{ handleLogout(); });
        _server.on("/api/auth/me",              HTTP_GET,  [this]{ handleMe(); });
        _server.on("/api/auth/change-password", HTTP_POST, [this]{ handleChangePassword(); });

        // Main page
        _server.on("/",      HTTP_GET, [this]{ handleRoot(); });
        _server.on("/admin", HTTP_GET, [this]{ handleRoot(); });

        // Protected
        _server.on("/api/status",            HTTP_GET,    [this]{ handleStatus(); });
        _server.on("/api/config",            HTTP_POST,   [this]{ handleConfig(); });
        _server.on("/api/open",              HTTP_POST,   [this]{ handleOpen(); });
        _server.on("/api/reset",             HTTP_POST,   [this]{ handleReset(); });
        _server.on("/api/admins",            HTTP_GET,    [this]{ handleListAdmins(); });
        _server.on("/api/admins",            HTTP_POST,   [this]{ handleCreateAdmin(); });
        _server.on("/api/admins",            HTTP_DELETE, [this]{ handleDeleteAdmin(); });
        _server.on("/api/admins/reset-password", HTTP_POST, [this]{ handleResetAdminPassword(); });
        _server.on("/api/employees",         HTTP_POST,   [this]{ handleCreateEmployee(); });
        _server.on("/api/employees/update",  HTTP_POST,   [this]{ handleUpdateEmployee(); });
        _server.on("/api/employees/delete",  HTTP_POST,   [this]{ handleDeleteEmployee(); });
        _server.on("/api/employees/card",        HTTP_POST,   [this]{ handleAssignCard(); });
        _server.on("/api/employees/card/assign", HTTP_POST,   [this]{ handleDirectAssignCard(); });
        _server.on("/api/employees/pending", HTTP_GET,    [this]{ handlePendingTap(); });
        _server.on("/api/photos",            HTTP_GET,    [this]{ handleListPhotos(); });
        _server.on("/api/photos/delete",     HTTP_POST,   [this]{ handleDeletePhoto(); });

        _server.onNotFound([this]{
            const String& uri = _server.uri();
            if (_server.method()==HTTP_OPTIONS) { cors(); _server.send(204); return; }
            if (uri=="/api/local/employees")          { handleLocalEmployees(); return; }
            if (uri.startsWith("/api/local/events"))  { handleLocalEvents();    return; }
            if (uri=="/api/local/status")             { handleStatus();         return; }
            if (uri.startsWith("/api/proxy/"))        { handleProxy();          return; }
            // Serve photos — auth required (token via header or ?token= query param)
            if (uri.startsWith("/photos/") && uri.length() > 8) {
                Session ps;
                if (!AuthManager.validate(getToken(), ps)) {
                    cors(); _server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
                    return;
                }
                if (SdManager.isMounted() && SdManager.serveFile(_server, uri)) return;
                _server.send(404, "text/plain", "Photo not found");
                return;
            }
            // Serve any SD www file (JS, CSS, icons etc)
            if (SdManager.isMounted()) {
                String sdPath = "/www" + uri;
                if (SdManager.serveFile(_server, sdPath)) return;
            }
            _server.send(404,"text/plain","Not found");
        });

        _server.begin();
        Serial.println("[Web] :80  http://" + WiFi.localIP().toString());
    }

    void loop() { _server.handleClient(); }
} WebServerManager;
