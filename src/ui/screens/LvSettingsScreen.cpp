#include "LvSettingsScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "config/Config.h"
#include "config/UserConfig.h"
#include "ui/screens/LvTimezoneScreen.h"  // For TIMEZONE_TABLE
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "radio/SX1262.h"
#include "audio/AudioNotify.h"
#include "hal/Power.h"
#include "transport/WiFiInterface.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "fonts/fonts.h"

struct RadioPresetLv {
    const char* name;
    uint8_t sf; uint32_t bw; uint8_t cr; int8_t txPower; long preamble;
};
static const RadioPresetLv LV_PRESETS[] = {
    {"Short Turbo",   7,  500000, 5,  14, 18},
    {"Short Fast",    7,  250000, 5,  14, 18},
    {"Short Slow",    8,  250000, 5,  14, 18},
    {"Medium Fast",   9,  250000, 5,  17, 18},
    {"Medium Slow",   10, 250000, 5,  17, 18},
    {"Long Turbo",    11, 500000, 8,  22, 18},
    {"Long Fast",     11, 250000, 5,  22, 18},
    {"Long Moderate", 11, 125000, 8,  22, 18},
};
static constexpr int LV_NUM_PRESETS = 8;

namespace {

bool labelEq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

const char* onOff(bool enabled) {
    return enabled ? "ON" : "OFF";
}

const char* wifiModeLabel(RatWiFiMode mode) {
    switch (mode) {
        case RAT_WIFI_AP: return "Hotspot";
        case RAT_WIFI_STA: return "Client";
        case RAT_WIFI_OFF:
        default: return "Off";
    }
}

String maskedValue(const String& value) {
    if (value.isEmpty()) return String("");
    int len = constrain((int)value.length(), 4, 12);
    String masked;
    masked.reserve(len);
    for (int i = 0; i < len; i++) masked += '*';
    return masked;
}

bool isWiFiSSIDLabel(const char* label) {
    return labelEq(label, "WiFi SSID");
}

bool isWiFiPasswordLabel(const char* label) {
    return labelEq(label, "WiFi Password");
}

size_t selectedWiFiSlot(const UserSettings& s) {
    return s.wifiSTASelected < WIFI_STA_MAX_NETWORKS ? s.wifiSTASelected : 0;
}

void ensureWiFiSlot(UserSettings& s, size_t slot) {
    while (s.wifiSTANetworks.size() <= slot && s.wifiSTANetworks.size() < WIFI_STA_MAX_NETWORKS) {
        s.wifiSTANetworks.push_back({});
    }
}

String wifiProfileValue(const UserSettings& s, size_t slot) {
    String label = String(slot + 1);
    label += " ";
    if (slot < s.wifiSTANetworks.size() && !s.wifiSTANetworks[slot].ssid.isEmpty()) {
        label += s.wifiSTANetworks[slot].ssid;
    } else {
        label += "empty";
    }
    return label;
}

String selectedWiFiSSID(const UserSettings& s) {
    size_t slot = selectedWiFiSlot(s);
    if (slot >= s.wifiSTANetworks.size()) return String("");
    return s.wifiSTANetworks[slot].ssid;
}

void clipLabel(lv_obj_t* lbl, int width) {
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
}

bool extractReleaseTag(const String& payload, char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    int key = payload.indexOf("\"tag_name\"");
    if (key < 0) return false;
    int colon = payload.indexOf(':', key);
    if (colon < 0) return false;
    int start = payload.indexOf('"', colon + 1);
    if (start < 0) return false;
    int end = payload.indexOf('"', start + 1);
    if (end <= start + 1) return false;

    String tag = payload.substring(start + 1, end);
    if (tag.startsWith("v") || tag.startsWith("V")) tag.remove(0, 1);
    tag.trim();
    if (tag.isEmpty()) return false;
    strlcpy(out, tag.c_str(), outLen);
    return out[0] != '\0';
}

}  // namespace

int LvSettingsScreen::detectPreset() const {
    if (!_cfg) return -1;
    auto& s = _cfg->settings();
    // Check if frequency matches the current region's default
    uint32_t regionFreq = REGION_FREQ[constrain(s.radioRegion, 0, REGION_COUNT - 1)];
    if (s.loraFrequency != regionFreq) return -1;  // Custom frequency: no preset match
    for (int i = 0; i < LV_NUM_PRESETS; i++) {
        if (s.loraSF == LV_PRESETS[i].sf && s.loraBW == LV_PRESETS[i].bw
            && s.loraCR == LV_PRESETS[i].cr && s.loraTxPower == LV_PRESETS[i].txPower)
            return i;
    }
    return -1;
}

void LvSettingsScreen::applyPreset(int presetIdx) {
    if (!_cfg || presetIdx < 0 || presetIdx >= LV_NUM_PRESETS) return;
    auto& s = _cfg->settings();
    const auto& p = LV_PRESETS[presetIdx];
    s.loraSF = p.sf; s.loraBW = p.bw; s.loraCR = p.cr;
    s.loraTxPower = p.txPower; s.loraPreamble = p.preamble;
    s.loraFrequency = REGION_FREQ[constrain(s.radioRegion, 0, REGION_COUNT - 1)];
}

bool LvSettingsScreen::isEditable(int idx) const {
    if (idx < 0 || idx >= (int)_items.size()) return false;
    auto t = _items[idx].type;
    return t == SettingType::INTEGER || t == SettingType::TOGGLE
        || t == SettingType::ENUM_CHOICE || t == SettingType::ACTION
        || t == SettingType::TEXT_INPUT;
}

void LvSettingsScreen::skipToNextEditable(int dir) {
    int n = _catRangeEnd;
    int start = _selectedIdx;
    for (int i = 0; i < (n - _catRangeStart); i++) {
        _selectedIdx += dir;
        if (_selectedIdx < _catRangeStart) _selectedIdx = _catRangeStart;
        if (_selectedIdx >= n) _selectedIdx = n - 1;
        if (isEditable(_selectedIdx)) return;
        if (_selectedIdx == _catRangeStart && dir < 0) return;
        if (_selectedIdx == n - 1 && dir > 0) return;
    }
    _selectedIdx = start;
}

bool LvSettingsScreen::settingNeedsReboot(const SettingItem& item) const {
    if (!_cfg) return false;
    const auto& s = _cfg->settings();
    if (labelEq(item.label, "WiFi Mode")) return s.wifiMode != _rebootSnap.wifiMode;
    if (labelEq(item.label, "LoRa Radio")) return loraSettingsChanged();
    if (labelEq(item.label, "WiFi Profile")) return s.wifiSTASelected != _rebootSnap.wifiSTASelected;
    if (isWiFiSSIDLabel(item.label) || isWiFiPasswordLabel(item.label)) return interfaceSettingsChanged();
    if (labelEq(item.label, "Scan Networks") || labelEq(item.label, "Forget Network")) return interfaceSettingsChanged();
    if (labelEq(item.label, "TCP Server") || labelEq(item.label, "Host") ||
        labelEq(item.label, "Port")) return tcpSettingsChanged();
    if (labelEq(item.label, "LAN Discovery")) return s.autoIfaceEnabled != _rebootSnap.autoIfaceEnabled;
    if (labelEq(item.label, "SD Message Store")) return storageSettingsChanged();
    return false;
}

bool LvSettingsScreen::categoryNeedsReboot(int catIdx) const {
    if (catIdx < 0 || catIdx >= (int)_categories.size()) return false;
    if (labelEq(_categories[catIdx].name, "LoRa")) {
        return loraSettingsChanged();
    }
    if (labelEq(_categories[catIdx].name, "Network")) {
        return interfaceSettingsChanged() || tcpSettingsChanged();
    }
    if (labelEq(_categories[catIdx].name, "Storage & Maintenance")) {
        return storageSettingsChanged();
    }
    return false;
}

bool LvSettingsScreen::confirmableAction(const SettingItem& item) const {
    return labelEq(item.label, "Developer Radio Controls")
        || labelEq(item.label, "Format SD Card")
        || labelEq(item.label, "Erase rsDeck SD Data")
        || labelEq(item.label, "Erase Device");
}

bool LvSettingsScreen::armedAction(const SettingItem& item) const {
    return (_confirmingInitSD && labelEq(item.label, "Format SD Card")) ||
        (_confirmingWipeSD && labelEq(item.label, "Erase rsDeck SD Data")) ||
        (_confirmingReset && labelEq(item.label, "Erase Device")) ||
        (_confirmingDevMode && labelEq(item.label, "Developer Radio Controls"));
}

bool LvSettingsScreen::destructiveAction(const SettingItem& item) const {
    return labelEq(item.label, "Format SD Card")
        || labelEq(item.label, "Erase rsDeck SD Data")
        || labelEq(item.label, "Erase Device");
}

const char* LvSettingsScreen::confirmationTitle() const {
    if (_confirmingInitSD) return "ARMED: FORMAT SD CARD";
    if (_confirmingWipeSD) return "ARMED: ERASE SD DATA";
    if (_confirmingReset) return "ARMED: ERASE DEVICE";
    if (_confirmingDevMode) return "ARMED: UNLOCK RF CONTROLS";
    return nullptr;
}

const char* LvSettingsScreen::confirmationDetail() const {
    if (_confirmingInitSD) return "Hold trackball to format. Esc cancels.";
    if (_confirmingWipeSD) return "Hold trackball to erase SD data. Esc cancels.";
    if (_confirmingReset) return "Hold trackball to erase device. Esc cancels.";
    if (_confirmingDevMode) return "Hold trackball to unlock. Esc cancels.";
    return nullptr;
}

bool LvSettingsScreen::hasPendingConfirmation() const {
    return _confirmingInitSD || _confirmingWipeSD || _confirmingReset || _confirmingDevMode;
}

void LvSettingsScreen::clearConfirmations() {
    _confirmingInitSD = false;
    _confirmingWipeSD = false;
    _confirmingReset = false;
    _confirmingDevMode = false;
}

void LvSettingsScreen::runFormatSD() {
    _confirmingInitSD = false;
    if (!_sd || !_sd->isReady()) {
        if (_ui) _ui->lvStatusBar().showToast("No SD card", 1200);
        rebuildItemList();
        return;
    }
    if (_ui) _ui->lvStatusBar().showToast("Formatting SD card...", 2000);
    bool ok = _sd->formatForRsDeck();
    if (_ui) _ui->lvStatusBar().showToast(ok ? "SD card formatted" : "SD format failed", 1500);
    rebuildItemList();
}

void LvSettingsScreen::runWipeSD() {
    _confirmingWipeSD = false;
    if (!_sd || !_sd->isReady()) {
        if (_ui) _ui->lvStatusBar().showToast("No SD card", 1200);
        rebuildItemList();
        return;
    }
    if (_ui) _ui->lvStatusBar().showToast("Erasing rsDeck SD data...", 2000);
    bool ok = _sd->wipeRsDeck();
    if (_ui) _ui->lvStatusBar().showToast(ok ? "SD data erased" : "SD erase failed", 1500);
    rebuildItemList();
}

void LvSettingsScreen::runFactoryReset() {
    _confirmingReset = false;
    if (_ui) _ui->lvStatusBar().showToast("Erasing device...", 3000);
    if (_sd && _sd->isReady()) _sd->wipeRsDeck();
    if (_flash) _flash->format();
    nvs_flash_erase();
    delay(1500);  // Long enough for key state to clear before reboot
    ESP.restart();
}

void LvSettingsScreen::runEnableDevMode() {
    _confirmingDevMode = false;
    if (!_cfg) {
        rebuildItemList();
        return;
    }
    _cfg->settings().devMode = true;
    applyAndSave();
    buildItems();
    enterCategory(_categoryIdx);
    if (_ui) _ui->lvStatusBar().showToast("Developer radio controls unlocked", 1500);
}

void LvSettingsScreen::startFirmwareCheck() {
    if (WiFi.status() != WL_CONNECTED) {
        if (_ui) _ui->lvStatusBar().showToast("Connect WiFi to check firmware", 2500);
        return;
    }
    if (_fwCheckState == FirmwareCheckState::RUNNING) {
        if (_ui) _ui->lvStatusBar().showToast("Firmware check already running", 1200);
        return;
    }

    _fwCheckVersion[0] = '\0';
    _fwCheckState = FirmwareCheckState::RUNNING;
    if (xTaskCreatePinnedToCore(
            firmwareCheckTask,
            "fw-check",
            6144,
            this,
            1,
            &_fwCheckTask,
            0) != pdPASS) {
        _fwCheckTask = nullptr;
        _fwCheckState = FirmwareCheckState::FAILED;
    } else if (_ui) {
        _ui->lvStatusBar().showToast("Checking firmware release...", 1200);
    }
}

void LvSettingsScreen::firmwareCheckTask(void* arg) {
    auto* self = static_cast<LvSettingsScreen*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    FirmwareCheckState result = FirmwareCheckState::FAILED;
    char version[sizeof(self->_fwCheckVersion)] = {};

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(5000);
    if (http.begin("https://api.github.com/repos/ratspeak/rsDeck/releases/latest")) {
        http.addHeader("Accept", "application/vnd.github.v3+json");
        int httpCode = http.GET();
        if (httpCode == 200) {
            WiFiClient* stream = http.getStreamPtr();
            String payload;
            payload.reserve(1536);
            unsigned long start = millis();
            while (stream && http.connected() && millis() - start < 5000 && payload.length() < 2048) {
                while (stream->available() && payload.length() < 2048) {
                    payload += (char)stream->read();
                    if (payload.indexOf("\"tag_name\"") >= 0 && payload.indexOf('\n', payload.indexOf("\"tag_name\"")) >= 0) {
                        break;
                    }
                }
                if (extractReleaseTag(payload, version, sizeof(version))) break;
                delay(10);
            }
            if (extractReleaseTag(payload, version, sizeof(version))) {
                result = strcmp(version, RSDECK_VERSION_STRING) > 0
                    ? FirmwareCheckState::AVAILABLE
                    : FirmwareCheckState::CURRENT;
            }
        }
    }
    http.end();

    if (version[0]) strlcpy(self->_fwCheckVersion, version, sizeof(self->_fwCheckVersion));
    self->_fwCheckState = result;
    self->_fwCheckTask = nullptr;
    vTaskDelete(nullptr);
}

void LvSettingsScreen::buildItems() {
    _items.clear();
    _categories.clear();
    if (!_cfg) return;
    auto& s = _cfg->settings();
    int idx = 0;

    // Identity & Device
    int devStart = idx;
    _items.push_back({"Firmware", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String(RSDECK_VERSION_STRING); }});
    idx++;
    _items.push_back({"LXMF Address", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _destinationHash.length() > 0 ? _destinationHash : String("unknown"); }});
    idx++;
    _items.push_back({"Identity Hash", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _identityHash; }});
    idx++;
    {
        SettingItem nameItem;
        nameItem.label = "Display Name";
        nameItem.type = SettingType::TEXT_INPUT;
        nameItem.textGetter = [&s]() { return s.displayName; };
        nameItem.textSetter = [this, &s](const String& v) {
            s.displayName = v;
            // Keep active identity slot in sync
            if (_idMgr && _idMgr->activeIndex() >= 0) {
                _idMgr->setDisplayName(_idMgr->activeIndex(), v);
            }
        };
        nameItem.maxTextLen = 16;
        _items.push_back(nameItem);
        idx++;
    }
    if (_idMgr && _idMgr->count() > 0) {
        SettingItem idSwitch;
        idSwitch.label = "Identity Slot";
        idSwitch.type = SettingType::ENUM_CHOICE;
        idSwitch.getter = [this]() { return _idMgr->activeIndex(); };
        idSwitch.setter = [this](int v) {
            if (v == _idMgr->activeIndex()) return;
            // Save current display name to outgoing identity slot
            if (_cfg) {
                _idMgr->setDisplayName(_idMgr->activeIndex(), _cfg->settings().displayName);
            }
            RNS::Identity newId;
            if (_idMgr->switchTo(v, newId)) {
                // Load incoming identity's display name into config
                if (_cfg) {
                    _cfg->settings().displayName = _idMgr->getDisplayName(v);
                }
                if (_ui) _ui->lvStatusBar().showToast("Identity switched; rebooting", 2000);
                applyAndSave();
                delay(1000);
                ESP.restart();
            } else {
                if (_ui) _ui->lvStatusBar().showToast("Identity switch failed", 1500);
            }
        };
        idSwitch.minVal = 0;
        idSwitch.maxVal = _idMgr->count() - 1;
        idSwitch.step = 1;
        for (int i = 0; i < _idMgr->count(); i++) {
            auto& slot = _idMgr->identities()[i];
            static char labelBufs[8][32];
            if (!slot.displayName.isEmpty()) {
                snprintf(labelBufs[i], sizeof(labelBufs[i]), "%s [%.8s]",
                         slot.displayName.c_str(), slot.hash.c_str());
            } else {
                snprintf(labelBufs[i], sizeof(labelBufs[i]), "%.12s", slot.hash.c_str());
            }
            idSwitch.enumLabels.push_back(labelBufs[i]);
        }
        _items.push_back(idSwitch);
        idx++;
    }
    {
        SettingItem newId;
        newId.label = "Create Identity";
        newId.type = SettingType::ACTION;
        newId.formatter = [](int) { return String("[Enter]"); };
        newId.action = [this, &s]() {
            if (!_idMgr) { if (_ui) _ui->lvStatusBar().showToast("Not available", 1200); return; }
            if (_idMgr->count() >= 8) { if (_ui) _ui->lvStatusBar().showToast("Identity slots full", 1200); return; }
            int idx = _idMgr->createIdentity(s.displayName);
            if (idx >= 0) {
                if (_ui) _ui->lvStatusBar().showToast("Identity created", 1200);
                buildItems();
                rebuildCategoryList();
            }
        };
        _items.push_back(newId);
        idx++;
    }
    if (_sd && _sd->isReady()) {
        SettingItem importId;
        importId.label = "Import Identity";
        importId.type = SettingType::ACTION;
        importId.formatter = [](int) { return String("[Enter]"); };
        importId.action = [this, &s]() {
            if (!_sd || !_sd->isReady()) {
                if (_ui) _ui->lvStatusBar().showToast("No SD card", 1200);
                return;
            }
            if (!_sd->exists(SD_PATH_IMPORT_IDENTITY) && !_sd->exists(SD_PATH_IMPORT_ID)) {
                File dir = _sd->openDir(SD_PATH_IDENTITY_DIR);
                int identityFiles = 0;
                while (dir) {
                    File entry = dir.openNextFile();
                    if (!entry) break;
                    if (!entry.isDirectory()) {
                        String name = entry.name();
                        name.toLowerCase();
                        bool reservedIdentityFile = name == "identity.key" || name.endsWith("/identity.key") ||
                            name == "identity.identity" || name.endsWith("/identity.identity");
                        if (!reservedIdentityFile && (name.endsWith(".identity") || name.endsWith(".key"))) {
                            identityFiles++;
                        }
                    }
                    entry.close();
                }
                dir.close();
                if (identityFiles == 0) {
                    if (_ui) _ui->lvStatusBar().showToast("Missing identity file", 1200);
                    return;
                }
                if (identityFiles > 1) {
                    if (_ui) _ui->lvStatusBar().showToast("Rename one import.identity", 1500);
                    return;
                }
            }
            if (!_idMgr) {
                if (_ui) _ui->lvStatusBar().showToast("Not available", 1200);
                return;
            }
            if (_idMgr->count() >= 8) {
                if (_ui) _ui->lvStatusBar().showToast("Identity slots full", 1200);
                return;
            }
            int idx = _idMgr->importIdentity(s.displayName);
            if (idx >= 0) {
                if (_ui) _ui->lvStatusBar().showToast("Identity imported", 1200);
                buildItems();
                rebuildCategoryList();
            } else if (_ui) {
                _ui->lvStatusBar().showToast("Identity import failed", 1200);
            }
        };
        _items.push_back(importId);
        idx++;
    }
    _items.push_back({"Auto Announce", SettingType::INTEGER,
        [&s]() { return s.announceInterval; }, [&s](int v) { s.announceInterval = v; },
        [](int v) { return String(v) + "m"; }, 30, 60 * 6, 5}); // 30m - 6h
    idx++;
    _categories.push_back({"Identity & Device", devStart, idx - devStart,
        [&s]() -> String {
            String name = s.displayName.isEmpty() ? String("Unnamed device") : s.displayName;
            if (s.devMode) name += " / Dev Enabled";
            return name;
        }});

    // Screen & Input
    int dispStart = idx;
    _items.push_back({"Screen Brightness", SettingType::INTEGER,
        [&s]() { return s.brightness; }, [&s](int v) { s.brightness = v; },
        [](int v) { return String(v) + "%"; }, 5, 100, 5});
    idx++;
    {
        SettingItem themeItem;
        themeItem.label = "Theme";
        themeItem.type = SettingType::ENUM_CHOICE;
        themeItem.getter = [&s]() { return s.themeLight ? 1 : 0; };
        themeItem.setter = [&s](int v) { s.themeLight = (v != 0); };
        themeItem.minVal = 0; themeItem.maxVal = 1; themeItem.step = 1;
        themeItem.enumLabels = {"Dark", "Light"};
        _items.push_back(themeItem);
        idx++;
    }
    _items.push_back({"Dim After", SettingType::INTEGER,
        [&s]() { return s.screenDimTimeout; }, [&s](int v) { s.screenDimTimeout = v; },
        [](int v) { return String(v) + "s"; }, 5, 300, 5});
    idx++;
    _items.push_back({"Display Off After", SettingType::INTEGER,
        [&s]() { return s.screenOffTimeout; }, [&s](int v) { s.screenOffTimeout = v; },
        [](int v) { return String(v) + "s"; }, 10, 600, 10});
    idx++;
    _items.push_back({"Key Backlight", SettingType::INTEGER,
        [&s]() { return s.keyboardBrightness; }, [&s](int v) { s.keyboardBrightness = v; },
        [](int v) { return v <= 0 ? String("OFF") : String(v) + "%"; }, 0, 100, 5});
    idx++;
    _items.push_back({"Keys Wake Light", SettingType::TOGGLE,
        [&s]() { return s.keyboardAutoOn ? 1 : 0; },
        [&s](int v) { s.keyboardAutoOn = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"Keys Auto-Off", SettingType::TOGGLE,
        [&s]() { return s.keyboardAutoOff ? 1 : 0; },
        [&s](int v) { s.keyboardAutoOff = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"Trackball Speed", SettingType::INTEGER,
        [&s]() { return s.trackballSpeed; }, [&s](int v) { s.trackballSpeed = v; },
        [](int v) { return String(v); }, 1, 5, 1});
    idx++;
    _categories.push_back({"Screen & Input", dispStart, idx - dispStart,
        [&s]() -> String {
            String summary = String("Screen ") + String(s.brightness);
            summary += "% / Keys ";
            summary += s.keyboardBrightness == 0 ? String("OFF") : String(s.keyboardBrightness) + "%";
            return summary;
        }});

    // Battery
    int battStart = idx;
    _items.push_back({"Battery Display", SettingType::ENUM_CHOICE,
        [&s]() { return (int)s.batteryDisplay; },
        [&s](int v) { s.batteryDisplay = (uint8_t)v; },
        nullptr, BATTERY_DISPLAY_PERCENT, BATTERY_DISPLAY_BAR, 1, {"Percent", "Bar"}});
    idx++;

    _items.push_back({"Estimated Charge", SettingType::READONLY, nullptr, nullptr,
        [this](int) -> String {
            if (!_power) return String("--");
            return String(_power->batteryPercent()) + "%";
        }});
    idx++;

    if (s.devMode) {
        _items.push_back({"Discharge Curve", SettingType::ENUM_CHOICE,
            [&s]() { return (int)s.batteryModel; },
            [&s](int v) { s.batteryModel = (uint8_t)v; },
            nullptr, BATTERY_MODEL_LIPO, BATTERY_MODEL_LINEAR, 1, {"LiPo / Li-Ion", "Linear"}});
        idx++;

        _items.push_back({"Charge Above", SettingType::INTEGER,
            [&s]() { return (int)roundf(s.chargeThresholdV * 100); },
            [&s](int v) { s.chargeThresholdV = v / 100.0f; },
            [](int v) -> String {
                char buf[8]; snprintf(buf, sizeof(buf), "%.2fV", v / 100.0f); return String(buf);
            }, 380, 430, 1});
        idx++;
        _items.push_back({"Full Threshold", SettingType::INTEGER,
            [&s]() { return (int)roundf(s.fullBatteryV * 100); },
            [&s](int v) { s.fullBatteryV = v / 100.0f; },
            [](int v) -> String {
                char buf[8]; snprintf(buf, sizeof(buf), "%.2fV", v / 100.0f); return String(buf);
            }, 350, 420, 1});
        idx++;

        _items.push_back({"Voltage", SettingType::READONLY, nullptr, nullptr,
            [this](int) -> String {
                if (!_power) return String("--");
                char buf[12];
                snprintf(buf, sizeof(buf), "%.2fV", _power->batteryVoltage());
                return String(buf);
            }});
        idx++;
    }

    _categories.push_back({"Battery", battStart, idx - battStart,
        [this, &s]() -> String {
            String summary = s.batteryDisplay == BATTERY_DISPLAY_PERCENT ? String("Percent") : String("Bar");
            if (_power) {
                summary += " / ";
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", _power->batteryPercent());
                summary += buf;
            }
            return summary;
        }});

    // LoRa link
    int radioStart = idx;
    _items.push_back({"LoRa Radio", SettingType::TOGGLE,
        [&s]() { return s.loraEnabled ? 1 : 0; },
        [&s](int v) { s.loraEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    {
        // Region picker - always visible
        SettingItem regionItem;
        regionItem.label = "Region";
        regionItem.type = SettingType::ENUM_CHOICE;
        regionItem.getter = [&s]() { return (int)s.radioRegion; };
        regionItem.setter = [this, &s](int v) {
            int region = constrain(v, 0, REGION_COUNT - 1);
            s.radioRegion = region;
            s.loraFrequency = REGION_FREQ[region];
        };
        regionItem.minVal = 0; regionItem.maxVal = REGION_COUNT - 1; regionItem.step = 1;
        regionItem.enumLabels = {REGION_LABELS[0], REGION_LABELS[1], REGION_LABELS[2], REGION_LABELS[3]};
        _items.push_back(regionItem);
        idx++;

        SettingItem presetItem;
        presetItem.label = "Link Preset";
        presetItem.type = SettingType::ENUM_CHOICE;
        presetItem.getter = [this]() { int p = detectPreset(); return (p >= 0) ? p : LV_NUM_PRESETS; };
        presetItem.setter = [this](int v) { if (v >= 0 && v < LV_NUM_PRESETS) applyPreset(v); };
        presetItem.minVal = 0; presetItem.maxVal = LV_NUM_PRESETS - 1; presetItem.step = 1;
        presetItem.enumLabels = {};
        for (int i = 0; i < LV_NUM_PRESETS; i++)
            presetItem.enumLabels.push_back(LV_PRESETS[i].name);
        presetItem.enumLabels.push_back("Custom");
        _items.push_back(presetItem);
        idx++;
    }
    {
        SettingItem devModeItem;
        devModeItem.label = "Developer Radio Controls";
        devModeItem.type = SettingType::ACTION;
        devModeItem.formatter = [this, &s](int) {
            if (_confirmingDevMode) return String("Hold");
            return s.devMode ? String("Unlocked") : String("Locked");
        };
        devModeItem.action = [this, &s]() {
            if (s.devMode) {
                s.devMode = false;
                _confirmingDevMode = false;
                clearConfirmations();
                applyAndSave();
                buildItems();
                enterCategory(_categoryIdx);
                if (_ui) _ui->lvStatusBar().showToast("Developer radio controls locked", 1500);
                return;
            }
            if (!_confirmingDevMode) {
                clearConfirmations();
                _confirmingDevMode = true;
                if (_ui) _ui->lvStatusBar().showToast("RF controls armed. Hold to unlock", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold trackball to unlock RF controls", 2500);
        };
        _items.push_back(devModeItem);
        idx++;
    }
    // Custom radio parameters - only visible in Developer Mode
    if (s.devMode) {
        _items.push_back({"Frequency", SettingType::INTEGER,
            [&s]() { return (int)(s.loraFrequency); },
            [&s](int v) { s.loraFrequency = (uint32_t)v; },
            [](int v) -> String {
                char buf[20];
                int mhz = v / 1000000;
                int rem = v % 1000000;
                if (rem == 0) {
                    snprintf(buf, sizeof(buf), "%d MHz", mhz);
                } else {
                    char frac[8];
                    snprintf(frac, sizeof(frac), "%06d", rem);
                    int len = 6;
                    while (len > 0 && frac[len - 1] == '0') len--;
                    frac[len] = '\0';
                    snprintf(buf, sizeof(buf), "%d.%s MHz", mhz, frac);
                }
                return String(buf);
            },
            137000000, 1020000000, 125000});
        idx++;
        _items.push_back({"TX Power", SettingType::INTEGER,
            [&s]() { return s.loraTxPower; }, [&s](int v) { s.loraTxPower = v; },
            [](int v) { return String(v) + " dBm"; }, -9, 22, 1});
        idx++;
        _items.push_back({"Spread Factor", SettingType::INTEGER,
            [&s]() { return s.loraSF; }, [&s](int v) { s.loraSF = v; },
            [](int v) { return String("SF") + String(v); }, 5, 12, 1});
        idx++;
        _items.push_back({"Bandwidth", SettingType::ENUM_CHOICE,
            [&s]() {
                if (s.loraBW <= 41700)  return 0;
                if (s.loraBW <= 62500)  return 1;
                if (s.loraBW <= 125000) return 2;
                if (s.loraBW <= 250000) return 3;
                return 4;
            },
            [&s](int v) {
                static const uint32_t bws[] = {41700, 62500, 125000, 250000, 500000};
                s.loraBW = bws[constrain(v, 0, 4)];
            },
            nullptr, 0, 4, 1, {"41.7k", "62.5k", "125k", "250k", "500k"}});
        idx++;
        _items.push_back({"Coding Rate", SettingType::INTEGER,
            [&s]() { return s.loraCR; }, [&s](int v) { s.loraCR = v; },
            [](int v) { return String("4/") + String(v); }, 5, 8, 1});
        idx++;
        _items.push_back({"Preamble", SettingType::INTEGER,
            [&s]() { return (int)s.loraPreamble; }, [&s](int v) { s.loraPreamble = v; },
            [](int v) { return String(v); }, 6, 65, 1});
        idx++;
    }
    _categories.push_back({"LoRa", radioStart, idx - radioStart,
        [this]() {
            if (loraSettingsChanged()) {
                return String("Saved - reboot to apply");
            }
            int p = detectPreset();
            auto& s = _cfg->settings();
            if (!s.loraEnabled) return String("Off");
            String label = (p >= 0) ? String(LV_PRESETS[p].name) : String("Custom");
            label += " ";
            label += REGION_LABELS[constrain(s.radioRegion, 0, REGION_COUNT - 1)];
            if (s.devMode) label += " / Dev";
            return label;
        }});

    // Network
    int netStart = idx;
    _items.push_back({"WiFi Mode", SettingType::ENUM_CHOICE,
        [&s]() { return (int)s.wifiMode; },
        [&s](int v) {
            s.wifiMode = (RatWiFiMode)v;
            if (s.wifiMode != RAT_WIFI_OFF) s.wifiRestoreMode = s.wifiMode;
        },
        nullptr, 0, 2, 1, {"Off", "Hotspot", "Client"}});
    idx++;
    {
        SettingItem scanItem;
        scanItem.label = "Scan Networks";
        scanItem.type = SettingType::ACTION;
        scanItem.formatter = [](int) { return String("[Enter]"); };
        scanItem.action = [this, &s]() {
            _wifiTargetSlot = selectedWiFiSlot(s);
            showWifiPicker();
        };
        _items.push_back(scanItem);
        idx++;
    }
    _items.push_back({"WiFi Profile", SettingType::INTEGER,
        [&s]() { return (int)selectedWiFiSlot(s) + 1; },
        [&s](int v) { s.wifiSTASelected = (uint8_t)constrain(v - 1, 0, (int)WIFI_STA_MAX_NETWORKS - 1); },
        [&s](int v) { return wifiProfileValue(s, constrain(v - 1, 0, (int)WIFI_STA_MAX_NETWORKS - 1)); },
        1, (int)WIFI_STA_MAX_NETWORKS, 1});
    idx++;
    {
        SettingItem ssidItem;
        ssidItem.label = "WiFi SSID";
        ssidItem.type = SettingType::TEXT_INPUT;
        ssidItem.textGetter = [&s]() {
            size_t slot = selectedWiFiSlot(s);
            return slot < s.wifiSTANetworks.size() ? s.wifiSTANetworks[slot].ssid : String("");
        };
        ssidItem.textSetter = [&s](const String& v) {
            size_t slot = selectedWiFiSlot(s);
            ensureWiFiSlot(s, slot);
            if (slot >= s.wifiSTANetworks.size()) return;
            bool changed = s.wifiSTANetworks[slot].ssid != v;
            s.wifiSTANetworks[slot].ssid = v;
            if (changed || v.isEmpty()) s.wifiSTANetworks[slot].password = "";
        };
        ssidItem.maxTextLen = 32;
        _items.push_back(ssidItem);
        idx++;
    }
    {
        SettingItem passItem;
        passItem.label = "WiFi Password";
        passItem.type = SettingType::TEXT_INPUT;
        passItem.textGetter = [&s]() {
            size_t slot = selectedWiFiSlot(s);
            return slot < s.wifiSTANetworks.size() ? s.wifiSTANetworks[slot].password : String("");
        };
        passItem.textSetter = [&s](const String& v) {
            size_t slot = selectedWiFiSlot(s);
            ensureWiFiSlot(s, slot);
            if (slot < s.wifiSTANetworks.size()) s.wifiSTANetworks[slot].password = v;
        };
        passItem.maxTextLen = 64;
        _items.push_back(passItem);
        idx++;
    }
    {
        SettingItem forgetItem;
        forgetItem.label = "Forget Network";
        forgetItem.type = SettingType::ACTION;
        forgetItem.formatter = [&s](int) {
            String ssid = selectedWiFiSSID(s);
            return ssid.isEmpty() ? String("Empty") : String("[Enter]");
        };
        forgetItem.action = [this, &s]() {
            size_t slot = selectedWiFiSlot(s);
            if (slot >= s.wifiSTANetworks.size() || s.wifiSTANetworks[slot].ssid.isEmpty()) {
                if (_ui) _ui->lvStatusBar().showToast("WiFi profile already empty", 1200);
                return;
            }
            s.wifiSTANetworks[slot].ssid = "";
            s.wifiSTANetworks[slot].password = "";
            applyAndSave();
            if (_ui) _ui->lvStatusBar().showToast("WiFi profile cleared", 1200);
        };
        _items.push_back(forgetItem);
        idx++;
    }
    {
        SettingItem tcpPreset;
        tcpPreset.label = "TCP Server";
        tcpPreset.type = SettingType::ENUM_CHOICE;
        tcpPreset.getter = [&s]() {
            for (auto& ep : s.tcpConnections) {
                if (!ep.autoConnect || ep.host.isEmpty()) continue;
                if (ep.host == "rns.ratspeak.org") return 1;
                return 2;
            }
            return 0;
        };
        tcpPreset.setter = [&s](int v) {
            if (v == 0) {
                for (auto& ep : s.tcpConnections) ep.autoConnect = false;
            }
            else if (v == 1) {
                s.tcpConnections.clear();
                TCPEndpoint ep; ep.host = "rns.ratspeak.org"; ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else if (v == 2 && s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.port = TCP_DEFAULT_PORT; ep.autoConnect = false;
                s.tcpConnections.push_back(ep);
            }
        };
        tcpPreset.minVal = 0; tcpPreset.maxVal = 2; tcpPreset.step = 1;
        tcpPreset.enumLabels = {"None", "Ratspeak Hub", "Custom"};
        _items.push_back(tcpPreset);
        idx++;
    }
    {
        SettingItem tcpHost;
        tcpHost.label = "Host";
        tcpHost.type = SettingType::TEXT_INPUT;
        tcpHost.textGetter = [&s]() { return s.tcpConnections.empty() ? String("") : s.tcpConnections[0].host; };
        tcpHost.textSetter = [&s](const String& v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.host = v; ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else { s.tcpConnections[0].host = v; }
        };
        tcpHost.maxTextLen = 40;
        _items.push_back(tcpHost);
        idx++;
    }
    _items.push_back({"Port", SettingType::INTEGER,
        [&s]() { return s.tcpConnections.empty() ? TCP_DEFAULT_PORT : (int)s.tcpConnections[0].port; },
        [&s](int v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.port = v; ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else { s.tcpConnections[0].port = v; }
        },
        [](int v) { return String(v); }, 1, 65535, 1});
    idx++;
    _items.push_back({"LAN Discovery", SettingType::TOGGLE,
        [&s]() { return s.autoIfaceEnabled ? 1 : 0; },
        [&s](int v) { s.autoIfaceEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _categories.push_back({"Network", netStart, idx - netStart,
        [this, &s]() {
            if (interfaceSettingsChanged() || tcpSettingsChanged()) return String("Saved - reboot to apply");
            String summary = wifiModeLabel(s.wifiMode);
            if (s.wifiMode == RAT_WIFI_STA) {
                String ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : selectedWiFiSSID(s);
                summary += ": ";
                summary += ssid.isEmpty() ? String("No profile") : ssid;
            }
            if (!s.tcpConnections.empty()) summary += " + TCP";
            if (s.autoIfaceEnabled) summary += " + LAN";
            return summary;
        }});

    // Time & Location
    int gpsStart = idx;
#if HAS_GPS
    _items.push_back({"GPS Time Sync", SettingType::TOGGLE,
        [&s]() { return s.gpsTimeEnabled ? 1 : 0; },
        [&s](int v) { s.gpsTimeEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"GPS Location", SettingType::TOGGLE,
        [&s]() { return s.gpsLocationEnabled ? 1 : 0; },
        [&s](int v) { s.gpsLocationEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    // GPS Telemetry (issue #64) — opt-in position sharing to a hub. Off
    // by default for privacy. The hub hash persists; the manager reads it
    // fresh on every send so changes apply without a live callback.
    _items.push_back({"Send Telemetry", SettingType::TOGGLE,
        [&s]() { return s.gpsTelemetryEnabled ? 1 : 0; },
        [&s](int v) { s.gpsTelemetryEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    {
        // Pick a discovered/saved peer as the telemetry hub. Free-text hex
        // entry below still covers hubs not yet announced on the mesh.
        SettingItem selectHub;
        selectHub.label = "Select Hub";
        selectHub.type = SettingType::ACTION;
        selectHub.formatter = [this, &s](int) -> String {
            String cur = s.gpsTelemetryHubHash;
            if (cur.isEmpty()) return String("[Enter]");
            std::string hex(cur.c_str());
            String name;
            if (_am) {
                std::string nm = _am->lookupName(hex);
                if (!nm.empty()) name = String(nm.c_str());
            }
            if (!name.isEmpty()) {
                if ((int)name.length() > 10) name = name.substring(0, 10);
                return name + " " + cur.substring(0, 8);
            }
            return cur.substring(0, 8) + "...";
        };
        selectHub.action = [this]() { showPeerPicker(); };
        _items.push_back(selectHub);
        idx++;
    }
    {
        SettingItem hubItem;
        hubItem.label = "Telemetry Hub";
        hubItem.type = SettingType::TEXT_INPUT;
        hubItem.textGetter = [&s]() { return s.gpsTelemetryHubHash; };
        hubItem.textSetter = [&s](const String& v) { s.gpsTelemetryHubHash = v; };
        hubItem.maxTextLen = (int)UserSettings::GPS_TELEMETRY_HUB_HASH_LEN;
        _items.push_back(hubItem);
        idx++;
    }
    // Periodic telemetry interval (issue #64 "configurable by interval"
    // privacy requirement). 0 = on-demand only (default — periodic must
    // never start silently just because "Send Telemetry" is enabled; the
    // user must pick a non-zero interval explicitly). Non-zero values are
    // clamped at sanitizeSettings() to the LoRa channel-protection floor.
    _items.push_back({"Telemetry Interval (s)", SettingType::INTEGER,
        [&s]() { return s.gpsTelemetryIntervalS; },
        [&s](int v) { s.gpsTelemetryIntervalS = v; },
        [](int v) -> String {
            // 0 is the on-demand-only sentinel; present it as "Off" so the
            // user is never confused about whether periodic is running.
            if (v == 0) return String("Off");
            return String(v) + "s";
        },
        0, UserSettings::GPS_TELEMETRY_INTERVAL_MAX_S, 60});
    idx++;
#endif
    _items.push_back({"Timezone", SettingType::INTEGER,
        [&s]() { return (int)s.timezoneIdx; },
        [&s](int v) { s.timezoneIdx = (uint8_t)v; s.timezoneSet = true; },
        [](int v) {
            if (v >= 0 && v < TIMEZONE_COUNT) return String(TIMEZONE_TABLE[v].label);
            return String("Unknown");
        },
        0, TIMEZONE_COUNT - 1, 1});
    idx++;
    _items.push_back({"24-Hour Clock", SettingType::TOGGLE,
        [&s]() { return s.use24HourTime ? 1 : 0; },
        [&s](int v) { s.use24HourTime = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _categories.push_back({"Time & Location", gpsStart, idx - gpsStart,
        [&s]() {
            if (s.timezoneIdx < TIMEZONE_COUNT)
                return String(TIMEZONE_TABLE[s.timezoneIdx].label);
            return String("Not set");
        }});

    // Alerts & Audio
    int audioStart = idx;
    _items.push_back({"Audio Alerts", SettingType::TOGGLE,
        [&s]() { return s.audioEnabled ? 1 : 0; },
        [&s](int v) { s.audioEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"Volume", SettingType::INTEGER,
        [&s]() { return s.audioVolume; }, [&s](int v) { s.audioVolume = v; },
        [](int v) { return String(v) + "%"; }, 0, 100, 10});
    idx++;
    _categories.push_back({"Alerts & Audio", audioStart, idx - audioStart,
        [&s]() -> String {
            if (!s.audioEnabled) return String("Muted");
            String summary = String("Volume ") + String(s.audioVolume);
            summary += "%";
            return summary;
        }});

    // Diagnostics
    int infoStart = idx;
    _items.push_back({"RNS Transport", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns && _rns->isTransportActive() ? String("ACTIVE") : String("OFFLINE"); }});
    idx++;
    _items.push_back({"Known Paths", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns ? String((int)_rns->pathCount()) : String("0"); }});
    idx++;
    _items.push_back({"Live Links", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns ? String((int)_rns->linkCount()) : String("0"); }});
    idx++;
    _items.push_back({"LoRa Driver", SettingType::READONLY, nullptr, nullptr,
        [this](int) {
            if (_radio && _radio->isRadioOnline()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "SF%d BW%luk %ddBm",
                    _radio->getSpreadingFactor(),
                    (unsigned long)(_radio->getSignalBandwidth() / 1000),
                    _radio->getTxPower());
                return String(buf);
            }
            return String("OFFLINE");
        }});
    idx++;
    _items.push_back({"Heap", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"PSRAM", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreePsram() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Uptime", SettingType::READONLY, nullptr, nullptr,
        [](int) -> String {
            unsigned long m = millis() / 60000;
            if (m >= 60) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%luh %lum", m / 60, m % 60);
                return String(buf);
            }
            return String(m) + "m";
        }});
    idx++;
    _categories.push_back({"Diagnostics", infoStart, idx - infoStart,
        [this]() -> String {
            if (!_rns || !_rns->isTransportActive()) return String("Transport offline");
            String summary = String("Paths ") + String((int)_rns->pathCount());
            summary += " / Links ";
            summary += String((int)_rns->linkCount());
            return summary;
        }});

    // Storage & Maintenance
    int sysStart = idx;
    _items.push_back({"Free Heap", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Free PSRAM", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreePsram() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Flash", SettingType::READONLY, nullptr, nullptr,
        [this](int) {
            if (!_flash || !_flash->isReady()) return String("Error");
            char buf[24];
            snprintf(buf, sizeof(buf), "%lu/%lu KB",
                     (unsigned long)(_flash->usedBytes() / 1024),
                     (unsigned long)(_flash->totalBytes() / 1024));
            return String(buf);
        }});
    idx++;
    _items.push_back({"SD Card", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _sd && _sd->isReady() ? String("Ready") : String("Not Found"); }});
    idx++;
    _items.push_back({"SD Message Store", SettingType::TOGGLE,
        [&s]() { return s.sdStorageEnabled ? 1 : 0; },
        [&s](int v) { s.sdStorageEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    {
        SettingItem announceItem;
        announceItem.label = "Send Announce";
        announceItem.type = SettingType::ACTION;
        announceItem.formatter = [](int) { return String("[Enter]"); };
        announceItem.action = [this]() {
            if (_rns && _cfg) {
                RNS::Bytes appData = encodeAnnounceName(_cfg->settings().displayName);
                _rns->announce(appData);
                if (_ui) { _ui->lvStatusBar().flashAnnounce(); _ui->lvStatusBar().showToast("Announce sent"); }
            } else {
                if (_ui) _ui->lvStatusBar().showToast("RNS not ready");
            }
        };
        _items.push_back(announceItem);
        idx++;
    }
    {
        SettingItem showQrItem;
        showQrItem.label = "Show QR Code";
        showQrItem.type = SettingType::ACTION;
        showQrItem.formatter = [](int) { return String("[Enter]"); };
        showQrItem.action = [this]() {
            if (_showQrCb) _showQrCb();
            else if (_ui) _ui->lvStatusBar().showToast("QR not available");
        };
        _items.push_back(showQrItem);
        idx++;
    }
    {
        SettingItem initSD;
        initSD.label = "Format SD Card";
        initSD.type = SettingType::ACTION;
        initSD.formatter = [this](int) {
            if (!_sd || !_sd->isReady()) return String("No Card");
            return _confirmingInitSD ? String("Hold") : String("Arm");
        };
        initSD.action = [this]() {
            if (!_sd || !_sd->isReady()) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (!_confirmingInitSD) {
                clearConfirmations();
                _confirmingInitSD = true;
                if (_ui) _ui->lvStatusBar().showToast("Format SD armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold trackball to format SD", 2500);
        };
        _items.push_back(initSD);
        idx++;
    }
    {
        SettingItem wipeSD;
        wipeSD.label = "Erase rsDeck SD Data";
        wipeSD.type = SettingType::ACTION;
        wipeSD.formatter = [this](int) {
            if (!_sd || !_sd->isReady()) return String("No Card");
            return _confirmingWipeSD ? String("Hold") : String("Arm");
        };
        wipeSD.action = [this]() {
            if (!_sd || !_sd->isReady()) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (!_confirmingWipeSD) {
                clearConfirmations();
                _confirmingWipeSD = true;
                if (_ui) _ui->lvStatusBar().showToast("SD erase armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold trackball to erase SD data", 2500);
        };
        _items.push_back(wipeSD);
        idx++;
    }
    {
        SettingItem factoryReset;
        factoryReset.label = "Erase Device";
        factoryReset.type = SettingType::ACTION;
        factoryReset.formatter = [this](int) { return _confirmingReset ? String("Hold") : String("Arm"); };
        factoryReset.action = [this]() {
            if (!_confirmingReset) {
                clearConfirmations();
                _confirmingReset = true;
                if (_ui) _ui->lvStatusBar().showToast("Device erase armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold trackball to erase device", 2500);
        };
        _items.push_back(factoryReset);
        idx++;
    }
    {
        SettingItem rebootItem;
        rebootItem.label = "Reboot Now";
        rebootItem.type = SettingType::ACTION;
        rebootItem.formatter = [](int) { return String("[Enter]"); };
        rebootItem.action = [this]() {
            if (_ui) _ui->lvStatusBar().showToast("Rebooting...", 1500);
            delay(500);
            ESP.restart();
        };
        _items.push_back(rebootItem);
        idx++;
    }
    {
        SettingItem updateCheck;
        updateCheck.label = "Check Firmware";
        updateCheck.type = SettingType::ACTION;
        updateCheck.formatter = [this](int) {
            return _fwCheckState == FirmwareCheckState::RUNNING ? String("Checking") : String("[Enter]");
        };
        updateCheck.action = [this]() {
            startFirmwareCheck();
            rebuildItemList();
        };
        _items.push_back(updateCheck);
        idx++;
    }
    _categories.push_back({"Storage & Maintenance", sysStart, idx - sysStart,
        [this](){
            if (_rebootNeeded) return String("Reboot pending");
            return (_sd && _sd->isReady()) ? String("SD ready") : String("Flash only");
        }});
}

void LvSettingsScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    _scrollContainer = lv_obj_create(parent);
    lv_obj_set_size(_scrollContainer, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(_scrollContainer, 0, 0);
    lv_obj_set_style_bg_color(_scrollContainer, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_scrollContainer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scrollContainer, 0, 0);
    lv_obj_set_style_pad_all(_scrollContainer, 0, 0);
    lv_obj_set_style_pad_row(_scrollContainer, 0, 0);
    lv_obj_set_style_radius(_scrollContainer, 0, 0);
    lv_obj_set_layout(_scrollContainer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_scrollContainer, LV_FLEX_FLOW_COLUMN);
}

void LvSettingsScreen::onEnter() {
    buildItems();
    snapshotRebootSettings();
    snapshotTCPSettings();
    _rebootNeeded = false;
    _view = SettingsView::CATEGORY_LIST;
    _categoryIdx = 0;
    _selectedIdx = 0;
    _editing = false;
    _textEditing = false;
    _wifiScanActive = false;
    _wifiTargetSlot = 0;
    _peerPickerIndices.clear();
    _peerPickerIdx = 0;
    clearConfirmations();
    _kbBrightness = _cfg ? _cfg->settings().keyboardBrightness : 0;
    rebuildCategoryList();
}

void LvSettingsScreen::refreshUI() {
    FirmwareCheckState fwState = _fwCheckState;
    if (fwState != FirmwareCheckState::IDLE && fwState != FirmwareCheckState::RUNNING) {
        if (_ui) {
            if (fwState == FirmwareCheckState::AVAILABLE) {
                char msg[64];
                snprintf(msg, sizeof(msg), "v%s available at ratspeak.org", _fwCheckVersion);
                _ui->lvStatusBar().showToast(msg, 5000);
            } else if (fwState == FirmwareCheckState::CURRENT) {
                _ui->lvStatusBar().showToast("Firmware is current", 2000);
            } else {
                _ui->lvStatusBar().showToast("Couldn't fetch firmware", 2000);
            }
        }
        _fwCheckState = FirmwareCheckState::IDLE;
        if (_view == SettingsView::ITEM_LIST) rebuildItemList();
    }

    // Periodically refresh live readonly rows (Voltage, Estimated Charge)
    // while sitting on the item list, so they don't show a stale snapshot
    // from whenever the screen was last entered/edited. Skip while any
    // edit mode is active so we don't clobber an in-progress "< Bar >"-
    // style edit display.
    if (_view == SettingsView::ITEM_LIST && !_editing && !_textEditing && !_freqEditing) {
        unsigned long now = millis();
        if (now - _lastReadonlyRefresh >= 1000) {
            _lastReadonlyRefresh = now;
            bool hasReadonly = false;
            for (auto& item : _items) {
                if (item.type == SettingType::READONLY) { hasReadonly = true; break; }
            }
            if (hasReadonly) rebuildItemList();
        }
    }

    if (_view != SettingsView::WIFI_PICKER || !_wifiScanActive) return;
    if (!WiFiInterface::isScanComplete()) return;

    _wifiScanActive = false;
    _wifiResults = WiFiInterface::getScanResults();
    if (_wifiResults.empty() && _ui) {
        _ui->lvStatusBar().showToast("No networks found", 1500);
    }
    rebuildWifiList();
}

void LvSettingsScreen::showWifiPicker() {
    _wifiResults.clear();
    _wifiPickerIdx = 0;
    _wifiScanActive = true;
    WiFiInterface::startAsyncScan();
    _view = SettingsView::WIFI_PICKER;
    rebuildWifiList();
}

void LvSettingsScreen::rebuildCategoryList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Title
    lv_obj_t* titleRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(titleRow, Theme::CONTENT_W, 26);
    lv_obj_set_style_bg_color(titleRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(titleRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(titleRow, 1, 0);
    lv_obj_set_style_border_side(titleRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(titleRow, 0, 0);
    lv_obj_set_style_radius(titleRow, 0, 0);
    lv_obj_clear_flag(titleRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* titleLbl = lv_label_create(titleRow);
    lv_obj_set_style_text_font(titleLbl, font, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(titleLbl, "SETTINGS DECK");
    lv_obj_align(titleLbl, LV_ALIGN_LEFT_MID, 8, 0);

    if (_rebootNeeded) {
        lv_obj_t* pendingLbl = lv_label_create(titleRow);
        lv_obj_set_style_text_font(pendingLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(pendingLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_label_set_text(pendingLbl, "REBOOT PENDING");
        lv_obj_align(pendingLbl, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    for (int i = 0; i < (int)_categories.size(); i++) {
        auto& cat = _categories[i];
        bool selected = (i == _categoryIdx);
        bool pending = categoryNeedsReboot(i);

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 40);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? Theme::BG_HOVER : Theme::BG), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::BORDER), 0);
        lv_obj_set_style_border_width(row, pending ? 2 : 1, 0);
        lv_obj_set_style_border_side(row, pending ? (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM) : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->_categoryIdx = idx;
            self->enterCategory(idx);
        }, LV_EVENT_CLICKED, this);
        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        // Category name
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        clipLabel(nameLbl, Theme::CONTENT_W - 72);
        lv_label_set_text(nameLbl, cat.name);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 12, 4);

        char countBuf[8];
        snprintf(countBuf, sizeof(countBuf), "%d", cat.count);
        lv_obj_t* countLbl = lv_label_create(row);
        lv_obj_set_style_text_font(countLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(countLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(countLbl, countBuf);
        lv_obj_align(countLbl, LV_ALIGN_TOP_RIGHT, -24, 5);

        // Summary
        if (cat.summary) {
            lv_obj_t* sumLbl = lv_label_create(row);
            lv_obj_set_style_text_font(sumLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(sumLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_MUTED), 0);
            clipLabel(sumLbl, Theme::CONTENT_W - 44);
            lv_label_set_text(sumLbl, cat.summary().c_str());
            lv_obj_align(sumLbl, LV_ALIGN_BOTTOM_LEFT, 20, -4);
        }

        // Arrow
        lv_obj_t* arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : (selected ? Theme::ACCENT : Theme::TEXT_MUTED)), 0);
        lv_label_set_text(arrow, pending ? "!" : ">");
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);

        _rowObjs.push_back(row);
    }

    // Restore focus to current category
    if (_categoryIdx >= 0 && _categoryIdx < (int)_rowObjs.size()) {
        lv_group_focus_obj(_rowObjs[_categoryIdx]);
    }
}

void LvSettingsScreen::rebuildItemList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    _editValueLbl = nullptr;  // Invalidate cached label before destroying widgets
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Category header
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 24);
    lv_obj_set_style_bg_color(headerRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(headerRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(headerRow, [](lv_event_t* e) {
        auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
        self->exitToCategories();
    }, LV_EVENT_CLICKED, this);

    char headerBuf[48];
    snprintf(headerBuf, sizeof(headerBuf), "< %s", _categories[_categoryIdx].name);
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    clipLabel(headerLbl, Theme::CONTENT_W - 128);
    lv_label_set_text(headerLbl, headerBuf);
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 8, 0);

    if (_rebootNeeded) {
        lv_obj_t* pendingLbl = lv_label_create(headerRow);
        lv_obj_set_style_text_font(pendingLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(pendingLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_label_set_text(pendingLbl, "REBOOT NEEDED");
        lv_obj_align(pendingLbl, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    if (_rebootNeeded) {
        lv_obj_t* notice = lv_obj_create(_scrollContainer);
        lv_obj_set_size(notice, Theme::CONTENT_W, 20);
        lv_obj_set_style_bg_color(notice, lv_color_hex(Theme::PRIMARY_SUBTLE), 0);
        lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(notice, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_obj_set_style_border_width(notice, 1, 0);
        lv_obj_set_style_border_side(notice, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(notice, 0, 0);
        lv_obj_set_style_radius(notice, 0, 0);
        lv_obj_clear_flag(notice, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* noticeLbl = lv_label_create(notice);
        lv_obj_set_style_text_font(noticeLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(noticeLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        clipLabel(noticeLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(noticeLbl, "Saved interface config is pending reboot");
        lv_obj_align(noticeLbl, LV_ALIGN_LEFT_MID, 8, 0);
    }

    const char* confirmTitle = confirmationTitle();
    const char* confirmDetail = confirmationDetail();
    if (confirmTitle && confirmDetail) {
        uint32_t confirmColor = _confirmingDevMode ? Theme::WARNING_CLR : Theme::ERROR_CLR;
        lv_obj_t* confirm = lv_obj_create(_scrollContainer);
        lv_obj_set_size(confirm, Theme::CONTENT_W, 34);
        lv_obj_set_style_bg_color(confirm, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(confirm, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(confirm, lv_color_hex(confirmColor), 0);
        lv_obj_set_style_border_width(confirm, 2, 0);
        lv_obj_set_style_border_side(confirm, (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM), 0);
        lv_obj_set_style_pad_all(confirm, 0, 0);
        lv_obj_set_style_radius(confirm, 0, 0);
        lv_obj_clear_flag(confirm, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* confirmTitleLbl = lv_label_create(confirm);
        lv_obj_set_style_text_font(confirmTitleLbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(confirmTitleLbl, lv_color_hex(confirmColor), 0);
        clipLabel(confirmTitleLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(confirmTitleLbl, confirmTitle);
        lv_obj_align(confirmTitleLbl, LV_ALIGN_TOP_LEFT, 8, 3);

        lv_obj_t* confirmDetailLbl = lv_label_create(confirm);
        lv_obj_set_style_text_font(confirmDetailLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(confirmDetailLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        clipLabel(confirmDetailLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(confirmDetailLbl, confirmDetail);
        lv_obj_align(confirmDetailLbl, LV_ALIGN_BOTTOM_LEFT, 8, -3);
    }

    for (int i = _catRangeStart; i < _catRangeEnd; i++) {
        const auto& item = _items[i];
        bool selected = (i == _selectedIdx);
        bool editable = isEditable(i);
        bool rebootPending = settingNeedsReboot(item);
        bool armed = armedAction(item);
        bool destructive = destructiveAction(item);
        uint32_t armedColor = destructive ? Theme::ERROR_CLR : Theme::WARNING_CLR;

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 28);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        if (editable) {
            lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        }
        lv_obj_set_style_bg_color(row, lv_color_hex(armed ? Theme::BG_SURFACE : (selected && editable ? Theme::BG_HOVER : Theme::BG)), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(armed ? armedColor : (rebootPending ? Theme::WARNING_CLR : Theme::BORDER)), 0);
        lv_obj_set_style_border_width(row, rebootPending || armed ? 2 : 1, 0);
        lv_obj_set_style_border_side(row, rebootPending || armed ? (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM) : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (editable) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, (void*)(intptr_t)i);
            lv_obj_add_event_cb(row, [](lv_event_t* e) {
                auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
                if (self->_editing || self->_textEditing || self->_freqEditing) {
                    self->_editing = false;
                    self->_textEditing = false;
                    self->_freqEditing = false;
                    self->_numericTyping = false;
                }
                self->_selectedIdx = idx;
                KeyEvent tap = {};
                tap.enter = true;
                self->handleKey(tap);
            }, LV_EVENT_CLICKED, this);
            lv_group_add_obj(LvInput::group(), row);
            lv_obj_add_event_cb(row, [](lv_event_t* e) {
                lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
            }, LV_EVENT_FOCUSED, nullptr);
        }

        // Label
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        uint32_t nameColor =
            armed ? armedColor :
            rebootPending ? Theme::WARNING_CLR :
            destructive ? Theme::ERROR_CLR :
            item.type == SettingType::ACTION ? Theme::TEXT_PRIMARY :
            item.type == SettingType::READONLY ? Theme::TEXT_MUTED : Theme::TEXT_SECONDARY;
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(nameColor), 0);
        clipLabel(nameLbl, Theme::CONTENT_W - 136);
        lv_label_set_text(nameLbl, item.label);
        lv_obj_align(nameLbl, LV_ALIGN_LEFT_MID, 8, 0);

        // Value
        String valStr;
        uint32_t valColor = Theme::PRIMARY;

        if (_freqEditing && selected) {
            valStr = String("< ") + freqFormatWithCursor() + " >";
            valColor = Theme::WARNING_CLR;
        } else if (_editing && selected) {
            if (item.type == SettingType::ENUM_CHOICE && !item.enumLabels.empty()) {
                int vi = constrain(_editValue, 0, (int)item.enumLabels.size() - 1);
                valStr = String("< ") + item.enumLabels[vi] + " >";
            } else if (item.formatter) {
                valStr = String("< ") + item.formatter(_editValue) + " >";
            } else {
                valStr = String("< ") + String(_editValue) + " >";
            }
            valColor = Theme::WARNING_CLR;
        } else if (_textEditing && selected) {
            valStr = _editText + "_";
            valColor = Theme::WARNING_CLR;
        } else {
            switch (item.type) {
                case SettingType::READONLY:
                    valStr = item.formatter ? item.formatter(0) : "";
                    valColor = Theme::TEXT_MUTED;
                    break;
                case SettingType::TEXT_INPUT: {
                    String v = item.textGetter ? item.textGetter() : "";
                    valStr = v.isEmpty() ? "(not set)" : (isWiFiPasswordLabel(item.label) ? maskedValue(v) : v);
                    valColor = v.isEmpty() ? Theme::TEXT_MUTED : Theme::PRIMARY;
                    break;
                }
                case SettingType::ENUM_CHOICE:
                    if (!item.enumLabels.empty()) {
                        int vi = item.getter ? constrain(item.getter(), 0, (int)item.enumLabels.size() - 1) : 0;
                        valStr = item.enumLabels[vi];
                    }
                    break;
                case SettingType::ACTION:
                    valStr = item.formatter ? item.formatter(0) : "";
                    valColor = destructive ? Theme::ERROR_CLR : Theme::TEXT_MUTED;
                    break;
                default: {
                    int v = item.getter ? item.getter() : 0;
                    valStr = item.formatter ? item.formatter(v) : String(v);
                    break;
                }
            }
        }

        if (armed) {
            valColor = armedColor;
        } else if (rebootPending) {
            valColor = Theme::WARNING_CLR;
        }

        if (!valStr.isEmpty()) {
            lv_obj_t* valLbl = lv_label_create(row);
            lv_obj_set_style_text_font(valLbl, font, 0);
            lv_obj_set_style_text_color(valLbl, lv_color_hex(valColor), 0);
            lv_obj_set_style_text_align(valLbl, LV_TEXT_ALIGN_RIGHT, 0);
            clipLabel(valLbl, 124);
            lv_label_set_text(valLbl, valStr.c_str());
            lv_obj_align(valLbl, LV_ALIGN_RIGHT_MID, -8, 0);
            // Cache value label for the actively edited item (in-place updates)
            if (i == _selectedIdx && (_textEditing || _freqEditing || _editing)) {
                _editValueLbl = valLbl;
            }
        }

        _rowObjs.push_back(row);
    }

    // Restore focus to the currently selected item after rebuild
    int focusOffset = _selectedIdx - _catRangeStart;
    if (focusOffset >= 0 && focusOffset < (int)_rowObjs.size()) {
        lv_group_focus_obj(_rowObjs[focusOffset]);
    }
}

void LvSettingsScreen::selectWifiResult(int resultIdx) {
    if (!_cfg || resultIdx < 0 || resultIdx >= (int)_wifiResults.size()) return;

    auto& net = _wifiResults[resultIdx];
    auto& nets = _cfg->settings().wifiSTANetworks;
    while (nets.size() <= _wifiTargetSlot && nets.size() < WIFI_STA_MAX_NETWORKS) {
        nets.push_back({});
    }
    if (_wifiTargetSlot >= WIFI_STA_MAX_NETWORKS || _wifiTargetSlot >= nets.size()) {
        if (_ui) _ui->lvStatusBar().showToast("Network slots full", 1500);
        return;
    }

    for (size_t i = 0; i < nets.size(); i++) {
        if (i != _wifiTargetSlot && !nets[i].ssid.isEmpty() && nets[i].ssid == net.ssid) {
            if (_ui) _ui->lvStatusBar().showToast("Already saved", 1200);
            return;
        }
    }

    bool sameSSID = nets[_wifiTargetSlot].ssid == net.ssid;
    nets[_wifiTargetSlot].ssid = net.ssid;
    if (!sameSSID) nets[_wifiTargetSlot].password = "";
    _cfg->settings().wifiSTASelected = (uint8_t)_wifiTargetSlot;
    applyAndSave();
}

void LvSettingsScreen::rebuildWifiList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Header
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 22);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    char header[48];
    snprintf(header, sizeof(header), "< Scan for profile %u", (unsigned)(_wifiTargetSlot + 1));
    lv_label_set_text(headerLbl, header);
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 4, 0);

    // Make header tappable to go back
    lv_obj_add_flag(headerRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(headerRow, [](lv_event_t* e) {
        auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
        self->_wifiScanActive = false;
        self->_view = SettingsView::ITEM_LIST;
        self->rebuildItemList();
    }, LV_EVENT_CLICKED, this);

    if (_wifiResults.empty()) {
        lv_obj_t* emptyLbl = lv_label_create(_scrollContainer);
        lv_obj_set_style_text_font(emptyLbl, font, 0);
        lv_obj_set_style_text_color(emptyLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(emptyLbl, _wifiScanActive ? "Scanning..." : "No networks found");
        return;
    }

    for (int i = 0; i < (int)_wifiResults.size(); i++) {
        auto& net = _wifiResults[i];

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 22);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->selectWifiResult(idx);
            self->_wifiScanActive = false;
            self->_view = SettingsView::ITEM_LIST;
            self->rebuildItemList();
        }, LV_EVENT_CLICKED, this);
        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        // Lock + SSID
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", net.encrypted ? "*" : " ", net.ssid.c_str());
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Signal
        char sigBuf[12];
        snprintf(sigBuf, sizeof(sigBuf), "%ddBm", net.rssi);
        lv_obj_t* sigLbl = lv_label_create(row);
        lv_obj_set_style_text_font(sigLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(sigLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(sigLbl, sigBuf);
        lv_obj_align(sigLbl, LV_ALIGN_RIGHT_MID, -4, 0);

        _rowObjs.push_back(row);
    }
}

void LvSettingsScreen::showPeerPicker() {
    _peerPickerIndices.clear();
    _peerPickerIdx = 0;
    if (!_am) {
        if (_ui) _ui->lvStatusBar().showToast("No peers seen yet", 1500);
        return;
    }
    const auto& nodes = _am->nodes();
    if (nodes.empty()) {
        if (_ui) _ui->lvStatusBar().showToast("No peers seen yet", 1500);
        return;
    }
    // Saved contacts first (in their original order), then transient peers.
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (nodes[i].saved) _peerPickerIndices.push_back(i);
    }
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (!nodes[i].saved) _peerPickerIndices.push_back(i);
    }
    if ((int)_peerPickerIndices.size() > PEER_PICKER_CAP) {
        _peerPickerIndices.resize(PEER_PICKER_CAP);
    }
    _view = SettingsView::PEER_PICKER;
    rebuildPeerList();
}

void LvSettingsScreen::rebuildPeerList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Header (tappable to go back)
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 22);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(headerLbl, "< Select Telemetry Hub");
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_flag(headerRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(headerRow, [](lv_event_t* e) {
        auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
        self->_view = SettingsView::ITEM_LIST;
        self->rebuildItemList();
    }, LV_EVENT_CLICKED, this);

    if (_peerPickerIndices.empty()) {
        lv_obj_t* emptyLbl = lv_label_create(_scrollContainer);
        lv_obj_set_style_text_font(emptyLbl, font, 0);
        lv_obj_set_style_text_color(emptyLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(emptyLbl, "No peers seen yet");
        return;
    }

    for (int i = 0; i < (int)_peerPickerIndices.size(); i++) {
        int nodeIdx = _peerPickerIndices[i];
        const auto& node = _am->nodes()[nodeIdx];
        std::string hex = node.hash.toHex();

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 28);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->selectPeerResult(idx);
        }, LV_EVENT_CLICKED, this);
        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        // Primary line: name (if known) or short hash
        std::string name = node.name;
        if (name.empty()) name = _am->lookupName(hex);
        char line[80];
        if (!name.empty()) {
            // Truncate name to fit alongside the hash suffix on one line
            int nlen = (int)name.size();
            if (nlen > 22) {
                snprintf(line, sizeof(line), "%.22s %s...", name.c_str(), hex.substr(0, 8).c_str());
            } else {
                snprintf(line, sizeof(line), "%s %s...", name.c_str(), hex.substr(0, 8).c_str());
            }
        } else {
            snprintf(line, sizeof(line), "%s...", hex.substr(0, 8).c_str());
        }
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        clipLabel(lbl, Theme::CONTENT_W - 12);
        lv_label_set_text(lbl, line);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Saved-contact star marker
        if (node.saved) {
            lv_obj_t* star = lv_label_create(row);
            lv_obj_set_style_text_font(star, font, 0);
            lv_obj_set_style_text_color(star, lv_color_hex(Theme::ACCENT), 0);
            lv_label_set_text(star, "*");
            lv_obj_align(star, LV_ALIGN_RIGHT_MID, -4, 0);
        }

        _rowObjs.push_back(row);
    }

    if (_peerPickerIdx >= 0 && _peerPickerIdx < (int)_rowObjs.size()) {
        lv_group_focus_obj(_rowObjs[_peerPickerIdx]);
    }
}

void LvSettingsScreen::selectPeerResult(int listIdx) {
    if (!_cfg || !_am) return;
    if (listIdx < 0 || listIdx >= (int)_peerPickerIndices.size()) return;
    const auto& node = _am->nodes()[_peerPickerIndices[listIdx]];
    std::string hex = node.hash.toHex();
    if (hex.size() != (size_t)UserSettings::GPS_TELEMETRY_HUB_HASH_LEN) {
        if (_ui) _ui->lvStatusBar().showToast("Invalid peer hash", 1500);
        return;
    }
    // Save in memory; persist on the normal settings-save path (matches
    // WiFi picker behavior — no flash write until the user backs out).
    _cfg->settings().gpsTelemetryHubHash = String(hex.c_str());
    String toast;
    std::string name = node.name;
    if (name.empty()) name = _am->lookupName(hex);
    if (!name.empty()) {
        toast = String("Hub set: ") + String(name.c_str());
    } else {
        toast = String("Hub set: ") + String(hex.substr(0, 8).c_str()) + "...";
    }
    if (_ui) _ui->lvStatusBar().showToast(toast.c_str(), 1500);
    _view = SettingsView::ITEM_LIST;
    rebuildItemList();
}

void LvSettingsScreen::updateCategorySelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        bool pending = categoryNeedsReboot(oldIdx);
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[oldIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_MUTED), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        bool pending = categoryNeedsReboot(newIdx);
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::BG_HOVER), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[newIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::PRIMARY), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateItemSelection(int oldIdx, int newIdx) {
    // _rowObjs maps directly to items (header row is NOT in _rowObjs)
    int oldRow = oldIdx - _catRangeStart;
    int newRow = newIdx - _catRangeStart;
    auto setItemRowBg = [&](int row, bool selected) {
        if (row < 0 || row >= (int)_rowObjs.size()) return;
        int itemIdx = row + _catRangeStart;
        bool editable = isEditable(itemIdx);
        const auto& item = _items[itemIdx];
        bool armed = armedAction(item);
        lv_obj_set_style_bg_color(_rowObjs[row], lv_color_hex(
            armed ? Theme::BG_SURFACE : ((selected && editable) ? Theme::BG_HOVER : Theme::BG)), 0);
    };
    setItemRowBg(oldRow, false);
    setItemRowBg(newRow, true);
    if (newRow >= 0 && newRow < (int)_rowObjs.size()) {
        lv_obj_scroll_to_view(_rowObjs[newRow], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateWifiSelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::BG_HOVER), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updatePeerSelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::BG_HOVER), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::enterCategory(int catIdx) {
    if (catIdx < 0 || catIdx >= (int)_categories.size()) return;
    _categoryIdx = catIdx;
    auto& cat = _categories[catIdx];
    _catRangeStart = cat.startIdx;
    _catRangeEnd = cat.startIdx + cat.count;
    _selectedIdx = _catRangeStart;
    _editing = false;
    _textEditing = false;
    if (!isEditable(_selectedIdx)) skipToNextEditable(1);
    _view = SettingsView::ITEM_LIST;
    rebuildItemList();
}

void LvSettingsScreen::exitToCategories() {
    _view = SettingsView::CATEGORY_LIST;
    _editing = false;
    _textEditing = false;
    clearConfirmations();
    rebuildCategoryList();
}

bool LvSettingsScreen::handleKey(const KeyEvent& event) {
    switch (_view) {
        case SettingsView::CATEGORY_LIST: {
            // LVGL focus group handles up/down navigation
            if (event.enter || event.character == '\n' || event.character == '\r') {
                // Get focused category from LVGL group
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) _categoryIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
                enterCategory(_categoryIdx);
                return true;
            }
            return false;
        }

        case SettingsView::ITEM_LIST: {
            if (_items.empty()) return false;

            // Text edit mode - in-place label updates for responsiveness
            if (_textEditing) {
                auto& item = _items[_selectedIdx];
                auto updateEditLabel = [this, &item]() {
                    if (_editValueLbl) {
                        String display = _editText + "_";
                        lv_label_set_text(_editValueLbl, display.c_str());
                        lv_obj_align(_editValueLbl, LV_ALIGN_RIGHT_MID, -8, 0);
                    }
                };
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    if (item.textSetter) item.textSetter(_editText);
                    _textEditing = false;
                    _editValueLbl = nullptr;
                    applyAndSave();
                    rebuildItemList();
                    return true;
                }
                if (event.del || event.character == 8) {
                    if (_editText.length() > 0) { _editText.remove(_editText.length() - 1); updateEditLabel(); }
                    return true;
                }
                if (event.character == 0x1B) { _textEditing = false; _editValueLbl = nullptr; rebuildItemList(); return true; }
                if (event.character >= 0x20 && event.character <= 0x7E && (int)_editText.length() < item.maxTextLen) {
                    _editText += (char)event.character; updateEditLabel(); return true;
                }
                return true;
            }

            // Frequency digit-cursor edit mode - in-place label updates
            if (_freqEditing) {
                auto updateFreqLabel = [this]() {
                    if (_editValueLbl) {
                        String display = String("< ") + freqFormatWithCursor() + " >";
                        lv_label_set_text(_editValueLbl, display.c_str());
                        lv_obj_align(_editValueLbl, LV_ALIGN_RIGHT_MID, -8, 0);
                    }
                };
                if (event.left) {
                    if (_freqCursor > 0) _freqCursor--;
                    updateFreqLabel(); return true;
                }
                if (event.right) {
                    if (_freqCursor < 8) _freqCursor++;
                    updateFreqLabel(); return true;
                }
                if (event.character >= '0' && event.character <= '9') {
                    _freqDigits[_freqCursor] = event.character - '0';
                    _editValue = freqRecompose();
                    if (_freqCursor < 8) _freqCursor++;
                    updateFreqLabel(); return true;
                }
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    auto& item = _items[_selectedIdx];
                    _editValue = freqRecompose();
                    if (item.setter) item.setter(_editValue);
                    _freqEditing = false; _editing = false;
                    _editValueLbl = nullptr;
                    applyAndSave(); rebuildItemList(); return true;
                }
                if (event.del || event.character == 8) {
                    if (_freqCursor > 0) _freqCursor--;
                    updateFreqLabel(); return true;
                }
                if (event.character == 0x1B) {
                    _editValue = _freqOriginal;
                    _freqEditing = false; _editing = false;
                    _editValueLbl = nullptr;
                    rebuildItemList(); return true;
                }
                return true;
            }

            // Value edit mode
            if (_editing) {
                auto& item = _items[_selectedIdx];
                // Direct digit entry for INTEGER fields
                if (event.character >= '0' && event.character <= '9') {
                    int digit = event.character - '0';
                    if (!_numericTyping) {
                        _editValue = digit;
                        _numericTyping = true;
                    } else {
                        int newVal = _editValue * 10 + digit;
                        if (newVal <= item.maxVal) _editValue = newVal;
                    }
                    rebuildItemList(); return true;
                }
                if (event.left) {
                    _editValue -= item.step;
                    if (_editValue < item.minVal) _editValue = item.minVal;
                    _numericTyping = false;
                    rebuildItemList(); return true;
                }
                if (event.right) {
                    _editValue += item.step;
                    if (_editValue > item.maxVal) _editValue = item.maxVal;
                    _numericTyping = false;
                    rebuildItemList(); return true;
                }
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    if (_editValue < item.minVal) _editValue = item.minVal;
                    if (item.setter) item.setter(_editValue);
                    _editing = false;
                    _numericTyping = false;
                    applyAndSave();
                    rebuildItemList(); return true;
                }
                if (event.del || event.character == 8) {
                    if (_numericTyping && _editValue > 0) {
                        _editValue /= 10;
                    } else {
                        _editing = false;
                        _numericTyping = false;
                    }
                    rebuildItemList(); return true;
                }
                if (event.character == 0x1B) {
                    _editing = false; _numericTyping = false; rebuildItemList(); return true;
                }
                return true;
            }

            // Browse mode - LVGL focus group handles up/down
            if (hasPendingConfirmation() && (event.up || event.down || event.left || event.right || event.tab)) {
                clearConfirmations();
                rebuildItemList();
                if (_ui) _ui->lvStatusBar().showToast("Confirmation cancelled", 1000);
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                if (hasPendingConfirmation()) {
                    clearConfirmations();
                    rebuildItemList();
                    if (_ui) _ui->lvStatusBar().showToast("Confirmation cancelled", 1000);
                    return true;
                }
                exitToCategories(); return true;
            }
            if (event.enter || event.character == '\n' || event.character == '\r') {
                // Sync _selectedIdx from LVGL focus
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) _selectedIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
                if (!isEditable(_selectedIdx)) return true;
                auto& item = _items[_selectedIdx];
                if (item.type == SettingType::ACTION) {
                    if (!confirmableAction(item)) clearConfirmations();
                    if (item.action) item.action();
                    if (_view == SettingsView::ITEM_LIST) rebuildItemList();
                } else if (item.type == SettingType::TEXT_INPUT) {
                    clearConfirmations();
                    _textEditing = true;
                    _editText = item.textGetter ? item.textGetter() : "";
                    rebuildItemList();
                } else if (item.type == SettingType::TOGGLE) {
                    clearConfirmations();
                    int val = item.getter ? item.getter() : 0;
                    if (item.setter) item.setter(val ? 0 : 1);
                    applyAndSave();
                    rebuildItemList();
                } else if (strcmp(item.label, "Frequency") == 0 && item.type == SettingType::INTEGER) {
                    clearConfirmations();
                    // Radio-style digit cursor editor for frequency
                    _editing = true;
                    _editValue = item.getter ? item.getter() : 0;
                    _freqOriginal = _editValue;
                    freqDecompose(_editValue);
                    _freqCursor = 0;
                    _freqEditing = true;
                    rebuildItemList();
                } else {
                    clearConfirmations();
                    _editing = true;
                    _numericTyping = false;
                    _editValue = item.getter ? item.getter() : 0;
                    rebuildItemList();
                }
                return true;
            }
            return false;
        }

        case SettingsView::WIFI_PICKER: {
            // LVGL handles up/down navigation, click handler handles selection
            if (event.enter || event.character == '\n' || event.character == '\r') {
                if (_wifiScanActive) return true;
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) {
                    int idx = (int)(intptr_t)lv_obj_get_user_data(focused);
                    if (idx < (int)_wifiResults.size()) {
                        selectWifiResult(idx);
                    }
                }
                _wifiScanActive = false;
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                _wifiScanActive = false;
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            return false;
        }

        case SettingsView::PEER_PICKER: {
            if (event.enter || event.character == '\n' || event.character == '\r') {
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) {
                    int idx = (int)(intptr_t)lv_obj_get_user_data(focused);
                    if (idx >= 0 && idx < (int)_peerPickerIndices.size()) {
                        selectPeerResult(idx);
                        return true;
                    }
                }
                // No focused row — bail out to item list
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            return false;
        }
    }
    return false;
}

bool LvSettingsScreen::handleLongPress() {
    if (_view != SettingsView::ITEM_LIST || !hasPendingConfirmation()) return false;

    int focusedIdx = _selectedIdx;
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (focused) {
        int candidate = (int)(intptr_t)lv_obj_get_user_data(focused);
        if (candidate >= 0 && candidate < (int)_items.size()) focusedIdx = candidate;
    }
    if (focusedIdx < 0 || focusedIdx >= (int)_items.size()) return false;

    const auto& item = _items[focusedIdx];
    if (_confirmingInitSD && labelEq(item.label, "Format SD Card")) {
        runFormatSD();
        return true;
    }
    if (_confirmingWipeSD && labelEq(item.label, "Erase rsDeck SD Data")) {
        runWipeSD();
        return true;
    }
    if (_confirmingReset && labelEq(item.label, "Erase Device")) {
        runFactoryReset();
        return true;
    }
    if (_confirmingDevMode && labelEq(item.label, "Developer Radio Controls")) {
        runEnableDevMode();
        return true;
    }

    if (_ui) _ui->lvStatusBar().showToast("Select armed action, then hold", 2000);
    return true;
}

void LvSettingsScreen::snapshotRebootSettings() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    _rebootSnap.wifiMode = s.wifiMode;
    _rebootSnap.wifiSTANetworks = s.wifiSTANetworks;
    _rebootSnap.wifiSTASelected = s.wifiSTASelected;
    _rebootSnap.autoIfaceEnabled = s.autoIfaceEnabled;
    _rebootSnap.sdStorageEnabled = s.sdStorageEnabled;
    _rebootSnap.loraEnabled = s.loraEnabled;
    _gpsSnapEnabled = s.gpsTimeEnabled;
}

bool LvSettingsScreen::rebootSettingsChanged() const {
    return loraSettingsChanged() || interfaceSettingsChanged()
        || storageSettingsChanged() || tcpSettingsChanged();
}

bool LvSettingsScreen::loraSettingsChanged() const {
    if (!_cfg) return false;
    return _cfg->settings().loraEnabled != _rebootSnap.loraEnabled;
}

bool LvSettingsScreen::interfaceSettingsChanged() const {
    if (!_cfg) return false;
    const auto& s = _cfg->settings();
    if (s.wifiMode != _rebootSnap.wifiMode) return true;
    if (s.wifiSTASelected != _rebootSnap.wifiSTASelected) return true;
    if (s.autoIfaceEnabled != _rebootSnap.autoIfaceEnabled) return true;
    if (s.wifiSTANetworks.size() != _rebootSnap.wifiSTANetworks.size()) return true;
    for (size_t i = 0; i < s.wifiSTANetworks.size(); i++) {
        if (s.wifiSTANetworks[i].ssid != _rebootSnap.wifiSTANetworks[i].ssid) return true;
        if (s.wifiSTANetworks[i].password != _rebootSnap.wifiSTANetworks[i].password) return true;
    }
    return false;
}

bool LvSettingsScreen::storageSettingsChanged() const {
    if (!_cfg) return false;
    return _cfg->settings().sdStorageEnabled != _rebootSnap.sdStorageEnabled;
}

void LvSettingsScreen::snapshotTCPSettings() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    _tcpSnapHost = s.tcpConnections.empty() ? "" : s.tcpConnections[0].host;
    _tcpSnapPort = s.tcpConnections.empty() ? 0 : s.tcpConnections[0].port;
    _tcpSnapAuto = !s.tcpConnections.empty() && s.tcpConnections[0].autoConnect;
}

bool LvSettingsScreen::tcpSettingsChanged() const {
    if (!_cfg) return false;
    auto& s = _cfg->settings();
    String curHost = s.tcpConnections.empty() ? "" : s.tcpConnections[0].host;
    uint16_t curPort = s.tcpConnections.empty() ? 0 : s.tcpConnections[0].port;
    bool curAuto = !s.tcpConnections.empty() && s.tcpConnections[0].autoConnect;
    return curHost != _tcpSnapHost || curPort != _tcpSnapPort || curAuto != _tcpSnapAuto;
}

// --- Frequency digit-cursor editor helpers ---

void LvSettingsScreen::freqDecompose(int value) {
    // Decompose Hz value into 9 individual digits (left-padded with zeros)
    for (int i = 8; i >= 0; i--) {
        _freqDigits[i] = value % 10;
        value /= 10;
    }
}

int LvSettingsScreen::freqRecompose() const {
    int val = 0;
    for (int i = 0; i < 9; i++) val = val * 10 + _freqDigits[i];
    return val;
}

String LvSettingsScreen::freqFormatWithCursor() const {
    // Format as "NNN.NNN.NNN" with brackets around cursor digit
    char buf[24];
    char digits[9];
    for (int i = 0; i < 9; i++) digits[i] = '0' + _freqDigits[i];

    // Build string with cursor brackets: e.g., "920.[6]50.500"
    int pos = 0;
    for (int i = 0; i < 9; i++) {
        if (i == 3 || i == 6) buf[pos++] = '.';
        if (i == _freqCursor) {
            buf[pos++] = '[';
            buf[pos++] = digits[i];
            buf[pos++] = ']';
        } else {
            buf[pos++] = digits[i];
        }
    }
    buf[pos] = '\0';
    return String(buf);
}

void LvSettingsScreen::applyAndSave() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    // Theme switch applies live: palette globals -> shared styles -> shell.
    // Our own rows re-read Theme:: on the rebuild that follows every commit.
    Theme::Scheme want = s.themeLight ? Theme::Scheme::LIGHT : Theme::Scheme::DARK;
    if (want != Theme::scheme()) {
        Theme::setScheme(want);
        if (_ui) _ui->applyTheme();
        if (_screen) lv_obj_set_style_bg_color(_screen, lv_color_hex(Theme::BG), 0);
        if (_scrollContainer) lv_obj_set_style_bg_color(_scrollContainer, lv_color_hex(Theme::BG), 0);
    }
    if (_power) {

        // Battery
        _power->setBatteryModel(s.batteryModel);
        _power->setChargeThreshold(s.chargeThresholdV);
        _power->setFullBatteryVoltage(s.fullBatteryV);

        _power->setBrightness(s.brightness);
        _power->setDimTimeout(s.screenDimTimeout);
        _power->setOffTimeout(s.screenOffTimeout);
        if (_kbBrightness != s.keyboardBrightness) {
            _power->setKbBrightness(s.keyboardBrightness, true);
            _kbBrightness = s.keyboardBrightness;
        }
        _power->setKbAutoOn(s.keyboardAutoOn);
        _power->setKbAutoOff(s.keyboardAutoOff);
    }
    if (_radio && _radio->isRadioOnline()) {
        if (s.loraEnabled) {
            _radio->setFrequency(s.loraFrequency);
            _radio->setTxPower(s.loraTxPower);
            _radio->setSpreadingFactor(s.loraSF);
            _radio->setSignalBandwidth(s.loraBW);
            _radio->setCodingRate4(s.loraCR);
            _radio->setPreambleLength(s.loraPreamble);
            _radio->receive();
        } else {
            _radio->sleep();
        }
    }
    if (_audio) {
        _audio->setEnabled(s.audioEnabled);
        _audio->setVolume(s.audioVolume);
    }

    // Detect TCP server change before saving
    bool tcpChanged = tcpSettingsChanged();

    bool saved = false;
    if (_saveCallback) { saved = _saveCallback(); }
    else if (_sd && _flash) { saved = _cfg->save(*_sd, *_flash); }
    else if (_flash) { saved = _cfg->save(*_flash); }

    // TCP server changes are persisted only. Recreating clients live can race
    // in-flight sockets/netif teardown on ESP32; reboot applies them cleanly.

    // Apply GPS toggle live (start/stop GPS UART)
    if (s.gpsTimeEnabled != _gpsSnapEnabled) {
        _gpsSnapEnabled = s.gpsTimeEnabled;
        if (_gpsChangeCb) _gpsChangeCb(s.gpsTimeEnabled);
    }

    // Check if reboot-required settings changed (show toast only on first detection)
    bool wasRebootNeeded = _rebootNeeded;
    _rebootNeeded = rebootSettingsChanged();

    if (_ui) {
        if (!saved) {
            _ui->lvStatusBar().showToast("Save failed", 2000);
        } else if (_rebootNeeded && !wasRebootNeeded) {
            _ui->lvStatusBar().showToast(
                tcpChanged ? "TCP server saved; reboot to apply" : "Interface changes saved; reboot to apply",
                3000);
        } else if (!_rebootNeeded && wasRebootNeeded) {
            _ui->lvStatusBar().showToast("Pending reboot cleared", 1500);
        } else if (tcpChanged) {
            _ui->lvStatusBar().showToast("TCP server saved; reboot to apply", 3000);
        } else {
            _ui->lvStatusBar().showToast("Saved", 800);
        }
    }
}
