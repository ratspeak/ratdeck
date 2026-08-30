#pragma once

#include "ui/UIManager.h"
#include "reticulum/AnnounceManager.h"
#include <functional>
#include <string>
#include <vector>

class UserConfig;

class LvNodesScreen : public LvScreen {
public:
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    bool handleLongPress() override;

    const char* title() const override { return "Peers"; }

private:
    void rebuildList();
    int getFocusedNodeIdx() const;

    // Filter input helpers
    bool isFilterFocused() const;
    void updateFilterDisplay();
    bool matchesFilter(const DiscoveredNode& node) const;

    // Action modal helpers
    enum class NodeAction { BROWSE, ACTION_MENU, NICKNAME_INPUT };
    void showActionMenu(int nodeIdx);
    void hideOverlay();
    void showNicknameInput();
    void updateMenuSelection();
    void updateNicknameDisplay();
    void updateOverlayDetails(const char* title);

    AnnounceManager* _am = nullptr;
    class UIManager* _ui = nullptr;
    UserConfig* _cfg = nullptr;
    NodeSelectedCallback _onSelect;
    bool _confirmDelete = false;
    bool _focusActive = false;

    // Action modal state
    NodeAction _actionState = NodeAction::BROWSE;
    int _menuIdx = 0;
    std::string _actionNodeHex;
    String _nicknameText;

    // Overlay widgets
    lv_obj_t* _overlay = nullptr;
    lv_obj_t* _overlayTitle = nullptr;
    lv_obj_t* _overlayMeta = nullptr;
    lv_obj_t* _overlayReach = nullptr;
    lv_obj_t* _menuLabels[3] = {};
    lv_obj_t* _menuBtns[3] = {};
    lv_obj_t* _nicknameBox = nullptr;
    lv_obj_t* _nicknameLbl = nullptr;
    lv_obj_t* _nicknameHint = nullptr;
    int _lastNodeCount = -1;
    int _lastContactCount = -1;

    // UI-owned snapshot and sorted indices into it.
    std::vector<DiscoveredNode> _nodesSnapshot;
    std::vector<int> _sortedContactIndices;
    std::vector<int> _sortedOnlineIndices;

    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 5000;
    static constexpr unsigned long AGE_REBUILD_INTERVAL_MS = 30000;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
    lv_obj_t* _emptyTitle = nullptr;
    lv_obj_t* _emptyHint = nullptr;

    // Filter bar — fixed above the list, first object in the focus group
    lv_obj_t* _filterBar = nullptr;
    lv_obj_t* _filterLbl = nullptr;
    String _filterText;
};
