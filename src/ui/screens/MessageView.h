#pragma once

#include "ui/UIManager.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <vector>

class LXMFManager;
class AnnounceManager;

// Pre-computed layout for a single message (may span multiple lines)
struct MsgLayout {
    int msgIdx;              // index into _cachedMsgs
    std::vector<std::string> lines;  // word-wrapped lines
    std::string timeStr;     // "12:34" or "HH:MM"
    bool incoming;
    int totalHeight;         // pixel height for this message block
};

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
    void refreshMessages();
    void rebuildLayout();

    static std::vector<std::string> wordWrap(const std::string& text, int maxChars);
    static std::string formatTime(double timestamp);

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    int _lastMsgCount = -1;
    int _scrollPixels = 0;   // pixel scroll from bottom
    int _totalContentH = 0;  // total height of all messages
    std::vector<LXMFMessage> _cachedMsgs;
    std::vector<MsgLayout> _layout;
    unsigned long _lastRefreshMs = 0;
    static constexpr unsigned long REFRESH_INTERVAL_MS = 500;
    static constexpr int CHAR_W = 6;
    static constexpr int LINE_H = 10;
    static constexpr int BUBBLE_PAD = 3;
    static constexpr int BUBBLE_GAP = 4;
    static constexpr int MAX_BUBBLE_CHARS = 38;
};
