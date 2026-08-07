#pragma once
#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <vector>
#include <time.h>

#define DEVICE_LOG  "/data/device.log"
#define MAX_DEV_LOG 500

enum LogLevel { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 };

struct LoggerClass {
private:
    bool _sdReady = false;
    SemaphoreHandle_t _mutex = nullptr;

    static const char* lvlStr(LogLevel l) {
        switch (l) {
            case LOG_ERROR: return "ERROR";
            case LOG_WARN:  return "WARN";
            default:        return "INFO";
        }
    }

    String timestamp() {
        struct tm t;
        if (!getLocalTime(&t, 50)) {
            char buf[20];
            snprintf(buf, sizeof(buf), "~%lums", millis());
            return String(buf);
        }
        // getLocalTime() returns local (TZ-adjusted) time, not UTC — use the
        // real offset (%z), not a literal "Z", or every log timestamp reads
        // 4h ahead once something (dashboard, browser) converts it back
        // assuming it really was UTC. Same bug/fix as ApiClient::nowIso().
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
        return String(buf);
    }

    void appendToSD(const char* lvl, const char* mod, const String& msg, const String& det) {
        File f = SD.open(DEVICE_LOG, FILE_APPEND);
        if (!f) return;
        JsonDocument doc;
        doc["ts"]     = timestamp();
        doc["level"]  = lvl;
        doc["module"] = mod;
        doc["msg"]    = msg;
        if (det.length() > 0) doc["detail"] = det;
        serializeJson(doc, f);
        f.println();
        f.close();
    }

public:
    void begin() {
        _mutex = xSemaphoreCreateMutex();
    }

    void setSDReady(bool ready) { _sdReady = ready; }

    void log(LogLevel level, const char* module, const String& msg, const String& detail = "") {
        const char* lvl = lvlStr(level);
        if (detail.length() > 0)
            Serial.printf("[%s][%s] %s | %s\n", lvl, module, msg.c_str(), detail.c_str());
        else
            Serial.printf("[%s][%s] %s\n", lvl, module, msg.c_str());

        if (!_sdReady || !_mutex) return;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
        appendToSD(lvl, module, msg, detail);
        xSemaphoreGive(_mutex);
    }

    // printf-style convenience
    void logf(LogLevel level, const char* module, const char* fmt, ...) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log(level, module, String(buf));
    }

    void trimLog() {
        if (!_sdReady || !SD.exists(DEVICE_LOG)) return;
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;

        File f = SD.open(DEVICE_LOG, FILE_READ);
        if (!f) { xSemaphoreGive(_mutex); return; }
        int total = 0;
        while (f.available()) { f.readStringUntil('\n'); total++; }
        f.close();

        if (total <= MAX_DEV_LOG) { xSemaphoreGive(_mutex); return; }
        int skip = total - MAX_DEV_LOG;
        String tmp = "/data/dev.tmp";
        f = SD.open(DEVICE_LOG, FILE_READ);
        File dst = SD.open(tmp, FILE_WRITE);
        if (!f || !dst) {
            if (f) f.close();
            if (dst) dst.close();
            xSemaphoreGive(_mutex); return;
        }
        int i = 0;
        while (f.available()) {
            String l = f.readStringUntil('\n'); l.trim();
            if (i++ >= skip && l.length() > 0) dst.println(l);
        }
        f.close(); dst.close();
        SD.remove(DEVICE_LOG); SD.rename(tmp, DEVICE_LOG);
        xSemaphoreGive(_mutex);
    }

    // Returns last `limit` entries as JSON object {logs:[...]}, newest first.
    // lvlFilter: "INFO", "WARN", "ERROR", or "" for all.
    String readEntries(int limit = 200, const String& lvlFilter = "") {
        if (!_sdReady || !SD.exists(DEVICE_LOG)) return "{\"logs\":[]}";
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
            return "{\"logs\":[]}";

        File f = SD.open(DEVICE_LOG, FILE_READ);
        if (!f) { xSemaphoreGive(_mutex); return "{\"logs\":[]}"; }

        String needle = lvlFilter.length() > 0 ? ("\"" + lvlFilter + "\"") : "";
        std::vector<String> lines;
        lines.reserve(min(limit, 300));
        while (f.available()) {
            String l = f.readStringUntil('\n'); l.trim();
            if (l.length() < 5) continue;
            if (needle.length() > 0 && l.indexOf(needle) < 0) continue;
            lines.push_back(l);
            if ((int)lines.size() > limit) lines.erase(lines.begin());
        }
        f.close();
        xSemaphoreGive(_mutex);

        String out = "{\"logs\":[";
        for (int i = (int)lines.size() - 1; i >= 0; i--) {
            if (i < (int)lines.size() - 1) out += ",";
            out += lines[i];
        }
        out += "]}";
        return out;
    }

    void clearLog() {
        if (!_sdReady) return;
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
        SD.remove(DEVICE_LOG);
        xSemaphoreGive(_mutex);
    }
} Logger;
