#include "LvMessagesScreen.h"
#include "ui/Theme.h"
#include "ui/UIManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "storage/MessageStore.h"
#include <Arduino.h>
#include <time.h>
#include <algorithm>

void LvMessagesScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    _lblEmpty = lv_label_create(parent);
    lv_obj_set_style_text_font(_lblEmpty, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblEmpty, lv_color_hex(Theme::MUTED), 0);
    lv_label_set_text(_lblEmpty, "No conversations");
    lv_obj_center(_lblEmpty);

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

    _lastConvCount = -1;
    rebuildList();
}

void LvMessagesScreen::onEnter() {
    _lastConvCount = -1;
    _selectedIdx = 0;
    rebuildList();
}

void LvMessagesScreen::refreshUI() {
    if (!_lxmf) return;
    int count = (int)_lxmf->conversations().size();
    int unread = _lxmf->unreadCount();
    if (count != _lastConvCount || unread != _lastUnreadTotal) {
        rebuildList();
    }
}

// Update only the selection highlight without rebuilding widgets
void LvMessagesScreen::updateSelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rows.size()) {
        lv_obj_set_style_bg_color(_rows[oldIdx], lv_color_hex(Theme::BG), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rows.size()) {
        lv_obj_set_style_bg_color(_rows[newIdx], lv_color_hex(Theme::SELECTION_BG), 0);
        lv_obj_scroll_to_view(_rows[newIdx], LV_ANIM_OFF);
    }
}

void LvMessagesScreen::rebuildList() {
    if (!_lxmf || !_list) return;

    const auto& convs = _lxmf->conversations();
    int count = (int)convs.size();
    _lastConvCount = count;
    _lastUnreadTotal = _lxmf->unreadCount();
    _rows.clear();
    _sortedPeers.clear();
    lv_obj_clean(_list);

    if (count == 0) {
        lv_obj_clear_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);

    // Build conversation info for sorting by most recent
    struct ConvInfo {
        std::string peerHex;
        double lastTs = 0;
        std::string preview;
        bool hasUnread = false;
    };

    std::vector<ConvInfo> sorted;
    sorted.reserve(count);

    for (int i = 0; i < count; i++) {
        ConvInfo ci;
        ci.peerHex = convs[i];
        ci.hasUnread = _lxmf->unreadCount(ci.peerHex) > 0;
        auto msgs = _lxmf->getMessages(ci.peerHex);
        if (!msgs.empty()) {
            const auto& last = msgs.back();
            ci.lastTs = last.timestamp;
            std::string prefix = last.incoming ? "Them: " : "You: ";
            std::string content = last.content;
            if (content.size() > 15) content = content.substr(0, 15) + "...";
            ci.preview = prefix + content;
        }
        sorted.push_back(ci);
    }

    std::sort(sorted.begin(), sorted.end(), [](const ConvInfo& a, const ConvInfo& b) {
        return a.lastTs > b.lastTs;
    });

    for (auto& ci : sorted) _sortedPeers.push_back(ci.peerHex);

    if (_selectedIdx >= count) _selectedIdx = count - 1;
    if (_selectedIdx < 0) _selectedIdx = 0;

    const lv_font_t* nameFont = &lv_font_montserrat_14;
    const lv_font_t* smallFont = &lv_font_montserrat_12;

    for (int i = 0; i < (int)sorted.size(); i++) {
        const auto& ci = sorted[i];

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, 38);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            i == _selectedIdx ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Green dot for unread
        if (ci.hasUnread) {
            lv_obj_t* dot = lv_obj_create(row);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, 3, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(Theme::PRIMARY), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_set_pos(dot, 5, 8);
        }

        // Peer name (line 1)
        std::string peerName;
        if (_am) peerName = _am->lookupName(ci.peerHex);
        std::string displayName = !peerName.empty() ?
            peerName.substr(0, 15) : ci.peerHex.substr(0, 12);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, nameFont, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(nameLbl, displayName.c_str());
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 14, 2);

        // Timestamp (line 1, right)
        if (ci.lastTs > 1000000) {
            time_t t = (time_t)ci.lastTs;
            struct tm* tm = localtime(&t);
            if (tm) {
                char timeBuf[8];
                snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm->tm_hour, tm->tm_min);
                lv_obj_t* timeLbl = lv_label_create(row);
                lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);
                lv_obj_set_style_text_color(timeLbl, lv_color_hex(Theme::MUTED), 0);
                lv_label_set_text(timeLbl, timeBuf);
                lv_obj_align(timeLbl, LV_ALIGN_TOP_RIGHT, -4, 4);
            }
        }

        // Preview (line 2)
        if (!ci.preview.empty()) {
            lv_obj_t* prevLbl = lv_label_create(row);
            lv_obj_set_style_text_font(prevLbl, smallFont, 0);
            lv_obj_set_style_text_color(prevLbl, lv_color_hex(Theme::MUTED), 0);
            lv_label_set_text(prevLbl, ci.preview.c_str());
            lv_obj_align(prevLbl, LV_ALIGN_BOTTOM_LEFT, 14, -4);
        }

        _rows.push_back(row);
    }
}

bool LvMessagesScreen::handleLongPress() {
    if (!_lxmf) return false;
    int count = (int)_lxmf->conversations().size();
    if (count == 0 || _selectedIdx >= count) return false;
    _lpState = LP_MENU;
    _menuIdx = 0;
    if (_ui) _ui->lvStatusBar().showToast("Up/Down: Add Friend | Delete | Cancel", 5000);
    return true;
}

bool LvMessagesScreen::handleKey(const KeyEvent& event) {
    if (!_lxmf) return false;

    // Long-press menu mode
    if (_lpState == LP_MENU) {
        if (event.up || event.down) {
            _menuIdx = (_menuIdx + (event.down ? 1 : -1) + 3) % 3;
            const char* labels[] = {">> Add Friend <<", ">> Delete Chat <<", ">> Cancel <<"};
            if (_ui) _ui->lvStatusBar().showToast(labels[_menuIdx], 5000);
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            int count = (int)_lxmf->conversations().size();
            if (_menuIdx == 0 && _selectedIdx < (int)_sortedPeers.size()) {
                // Add friend
                const auto& peerHex = _sortedPeers[_selectedIdx];
                if (_am) {
                    const DiscoveredNode* existing = _am->findNodeByHex(peerHex);
                    if (existing && !existing->saved) {
                        auto& node = const_cast<DiscoveredNode&>(*existing);
                        node.saved = true;
                        _am->saveContacts();
                        if (_ui) _ui->lvStatusBar().showToast("Added to friends!", 1200);
                    } else if (!existing) {
                        _am->addManualContact(peerHex, "");
                        if (_ui) _ui->lvStatusBar().showToast("Added to friends!", 1200);
                    } else {
                        if (_ui) _ui->lvStatusBar().showToast("Already a friend", 1200);
                    }
                }
            } else if (_menuIdx == 1 && _selectedIdx < (int)_sortedPeers.size()) {
                // Confirm delete
                _lpState = LP_CONFIRM_DELETE;
                if (_ui) _ui->lvStatusBar().showToast("Delete chat? Enter=Yes Esc=No", 5000);
                return true;
            } else {
                // Cancel
                if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
            }
            _lpState = LP_NONE;
            return true;
        }
        if (event.del || event.character == 8 || event.character == 0x1B) {
            _lpState = LP_NONE;
            if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
            return true;
        }
        return true;
    }

    // Confirm delete mode
    if (_lpState == LP_CONFIRM_DELETE) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_selectedIdx < (int)_sortedPeers.size()) {
                const auto& peerHex = _sortedPeers[_selectedIdx];
                _lxmf->markRead(peerHex);
                extern MessageStore messageStore;
                messageStore.deleteConversation(peerHex);
                messageStore.refreshConversations();
                if (_ui) _ui->lvStatusBar().showToast("Chat deleted", 1200);
                _selectedIdx = 0;
                _lastConvCount = -1;
                rebuildList();
            }
            _lpState = LP_NONE;
            return true;
        }
        _lpState = LP_NONE;
        if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
        return true;
    }

    int count = (int)_lxmf->conversations().size();
    if (count == 0) return false;

    if (event.up) {
        if (_selectedIdx > 0) {
            int prev = _selectedIdx;
            _selectedIdx--;
            updateSelection(prev, _selectedIdx);
        }
        return true;
    }
    if (event.down) {
        if (_selectedIdx < count - 1) {
            int prev = _selectedIdx;
            _selectedIdx++;
            updateSelection(prev, _selectedIdx);
        }
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_selectedIdx < (int)_sortedPeers.size() && _onOpen) {
            _onOpen(_sortedPeers[_selectedIdx]);
        }
        return true;
    }
    return false;
}
