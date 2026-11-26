/**
 * InOut Firmware v0.6.0
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
#include <Update.h>
#include <mbedtls/sha256.h>
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
volatile bool _systemError      = false;  // SD missing or both NFC readers dead → red solid
volatile bool _serverWarning    = false;  // last sync failed → yellow blink
unsigned long _warnLast         = 0;      // warning blink state
bool          _warnOn           = false;

TaskHandle_t hSync = nullptr;

void buzz(uint32_t freq, uint32_t durationMs) {
    if (!_buzzerMuted) tone(DEFAULT_BUZZ, freq, durationMs);
}

// ── Feedback ──────────────────────────────────────────────────────────────────
// Buzzer only — GPIO14/13/12 are now the READER/SD/CAM status LEDs (see
// handleTap() for the READER_LED bracket that replaced the old per-outcome
// LED flashes here).
void feedbackGranted() {
    buzz(1000, 100); delay(120);
    buzz(1500,  80); delay(90);
}
void feedbackDenied() {
    for (int i=0;i<3;i++) {
        buzz(300, 80); delay(120);
        delay(60);
    }
}
void feedbackRegister() {
    for (int i=0;i<2;i++) { buzz(1200, 60); delay(100); }
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
    String dirStr = (Config.directionMode == "toggle") ? "" : (dir == DIR_IN ? "in" : "out");
    Lcd.showTap(r.granted, r.name, dirStr);
    if (r.granted) {
        feedbackGranted();
        Relay.open(r.openMs);
        // 2 green blinks = tap granted; 3rd blink if photo was also saved
        for (int i = 0; i < 2; i++) {
            digitalWrite(PIN_READER_LED, HIGH); delay(150);
            digitalWrite(PIN_READER_LED, LOW);  delay(100);
        }
        bool photoOk = CamUart.capture(uid, r.happenedAt);
        if (photoOk) {
            delay(80);
            digitalWrite(PIN_READER_LED, HIGH); delay(150);
            digitalWrite(PIN_READER_LED, LOW);
        }
    } else {
        feedbackDenied();
        // Brief red flash on denied — skip if the error LED is already solid
        if (!_systemError) {
            digitalWrite(PIN_SD_LED, HIGH); delay(200);
            digitalWrite(PIN_SD_LED, LOW);
        }
    }
    delay(40);
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
// Called from syncTask when the heartbeat carries ota_url + ota_version, and
// from the serial `update` command. Sets _otaInProgress so loop() freezes.
//
// Downloads and flashes by hand instead of httpUpdate so the image can be
// checked before it becomes bootable (§10.5):
//   * ota_size   — Content-Length must match, and so must the bytes written;
//   * ota_sha256 — sha256 over every byte written, compared before
//                  Update.end(). On mismatch Update.abort() discards the
//                  new partition and the device keeps running the old one.
// Both are optional: a server that sends neither still gets an update (the
// ESP32 image header check in Update still applies), just unverified.
// A version that failed is not retried for an hour, so a bad URL or digest
// doesn't turn into a download every 30 s.
static String        _otaFailedVer;
static unsigned long _otaFailedAt = 0;

static void otaFail(const String& version, const char* why, const String& detail = "") {
    Logger.log(LOG_ERROR, "OTA", String("Update failed — ") + why, detail);
    Update.abort();
    _otaFailedVer = version;
    _otaFailedAt  = millis();
    Lcd.showOtaError();
    delay(3000);
    _otaInProgress = false;
}

void performOta(const String& url, const String& version, const String& sha256, size_t size) {
    if (version == FIRMWARE_VERSION) {
        Logger.logf(LOG_INFO, "OTA", "Already on v%s — skipping", version.c_str());
        return;
    }
    if (version == _otaFailedVer && millis() - _otaFailedAt < 3600000UL) return;
    if (sha256.length() && sha256.length() != 64) {
        Logger.log(LOG_ERROR, "OTA", "ota_sha256 is not a sha256 hex digest — refusing", sha256);
        return;
    }

    _otaInProgress = true;
    Logger.logf(LOG_INFO, "OTA", "Starting update v%s (%s, %u bytes expected)", version.c_str(),
                sha256.length() ? "sha256 verified" : "UNVERIFIED — no ota_sha256", (unsigned)size);
    Lcd.showOta(version);
    ApiClient.endCycle();  // free the sync TLS session — this one needs the heap

    WiFiClientSecure client;
    ApiClient.configureClient(client);
    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub release URLs redirect
    http.setTimeout(20000);
    int code = http.GET();
    if (code != 200) {
        String why = code > 0 ? ("HTTP " + String(code)) : HTTPClient::errorToString(code);
        http.end();
        { otaFail(version, "download", why); return; }
    }
    int len = http.getSize();
    if (size > 0 && len > 0 && (size_t)len != size) {
        http.end();
        { otaFail(version, "size mismatch",
                       "server says " + String((unsigned)size) + ", download is " + String(len)); return; }
    }
    size_t total = len > 0 ? (size_t)len : size;
    if (total == 0) {
        http.end();
        { otaFail(version, "unknown image size (no Content-Length, no ota_size)"); return; }
    }
    if (!Update.begin(total, U_FLASH)) {
        http.end();
        { otaFail(version, "Update.begin", Update.errorString()); return; }
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t written = 0;
    unsigned long lastData = millis();
    int lastPct = -1;
    while (written < total) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected() || millis() - lastData > 20000) break;
            delay(5);
            continue;
        }
        int rd = stream->readBytes(buf, min(avail, min(sizeof(buf), total - written)));
        if (rd <= 0) continue;
        lastData = millis();
        if (Update.write(buf, rd) != (size_t)rd) {
            mbedtls_sha256_free(&ctx);
            http.end();
            { otaFail(version, "flash write", Update.errorString()); return; }
        }
        mbedtls_sha256_update(&ctx, buf, rd);
        written += rd;
        int pct = (int)((written * 100) / total);
        if (pct != lastPct) { Lcd.showOtaProgress(pct); lastPct = pct; }
    }
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", digest[i]);

    if (written != total)
        { otaFail(version, "download incomplete",
                       String((unsigned)written) + " of " + String((unsigned)total) + " bytes"); return; }
    if (sha256.length() && !sha256.equalsIgnoreCase(hex))
        { otaFail(version, "sha256 mismatch — image discarded",
                       String("expected ") + sha256 + " got " + hex); return; }
    if (!Update.end(true))
        { otaFail(version, "Update.end", Update.errorString()); return; }

    Logger.logf(LOG_INFO, "OTA", "v%s written and verified (sha256 %s) — rebooting", version.c_str(), hex);
    Config.markOtaReboot();
    delay(500);
    ESP.restart();
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
    ApiClient.lockCycle(portMAX_DELAY);
    if (!ApiClient.syncWhitelist()) {
        Logger.log(LOG_WARN, "SYNC", "Server unreachable — working locally");
        Lcd.showNotice(" Server offline", " Working locally", 5000);
    }
    ApiClient.endCycle();
    ApiClient.unlockCycle();
    unsigned long lastWhitelistSync = millis();
    // Adaptive heap guard: starts at 55 KB, lowers by 2 KB every 5 consecutive
    // cycles where the observed floor stays >10 KB above the guard, down to
    // 45 KB minimum. Resets the counter any time the floor gets close.
    static uint32_t heapGuard   = 55000;
    static uint32_t heapFloor   = UINT32_MAX;
    static int      stableCount = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ApiClient.lockCycle(portMAX_DELAY);

        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        heapFloor = min(heapFloor, maxAlloc);

        // Drain all pending events in batches. Track whether any attempt was
        // rejected by the server (syncEvents < 0) so the warning LED can
        // distinguish a real failure from a heap-guard pause.
        bool syncFailed = false;
        for (int i = 0; i < 100 && SdManager.unsyncedCount() > 0; i++) {
            if (ESP.getMaxAllocHeap() < heapGuard) {
                Logger.logf(LOG_WARN, "SYNC", "Low heap (%u B) — deferring remaining events", ESP.getMaxAllocHeap());
                break;
            }
            int r = ApiClient.syncEvents();
            if (r < 0) { syncFailed = true; break; }
            if (r == 0) break;
        }
        _serverWarning = syncFailed;

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
        SdManager.trimLog();
        Logger.trimLog();
        if (ESP.getMaxAllocHeap() >= 65000) ApiClient.sendHeartbeat();
        // Whitelist: fetched when the heartbeat's whitelist_version differs
        // from ours (or the server asked for a resync). The 15-minute
        // fallback covers heartbeats skipped for low heap; with ?since= an
        // up-to-date device gets an empty delta, so it costs almost nothing.
        if (ApiClient.whitelistStale || millis() - lastWhitelistSync > 15UL * 60UL * 1000UL) {
            if (ApiClient.syncWhitelist()) lastWhitelistSync = millis();
        }
        ApiClient.endCycle();
        ApiClient.unlockCycle();
        if (ApiClient.pendingOtaUrl.length() > 0) {
            String url = ApiClient.pendingOtaUrl;
            String ver = ApiClient.pendingOtaVersion;
            String sha = ApiClient.pendingOtaSha256;
            size_t sz  = ApiClient.pendingOtaSize;
            ApiClient.pendingOtaUrl     = "";
            ApiClient.pendingOtaVersion = "";
            ApiClient.pendingOtaSha256  = "";
            ApiClient.pendingOtaSize    = 0;
            performOta(url, ver, sha, sz);
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

    for (int p : {PIN_SERVER_LED, PIN_READER_LED, PIN_SD_LED, PIN_CAM_LED}) {
        pinMode(p, OUTPUT); digitalWrite(p, LOW);
    }
    digitalWrite(PIN_CAM_LED, HIGH);  // power LED — always on
    // External pull-ups on VN/VP — plain INPUT (these pins have no internal pulls).
    pinMode(PIN_BTN_RESET, INPUT);
    pinMode(PIN_BTN_FORGET, INPUT);

    // Load config first — needed for CS pin values
    Config.load();
    Config.bumpBootCount();  // event_id prefix — must precede the first tap

    // ── 1. NFC on VSPI ────────────────────────────────────────────────────────
    // This calls SPI.begin() and drives ALL CS pins HIGH (including SD CS)
    NfcReader.begin(Config.csPin_IN, Config.csPin_OUT);

    // ── 2. SD on HSPI ────────────────────────────────────────────────────────
    delay(500);  // SD power rail stabilize
    bool sdOk = SdManager.begin();
    if (sdOk) {
        Config.whitelistVersion = SdManager.readWhitelistVersion();
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
    // The admin panel is NOT started at boot — it is the single largest
    // consumer of heap on this device (route handlers, listening socket, and
    // several KB of lwIP buffers per connected browser, on top of multi-KB
    // response strings while a page is open). Card reads, SD logging and the
    // server sync all work without it. Start it from the serial console with
    // `locale` when you need it, and `locale off` to give the heap back.
    Logger.log(LOG_INFO, "WEB", "Admin panel off (default) — type 'locale' on serial to start it");

    // ── 9. ESP32-CAM on UART2 ────────────────────────────────────────────────
    CamUart.begin();
    ApiClient.camOkFn = []() { return CamUart.isReady(); };
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
    // Red solid if SD is missing or both readers are dead — device can't function
    _systemError = !sdOk || (!inOk && !outOk);
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

    // ── Status LEDs ───────────────────────────────────────────────────────────
    // Yellow warning (GPIO 27): blinks (300ms on / 600ms off) when the last
    // sync attempt failed. Clears as soon as a sync cycle succeeds.
    unsigned long now = millis();
    if (_serverWarning) {
        if (!_warnOn && now - _warnLast >= 600) {
            digitalWrite(PIN_SERVER_LED, HIGH); _warnOn = true; _warnLast = now;
        } else if (_warnOn && now - _warnLast >= 300) {
            digitalWrite(PIN_SERVER_LED, LOW);  _warnOn = false; _warnLast = now;
        }
    } else {
        digitalWrite(PIN_SERVER_LED, LOW);
        _warnOn   = false;
        _warnLast = now;
    }

    // Red error (GPIO 13): solid on hardware failure.
    // Denied-tap flashes are handled in handleTap() and don't need driving here.
    if (_systemError) digitalWrite(PIN_SD_LED, HIGH);

    delay(30);
}
