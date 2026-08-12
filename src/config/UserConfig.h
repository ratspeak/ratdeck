#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include "config/BoardConfig.h"

enum RatWiFiMode : uint8_t { RAT_WIFI_OFF = 0, RAT_WIFI_AP = 1, RAT_WIFI_STA = 2 };

struct WiFiNetwork {
    String ssid;
    String password;
};

constexpr size_t WIFI_STA_MAX_NETWORKS = 3;

constexpr uint8_t BATTERY_DISPLAY_PERCENT = 0;
constexpr uint8_t BATTERY_DISPLAY_BAR = 1;
constexpr uint8_t BATTERY_MODEL_LIPO = 0;
constexpr uint8_t BATTERY_MODEL_LINEAR = 1;
// 4.18V, not the 4.2V absolute LiPo max - actively charging cells commonly
// sit in the high-4.1/low-4.2 range well before topping out, so a threshold
// right at 4.2 would misclassify a nearly-full charging cell as
// "discharging". Previously 4.0f, which misclassified anything >=80% SoC as
// actively charging even when just resting on battery.
constexpr float BATTERY_CHARGE_THRESHOLD_DEFAULT = 4.18f;
constexpr float BATTERY_FULL_VOLTAGE_DEFAULT = 3.9f;
constexpr float BATTERY_CHARGE_THRESHOLD_MIN = 3.80f;
constexpr float BATTERY_CHARGE_THRESHOLD_MAX = 4.30f;
constexpr float BATTERY_FULL_VOLTAGE_MIN = 3.50f;
constexpr float BATTERY_FULL_VOLTAGE_MAX = 4.20f;
// ADC voltage-divider ratio for the battery-sense pin. Was hardcoded to
// 2.0f in Power.cpp (matches the stock T-Deck Plus battery's sense-line
// wiring) — aftermarket/extended battery packs can be wired through a
// different physical divider network on their sense connection, which no
// amount of curve/offset tuning (fullBatteryV, chargeThresholdV) can
// correct, since those only reshape an already-correct voltage reading.
// Derive the right value the same way as a multimeter-based ADC
// calibration: measured_mv / (raw_computed_mv_at_2.0x_ratio) * 2.0.
constexpr float BATTERY_ADC_DIVIDER_DEFAULT = 2.0f;
constexpr float BATTERY_ADC_DIVIDER_MIN = 1.0f;
constexpr float BATTERY_ADC_DIVIDER_MAX = 4.0f;

struct TCPEndpoint {
    String host;
    uint16_t port = TCP_DEFAULT_PORT;
    bool autoConnect = true;
};

struct UserSettings {
    // Radio
    uint8_t radioRegion = REGION_AMERICAS;
    uint32_t loraFrequency = LORA_DEFAULT_FREQ;
    uint8_t loraSF = LORA_DEFAULT_SF;
    uint32_t loraBW = LORA_DEFAULT_BW;
    uint8_t loraCR = LORA_DEFAULT_CR;
    int8_t loraTxPower = LORA_DEFAULT_TX_POWER;
    long loraPreamble = LORA_DEFAULT_PREAMBLE;
    bool loraEnabled = true;

    // WiFi
    RatWiFiMode wifiMode = RAT_WIFI_STA;
    RatWiFiMode wifiRestoreMode = RAT_WIFI_STA;
    String wifiAPSSID;
    String wifiAPPassword = WIFI_AP_PASSWORD;
    std::vector<WiFiNetwork> wifiSTANetworks;
    uint8_t wifiSTASelected = 0;

    // AutoInterface (Reticulum LAN auto-discovery via IPv6 multicast).
    // Active only in STA mode; opt-in until proven stable on real APs.
    bool   autoIfaceEnabled  = false;
    String autoIfaceGroupId  = "reticulum";
    uint8_t autoIfaceMaxPeers = 8;

    // TCP outbound connections (STA mode only)
    std::vector<TCPEndpoint> tcpConnections;

    // Display
    uint16_t screenDimTimeout = 30;   // seconds
    uint16_t screenOffTimeout = 60;   // seconds
    uint8_t brightness = 100;  // Percentage 1-100
    bool denseFontMode = false;       // T-Deck Plus: adaptive font toggle
    bool themeLight = false;          // false = dark (original palette)

    // Battery
    uint8_t batteryDisplay = BATTERY_DISPLAY_BAR;
    uint8_t batteryModel = BATTERY_MODEL_LIPO;
    float chargeThresholdV = BATTERY_CHARGE_THRESHOLD_DEFAULT;
    float fullBatteryV = BATTERY_FULL_VOLTAGE_DEFAULT;
    float adcDividerRatio = BATTERY_ADC_DIVIDER_DEFAULT;

    // Keyboard
    uint8_t keyboardBrightness = 100; // Percentage 0-100 (0 = off)
    bool keyboardAutoOn = false;      // Backlight ON when switching to ACTIVE power state
    bool keyboardAutoOff = false;     // Backlight OFF when switching from ACTIVE power state

    // Trackball
    uint8_t trackballSpeed = 3;       // 1-5 sensitivity

    // Touch
    uint8_t touchSensitivity = 3;     // 1-5

    // BLE
    bool bleEnabled = false;

    // GPS & Time
    bool gpsTimeEnabled = true;      // GPS time sync (default ON)
    bool gpsLocationEnabled = false; // GPS position tracking (default OFF, user must opt in)
    uint8_t timezoneIdx = 6;         // Index into TIMEZONE_TABLE (default: New York EST/EDT)
    bool timezoneSet = false;        // false = show timezone picker at boot
    bool use24HourTime = false;      // false = 12h (no AM/PM), true = 24h

    // GPS Telemetry (issue #64) — opt-in position sharing to a hub. Off by
    // default per privacy requirement; user must explicitly enable AND have
    // a working hub hash configured.
    bool   gpsTelemetryEnabled = false;                                       // master opt-in switch, default OFF
    String gpsTelemetryHubHash = "da424e0f47657d7575df58a2b83b111b";           // 32 hex chars, Lyra collector default
    static constexpr size_t GPS_TELEMETRY_HUB_HASH_LEN = 32;
    static constexpr const char* GPS_TELEMETRY_DEFAULT_HUB_HASH = "da424e0f47657d7575df58a2b83b111b";

    // Periodic telemetry cadence (seconds). 0 = on-demand only (default):
    // periodic sending must never silently start just because the user
    // enabled "Send Telemetry"; the user must also pick a non-zero
    // interval explicitly. Non-zero values are clamped to
    // [GPS_TELEMETRY_INTERVAL_MIN_S, GPS_TELEMETRY_INTERVAL_MAX_S] by
    // sanitizeSettings(). The 60 s minimum protects the LoRa channel
    // from being hammered by a careless 1-second setting.
    int gpsTelemetryIntervalS = 0;
    static constexpr int GPS_TELEMETRY_INTERVAL_MIN_S = 60;        // 1 min floor
    static constexpr int GPS_TELEMETRY_INTERVAL_MAX_S = 86400;     // 24 h ceiling

    // Audio
    bool audioEnabled = true;
    uint8_t audioVolume = 80;  // 0-100

    // Identity
    String displayName;

    // Storage
    bool sdStorageEnabled = false;   // Removable SD stores plaintext unless explicitly enabled

    // Announce
    uint16_t announceInterval = 30; // minutes, 30-360

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
    String serializeToJson();
    void sanitizeSettings();

    UserSettings _settings;
};
