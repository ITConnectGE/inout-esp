#pragma once
/**
 * lcd_display.h — 16x2 LCD via I2C
 * SDA=21  SCL=22
 *
 * Idle:  Line 0: "14:23      12/06"  (time left, date right)
 *        Line 1: "     InOut      "  (centered)
 *
 * Tap:   Line 0: "✓ GRANTED" / "✗ DENIED"
 *        Line 1: employee name
 *        Reverts to idle after TAP_TIMEOUT_MS
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include "config.h"

#define TAP_TIMEOUT_MS 4000
#define LCD_COLS 16

struct LcdDisplayClass {
private:
    LiquidCrystal_I2C* _lcd = nullptr;
    bool     _found         = false;
    bool     _showingTap    = false;
    uint32_t _tapShownAt    = 0;
    uint32_t _lastClockMs   = 0;
    String   _fallback      = "";
    uint32_t _fallbackUntil = 0;

    uint8_t _chCheck[8] = {0,1,3,22,28,8,0,0};
    uint8_t _chCross[8] = {0,17,10,4,10,17,0,0};

    // Print exactly LCD_COLS chars on a row — pads or truncates
    void printLine(int row, const String& s) {
        _lcd->setCursor(0, row);
        String p = s.substring(0, LCD_COLS);
        while ((int)p.length() < LCD_COLS) p += ' ';
        _lcd->print(p);
    }

    // "HH:MM      dd/mm"  — time on left (5), spaces, date on right (5)
    String clockLine() {
        struct tm t;
        if (!getLocalTime(&t, 50) || (_fallbackUntil && millis() < _fallbackUntil)) {
            if (_fallback.length() > 0) {
                String s = _fallback.substring(0, LCD_COLS);
                int pad = (LCD_COLS - (int)s.length()) / 2;
                String out = "";
                for (int i = 0; i < pad; i++) out += ' ';
                out += s;
                return out;
            }
            return "  Syncing NTP..";
        }
        char buf[17];
        snprintf(buf, sizeof(buf), "%02d:%02d      %02d/%02d",
                 t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
        return String(buf);
    }

    void renderIdle() {
        printLine(0, clockLine());
        printLine(1, "     InOut      ");
    }

public:
    void begin() {
        Wire.begin(PIN_SDA, PIN_SCL);
        uint8_t addr = 0;
        for (uint8_t a : {0x27, 0x3F}) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) { addr = a; break; }
        }
        if (!addr) {
            Serial.println("[LCD] Not found");
            return;
        }
        Serial.printf("[LCD] Found at 0x%02X\n", addr);
        _lcd = new LiquidCrystal_I2C(addr, 16, 2);
        _lcd->init();
        _lcd->backlight();
        _lcd->clear();                   // ← explicit clear on startup
        delay(50);
        _lcd->createChar(0, _chCheck);
        _lcd->createChar(1, _chCross);
        _found = true;
        // Show clean boot message
        printLine(0, "  InOut v0.3.1");
        printLine(1, "  Starting...   ");
        _lastClockMs = millis();
    }

    bool isFound() { return _found; }

    void loop() {
        if (!_found) return;
        // Clear tap message after timeout
        if (_showingTap && (millis() - _tapShownAt) > TAP_TIMEOUT_MS) {
            _lcd->clear();
            delay(5);
            renderIdle();
            _lastClockMs = millis();
            _showingTap = false;
        }
        // Update clock every second when idle
        if (!_showingTap && (millis() - _lastClockMs) >= 1000) {
            printLine(0, clockLine());
            _lastClockMs = millis();
        }
    }

    void showTap(bool granted, const String& name) {
        if (!_found) return;
        _lcd->clear();
        delay(5);
        _lcd->setCursor(0, 0);
        if (granted) { _lcd->write(byte(0)); _lcd->print(" GRANTED        "); }
        else         { _lcd->write(byte(1)); _lcd->print(" DENIED         "); }
        printLine(1, name.length() > 0 ? name : "Unknown card");
        _showingTap = true;
        _tapShownAt = millis();
    }

    void showBoot(const String& msg) {
        if (!_found) return;
        printLine(0, "  InOut v0.3.1");
        printLine(1, msg.substring(0, LCD_COLS));
    }

    void showWifi(const String& ssid) {
        if (!_found) return;
        printLine(0, "Connect to WiFi:");
        printLine(1, ssid.substring(0, LCD_COLS));
    }

    void setFallback(const String& s) { _fallback = s; }

    // Call once at end of setup() — clears boot messages and shows idle immediately
    void showReady() {
        if (!_found) return;
        _lcd->clear();
        delay(10);
        _showingTap    = false;
        _lastClockMs   = millis();
        _fallbackUntil = millis() + 10000;
        renderIdle();
    }
} Lcd;
