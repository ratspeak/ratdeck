#include "LvSettingsScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "config/Config.h"
#include "config/UserConfig.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "radio/SX1262.h"
#include "audio/AudioNotify.h"
#include "hal/Power.h"
#include "transport/WiFiInterface.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/IdentityManager.h"
#include <Arduino.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

struct RadioPresetLv {
    const char* name;
    uint8_t sf; uint32_t bw; uint8_t cr; int8_t txPower; long preamble;
};
static const RadioPresetLv LV_PRESETS[] = {
    {"Balanced",   9,  125000, 5,  17, 18},
    {"Long Range", 12,  62500, 8,  22, 18},
    {"Fast",       7,  250000, 5,  14, 18},
};
static constexpr int LV_NUM_PRESETS = 3;

int LvSettingsScreen::detectPreset() const {
    if (!_cfg) return -1;
    auto& s = _cfg->settings();
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

// buildItems() — identical logic to SettingsScreen::buildItems()
void LvSettingsScreen::buildItems() {
    _items.clear();
    _categories.clear();
    if (!_cfg) return;
    auto& s = _cfg->settings();
    int idx = 0;

    // Device
    int devStart = idx;
    _items.push_back({"Version", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String(RATDECK_VERSION_STRING); }});
    idx++;
    _items.push_back({"LXMF Addr", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _destinationHash.length() > 0 ? _destinationHash.substring(0, 16) : String("unknown"); }});
    idx++;
    _items.push_back({"Identity", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _identityHash.substring(0, 16); }});
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
        idSwitch.label = "Active Identity";
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
                if (_ui) _ui->lvStatusBar().showToast("Identity switched! Rebooting...", 2000);
                applyAndSave();
                delay(1000);
                ESP.restart();
            } else {
                if (_ui) _ui->lvStatusBar().showToast("Switch failed!", 1500);
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
        newId.label = "New Identity";
        newId.type = SettingType::ACTION;
        newId.formatter = [](int) { return String("[Enter]"); };
        newId.action = [this, &s]() {
            if (!_idMgr) { if (_ui) _ui->lvStatusBar().showToast("Not available", 1200); return; }
            if (_idMgr->count() >= 8) { if (_ui) _ui->lvStatusBar().showToast("Max 8 identities!", 1200); return; }
            int idx = _idMgr->createIdentity(s.displayName);
            if (idx >= 0) {
                if (_ui) _ui->lvStatusBar().showToast("Identity created!", 1200);
                buildItems();
                rebuildCategoryList();
            }
        };
        _items.push_back(newId);
        idx++;
    }
    _items.push_back({"Announce Interval", SettingType::INTEGER,
        [&s]() { return s.announceInterval; }, [&s](int v) { s.announceInterval = v; },
        [](int v) { return String(v) + "m"; }, 5, 60 * 6, 5}); // 5m - 6h
    idx++;
    {
        SettingItem devModeItem;
        devModeItem.label = "Developer Mode";
        devModeItem.type = SettingType::ACTION;
        devModeItem.formatter = [&s](int) { return s.devMode ? String("ON") : String("OFF"); };
        devModeItem.action = [this, &s]() {
            if (s.devMode) {
                // Already on — just turn it off
                s.devMode = false;
                _confirmingDevMode = false;
                applyAndSave();
                buildItems();
                enterCategory(_categoryIdx);
                if (_ui) _ui->lvStatusBar().showToast("Developer mode OFF", 1200);
                return;
            }
            if (!_confirmingDevMode) {
                _confirmingDevMode = true;
                if (_ui) _ui->lvStatusBar().showToast("WARNING: KNOW YOUR LAWS! Enter=Enable", 5000);
                rebuildItemList();
                return;
            }
            // Confirmed
            _confirmingDevMode = false;
            s.devMode = true;
            applyAndSave();
            buildItems();
            enterCategory(_categoryIdx);
            if (_ui) _ui->lvStatusBar().showToast("Developer mode ON", 1200);
        };
        _items.push_back(devModeItem);
        idx++;
    }
    _categories.push_back({"Device", devStart, idx - devStart,
        [&s]() { return s.displayName.isEmpty() ? String("(unnamed)") : s.displayName; }});

    // Display & Input
    int dispStart = idx;
    _items.push_back({"Brightness", SettingType::INTEGER,
        [&s]() { return s.brightness; }, [&s](int v) { s.brightness = v; },
        [](int v) { return String(v) + "%"; }, 5, 100, 5});
    idx++;
    _items.push_back({"Dim Timeout", SettingType::INTEGER,
        [&s]() { return s.screenDimTimeout; }, [&s](int v) { s.screenDimTimeout = v; },
        [](int v) { return String(v) + "s"; }, 5, 300, 5});
    idx++;
    _items.push_back({"Off Timeout", SettingType::INTEGER,
        [&s]() { return s.screenOffTimeout; }, [&s](int v) { s.screenOffTimeout = v; },
        [](int v) { return String(v) + "s"; }, 10, 600, 10});
    idx++;
    _items.push_back({"Trackball Speed", SettingType::INTEGER,
        [&s]() { return s.trackballSpeed; }, [&s](int v) { s.trackballSpeed = v; },
        [](int v) { return String(v); }, 1, 5, 1});
    idx++;
    _categories.push_back({"Display & Input", dispStart, idx - dispStart,
        [&s]() { return String(s.brightness) + "%"; }});

    // Radio
    int radioStart = idx;
    {
        SettingItem presetItem;
        presetItem.label = "Preset";
        presetItem.type = SettingType::ENUM_CHOICE;
        presetItem.getter = [this]() { int p = detectPreset(); return (p >= 0) ? p : LV_NUM_PRESETS; };
        presetItem.setter = [this](int v) { if (v >= 0 && v < LV_NUM_PRESETS) applyPreset(v); };
        presetItem.minVal = 0; presetItem.maxVal = LV_NUM_PRESETS; presetItem.step = 1;
        presetItem.enumLabels = {"Balanced", "Long Range", "Fast", "Custom"};
        _items.push_back(presetItem);
        idx++;
    }
    // Custom radio parameters — only visible in Developer Mode
    if (s.devMode) {
        _items.push_back({"Frequency", SettingType::INTEGER,
            [&s]() { return (int)(s.loraFrequency / 1000); },
            [&s](int v) { s.loraFrequency = (uint32_t)v * 1000; },
            [](int v) -> String {
                char buf[16]; snprintf(buf, sizeof(buf), "%d.%03d MHz", v / 1000, v % 1000);
                return String(buf);
            },
            137000, 1020000, 125});
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
                if (s.loraBW <= 62500)  return 0;
                if (s.loraBW <= 125000) return 1;
                if (s.loraBW <= 250000) return 2;
                return 3;
            },
            [&s](int v) {
                static const uint32_t bws[] = {62500, 125000, 250000, 500000};
                s.loraBW = bws[constrain(v, 0, 3)];
            },
            nullptr, 0, 3, 1, {"62.5k", "125k", "250k", "500k"}});
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
    _categories.push_back({"Radio", radioStart, idx - radioStart,
        [this]() { int p = detectPreset(); return (p >= 0) ? String(LV_PRESETS[p].name) : String("Custom"); }});

    // Network
    int netStart = idx;
    _items.push_back({"WiFi Mode", SettingType::ENUM_CHOICE,
        [&s]() { return (int)s.wifiMode; },
        [&s](int v) { s.wifiMode = (RatWiFiMode)v; },
        nullptr, 0, 2, 1, {"OFF", "AP", "STA"}});
    idx++;
    {
        SettingItem ssidItem;
        ssidItem.label = "WiFi SSID";
        ssidItem.type = SettingType::TEXT_INPUT;
        ssidItem.textGetter = [&s]() { return s.wifiSTASSID; };
        ssidItem.textSetter = [&s](const String& v) { s.wifiSTASSID = v; };
        ssidItem.maxTextLen = 32;
        _items.push_back(ssidItem);
        idx++;
    }
    {
        SettingItem passItem;
        passItem.label = "WiFi Password";
        passItem.type = SettingType::TEXT_INPUT;
        passItem.textGetter = [&s]() { return s.wifiSTAPassword; };
        passItem.textSetter = [&s](const String& v) { s.wifiSTAPassword = v; };
        passItem.maxTextLen = 32;
        _items.push_back(passItem);
        idx++;
    }
    {
        SettingItem tcpPreset;
        tcpPreset.label = "TCP Server";
        tcpPreset.type = SettingType::ENUM_CHOICE;
        tcpPreset.getter = [&s]() {
            for (auto& ep : s.tcpConnections) { if (ep.host == "rns.ratspeak.org") return 1; }
            if (!s.tcpConnections.empty()) return 2;
            return 0;
        };
        tcpPreset.setter = [&s](int v) {
            if (v == 0) { s.tcpConnections.clear(); }
            else if (v == 1) {
                s.tcpConnections.clear();
                TCPEndpoint ep; ep.host = "rns.ratspeak.org"; ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
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
        tcpHost.label = "TCP Host";
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
    _items.push_back({"TCP Port", SettingType::INTEGER,
        [&s]() { return s.tcpConnections.empty() ? TCP_DEFAULT_PORT : (int)s.tcpConnections[0].port; },
        [&s](int v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.port = v; ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else { s.tcpConnections[0].port = v; }
        },
        [](int v) { return String(v); }, 1, 65535, 1});
    idx++;
    _items.push_back({"BLE", SettingType::TOGGLE,
        [&s]() { return s.bleEnabled ? 1 : 0; },
        [&s](int v) { s.bleEnabled = (v != 0); },
        [](int v) { return v ? String("ON") : String("OFF"); }});
    idx++;
    _categories.push_back({"Network", netStart, idx - netStart,
        [&s]() {
            const char* modes[] = {"OFF", "AP", "STA"};
            return String(modes[constrain((int)s.wifiMode, 0, 2)]);
        }});

    // Audio
    int audioStart = idx;
    _items.push_back({"Audio", SettingType::TOGGLE,
        [&s]() { return s.audioEnabled ? 1 : 0; },
        [&s](int v) { s.audioEnabled = (v != 0); },
        [](int v) { return v ? String("ON") : String("OFF"); }});
    idx++;
    _items.push_back({"Volume", SettingType::INTEGER,
        [&s]() { return s.audioVolume; }, [&s](int v) { s.audioVolume = v; },
        [](int v) { return String(v) + "%"; }, 0, 100, 10});
    idx++;
    _categories.push_back({"Audio", audioStart, idx - audioStart,
        [&s]() { return s.audioEnabled ? (String(s.audioVolume) + "%") : String("OFF"); }});

    // Info (diagnostics moved from Home screen)
    int infoStart = idx;
    _items.push_back({"Transport", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns && _rns->isTransportActive() ? String("ACTIVE") : String("OFFLINE"); }});
    idx++;
    _items.push_back({"Paths", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns ? String((int)_rns->pathCount()) : String("0"); }});
    idx++;
    _items.push_back({"Links", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _rns ? String((int)_rns->linkCount()) : String("0"); }});
    idx++;
    _items.push_back({"Radio", SettingType::READONLY, nullptr, nullptr,
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
    _categories.push_back({"Info", infoStart, idx - infoStart,
        [this]() { return _rns && _rns->isTransportActive() ? String("ACTIVE") : String("OFFLINE"); }});

    // System
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
    {
        SettingItem announceItem;
        announceItem.label = "Send Announce";
        announceItem.type = SettingType::ACTION;
        announceItem.formatter = [](int) { return String("[Enter]"); };
        announceItem.action = [this]() {
            if (_rns && _cfg) {
                RNS::Bytes appData = encodeAnnounceName(_cfg->settings().displayName);
                _rns->announce(appData);
                if (_ui) { _ui->lvStatusBar().flashAnnounce(); _ui->lvStatusBar().showToast("Announce sent!"); }
            } else {
                if (_ui) _ui->lvStatusBar().showToast("RNS not ready");
            }
        };
        _items.push_back(announceItem);
        idx++;
    }
    {
        SettingItem initSD;
        initSD.label = "Init SD Card";
        initSD.type = SettingType::ACTION;
        initSD.formatter = [this](int) { return (_sd && _sd->isReady()) ? String("[Enter]") : String("No Card"); };
        initSD.action = [this]() {
            if (!_sd || !_sd->isReady()) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (_ui) _ui->lvStatusBar().showToast("Initializing SD...", 2000);
            bool ok = _sd->formatForRatputer();
            if (_ui) _ui->lvStatusBar().showToast(ok ? "SD initialized!" : "SD init failed!", 1500);
        };
        _items.push_back(initSD);
        idx++;
    }
    {
        SettingItem wipeSD;
        wipeSD.label = "Wipe SD Data";
        wipeSD.type = SettingType::ACTION;
        wipeSD.formatter = [this](int) { return (_sd && _sd->isReady()) ? String("[Enter]") : String("No Card"); };
        wipeSD.action = [this]() {
            if (!_sd || !_sd->isReady()) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (_ui) _ui->lvStatusBar().showToast("Wiping SD data...", 2000);
            bool ok = _sd->wipeRatputer();
            if (_ui) _ui->lvStatusBar().showToast(ok ? "SD wiped & reinit!" : "Wipe failed!", 1500);
        };
        _items.push_back(wipeSD);
        idx++;
    }
    {
        SettingItem factoryReset;
        factoryReset.label = "Factory Reset";
        factoryReset.type = SettingType::ACTION;
        factoryReset.formatter = [this](int) { return _confirmingReset ? String("[Confirm?]") : String("[Enter]"); };
        factoryReset.action = [this]() {
            if (!_confirmingReset) {
                _confirmingReset = true;
                if (_ui) _ui->lvStatusBar().showToast("Press again to confirm!", 2000);
                rebuildItemList();
                return;
            }
            _confirmingReset = false;
            if (_ui) _ui->lvStatusBar().showToast("Factory resetting...", 3000);
            if (_sd && _sd->isReady()) _sd->wipeRatputer();
            if (_flash) _flash->format();
            nvs_flash_erase();
            delay(500);
            ESP.restart();
        };
        _items.push_back(factoryReset);
        idx++;
    }
    {
        SettingItem rebootItem;
        rebootItem.label = "Reboot Device";
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
        updateCheck.label = "Check for Updates";
        updateCheck.type = SettingType::ACTION;
        updateCheck.formatter = [](int) { return String("[Enter]"); };
        updateCheck.action = [this]() {
            if (WiFi.status() != WL_CONNECTED) {
                if (_ui) _ui->lvStatusBar().showToast("Connect to WiFi to check!", 2000);
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Checking for updates...", 3000);

            HTTPClient http;
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            http.setTimeout(8000);
            http.begin("https://api.github.com/repos/ratspeak/ratdeck/releases/latest");
            http.addHeader("Accept", "application/vnd.github.v3+json");
            int httpCode = http.GET();

            if (httpCode != 200) {
                http.end();
                if (_ui) _ui->lvStatusBar().showToast("Couldn't fetch data!", 2000);
                return;
            }

            String payload = http.getString();
            http.end();

            JsonDocument doc;
            if (deserializeJson(doc, payload)) {
                if (_ui) _ui->lvStatusBar().showToast("Couldn't fetch data!", 2000);
                return;
            }

            const char* tagName = doc["tag_name"] | "";
            // Strip leading 'v' if present
            const char* remoteVer = tagName;
            if (remoteVer[0] == 'v' || remoteVer[0] == 'V') remoteVer++;

            if (strlen(remoteVer) == 0) {
                if (_ui) _ui->lvStatusBar().showToast("Couldn't fetch data!", 2000);
                return;
            }

            // Compare versions (simple string compare works for semver)
            int cmp = strcmp(remoteVer, RATDECK_VERSION_STRING);
            if (cmp > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "v%s available! Flash at ratspeak.org", remoteVer);
                if (_ui) _ui->lvStatusBar().showToast(msg, 5000);
            } else {
                if (_ui) _ui->lvStatusBar().showToast("You're up to date!", 2000);
            }
        };
        _items.push_back(updateCheck);
        idx++;
    }
    _categories.push_back({"System", sysStart, idx - sysStart,
        [](){ return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB free"; }});
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
    _confirmingReset = false;
    _confirmingDevMode = false;
    rebuildCategoryList();
}

void LvSettingsScreen::rebuildCategoryList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_montserrat_12;

    // Title
    lv_obj_t* titleRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(titleRow, Theme::CONTENT_W, 24);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(titleRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(titleRow, 1, 0);
    lv_obj_set_style_border_side(titleRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(titleRow, 0, 0);
    lv_obj_set_style_radius(titleRow, 0, 0);
    lv_obj_clear_flag(titleRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* titleLbl = lv_label_create(titleRow);
    lv_obj_set_style_text_font(titleLbl, font, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(titleLbl, "SETTINGS");
    lv_obj_align(titleLbl, LV_ALIGN_LEFT_MID, 4, 0);

    for (int i = 0; i < (int)_categories.size(); i++) {
        auto& cat = _categories[i];
        bool selected = (i == _categoryIdx);

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 34);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Category name + count
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%d)", cat.name, cat.count);
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(selected ? Theme::ACCENT : Theme::PRIMARY), 0);
        lv_label_set_text(nameLbl, buf);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 12, 4);

        // Summary
        if (cat.summary) {
            lv_obj_t* sumLbl = lv_label_create(row);
            lv_obj_set_style_text_font(sumLbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(sumLbl, lv_color_hex(Theme::MUTED), 0);
            lv_label_set_text(sumLbl, cat.summary().c_str());
            lv_obj_align(sumLbl, LV_ALIGN_BOTTOM_LEFT, 20, -2);
        }

        // Arrow
        lv_obj_t* arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(selected ? Theme::ACCENT : Theme::MUTED), 0);
        lv_label_set_text(arrow, ">");
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);

        _rowObjs.push_back(row);
    }
}

void LvSettingsScreen::rebuildItemList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_montserrat_12;

    // Category header
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 22);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    char headerBuf[48];
    snprintf(headerBuf, sizeof(headerBuf), "< %s", _categories[_categoryIdx].name);
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(headerLbl, headerBuf);
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 4, 0);

    for (int i = _catRangeStart; i < _catRangeEnd; i++) {
        const auto& item = _items[i];
        bool selected = (i == _selectedIdx);
        bool editable = isEditable(i);

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 22);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            (selected && editable) ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Label
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(
            item.type == SettingType::ACTION ? (selected ? Theme::ACCENT : Theme::PRIMARY) : Theme::SECONDARY), 0);
        lv_label_set_text(nameLbl, item.label);
        lv_obj_align(nameLbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Value
        String valStr;
        uint32_t valColor = Theme::PRIMARY;

        if (_editing && selected) {
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
                    valColor = Theme::MUTED;
                    break;
                case SettingType::TEXT_INPUT: {
                    String v = item.textGetter ? item.textGetter() : "";
                    valStr = v.isEmpty() ? "(not set)" : v;
                    valColor = v.isEmpty() ? Theme::MUTED : Theme::PRIMARY;
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
                    valColor = Theme::MUTED;
                    break;
                default: {
                    int v = item.getter ? item.getter() : 0;
                    valStr = item.formatter ? item.formatter(v) : String(v);
                    break;
                }
            }
        }

        if (!valStr.isEmpty()) {
            lv_obj_t* valLbl = lv_label_create(row);
            lv_obj_set_style_text_font(valLbl, font, 0);
            lv_obj_set_style_text_color(valLbl, lv_color_hex(valColor), 0);
            lv_label_set_text(valLbl, valStr.c_str());
            lv_obj_align(valLbl, LV_ALIGN_RIGHT_MID, -4, 0);
        }

        _rowObjs.push_back(row);
    }
}

void LvSettingsScreen::rebuildWifiList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_montserrat_12;

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
    lv_label_set_text(headerLbl, "< Select WiFi Network");
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 4, 0);

    if (_wifiResults.empty()) {
        lv_obj_t* emptyLbl = lv_label_create(_scrollContainer);
        lv_obj_set_style_text_font(emptyLbl, font, 0);
        lv_obj_set_style_text_color(emptyLbl, lv_color_hex(Theme::MUTED), 0);
        lv_label_set_text(emptyLbl, "No networks found");
        return;
    }

    for (int i = 0; i < (int)_wifiResults.size(); i++) {
        auto& net = _wifiResults[i];
        bool selected = (i == _wifiPickerIdx);

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 22);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Lock + SSID
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", net.encrypted ? "*" : " ", net.ssid.c_str());
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(selected ? Theme::ACCENT : Theme::PRIMARY), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Signal
        char sigBuf[12];
        snprintf(sigBuf, sizeof(sigBuf), "%ddBm", net.rssi);
        lv_obj_t* sigLbl = lv_label_create(row);
        lv_obj_set_style_text_font(sigLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(sigLbl, lv_color_hex(Theme::MUTED), 0);
        lv_label_set_text(sigLbl, sigBuf);
        lv_obj_align(sigLbl, LV_ALIGN_RIGHT_MID, -4, 0);

        _rowObjs.push_back(row);
    }
}

void LvSettingsScreen::updateCategorySelection(int oldIdx, int newIdx) {
    // _rowObjs maps directly to category indices (title row is NOT in _rowObjs)
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[oldIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(Theme::MUTED), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::SELECTION_BG), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(Theme::ACCENT), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[newIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(Theme::ACCENT), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateItemSelection(int oldIdx, int newIdx) {
    // _rowObjs maps directly to items (header row is NOT in _rowObjs)
    int oldRow = oldIdx - _catRangeStart;
    int newRow = newIdx - _catRangeStart;
    auto setItemRowBg = [&](int row, bool selected) {
        if (row < 0 || row >= (int)_rowObjs.size()) return;
        bool editable = isEditable(row + _catRangeStart);
        lv_obj_set_style_bg_color(_rowObjs[row], lv_color_hex(
            (selected && editable) ? Theme::SELECTION_BG : Theme::BG), 0);
    };
    setItemRowBg(oldRow, false);
    setItemRowBg(newRow, true);
    if (newRow >= 0 && newRow < (int)_rowObjs.size()) {
        lv_obj_scroll_to_view(_rowObjs[newRow], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateWifiSelection(int oldIdx, int newIdx) {
    // _rowObjs maps directly to wifi items (header row is NOT in _rowObjs)
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::SELECTION_BG), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::ACCENT), 0);
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
    _confirmingReset = false;
    _confirmingDevMode = false;
    rebuildCategoryList();
}

bool LvSettingsScreen::handleKey(const KeyEvent& event) {
    switch (_view) {
        case SettingsView::CATEGORY_LIST: {
            if (event.up) {
                if (_categoryIdx > 0) {
                    int prev = _categoryIdx;
                    _categoryIdx--;
                    updateCategorySelection(prev, _categoryIdx);
                }
                return true;
            }
            if (event.down) {
                if (_categoryIdx < (int)_categories.size() - 1) {
                    int prev = _categoryIdx;
                    _categoryIdx++;
                    updateCategorySelection(prev, _categoryIdx);
                }
                return true;
            }
            if (event.enter || event.character == '\n' || event.character == '\r') {
                enterCategory(_categoryIdx);
                return true;
            }
            return false;
        }

        case SettingsView::ITEM_LIST: {
            if (_items.empty()) return false;

            // Text edit mode
            if (_textEditing) {
                auto& item = _items[_selectedIdx];
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    if (item.textSetter) item.textSetter(_editText);
                    _textEditing = false;
                    applyAndSave();
                    rebuildItemList();
                    return true;
                }
                if (event.del || event.character == 8) {
                    if (_editText.length() > 0) { _editText.remove(_editText.length() - 1); rebuildItemList(); }
                    return true;
                }
                if (event.character == 0x1B) { _textEditing = false; rebuildItemList(); return true; }
                if (event.character >= 0x20 && event.character <= 0x7E && (int)_editText.length() < item.maxTextLen) {
                    _editText += (char)event.character; rebuildItemList(); return true;
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

            // Browse mode
            if (event.up) {
                int prev = _selectedIdx;
                skipToNextEditable(-1);
                if (_selectedIdx != prev) updateItemSelection(prev, _selectedIdx);
                return true;
            }
            if (event.down) {
                int prev = _selectedIdx;
                skipToNextEditable(1);
                if (_selectedIdx != prev) updateItemSelection(prev, _selectedIdx);
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                exitToCategories(); return true;
            }
            if (event.enter || event.character == '\n' || event.character == '\r') {
                if (!isEditable(_selectedIdx)) return true;
                auto& item = _items[_selectedIdx];
                if (item.type == SettingType::ACTION) {
                    if (item.action) item.action();
                    rebuildItemList();
                } else if (item.type == SettingType::TEXT_INPUT) {
                    if (strcmp(item.label, "WiFi SSID") == 0) {
                        _wifiResults.clear();
                        _wifiPickerIdx = 0;
                        _wifiResults = WiFiInterface::scanNetworks();
                        if (_wifiResults.empty()) {
                            if (_ui) _ui->lvStatusBar().showToast("No networks found", 1500);
                        } else {
                            _view = SettingsView::WIFI_PICKER;
                            rebuildWifiList();
                        }
                        return true;
                    }
                    _textEditing = true;
                    _editText = item.textGetter ? item.textGetter() : "";
                    rebuildItemList();
                } else if (item.type == SettingType::TOGGLE) {
                    int val = item.getter ? item.getter() : 0;
                    if (item.setter) item.setter(val ? 0 : 1);
                    applyAndSave();
                    rebuildItemList();
                } else {
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
            if (event.up) {
                if (_wifiPickerIdx > 0) {
                    int prev = _wifiPickerIdx;
                    _wifiPickerIdx--;
                    updateWifiSelection(prev, _wifiPickerIdx);
                }
                return true;
            }
            if (event.down) {
                if (_wifiPickerIdx < (int)_wifiResults.size() - 1) {
                    int prev = _wifiPickerIdx;
                    _wifiPickerIdx++;
                    updateWifiSelection(prev, _wifiPickerIdx);
                }
                return true;
            }
            if (event.enter || event.character == '\n' || event.character == '\r') {
                if (_wifiPickerIdx < (int)_wifiResults.size()) {
                    auto& net = _wifiResults[_wifiPickerIdx];
                    if (_cfg) { _cfg->settings().wifiSTASSID = net.ssid; applyAndSave(); }
                }
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

void LvSettingsScreen::snapshotRebootSettings() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    _rebootSnap.wifiMode = s.wifiMode;
    _rebootSnap.wifiSTASSID = s.wifiSTASSID;
    _rebootSnap.wifiSTAPassword = s.wifiSTAPassword;
    _rebootSnap.bleEnabled = s.bleEnabled;
}

bool LvSettingsScreen::rebootSettingsChanged() const {
    if (!_cfg) return false;
    auto& s = _cfg->settings();
    return s.wifiMode != _rebootSnap.wifiMode
        || s.wifiSTASSID != _rebootSnap.wifiSTASSID
        || s.wifiSTAPassword != _rebootSnap.wifiSTAPassword
        || s.bleEnabled != _rebootSnap.bleEnabled;
}

void LvSettingsScreen::snapshotTCPSettings() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    _tcpSnapHost = s.tcpConnections.empty() ? "" : s.tcpConnections[0].host;
    _tcpSnapPort = s.tcpConnections.empty() ? 0 : s.tcpConnections[0].port;
}

bool LvSettingsScreen::tcpSettingsChanged() const {
    if (!_cfg) return false;
    auto& s = _cfg->settings();
    String curHost = s.tcpConnections.empty() ? "" : s.tcpConnections[0].host;
    uint16_t curPort = s.tcpConnections.empty() ? 0 : s.tcpConnections[0].port;
    return curHost != _tcpSnapHost || curPort != _tcpSnapPort;
}

void LvSettingsScreen::applyAndSave() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    if (_power) {
        _power->setBrightness(s.brightness);
        _power->setDimTimeout(s.screenDimTimeout);
        _power->setOffTimeout(s.screenOffTimeout);
    }
    if (_radio && _radio->isRadioOnline()) {
        _radio->setFrequency(s.loraFrequency);
        _radio->setTxPower(s.loraTxPower);
        _radio->setSpreadingFactor(s.loraSF);
        _radio->setSignalBandwidth(s.loraBW);
        _radio->setCodingRate4(s.loraCR);
        _radio->setPreambleLength(s.loraPreamble);
        _radio->receive();
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

    // Apply TCP changes live (stop old clients, create new ones, clear transient nodes)
    if (tcpChanged) {
        snapshotTCPSettings();
        if (_tcpChangeCb) _tcpChangeCb();
    }

    // Check if reboot-required settings changed
    if (rebootSettingsChanged()) {
        _rebootNeeded = true;
    }

    if (_ui) {
        if (_rebootNeeded) {
            _ui->lvStatusBar().showToast("Reboot Required!", 2000);
        } else if (tcpChanged) {
            _ui->lvStatusBar().showToast("TCP Updated", 1200);
        } else {
            _ui->lvStatusBar().showToast(saved ? "Saved" : "Applied", 800);
        }
    }
}
