#include "LvNodesScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/UIManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <algorithm>

void LvNodesScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    // Empty state label
    _lblEmpty = lv_label_create(parent);
    lv_obj_set_style_text_font(_lblEmpty, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblEmpty, lv_color_hex(Theme::MUTED), 0);
    lv_label_set_text(_lblEmpty, "No nodes discovered");
    lv_obj_center(_lblEmpty);

    // Scrollable list container
    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(_list, 0, 0);
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_style_bg_color(_list, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_row(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

    _lastNodeCount = -1;
    _lastContactCount = -1;
    rebuildList();
}

void LvNodesScreen::onEnter() {
    _lastNodeCount = -1;
    _lastContactCount = -1;
    _selectedIdx = 0;
    rebuildList();
}

void LvNodesScreen::refreshUI() {
    if (!_am) return;
    unsigned long now = millis();
    if (now - _lastRebuild < REBUILD_INTERVAL_MS) return;
    int contacts = 0;
    for (const auto& n : _am->nodes()) { if (n.saved) contacts++; }
    if (_am->nodeCount() != _lastNodeCount || contacts != _lastContactCount) {
        _lastRebuild = now;
        rebuildList();
    }
}

// Update only the selection highlight without rebuilding widgets
void LvNodesScreen::updateSelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rows.size()) {
        lv_obj_set_style_bg_color(_rows[oldIdx], lv_color_hex(Theme::BG), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rows.size()) {
        lv_obj_set_style_bg_color(_rows[newIdx], lv_color_hex(Theme::SELECTION_BG), 0);
    }
    scrollToSelected();
}

void LvNodesScreen::rebuildList() {
    if (!_am || !_list) return;
    int count = _am->nodeCount();
    _lastNodeCount = count;
    _rows.clear();
    _rowToNodeIdx.clear();
    lv_obj_clean(_list);

    // Separate contacts and online nodes
    std::vector<int> contactIndices;
    std::vector<int> onlineIndices;
    const auto& nodes = _am->nodes();
    for (int i = 0; i < count; i++) {
        if (nodes[i].saved) contactIndices.push_back(i);
        else onlineIndices.push_back(i);
    }
    _lastContactCount = contactIndices.size();

    // Sort online nodes: most recently seen first
    std::sort(onlineIndices.begin(), onlineIndices.end(), [&nodes](int a, int b) {
        return nodes[a].lastSeen > nodes[b].lastSeen;
    });

    if (count == 0) {
        lv_obj_clear_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
        _totalRows = 0;
        return;
    }

    lv_obj_add_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);

    const lv_font_t* font = &lv_font_montserrat_14;
    const lv_font_t* smallFont = &lv_font_montserrat_10;
    int rowIdx = 0;

    // Helper to create a section header
    auto makeSectionHeader = [&](const char* title, int itemCount) {
        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, 22);
        lv_obj_set_style_bg_color(row, lv_color_hex(Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char buf[32];
        snprintf(buf, sizeof(buf), "%s (%d)", title, itemCount);
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::ACCENT), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        _rows.push_back(row);
        _rowToNodeIdx.push_back(-1); // header, not selectable
        return rowIdx++;
    };

    // Helper to create a node row
    auto makeNodeRow = [&](int nodeIdx) {
        const auto& node = nodes[nodeIdx];
        bool selected = (rowIdx == _selectedIdx);

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, 24);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            selected ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Name (truncated to 15) + destination hash (12 chars)
        std::string truncName = node.name.substr(0, 15);
        std::string displayHash = node.hash.toHex().substr(0, 12);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s [%s]", truncName.c_str(), displayHash.c_str());

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(
            node.saved ? Theme::ACCENT : Theme::PRIMARY), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        // Hops + age
        unsigned long ageSec = (millis() - node.lastSeen) / 1000;
        char infoBuf[24];
        if (node.hops < 128) {
            if (ageSec < 60) snprintf(infoBuf, sizeof(infoBuf), "%dhop %lus", node.hops, ageSec);
            else snprintf(infoBuf, sizeof(infoBuf), "%dhop %lum", node.hops, ageSec / 60);
        } else {
            if (ageSec < 60) snprintf(infoBuf, sizeof(infoBuf), "%lus", ageSec);
            else snprintf(infoBuf, sizeof(infoBuf), "%lum", ageSec / 60);
        }

        lv_obj_t* info = lv_label_create(row);
        lv_obj_set_style_text_font(info, smallFont, 0);
        lv_obj_set_style_text_color(info, lv_color_hex(Theme::SECONDARY), 0);
        lv_label_set_text(info, infoBuf);
        lv_obj_align(info, LV_ALIGN_RIGHT_MID, -4, 0);

        _rows.push_back(row);
        _rowToNodeIdx.push_back(nodeIdx);
        rowIdx++;
    };

    // Contacts section (if any)
    if (!contactIndices.empty()) {
        _contactHeaderIdx = makeSectionHeader("Contacts", contactIndices.size());
        for (int ni : contactIndices) makeNodeRow(ni);
    } else {
        _contactHeaderIdx = -1;
    }

    // Online section
    _onlineHeaderIdx = makeSectionHeader("Online", onlineIndices.size());
    for (int ni : onlineIndices) makeNodeRow(ni);

    _totalRows = rowIdx;

    // Clamp selection to valid selectable row
    if (_selectedIdx >= _totalRows) _selectedIdx = _totalRows - 1;
    if (_selectedIdx < 0) _selectedIdx = 0;
    // Skip headers
    while (_selectedIdx < _totalRows && _rowToNodeIdx[_selectedIdx] == -1) _selectedIdx++;
    if (_selectedIdx >= _totalRows) _selectedIdx = _totalRows - 1;

    // Apply highlight to corrected selection (rows were built before skip-headers loop)
    if (_selectedIdx >= 0 && _selectedIdx < (int)_rows.size()) {
        lv_obj_set_style_bg_color(_rows[_selectedIdx], lv_color_hex(Theme::SELECTION_BG), 0);
    }

    scrollToSelected();
}

void LvNodesScreen::scrollToSelected() {
    if (_selectedIdx >= 0 && _selectedIdx < (int)_rows.size()) {
        lv_obj_scroll_to_view(_rows[_selectedIdx], LV_ANIM_OFF);
    }
}


bool LvNodesScreen::handleLongPress() {
    if (!_am || _totalRows == 0) return false;
    if (_selectedIdx < 0 || _selectedIdx >= (int)_rowToNodeIdx.size()) return false;
    int nodeIdx = _rowToNodeIdx[_selectedIdx];
    if (nodeIdx < 0 || nodeIdx >= (int)_am->nodes().size()) return false;
    const auto& node = _am->nodes()[nodeIdx];
    if (node.saved) {
        _confirmDelete = true;
        if (_ui) _ui->lvStatusBar().showToast("Remove friend? Enter=Yes Esc=No", 5000);
    } else {
        // Add as friend
        auto& mutableNode = const_cast<DiscoveredNode&>(node);
        mutableNode.saved = true;
        _am->saveContacts();
        if (_ui) _ui->lvStatusBar().showToast("Added to friends!", 1200);
        rebuildList();
    }
    return true;
}

bool LvNodesScreen::handleKey(const KeyEvent& event) {
    if (!_am || _totalRows == 0) return false;

    // Confirm delete mode
    if (_confirmDelete) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_selectedIdx >= 0 && _selectedIdx < (int)_rowToNodeIdx.size()) {
                int nodeIdx = _rowToNodeIdx[_selectedIdx];
                if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
                    auto& nodes = const_cast<std::vector<DiscoveredNode>&>(_am->nodes());
                    nodes.erase(nodes.begin() + nodeIdx);
                    _am->saveContacts();
                    if (_ui) _ui->lvStatusBar().showToast("Contact deleted", 1200);
                    _selectedIdx = 0;
                    rebuildList();
                }
            }
            _confirmDelete = false;
            return true;
        }
        _confirmDelete = false;
        if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
        return true;
    }

    if (event.up) {
        int prev = _selectedIdx;
        _selectedIdx--;
        while (_selectedIdx >= 0 && _rowToNodeIdx[_selectedIdx] == -1) _selectedIdx--;
        if (_selectedIdx < 0) _selectedIdx = prev;
        if (_selectedIdx != prev) updateSelection(prev, _selectedIdx);
        return true;
    }
    if (event.down) {
        int prev = _selectedIdx;
        _selectedIdx++;
        while (_selectedIdx < _totalRows && _rowToNodeIdx[_selectedIdx] == -1) _selectedIdx++;
        if (_selectedIdx >= _totalRows) _selectedIdx = prev;
        if (_selectedIdx != prev) updateSelection(prev, _selectedIdx);
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_selectedIdx >= 0 && _selectedIdx < (int)_rowToNodeIdx.size()) {
            int nodeIdx = _rowToNodeIdx[_selectedIdx];
            if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size() && _onSelect) {
                _onSelect(_am->nodes()[nodeIdx].hash.toHex());
            }
        }
        return true;
    }
    // 's' or 'S' to save/unsave contact
    if (event.character == 's' || event.character == 'S') {
        if (_selectedIdx >= 0 && _selectedIdx < (int)_rowToNodeIdx.size()) {
            int nodeIdx = _rowToNodeIdx[_selectedIdx];
            if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
                auto& node = const_cast<DiscoveredNode&>(_am->nodes()[nodeIdx]);
                node.saved = !node.saved;
                if (node.saved) {
                    _am->saveContacts();
                }
                rebuildList();
            }
        }
        return true;
    }
    return false;
}
