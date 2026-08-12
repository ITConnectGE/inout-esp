/**
 * ESP32-CAM firmware for InOut checkpoint system
 *
 * Receives commands from the main ESP32 over UART0 (TX=GPIO1, RX=GPIO3)
 * at 921600 baud and sends back a raw JPEG on each CAPTURE request.
 *
 * Protocol:
 *   Main → CAM : "PING\n"                 → "CAM_READY\n"
 *   Main → CAM : "CAPTURE <uid>\n"        → "READY <len> <checksum>\n" + <len> bytes
 *                                          → "ERROR\n" on failure
 *   Main → CAM : "ACK\n"                  → frame released, ready for next CAPTURE
 *   Main → CAM : "RETRY\n"                → resends the same cached frame
 *   Main → CAM : "SET_WIFI_SSID <ssid>\n" → saves SSID to NVS, replies "OK\n"
 *   Main → CAM : "SET_WIFI_PASS <pass>\n" → saves password to NVS, replies "OK\n"
 *   Main → CAM : "CAM_VERSION\n"          → replies "VERSION <version>\n"
 *   Main → CAM : "CAM_OTA <url>\n"        → replies "OTA_START\n", connects WiFi,
 *                                            downloads firmware, reboots on success;
 *                                            replies "OTA_FAILED\n" and resumes on error
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include "esp_camera.h"

#define CAM_FIRMWARE_VERSION "1.0.0"

// ── AI-Thinker ESP32-CAM pinout ───────────────────────────────────────────────
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

static bool _camOk = false;
static camera_fb_t* _pendingFb = nullptr;
static uint32_t _pendingChecksum = 0;
static String _wifiSsid;
static String _wifiPass;

static uint32_t checksum32(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void sendFrame(camera_fb_t* fb, uint32_t sum) {
    Serial.printf("READY %u %u\n", (unsigned)fb->len, (unsigned)sum);
    Serial.write(fb->buf, fb->len);
    Serial.flush();
}

static bool initCamera() {
    camera_config_t cfg;
    cfg.pin_pwdn      = PWDN_GPIO_NUM;
    cfg.pin_reset     = RESET_GPIO_NUM;
    cfg.pin_xclk      = XCLK_GPIO_NUM;
    cfg.pin_sccb_sda  = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl  = SIOC_GPIO_NUM;
    cfg.pin_d7 = Y9_GPIO_NUM;  cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM;  cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM;  cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d1 = Y3_GPIO_NUM;  cfg.pin_d0 = Y2_GPIO_NUM;
    cfg.pin_vsync     = VSYNC_GPIO_NUM;
    cfg.pin_href      = HREF_GPIO_NUM;
    cfg.pin_pclk      = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz  = 20000000;
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.ledc_channel  = LEDC_CHANNEL_0;
    cfg.pixel_format  = PIXFORMAT_JPEG;
    cfg.frame_size    = FRAMESIZE_HD;
    cfg.jpeg_quality  = 12;
    cfg.fb_count      = 1;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
    return esp_camera_init(&cfg) == ESP_OK;
}

static void loadWifiCreds() {
    Preferences p; p.begin("wf", true);
    _wifiSsid = p.getString("ssid", "");
    _wifiPass = p.getString("pass", "");
    p.end();
}

// Download and flash new firmware over WiFi. Sends OTA_START immediately so
// the main ESP knows the request was accepted. On success the device reboots
// automatically into the new partition. On failure sends OTA_FAILED and
// re-initialises the camera so normal capture can resume.
static void performOta(const String& url) {
    Serial.println("OTA_START");
    Serial.flush();

    if (_wifiSsid.isEmpty()) {
        Serial.println("OTA_FAILED");
        return;
    }

    // Release camera resources before connecting WiFi — frees PSRAM frame
    // buffer and avoids DMA conflicts between the camera peripheral and WiFi.
    if (_pendingFb) { esp_camera_fb_return(_pendingFb); _pendingFb = nullptr; }
    esp_camera_deinit();
    _camOk = false;

    WiFi.begin(_wifiSsid.c_str(), _wifiPass.c_str());
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        _camOk = initCamera();
        Serial.println("OTA_FAILED");
        Serial.flush();
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.update(client, url);

    // Reaches here only on HTTP_UPDATE_FAILED or HTTP_UPDATE_NO_UPDATES.
    // HTTP_UPDATE_OK reboots automatically before this line.
    WiFi.disconnect(true);
    _camOk = initCamera();
    Serial.println("OTA_FAILED");
    Serial.flush();
}

void setup() {
    Serial.begin(921600);
    delay(200);

    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(100);

    loadWifiCreds();
    _camOk = initCamera();

    Serial.println("CAM_READY");
}

void loop() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "PING") {
        Serial.println("CAM_READY");

    } else if (cmd == "ACK") {
        if (_pendingFb) { esp_camera_fb_return(_pendingFb); _pendingFb = nullptr; }

    } else if (cmd == "RETRY") {
        if (_pendingFb) sendFrame(_pendingFb, _pendingChecksum);
        else            Serial.println("ERROR");

    } else if (cmd.startsWith("SET_WIFI_SSID ")) {
        _wifiSsid = cmd.substring(14);
        Preferences p; p.begin("wf", false);
        p.putString("ssid", _wifiSsid); p.end();
        Serial.println("OK");

    } else if (cmd.startsWith("SET_WIFI_PASS ")) {
        _wifiPass = cmd.substring(14);
        Preferences p; p.begin("wf", false);
        p.putString("pass", _wifiPass); p.end();
        Serial.println("OK");

    } else if (cmd == "CAM_VERSION") {
        Serial.println("VERSION " CAM_FIRMWARE_VERSION);

    } else if (cmd.startsWith("CAM_OTA ")) {
        String url = cmd.substring(8); url.trim();
        performOta(url);

    } else if (cmd.startsWith("CAPTURE")) {
        if (!_camOk) { Serial.println("ERROR"); return; }

        if (_pendingFb) { esp_camera_fb_return(_pendingFb); _pendingFb = nullptr; }

        camera_fb_t* stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { Serial.println("ERROR"); return; }

        _pendingFb = fb;
        _pendingChecksum = checksum32(fb->buf, fb->len);
        sendFrame(fb, _pendingChecksum);
    }
}
