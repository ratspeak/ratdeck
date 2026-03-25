#pragma once

#include "ui/UIManager.h"
#include <functional>

class LvNameInputScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Setup"; }

    void setDoneCallback(std::function<void(const String&)> cb) { _doneCb = cb; }
    void onEnter() override { _enterTime = millis(); }

    static constexpr int MAX_NAME_LEN = 16;

private:
    lv_obj_t* _textarea = nullptr;
    std::function<void(const String&)> _doneCb;
    unsigned long _enterTime = 0;
    static constexpr unsigned long ENTER_GUARD_MS = 600;  // Ignore Enter for 600ms after screen appears
};
