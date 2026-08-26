#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <string>
#include <vector>

class AnnounceManager;

// Contacts tab — saved contacts only. Tap opens full action modal
// (Message / Send GPS / Edit Name / Remove / Close), matching Pro.
class LvContactsScreen : public LvScreen {
public:
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;
    using SendGpsCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setSendGpsCallback(SendGpsCallback cb) { _onSendGps = cb; }
    void setShowQrCallback(std::function<void()> cb) { _showQrCb = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    bool handleLongPress() override;

    const char* title() const override { return "Contacts"; }

private:
    void rebuildList();
    void showActionMenu(int listIdx);
    void hideOverlay();
    void showNicknameInput();
    void updateMenuSelection();
    void updateNicknameDisplay();
    void updateOverlayDetails(const char* title);
    int menuEntryCount() const { return 5; }  // Msg/GPS/Edit/Remove/Close
    int nodeIdxFromList(int listIdx) const;

    AnnounceManager* _am = nullptr;
    class UIManager* _ui = nullptr;
    NodeSelectedCallback _onSelect;
    SendGpsCallback _onSendGps;
    std::function<void()> _showQrCb;
    bool _confirmDelete = false;
    bool _focusActive = false;
    int _deleteIdx = -1;
    int _lastContactCount = -1;
    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 30000;
    std::vector<int> _contactIndices;
    std::vector<std::vector<uint8_t>> _avatarBuffers;

    enum class ContactAction { BROWSE, ACTION_MENU, NICKNAME_INPUT };
    ContactAction _actionState = ContactAction::BROWSE;
    int _menuIdx = 0;
    int _actionListIdx = -1;  // index into _contactIndices
    String _nicknameText;

    static constexpr int MAX_MENU_ENTRIES = 5;
    lv_obj_t* _overlay = nullptr;
    lv_obj_t* _overlayTitle = nullptr;
    lv_obj_t* _overlayMeta = nullptr;
    lv_obj_t* _overlayReach = nullptr;
    lv_obj_t* _menuLabels[MAX_MENU_ENTRIES] = {};
    lv_obj_t* _menuBtns[MAX_MENU_ENTRIES] = {};
    lv_obj_t* _nicknameBox = nullptr;
    lv_obj_t* _nicknameLbl = nullptr;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
