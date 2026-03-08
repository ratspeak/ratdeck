#include "LvHomeScreen.h"
#include "ui/Theme.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "radio/SX1262.h"
#include "config/UserConfig.h"
#include <Arduino.h>
#include <WiFi.h>

void LvHomeScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* font = &lv_font_montserrat_14;
    auto mkLabel = [&](const char* initial) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(lbl, initial);
        return lbl;
    };

    _lblName = mkLabel("Name: ...");
    _lblId = mkLabel("ID: ...");
    _lblStatus = mkLabel("Status: ...");
    _lblNodes = mkLabel("Online Nodes: ...");

    // Force refresh on new UI
    _lastUptime = ULONG_MAX;
    _lastHeap = UINT32_MAX;
    refreshUI();
}

void LvHomeScreen::onEnter() {
    _lastUptime = ULONG_MAX;
    _lastHeap = UINT32_MAX;
    refreshUI();
}

void LvHomeScreen::refreshUI() {
    if (!_lblName) return;

    unsigned long upMins = millis() / 60000;
    uint32_t heap = ESP.getFreeHeap() / 1024;
    if (upMins == _lastUptime && heap == _lastHeap) return;
    _lastUptime = upMins;
    _lastHeap = heap;

    // Name
    if (_cfg && !_cfg->settings().displayName.isEmpty()) {
        lv_label_set_text_fmt(_lblName, "Name: %s", _cfg->settings().displayName.c_str());
    } else if (_rns) {
        String dh = _rns->destinationHashHex();
        String fallback = "Ratspeak.org-" + dh.substring(0, 3);
        lv_label_set_text_fmt(_lblName, "Name: %s", fallback.c_str());
    } else {
        lv_label_set_text(_lblName, "Name: ---");
    }

    // ID (LXMF destination hash, 12 chars)
    if (_rns) {
        String dh = _rns->destinationHashHex();
        if (dh.length() > 12) dh = dh.substring(0, 12);
        lv_label_set_text_fmt(_lblId, "ID: %s", dh.c_str());
    } else {
        lv_label_set_text(_lblId, "ID: ---");
    }

    // Status
    bool loraUp = _radioOnline && _radio && _radio->isRadioOnline();
    bool tcpUp = WiFi.status() == WL_CONNECTED;
    if (loraUp && tcpUp) {
        lv_label_set_text(_lblStatus, "Status: Online (LoRa/TCP)");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::PRIMARY), 0);
    } else if (loraUp) {
        lv_label_set_text(_lblStatus, "Status: Online (LoRa)");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::PRIMARY), 0);
    } else if (tcpUp) {
        lv_label_set_text(_lblStatus, "Status: Online (TCP)");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::PRIMARY), 0);
    } else {
        lv_label_set_text(_lblStatus, "Status: Offline");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::ERROR_CLR), 0);
    }

    // Online Nodes (30 min window)
    if (_am) {
        int online = _am->nodesOnlineSince(1800000);
        lv_label_set_text_fmt(_lblNodes, "Online Nodes: %d", online);
    } else {
        lv_label_set_text(_lblNodes, "Online Nodes: 0");
    }

}

bool LvHomeScreen::handleKey(const KeyEvent& event) {
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_announceCb) _announceCb();
        return true;
    }
    return false;
}
