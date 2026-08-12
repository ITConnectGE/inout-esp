#pragma once
/**
 * serial_cmd.h — USB serial command interface
 *
 * Type commands in any serial terminal at 115200 baud.
 * Each line is processed on Enter (\n or \r\n).
 *
 * Commands:
 *   help
 *   status
 *   config
 *   restart
 *   open                                 Trigger relay (hardware test)
 *   nfc status                           Live re-check both PN532 readers
 *   sync                                 Force whitelist+events+employees sync now
 *   set server <url>
 *   set token <token>
 *   set tz <IANA name>
 *   forget wifi
 *   clear events
 *   clear photos
 *   clear logs
 *   clear all
 *   list employees
 *   list cards
 *   list admins
 *   remove <#>
 *   add admin <username> <password> [role]
 *   reset password <username> <new_password>
 *   dump logs
 *   factory reset confirm
 *   new factory password                 Rotate factory password (active on next factory reset)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "sd_manager.h"
#include "logger.h"
#include "web_server.h"
#include "api_client.h"
#include "nfc_reader.h"
#include "auth_manager.h"
#include "relay.h"

struct SerialCmdClass {
private:
    String _buf;

    // ── Helpers ───────────────────────────────────────────────────────────────

    static void hr() { Serial.println("----------------------------------------"); }

    void printHelp() {
        Serial.println();
        hr();
        Serial.println("  InOut v0.4.2 — Serial Commands");
        hr();
        Serial.println("  help                      This list");
        Serial.println("  status                    Device status");
        Serial.println("  config                    Dump current configuration");
        Serial.println("  restart                   Restart ESP32");
        Serial.println("  open                      Trigger relay (hardware test)");
        Serial.println("  nfc status                Live re-check both PN532 readers");
        Serial.println("  sync                      Force whitelist+events+employees sync now");
        Serial.println("  set <server|token|tz> <value>     Set backend URL, device token, or IANA timezone");
        Serial.println("  forget wifi               Erase WiFi credentials + restart");
        Serial.println("  clear <events|photos|logs|all>    Clear SD data  (all = events+photos+logs)");
        Serial.println("  list <employees|cards|admins>     List employees, cards, or admin accounts");
        Serial.println("  remove <#>                Remove employee by # from list");
        Serial.println("  add admin <user> <pass> [role]   Create admin (role: admin/super_admin)");
        Serial.println("  reset password <user> <newpass>  Force-reset an admin's password");
        Serial.println("  dump logs                 Print device log to serial");
        Serial.println("  factory reset confirm     Full wipe + forget WiFi + restart");
        Serial.println("  new factory password      Generate new factory password (active on next reset)");
        hr();
        Serial.println();
    }

    void printStatus() {
        Serial.println();
        hr();
        Serial.println("  Status");
        hr();
        Serial.printf("  Firmware : v0.4.2\n");
        Serial.printf("  Uptime   : %lu s\n", millis() / 1000);
        bool online = WiFi.status() == WL_CONNECTED;
        Serial.printf("  WiFi     : %s", online ? "connected" : "offline");
        if (online) Serial.printf("  IP=%s  RSSI=%d dBm",
                                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
        Serial.println();
        Serial.printf("  Server   : %s\n", Config.serverUrl.c_str());
        Serial.printf("  SD       : %s", SdManager.isMounted() ? "mounted" : "NOT MOUNTED");
        if (SdManager.isMounted())
            Serial.printf("  used=%llu MB / %llu MB", SdManager.usedMB(), SdManager.totalMB());
        Serial.println();
        Serial.printf("  Unsynced : %d events\n", SdManager.unsyncedCount());
        Serial.printf("  Free heap: %u bytes\n", ESP.getFreeHeap());
        struct tm t;
        if (getLocalTime(&t, 100)) {
            // Local time, real offset — not UTC, so no literal "Z" (see
            // ApiClient::nowIso() / Logger::timestamp() for the same fix).
            char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
            Serial.printf("  Time     : %s\n", buf);
        } else {
            Serial.printf("  Time     : not set\n");
        }
        hr();
        Serial.println();
    }

    void dumpLogs() {
        File f = SD.open(DEVICE_LOG, FILE_READ);
        if (!f) { Serial.println("[CMD] No log file found."); return; }
        Serial.println();
        hr();
        Serial.println("  device.log");
        hr();
        while (f.available()) {
            String line = f.readStringUntil('\n');
            Serial.println(line);
        }
        f.close();
        hr();
        Serial.println();
    }

    // Both commands delegate to WebServerManager.performReset() — the same
    // routine POST /api/reset uses — so serial and web resets can't drift.
    void doForgetWifi() {
        Serial.println("[CMD] Erasing WiFi credentials...");
        WebServerManager.performReset(false);
    }

    void doFactoryReset() {
        Serial.println("[CMD] === FACTORY RESET ===");
        WebServerManager.performReset(true);
    }

    void doOpen() {
        Relay.open(Config.relayMs);
        Serial.printf("[CMD] Relay opened for %dms\n", Config.relayMs);
    }

    void doNfcStatus() {
        Serial.println();
        hr();
        Serial.println("  NFC Readers (live)");
        hr();
        uint32_t vIn  = NfcReader.ping(READER_IN);
        uint32_t vOut = NfcReader.ping(READER_OUT);
        Serial.printf("  IN   CS=GPIO%-3d  %s", Config.csPin_IN, vIn ? "OK " : "FAIL");
        if (vIn) Serial.printf("  PN5%02x v%d.%d", (vIn>>24)&0xFF, (vIn>>16)&0xFF, (vIn>>8)&0xFF);
        Serial.println();
        Serial.printf("  OUT  CS=GPIO%-3d  %s", Config.csPin_OUT, vOut ? "OK " : "FAIL");
        if (vOut) Serial.printf("  PN5%02x v%d.%d", (vOut>>24)&0xFF, (vOut>>16)&0xFF, (vOut>>8)&0xFF);
        Serial.println();
        hr();
        Serial.println();
    }

    void doSync() {
        Serial.println("[CMD] Syncing with server...");
        bool wl = ApiClient.syncWhitelist();
        int  ev = ApiClient.syncEvents();
        int  em = ApiClient.syncEmployees();
        ApiClient.sendHeartbeat();
        Serial.printf("[CMD] Whitelist: %s  Events synced: %d  Employees synced: %d\n",
                      wl ? "ok" : "failed", ev, em);
    }

    // Splits on single spaces, skipping empty tokens (repeated spaces).
    static void splitTokens(const String& s, std::vector<String>& out) {
        int start = 0;
        while (start < (int)s.length()) {
            int sp = s.indexOf(' ', start);
            if (sp < 0) sp = s.length();
            if (sp > start) out.push_back(s.substring(start, sp));
            start = sp + 1;
        }
    }

    void doAddAdmin(const String& args) {
        std::vector<String> tok; splitTokens(args, tok);
        if (tok.size() < 2) {
            Serial.println("[CMD] Usage: add admin <username> <password> [role]");
            return;
        }
        String username = tok[0], password = tok[1];
        String role = tok.size() > 2 ? tok[2] : "admin";
        if (role != "admin" && role != "super_admin") role = "admin";
        if (username.length() < 3) { Serial.println("[CMD] Username min 3 chars"); return; }
        if (password.length() < 8) { Serial.println("[CMD] Password min 8 chars"); return; }
        if (AuthManager.addAdmin(username, password, role))
            Serial.printf("[CMD] Admin '%s' created (role=%s). Must change password on first login.\n",
                          username.c_str(), role.c_str());
        else
            Serial.println("[CMD] Failed — username already exists.");
    }

    void doResetPassword(const String& args) {
        std::vector<String> tok; splitTokens(args, tok);
        if (tok.size() != 2) {
            Serial.println("[CMD] Usage: reset password <username> <new_password>");
            return;
        }
        String username = tok[0], newPass = tok[1];
        if (newPass.length() < 8) { Serial.println("[CMD] Password min 8 chars"); return; }
        if (AuthManager.setPassword(username, newPass))
            Serial.printf("[CMD] Password reset for '%s'. Must change on next login.\n", username.c_str());
        else
            Serial.println("[CMD] Admin not found.");
    }

    // ── Dispatcher ────────────────────────────────────────────────────────────

    void dispatch(const String& raw) {
        String cmd = raw; cmd.trim();
        String lo  = cmd; lo.toLowerCase();

        if (lo == "help") {
            printHelp();

        } else if (lo == "status") {
            printStatus();

        } else if (lo == "config") {
            Config.printConfig();

        } else if (lo == "restart") {
            Serial.println("[CMD] Restarting...");
            delay(100);
            ESP.restart();

        } else if (lo == "open") {
            doOpen();

        } else if (lo == "nfc status") {
            doNfcStatus();

        } else if (lo == "sync") {
            doSync();

        } else if (lo.startsWith("set server ")) {
            Config.serverUrl = cmd.substring(11);
            Config.save();
            Serial.printf("[CMD] Server set to: %s\n", Config.serverUrl.c_str());

        } else if (lo.startsWith("set token ")) {
            Config.deviceToken = cmd.substring(10);
            Config.save();
            Serial.printf("[CMD] Token set (%d chars).\n", Config.deviceToken.length());

        } else if (lo.startsWith("set tz ")) {
            Config.timezone = cmd.substring(7);
            Config.save();
            setenv("TZ", Config.getPosixTz().c_str(), 1);
            tzset();
            Serial.printf("[CMD] Timezone set to: %s (%s)\n",
                          Config.timezone.c_str(), Config.getPosixTz().c_str());

        } else if (lo == "forget wifi") {
            doForgetWifi();

        } else if (lo == "clear events") {
            SdManager.clearEvents();
            Serial.println("[CMD] Events log cleared.");

        } else if (lo == "clear photos") {
            SdManager.deleteAllPhotos();
            Serial.println("[CMD] All photos deleted.");

        } else if (lo == "clear logs") {
            Logger.clearLog();
            Serial.println("[CMD] Device log cleared.");

        } else if (lo == "clear all") {
            SdManager.clearEvents();
            SdManager.deleteAllPhotos();
            Logger.clearLog();
            Serial.println("[CMD] Events, photos, and logs cleared.");

        } else if (lo == "dump logs") {
            dumpLogs();

        } else if (lo == "list employees") {
            Serial.println();
            SdManager.printEmployees();
            Serial.println();

        } else if (lo == "list cards") {
            SdManager.printCards();

        } else if (lo == "list admins") {
            AuthManager.printAdmins();

        } else if (lo.startsWith("add admin ")) {
            doAddAdmin(cmd.substring(10));

        } else if (lo.startsWith("reset password ")) {
            doResetPassword(cmd.substring(15));

        } else if (lo.startsWith("remove ")) {
            String arg = cmd.substring(7); arg.trim();
            bool isNum = arg.length() > 0;
            for (char c : arg) if (!isdigit(c)) { isNum = false; break; }
            if (!isNum) {
                Serial.println("[CMD] Usage: remove <#>  (run 'list employees' to see numbers)");
            } else {
                int idx = arg.toInt();
                if (SdManager.deleteEmployeeByIndex(idx))
                    Serial.printf("[CMD] Employee #%d removed.\n", idx);
                else
                    Serial.printf("[CMD] No employee at #%d — run 'list employees' first.\n", idx);
            }

        } else if (lo == "factory reset confirm") {
            doFactoryReset();

        } else if (lo == "factory reset") {
            Serial.println("[CMD] Type  factory reset confirm  to proceed.");

        } else if (lo == "new factory password") {
            AuthManager.generateNewFactoryPassword();

        } else if (lo.length() > 0) {
            Serial.printf("[CMD] Unknown command: \"%s\"  (type 'help' for list)\n", cmd.c_str());
        }
    }

public:
    void loop() {
        while (Serial.available()) {
            char ch = (char)Serial.read();
            if (ch == '\r') continue;
            if (ch == '\n') {
                _buf.trim();
                if (_buf.length()) dispatch(_buf);
                _buf = "";
            } else if (_buf.length() < 256) {
                _buf += ch;
            }
        }
    }
} SerialCmd;
