#include "LvContactsScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/LxmFaceAvatar.h"
#include "ui/UIManager.h"
#include "platform/CoreSync.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <algorithm>
#include <climits>
#include "fonts/fonts.h"

namespace {

constexpr int kContactRowH = 38;
constexpr int kContactAvatar = 32;
constexpr int kContactTextX = 48;

int findNodeIndexByHex(const std::vector<DiscoveredNode>& nodes, const std::string& hex) {
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (nodes[i].hash.toHex() == hex) return i;
    }
    return -1;
}

unsigned long nodeAgeMs(const DiscoveredNode& node, unsigned long now) {
    if (node.lastSeen == 0 || now < node.lastSeen) return ULONG_MAX;
    return now - node.lastSeen;
}

std::string displayNameFor(const DiscoveredNode& node) {
    if (!node.name.empty()) return node.name;
    std::string hex = node.hash.toHex();
    return hex.substr(0, 12);
}

std::string identityLineFor(const DiscoveredNode& node) {
    std::string hex = node.hash.toHex();
    return "ID: " + hex;
}

std::string compactAge(unsigned long ageMs) {
    if (ageMs == ULONG_MAX) return "old";
    if (ageMs < 5000) return "now";

    unsigned long sec = ageMs / 1000;
    char buf[12];
    if (sec < 60) {
        snprintf(buf, sizeof(buf), "%lus", sec);
    } else if (sec < 3600) {
        snprintf(buf, sizeof(buf), "%lum", sec / 60);
    } else if (sec < 86400) {
        snprintf(buf, sizeof(buf), "%luh", sec / 3600);
    } else {
        snprintf(buf, sizeof(buf), "%lud", sec / 86400);
    }
    return buf;
}

std::string contactMetaFor(unsigned long ageMs) {
    return compactAge(ageMs);
}

lv_obj_t* createEmptyState(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_size(box, 252, 94);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    lv_obj_t* head = lv_obj_create(box);
    lv_obj_set_size(head, 18, 18);
    lv_obj_set_pos(head, 117, 7);
    lv_obj_set_style_radius(head, 9, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* shoulders = lv_obj_create(box);
    lv_obj_set_size(shoulders, 36, 17);
    lv_obj_set_pos(shoulders, 108, 27);
    lv_obj_set_style_radius(shoulders, 8, 0);
    lv_obj_set_style_bg_opa(shoulders, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shoulders, 2, 0);
    lv_obj_set_style_border_color(shoulders, lv_color_hex(Theme::BORDER_ACTIVE), 0);
    lv_obj_set_style_pad_all(shoulders, 0, 0);
    lv_obj_clear_flag(shoulders, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(box);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(title, "No trusted contacts");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 49);

    lv_obj_t* hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(hint, "Saved peers appear here");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 70);

    return box;
}

}  // namespace

void LvContactsScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    _emptyState = createEmptyState(parent);

    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, lv_pct(100), lv_pct(100));
    lv_obj_add_style(_list, LvTheme::styleList(), 0);
    lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);

    _lastContactCount = -1;
    rebuildList();
}

void LvContactsScreen::onEnter() {
    _lastContactCount = -1;
    _focusActive = false;
    rebuildList();
}

void LvContactsScreen::refreshUI() {
    if (!_am) return;
    unsigned long now = millis();
    int contacts = 0;
    {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return;
        for (const auto& node : _am->nodes()) {
            if (node.saved) contacts++;
        }
    }
    if (contacts != _lastContactCount || now - _lastRebuild >= REBUILD_INTERVAL_MS) {
        rebuildList();
    }
}

void LvContactsScreen::rebuildList() {
    if (!_am || !_list) return;

    std::vector<DiscoveredNode> snapshot;
    {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return;
        snapshot = _am->nodes();
    }
    _nodesSnapshot.swap(snapshot);

    _lastRebuild = millis();
    _contactIndices.clear();

    lv_obj_clean(_list);
    _avatarBuffers.clear();

    const auto& nodes = _nodesSnapshot;
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (nodes[i].saved) _contactIndices.push_back(i);
    }
    std::sort(_contactIndices.begin(), _contactIndices.end(), [&nodes](int a, int b) {
        std::string an = displayNameFor(nodes[a]);
        std::string bn = displayNameFor(nodes[b]);
        if (an == bn) return nodes[a].hash.toHex() < nodes[b].hash.toHex();
        return an < bn;
    });
    int count = (int)_contactIndices.size();
    _lastContactCount = count;
    _avatarBuffers.reserve(count);

    // Keep list visible even with no contacts so the QR header row is reachable.
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);
    if (_emptyState) {
        if (count == 0) {
            lv_obj_clear_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(_emptyState);
        } else {
            lv_obj_add_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
        }
    }

    {
        lv_obj_t* qrRow = lv_obj_create(_list);
        lv_obj_set_size(qrRow, Theme::CONTENT_W, 28);
        lv_obj_add_style(qrRow, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(qrRow, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(qrRow, lv_color_hex(Theme::PRIMARY_SUBTLE), 0);
        lv_obj_set_style_bg_opa(qrRow, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(qrRow, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(qrRow, 1, 0);
        lv_obj_set_style_border_color(qrRow, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_pad_all(qrRow, 0, 0);
        lv_obj_clear_flag(qrRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(qrRow, LV_OBJ_FLAG_CLICKABLE);
        // Sentinel marks this as not-a-contact for handleLongPress().
        lv_obj_set_user_data(qrRow, (void*)(intptr_t)-1);

        lv_obj_add_event_cb(qrRow, [](lv_event_t* e) {
            auto* self = (LvContactsScreen*)lv_event_get_user_data(e);
            if (self->_showQrCb) self->_showQrCb();
        }, LV_EVENT_CLICKED, this);

        lv_group_add_obj(LvInput::group(), qrRow);
        lv_obj_add_event_cb(qrRow, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        lv_obj_t* qrLbl = lv_label_create(qrRow);
        lv_obj_set_style_text_font(qrLbl, &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(qrLbl, lv_color_hex(Theme::ACCENT), 0);
        lv_label_set_text(qrLbl, "Share My QR");
        lv_obj_align(qrLbl, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t* hintLbl = lv_label_create(qrRow);
        lv_obj_set_style_text_font(hintLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(hintLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(hintLbl, "Enter");
        lv_obj_align(hintLbl, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    unsigned long now = millis();

    for (int i = 0; i < count; i++) {
        int nodeIdx = _contactIndices[i];
        const auto& node = nodes[nodeIdx];
        unsigned long age = nodeAgeMs(node, now);

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, kContactRowH);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);

        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvContactsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (idx >= 0 && idx < (int)self->_contactIndices.size() && self->_onSelect) {
                int nodeIdx = self->_contactIndices[idx];
                if (nodeIdx >= 0 && nodeIdx < (int)self->_nodesSnapshot.size()) {
                    self->_onSelect(self->_nodesSnapshot[nodeIdx].hash.toHex());
                }
            }
        }, LV_EVENT_CLICKED, this);

        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        std::string hashHex = node.hash.toHex();
        _avatarBuffers.emplace_back(LxmFaceAvatar::bufferSize(kContactAvatar));
        auto avatar = LxmFaceAvatar::create(row, 8, 3, kContactAvatar,
                                            _avatarBuffers.back().data(),
                                            Theme::PRIMARY_SUBTLE, Theme::BORDER);
        LxmFaceAvatar::render(avatar.canvas, String(hashHex.c_str()));

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(Theme::ACCENT), 0);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nameLbl, Theme::CONTENT_W - kContactTextX - 72);
        std::string name = displayNameFor(node);
        lv_label_set_text(nameLbl, name.c_str());
        lv_obj_set_pos(nameLbl, kContactTextX, 2);

        lv_obj_t* metaLbl = lv_label_create(row);
        lv_obj_set_style_text_font(metaLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(metaLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(metaLbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(metaLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(metaLbl, 64);
        std::string meta = contactMetaFor(age);
        lv_label_set_text(metaLbl, meta.c_str());
        lv_obj_set_pos(metaLbl, Theme::CONTENT_W - 72, 5);

        lv_obj_t* idLbl = lv_label_create(row);
        lv_obj_set_style_text_font(idLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(idLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_long_mode(idLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(idLbl, Theme::CONTENT_W - kContactTextX - 8);
        std::string identity = identityLineFor(node);
        lv_label_set_text(idLbl, identity.c_str());
        lv_obj_set_pos(idLbl, kContactTextX, 22);
    }

    // Clear auto-focus if user hasn't started navigating yet
    if (!_focusActive) {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_clear_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }
}

bool LvContactsScreen::handleLongPress() {
    if (!_am || _contactIndices.empty()) return false;
    // Only consume long-press once the user has actively navigated into
    // the list. Otherwise let main.cpp blank the screen.
    if (!_focusActive) return false;
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (!focused) return false;
    int deleteIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
    if (deleteIdx < 0 || deleteIdx >= (int)_contactIndices.size()) return false;
    int nodeIdx = _contactIndices[deleteIdx];
    if (nodeIdx < 0 || nodeIdx >= (int)_nodesSnapshot.size()) return false;
    _deleteNodeHex = _nodesSnapshot[nodeIdx].hash.toHex();
    _confirmDelete = true;
    if (_ui) _ui->lvStatusBar().showToast("Remove? Enter=Remove Esc=Keep", 5000);
    return true;
}

bool LvContactsScreen::handleKey(const KeyEvent& event) {
    if (!_am || _contactIndices.empty()) return false;

    if (!_focusActive && (event.up || event.down || event.enter)) {
        _focusActive = true;
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_add_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        return true;
    }

    if (_confirmDelete) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            bool removed = false;
            {
                CoreSync::RnsGuard backendGuard;
                int nodeIdx = findNodeIndexByHex(_am->nodes(), _deleteNodeHex);
                removed = nodeIdx >= 0 && _am->deleteContact(nodeIdx);
            }
            if (removed && _ui) _ui->lvStatusBar().showToast("Contact removed", 1200);
            _confirmDelete = false;
            _deleteNodeHex.clear();
            if (removed) rebuildList();
            return true;
        }
        _confirmDelete = false;
        _deleteNodeHex.clear();
        if (_ui) _ui->lvStatusBar().showToast("Kept contact", 800);
        return true;
    }

    return false;
}
