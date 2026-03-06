#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <string>

class LXMFManager;
class AnnounceManager;

class MessageView : public Screen {
public:
    using BackCallback = std::function<void()>;

    void update() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;

    void setPeerHex(const std::string& hex) { _peerHex = hex; }
    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }
    void draw(LGFX_TDeck& gfx) override;

private:
    void sendCurrentMessage();

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    int _lastMsgCount = -1;
    int _scrollOffset = 0;  // scroll from bottom
};
