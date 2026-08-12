#include "UserConfig.h"
#include "config/BoardConfig.h"

namespace {
bool validLoRaFrequency(uint32_t freq) {
    return (freq >= 863000000UL && freq <= 870000000UL) ||
           (freq >= 902000000UL && freq <= 928000000UL) ||
           (freq >= 920000000UL && freq <= 925000000UL);
}
}

void UserConfig::sanitizeSettings() {
    if (_settings.radioRegion >= REGION_COUNT) _settings.radioRegion = REGION_AMERICAS;
    if (!validLoRaFrequency(_settings.loraFrequency)) {
        _settings.loraFrequency = REGION_FREQ[constrain((int)_settings.radioRegion, 0, REGION_COUNT - 1)];
    }
    _settings.loraSF = constrain(_settings.loraSF, 5, 12);
    _settings.loraBW = constrain(_settings.loraBW, 7800UL, 500000UL);
    _settings.loraCR = constrain(_settings.loraCR, 5, 8);
    _settings.loraTxPower = constrain(_settings.loraTxPower, -9, 22);
    _settings.loraPreamble = constrain(_settings.loraPreamble, 6L, 65L);
    _settings.batteryDisplay = constrain((int)_settings.batteryDisplay,
        (int)BATTERY_DISPLAY_PERCENT, (int)BATTERY_DISPLAY_BAR);
    _settings.batteryModel = constrain((int)_settings.batteryModel,
        (int)BATTERY_MODEL_LIPO, (int)BATTERY_MODEL_LINEAR);
    _settings.chargeThresholdV = constrain(_settings.chargeThresholdV,
        BATTERY_CHARGE_THRESHOLD_MIN, BATTERY_CHARGE_THRESHOLD_MAX);
    _settings.fullBatteryV = constrain(_settings.fullBatteryV,
        BATTERY_FULL_VOLTAGE_MIN, BATTERY_FULL_VOLTAGE_MAX);
    _settings.adcDividerRatio = constrain(_settings.adcDividerRatio,
        BATTERY_ADC_DIVIDER_MIN, BATTERY_ADC_DIVIDER_MAX);

    // GPS telemetry hub hash must be exactly 32 hex chars; otherwise reset
    // to the default Lyra collector hash. Empty / malformed values would
    // make TelemetryManager refuse every send anyway, but persisting a
    // broken hash causes silent UI confusion.
    if (_settings.gpsTelemetryHubHash.length() != UserSettings::GPS_TELEMETRY_HUB_HASH_LEN) {
        _settings.gpsTelemetryHubHash = UserSettings::GPS_TELEMETRY_DEFAULT_HUB_HASH;
    } else {
        bool allHex = true;
        for (size_t i = 0; i < UserSettings::GPS_TELEMETRY_HUB_HASH_LEN; i++) {
            char c = _settings.gpsTelemetryHubHash.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                allHex = false;
                break;
            }
        }
        if (!allHex) {
            _settings.gpsTelemetryHubHash = UserSettings::GPS_TELEMETRY_DEFAULT_HUB_HASH;
        }
    }

    // GPS telemetry periodic interval (seconds). 0 is a valid "off" value
    // (the on-demand-only default). Negative values are coerced to 0. Any
    // positive value below the LoRa channel-protection floor is clamped
    // up to the floor; values above the ceiling are clamped down.
    if (_settings.gpsTelemetryIntervalS < 0) {
        _settings.gpsTelemetryIntervalS = 0;
    } else if (_settings.gpsTelemetryIntervalS > 0 &&
               _settings.gpsTelemetryIntervalS < UserSettings::GPS_TELEMETRY_INTERVAL_MIN_S) {
        _settings.gpsTelemetryIntervalS = UserSettings::GPS_TELEMETRY_INTERVAL_MIN_S;
    } else if (_settings.gpsTelemetryIntervalS > UserSettings::GPS_TELEMETRY_INTERVAL_MAX_S) {
        _settings.gpsTelemetryIntervalS = UserSettings::GPS_TELEMETRY_INTERVAL_MAX_S;
    }
}

bool UserConfig::parseJson(const String& json) {
    Serial.printf("[CONFIG] Parsing config (%d bytes)\n", json.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
        return false;
    }

    _settings.radioRegion   = constrain((int)(doc["radio_region"] | 0), 0, REGION_COUNT - 1);
    _settings.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
    _settings.loraSF        = doc["lora_sf"]   | (int)LORA_DEFAULT_SF;
    _settings.loraBW        = doc["lora_bw"]   | (long)LORA_DEFAULT_BW;
    _settings.loraCR        = doc["lora_cr"]   | (int)LORA_DEFAULT_CR;
    _settings.loraTxPower   = doc["lora_txp"]  | (int)LORA_DEFAULT_TX_POWER;
    _settings.loraPreamble  = doc["lora_pre"]  | (long)LORA_DEFAULT_PREAMBLE;
    _settings.loraEnabled   = doc["lora_on"]   | true;

    // WiFi mode — migrate from legacy wifi_enabled bool
    int mode = doc["wifi_mode"] | -1;
    if (mode >= 0) {
        _settings.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
    } else {
        _settings.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
    }
    int restoreMode = doc["wifi_restore_mode"] | (int)(_settings.wifiMode == RAT_WIFI_OFF ? RAT_WIFI_STA : _settings.wifiMode);
    _settings.wifiRestoreMode = (RatWiFiMode)constrain(restoreMode, 1, 2);
    if (_settings.wifiMode != RAT_WIFI_OFF) _settings.wifiRestoreMode = _settings.wifiMode;
    _settings.wifiAPSSID     = doc["wifi_ap_ssid"]     | "";
    _settings.wifiAPPassword = doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD;
    _settings.wifiSTASelected = constrain((int)(doc["wifi_sta_selected"] | 0), 0, (int)WIFI_STA_MAX_NETWORKS - 1);

    // Migrate legacy single-network config into the multi-network list.
    _settings.wifiSTANetworks.clear();
    JsonArray staArr = doc["wifi_sta_networks"];
    if (staArr) {
        for (JsonObject obj : staArr) {
            if (_settings.wifiSTANetworks.size() >= WIFI_STA_MAX_NETWORKS) break;
            WiFiNetwork n;
            n.ssid = obj["ssid"] | "";
            n.password = obj["pass"] | "";
            _settings.wifiSTANetworks.push_back(n);
        }
    } else {
        WiFiNetwork legacy;
        legacy.ssid = doc["wifi_sta_ssid"] | "";
        legacy.password = doc["wifi_sta_pass"] | "";
        if (!legacy.ssid.isEmpty()) _settings.wifiSTANetworks.push_back(legacy);
    }

    // AutoInterface (LAN auto-discovery)
    _settings.autoIfaceEnabled  = doc["autoiface_en"]    | false;
    _settings.autoIfaceGroupId  = doc["autoiface_group"] | "reticulum";
    _settings.autoIfaceMaxPeers = doc["autoiface_max"]   | 8;

    // TCP outbound connections
    _settings.tcpConnections.clear();
    JsonArray tcpArr = doc["tcp_connections"];
    if (tcpArr) {
        for (JsonObject obj : tcpArr) {
            if (_settings.tcpConnections.size() >= MAX_TCP_CONNECTIONS) break;
            TCPEndpoint ep;
            ep.host = obj["host"] | "";
            ep.port = obj["port"] | TCP_DEFAULT_PORT;
            ep.autoConnect = obj["auto"] | true;
            if (!ep.host.isEmpty()) _settings.tcpConnections.push_back(ep);
        }
    }

    _settings.screenDimTimeout = doc["screen_dim"] | 30;
    _settings.screenOffTimeout = doc["screen_off"] | 60;
    // Brightness: stored as 1-100%. Migrate old 0-255 values.
    int rawBri = doc["brightness"] | 100;
    if (rawBri > 100) rawBri = rawBri * 100 / 255;  // Migrate from PWM to percentage
    _settings.brightness = constrain(rawBri, 1, 100);
    _settings.denseFontMode    = doc["dense_font"] | false;
    _settings.themeLight       = doc["theme_light"] | false;
    _settings.keyboardBrightness = constrain(doc["kb_brightness"] | 100, 0, 100);
    _settings.keyboardAutoOn     = doc["kb_auto_on"] | false;
    _settings.keyboardAutoOff    = doc["kb_auto_off"] | false;
    _settings.trackballSpeed   = doc["trackball_speed"] | 3;
    _settings.touchSensitivity = doc["touch_sens"] | 3;
    _settings.bleEnabled       = false;

    // battery settings
    _settings.batteryDisplay = doc["batt_display"] | BATTERY_DISPLAY_BAR;
    _settings.batteryModel = doc["batt_model"] | BATTERY_MODEL_LIPO;
    _settings.chargeThresholdV = doc["charge_thresh_v"] | BATTERY_CHARGE_THRESHOLD_DEFAULT;
    _settings.fullBatteryV = doc["full_battery_v"] | BATTERY_FULL_VOLTAGE_DEFAULT;
    _settings.adcDividerRatio = doc["adc_divider"] | BATTERY_ADC_DIVIDER_DEFAULT;

    _settings.gpsTimeEnabled     = doc["gps_time"]     | true;
    _settings.gpsLocationEnabled = doc["gps_location"] | false;
    _settings.timezoneIdx        = doc["tz_idx"]       | 6;
    _settings.timezoneSet        = doc["tz_set"]       | false;
    _settings.use24HourTime      = doc["time_24h"]     | false;

    // GPS telemetry (issue #64) — opt-in position sharing to a hub.
    _settings.gpsTelemetryEnabled = doc["gps_telemetry_enabled"] | false;
    _settings.gpsTelemetryHubHash = doc["gps_telemetry_hub"]      | UserSettings::GPS_TELEMETRY_DEFAULT_HUB_HASH;
    // Periodic interval (seconds). 0 = on-demand only (default). Non-zero
    // values are clamped in sanitizeSettings() to enforce the LoRa
    // channel-protection minimum.
    _settings.gpsTelemetryIntervalS = doc["gps_telemetry_interval_s"] | 0;

    _settings.audioEnabled = doc["audio_on"]  | true;
    _settings.audioVolume  = doc["audio_vol"] | 80;

    _settings.displayName = doc["display_name"] | "";
    _settings.sdStorageEnabled = doc["sd_storage"] | _settings.sdStorageEnabled;
    _settings.announceInterval = doc["announce_int"] | 30;
    if (_settings.announceInterval < 30) _settings.announceInterval = 30;
    if (_settings.announceInterval > 360) _settings.announceInterval = 360;
    _settings.devMode     = doc["dev_mode"]     | false;

    sanitizeSettings();
    Serial.println("[CONFIG] Settings loaded");
    return true;
}

String UserConfig::serializeToJson() {
    sanitizeSettings();
    JsonDocument doc;

    doc["radio_region"] = _settings.radioRegion;
    doc["lora_freq"] = _settings.loraFrequency;
    doc["lora_sf"]   = _settings.loraSF;
    doc["lora_bw"]   = _settings.loraBW;
    doc["lora_cr"]   = _settings.loraCR;
    doc["lora_txp"]  = _settings.loraTxPower;
    doc["lora_pre"]  = _settings.loraPreamble;
    doc["lora_on"]   = _settings.loraEnabled;

    doc["wifi_mode"] = (int)_settings.wifiMode;
    doc["wifi_restore_mode"] = (int)_settings.wifiRestoreMode;
    doc["wifi_ap_ssid"] = _settings.wifiAPSSID;
    doc["wifi_ap_pass"] = _settings.wifiAPPassword;
    doc["wifi_sta_selected"] = (int)constrain((int)_settings.wifiSTASelected, 0, (int)WIFI_STA_MAX_NETWORKS - 1);
    JsonArray staArr = doc["wifi_sta_networks"].to<JsonArray>();
    for (size_t slot = 0; slot < WIFI_STA_MAX_NETWORKS; slot++) {
        JsonObject obj = staArr.add<JsonObject>();
        if (slot < _settings.wifiSTANetworks.size()) {
            obj["ssid"] = _settings.wifiSTANetworks[slot].ssid;
            obj["pass"] = _settings.wifiSTANetworks[slot].password;
        } else {
            obj["ssid"] = "";
            obj["pass"] = "";
        }
    }

    doc["autoiface_en"]    = _settings.autoIfaceEnabled;
    doc["autoiface_group"] = _settings.autoIfaceGroupId;
    doc["autoiface_max"]   = _settings.autoIfaceMaxPeers;

    JsonArray tcpArr = doc["tcp_connections"].to<JsonArray>();
    for (auto& ep : _settings.tcpConnections) {
        JsonObject obj = tcpArr.add<JsonObject>();
        obj["host"] = ep.host;
        obj["port"] = ep.port;
        obj["auto"] = ep.autoConnect;
    }

    doc["screen_dim"] = _settings.screenDimTimeout;
    doc["screen_off"] = _settings.screenOffTimeout;
    doc["brightness"] = _settings.brightness;
    doc["dense_font"] = _settings.denseFontMode;
    doc["theme_light"] = _settings.themeLight;
    doc["kb_brightness"] = _settings.keyboardBrightness;
    doc["kb_auto_on"] = _settings.keyboardAutoOn;
    doc["kb_auto_off"] = _settings.keyboardAutoOff;
    doc["trackball_speed"] = _settings.trackballSpeed;
    doc["touch_sens"] = _settings.touchSensitivity;
    doc["ble_enabled"] = false;

    // battery settings
    doc["batt_display"] = _settings.batteryDisplay;
    doc["batt_model"]   = _settings.batteryModel;
    doc["charge_thresh_v"] = _settings.chargeThresholdV;
    doc["full_battery_v"]  = _settings.fullBatteryV;
    doc["adc_divider"]     = _settings.adcDividerRatio;

    doc["gps_time"]     = _settings.gpsTimeEnabled;
    doc["gps_location"] = _settings.gpsLocationEnabled;
    doc["tz_idx"]       = _settings.timezoneIdx;
    doc["tz_set"]       = _settings.timezoneSet;
    doc["time_24h"]     = _settings.use24HourTime;

    // GPS telemetry (issue #64) — opt-in position sharing to a hub.
    doc["gps_telemetry_enabled"] = _settings.gpsTelemetryEnabled;
    doc["gps_telemetry_hub"]      = _settings.gpsTelemetryHubHash;
    doc["gps_telemetry_interval_s"] = _settings.gpsTelemetryIntervalS;

    doc["audio_on"]  = _settings.audioEnabled;
    doc["audio_vol"] = _settings.audioVolume;

    doc["display_name"] = _settings.displayName;
    doc["sd_storage"] = _settings.sdStorageEnabled;
    doc["announce_int"] = _settings.announceInterval;
    doc["dev_mode"]     = _settings.devMode;

    String json;
    serializeJson(doc, json);
    return json;
}

bool UserConfig::load(FlashStore& flash) {
    String json = flash.readString(PATH_USER_CONFIG);
    if (json.isEmpty()) {
        Serial.println("[CONFIG] No saved config, using defaults");
        return false;
    }
    return parseJson(json);
}

bool UserConfig::save(FlashStore& flash) {
    String json = serializeToJson();
    bool ok = flash.writeString(PATH_USER_CONFIG, json);
    if (ok) Serial.println("[CONFIG] Settings saved to flash");
    return ok;
}

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    String json = flash.readString(PATH_USER_CONFIG);
    bool flashOk = false;
    if (!json.isEmpty()) {
        flashOk = parseJson(json);
    } else {
        Serial.println("[CONFIG] No flash config found");
    }

    if (flashOk && _settings.sdStorageEnabled && sd.isReady()) {
        String sdJson = sd.readString(SD_PATH_USER_CONFIG);
        if (!sdJson.isEmpty()) {
            Serial.println("[CONFIG] Loading opt-in SD config");
            bool sdOk = parseJson(sdJson);
            if (sdOk) {
                _settings.sdStorageEnabled = true;
                return true;
            }
            Serial.println("[CONFIG] SD config invalid, keeping flash config");
            return true;
        }
    }

    if (!flashOk && sd.isReady()) {
        String sdJson = sd.readString(SD_PATH_USER_CONFIG);
        if (!sdJson.isEmpty()) {
            Serial.println("[CONFIG] Flash config unavailable, trying SD config");
            bool sdOk = parseJson(sdJson);
            if (sdOk) {
                _settings.sdStorageEnabled = true;
                flash.writeString(PATH_USER_CONFIG, serializeToJson());
                return true;
            }
            Serial.println("[CONFIG] SD config invalid");
        }
    }

    if (!flashOk) Serial.println("[CONFIG] No saved config, using defaults");
    return flashOk;
}

bool UserConfig::save(SDStore& sd, FlashStore& flash) {
    String json = serializeToJson();
    bool ok = false;

    if (_settings.sdStorageEnabled && sd.isReady()) {
        sd.ensureDir("/ratdeck");
        sd.ensureDir("/ratdeck/config");
        if (sd.writeString(SD_PATH_USER_CONFIG, json)) {
            Serial.println("[CONFIG] Saved to SD");
            ok = true;
        } else {
            Serial.println("[CONFIG] SD write failed");
        }
    }

    // Write to flash (backup)
    if (flash.writeString(PATH_USER_CONFIG, json)) {
        Serial.println("[CONFIG] Saved to flash");
        ok = true;
    }

    return ok;
}
