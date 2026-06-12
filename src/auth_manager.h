#pragma once
/**
 * auth_manager.h — Admin accounts on SD card
 *
 * /data/admins.json:
 * {
 *   "admins": [
 *     {
 *       "id": "1",
 *       "username": "admin",
 *       "password_hash": "sha256hex",
 *       "role": "super_admin",  // or "admin"
 *       "must_change_password": true,
 *       "created_at": 1234567890
 *     }
 *   ]
 * }
 *
 * Sessions stored in RAM (cleared on reboot):
 *   token → {username, role, expires_at}
 *
 * Roles:
 *   super_admin — full access: device setup, manage admins, all pages
 *   admin       — dashboard, employees, cards only
 */

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <map>

#define ADMINS_FILE "/data/admins.json"
#define SESSION_TTL_MS (8UL * 60 * 60 * 1000)  // 8 hours

struct Session {
    String username;
    String role;
    uint32_t expiresAt;
};

struct AuthManagerClass {
private:
    std::map<String, Session> _sessions;

    // SHA-256 hex string
    String sha256(const String& input) {
        uint8_t hash[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t*)input.c_str(), input.length());
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);
        String hex = "";
        for (int i = 0; i < 32; i++) {
            if (hash[i] < 0x10) hex += "0";
            hex += String(hash[i], HEX);
        }
        return hex;
    }

    // Simple random token
    String makeToken() {
        char buf[33];
        for (int i = 0; i < 32; i++) {
            int r = random(36);
            buf[i] = r < 10 ? '0' + r : 'a' + r - 10;
        }
        buf[32] = 0;
        return String(buf);
    }

    bool loadDoc(JsonDocument& doc) {
        if (!SD.exists(ADMINS_FILE)) return false;
        File f = SD.open(ADMINS_FILE, FILE_READ);
        if (!f) return false;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        return !err;
    }

    bool saveDoc(JsonDocument& doc) {
        File f = SD.open(ADMINS_FILE, FILE_WRITE);
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        return true;
    }

public:
    void begin() {
        // Create default super admin if file doesn't exist
        if (!SD.exists(ADMINS_FILE)) {
            JsonDocument doc;
            JsonArray arr = doc["admins"].to<JsonArray>();
            JsonObject a = arr.add<JsonObject>();
            a["id"]                   = "1";
            a["username"]             = "admin";
            a["password_hash"]        = sha256("12345678");
            a["role"]                 = "super_admin";
            a["must_change_password"] = true;
            a["created_at"]           = (long)(millis()/1000);
            File f = SD.open(ADMINS_FILE, FILE_WRITE);
            if (f) { serializeJson(doc, f); f.close(); }
            Serial.println("[Auth] Default super admin created (admin/12345678)");
        } else {
            Serial.println("[Auth] Admins file loaded from SD");
        }
    }

    // ── Login ─────────────────────────────────────────────────────────────────
    // Returns session token on success, "" on failure
    String login(const String& username, const String& password, String& role, bool& mustChange) {
        JsonDocument doc;
        if (!loadDoc(doc)) {
            Serial.println("[Auth] Login failed: could not load admins.json");
            return "";
        }
        String inputHash = sha256(password);
        Serial.printf("[Auth] Login attempt: user=%s hash=%s\n",
                      username.c_str(), inputHash.c_str());
        JsonArray arr = doc["admins"].as<JsonArray>();
        Serial.printf("[Auth] Admins in file: %d\n", (int)arr.size());
        for (JsonObject a : arr) {
            String storedUser = String(a["username"] | "");
            String storedHash = String(a["password_hash"] | "");
            Serial.printf("[Auth]   checking: user=%s hash=%s\n",
                          storedUser.c_str(), storedHash.c_str());
            if (storedUser == username && storedHash == inputHash) {
                role       = String(a["role"] | "admin");
                mustChange = a["must_change_password"] | false;
                String token = makeToken();
                _sessions[token] = { username, role, millis() + SESSION_TTL_MS };
                Serial.printf("[Auth] Login OK: %s (%s)\n", username.c_str(), role.c_str());
                return token;
            }
        }
        Serial.printf("[Auth] Login FAILED for: %s\n", username.c_str());
        return "";
    }

    // ── Validate token ────────────────────────────────────────────────────────
    bool validate(const String& token, Session& session) {
        auto it = _sessions.find(token);
        if (it == _sessions.end()) return false;
        if (millis() > it->second.expiresAt) {
            _sessions.erase(it); return false;
        }
        session = it->second;
        return true;
    }

    void logout(const String& token) {
        _sessions.erase(token);
    }

    // ── Change password ───────────────────────────────────────────────────────
    bool changePassword(const String& username, const String& oldPass, const String& newPass) {
        JsonDocument doc;
        if (!loadDoc(doc)) return false;
        for (JsonObject a : doc["admins"].as<JsonArray>()) {
            if (String(a["username"] | "") == username &&
                String(a["password_hash"] | "") == sha256(oldPass)) {
                a["password_hash"]        = sha256(newPass);
                a["must_change_password"] = false;
                return saveDoc(doc);
            }
        }
        return false;
    }

    // Force password change (for super admin resetting another admin)
    bool setPassword(const String& username, const String& newPass) {
        JsonDocument doc;
        if (!loadDoc(doc)) return false;
        for (JsonObject a : doc["admins"].as<JsonArray>()) {
            if (String(a["username"] | "") == username) {
                a["password_hash"]        = sha256(newPass);
                a["must_change_password"] = true;
                return saveDoc(doc);
            }
        }
        return false;
    }

    // ── Admin CRUD (super admin only) ─────────────────────────────────────────
    bool addAdmin(const String& username, const String& password, const String& role) {
        JsonDocument doc;
        loadDoc(doc);
        if (!doc["admins"].is<JsonArray>()) doc["admins"].to<JsonArray>();

        // Check duplicate
        for (JsonObject a : doc["admins"].as<JsonArray>())
            if (String(a["username"] | "") == username) return false;

        String newId = String(millis());
        JsonObject a = doc["admins"].add<JsonObject>();
        a["id"]                   = newId;
        a["username"]             = username;
        a["password_hash"]        = sha256(password);
        a["role"]                 = role;
        a["must_change_password"] = true;
        a["created_at"]           = (long)(millis()/1000);
        return saveDoc(doc);
    }

    bool deleteAdmin(const String& username) {
        // Prevent deleting last super admin
        JsonDocument doc;
        if (!loadDoc(doc)) return false;
        int superCount = 0;
        for (JsonObject a : doc["admins"].as<JsonArray>())
            if (String(a["role"] | "") == "super_admin") superCount++;

        JsonArray arr = doc["admins"].as<JsonArray>();
        // Find and check target
        for (JsonObject a : arr)
            if (String(a["username"] | "") == username &&
                String(a["role"] | "") == "super_admin" && superCount <= 1)
                return false;  // can't delete last super admin

        // Rebuild without target
        JsonDocument newDoc;
        JsonArray newArr = newDoc["admins"].to<JsonArray>();
        for (JsonObject a : arr)
            if (String(a["username"] | "") != username) newArr.add(a);
        return saveDoc(newDoc);
    }

    // Return admins list as JSON string (passwords excluded)
    String getAdminsList() {
        JsonDocument doc;
        if (!loadDoc(doc)) return "{\"admins\":[]}";
        JsonDocument out;
        JsonArray arr = out["admins"].to<JsonArray>();
        for (JsonObject a : doc["admins"].as<JsonArray>()) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]                   = a["id"];
            o["username"]             = a["username"];
            o["role"]                 = a["role"];
            o["must_change_password"] = a["must_change_password"];
            o["created_at"]           = a["created_at"];
        }
        String s; serializeJson(out, s); return s;
    }

    bool isSuperAdmin(const String& token) {
        Session s;
        if (!validate(token, s)) return false;
        return s.role == "super_admin";
    }
} AuthManager;
