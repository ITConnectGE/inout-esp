#pragma once
/**
 * nfc_reader.h — Dual PN532 on dedicated VSPI (SCK=18 MISO=19 MOSI=23)
 * SD card is on HSPI — completely separate bus, zero conflict.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include "config.h"

enum ReaderIndex   { READER_IN = 0, READER_OUT = 1 };
enum CardDirection { DIR_IN    = 0, DIR_OUT    = 1 };

#define DEBOUNCE_MS 3000

extern SPIClass spiVSPI;

struct NfcReaderManager {
private:
    Adafruit_PN532* _r[2] = { nullptr, nullptr };
    String   _lastUid[2]  = { "", "" };
    uint32_t _lastMs[2]   = { 0, 0 };
    bool     _ok[2]       = { false, false };

    void deassertAll() {
        digitalWrite(Config.csPin_IN,  HIGH);
        digitalWrite(Config.csPin_OUT, HIGH);
    }

    String toHex(uint8_t* buf, uint8_t len) {
        String s;
        for (int i = 0; i < len; i++) {
            if (buf[i] < 0x10) s += "0";
            s += String(buf[i], HEX);
        }
        s.toUpperCase();
        return s;
    }

    bool initOne(int idx, const char* label) {
        deassertAll(); delay(10);
        _r[idx]->begin(); delay(150);
        deassertAll();
        _r[idx]->SAMConfig(); delay(50);
        deassertAll();
        uint32_t ver = _r[idx]->getFirmwareVersion();
        deassertAll();
        if (!ver) {
            Serial.printf("[NFC] %s NOT FOUND  CS=GPIO%d\n", label,
                          idx == 0 ? Config.csPin_IN : Config.csPin_OUT);
            return false;
        }
        Serial.printf("[NFC] %s OK  PN5%02x v%d.%d\n", label,
                      (ver>>24)&0xFF, (ver>>16)&0xFF, (ver>>8)&0xFF);
        return true;
    }

public:
    void begin(int csIn, int csOut) {
        // Drive PN532 CS pins HIGH before SPI init
        pinMode(csIn,  OUTPUT); digitalWrite(csIn,  HIGH);
        pinMode(csOut, OUTPUT); digitalWrite(csOut, HIGH);
        delay(20);

        // Dedicated VSPI — SD is on HSPI, no sharing
        spiVSPI.begin(PIN_VSPI_SCK, PIN_VSPI_MISO, PIN_VSPI_MOSI);
        delay(100);
        deassertAll();

        _r[READER_IN]  = new Adafruit_PN532(csIn,  &spiVSPI);
        _r[READER_OUT] = new Adafruit_PN532(csOut, &spiVSPI);

        _ok[READER_IN]  = initOne(READER_IN,  "IN ");
        delay(100);
        _ok[READER_OUT] = initOne(READER_OUT, "OUT");
        deassertAll();

        Serial.printf("[NFC] IN:%s OUT:%s\n",
                      _ok[READER_IN]  ? "OK" : "FAIL",
                      _ok[READER_OUT] ? "OK" : "FAIL");
    }

    bool isOk(ReaderIndex idx) { return _ok[idx]; }

    bool poll(ReaderIndex idx, String& uid) {
        if (!_ok[idx]) return false;
        uint8_t buf[7]; uint8_t len = 0;
        bool found = _r[idx]->readPassiveTargetID(
            PN532_MIFARE_ISO14443A, buf, &len, 50);
        if (!found || len == 0) return false;
        String hex = toHex(buf, len);
        uint32_t now = millis();
        if (hex == _lastUid[idx] && (now - _lastMs[idx]) < DEBOUNCE_MS) return false;
        _lastUid[idx] = hex; _lastMs[idx] = now; uid = hex;
        return true;
    }
} NfcReader;
