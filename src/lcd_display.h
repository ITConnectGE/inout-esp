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
 *        Line 1: first name, then last name, shown as two sequential phases
 *                (each Georgian-aware, scrolls if >LCD_COLS chars) — showing
 *                "First Last" combined in one scroll routinely needs more
 *                distinct Georgian letters than the LCD's 8 CGRAM slots
 *                allow at once, so splitting gives each part its own budget.
 *        Reverts to idle after both phases have shown (extended for long names)
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
#define NAME_PHASE_MIN_MS 1800

struct LcdDisplayClass {
private:
    LiquidCrystal_I2C* _lcd = nullptr;
    bool     _found         = false;
    bool     _showingTap    = false;
    uint32_t _tapShownAt    = 0;
    uint32_t _tapTimeoutMs  = TAP_TIMEOUT_MS;
    bool     _showingNotice = false;
    uint32_t _noticeUntil   = 0;
    // First/last name shown as two sequential phases instead of one
    // combined scroll — see showTap() for why.
    String   _nameFirst, _nameLast;
    bool     _showingLastPart   = false;
    uint32_t _namePhaseSwitchAt = 0;
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
        printLine(0, "  InOut v" FIRMWARE_VERSION);
        printLine(1, "  Starting...   ");
        _lastClockMs = millis();
    }

    bool isFound() { return _found; }

    void loop() {
        if (!_found) return;
        if (_showingTap) {
            // Slide the name into view if it's longer than the display width.
            GeorgianLcd.tick(GEORGIAN_SCROLL_STEP_MS);
            // Switch from first-name to last-name phase partway through the
            // tap display. Each phase is its own startScroll() call, so it
            // gets a fresh 8-slot CGRAM budget scoped to just that name part
            // — showing "First Last" combined in one scroll routinely needs
            // more than 8 distinct Georgian letters and falls back to '?'.
            if (_namePhaseSwitchAt && !_showingLastPart && millis() >= _namePhaseSwitchAt) {
                _showingLastPart = true;
                GeorgianLcd.startScroll(1, LCD_COLS, _nameLast);
                uint32_t lastNeeded = GeorgianLcd.scrollDurationMs(GEORGIAN_SCROLL_STEP_MS) + 700;
                uint32_t lastPhaseMs = lastNeeded > NAME_PHASE_MIN_MS ? lastNeeded : NAME_PHASE_MIN_MS;
                _tapTimeoutMs = (uint32_t)(millis() - _tapShownAt) + lastPhaseMs;
            }
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

        // Show first name, then last name, as two sequential phases rather
        // than scrolling "First Last" combined — see the phase-switch note
        // in loop() for why. Only real names get split; the "Unknown card"
        // fallback (empty name — denied/unrecognised tap) stays one phase.
        bool hasName = name.length() > 0;
        String shown = hasName ? name : "Unknown card";
        int sp = hasName ? shown.indexOf(' ') : -1;
        _nameFirst = sp > 0 ? shown.substring(0, sp) : shown;
        _nameLast  = sp > 0 ? shown.substring(sp + 1) : "";
        _nameLast.trim();
        _showingLastPart = false;

        GeorgianLcd.startScroll(1, LCD_COLS, _nameFirst);
        _showingTap = true;
        _tapShownAt = millis();

        uint32_t firstNeeded = GeorgianLcd.scrollDurationMs(GEORGIAN_SCROLL_STEP_MS) + 700;
        uint32_t firstPhaseMs = firstNeeded > NAME_PHASE_MIN_MS ? firstNeeded : NAME_PHASE_MIN_MS;
        if (_nameLast.length() > 0) {
            _namePhaseSwitchAt = millis() + firstPhaseMs;
            _tapTimeoutMs = firstPhaseMs; // extended when the last-name phase starts
        } else {
            _namePhaseSwitchAt = 0;
            _tapTimeoutMs = firstPhaseMs > TAP_TIMEOUT_MS ? firstPhaseMs : TAP_TIMEOUT_MS;
        }
    }

    void showBoot(const String& msg) {
        if (!_found) return;
        printLine(0, "  InOut v" FIRMWARE_VERSION);
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

    // OTA display — called from syncTask while main loop is frozen; loop() is not
    // running so these write directly without using the notice/tap state machine.
    void showOta(const String& version) {
        if (!_found) return;
        _showingTap    = false;
        _showingNotice = false;
        _lcd->clear(); delay(5);
        printLine(0, "Updating v" + version);
        printLine(1, "Connecting...   ");
    }

    void showOtaProgress(int pct) {
        if (!_found) return;
        char line[17];
        snprintf(line, sizeof(line), "  Progress: %3d%%", pct);
        printLine(1, String(line));
    }

    void showOtaError() {
        if (!_found) return;
        _lcd->clear(); delay(5);
        printLine(0, " Update failed!");
        printLine(1, " Rebooting...  ");
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
