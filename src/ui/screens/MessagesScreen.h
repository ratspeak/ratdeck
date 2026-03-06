#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <string>

class LXMFManager;
class AnnounceManager;

class MessagesScreen : public Screen {
public:
    using OpenCallback = std::function<void(const std::string& peerHex)>;

    void update() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setOpenCallback(OpenCallback cb) { _onOpen = cb; }

    const char* title() const override { return "Messages"; }
    void draw(LGFX_TDeck& gfx) override;

private:
    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    OpenCallback _onOpen;
    int _lastConvCount = -1;
    int _selectedIdx = 0;
};
