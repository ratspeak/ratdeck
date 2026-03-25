#pragma once

#include "ui/UIManager.h"
#include <functional>

struct TimezoneEntry {
    const char* label;      // Display name (e.g., "New York (EST/EDT)")
    const char* posixTZ;    // POSIX TZ string for setenv("TZ", ...)
    int8_t baseOffset;      // Base UTC offset for display (e.g., -5 for EST)
};

// Major world timezones with DST rules where applicable
static const TimezoneEntry TIMEZONE_TABLE[] = {
    {"Honolulu",              "HST10",                                    -10},
    {"Anchorage",             "AKST9AKDT,M3.2.0,M11.1.0",               -9},
    {"Los Angeles",           "PST8PDT,M3.2.0,M11.1.0",                 -8},
    {"Denver",                "MST7MDT,M3.2.0,M11.1.0",                 -7},
    {"Phoenix",               "MST7",                                    -7},
    {"Chicago",               "CST6CDT,M3.2.0,M11.1.0",                 -6},
    {"New York",              "EST5EDT,M3.2.0,M11.1.0",                  -5},
    {"San Juan",              "AST4",                                     -4},
    {"Sao Paulo",             "<-03>3",                                   -3},
    {"London",                "GMT0BST,M3.5.0/1,M10.5.0",                0},
    {"Paris",                 "CET-1CEST,M3.5.0,M10.5.0/3",              1},
    {"Cairo",                 "EET-2",                                    2},
    {"Moscow",                "MSK-3",                                    3},
    {"Dubai",                 "<+04>-4",                                  4},
    {"Karachi",               "PKT-5",                                    5},
    {"Kolkata",               "IST-5:30",                                 5},
    {"Bangkok",               "<+07>-7",                                  7},
    {"Singapore",             "<+08>-8",                                  8},
    {"Tokyo",                 "JST-9",                                    9},
    {"Sydney",                "AEST-10AEDT,M10.1.0,M4.1.0/3",           10},
    {"Auckland",              "NZST-12NZDT,M9.5.0,M4.1.0/3",            12},
};

static constexpr int TIMEZONE_COUNT = sizeof(TIMEZONE_TABLE) / sizeof(TIMEZONE_TABLE[0]);

class LvTimezoneScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    bool handleKey(const KeyEvent& event) override;
    void onEnter() override { _enterTime = millis(); }
    const char* title() const override { return "Timezone"; }

    void setDoneCallback(std::function<void(int tzIndex)> cb) { _doneCb = cb; }

    // Pre-select an index (e.g., from saved config)
    void setSelectedIndex(int idx) { _selectedIdx = idx; }

private:
    void updateSelection(int oldIdx, int newIdx);

    std::function<void(int)> _doneCb;
    int _selectedIdx = 6;  // Default: New York (EST/EDT)
    unsigned long _enterTime = 0;
    static constexpr unsigned long ENTER_GUARD_MS = 600;

    // LVGL widgets
    lv_obj_t* _scrollContainer = nullptr;
    static constexpr int VISIBLE_ROWS = 5;
    static constexpr int ROW_H = 28;
};
