/**
 * InOut Firmware v0.3
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
#include "relay.h"
#include "nfc_reader.h"
#include "sd_manager.h"
#include "auth_manager.h"
#include "api_client.h"
#include "lcd_display.h"
#include "web_server.h"
#include "cam_uart.h"

SPIClass spiVSPI(VSPI);
SPIClass spiHSPI(HSPI);

bool _lcdFound = false;

TaskHandle_t hHeartbeat = nullptr;
TaskHandle_t hSync      = nullptr;

// ── Feedback ──────────────────────────────────────────────────────────────────
void feedbackGranted() {
    digitalWrite(DEFAULT_LED1, HIGH);
    tone(DEFAULT_BUZZ, 1000, 100); delay(120);
    tone(DEFAULT_BUZZ, 1500,  80); delay(90);
    digitalWrite(DEFAULT_LED1, LOW);
}
void feedbackDenied() {
    for (int i=0;i<3;i++) {
        digitalWrite(DEFAULT_LED2, HIGH);
        tone(DEFAULT_BUZZ, 300, 80); delay(120);
        digitalWrite(DEFAULT_LED2, LOW); delay(60);
    }
}
void feedbackRegister() {
    for (int i=0;i<2;i++) { tone(DEFAULT_BUZZ, 1200, 60); delay(100); }
    digitalWrite(DEFAULT_LED1, HIGH); delay(200); digitalWrite(DEFAULT_LED1, LOW);
}
void feedbackBoot() {
    for (int f : {700,900,1100,1400}) { tone(DEFAULT_BUZZ,f,55); delay(80); }
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
    digitalWrite(DEFAULT_LED3, HIGH);
    Lcd.showTap(r.granted, r.name);
    if (r.granted) { feedbackGranted(); Relay.open(r.openMs); CamUart.capture(uid); }
    else           { feedbackDenied(); }
    delay(40); digitalWrite(DEFAULT_LED3, LOW);
}

// ── Sync task ─────────────────────────────────────────────────────────────────
void syncTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    // Keep checking NTP until synced — re-applies timezone once confirmed
    for (int i = 0; i < 30 && !ApiClient.isNtpSynced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ApiClient.syncWhitelist();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ApiClient.syncEvents();
        ApiClient.syncEmployees();
        long age = (millis()/1000) - SdManager.whitelistUpdatedAt();
        if (age > 300 || age < 0) ApiClient.syncWhitelist();
        SdManager.trimLog();
    }
}

// ── Heartbeat ─────────────────────────────────────────────────────────────────
void heartbeatTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ApiClient.sendHeartbeat();
        Lcd.setFallback(WiFi.localIP().toString());
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200); delay(300);
    Serial.println("\n[InOut] v0.3 booting");

    for (int p : {DEFAULT_LED1, DEFAULT_LED2, DEFAULT_LED3}) {
        pinMode(p, OUTPUT); digitalWrite(p, LOW);
    }

    // Load config first — needed for CS pin values
    Config.load();

    // ── 1. NFC on VSPI ────────────────────────────────────────────────────────
    // This calls SPI.begin() and drives ALL CS pins HIGH (including SD CS)
    NfcReader.begin(Config.csPin_IN, Config.csPin_OUT);

    // ── 2. SD on HSPI ────────────────────────────────────────────────────────
    delay(500);  // SD power rail stabilize
    SdManager.begin();

    // ── 3. Auth (requires SD) ─────────────────────────────────────────────────
    if (SdManager.isMounted()) {
        AuthManager.begin();
    } else {
        Serial.println("[Auth] SD not mounted — auth unavailable");
    }

    // ── 4. LCD on I2C ─────────────────────────────────────────────────────────
    Lcd.begin();
    _lcdFound = Lcd.isFound();
    Lcd.showBoot("Booting...");

    // SPIFFS not used — all web content served from SD card

    // ── 6. Relay ──────────────────────────────────────────────────────────────
    Relay.begin(Config.relayPin);

    // ── 7. WiFi ───────────────────────────────────────────────────────────────
    Lcd.showBoot("WiFi setup...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setAPCallback([](WiFiManager*) {
        Lcd.showWifi("InOut-Setup");
        digitalWrite(DEFAULT_LED2, HIGH);
    });

    WiFiManagerParameter pServer("server", "Server URL",   Config.serverUrl.c_str(),   128);
    WiFiManagerParameter pToken ("token",  "Device Token", Config.deviceToken.c_str(), 128);
    wm.addParameter(&pServer);
    wm.addParameter(&pToken);

    if (!wm.autoConnect("InOut-Setup")) {
        Lcd.showBoot("WiFi failed!");
        delay(2000); ESP.restart();
    }
    digitalWrite(DEFAULT_LED2, LOW);
    Serial.println("[WiFi] " + WiFi.localIP().toString());
    Lcd.setFallback(WiFi.localIP().toString());

    if (strlen(pServer.getValue()) > 0) {
        Config.serverUrl   = pServer.getValue();
        Config.deviceToken = pToken.getValue();
        Config.save();
    }

    // ── 8. API + Web server ───────────────────────────────────────────────────
    ApiClient.begin();
    // Apply saved timezone
    setenv("TZ", Config.getPosixTz().c_str(), 1);
    tzset();
    Serial.println("[TZ] " + Config.timezone + " → " + Config.getPosixTz());
    WebServerManager.begin();

    // ── 9. ESP32-CAM on UART2 ────────────────────────────────────────────────
    CamUart.begin();

    // ── 10. Background tasks ──────────────────────────────────────────────────
    xTaskCreate(syncTask,      "sync",      8192, nullptr, 1, &hSync);
    xTaskCreate(heartbeatTask, "heartbeat", 4096, nullptr, 1, &hHeartbeat);

    feedbackBoot();
    Lcd.showReady();
    Serial.printf("[InOut] Ready  IN:%s OUT:%s  SD:%s  LCD:%s\n",
                  NfcReader.isOk(READER_IN)  ? "OK" : "FAIL",
                  NfcReader.isOk(READER_OUT) ? "OK" : "FAIL",
                  SdManager.isMounted()       ? "OK" : "NO CARD",
                  _lcdFound                   ? "OK" : "NOT FOUND");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    WebServerManager.loop();
    Relay.loop();
    Lcd.loop();
    String uid;
    if (NfcReader.poll(READER_IN,  uid)) handleTap(uid, DIR_IN);
    if (NfcReader.poll(READER_OUT, uid)) handleTap(uid, DIR_OUT);
    delay(30);
}
