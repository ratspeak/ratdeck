#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include "config/BoardConfig.h"

enum RatWiFiMode : uint8_t { RAT_WIFI_OFF = 0, RAT_WIFI_AP = 1, RAT_WIFI_STA = 2 };

struct TCPEndpoint {
    String host;
    uint16_t port = TCP_DEFAULT_PORT;
    bool autoConnect = true;
};

struct UserSettings {
    // Radio
    uint32_t loraFrequency = LORA_DEFAULT_FREQ;
    uint8_t loraSF = LORA_DEFAULT_SF;
    uint32_t loraBW = LORA_DEFAULT_BW;
    uint8_t loraCR = LORA_DEFAULT_CR;
    int8_t loraTxPower = LORA_DEFAULT_TX_POWER;
    long loraPreamble = LORA_DEFAULT_PREAMBLE;

    // WiFi
    RatWiFiMode wifiMode = RAT_WIFI_STA;
    String wifiAPSSID;
    String wifiAPPassword = WIFI_AP_PASSWORD;
    String wifiSTASSID;
    String wifiSTAPassword;

    // TCP outbound connections (STA mode only)
    std::vector<TCPEndpoint> tcpConnections;

    // Display
    uint16_t screenDimTimeout = 30;   // seconds
    uint16_t screenOffTimeout = 60;   // seconds
    uint8_t brightness = 100;  // Percentage 1-100
    bool denseFontMode = false;       // T-Deck Plus: adaptive font toggle

    // Trackball
    uint8_t trackballSpeed = 3;       // 1-5 sensitivity

    // Touch
    uint8_t touchSensitivity = 3;     // 1-5

    // BLE
    bool bleEnabled = false;

    // Audio
    bool audioEnabled = true;
    uint8_t audioVolume = 80;  // 0-100

    // Identity
    String displayName;

    // Announce
    uint16_t announceInterval = 5; // minutes, 5-360

    // Developer mode — unlocks custom radio parameters
    bool devMode = false;
};

class UserConfig {
public:
    // Flash-only (original API, kept for compatibility)
    bool load(FlashStore& flash);
    bool save(FlashStore& flash);

    // Dual-backend: SD primary, flash fallback
    bool load(SDStore& sd, FlashStore& flash);
    bool save(SDStore& sd, FlashStore& flash);

    UserSettings& settings() { return _settings; }
    const UserSettings& settings() const { return _settings; }

private:
    bool parseJson(const String& json);
    String serializeToJson() const;

    UserSettings _settings;
};
