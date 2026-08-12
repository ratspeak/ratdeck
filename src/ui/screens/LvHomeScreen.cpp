#include "LvHomeScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/LxmFaceAvatar.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "radio/SX1262.h"
#include "config/UserConfig.h"
#include "transport/TCPClientInterface.h"
#include <Arduino.h>
#include <WiFi.h>
#include "fonts/fonts.h"

namespace {

constexpr lv_coord_t kPad = 6;
constexpr lv_coord_t kPanelW = Theme::CONTENT_W - (kPad * 2);
constexpr lv_coord_t kIdentityY = 4;
constexpr lv_coord_t kIdentityH = 58;
constexpr lv_coord_t kChipY = 68;
constexpr lv_coord_t kChipH = 36;
constexpr lv_coord_t kStatY = 108;
constexpr lv_coord_t kStatH = 38;
constexpr lv_coord_t kFooterY = 152;
constexpr lv_coord_t kFooterH = 38;
constexpr lv_coord_t kCellW = 98;
constexpr lv_coord_t kGap = 7;
constexpr int kAvatarSize = 40;

lv_obj_t* makePanel(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    lv_coord_t h, uint32_t bg, uint32_t border) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 4, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* makeLabel(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                    const lv_font_t* font, uint32_t color, const char* text,
                    lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

lv_obj_t* makeCaption(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = makeLabel(parent, 7, 4, 84, &lv_font_rsdeck_10,
                              Theme::TEXT_MUTED, text);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    return lbl;
}

lv_obj_t* makeChip(lv_obj_t* parent, lv_coord_t x, const char* title,
                   lv_obj_t** valueLabel) {
    lv_obj_t* chip = makePanel(parent, x, kChipY, kCellW, kChipH,
                               Theme::BG_ELEVATED, Theme::BORDER);
    makeCaption(chip, title);
    *valueLabel = makeLabel(chip, 7, 18, 84, &lv_font_rsdeck_12,
                            Theme::TEXT_SECONDARY, "...");
    return chip;
}

lv_obj_t* makeStat(lv_obj_t* parent, lv_coord_t x, const char* title,
                   lv_obj_t** valueLabel) {
    lv_obj_t* stat = makePanel(parent, x, kStatY, kCellW, kStatH,
                               Theme::BG_ELEVATED, Theme::BORDER);
    makeCaption(stat, title);
    *valueLabel = makeLabel(stat, 7, 17, 84, &lv_font_rsdeck_14,
                            Theme::TEXT_PRIMARY, "0");
    return stat;
}

void setPanelTone(lv_obj_t* obj, uint32_t bg, uint32_t border) {
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
}

void setChipState(lv_obj_t* chip, lv_obj_t* label, const char* text, bool active,
                  bool warning = false) {
    uint32_t color = Theme::TEXT_MUTED;
    uint32_t bg = Theme::BG_ELEVATED;
    uint32_t border = Theme::BORDER;

    if (active) {
        color = Theme::SUCCESS;
        bg = Theme::PRIMARY_SUBTLE;
        border = Theme::PRIMARY;
    } else if (warning) {
        color = Theme::WARNING_CLR;
        border = Theme::WARNING_CLR;
    }

    setPanelTone(chip, bg, border);
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    }
}

void makeClickable(lv_obj_t* obj, void (*cb)(lv_event_t*), void* userData) {
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(LvInput::group(), obj);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, userData);
}

bool tcpConfigured(const UserSettings& settings) {
    for (const auto& ep : settings.tcpConnections) {
        if (!ep.host.isEmpty() && ep.autoConnect) return true;
    }
    return false;
}

void formatAge(unsigned long elapsedMs, char* out, size_t len) {
    unsigned long sec = elapsedMs / 1000;
    if (sec < 60) {
        snprintf(out, len, "%lus ago", sec);
    } else if (sec < 3600) {
        snprintf(out, len, "%lum ago", sec / 60);
    } else {
        snprintf(out, len, "%luh ago", sec / 3600);
    }
}

}  // namespace

void LvHomeScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_layout(parent, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    _identityPanel = makePanel(parent, kPad, kIdentityY, kPanelW, kIdentityH,
                               Theme::BG_ELEVATED, Theme::BORDER);

    lv_obj_t* rail = lv_obj_create(_identityPanel);
    lv_obj_set_pos(rail, 0, 0);
    lv_obj_set_size(rail, 3, kIdentityH);
    lv_obj_set_style_bg_color(rail, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    _avatarBuffer.assign(LxmFaceAvatar::bufferSize(kAvatarSize), 0);
    auto avatar = LxmFaceAvatar::create(_identityPanel, 11, 9, kAvatarSize,
                                        _avatarBuffer.data(),
                                        Theme::PRIMARY_SUBTLE, Theme::BORDER_ACTIVE);
    _avatarBox = avatar.box;
    _avatarCanvas = avatar.canvas;
    _avatarSeed = "";

    _lblConsoleTitle = makeLabel(_identityPanel, 58, 6, 116,
                                 &lv_font_rsdeck_10, Theme::ACCENT,
                                 "IDENTITY:");
    lv_obj_set_style_text_letter_space(_lblConsoleTitle, 1, 0);

    _lblStatus = makeLabel(_identityPanel, 184, 6, 114, &lv_font_rsdeck_10,
                           Theme::TEXT_SECONDARY, "OFFLINE",
                           LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_text_letter_space(_lblStatus, 1, 0);

    _lblName = makeLabel(_identityPanel, 58, 21, 240, &lv_font_rsdeck_14,
                         Theme::TEXT_PRIMARY, "...");
    lv_label_set_long_mode(_lblName, LV_LABEL_LONG_DOT);

    _lblId = makeLabel(_identityPanel, 58, 41, 136, &lv_font_rsdeck_10,
                       Theme::TEXT_SECONDARY, "LXMF ----");
    _lblIdentity = makeLabel(_identityPanel, 198, 41, 100,
                             &lv_font_rsdeck_10, Theme::TEXT_MUTED,
                             "ID ----", LV_TEXT_ALIGN_RIGHT);

    _chipLora = makeChip(parent, kPad, "LORA", &_lblLoraState);
    makeClickable(_chipLora, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->toggleLora();
    }, this);
    _chipTcp = makeChip(parent, kPad + kCellW + kGap, "TCP", &_lblTcpState);
    makeClickable(_chipTcp, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->toggleTcp();
    }, this);
    _chipWifi = makeChip(parent, kPad + (kCellW + kGap) * 2, "WIFI", &_lblWifiState);
    makeClickable(_chipWifi, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->toggleWiFi();
    }, this);

    _statNodes = makeStat(parent, kPad, "PEERS", &_lblNodes);
    makeClickable(_statNodes, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->openPeers();
    }, this);
    _statPaths = makeStat(parent, kPad + kCellW + kGap, "AUDIO", &_lblPaths);
    makeClickable(_statPaths, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->toggleAudio();
    }, this);

    _statLinks = makeStat(parent, kPad + (kCellW + kGap) * 2, "GPS", &_lblLinks);
    makeClickable(_statLinks, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        self->toggleGPS();
    }, this);

    lv_obj_t* footer = makePanel(parent, kPad, kFooterY, 198, kFooterH,
                                 Theme::BG_ELEVATED, Theme::BORDER);
    _lblSummary = makeLabel(footer, 8, 6, 182, &lv_font_rsdeck_12,
                            Theme::TEXT_SECONDARY, "Waiting for transport");
    lv_label_set_long_mode(_lblSummary, LV_LABEL_LONG_DOT);
    _lblLastAnnounce = makeLabel(footer, 8, 23, 182, &lv_font_rsdeck_10,
                                 Theme::TEXT_MUTED, "Announced: never");
    lv_label_set_long_mode(_lblLastAnnounce, LV_LABEL_LONG_CLIP);

    _btnAnnounce = lv_btn_create(parent);
    lv_obj_set_pos(_btnAnnounce, 210, kFooterY);
    lv_obj_set_size(_btnAnnounce, 104, kFooterH);
    lv_obj_add_style(_btnAnnounce, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnAnnounce, LvTheme::styleBtnFocused(), LV_STATE_FOCUSED);
    lv_obj_add_style(_btnAnnounce, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_btnAnnounce, lv_color_hex(Theme::PRIMARY_SUBTLE), 0);
    lv_obj_set_style_border_color(_btnAnnounce, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_shadow_width(_btnAnnounce, 0, 0);
    lv_obj_set_style_radius(_btnAnnounce, 4, 0);
    lv_obj_set_style_pad_all(_btnAnnounce, 0, 0);

    _lblAnnounceAction = lv_label_create(_btnAnnounce);
    lv_obj_set_style_text_font(_lblAnnounceAction, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_lblAnnounceAction, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(_lblAnnounceAction, "ANNOUNCE");
    lv_obj_center(_lblAnnounceAction);

    lv_group_add_obj(LvInput::group(), _btnAnnounce);
    lv_obj_add_event_cb(_btnAnnounce, [](lv_event_t* e) {
        auto* self = (LvHomeScreen*)lv_event_get_user_data(e);
        if (self->_announceCb) self->_announceCb();
    }, LV_EVENT_CLICKED, this);

    _lastRefreshMs = 0;
    _lastUptime = ULONG_MAX;
    _lastHeap = UINT32_MAX;
    refreshUI();
}

void LvHomeScreen::onEnter() {
    _lastRefreshMs = 0;
    _lastUptime = ULONG_MAX;
    _lastHeap = UINT32_MAX;
    _avatarSeed = "";
    refreshUI();
}

void LvHomeScreen::refreshUI() {
    if (!_lblName) return;

    unsigned long now = millis();
    bool force = (_lastUptime == ULONG_MAX || _lastHeap == UINT32_MAX);
    if (!force && (now - _lastRefreshMs) < 1000) return;
    _lastRefreshMs = now;

    unsigned long upMins = now / 60000;
    uint32_t heap = ESP.getFreeHeap() / 1024;
    _lastUptime = upMins;
    _lastHeap = heap;

    String displayName;
    if (_cfg && !_cfg->settings().displayName.isEmpty()) {
        displayName = _cfg->settings().displayName;
    } else if (_rns) {
        String dh = _rns->destinationHashHex();
        displayName = "Ratspeak.org-" + dh.substring(0, 3);
    } else {
        displayName = "rsDeck";
    }
    lv_label_set_text(_lblName, displayName.c_str());

    String avatarSeed = displayName;

    if (_rns) {
        String dh = _rns->destinationHashHex();
        if (!dh.isEmpty() && dh != "unknown") avatarSeed = dh;
        if (dh.length() > 12) dh = dh.substring(0, 12);
        lv_label_set_text_fmt(_lblId, "LXMF %s", dh.c_str());

        String ih = _rns->identityHash();
        if (ih.length() > 14) ih = ih.substring(0, 14);
        lv_label_set_text_fmt(_lblIdentity, "ID %s", ih.c_str());
    } else {
        lv_label_set_text(_lblId, "LXMF ----");
        lv_label_set_text(_lblIdentity, "ID ----");
    }
    renderAvatar(avatarSeed);

    auto* loraIf = _rns ? _rns->loraInterface() : nullptr;
    bool loraRuntimeUp = _radioOnline && _radio && _radio->isRadioOnline()
        && loraIf && loraIf->isOnline();
    bool loraEnabled = _cfg ? _cfg->settings().loraEnabled : loraRuntimeUp;
    bool tcpUp = false;
    int tcpClientCount = 0;
    if (_tcpClients) {
        for (auto* tcp : *_tcpClients) {
            if (tcp) tcpClientCount++;
            if (tcp && tcp->isConnected()) { tcpUp = true; break; }
        }
    }
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool wifiEnabled = _cfg ? _cfg->settings().wifiMode != RAT_WIFI_OFF : wifiUp;
    bool wifiRuntimeActive = wifiUp || (_cfg && _cfg->settings().wifiMode == RAT_WIFI_AP);
    bool tcpEnabled = _cfg ? tcpConfigured(_cfg->settings()) : tcpClientCount > 0;
    bool transportActive = _rns && _rns->isTransportActive();
    bool reachable = loraRuntimeUp || tcpUp;

    setChipState(_chipLora, _lblLoraState, loraEnabled ? "ON" : "OFF",
                 loraEnabled && loraRuntimeUp, loraEnabled != loraRuntimeUp);
    setChipState(_chipTcp, _lblTcpState, tcpEnabled ? "ON" : "OFF",
                 tcpUp, tcpEnabled && !tcpUp);
    setChipState(_chipWifi, _lblWifiState, wifiEnabled ? "ON" : "OFF",
                 wifiRuntimeActive, wifiEnabled && !wifiRuntimeActive);

    if (reachable) {
        lv_label_set_text(_lblStatus, "ONLINE");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::SUCCESS), 0);
        setPanelTone(_identityPanel, Theme::BG_ELEVATED, Theme::PRIMARY);
    } else if (wifiUp || transportActive) {
        lv_label_set_text(_lblStatus, wifiUp ? "WIFI ONLY" : "ROUTING");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::WARNING_CLR), 0);
        setPanelTone(_identityPanel, Theme::BG_ELEVATED, Theme::WARNING_CLR);
    } else {
        lv_label_set_text(_lblStatus, "OFFLINE");
        lv_obj_set_style_text_color(_lblStatus, lv_color_hex(Theme::ERROR_CLR), 0);
        setPanelTone(_identityPanel, Theme::BG_ELEVATED, Theme::BORDER);
    }

    int online = 0;
    if (_am) {
        online = _am->nodesOnlineSince(1800000);
    }
    lv_label_set_text_fmt(_lblNodes, "%d", online);
    lv_obj_set_style_text_color(_lblNodes, lv_color_hex(
        online > 0 ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
    setPanelTone(_statNodes, online > 0 ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED,
                 online > 0 ? Theme::PRIMARY : Theme::BORDER);

    bool audioOn = !_cfg || _cfg->settings().audioEnabled;
    lv_label_set_text(_lblPaths, audioOn ? "ON" : "OFF");
    lv_obj_set_style_text_color(_lblPaths, lv_color_hex(
        audioOn ? Theme::SUCCESS : Theme::TEXT_MUTED), 0);
    setPanelTone(_statPaths, audioOn ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED,
                 audioOn ? Theme::PRIMARY : Theme::BORDER);

#if HAS_GPS
    bool gpsOn = _cfg && _cfg->settings().gpsTimeEnabled;
    if (_gps && gpsOn) {
        // Sticky checkpoint counter (see LvHomeScreen.h comment). This is
        // deliberately decoupled from GPSManager::fixAgeMs(), which resets
        // on every ~1Hz NMEA sentence and would otherwise make the UI
        // jitter between small values every refresh.
        static constexpr unsigned long CHECK_WINDOW_MS = 30000; // 30s per spec
        bool receivingNow = _gps->hasLocationFix();
        unsigned long nowMs = millis();

        if (receivingNow && !_gpsCheckpointSet) {
            // First fix ever, or first fix after having gone fully idle —
            // start a fresh 0s..30s cycle.
            _gpsCheckpointMs = nowMs;
            _gpsCheckpointSet = true;
        }

        unsigned long ageMs = _gpsCheckpointSet ? (nowMs - _gpsCheckpointMs) : 0;
        if (_gpsCheckpointSet && ageMs >= CHECK_WINDOW_MS) {
            if (receivingNow) {
                // Checkpoint reached and still receiving -> reset to FIX 0s.
                _gpsCheckpointMs = nowMs;
                ageMs = 0;
            }
            // else: not receiving -> leave checkpoint alone; ageMs keeps
            // growing past 30s uninterrupted (STALE 31s, 32s, ...).
        }

        bool everFixed = _gpsCheckpointSet;
        bool stale = everFixed && (ageMs >= CHECK_WINDOW_MS);
        String status;
        uint32_t color;
        if (!everFixed) {
            status = "NO FIX";
            color = Theme::TEXT_MUTED;
        } else if (!stale) {
            status = "FIX";
            if (_gps->satellites() > 0) {
                status += " (" + String(_gps->satellites()) + " sat)";
            }
            color = Theme::SUCCESS;
        } else {
            status = "STALE";
            color = Theme::WARNING_CLR;
        }
        if (everFixed) {
            char ageStr[16];
            uint32_t secs = ageMs / 1000;
            if (secs < 60) snprintf(ageStr, sizeof(ageStr), "%lus", (unsigned long)secs);
            else snprintf(ageStr, sizeof(ageStr), "%lum", (unsigned long)(secs/60));
            status += " ";
            status += ageStr;
        }
        lv_label_set_text(_lblLinks, status.c_str());
        lv_obj_set_style_text_color(_lblLinks, lv_color_hex(color), 0);
        uint32_t border = (everFixed && !stale) ? Theme::SUCCESS : (everFixed ? Theme::WARNING_CLR : Theme::BORDER);
        setPanelTone(_statLinks, gpsOn ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED, border);
    } else {
        lv_label_set_text(_lblLinks, gpsOn ? "ON" : "OFF");
        lv_obj_set_style_text_color(_lblLinks, lv_color_hex(
            gpsOn ? Theme::SUCCESS : Theme::TEXT_MUTED), 0);
        setPanelTone(_statLinks, gpsOn ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED,
                      gpsOn ? Theme::PRIMARY : Theme::BORDER);
    }
#else
    lv_label_set_text(_lblLinks, "OFF");
    lv_obj_set_style_text_color(_lblLinks, lv_color_hex(Theme::TEXT_MUTED), 0);
    setPanelTone(_statLinks, Theme::BG_ELEVATED, Theme::BORDER);
#endif

    if (!_rns) {
        lv_label_set_text(_lblSummary, "Identity unavailable");
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::WARNING_CLR), 0);
    } else if (!reachable && transportActive) {
        lv_label_set_text(_lblSummary, "Transport active, no link");
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::WARNING_CLR), 0);
    } else if (!reachable) {
        lv_label_set_text(_lblSummary, "No active transport");
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::ERROR_CLR), 0);
    } else if (online == 0) {
        lv_label_set_text(_lblSummary, "Listening for peers");
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    } else if (online == 1) {
        lv_label_set_text(_lblSummary, "1 peer heard in 30m");
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::ACCENT), 0);
    } else {
        lv_label_set_text_fmt(_lblSummary, "%d peers heard in 30m", online);
        lv_obj_set_style_text_color(_lblSummary, lv_color_hex(Theme::ACCENT), 0);
    }

    if (_rns && _rns->lastAnnounceTime() > 0) {
        char age[16];
        formatAge(now - _rns->lastAnnounceTime(), age, sizeof(age));
        lv_label_set_text_fmt(_lblLastAnnounce, "Announced: %s", age);
    } else {
        lv_label_set_text(_lblLastAnnounce, "Announced: never");
    }
}

bool LvHomeScreen::handleKey(const KeyEvent& event) {
    if (event.enter || event.character == '\n' || event.character == '\r') {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused == _statPaths) {
            toggleAudio();
            return true;
        }
        if (focused == _chipLora) {
            toggleLora();
            return true;
        }
        if (focused == _chipTcp) {
            toggleTcp();
            return true;
        }
        if (focused == _chipWifi) {
            toggleWiFi();
            return true;
        }
        if (focused == _statLinks) {
            toggleGPS();
            return true;
        }
        if (focused == _statNodes) {
            openPeers();
            return true;
        }
        if (_announceCb) _announceCb();
        return true;
    }
    return false;
}

void LvHomeScreen::toggleAudio() {
    if (_audioToggleCb) _audioToggleCb();
    forceRefresh();
}

void LvHomeScreen::toggleLora() {
    if (_loraToggleCb) _loraToggleCb();
    forceRefresh();
}

void LvHomeScreen::toggleTcp() {
    if (_tcpToggleCb) _tcpToggleCb();
    forceRefresh();
}

void LvHomeScreen::toggleWiFi() {
    if (_wifiToggleCb) _wifiToggleCb();
    forceRefresh();
}

void LvHomeScreen::toggleGPS() {
    if (_gpsToggleCb) _gpsToggleCb();
    forceRefresh();
}

void LvHomeScreen::openPeers() {
    if (_peersCb) _peersCb();
}

void LvHomeScreen::forceRefresh() {
    _lastRefreshMs = 0;
    _lastUptime = ULONG_MAX;
    _lastHeap = UINT32_MAX;
    refreshUI();
}

void LvHomeScreen::renderAvatar(const String& seed) {
    if (seed.isEmpty() || seed == _avatarSeed || !_avatarCanvas) return;
    if (LxmFaceAvatar::render(_avatarCanvas, seed)) {
        _avatarSeed = seed;
    }
}
