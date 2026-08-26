#include "LvStatusBar.h"
#include "Theme.h"
#include "LvTheme.h"
#include "config/Config.h"
#include <Arduino.h>
#include <cstdio>
#include <time.h>
#include "fonts/fonts.h"

namespace {

constexpr int kTimeW = 50;
constexpr int kBattW = 44;
constexpr int kLinksX = 56;
constexpr int kLinksW = Theme::SCREEN_W - kLinksX - kBattW - 8;

int normalizedBattery(int pct) {
    if (pct < 0) return -1;
    if (pct > 100) return 100;
    return pct;
}

}  // namespace

void LvStatusBar::create(lv_obj_t* parent) {
    _bar = lv_obj_create(parent);
    lv_obj_set_size(_bar, Theme::SCREEN_W, Theme::STATUS_BAR_H);
    lv_obj_align(_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_style(_bar, LvTheme::styleStatusBar(), 0);
    lv_obj_clear_flag(_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Left: time. Fixed width keeps the center indicators stable.
    _lblTime = lv_label_create(_bar);
    lv_obj_set_size(_lblTime, kTimeW, Theme::STATUS_BAR_H);
    lv_label_set_long_mode(_lblTime, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lblTime, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_align(_lblTime, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(_lblTime, "--:--");
    lv_obj_align(_lblTime, LV_ALIGN_LEFT_MID, 4, 0);

    // Center: brand/status text. Transport states are shown on Home.
    _lblLinks = lv_label_create(_bar);
    lv_obj_set_size(_lblLinks, kLinksW, Theme::STATUS_BAR_H);
    lv_label_set_long_mode(_lblLinks, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(_lblLinks, true);
    lv_obj_set_style_text_font(_lblLinks, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_align(_lblLinks, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_lblLinks, LV_ALIGN_LEFT_MID, kLinksX, 0);

    // Right: battery percentage.
    _lblBatt = lv_label_create(_bar);
    lv_obj_set_size(_lblBatt, kBattW, Theme::STATUS_BAR_H);
    lv_obj_set_style_text_font(_lblBatt, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_align(_lblBatt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(_lblBatt, LV_LABEL_LONG_CLIP);
    lv_obj_align(_lblBatt, LV_ALIGN_RIGHT_MID, -4, 0);

    // Right: battery bar
    _barBatt = lv_bar_create(_bar);
    lv_obj_set_size(_barBatt, 20, 8);
    lv_bar_set_range(_barBatt, 0, 100);
    lv_bar_set_value(_barBatt, 0, LV_ANIM_OFF);
    lv_obj_align(_barBatt, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(_barBatt, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_barBatt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_barBatt, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_barBatt, LV_OPA_COVER, LV_PART_INDICATOR);

    // Toast overlay (hidden by default). It is a child of the bar so boot mode
    // and other shell visibility changes affect it with the status bar.
    _toast = lv_obj_create(_bar);
    lv_obj_set_size(_toast, Theme::SCREEN_W, Theme::STATUS_BAR_H);
    lv_obj_align(_toast, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_style(_toast, LvTheme::styleStatusToast(), 0);
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_toast, LV_OBJ_FLAG_HIDDEN);

    _lblToast = lv_label_create(_toast);
    lv_obj_set_width(_lblToast, Theme::SCREEN_W - 12);
    lv_label_set_long_mode(_lblToast, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(_lblToast, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_lblToast, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_text_align(_lblToast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_lblToast);
    lv_label_set_text(_lblToast, "");

    refreshTimeColor();
    refreshIndicators();
    refreshBattery();
}

void LvStatusBar::update() {
    if (_announceFlashEnd > 0 && millis() >= _announceFlashEnd) {
        _announceFlashEnd = 0;
        refreshIndicators();
    }
    if (_toast && _toastEnd > 0 && millis() >= _toastEnd) {
        _toastEnd = 0;
        lv_obj_add_flag(_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void LvStatusBar::updateTime() {
    if (!_lblTime) return;
    time_t now = time(nullptr);
    if (now <= 1700000000) {
        if (_lastHour != -2) {
            lv_label_set_text(_lblTime, "--:--");
            _lastHour = -2;
            _lastMinute = -2;
            refreshTimeColor();
        }
        return;
    }
    struct tm* local = localtime(&now);
    if (!local) {
        if (_lastHour != -2) {
            lv_label_set_text(_lblTime, "--:--");
            _lastHour = -2;
            _lastMinute = -2;
            refreshTimeColor();
        }
        return;
    }
    if (local->tm_hour == _lastHour && local->tm_min == _lastMinute) return;
    _lastHour = local->tm_hour;
    _lastMinute = local->tm_min;
    char buf[8];
    if (_use24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d", local->tm_hour, local->tm_min);
    } else {
        int h = local->tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%d:%02d", h, local->tm_min);
    }
    lv_label_set_text(_lblTime, buf);
    refreshTimeColor();
}

void LvStatusBar::setLoRaOnline(bool online) {
    if (_loraOnline == online) return;
    _loraOnline = online;
    refreshIndicators();
}

void LvStatusBar::setBLEActive(bool active) {
    if (_bleActive == active) return;
    _bleActive = active;
    refreshIndicators();
}

void LvStatusBar::setBLEEnabled(bool enabled) {
    if (_bleEnabled == enabled) return;
    _bleEnabled = enabled;
    refreshIndicators();
}

void LvStatusBar::setWiFiActive(bool active) {
    if (_wifiActive == active) return;
    _wifiActive = active;
    refreshIndicators();
}

void LvStatusBar::setWiFiEnabled(bool enabled) {
    if (_wifiEnabled == enabled) return;
    _wifiEnabled = enabled;
    refreshIndicators();
}

void LvStatusBar::setTCPConnected(bool connected) {
    if (_tcpConnected == connected) return;
    _tcpConnected = connected;
    refreshIndicators();
}

void LvStatusBar::setAutoIfacePeers(int n) {
    if (n < -1) n = -1;
    if (_autoIfacePeers == n) return;
    _autoIfacePeers = n;
    refreshIndicators();
}

void LvStatusBar::setGPSFix(bool hasFix) {
    if (_gpsFix == hasFix) return;
    _gpsFix = hasFix;
    refreshIndicators();
    refreshTimeColor();
}

void LvStatusBar::setBatteryPercent(int pct) {
    pct = normalizedBattery(pct);

    // Suppress single-percent ADC noise: only update if the value changed by 2% or more
    if (_battPct >= 0 && abs(pct - _battPct) < 2) return;

    _battPct = pct;
    refreshBattery();
}

void LvStatusBar::setBatteryDisplay(uint8_t mode) {
    if (_battDisplay == mode) return;
    _battDisplay = mode;
    refreshBattery();
}

void LvStatusBar::flashAnnounce() {
    _announceFlashEnd = millis() + 1000;
    refreshIndicators();
}

void LvStatusBar::showToast(const char* msg, uint32_t durationMs) {
    if (!_toast || !_lblToast) return;
    lv_label_set_text(_lblToast, msg);
    _toastEnd = millis() + durationMs;
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_toast);
}

void LvStatusBar::setUse24Hour(bool use24h) {
    if (_use24h == use24h) return;
    _use24h = use24h;
    _lastHour = -1;
    _lastMinute = -1;
    updateTime();
}

void LvStatusBar::applyTheme() {
    if (_lblToast) lv_obj_set_style_text_color(_lblToast, lv_color_hex(Theme::BG), 0);
    refreshIndicators();
    refreshBattery();
    refreshTimeColor();
}

void LvStatusBar::refreshIndicators() {
    if (!_lblLinks) return;

    const bool txFlash = _announceFlashEnd > 0 && millis() < _announceFlashEnd;
    const uint32_t brandColor = txFlash ? Theme::WARNING_CLR : Theme::ACCENT;
    char buf[160];
    snprintf(buf, sizeof(buf), "#%06X " RSDECK_BRAND_TEXT "#", (unsigned int)brandColor);
    lv_label_set_text(_lblLinks, buf);

    if (_bar) {
        lv_obj_set_style_bg_color(_bar,
            lv_color_hex(txFlash ? Theme::STATUS_FLASH : Theme::STATUS_BG), 0);
        lv_obj_set_style_border_color(_bar,
            lv_color_hex(txFlash ? Theme::PRIMARY : Theme::DIVIDER), 0);
    }
}

void LvStatusBar::refreshBattery() {
    if (!_lblBatt || !_barBatt) return;

    if(_battDisplay == 1){
        // Bar mode
        lv_obj_clear_flag(_barBatt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_lblBatt, LV_OBJ_FLAG_HIDDEN);

        lv_bar_set_value(_barBatt, _battPct < 0 ? 0 : _battPct, LV_ANIM_OFF);

        uint32_t indicatorCol;
        if (_isCharging) {
            indicatorCol = Theme::SUCCESS;

        } else if (_battPct >= 0 && _battPct <= 10) {
            indicatorCol = Theme::ERROR_CLR;
        } else if (_battPct >= 0 && _battPct <= 20) {
            indicatorCol = Theme::WARNING_CLR;

        } else {
            indicatorCol = 0xFFFFFF;
        }
        lv_obj_set_style_bg_color(_barBatt, lv_color_hex(indicatorCol), LV_PART_INDICATOR);

    } else {
        // Percent mode
        lv_obj_clear_flag(_lblBatt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_barBatt, LV_OBJ_FLAG_HIDDEN);

        char buf[8];
        uint32_t col = Theme::TEXT_MUTED;
        if (_battPct < 0) {
            snprintf(buf, sizeof(buf), "--%%");
        } else if (_isCharging) {
            snprintf(buf, sizeof(buf), "+%d%%", _battPct);
            col = Theme::SUCCESS;
        } else {
            snprintf(buf, sizeof(buf), "%d%%", _battPct);
            col = Theme::TEXT_PRIMARY;
            if (_battPct <= 10)      col = Theme::ERROR_CLR;
            else if (_battPct <= 20) col = Theme::WARNING_CLR;
        }
        lv_label_set_text(_lblBatt, buf);
        lv_obj_set_style_text_color(_lblBatt, lv_color_hex(col), 0);
    }


}

void LvStatusBar::setCharging(bool charging) {
    if (_isCharging == charging) return;
    _isCharging = charging;
    refreshBattery();
}


void LvStatusBar::refreshTimeColor() {
    if (!_lblTime) return;
    const bool validTime = _lastHour >= 0;
    const uint32_t col = validTime
        ? (_gpsFix ? Theme::ACCENT : Theme::TEXT_PRIMARY)
        : Theme::TEXT_MUTED;
    lv_obj_set_style_text_color(_lblTime, lv_color_hex(col), 0);
}
