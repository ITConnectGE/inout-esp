#pragma once
#include <Arduino.h>

struct RelayClass {
private:
    int      _pin       = -1;
    bool     _activeHigh = true;
    bool     _isOpen    = false;
    uint32_t _closeAt   = 0;
public:
    void begin(int pin, bool activeHigh = true) {
        _pin = pin; _activeHigh = activeHigh;
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, _activeHigh ? LOW : HIGH);
        Serial.printf("[Relay] GPIO%d activeHigh=%d\n", _pin, _activeHigh);
    }
    // Non-blocking: opens relay and returns immediately
    void open(int ms = 3000) {
        if (_pin < 0) return;
        digitalWrite(_pin, _activeHigh ? HIGH : LOW);
        _closeAt = millis() + (uint32_t)ms;
        _isOpen  = true;
    }
    // Call from loop() — closes relay when timer expires
    void loop() {
        if (_isOpen && millis() >= _closeAt) {
            digitalWrite(_pin, _activeHigh ? LOW : HIGH);
            _isOpen = false;
        }
    }
    bool isOpen() { return _isOpen; }
    void setActiveHigh(bool v) { _activeHigh = v; }
} Relay;
