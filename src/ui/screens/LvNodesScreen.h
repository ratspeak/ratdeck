#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <string>
#include <vector>

class AnnounceManager;

class LvNodesScreen : public LvScreen {
public:
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    bool handleLongPress() override;

    const char* title() const override { return "Nodes"; }

private:
    void rebuildList();
    void updateSelection(int oldIdx, int newIdx);
    void scrollToSelected();

    AnnounceManager* _am = nullptr;
    class UIManager* _ui = nullptr;
    NodeSelectedCallback _onSelect;
    bool _confirmDelete = false;
    int _lastNodeCount = -1;
    int _lastContactCount = -1;
    int _selectedIdx = 0;
    int _totalRows = 0;

    // Section tracking
    bool _contactsCollapsed = false;
    int _contactHeaderIdx = -1;   // Row index of "Contacts" header
    int _onlineHeaderIdx = -1;    // Row index of "Online" header
    std::vector<int> _rowToNodeIdx; // Maps row index -> node index in _am->nodes(), -1 for headers

    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 2000;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _lblEmpty = nullptr;
    std::vector<lv_obj_t*> _rows;
};
