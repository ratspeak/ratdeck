#include "UserConfig.h"
#include "config/BoardConfig.h"

bool UserConfig::parseJson(const String& json) {
    Serial.printf("[CONFIG] Raw JSON (%d bytes): %s\n", json.length(), json.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
        return false;
    }

    _settings.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
    _settings.loraSF        = doc["lora_sf"]   | (int)LORA_DEFAULT_SF;
    _settings.loraBW        = doc["lora_bw"]   | (long)LORA_DEFAULT_BW;
    _settings.loraCR        = doc["lora_cr"]   | (int)LORA_DEFAULT_CR;
    _settings.loraTxPower   = doc["lora_txp"]  | (int)LORA_DEFAULT_TX_POWER;
    _settings.loraPreamble  = doc["lora_pre"]  | (long)LORA_DEFAULT_PREAMBLE;

    // WiFi mode — migrate from legacy wifi_enabled bool
    int mode = doc["wifi_mode"] | -1;
    if (mode >= 0) {
        _settings.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
    } else {
        _settings.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
    }
    _settings.wifiAPSSID     = doc["wifi_ap_ssid"]     | "";
    _settings.wifiAPPassword = doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD;
    _settings.wifiSTASSID    = doc["wifi_sta_ssid"]    | "";
    _settings.wifiSTAPassword = doc["wifi_sta_pass"]   | "";

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
    _settings.trackballSpeed   = doc["trackball_speed"] | 3;
    _settings.touchSensitivity = doc["touch_sens"] | 3;
    _settings.bleEnabled       = doc["ble_enabled"] | false;

    _settings.audioEnabled = doc["audio_on"]  | true;
    _settings.audioVolume  = doc["audio_vol"] | 80;

    _settings.displayName = doc["display_name"] | "";
    _settings.announceInterval = doc["announce_int"] | 5;
    _settings.devMode     = doc["dev_mode"]     | false;

    Serial.println("[CONFIG] Settings loaded");
    return true;
}

String UserConfig::serializeToJson() const {
    JsonDocument doc;

    doc["lora_freq"] = _settings.loraFrequency;
    doc["lora_sf"]   = _settings.loraSF;
    doc["lora_bw"]   = _settings.loraBW;
    doc["lora_cr"]   = _settings.loraCR;
    doc["lora_txp"]  = _settings.loraTxPower;
    doc["lora_pre"]  = _settings.loraPreamble;

    doc["wifi_mode"] = (int)_settings.wifiMode;
    doc["wifi_ap_ssid"] = _settings.wifiAPSSID;
    doc["wifi_ap_pass"] = _settings.wifiAPPassword;
    doc["wifi_sta_ssid"] = _settings.wifiSTASSID;
    doc["wifi_sta_pass"] = _settings.wifiSTAPassword;

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
    doc["trackball_speed"] = _settings.trackballSpeed;
    doc["touch_sens"] = _settings.touchSensitivity;
    doc["ble_enabled"] = _settings.bleEnabled;

    doc["audio_on"]  = _settings.audioEnabled;
    doc["audio_vol"] = _settings.audioVolume;

    doc["display_name"] = _settings.displayName;
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
    // Try SD card first
    if (sd.isReady()) {
        String json = sd.readString(SD_PATH_USER_CONFIG);
        if (!json.isEmpty()) {
            Serial.println("[CONFIG] Loading from SD card");
            return parseJson(json);
        }
    }

    // Fall back to flash
    String json = flash.readString(PATH_USER_CONFIG);
    if (json.isEmpty()) {
        Serial.println("[CONFIG] No saved config, using defaults");
        return false;
    }

    bool ok = parseJson(json);

    // Auto-migrate: flash had config but SD didn't — copy to SD
    if (ok && sd.isReady()) {
        Serial.println("[CONFIG] Migrating config from flash to SD...");
        sd.ensureDir("/ratputer");
        sd.ensureDir("/ratputer/config");
        String migrateJson = serializeToJson();
        if (sd.writeString(SD_PATH_USER_CONFIG, migrateJson)) {
            Serial.println("[CONFIG] Migration complete");
        }
    }

    return ok;
}

bool UserConfig::save(SDStore& sd, FlashStore& flash) {
    String json = serializeToJson();
    bool ok = false;

    // Write to SD (primary)
    if (sd.isReady()) {
        sd.ensureDir("/ratputer");
        sd.ensureDir("/ratputer/config");
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
