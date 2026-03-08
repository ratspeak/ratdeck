#pragma once

#include <lvgl.h>
#include <string>

class LvStatusBar {
public:
    void create(lv_obj_t* parent);
    void update();

    // Status indicators: green=active, yellow=enabled-but-unconfigured, red=off
    void setLoRaOnline(bool online);
    void setBLEActive(bool active);
    void setBLEEnabled(bool enabled) { _bleEnabled = enabled; refreshIndicators(); }
    void setWiFiActive(bool active);
    void setWiFiEnabled(bool enabled) { _wifiEnabled = enabled; refreshIndicators(); }
    void setBatteryPercent(int pct);
    void setTransportMode(const char* mode);
    void flashAnnounce();
    void showToast(const char* msg, uint32_t durationMs = 1500);

    lv_obj_t* obj() { return _bar; }

private:
    void refreshIndicators();

    lv_obj_t* _bar = nullptr;
    lv_obj_t* _bars[3] = {};
    lv_obj_t* _lblBrand = nullptr;
    lv_obj_t* _lblBatt = nullptr;
    lv_obj_t* _toast = nullptr;
    lv_obj_t* _lblToast = nullptr;

    bool _loraOnline = false;
    bool _bleActive = false;
    bool _bleEnabled = false;
    bool _wifiActive = false;
    bool _wifiEnabled = false;
    int _battPct = -1;
    unsigned long _announceFlashEnd = 0;
    unsigned long _toastEnd = 0;
};
