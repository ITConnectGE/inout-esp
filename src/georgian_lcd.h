#pragma once
/**
 * georgian_lcd.h — renders UTF-8 Georgian (Mkhedruli) text on a stock HD44780
 * 16x2 character LCD.
 *
 * The LCD only has 8 CGRAM slots (custom characters) and Georgian has 33
 * letters, so glyphs are loaded on demand: each call to print()/printPadded()
 * scans the text once, assigns the distinct Georgian letters it finds to a
 * small pool of reserved slots, uploads their bitmaps, then writes the line.
 * Slots are NOT reused mid-call, so a single print can safely display up to
 * GEORGIAN_LCD_SLOT_COUNT distinct Georgian letters at once.
 *
 * Text that needs MORE distinct letters than that at once (rare within a
 * single 16-char window, but possible) falls back to '?' for the overflow
 * letters — there's no way around that with only 8 CGRAM slots on the chip.
 *
 * Text LONGER than the display (more than `cols` characters) is handled
 * separately by startScroll()/tick(): if it fits, it's shown statically in
 * one shot; if not, it slides left through the text like a classic LCD
 * ticker, a character at a time, dwelling briefly at both ends.
 *
 * All 8 CGRAM slots are available for Georgian glyphs (the tap screen uses
 * plain "OK"/"NO" text instead of custom check/cross icons, specifically
 * to leave every slot free for letters).
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include "georgian_glyphs.h"

#define GEORGIAN_LCD_SLOT_START 0
#define GEORGIAN_LCD_SLOT_COUNT 8
#define GEORGIAN_LCD_MAX_CHARS  64

struct GeorgianLcdClass {
private:
    LiquidCrystal_I2C* _lcd = nullptr;

    // Look up a 3-byte UTF-8 sequence in the glyph table. Returns index or -1.
    int findGlyph(const uint8_t* p) {
        for (int g = 0; g < GEORGIAN_GLYPH_COUNT; g++) {
            const uint8_t* u = (const uint8_t*)GEORGIAN_GLYPHS[g].utf8;
            if (u[0] == p[0] && u[1] == p[1] && u[2] == p[2]) return g;
        }
        return -1;
    }

public:
    void begin(LiquidCrystal_I2C* lcd) { _lcd = lcd; }

    // Prints UTF-8 text (ASCII + Georgian mixed) at (col, row) on the LCD.
    // Returns the number of *characters* written (not bytes).
    //
    // IMPORTANT: createChar() leaves the HD44780's internal address counter
    // pointed at CGRAM, not DDRAM — any write() issued right after it would
    // silently land in CGRAM instead of on the visible screen (blank line).
    // So the cursor is (re-)set here, AFTER uploading glyph bitmaps and
    // immediately before the actual write pass, rather than trusting the
    // caller's setCursor() from before createChar() ran.
    int print(int row, int col, const String& text, int maxChars = 255) {
        if (!_lcd) return 0;
        const uint8_t* p = (const uint8_t*)text.c_str();
        size_t n = text.length();

        // Pass 1 — assign reserved slots to the distinct Georgian letters
        // that actually appear, in order of first appearance.
        int slotGlyph[GEORGIAN_LCD_SLOT_COUNT];
        int slotsUsed = 0;
        for (int i = 0; i < GEORGIAN_LCD_SLOT_COUNT; i++) slotGlyph[i] = -1;

        int printedChars = 0;
        size_t i = 0;
        while (i < n && printedChars < maxChars) {
            uint8_t b0 = p[i];
            if (b0 < 0x80) { i += 1; printedChars++; continue; }
            if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {
                int g = findGlyph(p + i);
                if (g >= 0) {
                    bool known = false;
                    for (int s = 0; s < slotsUsed; s++) {
                        if (slotGlyph[s] == g) { known = true; break; }
                    }
                    if (!known && slotsUsed < GEORGIAN_LCD_SLOT_COUNT) {
                        slotGlyph[slotsUsed++] = g;
                    }
                }
                i += 3;
            } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {
                i += 2;
            } else {
                i += 1;
            }
            printedChars++;
        }

        // Upload the bitmaps for this line's distinct letters.
        for (int s = 0; s < slotsUsed; s++) {
            _lcd->createChar(GEORGIAN_LCD_SLOT_START + s,
                              (uint8_t*)GEORGIAN_GLYPHS[slotGlyph[s]].bitmap);
        }

        // Re-target DDRAM now that CGRAM uploads (if any) are done.
        _lcd->setCursor(col, row);

        // Pass 2 — write the line using the slots assigned above.
        printedChars = 0;
        i = 0;
        while (i < n && printedChars < maxChars) {
            uint8_t b0 = p[i];
            if (b0 < 0x80) {
                _lcd->write(b0);
                i += 1;
            } else if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {
                int g = findGlyph(p + i);
                int slot = -1;
                if (g >= 0) {
                    for (int s = 0; s < slotsUsed; s++) {
                        if (slotGlyph[s] == g) { slot = s; break; }
                    }
                }
                _lcd->write(slot >= 0 ? (GEORGIAN_LCD_SLOT_START + slot) : '?');
                i += 3;
            } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {
                _lcd->write('?');
                i += 2;
            } else {
                i += 1;
                continue; // stray continuation byte — don't count it
            }
            printedChars++;
        }
        return printedChars;
    }

    // Prints text at (0, row) and pads/truncates to exactly `cols` characters.
    void printPadded(int row, const String& text, int cols) {
        if (!_lcd) return;
        int printed = print(row, 0, text, cols);
        for (int i = printed; i < cols; i++) _lcd->write(' ');
    }

    // ── Scrolling display ────────────────────────────────────────────────────
    // Text that fits in `cols` characters is shown statically, once. Longer
    // text slides through a `cols`-wide window, one character at a time,
    // dwelling at the start and end so it's readable. Call tick() regularly
    // (e.g. from the LCD's loop()) while scrolling text is on screen.
private:
    String   _scrollText;
    uint16_t _charOffsets[GEORGIAN_LCD_MAX_CHARS + 1]; // byte offset where each character starts
    uint16_t _totalChars   = 0;
    uint16_t _scrollStart  = 0; // character index of the window's left edge
    uint32_t _scrollShownAt = 0;
    bool     _scrolling    = false;
    int      _scrollRow = 0, _scrollCols = 16;

    // Fills _charOffsets/_totalChars by walking UTF-8 codepoint boundaries.
    void computeCharOffsets() {
        const uint8_t* p = (const uint8_t*)_scrollText.c_str();
        size_t n = _scrollText.length();
        size_t i = 0;
        _totalChars = 0;
        _charOffsets[0] = 0;
        while (i < n && _totalChars < GEORGIAN_LCD_MAX_CHARS) {
            uint8_t b0 = p[i];
            size_t step = 1;
            if (b0 < 0x80) step = 1;
            else if ((b0 & 0xF0) == 0xE0 && i + 2 < n) step = 3;
            else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) step = 2;
            i += step;
            _totalChars++;
            _charOffsets[_totalChars] = i;
        }
    }

    void renderScrollWindow() {
        if (!_lcd) return;
        uint16_t endChar = _scrollStart + _scrollCols;
        if (endChar > _totalChars) endChar = _totalChars;
        size_t startByte = _charOffsets[_scrollStart];
        size_t endByte   = _charOffsets[endChar];
        int printed = print(_scrollRow, 0, _scrollText.substring(startByte, endByte), _scrollCols);
        for (int i = printed; i < _scrollCols; i++) _lcd->write(' ');
    }

public:
    // Begins display of `text` at (0, row) within a `cols`-wide field.
    // Shows it statically if it fits; otherwise starts the scroll at the
    // beginning and renders that first window immediately.
    void startScroll(int row, int cols, const String& text) {
        _scrollRow = row; _scrollCols = cols;
        _scrollText = text;
        computeCharOffsets();
        _scrollStart  = 0;
        _scrolling    = _totalChars > (uint16_t)cols;
        _scrollShownAt = millis();
        renderScrollWindow();
    }

    bool isScrolling() const { return _scrolling; }

    // Rough total time (ms) needed to see the full scroll at least once,
    // including end dwells — useful for sizing a display timeout.
    uint32_t scrollDurationMs(uint32_t stepMs) const {
        if (!_scrolling) return 0;
        uint16_t steps = _totalChars - _scrollCols; // number of positions past the start
        return (uint32_t)steps * stepMs + 2 * (stepMs * 3); // steps + dwell at both ends
    }

    // Call regularly while scrolling text is on screen. Advances the window
    // by one character every `stepMs`, dwelling 3x longer at both ends.
    // No-op if the text fit statically (the common case for short names).
    void tick(uint32_t stepMs) {
        if (!_scrolling) return;
        uint16_t maxStart = _totalChars - _scrollCols;
        uint32_t interval = (_scrollStart == 0 || _scrollStart == maxStart) ? stepMs * 3 : stepMs;
        if (millis() - _scrollShownAt < interval) return;
        _scrollShownAt = millis();
        _scrollStart = (_scrollStart < maxStart) ? _scrollStart + 1 : 0;
        renderScrollWindow();
    }
} GeorgianLcd;
