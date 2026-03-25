#pragma once

#include <lvgl.h>
#include <string>

class LvStatusBar {
public:
    void create(lv_obj_t* parent);
    void update();

    // Status setters (kept for API compatibility, no bars drawn)
    void setLoRaOnline(bool online) { _loraOnline = online; }
    void setBLEActive(bool active) { _bleActive = active; }
    void setBLEEnabled(bool enabled) { _bleEnabled = enabled; }
    void setWiFiActive(bool active) { _wifiActive = active; }
    void setWiFiEnabled(bool enabled) { _wifiEnabled = enabled; }
    void setTCPConnected(bool connected) { _tcpConnected = connected; }
    void setGPSFix(bool hasFix);
    void setBatteryPercent(int pct);
    void setTransportMode(const char* mode);
    void flashAnnounce();
    void showToast(const char* msg, uint32_t durationMs = 1500);

    // Time display
    void setUse24Hour(bool use24h) { _use24h = use24h; }
    void updateTime();   // Call at 1 Hz to refresh clock

    lv_obj_t* obj() { return _bar; }

private:
    lv_obj_t* _bar = nullptr;
    lv_obj_t* _lblTime = nullptr;    // Top-left: current time
    lv_obj_t* _lblBrand = nullptr;   // Center: "Ratspeak.org"
    lv_obj_t* _lblBatt = nullptr;    // Right: battery %
    lv_obj_t* _toast = nullptr;
    lv_obj_t* _lblToast = nullptr;

    bool _loraOnline = false;
    bool _bleActive = false;
    bool _bleEnabled = false;
    bool _wifiActive = false;
    bool _wifiEnabled = false;
    bool _tcpConnected = false;
    bool _gpsFix = false;
    bool _use24h = false;
    int _battPct = -1;
    unsigned long _announceFlashEnd = 0;
    unsigned long _toastEnd = 0;
};
