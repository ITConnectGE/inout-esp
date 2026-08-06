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
 *   Capture   : main sends "CAPTURE <uid>\n" → cam replies "READY <len> <checksum>\n"
 *               followed immediately by <len> raw JPEG bytes
 *   On error  : cam replies "ERROR\n"
 *
 *   The cam holds the captured frame in memory instead of releasing it
 *   immediately, so a corrupted transfer (short read or checksum mismatch —
 *   this UART link runs at 921600 baud over plain wires, occasional bit
 *   errors happen) can be retried without re-capturing a new photo:
 *     main → cam : "RETRY\n"  → cam resends the SAME cached frame
 *     main → cam : "ACK\n"    → cam releases the frame, done
 */

#include <Arduino.h>
#include "sd_manager.h"

#define CAM_TX_PIN      32
#define CAM_RX_PIN      34
#define CAM_BAUD        921600
#define CAM_RX_BUF      16384   // must be set before Serial2.begin()
#define CAM_RECV_MS     6000    // max time to receive full JPEG
#define CAM_CHUNK_SIZE  4096    // stream-to-SD chunk size — keeps peak RAM tiny
#define CAM_MAX_ATTEMPTS 3      // total tries (1 initial + up to 2 retries) before giving up

struct CamUartClass {
private:
    bool _ready = false;

    // Member instead of a stack-local array in capture() — keeps the loop
    // task's call frame small instead of eating ~half its 8KB stack per
    // capture. Costs a permanent 4KB of static RAM instead (trivial next to
    // the ~270KB free at boot).
    //
    // Caveat: this makes capture() non-reentrant — a single shared buffer is
    // only safe because exactly one caller (the main loop, one tap at a
    // time) ever calls capture(). If that ever changes (capture moved to
    // its own task, a second camera added, etc.), this buffer would need to
    // go back to being call-local or get its own lock.
    uint8_t _chunk[CAM_CHUNK_SIZE];

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

    // Parses "READY <len> <checksum>". Returns false on a malformed/missing
    // header or an out-of-range length.
    bool parseHeader(const String& header, uint32_t& len, uint32_t& checksum) {
        if (!header.startsWith("READY ")) return false;
        if (sscanf(header.c_str() + 6, "%u %u", &len, &checksum) != 2) return false;
        return len > 0 && len <= 150000;
    }

    // Discard bytes until the line has been quiet for quietMs (bounded by
    // maxMs). Used on failure paths where the CAM may still be mid-write of
    // a response we gave up on (e.g. OOM, short read) — without this, the
    // straggling bytes get misread as the header of the *next* capture.
    void drainStale(uint32_t quietMs = 150, uint32_t maxMs = 2000) {
        uint32_t start = millis(), lastByte = millis();
        while (millis() - start < maxMs) {
            if (Serial2.available()) { Serial2.read(); lastByte = millis(); }
            else if (millis() - lastByte >= quietMs) return;
            else delay(1);
        }
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
    // happenedAt should be the same ISO timestamp logged for the card event so
    // the photo filename matches and can be correlated during batch sync.
    bool capture(const String& uid, const String& happenedAt = "") {
        if (!_ready) return false;

        // Acquired BEFORE the CAM is even told to start — the CAM streams
        // bytes unconditionally the moment it's ready, with no flow control
        // and only a 16KB RX ring buffer (~178ms worth of data) on our end.
        // If this lock were taken after reading the "READY <len>" header
        // (i.e. after the CAM has already started sending), any wait here
        // for syncTask's own SD access to free up would silently drop
        // incoming bytes — that's what was causing "Short read" regardless
        // of chunk size or receive timeout. Held through the whole capture
        // so syncTask's SD access can't interleave with these writes either.
        SdManagerLock _sdLock;
        if (!_sdLock.held) {
            Serial.println("[CAM] SD busy — skipping capture");
            return false;
        }

        while (Serial2.available()) Serial2.read();

        Serial2.print("CAPTURE "); Serial2.println(uid);

        String header = readLine(3000);
        uint32_t len, expectedSum;
        if (!parseHeader(header, len, expectedSum)) {
            Serial.println("[CAM] Capture failed: " + header);
            drainStale();
            return false;
        }

        String path;
        File f = SdManager.openPhotoFile(uid, happenedAt, path);
        if (!f) { drainStale(); return false; }

        for (int attempt = 1; attempt <= CAM_MAX_ATTEMPTS; attempt++) {
            if (attempt > 1) {
                // Ask the CAM to resend the SAME cached frame — no
                // re-capture, so the retried photo still matches this tap.
                Serial2.println("RETRY");
                String retryHeader = readLine(3000);
                if (!parseHeader(retryHeader, len, expectedSum)) {
                    Serial.println("[CAM] Retry header failed: " + retryHeader);
                    drainStale();
                    continue;
                }
            }

            f.seek(0);
            uint32_t got = 0, sum = 0;
            uint32_t start = millis();
            bool writeOk = true;
            while (got < len && (millis() - start) < CAM_RECV_MS) {
                int avail = Serial2.available();
                if (avail > 0) {
                    uint32_t want = min((uint32_t)avail, min((uint32_t)sizeof(_chunk), len - got));
                    int rd = Serial2.readBytes(_chunk, want);
                    if (rd <= 0) continue;
                    if (f.write(_chunk, rd) != (size_t)rd) { writeOk = false; break; }
                    for (int i = 0; i < rd; i++) sum += _chunk[i];
                    got += rd;
                } else {
                    delay(1);
                }
            }

            if (writeOk && got == len && sum == expectedSum) {
                f.close();
                Serial2.println("ACK");
                Logger.logf(LOG_INFO, "SD", "Photo saved: %s  (%u bytes, attempt %d)",
                            path.c_str(), got, attempt);
                return true;
            }

            if (!writeOk)         Serial.printf("[CAM] SD write failed mid-photo (attempt %d)\n", attempt);
            else if (got != len)  Serial.printf("[CAM] Short read: %u/%u bytes (attempt %d)\n", got, len, attempt);
            else                  Serial.printf("[CAM] Checksum mismatch (attempt %d)\n", attempt);
            drainStale();
        }

        SdManager.abortPhoto(f, path);
        return false;
    }
} CamUart;
