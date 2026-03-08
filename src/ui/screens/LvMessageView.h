#pragma once

#include "ui/UIManager.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <vector>

class LXMFManager;
class AnnounceManager;

class LvMessageView : public LvScreen {
public:
    using BackCallback = std::function<void()>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;

    void setPeerHex(const std::string& hex) { _peerHex = hex; }
    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }

private:
    void sendCurrentMessage();
    void rebuildMessages();
    std::string getPeerName();

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    class UIManager* _ui = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    int _lastMsgCount = -1;
    unsigned long _lastRefreshMs = 0;
    std::vector<LXMFMessage> _cachedMsgs;

    // LVGL widgets
    lv_obj_t* _header = nullptr;
    lv_obj_t* _lblHeader = nullptr;
    lv_obj_t* _msgScroll = nullptr;
    lv_obj_t* _inputRow = nullptr;
    lv_obj_t* _textarea = nullptr;
    lv_obj_t* _btnSend = nullptr;

    static constexpr unsigned long REFRESH_INTERVAL_MS = 2000;  // Check for new messages every 2s
};
