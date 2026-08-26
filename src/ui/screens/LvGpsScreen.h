#pragma once

#include "ui/UIManager.h"
#include <functional>

// =============================================================================
// GPS app — full-screen (tab bar hidden). Basic fix status + coordinates in
// DD / DM / DMS / Plus Code / UTM. BACK → Apps. OPEN MAP → Map app-mode.
// =============================================================================

class GPSManager;

class LvGpsScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;

    void setGps(GPSManager* gps) { _gps = gps; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    using OpenMapCallback = std::function<void()>;
    void setOpenMapCallback(OpenMapCallback cb) { _onOpenMap = cb; }

    const char* title() const override { return "GPS"; }

private:
    void rebuild();

    class UIManager* _ui = nullptr;
    GPSManager* _gps = nullptr;
    BackCallback _onBack;
    OpenMapCallback _onOpenMap;

    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _body = nullptr;
    lv_obj_t* _refreshBtn = nullptr;
    lv_obj_t* _mapBtn = nullptr;
    lv_timer_t* _timer = nullptr;
};
