#pragma once

#include "ui/UIManager.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <vector>

class LXMFManager;
class AnnounceManager;
class GPSManager;

class LvMessageView : public LvScreen {
public:
    using BackCallback = std::function<void()>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;

    void setPeerHex(const std::string& hex) { _peerHex = hex; }
    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setGPSManager(GPSManager* gps) { _gps = gps; }
    void setUserConfig(class UserConfig* cfg) { _cfg = cfg; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }

private:
    void sendCurrentMessage(bool viaLink = false);
    // Send the current GPS fix as a `LOC lat lon` message to the active
    // peer. Gates on: GPS has a fix, gpsLocationEnabled is on, the peer
    // is a saved contact. Toasts failure reasons; never logs coords.
    void sendGpsLocation();
    void rebuildMessages();
    void appendMessage(const LXMFMessage& msg);
    std::string getPeerName();
    void updateHeader();
    void markVisibleConversationRead();
    void updateComposerState();
    void refreshComposerPlaceholder();
    void updateComposerText();
    void showSendModeMenu();
    void hideSendModeMenu();
    void updateSendModeMenu();
    void chooseSendMode(int idx);
    // True iff Send GPS is currently available (GPS fix, location on,
    // saved contact). Drives both the menu visibility and the entry-cap.
    bool canSendGps() const;
    // Number of rows in the send-mode menu — 3 normally, 4 when Send GPS
    // is available. Drives keyboard nav cap + overlay height.
    int sendMenuEntryCount() const { return canSendGps() ? 4 : 3; }

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    GPSManager* _gps = nullptr;
    class UserConfig* _cfg = nullptr;
    class UIManager* _ui = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    int _lastMsgCount = -1;
    int _knownTotalCount = -1;
    unsigned long _lastRefreshMs = 0;
    std::vector<LXMFMessage> _cachedMsgs;
    bool _markReadPending = false;

    void updateMessageStatus(int msgIdx, LXMFStatus status);
    static void applyStatusGlyph(lv_obj_t* lbl, LXMFStatus status);

    // LVGL widgets
    lv_obj_t* _header = nullptr;
    lv_obj_t* _lblHeader = nullptr;
    lv_obj_t* _lblHeaderMeta = nullptr;
    lv_obj_t* _lblHeaderState = nullptr;
    lv_obj_t* _msgScroll = nullptr;
    lv_obj_t* _inputRow = nullptr;
    lv_obj_t* _textarea = nullptr;
    // Send GPS pill — sits left of SEND in the composer row, like Pro
    // Thread's GPS shortcut. Long-press of SEND still opens the legacy
    // send-mode menu (including "Send as link"); this pill is the
    // dedicated one-tap path for `LOC lat lon` so the user never has to
    // rely on a long-press gesture that may not fire on resistive touch.
    lv_obj_t* _btnGps = nullptr;
    lv_obj_t* _btnSend = nullptr;
    lv_obj_t* _sendOverlay = nullptr;
    // Send-mode menu — 4 rows when Send GPS is available, otherwise 3.
    static constexpr int MAX_SEND_MENU_ENTRIES = 4;
    lv_obj_t* _sendRows[MAX_SEND_MENU_ENTRIES] = {};
    lv_obj_t* _sendLabels[MAX_SEND_MENU_ENTRIES] = {};
    int _sendMenuIdx = 0;
    bool _suppressNextSendClick = false;

    // Per-message status labels for partial updates (avoids full rebuild)
    std::vector<lv_obj_t*> _statusLabels;
    std::vector<lv_obj_t*> _textLabels;
    std::vector<lv_obj_t*> _bubbleBoxes;

    static constexpr unsigned long REFRESH_INTERVAL_MS = 2000;  // Check for new messages every 2s
    static constexpr size_t CHAT_VIEW_MAX_MESSAGES = 40;
    static constexpr size_t MAX_COMPOSER_CHARS = 120;
};
