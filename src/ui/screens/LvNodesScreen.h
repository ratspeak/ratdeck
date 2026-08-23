#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <string>
#include <vector>

class AnnounceManager;
class UserConfig;

class LvNodesScreen : public LvScreen {
public:
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;
    // Send GPS to a peer. Callback is expected to format the current
    // location as `LOC lat lon` and dispatch via LXMF (the screen knows
    // nothing about GPS/LXMF internals — main.cpp wires the deps).
    using SendGpsCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setSendGpsCallback(SendGpsCallback cb) { _onSendGps = cb; }
    bool handleLongPress() override;

    const char* title() const override { return "Peers"; }

private:
    void rebuildList();
    int getFocusedNodeIdx() const;

    // Action modal helpers
    enum class NodeAction { BROWSE, ACTION_MENU, NICKNAME_INPUT };
    void showActionMenu(int nodeIdx);
    void hideOverlay();
    void showNicknameInput();
    void updateMenuSelection();
    void updateNicknameDisplay();
    void updateOverlayDetails(const char* title);
    // Number of menu entries visible in the current action-menu state.
    // 3 for unsaved peers (Save/Message/Close), 4 for saved contacts
    // (Message/Send GPS/Edit Name/Close). Drives the keyboard nav cap
    // and the "show/hide the 4th button" layout.
    int menuEntryCount() const;

    AnnounceManager* _am = nullptr;
    class UIManager* _ui = nullptr;
    UserConfig* _cfg = nullptr;
    NodeSelectedCallback _onSelect;
    SendGpsCallback _onSendGps;
    bool _confirmDelete = false;
    bool _focusActive = false;

    // Action modal state
    NodeAction _actionState = NodeAction::BROWSE;
    int _menuIdx = 0;
    int _actionNodeIdx = -1;
    String _nicknameText;

    // Overlay widgets — menu supports up to 4 entries (saved-contact
    // path adds Send GPS on top of the original 3). Unsaved peers use
    // only 3 of the 4 slots; the 4th is hidden via LV_OBJ_FLAG_HIDDEN.
    static constexpr int MAX_MENU_ENTRIES = 4;
    lv_obj_t* _overlay = nullptr;
    lv_obj_t* _overlayTitle = nullptr;
    lv_obj_t* _overlayMeta = nullptr;
    lv_obj_t* _overlayReach = nullptr;
    lv_obj_t* _menuLabels[MAX_MENU_ENTRIES] = {};
    lv_obj_t* _menuBtns[MAX_MENU_ENTRIES] = {};
    lv_obj_t* _nicknameBox = nullptr;
    lv_obj_t* _nicknameLbl = nullptr;
    lv_obj_t* _nicknameHint = nullptr;
    int _lastNodeCount = -1;
    int _lastContactCount = -1;

    // Sorted index vectors (into _am->nodes())
    std::vector<int> _sortedContactIndices;
    std::vector<int> _sortedOnlineIndices;

    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 5000;
    static constexpr unsigned long AGE_REBUILD_INTERVAL_MS = 30000;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
