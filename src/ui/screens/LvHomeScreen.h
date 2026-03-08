#pragma once

#include "ui/UIManager.h"
#include <functional>

class ReticulumManager;
class SX1262;
class UserConfig;
class LXMFManager;
class AnnounceManager;

class LvHomeScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setReticulumManager(ReticulumManager* rns) { _rns = rns; }
    void setRadio(SX1262* radio) { _radio = radio; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setRadioOnline(bool online) { _radioOnline = online; }
    void setAnnounceCallback(std::function<void()> cb) { _announceCb = cb; }

    const char* title() const override { return "Home"; }

private:
    ReticulumManager* _rns = nullptr;
    SX1262* _radio = nullptr;
    UserConfig* _cfg = nullptr;
    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    bool _radioOnline = false;
    std::function<void()> _announceCb;
    unsigned long _lastUptime = 0;
    uint32_t _lastHeap = 0;

    lv_obj_t* _lblName = nullptr;
    lv_obj_t* _lblId = nullptr;
    lv_obj_t* _lblStatus = nullptr;
    lv_obj_t* _lblNodes = nullptr;
};
