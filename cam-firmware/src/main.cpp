/**
 * ESP32-CAM firmware for InOut checkpoint system
 *
 * Receives commands from the main ESP32 over UART0 (TX=GPIO1, RX=GPIO3)
 * at 921600 baud and sends back a raw JPEG on each CAPTURE request.
 *
 * Protocol:
 *   Main → CAM : "PING\n"           → "CAM_READY\n"
 *   Main → CAM : "CAPTURE <uid>\n"  → "READY <len>\n" + <len> raw JPEG bytes
 *                                   → "ERROR\n"  on failure
 */

#include <Arduino.h>
#include "esp_camera.h"

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
    // VGA (640×480) — good balance of detail vs transfer time
    cfg.frame_size    = FRAMESIZE_HD;    // 1280×720
    cfg.jpeg_quality  = 12;   // 0–63; lower = better quality
    cfg.fb_count      = 1;
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
    return esp_camera_init(&cfg) == ESP_OK;
}

void setup() {
    // UART0 is the only viable UART on AI-Thinker ESP32-CAM.
    // Boot ROM messages at 115200 baud finish before this line runs.
    Serial.begin(921600);
    delay(200);

    // Power on camera module
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(100);

    _camOk = initCamera();

    // Signal ready — main ESP32 polls with PING after its own WiFi init
    Serial.println("CAM_READY");
}

void loop() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "PING") {
        Serial.println("CAM_READY");
        return;
    }

    if (cmd.startsWith("CAPTURE")) {
        if (!_camOk) { Serial.println("ERROR"); return; }

        // Discard one stale frame so the next one is fresh
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { Serial.println("ERROR"); return; }

        // Header line, then raw JPEG bytes
        Serial.printf("READY %u\n", (unsigned)fb->len);
        Serial.write(fb->buf, fb->len);
        Serial.flush();

        esp_camera_fb_return(fb);
    }
}
