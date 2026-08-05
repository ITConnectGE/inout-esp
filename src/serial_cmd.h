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
 *   restart
 *   forget wifi
 *   clear events
 *   clear photos
 *   clear logs
 *   clear all
 *   list employees
 *   remove <name>
 *   dump logs
 *   factory reset confirm
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "config.h"
#include "sd_manager.h"
#include "logger.h"

struct SerialCmdClass {
private:
    String _buf;

    // ── Helpers ───────────────────────────────────────────────────────────────

    static void hr() { Serial.println("----------------------------------------"); }

    void printHelp() {
        Serial.println();
        hr();
        Serial.println("  InOut v0.4.1 — Serial Commands");
        hr();
        Serial.println("  help                      This list");
        Serial.println("  status                    Device status");
        Serial.println("  restart                   Restart ESP32");
        Serial.println("  forget wifi               Erase WiFi credentials + restart");
        Serial.println("  clear events              Delete events log from SD");
        Serial.println("  clear photos              Delete all photos from SD");
        Serial.println("  clear logs                Delete device log from SD");
        Serial.println("  clear all                 Events + photos + logs");
        Serial.println("  list employees            List all employees on SD");
        Serial.println("  remove <#>                Remove employee by # from list");
        Serial.println("  dump logs                 Print device log to serial");
        Serial.println("  factory reset confirm     Full wipe + forget WiFi + restart");
        hr();
        Serial.println();
    }

    void printStatus() {
        Serial.println();
        hr();
        Serial.println("  Status");
        hr();
        Serial.printf("  Firmware : v0.4.1\n");
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
            char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
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

    void doForgetWifi() {
        Serial.println("[CMD] Erasing WiFi credentials...");
        WiFiManager wm;
        wm.resetSettings();
        delay(500);
        Serial.println("[CMD] Restarting...");
        ESP.restart();
    }

    void doFactoryReset() {
        Serial.println("[CMD] === FACTORY RESET ===");
        Serial.println("[CMD] Clearing SD data...");
        SdManager.clearEvents();
        SdManager.deleteAllPhotos();
        Logger.clearLog();
        // Remove whitelist and employees so they re-sync from server on next boot
        SD.remove("/data/whitelist.json");
        SD.remove("/data/employees.json");
        SD.remove("/data/time.json");
        SD.remove("/data/config.json");
        Serial.println("[CMD] Clearing NVS config...");
        Preferences p; p.begin("cp", false); p.clear(); p.end();
        Serial.println("[CMD] Erasing WiFi credentials...");
        WiFiManager wm; wm.resetSettings();
        delay(500);
        Serial.println("[CMD] Restarting — device will enter WiFi setup mode...");
        ESP.restart();
    }

    // ── Dispatcher ────────────────────────────────────────────────────────────

    void dispatch(const String& raw) {
        String cmd = raw; cmd.trim();
        String lo  = cmd; lo.toLowerCase();

        if (lo == "help") {
            printHelp();

        } else if (lo == "status") {
            printStatus();

        } else if (lo == "restart") {
            Serial.println("[CMD] Restarting...");
            delay(100);
            ESP.restart();

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
            } else if (_buf.length() < 128) {
                _buf += ch;
            }
        }
    }
} SerialCmd;
