#pragma once
/**
 * lcd_display.h — 16x2 LCD via I2C
 * SDA=21  SCL=22
 *
 * Idle:  Line 0: "14:23      12/06"  (time left, date right)
 *        Line 1: "     InOut      "  (centered)
 *
 * Tap:   Line 0: "OK GRANTED" / "NO DENIED"  (plain text, not icons — keeps
 *                all 8 CGRAM slots free for Georgian letters in the name)
 *        Line 1: employee name (Georgian-aware, scrolls if >LCD_COLS chars)
 *        Reverts to idle after TAP_TIMEOUT_MS (extended for long names)
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include "config.h"
#include "georgian_lcd.h"

#define TAP_TIMEOUT_MS 4000
#define LCD_COLS 16
#define GEORGIAN_SCROLL_STEP_MS 400

struct LcdDisplayClass {
private:
    LiquidCrystal_I2C* _lcd = nullptr;
    bool     _found         = false;
    bool     _showingTap    = false;
    uint32_t _tapShownAt    = 0;
    uint32_t _tapTimeoutMs  = TAP_TIMEOUT_MS;
    bool     _showingNotice = false;
    uint32_t _noticeUntil   = 0;
    uint32_t _lastClockMs   = 0;
    String   _fallback      = "";
    uint32_t _fallbackUntil = 0;

    // Print exactly LCD_COLS chars on a row — pads or truncates.
    // Georgian-aware: UTF-8 Georgian letters are rendered via CGRAM glyphs.
    void printLine(int row, const String& s) {
        GeorgianLcd.printPadded(row, s, LCD_COLS);
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
        GeorgianLcd.begin(_lcd);         // no icons — all 8 CGRAM slots free for Georgian glyphs
        _found = true;
        // Show clean boot message
        printLine(0, "  InOut v0.4.0");
        printLine(1, "  Starting...   ");
        _lastClockMs = millis();
    }

    bool isFound() { return _found; }

    void loop() {
        if (!_found) return;
        if (_showingTap) {
            // Slide the name into view if it's longer than the display width.
            GeorgianLcd.tick(GEORGIAN_SCROLL_STEP_MS);
            // Clear tap message after timeout
            if ((millis() - _tapShownAt) > _tapTimeoutMs) {
                _lcd->clear();
                delay(5);
                renderIdle();
                _lastClockMs = millis();
                _showingTap = false;
            }
        } else if (_showingNotice) {
            if (millis() >= _noticeUntil) {
                _showingNotice = false;
                _lcd->clear();
                delay(5);
                renderIdle();
                _lastClockMs = millis();
            }
        } else if ((millis() - _lastClockMs) >= 1000) {
            // Update clock every second when idle
            printLine(0, clockLine());
            _lastClockMs = millis();
        }
    }

    // dir: "in", "out", or "" (empty = don't show direction)
    void showTap(bool granted, const String& name, const String& dir = "") {
        if (!_found) return;
        _lcd->clear();
        delay(5);
        String line0 = granted ? "OK GRANTED" : "NO DENIED";
        if      (dir == "in")  line0 += " IN";
        else if (dir == "out") line0 += " OUT";
        printLine(0, line0);
        GeorgianLcd.startScroll(1, LCD_COLS, name.length() > 0 ? name : "Unknown card");
        _showingTap = true;
        _tapShownAt = millis();
        // Give long names enough time to scroll into view fully at least once
        uint32_t neededMs = GeorgianLcd.scrollDurationMs(GEORGIAN_SCROLL_STEP_MS) + 700;
        _tapTimeoutMs = neededMs > TAP_TIMEOUT_MS ? neededMs : TAP_TIMEOUT_MS;
    }

    void showBoot(const String& msg) {
        if (!_found) return;
        printLine(0, "  InOut v0.4.0");
        printLine(1, msg.substring(0, LCD_COLS));
    }

    void showWifi(const String& ssid) {
        if (!_found) return;
        printLine(0, "Connect to WiFi:");
        printLine(1, ssid.substring(0, LCD_COLS));
    }

    void setFallback(const String& s) { _fallback = s; }

    // Show a temporary 2-line message, then revert to idle after durationMs.
    void showNotice(const String& line0, const String& line1, uint32_t durationMs = 4000) {
        if (!_found) return;
        _showingTap    = false;
        _showingNotice = true;
        _noticeUntil   = millis() + durationMs;
        _lcd->clear();
        delay(5);
        printLine(0, line0);
        printLine(1, line1);
    }

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
