#pragma once
/**
 * config.h
 *
 * VSPI — PN532 readers only:  SCK=18  MISO=19  MOSI=23
 * HSPI — SD card only:        SCK=17  MISO=16  MOSI=33  CS=5
 * I2C  — LCD:                 SDA=21  SCL=22
 *
 * Separate buses = zero bus conflict, no delay needed between inits.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#define FIRMWARE_VERSION "0.4.4"

// ── VSPI — PN532 ─────────────────────────────────────────────────────────────
#define PIN_VSPI_SCK   18
#define PIN_VSPI_MISO  19
#define PIN_VSPI_MOSI  23
#define DEFAULT_CS_IN   25
#define DEFAULT_CS_OUT  4

// ── HSPI — SD card ───────────────────────────────────────────────────────────
#define PIN_HSPI_SCK   17
#define PIN_HSPI_MISO  16
#define PIN_HSPI_MOSI  33
#define DEFAULT_SD_CS   5

// ── I2C ──────────────────────────────────────────────────────────────────────
#define PIN_SDA        21
#define PIN_SCL        22
#define LCD_ADDRESS  0x27

// ── Other ─────────────────────────────────────────────────────────────────────
#define DEFAULT_RELAY   26
#define DEFAULT_BUZZ    15
#define DEFAULT_LED1    14
#define DEFAULT_LED2    13
#define DEFAULT_LED3    12
#define DEFAULT_RELAY_MS 3000
#define DEFAULT_SERVER  ""

// ── Buttons ──────────────────────────────────────────────────────────────────
// VN/VP (GPIO39/36) are input-only, no internal pull resistor — external
// pull-ups are required. Idle = HIGH, pressed = LOW.
#define PIN_BTN_RESET          39   // VN — tap = reset, hold 5s+ = factory reset
#define PIN_BTN_FORGET         36   // VP — hold 3s+ = forget WiFi credentials + restart
#define BTN_DEBOUNCE_MS        50
#define RESET_FACTORY_HOLD_MS  5000
#define FORGET_HOLD_MS         3000

struct ConfigData {
    String serverUrl;
    String deviceToken;
    String identifier;
    String timezone;      // IANA name e.g. "Asia/Tbilisi"
    int    csPin_IN;
    int    csPin_OUT;
    int    csPin_SD;
    int    relayPin;
    int    buzzPin;
    int    relayMs;
    int    configVersion;
    String directionMode = "in"; // "in", "out", or "toggle" — refreshed from server on sync

    // ── WiFi credentials (namespace "wf", separate from WiFiManager) ──────────
    String wifiSsid;
    String wifiPass;

    void loadWifiCreds() {
        Preferences p; p.begin("wf", true);
        wifiSsid = p.getString("ssid", "");
        wifiPass = p.getString("pass", "");
        p.end();
    }

    void saveWifiCreds(const String& ssid, const String& pass) {
        wifiSsid = ssid; wifiPass = pass;
        Preferences p; p.begin("wf", false);
        p.putString("ssid", ssid);
        p.putString("pass", pass);
        p.end();
    }

    void clearWifiCreds() {
        wifiSsid = ""; wifiPass = "";
        Preferences p; p.begin("wf", false); p.clear(); p.end();
    }

    void load() {
        Preferences p; p.begin("cp", true);
        serverUrl     = p.getString("server",    DEFAULT_SERVER);
        deviceToken   = p.getString("token",     "");
        identifier    = p.getString("id",        WiFi.macAddress());
        csPin_IN      = p.getInt("cs_in",        DEFAULT_CS_IN);
        csPin_OUT     = p.getInt("cs_out",       DEFAULT_CS_OUT);
        csPin_SD      = p.getInt("cs_sd",        DEFAULT_SD_CS);
        relayPin      = p.getInt("relay_pin",    DEFAULT_RELAY);
        buzzPin       = p.getInt("buzz_pin",     DEFAULT_BUZZ);
        relayMs       = p.getInt("relay_ms",     DEFAULT_RELAY_MS);
        configVersion = p.getInt("cfg_ver",      1);
        timezone      = p.getString("tz",         "");
        if (timezone.isEmpty()) timezone = "Asia/Tbilisi";
        p.end();
        Serial.printf("[Config] server=%s\n", serverUrl.c_str());
    }

    void save() {
        Preferences p; p.begin("cp", false);
        p.putString("server",    serverUrl);
        p.putString("token",     deviceToken);
        p.putString("id",        identifier);
        p.putInt("cs_in",        csPin_IN);
        p.putInt("cs_out",       csPin_OUT);
        p.putInt("cs_sd",        csPin_SD);
        p.putInt("relay_pin",    relayPin);
        p.putInt("buzz_pin",     buzzPin);
        p.putInt("relay_ms",     relayMs);
        p.putInt("cfg_ver",      configVersion);
        p.putString("tz",        timezone);
        p.end();
        Serial.println("[Config] Saved");
    }

    // Maps IANA timezone name to POSIX TZ string for ESP32 tzset()
    String getPosixTz() {
        if (timezone == "Asia/Tbilisi")      return "GET-4";
        if (timezone == "Europe/London")     return "GMT0BST,M3.5.0/1,M10.5.0";
        if (timezone == "Europe/Paris")      return "CET-1CEST,M3.5.0,M10.5.0/3";
        if (timezone == "Europe/Moscow")     return "MSK-3";
        if (timezone == "Asia/Dubai")        return "GST-4";
        if (timezone == "Asia/Istanbul")     return "TRT-3";
        if (timezone == "America/New_York")  return "EST5EDT,M3.2.0,M11.1.0";
        if (timezone == "America/Los_Angeles") return "PST8PDT,M3.2.0,M11.1.0";
        return "UTC0";  // fallback
    }

    // ── Serial console: dump current config (token value not shown) ────────────
    void printConfig() {
        Serial.println();
        Serial.println("  Server     : " + (serverUrl.length() ? serverUrl : String("(not set)")));
        Serial.println("  Token      : " + (deviceToken.length()
                        ? ("set (" + String(deviceToken.length()) + " chars)") : String("not set")));
        Serial.println("  Identifier : " + identifier);
        Serial.println("  Timezone   : " + timezone + "  (" + getPosixTz() + ")");
        Serial.println("  Direction  : " + directionMode);
        Serial.printf ("  Relay      : GPIO%d  %dms\n", relayPin, relayMs);
        Serial.printf ("  Buzzer     : GPIO%d\n", buzzPin);
        Serial.printf ("  CS IN/OUT  : GPIO%d / GPIO%d\n", csPin_IN, csPin_OUT);
        Serial.printf ("  CS SD      : GPIO%d\n", csPin_SD);
        Serial.printf ("  Config ver : %d\n", configVersion);
        Serial.println();
    }

    void applyFromJson(const JsonObject& obj) {
        if (!obj["server_url"].isNull())     serverUrl     = obj["server_url"].as<String>();
        if (!obj["device_token"].isNull())   deviceToken   = obj["device_token"].as<String>();
        if (!obj["relay_ms"].isNull())       relayMs       = obj["relay_ms"].as<int>();
        if (!obj["relay_pin"].isNull())      relayPin      = obj["relay_pin"].as<int>();
        if (!obj["cs_in"].isNull())          csPin_IN      = obj["cs_in"].as<int>();
        if (!obj["cs_out"].isNull())         csPin_OUT     = obj["cs_out"].as<int>();
        if (!obj["cs_sd"].isNull())          csPin_SD      = obj["cs_sd"].as<int>();
        if (!obj["config_version"].isNull()) configVersion = obj["config_version"].as<int>();
        save();
    }
} Config;
