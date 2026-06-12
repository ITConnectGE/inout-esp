#pragma once
/**
 * cam_uart.h — ESP32-CAM bridge over UART2
 *
 * Main ESP32        ESP32-CAM
 * TX GPIO32    →    RX GPIO3  (UART0)
 * RX GPIO34    ←    TX GPIO1  (UART0)
 * GND          ─    GND
 *
 * Protocol (921600 baud, 8N1):
 *   Handshake : main sends "PING\n"        → cam replies "CAM_READY\n"
 *   Capture   : main sends "CAPTURE <uid>\n" → cam replies "READY <len>\n"
 *               followed immediately by <len> raw JPEG bytes
 *   On error  : cam replies "ERROR\n"
 */

#include <Arduino.h>
#include "sd_manager.h"

#define CAM_TX_PIN      32
#define CAM_RX_PIN      34
#define CAM_BAUD        921600
#define CAM_RX_BUF      16384   // must be set before Serial2.begin()
#define CAM_RECV_MS     6000    // max time to receive full JPEG

struct CamUartClass {
private:
    bool _ready = false;

    // Read one line (up to \n), strip CR/LF, with timeout_ms
    String readLine(uint32_t timeout_ms) {
        String line;
        uint32_t start = millis();
        while (millis() - start < timeout_ms) {
            if (!Serial2.available()) { delay(1); continue; }
            char c = Serial2.read();
            if (c == '\n') { line.trim(); return line; }
            if (c != '\r') line += c;
        }
        return "";
    }

public:
    void begin() {
        Serial2.setRxBufferSize(CAM_RX_BUF);
        Serial2.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);
        delay(100);

        Serial.println("[CAM] Pinging ESP32-CAM...");
        for (int attempt = 0; attempt < 5; attempt++) {
            while (Serial2.available()) Serial2.read();   // flush stale bytes
            Serial2.println("PING");
            String resp = readLine(1500);
            if (resp == "CAM_READY") {
                _ready = true;
                Serial.println("[CAM] Online");
                return;
            }
        }
        Serial.println("[CAM] Not found — camera capture disabled");
    }

    bool isReady() { return _ready; }

    // Trigger capture; saves JPEG to SD as /photos/<uid>_<timestamp>.jpg
    // Returns true on success. Called from handleTap() after relay opens.
    bool capture(const String& uid) {
        if (!_ready) return false;

        while (Serial2.available()) Serial2.read();

        Serial2.print("CAPTURE "); Serial2.println(uid);

        String header = readLine(3000);
        if (!header.startsWith("READY ")) {
            Serial.println("[CAM] Capture failed: " + header);
            return false;
        }

        uint32_t len = (uint32_t)header.substring(6).toInt();
        if (len == 0 || len > 150000) {
            Serial.printf("[CAM] Bogus length %u\n", len);
            return false;
        }

        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) { Serial.println("[CAM] OOM"); return false; }

        uint32_t got = 0;
        uint32_t start = millis();
        while (got < len && (millis() - start) < CAM_RECV_MS) {
            int avail = Serial2.available();
            if (avail > 0) {
                uint32_t chunk = min((uint32_t)avail, len - got);
                Serial2.readBytes(buf + got, chunk);
                got += chunk;
            } else {
                delay(1);
            }
        }

        if (got != len) {
            Serial.printf("[CAM] Short read: %u/%u bytes\n", got, len);
            free(buf);
            return false;
        }

        bool ok = SdManager.savePhoto(uid, buf, len);
        free(buf);
        return ok;
    }
} CamUart;
