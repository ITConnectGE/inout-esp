/**
 * InOut Firmware v0.4.3
 * ESP32-WROOM32 · 2x PN532 · SD card · Relay · Buzzer · LEDs · LCD 16x2
 *
 * Single VSPI bus (SCK=18 MISO=19 MOSI=23) shared by PN532 readers + SD card.
 * The 500ms delay between NfcReader.begin() and SdManager.begin() is critical —
 * it lets the SD card's power rail stabilize before the bus is used.
 *
 * I2C: LCD SDA=21 SCL=22
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PN532.h>
#include <vector>
#include <map>

#include "config.h"
#include "logger.h"
#include "relay.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "auth_manager.h"
#include "api_client.h"
#include "lcd_display.h"
#include "web_server.h"
#include "cam_uart.h"
#include "serial_cmd.h"

SPIClass spiVSPI(VSPI);
SPIClass spiHSPI(HSPI);

bool          _lcdFound         = false;
bool          _buzzerMuted      = false;
volatile bool _otaInProgress    = false;
volatile bool _camOtaInProgress = false;

TaskHandle_t hSync = nullptr;

void buzz(uint32_t freq, uint32_t durationMs) {
    if (!_buzzerMuted) tone(DEFAULT_BUZZ, freq, durationMs);
}

// ── Feedback ──────────────────────────────────────────────────────────────────
void feedbackGranted() {
    digitalWrite(DEFAULT_LED1, HIGH);
    buzz(1000, 100); delay(120);
    buzz(1500,  80); delay(90);
    digitalWrite(DEFAULT_LED1, LOW);
}
void feedbackDenied() {
    for (int i=0;i<3;i++) {
        digitalWrite(DEFAULT_LED2, HIGH);
        buzz(300, 80); delay(120);
        digitalWrite(DEFAULT_LED2, LOW); delay(60);
    }
}
void feedbackRegister() {
    for (int i=0;i<2;i++) { buzz(1200, 60); delay(100); }
    digitalWrite(DEFAULT_LED1, HIGH); delay(200); digitalWrite(DEFAULT_LED1, LOW);
}
void feedbackBoot() {
    for (int f : {700,900,1100,1400}) { buzz(f, 55); delay(80); }
}

// ── Card tap ──────────────────────────────────────────────────────────────────
void handleTap(const String& uid, CardDirection dir) {
    if (hasPendingTap()) {
        onRegistrationTap(uid);
        feedbackRegister();
        Lcd.showTap(true, "Card registered!");
        return;
    }
    ApiResponse r = ApiClient.processCard(uid, dir);
    // For toggle mode the device can't determine direction locally — leave blank on LCD.
    String dirStr = (Config.directionMode == "toggle") ? "" : (dir == DIR_IN ? "in" : "out");
    digitalWrite(DEFAULT_LED3, HIGH);
    Lcd.showTap(r.granted, r.name, dirStr);
    if (r.granted) { feedbackGranted(); Relay.open(r.openMs); CamUart.capture(uid, r.happenedAt); }
    else           { feedbackDenied(); }
    delay(40); digitalWrite(DEFAULT_LED3, LOW);
}

// ── Buttons ───────────────────────────────────────────────────────────────────
// Reset (VN): tap = normal restart, held 5s+ = factory reset.
// Forget (VP): held 3s+ = erase WiFi credentials + restart.
// Both hold-triggers fire once their threshold is crossed, no need to wait
// for release. Both delegate to WebServerManager.performReset() so button,
// web, and serial resets can't drift — see web_server.h.
void handleButtons() {
    static bool resetHeld = false, resetFactoryFired = false;
    static unsigned long resetPressedAt = 0;
    static bool forgetHeld = false, forgetFired = false;
    static unsigned long forgetPressedAt = 0;

    bool resetDown = digitalRead(PIN_BTN_RESET) == LOW;
    if (resetDown && !resetHeld) {
        delay(BTN_DEBOUNCE_MS);
        if (digitalRead(PIN_BTN_RESET) == LOW) {
            resetHeld = true;
            resetFactoryFired = false;
            resetPressedAt = millis();
        }
    } else if (resetDown && resetHeld && !resetFactoryFired) {
        if (millis() - resetPressedAt >= RESET_FACTORY_HOLD_MS) {
            resetFactoryFired = true;
            Logger.log(LOG_WARN, "SYS", "Reset button held 5s+ — factory reset");
            Lcd.showNotice(" Factory reset", " ...", 1500);
            for (int f : {2000, 1500, 1000}) { buzz(f, 120); delay(160); }
            WebServerManager.performReset(true);  // restarts device, does not return
        }
    } else if (!resetDown && resetHeld) {
        resetHeld = false;
        if (!resetFactoryFired) {
            Logger.log(LOG_INFO, "SYS", "Reset button pressed — restarting");
            delay(100);
            ESP.restart();
        }
    }

    bool forgetDown = digitalRead(PIN_BTN_FORGET) == LOW;
    if (forgetDown && !forgetHeld) {
        delay(BTN_DEBOUNCE_MS);
        if (digitalRead(PIN_BTN_FORGET) == LOW) {
            forgetHeld = true;
            forgetFired = false;
            forgetPressedAt = millis();
        }
    } else if (forgetDown && forgetHeld && !forgetFired) {
        if (millis() - forgetPressedAt >= FORGET_HOLD_MS) {
            forgetFired = true;
            Logger.log(LOG_WARN, "SYS", "Forget-network button held 3s+ — forgetting WiFi");
            Lcd.showNotice(" Forgetting", " WiFi...", 1500);
            buzz(1500, 150); delay(200);
            WebServerManager.performReset(false);  // restarts device, does not return
        }
    } else if (!forgetDown && forgetHeld) {
        forgetHeld = false;
    }
}

// ── OTA update ───────────────────────────────────────────────────────────────
// Called from syncTask when sendHeartbeat() receives ota_url + ota_version.
// Sets _otaInProgress so loop() freezes (no NFC reads, no relay, no web handler).
// GitHub release URLs redirect; HTTPC_STRICT_FOLLOW_REDIRECTS handles that.
// On success httpUpdate reboots into the new partition automatically.
// On failure we reboot into the old partition (dual-OTA rollback is automatic).
void performOta(const String& url, const String& version) {
    if (version == FIRMWARE_VERSION) {
        Logger.logf(LOG_INFO, "OTA", "Already on v%s — skipping", version.c_str());
        return;
    }
    _otaInProgress = true;
    Logger.logf(LOG_INFO, "OTA", "Starting update v%s", version.c_str());
    Lcd.showOta(version);

    WiFiClientSecure client;
    ApiClient.configureClient(client);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress([](int cur, int total) {
        if (total > 0) Lcd.showOtaProgress((cur * 100) / total);
    });

    t_httpUpdate_return ret = httpUpdate.update(client, url);

    // HTTP_UPDATE_OK never reaches here — device reboots automatically.
    if (ret == HTTP_UPDATE_FAILED) {
        Logger.logf(LOG_ERROR, "OTA", "Failed (%d): %s",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
        Lcd.showOtaError();
        delay(4000);
        ESP.restart();  // boots old partition
    }
    // HTTP_UPDATE_NO_UPDATES: server said nothing to install — resume normal operation.
    Logger.log(LOG_WARN, "OTA", "Server reported no update available");
    _otaInProgress = false;
}

// ── Camera OTA ────────────────────────────────────────────────────────────────
// Called from syncTask or serialTask. Freezes NFC scans (loop skips polls)
// but sync continues — the 30s cycle in syncTask is blocked only for the
// duration of the camera download + reboot wait (≤120 s).
void performCamOta(const String& url, const String& version) {
    if (version.length() > 0 && version == CamUart.getVersion()) {
        Logger.logf(LOG_INFO, "OTA", "Camera already on v%s — skipping", version.c_str());
        return;
    }
    if (!CamUart.isReady()) {
        Logger.log(LOG_WARN, "OTA", "Camera not online — aborting camera OTA");
        return;
    }
    _camOtaInProgress = true;
    String prevVer = CamUart.getVersion();
    Logger.logf(LOG_INFO, "OTA", "Camera OTA → v%s", version.c_str());
    Lcd.showNotice(" Camera update", " In progress...", 125000);

    bool cameBack = CamUart.requestOta(url);

    if (cameBack) {
        String newVer = CamUart.getVersion();
        ApiClient.camFirmwareVersion = newVer;
        Serial.printf("[CAM] Firmware version: %s\n", newVer.c_str());
        if (version.length() > 0 ? newVer == version : newVer != prevVer) {
            Logger.logf(LOG_INFO, "OTA", "Camera updated → v%s", newVer.c_str());
            Lcd.showNotice(" Camera updated", " v" + newVer, 4000);
        } else {
            Logger.logf(LOG_ERROR, "OTA", "Camera OTA failed (still v%s)", newVer.c_str());
            Lcd.showNotice(" Cam update", " failed!", 4000);
        }
    } else {
        Logger.log(LOG_ERROR, "OTA", "Camera did not come back after OTA");
        Lcd.showNotice(" Cam update", " timed out!", 4000);
    }
    _camOtaInProgress = false;
}

// ── Serial command task ───────────────────────────────────────────────────────
// Runs on its own core so blocking operations in the main loop (HTTP calls,
// feedback delays, NFC polling) don't starve serial input.
void serialTask(void*) {
    for (;;) {
        SerialCmd.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Sync + heartbeat task ─────────────────────────────────────────────────────
// Each tap is decided locally and written to SD (processCard). Photos are saved
// to SD by CamUart. This task is the only path that sends data to the server:
//   events  → POST /device/events/batch  (multipart, photos[N] attached)
//   employees → POST /device/employees/batch
//   whitelist ← GET /device/sync
// No direct POST /device/events calls anywhere — one tap = one batch entry.
void syncTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    for (int i = 0; i < 30 && !ApiClient.isNtpSynced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!ApiClient.syncWhitelist()) {
        Logger.log(LOG_WARN, "SYNC", "Server unreachable — working locally");
        Lcd.showNotice(" Server offline", " Working locally", 5000);
    }
    // Adaptive heap guard: starts at 55 KB, lowers by 2 KB every 5 consecutive
    // cycles where the observed floor stays >10 KB above the guard, down to
    // 45 KB minimum. Resets the counter any time the floor gets close.
    static uint32_t heapGuard   = 55000;
    static uint32_t heapFloor   = UINT32_MAX;
    static int      stableCount = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        heapFloor = min(heapFloor, maxAlloc);

        // Drain all pending events in batches (each with its photo if one
        // exists). Loop until the queue is empty — clears a backlog in one
        // pass instead of many 30s rounds. Cap at 100 to guard against a
        // corrupt log; markAllSynced() drops bad-timestamp events.
        for (int i = 0; i < 100 && SdManager.unsyncedCount() > 0; i++) {
            if (ESP.getMaxAllocHeap() < heapGuard) {
                Logger.logf(LOG_WARN, "SYNC", "Low heap (%u B) — deferring remaining events", ESP.getMaxAllocHeap());
                break;
            }
            if (ApiClient.syncEvents() <= 0) break;
        }

        // Once the queue is fully drained, reset the guard back to 55 KB so
        // the next backlog starts fresh and adapts again from a safe baseline.
        if (SdManager.unsyncedCount() == 0 && heapGuard < 55000) {
            heapGuard   = 55000;
            heapFloor   = UINT32_MAX;
            stableCount = 0;
            Logger.logf(LOG_INFO, "HEAP", "Queue clear — guard reset to %u B", heapGuard);
        } else if (heapFloor > heapGuard + 10000 && heapGuard > 45000) {
            // Lower by 2 KB after 5 consecutive stable cycles.
            if (++stableCount >= 5) {
                heapGuard   = max(45000u, heapGuard - 2000u);
                heapFloor   = UINT32_MAX;
                stableCount = 0;
                Logger.logf(LOG_INFO, "HEAP", "Guard → %u B", heapGuard);
            }
        } else {
            stableCount = 0;
        }
        ApiClient.syncEmployees();
        long age = (millis()/1000) - SdManager.whitelistUpdatedAt();
        if (age > 300 || age < 0) ApiClient.syncWhitelist();
        SdManager.trimLog();
        Logger.trimLog();
        if (ESP.getMaxAllocHeap() >= 65000) ApiClient.sendHeartbeat();
        if (ApiClient.pendingOtaUrl.length() > 0) {
            String url = ApiClient.pendingOtaUrl;
            String ver = ApiClient.pendingOtaVersion;
            ApiClient.pendingOtaUrl     = "";
            ApiClient.pendingOtaVersion = "";
            performOta(url, ver);
        }
        if (ApiClient.pendingCamOtaUrl.length() > 0) {
            String url = ApiClient.pendingCamOtaUrl;
            String ver = ApiClient.pendingCamOtaVersion;
            ApiClient.pendingCamOtaUrl     = "";
            ApiClient.pendingCamOtaVersion = "";
            performCamOta(url, ver);
        }
        Lcd.setFallback(WiFi.localIP().toString());
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200); delay(300);
    Serial.println("\n[INFO][SYS] InOut v" FIRMWARE_VERSION " booting");

    Logger.begin();

    for (int p : {DEFAULT_LED1, DEFAULT_LED2, DEFAULT_LED3}) {
        pinMode(p, OUTPUT); digitalWrite(p, LOW);
    }
    // External pull-ups on VN/VP — plain INPUT (these pins have no internal pulls).
    pinMode(PIN_BTN_RESET, INPUT);
    pinMode(PIN_BTN_FORGET, INPUT);

    // Load config first — needed for CS pin values
    Config.load();

    // ── 1. NFC on VSPI ────────────────────────────────────────────────────────
    // This calls SPI.begin() and drives ALL CS pins HIGH (including SD CS)
    NfcReader.begin(Config.csPin_IN, Config.csPin_OUT);

    // ── 2. SD on HSPI ────────────────────────────────────────────────────────
    delay(500);  // SD power rail stabilize
    bool sdOk = SdManager.begin();
    if (sdOk) {
        Logger.setSDReady(true);
        Logger.log(LOG_INFO, "SYS", "InOut v" FIRMWARE_VERSION " boot — SD ready");
    }

    // ── 3. Auth (requires SD) ─────────────────────────────────────────────────
    if (SdManager.isMounted()) {
        AuthManager.begin();
    } else {
        Logger.log(LOG_ERROR, "AUTH", "SD not mounted — auth and persistent logging unavailable");
    }

    // ── 4. LCD on I2C ─────────────────────────────────────────────────────────
    Lcd.begin();
    _lcdFound = Lcd.isFound();
    Lcd.showBoot("Booting...");

    // SPIFFS not used — all web content served from SD card

    // ── 6. Relay ──────────────────────────────────────────────────────────────
    Relay.begin(Config.relayPin);

    // Serial task starts here so commands remain responsive during the
    // blocking WiFiManager autoConnect that follows.
    xTaskCreate(serialTask, "serial", 8192, nullptr, 2, nullptr);

    // ── 7. WiFi ───────────────────────────────────────────────────────────────
    Lcd.showBoot("WiFi setup...");

    // Try credentials saved via `set wifi` before handing off to WiFiManager.
    // If already connected, autoConnect() returns immediately without a portal.
    Config.loadWifiCreds();
    if (Config.wifiSsid.length() > 0) {
        Serial.printf("[WIFI] Trying stored credentials (SSID: %s)...\n", Config.wifiSsid.c_str());
        Lcd.showBoot("Connecting...");
        WiFi.begin(Config.wifiSsid.c_str(), Config.wifiPass.c_str());
        for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
        if (WiFi.status() == WL_CONNECTED)
            Serial.println("[WIFI] Connected via stored credentials");
        else
            Serial.println("[WIFI] Stored credentials failed — falling back to portal");
    }

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setAPCallback([](WiFiManager*) {
        Lcd.showWifi("InOut-Setup");
        digitalWrite(DEFAULT_LED2, HIGH);
    });

    WiFiManagerParameter pServer("server", "Server URL",   Config.serverUrl.c_str(), 128);
    WiFiManagerParameter pToken ("token",  "Device Token", "",                        128);
    wm.addParameter(&pServer);
    wm.addParameter(&pToken);

    if (!wm.autoConnect("InOut-Setup")) {
        Logger.log(LOG_ERROR, "WIFI", "autoConnect timed out — restarting");
        Lcd.showBoot("WiFi failed!");
        delay(2000); ESP.restart();
    }
    digitalWrite(DEFAULT_LED2, LOW);
    Logger.log(LOG_INFO, "WIFI", "Connected", WiFi.localIP().toString() + "  RSSI=" + String(WiFi.RSSI()) + "dBm");
    Lcd.setFallback(WiFi.localIP().toString());

    if (strlen(pServer.getValue()) > 0) {
        Config.serverUrl = pServer.getValue();
        if (strlen(pToken.getValue()) > 0)
            Config.deviceToken = pToken.getValue();
        Config.save();
    }

    // ── 8. API + Web server ───────────────────────────────────────────────────
    ApiClient.begin();
    // Apply saved timezone, then restore clock from SD — must come after
    // configTime() inside begin() so SNTP init doesn't wipe the saved epoch.
    setenv("TZ", Config.getPosixTz().c_str(), 1);
    tzset();
    ApiClient.restoreTimeFromSD();
    Logger.log(LOG_INFO, "SYS", "TZ: " + Config.timezone + " → " + Config.getPosixTz());
    WebServerManager.begin();

    // ── 9. ESP32-CAM on UART2 ────────────────────────────────────────────────
    CamUart.begin();
    if (CamUart.isReady()) {
        // WiFi.psk() returns the password of the currently connected network
        // regardless of whether it was set via portal or 'set wifi' command.
        CamUart.sendWifiCreds(WiFi.SSID(), WiFi.psk());
        CamUart.requestVersion();
        ApiClient.camFirmwareVersion = CamUart.getVersion();
        Serial.printf("[CAM] Firmware version: %s\n", ApiClient.camFirmwareVersion.c_str());
    }

    // ── 10. Background tasks ──────────────────────────────────────────────────
    xTaskCreate(syncTask, "sync", 8192, nullptr, 1, &hSync);

    feedbackBoot();
    Lcd.showReady();
    bool inOk  = NfcReader.isOk(READER_IN);
    bool outOk = NfcReader.isOk(READER_OUT);
    if (!inOk)  Logger.log(LOG_ERROR, "NFC", "IN reader not found", "CS=GPIO" + String(Config.csPin_IN));
    if (!outOk) Logger.log(LOG_ERROR, "NFC", "OUT reader not found", "CS=GPIO" + String(Config.csPin_OUT));
    Logger.logf(LOG_INFO, "SYS", "Ready  IN:%s OUT:%s  SD:%s  LCD:%s",
                inOk                    ? "OK" : "FAIL",
                outOk                   ? "OK" : "FAIL",
                SdManager.isMounted()   ? "OK" : "NO CARD",
                _lcdFound               ? "OK" : "NOT FOUND");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    if (_otaInProgress) { delay(50); return; }
    WebServerManager.loop();
    Relay.loop();
    Lcd.loop();
    handleButtons();
    if (!_camOtaInProgress) {
        String uid;
        if (NfcReader.poll(READER_IN,  uid)) handleTap(uid, DIR_IN);
        if (NfcReader.poll(READER_OUT, uid)) handleTap(uid, DIR_OUT);
    }
    delay(30);
}
