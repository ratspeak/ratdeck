#include "SettingsScreen.h"
#include "ui/Theme.h"
#include "hal/Display.h"
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

// Radio presets
struct RadioPreset {
    const char* name;
    uint8_t sf;
    uint32_t bw;
    uint8_t cr;
    int8_t txPower;
    long preamble;
};

static const RadioPreset PRESETS[] = {
    {"Balanced",   9,  250000, 5,  14, 18},
    {"Long Range", 12, 125000, 8,  17, 18},
    {"Fast",       7,  500000, 5,  10, 18},
};
static constexpr int NUM_PRESETS = 3;

bool SettingsScreen::isEditable(int idx) const {
    if (idx < 0 || idx >= (int)_items.size()) return false;
    auto t = _items[idx].type;
    return t == SettingType::INTEGER || t == SettingType::TOGGLE
        || t == SettingType::ENUM_CHOICE || t == SettingType::ACTION
        || t == SettingType::TEXT_INPUT;
}

void SettingsScreen::skipToNextEditable(int dir) {
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

int SettingsScreen::detectPreset() const {
    if (!_cfg) return -1;
    auto& s = _cfg->settings();
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (s.loraSF == PRESETS[i].sf && s.loraBW == PRESETS[i].bw
            && s.loraCR == PRESETS[i].cr && s.loraTxPower == PRESETS[i].txPower) {
            return i;
        }
    }
    return -1;
}

void SettingsScreen::applyPreset(int presetIdx) {
    if (!_cfg || presetIdx < 0 || presetIdx >= NUM_PRESETS) return;
    auto& s = _cfg->settings();
    const auto& p = PRESETS[presetIdx];
    s.loraSF = p.sf;
    s.loraBW = p.bw;
    s.loraCR = p.cr;
    s.loraTxPower = p.txPower;
    s.loraPreamble = p.preamble;
}

// =============================================================================
// Build items — no HEADER items, just flat list grouped by category
// =============================================================================
void SettingsScreen::buildItems() {
    _items.clear();
    _categories.clear();
    if (!_cfg) return;
    auto& s = _cfg->settings();
    int idx = 0;

    // ─── Device ───
    int devStart = idx;
    _items.push_back({"Version", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String(RATDECK_VERSION_STRING); }});
    idx++;
    _items.push_back({"Identity", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _identityHash.substring(0, 16); }});
    idx++;
    {
        SettingItem nameItem;
        nameItem.label = "Display Name";
        nameItem.type = SettingType::TEXT_INPUT;
        nameItem.textGetter = [&s]() { return s.displayName; };
        nameItem.textSetter = [&s](const String& v) { s.displayName = v; };
        nameItem.maxTextLen = 16;
        _items.push_back(nameItem);
        idx++;
    }
    // Identity switcher (if IdentityManager available)
    if (_idMgr && _idMgr->count() > 0) {
        SettingItem idSwitch;
        idSwitch.label = "Active Identity";
        idSwitch.type = SettingType::ENUM_CHOICE;
        idSwitch.getter = [this]() { return _idMgr->activeIndex(); };
        idSwitch.setter = [this](int v) {
            // Identity switch requires reboot
            if (v == _idMgr->activeIndex()) return;
            RNS::Identity newId;
            if (_idMgr->switchTo(v, newId)) {
                if (_ui) _ui->statusBar().showToast("Identity switched! Rebooting...", 2000);
                applyAndSave();
                delay(1000);
                ESP.restart();
            } else {
                if (_ui) _ui->statusBar().showToast("Switch failed!", 1500);
            }
        };
        idSwitch.formatter = nullptr;
        idSwitch.minVal = 0;
        idSwitch.maxVal = _idMgr->count() - 1;
        idSwitch.step = 1;
        // Build labels from identity slots
        for (int i = 0; i < _idMgr->count(); i++) {
            auto& slot = _idMgr->identities()[i];
            static char labelBufs[8][32];
            if (!slot.displayName.isEmpty()) {
                snprintf(labelBufs[i], sizeof(labelBufs[i]), "%s", slot.displayName.c_str());
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
            if (!_idMgr) {
                if (_ui) _ui->statusBar().showToast("Not available", 1200);
                return;
            }
            if (_idMgr->count() >= 8) {
                if (_ui) _ui->statusBar().showToast("Max 8 identities!", 1200);
                return;
            }
            int idx = _idMgr->createIdentity(s.displayName);
            if (idx >= 0) {
                if (_ui) _ui->statusBar().showToast("Identity created!", 1200);
                buildItems();  // Rebuild to show new identity in switcher
                markDirty();
            }
        };
        _items.push_back(newId);
        idx++;
    }
    _categories.push_back({"Device", devStart, idx - devStart,
        [&s]() { return s.displayName.isEmpty() ? String("(unnamed)") : s.displayName; }});

    // ─── Display & Input ───
    int dispStart = idx;
    _items.push_back({"Brightness", SettingType::INTEGER,
        [&s]() { return s.brightness; },
        [&s](int v) { s.brightness = v; },
        [](int v) { return String(v); },
        16, 255, 16});
    idx++;
    _items.push_back({"Dim Timeout", SettingType::INTEGER,
        [&s]() { return s.screenDimTimeout; },
        [&s](int v) { s.screenDimTimeout = v; },
        [](int v) { return String(v) + "s"; },
        5, 300, 5});
    idx++;
    _items.push_back({"Off Timeout", SettingType::INTEGER,
        [&s]() { return s.screenOffTimeout; },
        [&s](int v) { s.screenOffTimeout = v; },
        [](int v) { return String(v) + "s"; },
        10, 600, 10});
    idx++;
    _items.push_back({"Trackball Speed", SettingType::INTEGER,
        [&s]() { return s.trackballSpeed; },
        [&s](int v) { s.trackballSpeed = v; },
        [](int v) { return String(v); },
        1, 5, 1});
    idx++;
    _categories.push_back({"Display & Input", dispStart, idx - dispStart,
        [&s]() { return String(s.brightness * 100 / 255) + "%"; }});

    // ─── Radio ───
    int radioStart = idx;
    {
        SettingItem presetItem;
        presetItem.label = "Preset";
        presetItem.type = SettingType::ENUM_CHOICE;
        presetItem.getter = [this]() {
            int p = detectPreset();
            return (p >= 0) ? p : NUM_PRESETS;
        };
        presetItem.setter = [this](int v) {
            if (v >= 0 && v < NUM_PRESETS) applyPreset(v);
        };
        presetItem.formatter = nullptr;
        presetItem.minVal = 0;
        presetItem.maxVal = NUM_PRESETS;
        presetItem.step = 1;
        presetItem.enumLabels = {"Balanced", "Long Range", "Fast", "Custom"};
        _items.push_back(presetItem);
        idx++;
    }
    _items.push_back({"Frequency", SettingType::ENUM_CHOICE,
        [&s]() {
            if (s.loraFrequency <= 868000000) return 0;
            if (s.loraFrequency <= 906000000) return 1;
            if (s.loraFrequency <= 915000000) return 2;
            return 3;
        },
        [&s](int v) {
            static const uint32_t freqs[] = {868000000, 906000000, 915000000, 923000000};
            s.loraFrequency = freqs[constrain(v, 0, 3)];
        },
        nullptr, 0, 3, 1, {"868 MHz", "906 MHz", "915 MHz", "923 MHz"}});
    idx++;
    _items.push_back({"TX Power", SettingType::INTEGER,
        [&s]() { return s.loraTxPower; },
        [&s](int v) { s.loraTxPower = v; },
        [](int v) { return String(v) + " dBm"; },
        -9, 22, 1});
    idx++;
    _items.push_back({"Spread Factor", SettingType::INTEGER,
        [&s]() { return s.loraSF; },
        [&s](int v) { s.loraSF = v; },
        [](int v) { return String("SF") + String(v); },
        5, 12, 1});
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
        [&s]() { return s.loraCR; },
        [&s](int v) { s.loraCR = v; },
        [](int v) { return String("4/") + String(v); },
        5, 8, 1});
    idx++;
    _items.push_back({"Preamble", SettingType::INTEGER,
        [&s]() { return (int)s.loraPreamble; },
        [&s](int v) { s.loraPreamble = v; },
        [](int v) { return String(v); },
        6, 65, 1});
    idx++;
    _categories.push_back({"Radio", radioStart, idx - radioStart,
        [this]() {
            int p = detectPreset();
            return (p >= 0) ? String(PRESETS[p].name) : String("Custom");
        }});

    // ─── Network ───
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
            for (auto& ep : s.tcpConnections) {
                if (ep.host == "rns.ratspeak.org") return 1;
            }
            if (!s.tcpConnections.empty()) return 2;
            return 0;
        };
        tcpPreset.setter = [&s](int v) {
            if (v == 0) {
                s.tcpConnections.clear();
            } else if (v == 1) {
                s.tcpConnections.clear();
                TCPEndpoint ep;
                ep.host = "rns.ratspeak.org";
                ep.port = TCP_DEFAULT_PORT;
                ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            }
        };
        tcpPreset.formatter = nullptr;
        tcpPreset.minVal = 0;
        tcpPreset.maxVal = 2;
        tcpPreset.step = 1;
        tcpPreset.enumLabels = {"None", "Ratspeak Hub", "Custom"};
        _items.push_back(tcpPreset);
        idx++;
    }
    {
        SettingItem tcpHost;
        tcpHost.label = "TCP Host";
        tcpHost.type = SettingType::TEXT_INPUT;
        tcpHost.textGetter = [&s]() {
            return s.tcpConnections.empty() ? String("") : s.tcpConnections[0].host;
        };
        tcpHost.textSetter = [&s](const String& v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep;
                ep.host = v;
                ep.port = TCP_DEFAULT_PORT;
                ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else {
                s.tcpConnections[0].host = v;
            }
        };
        tcpHost.maxTextLen = 40;
        _items.push_back(tcpHost);
        idx++;
    }
    _items.push_back({"TCP Port", SettingType::INTEGER,
        [&s]() { return s.tcpConnections.empty() ? TCP_DEFAULT_PORT : (int)s.tcpConnections[0].port; },
        [&s](int v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep;
                ep.port = v;
                ep.autoConnect = true;
                s.tcpConnections.push_back(ep);
            } else {
                s.tcpConnections[0].port = v;
            }
        },
        [](int v) { return String(v); },
        1, 65535, 1});
    idx++;
    _items.push_back({"Transport Node", SettingType::TOGGLE,
        [&s]() { return s.transportEnabled ? 1 : 0; },
        [&s](int v) { s.transportEnabled = (v != 0); },
        [](int v) { return v ? String("ON") : String("OFF"); }});
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

    // ─── Audio ───
    int audioStart = idx;
    _items.push_back({"Audio", SettingType::TOGGLE,
        [&s]() { return s.audioEnabled ? 1 : 0; },
        [&s](int v) { s.audioEnabled = (v != 0); },
        [](int v) { return v ? String("ON") : String("OFF"); }});
    idx++;
    _items.push_back({"Volume", SettingType::INTEGER,
        [&s]() { return s.audioVolume; },
        [&s](int v) { s.audioVolume = v; },
        [](int v) { return String(v) + "%"; },
        0, 100, 10});
    idx++;
    _categories.push_back({"Audio", audioStart, idx - audioStart,
        [&s]() { return s.audioEnabled ? (String(s.audioVolume) + "%") : String("OFF"); }});

    // ─── System ───
    int sysStart = idx;
    _items.push_back({"Free Heap", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Free PSRAM", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreePsram() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Flash", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _flash && _flash->exists("/ratputer") ? String("Mounted") : String("Error"); }});
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
                const String& name = _cfg->settings().displayName;
                RNS::Bytes appData;
                if (!name.isEmpty()) {
                    size_t len = name.length();
                    if (len > 31) len = 31;
                    uint8_t buf[2 + 31];
                    buf[0] = 0x91;
                    buf[1] = 0xA0 | (uint8_t)len;
                    memcpy(buf + 2, name.c_str(), len);
                    appData = RNS::Bytes(buf, 2 + len);
                }
                _rns->announce(appData);
                if (_ui) {
                    _ui->statusBar().flashAnnounce();
                    _ui->statusBar().showToast("Announce sent!");
                }
            } else {
                if (_ui) _ui->statusBar().showToast("RNS not ready");
            }
        };
        _items.push_back(announceItem);
        idx++;
    }
    {
        SettingItem initSD;
        initSD.label = "Init SD Card";
        initSD.type = SettingType::ACTION;
        initSD.formatter = [this](int) {
            return (_sd && _sd->isReady()) ? String("[Enter]") : String("No Card");
        };
        initSD.action = [this]() {
            if (!_sd || !_sd->isReady()) {
                if (_ui) _ui->statusBar().showToast("No SD card!", 1200);
                return;
            }
            if (_ui) _ui->statusBar().showToast("Initializing SD...", 2000);
            bool ok = _sd->formatForRatputer();
            if (_ui) _ui->statusBar().showToast(ok ? "SD initialized!" : "SD init failed!", 1500);
        };
        _items.push_back(initSD);
        idx++;
    }
    {
        SettingItem wipeSD;
        wipeSD.label = "Wipe SD Data";
        wipeSD.type = SettingType::ACTION;
        wipeSD.formatter = [this](int) {
            return (_sd && _sd->isReady()) ? String("[Enter]") : String("No Card");
        };
        wipeSD.action = [this]() {
            if (!_sd || !_sd->isReady()) {
                if (_ui) _ui->statusBar().showToast("No SD card!", 1200);
                return;
            }
            if (_ui) _ui->statusBar().showToast("Wiping SD data...", 2000);
            bool ok = _sd->wipeRatputer();
            if (_ui) _ui->statusBar().showToast(ok ? "SD wiped & reinit!" : "Wipe failed!", 1500);
        };
        _items.push_back(wipeSD);
        idx++;
    }
    {
        SettingItem factoryReset;
        factoryReset.label = "Factory Reset";
        factoryReset.type = SettingType::ACTION;
        factoryReset.formatter = [this](int) {
            return _confirmingReset ? String("[Confirm?]") : String("[Enter]");
        };
        factoryReset.action = [this]() {
            if (!_confirmingReset) {
                _confirmingReset = true;
                if (_ui) _ui->statusBar().showToast("Press again to confirm!", 2000);
                markDirty();
                return;
            }
            _confirmingReset = false;
            if (_ui) _ui->statusBar().showToast("Factory resetting...", 3000);
            Serial.println("[SETTINGS] Factory reset initiated");
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
            if (_ui) _ui->statusBar().showToast("Rebooting...", 1500);
            Serial.println("[SETTINGS] Reboot requested");
            delay(500);
            ESP.restart();
        };
        _items.push_back(rebootItem);
        idx++;
    }
    _categories.push_back({"System", sysStart, idx - sysStart,
        [](){ return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB free"; }});
}

// =============================================================================
// Lifecycle
// =============================================================================
void SettingsScreen::onEnter() {
    buildItems();
    _view = SettingsView::CATEGORY_LIST;
    _categoryIdx = 0;
    _categoryScroll = 0;
    _selectedIdx = 0;
    _itemScroll = 0;
    _editing = false;
    _textEditing = false;
    _confirmingReset = false;
    markDirty();
}

void SettingsScreen::update() {
}

// =============================================================================
// Category navigation
// =============================================================================
void SettingsScreen::enterCategory(int catIdx) {
    if (catIdx < 0 || catIdx >= (int)_categories.size()) return;
    _categoryIdx = catIdx;
    auto& cat = _categories[catIdx];
    _catRangeStart = cat.startIdx;
    _catRangeEnd = cat.startIdx + cat.count;
    _selectedIdx = _catRangeStart;
    _itemScroll = 0;
    _editing = false;
    _textEditing = false;
    // Skip to first editable item
    if (!isEditable(_selectedIdx)) skipToNextEditable(1);
    _view = SettingsView::ITEM_LIST;
    markDirty();
}

void SettingsScreen::exitToCategories() {
    _view = SettingsView::CATEGORY_LIST;
    _editing = false;
    _textEditing = false;
    _confirmingReset = false;
    markDirty();
}

// =============================================================================
// Drawing
// =============================================================================
void SettingsScreen::draw(LGFX_TDeck& gfx) {
    switch (_view) {
        case SettingsView::CATEGORY_LIST: drawCategoryList(gfx); break;
        case SettingsView::ITEM_LIST:     drawItemList(gfx); break;
        case SettingsView::WIFI_PICKER:   drawWifiPicker(gfx); break;
    }
}

void SettingsScreen::drawCategoryList(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);
    int lineH = 28;  // Taller rows for categories
    int y = Theme::CONTENT_Y + 4;

    // Title
    gfx.setTextColor(Theme::ACCENT, Theme::BG);
    gfx.setCursor(4, y);
    gfx.print("SETTINGS");
    gfx.drawFastHLine(0, y + 12, Theme::SCREEN_W, Theme::BORDER);
    y += 18;

    int visibleStart = _categoryScroll;
    int maxVisible = (Theme::CONTENT_H - 22) / lineH;

    // Scroll adjustment
    if (_categoryIdx < _categoryScroll) _categoryScroll = _categoryIdx;
    if (_categoryIdx >= _categoryScroll + maxVisible) _categoryScroll = _categoryIdx - maxVisible + 1;
    visibleStart = _categoryScroll;

    for (int i = visibleStart; i < (int)_categories.size(); i++) {
        int row = i - visibleStart;
        int rowY = y + row * lineH;
        if (rowY + lineH > Theme::SCREEN_H - Theme::TAB_BAR_H) break;

        bool selected = (i == _categoryIdx);
        auto& cat = _categories[i];

        if (selected) {
            gfx.fillRect(0, rowY, Theme::SCREEN_W, lineH - 2, Theme::SELECTION_BG);
        }

        uint32_t bg = selected ? Theme::SELECTION_BG : Theme::BG;

        // Category name
        gfx.setTextColor(selected ? Theme::ACCENT : Theme::PRIMARY, bg);
        gfx.setCursor(12, rowY + 4);
        gfx.print(cat.name);

        // Item count
        gfx.setTextColor(Theme::MUTED, bg);
        char countStr[8];
        snprintf(countStr, sizeof(countStr), "(%d)", cat.count);
        gfx.setCursor(12 + strlen(cat.name) * 6 + 6, rowY + 4);
        gfx.print(countStr);

        // Summary on second line
        if (cat.summary) {
            String sum = cat.summary();
            gfx.setTextColor(Theme::MUTED, bg);
            gfx.setCursor(20, rowY + 15);
            gfx.print(sum.c_str());
        }

        // Arrow indicator
        gfx.setTextColor(selected ? Theme::ACCENT : Theme::MUTED, bg);
        gfx.setCursor(Theme::SCREEN_W - 14, rowY + 8);
        gfx.print(">");
    }
}

void SettingsScreen::drawItemList(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);
    int lineH = 14;

    // Category header
    int y = Theme::CONTENT_Y + 2;
    gfx.setTextColor(Theme::ACCENT, Theme::BG);
    gfx.setCursor(4, y);
    gfx.print("< ");
    gfx.print(_categories[_categoryIdx].name);
    gfx.drawFastHLine(0, y + 12, Theme::SCREEN_W, Theme::BORDER);
    int listY = y + 16;

    int visibleLines = (Theme::SCREEN_H - Theme::TAB_BAR_H - listY) / lineH;
    int relIdx = _selectedIdx - _catRangeStart;

    // Scroll adjustment
    if (relIdx < _itemScroll) _itemScroll = relIdx;
    if (relIdx >= _itemScroll + visibleLines) _itemScroll = relIdx - visibleLines + 1;

    int valX = 160;

    for (int r = 0; r < visibleLines; r++) {
        int itemRel = _itemScroll + r;
        int itemAbs = _catRangeStart + itemRel;
        if (itemAbs >= _catRangeEnd) break;

        int rowY = listY + r * lineH;
        const auto& item = _items[itemAbs];
        bool selected = (itemAbs == _selectedIdx);
        bool editable = isEditable(itemAbs);

        if (selected && editable) {
            gfx.fillRect(0, rowY, Theme::SCREEN_W, lineH, Theme::SELECTION_BG);
        }

        uint32_t bgCol = (selected && editable) ? Theme::SELECTION_BG : Theme::BG;

        if (item.type == SettingType::ACTION) {
            gfx.setTextColor(selected ? Theme::ACCENT : Theme::PRIMARY, bgCol);
            gfx.setCursor(4, rowY + 3);
            gfx.print(item.label);
            if (item.formatter) {
                String hint = item.formatter(0);
                gfx.setTextColor(Theme::MUTED, bgCol);
                gfx.setCursor(valX, rowY + 3);
                gfx.print(hint.c_str());
            }
        } else if (item.type == SettingType::TEXT_INPUT) {
            gfx.setTextColor(Theme::SECONDARY, bgCol);
            gfx.setCursor(4, rowY + 3);
            gfx.print(item.label);

            if (_textEditing && selected) {
                gfx.setTextColor(Theme::WARNING_CLR, bgCol);
                gfx.setCursor(valX, rowY + 3);
                // Show tail of text if too long
                int maxChars = (Theme::SCREEN_W - valX - 10) / 6;
                if ((int)_editText.length() > maxChars) {
                    gfx.print(_editText.substring(_editText.length() - maxChars).c_str());
                } else {
                    gfx.print(_editText.c_str());
                }
                int curX = valX + std::min((int)_editText.length(), maxChars) * 6;
                if ((millis() / 500) % 2 == 0) {
                    gfx.fillRect(curX, rowY + 3, 6, 8, Theme::WARNING_CLR);
                }
            } else {
                String val = item.textGetter ? item.textGetter() : "";
                gfx.setTextColor(val.isEmpty() ? Theme::MUTED : Theme::PRIMARY, bgCol);
                gfx.setCursor(valX, rowY + 3);
                gfx.print(val.isEmpty() ? "(not set)" : val.c_str());
            }
        } else {
            // Label
            gfx.setTextColor(Theme::SECONDARY, bgCol);
            gfx.setCursor(4, rowY + 3);
            gfx.print(item.label);

            // Value
            String valStr;
            if (_editing && selected) {
                if (item.type == SettingType::ENUM_CHOICE && !item.enumLabels.empty()) {
                    int vi = constrain(_editValue, 0, (int)item.enumLabels.size() - 1);
                    valStr = item.enumLabels[vi];
                } else if (item.formatter) {
                    valStr = item.formatter(_editValue);
                } else {
                    valStr = String(_editValue);
                }
                gfx.setTextColor(Theme::WARNING_CLR, bgCol);
                gfx.setCursor(valX - 12, rowY + 3);
                gfx.print("<");
                gfx.setCursor(valX, rowY + 3);
                gfx.print(valStr.c_str());
                int endX = valX + (int)valStr.length() * 6 + 4;
                gfx.setCursor(endX, rowY + 3);
                gfx.print(">");
            } else {
                if (item.type == SettingType::READONLY) {
                    valStr = item.formatter ? item.formatter(0) : "";
                    gfx.setTextColor(Theme::MUTED, bgCol);
                } else if (item.type == SettingType::ENUM_CHOICE && !item.enumLabels.empty()) {
                    int vi = item.getter ? constrain(item.getter(), 0, (int)item.enumLabels.size() - 1) : 0;
                    valStr = item.enumLabels[vi];
                    gfx.setTextColor(Theme::PRIMARY, bgCol);
                } else {
                    int val = item.getter ? item.getter() : 0;
                    valStr = item.formatter ? item.formatter(val) : String(val);
                    gfx.setTextColor(Theme::PRIMARY, bgCol);
                }
                gfx.setCursor(valX, rowY + 3);
                gfx.print(valStr.c_str());
            }
        }
    }

    // Scroll indicator
    int totalItems = _catRangeEnd - _catRangeStart;
    if (totalItems > visibleLines) {
        int barH = visibleLines * lineH;
        int thumbH = max(8, barH * visibleLines / totalItems);
        int thumbY = listY + (barH - thumbH) * _itemScroll / max(1, totalItems - visibleLines);
        gfx.fillRect(Theme::SCREEN_W - 2, listY, 2, barH, Theme::BORDER);
        gfx.fillRect(Theme::SCREEN_W - 2, thumbY, 2, thumbH, Theme::SECONDARY);
    }
}

void SettingsScreen::drawWifiPicker(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);
    int lineH = 16;
    int y = Theme::CONTENT_Y + 2;

    // Header
    gfx.setTextColor(Theme::ACCENT, Theme::BG);
    gfx.setCursor(4, y);
    gfx.print("< Select WiFi Network");
    gfx.drawFastHLine(0, y + 12, Theme::SCREEN_W, Theme::BORDER);
    int listY = y + 16;

    if (_wifiScanning) {
        gfx.setTextColor(Theme::WARNING_CLR, Theme::BG);
        gfx.setCursor(4, listY + 20);
        gfx.print("  Scanning for networks...");
        // Animated dots
        int dots = (millis() / 400) % 4;
        for (int d = 0; d < dots; d++) gfx.print(".");
        return;
    }

    if (_wifiResults.empty()) {
        gfx.setTextColor(Theme::MUTED, Theme::BG);
        gfx.setCursor(4, listY + 4);
        gfx.print("No networks found");
        return;
    }

    int visibleLines = (Theme::SCREEN_H - Theme::TAB_BAR_H - listY) / lineH;

    // Scroll
    if (_wifiPickerIdx < _wifiPickerScroll) _wifiPickerScroll = _wifiPickerIdx;
    if (_wifiPickerIdx >= _wifiPickerScroll + visibleLines) _wifiPickerScroll = _wifiPickerIdx - visibleLines + 1;

    for (int r = 0; r < visibleLines; r++) {
        int i = _wifiPickerScroll + r;
        if (i >= (int)_wifiResults.size()) break;

        int rowY = listY + r * lineH;
        bool selected = (i == _wifiPickerIdx);
        auto& net = _wifiResults[i];

        if (selected) {
            gfx.fillRect(0, rowY, Theme::SCREEN_W, lineH - 1, Theme::SELECTION_BG);
        }
        uint32_t bg = selected ? Theme::SELECTION_BG : Theme::BG;

        // Lock icon for encrypted
        gfx.setTextColor(Theme::MUTED, bg);
        gfx.setCursor(4, rowY + 4);
        gfx.print(net.encrypted ? "*" : " ");

        // SSID
        gfx.setTextColor(selected ? Theme::ACCENT : Theme::PRIMARY, bg);
        gfx.setCursor(14, rowY + 4);
        gfx.print(net.ssid.c_str());

        // Signal bars
        int bars = 1;
        if (net.rssi > -50) bars = 4;
        else if (net.rssi > -65) bars = 3;
        else if (net.rssi > -80) bars = 2;
        char sig[8];
        snprintf(sig, sizeof(sig), "%ddBm", net.rssi);
        gfx.setTextColor(Theme::MUTED, bg);
        gfx.setCursor(Theme::SCREEN_W - 50, rowY + 4);
        gfx.print(sig);

        // Draw signal bars
        int barX = Theme::SCREEN_W - 58;
        for (int b = 0; b < 4; b++) {
            int barH2 = 3 + b * 2;
            int barY2 = rowY + 12 - barH2;
            uint32_t barCol = (b < bars) ? Theme::ACCENT : Theme::BORDER;
            gfx.fillRect(barX + b * 4, barY2, 3, barH2, barCol);
        }
    }

    // Bottom hint
    int hintY = Theme::SCREEN_H - Theme::TAB_BAR_H - 12;
    gfx.setTextColor(Theme::MUTED, Theme::BG);
    gfx.setCursor(4, hintY);
    gfx.print("Enter=Select  Bksp=Back");
}

// =============================================================================
// Key handling
// =============================================================================
bool SettingsScreen::handleKey(const KeyEvent& event) {
    switch (_view) {
        case SettingsView::CATEGORY_LIST: return handleCategoryKeys(event);
        case SettingsView::ITEM_LIST:     return handleItemKeys(event);
        case SettingsView::WIFI_PICKER:   return handleWifiPickerKeys(event);
    }
    return false;
}

bool SettingsScreen::handleCategoryKeys(const KeyEvent& event) {
    if (event.up) {
        if (_categoryIdx > 0) { _categoryIdx--; markDirty(); }
        return true;
    }
    if (event.down) {
        if (_categoryIdx < (int)_categories.size() - 1) { _categoryIdx++; markDirty(); }
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        enterCategory(_categoryIdx);
        return true;
    }
    return false;
}

bool SettingsScreen::handleItemKeys(const KeyEvent& event) {
    if (_items.empty()) return false;

    // Text edit mode
    if (_textEditing) {
        auto& item = _items[_selectedIdx];
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (item.textSetter) item.textSetter(_editText);
            _textEditing = false;
            applyAndSave();
            markDirty();
            return true;
        }
        if (event.del || event.character == 8) {
            if (_editText.length() > 0) {
                _editText.remove(_editText.length() - 1);
                markDirty();
            }
            return true;
        }
        if (event.character == 0x1B) {
            _textEditing = false;
            markDirty();
            return true;
        }
        if (event.character >= 0x20 && event.character <= 0x7E
            && (int)_editText.length() < item.maxTextLen) {
            _editText += (char)event.character;
            markDirty();
            return true;
        }
        return true;
    }

    // Value edit mode
    if (_editing) {
        auto& item = _items[_selectedIdx];
        if (event.left) {
            _editValue -= item.step;
            if (_editValue < item.minVal) _editValue = item.minVal;
            markDirty();
            return true;
        }
        if (event.right) {
            _editValue += item.step;
            if (_editValue > item.maxVal) _editValue = item.maxVal;
            markDirty();
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (item.setter) item.setter(_editValue);
            _editing = false;
            applyAndSave();
            markDirty();
            return true;
        }
        if (event.del || event.character == 8 || event.character == 0x1B) {
            _editing = false;
            markDirty();
            return true;
        }
        return true;
    }

    // Browse mode
    if (event.up) {
        int prev = _selectedIdx;
        skipToNextEditable(-1);
        if (_selectedIdx != prev) markDirty();
        return true;
    }
    if (event.down) {
        int prev = _selectedIdx;
        skipToNextEditable(1);
        if (_selectedIdx != prev) markDirty();
        return true;
    }

    // Back to categories
    if (event.del || event.character == 8 || event.character == 0x1B) {
        exitToCategories();
        return true;
    }

    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (!isEditable(_selectedIdx)) return true;
        auto& item = _items[_selectedIdx];

        if (item.type == SettingType::ACTION) {
            if (item.action) item.action();
            markDirty();
        } else if (item.type == SettingType::TEXT_INPUT) {
            // WiFi SSID: open scanner
            if (strcmp(item.label, "WiFi SSID") == 0) {
                // Show scanning screen, render it, then scan (blocking)
                _wifiResults.clear();
                _wifiPickerIdx = 0;
                _wifiPickerScroll = 0;
                _wifiScanning = true;
                _view = SettingsView::WIFI_PICKER;
                // Force immediate render of "Scanning..." before blocking
                if (_ui) {
                    markDirty();
                    _ui->render();
                }
                // Now do the blocking scan
                _wifiScanning = false;
                _wifiResults = WiFiInterface::scanNetworks();
                if (_wifiResults.empty()) {
                    if (_ui) _ui->statusBar().showToast("No networks found", 1500);
                    _view = SettingsView::ITEM_LIST;
                }
                markDirty();
                return true;
            }
            // Normal text input
            _textEditing = true;
            _editText = item.textGetter ? item.textGetter() : "";
            markDirty();
        } else if (item.type == SettingType::TOGGLE) {
            int val = item.getter ? item.getter() : 0;
            if (item.setter) item.setter(val ? 0 : 1);
            applyAndSave();
            markDirty();
        } else {
            // Enter value edit mode
            _editing = true;
            _editValue = item.getter ? item.getter() : 0;
            markDirty();
        }
        return true;
    }
    return false;
}

bool SettingsScreen::handleWifiPickerKeys(const KeyEvent& event) {
    if (event.up) {
        if (_wifiPickerIdx > 0) { _wifiPickerIdx--; markDirty(); }
        return true;
    }
    if (event.down) {
        if (_wifiPickerIdx < (int)_wifiResults.size() - 1) { _wifiPickerIdx++; markDirty(); }
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_wifiPickerIdx < (int)_wifiResults.size()) {
            auto& net = _wifiResults[_wifiPickerIdx];
            if (_cfg) {
                _cfg->settings().wifiSTASSID = net.ssid;
                applyAndSave();
            }
            if (_ui) _ui->statusBar().showToast(("Selected: " + net.ssid).c_str(), 1500);
        }
        _view = SettingsView::ITEM_LIST;
        markDirty();
        return true;
    }
    if (event.del || event.character == 8 || event.character == 0x1B) {
        _view = SettingsView::ITEM_LIST;
        markDirty();
        return true;
    }
    return false;
}

// =============================================================================
// Apply & Save
// =============================================================================
void SettingsScreen::applyAndSave() {
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

    bool saved = false;
    if (_saveCallback) {
        saved = _saveCallback();
    } else if (_sd && _flash) {
        saved = _cfg->save(*_sd, *_flash);
    } else if (_flash) {
        saved = _cfg->save(*_flash);
    }

    if (_ui) {
        _ui->statusBar().showToast(saved ? "Saved" : "Applied", 800);
    }

    Serial.printf("[SETTINGS] Applied, save=%s\n", saved ? "OK" : "FAILED");
}
