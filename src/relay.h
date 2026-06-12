#pragma once
#include <Arduino.h>

struct RelayClass {
private:
    int  _pin = -1;
    bool _activeHigh = true;
public:
    void begin(int pin, bool activeHigh = true) {
        _pin = pin; _activeHigh = activeHigh;
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, _activeHigh ? LOW : HIGH);
        Serial.printf("[Relay] GPIO%d activeHigh=%d\n", _pin, _activeHigh);
    }
    void open(int ms = 3000) {
        if (_pin < 0) return;
        digitalWrite(_pin, _activeHigh ? HIGH : LOW);
        delay(ms);
        digitalWrite(_pin, _activeHigh ? LOW : HIGH);
    }
    void setActiveHigh(bool v) { _activeHigh = v; }
} Relay;
