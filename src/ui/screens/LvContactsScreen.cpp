#include "LvContactsScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/LxmFaceAvatar.h"
#include "ui/UIManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <algorithm>
#include <climits>
#include "fonts/fonts.h"

namespace {

constexpr int kContactRowH = 38;
constexpr int kContactAvatar = 32;
constexpr int kContactTextX = 48;

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
    return "ID: " + node.hash.toHex();
}

std::string compactAge(unsigned long ageMs) {
    if (ageMs == ULONG_MAX) return "old";
    if (ageMs < 5000) return "now";
    unsigned long sec = ageMs / 1000;
    char buf[12];
    if (sec < 60) snprintf(buf, sizeof(buf), "%lus", sec);
    else if (sec < 3600) snprintf(buf, sizeof(buf), "%lum", sec / 60);
    else if (sec < 86400) snprintf(buf, sizeof(buf), "%luh", sec / 3600);
    else snprintf(buf, sizeof(buf), "%lud", sec / 86400);
    return buf;
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

    // Full contact action modal (Message / Send GPS / Send Voice / Edit Name /
    // Remove / Close) — 6 entries.
    constexpr int kOverlayW = 260;
    constexpr int kOverlayH = 241;
    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, kOverlayW, kOverlayH);
    lv_obj_set_pos(_overlay, (Theme::SCREEN_W - kOverlayW) / 2,
                   Theme::STATUS_BAR_H + (Theme::CONTENT_H - kOverlayH) / 2);
    lv_obj_add_style(_overlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);

    _overlayTitle = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayTitle, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_overlayTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_overlayTitle, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayTitle, 236);
    lv_obj_set_pos(_overlayTitle, 12, 9);

    _overlayMeta = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayMeta, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_overlayMeta, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_long_mode(_overlayMeta, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayMeta, 236);
    lv_obj_set_pos(_overlayMeta, 12, 29);

    _overlayReach = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayReach, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_overlayReach, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_overlayReach, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayReach, 236);
    lv_obj_set_pos(_overlayReach, 12, 43);

    const char* menuText[MAX_MENU_ENTRIES] = {
        "Message", "Send GPS", "Send Voice", "Edit Name", "Remove", "Close"};
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) {
        lv_obj_t* btn = lv_obj_create(_overlay);
        lv_obj_set_size(btn, 236, 24);
        lv_obj_set_pos(btn, 12, 63 + i * 27);
        lv_obj_set_style_bg_color(btn, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = (LvContactsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->_menuIdx = idx;
            KeyEvent tap = {};
            tap.enter = true;
            self->handleKey(tap);
        }, LV_EVENT_CLICKED, this);

        _menuLabels[i] = lv_label_create(btn);
        lv_obj_set_style_text_font(_menuLabels[i], &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(_menuLabels[i], lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(_menuLabels[i], menuText[i]);
        lv_obj_center(_menuLabels[i]);
        _menuBtns[i] = btn;
    }

    _nicknameBox = lv_obj_create(_overlay);
    lv_obj_set_size(_nicknameBox, 236, 86);
    lv_obj_set_pos(_nicknameBox, 12, 63);
    lv_obj_set_style_bg_opa(_nicknameBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_nicknameBox, 0, 0);
    lv_obj_set_style_pad_all(_nicknameBox, 0, 0);
    lv_obj_clear_flag(_nicknameBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* nickTitle = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(nickTitle, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(nickTitle, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(nickTitle, "Contact name");
    lv_obj_set_pos(nickTitle, 0, 0);

    _nicknameLbl = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(_nicknameLbl, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_nicknameLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_long_mode(_nicknameLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_nicknameLbl, 236);
    lv_obj_set_pos(_nicknameLbl, 0, 18);
    lv_label_set_text(_nicknameLbl, "_");

    lv_obj_t* nickHint = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(nickHint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(nickHint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(nickHint, "Enter=Save  Esc=Cancel");
    lv_obj_set_pos(nickHint, 0, 48);
}

void LvContactsScreen::destroyUI() {
    if (_overlay) {
        lv_obj_del(_overlay);
        _overlay = nullptr;
    }
    _list = nullptr;
    _emptyState = nullptr;
    _actionState = ContactAction::BROWSE;
}

void LvContactsScreen::onEnter() {
    _lastContactCount = -1;
    _focusActive = false;
    hideOverlay();
    rebuildList();
}

void LvContactsScreen::refreshUI() {
    if (!_am || _actionState != ContactAction::BROWSE) return;
    unsigned long now = millis();
    int contacts = 0;
    for (const auto& n : _am->nodes()) { if (n.saved) contacts++; }
    if (contacts != _lastContactCount || now - _lastRebuild >= REBUILD_INTERVAL_MS) {
        rebuildList();
    }
}

int LvContactsScreen::nodeIdxFromList(int listIdx) const {
    if (listIdx < 0 || listIdx >= (int)_contactIndices.size()) return -1;
    return _contactIndices[listIdx];
}

void LvContactsScreen::rebuildList() {
    if (!_am || !_list) return;
    _lastRebuild = millis();
    _contactIndices.clear();

    lv_obj_clean(_list);
    _avatarBuffers.clear();

    const auto& nodes = _am->nodes();
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
            if (idx >= 0 && idx < (int)self->_contactIndices.size()) {
                self->showActionMenu(idx);
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
        lv_label_set_text(nameLbl, displayNameFor(node).c_str());
        lv_obj_set_pos(nameLbl, kContactTextX, 2);

        lv_obj_t* metaLbl = lv_label_create(row);
        lv_obj_set_style_text_font(metaLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(metaLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(metaLbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(metaLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(metaLbl, 64);
        lv_label_set_text(metaLbl, compactAge(age).c_str());
        lv_obj_set_pos(metaLbl, Theme::CONTENT_W - 72, 5);

        lv_obj_t* idLbl = lv_label_create(row);
        lv_obj_set_style_text_font(idLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(idLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_long_mode(idLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(idLbl, Theme::CONTENT_W - kContactTextX - 8);
        lv_label_set_text(idLbl, identityLineFor(node).c_str());
        lv_obj_set_pos(idLbl, kContactTextX, 22);
    }

    if (!_focusActive) {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_clear_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }
}

void LvContactsScreen::updateOverlayDetails(const char* title) {
    if (!_overlayTitle || !_overlayMeta || !_overlayReach) return;
    int nodeIdx = nodeIdxFromList(_actionListIdx);
    if (!_am || nodeIdx < 0 || nodeIdx >= (int)_am->nodes().size()) {
        lv_label_set_text(_overlayTitle, title ? title : "Contact");
        lv_label_set_text(_overlayMeta, "ID: unavailable");
        lv_label_set_text(_overlayReach, "Saved contact");
        return;
    }
    const auto& node = _am->nodes()[nodeIdx];
    std::string heading = title ? title : displayNameFor(node);
    lv_obj_set_style_text_color(_overlayTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(_overlayTitle, heading.c_str());
    lv_label_set_text(_overlayMeta, identityLineFor(node).c_str());
    lv_label_set_text(_overlayReach, "Saved contact");
}

void LvContactsScreen::showActionMenu(int listIdx) {
    _actionListIdx = listIdx;
    _menuIdx = 0;
    _actionState = ContactAction::ACTION_MENU;
    _nicknameText = "";
    if (!_overlay) return;
    lv_label_set_text(_menuLabels[0], "Message");
    lv_label_set_text(_menuLabels[1], "Send GPS");
    lv_label_set_text(_menuLabels[2], "Send Voice");
    lv_label_set_text(_menuLabels[3], "Edit Name");
    lv_label_set_text(_menuLabels[4], "Remove");
    lv_label_set_text(_menuLabels[5], "Close");
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) {
        lv_obj_clear_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
    }
    updateOverlayDetails(nullptr);
    lv_obj_add_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    updateMenuSelection();
}

void LvContactsScreen::hideOverlay() {
    _actionState = ContactAction::BROWSE;
    _actionListIdx = -1;
    _nicknameText = "";
    if (_overlay) lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LvContactsScreen::showNicknameInput() {
    _actionState = ContactAction::NICKNAME_INPUT;
    int nodeIdx = nodeIdxFromList(_actionListIdx);
    if (_am && nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
        _nicknameText = String(_am->nodes()[nodeIdx].name.c_str());
    }
    updateOverlayDetails("Set contact name");
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) lv_obj_add_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
    updateNicknameDisplay();
}

void LvContactsScreen::updateMenuSelection() {
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) {
        bool sel = (i == _menuIdx);
        lv_obj_set_style_text_color(_menuLabels[i], lv_color_hex(
            sel ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_color(_menuBtns[i], lv_color_hex(
            sel ? Theme::PRIMARY_SUBTLE : Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(_menuBtns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(_menuBtns[i],
            lv_color_hex(sel ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
    }
}

void LvContactsScreen::updateNicknameDisplay() {
    if (_nicknameLbl) {
        String display = _nicknameText + "_";
        lv_label_set_text(_nicknameLbl, display.c_str());
    }
}

bool LvContactsScreen::handleLongPress() {
    if (!_am || _contactIndices.empty() || !_focusActive) return false;
    if (_actionState != ContactAction::BROWSE) return false;
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (!focused) return false;
    _deleteIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
    if (_deleteIdx < 0 || _deleteIdx >= (int)_contactIndices.size()) return false;
    _confirmDelete = true;
    if (_ui) _ui->lvStatusBar().showToast("Remove? Enter=Remove Esc=Keep", 5000);
    return true;
}

bool LvContactsScreen::handleKey(const KeyEvent& event) {
    if (!_am) return false;

    if (_actionState == ContactAction::NICKNAME_INPUT) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            int nodeIdx = nodeIdxFromList(_actionListIdx);
            if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
                auto& node = const_cast<DiscoveredNode&>(_am->nodes()[nodeIdx]);
                String finalName = _nicknameText;
                finalName.trim();
                if (finalName.isEmpty()) {
                    if (!node.name.empty()) finalName = String(node.name.c_str());
                    else finalName = String(node.hash.toHex().substr(0, 12).c_str());
                }
                node.name = finalName.c_str();
                node.saved = true;
                _am->saveContacts();
                if (_ui) _ui->lvStatusBar().showToast("Contact saved", 1200);
                hideOverlay();
                rebuildList();
            } else {
                hideOverlay();
            }
            return true;
        }
        if (event.character == 0x1B) { hideOverlay(); return true; }
        if (event.character == '\b' || event.character == 0x7F) {
            if (_nicknameText.length() > 0) _nicknameText.remove(_nicknameText.length() - 1);
            updateNicknameDisplay();
            return true;
        }
        if (event.character >= 0x20 && event.character <= 0x7E && _nicknameText.length() < 16) {
            _nicknameText += (char)event.character;
            updateNicknameDisplay();
            return true;
        }
        return true;
    }

    if (_actionState == ContactAction::ACTION_MENU) {
        int maxIdx = menuEntryCount() - 1;
        if (event.up) {
            if (_menuIdx > 0) { _menuIdx--; updateMenuSelection(); }
            return true;
        }
        if (event.down) {
            if (_menuIdx < maxIdx) { _menuIdx++; updateMenuSelection(); }
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            int nodeIdx = nodeIdxFromList(_actionListIdx);
            // [0]=Message [1]=Send GPS [2]=Send Voice [3]=Edit Name [4]=Remove [5]=Close
            switch (_menuIdx) {
                case 0:
                    if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size() && _onSelect) {
                        std::string hex = _am->nodes()[nodeIdx].hash.toHex();
                        hideOverlay();
                        _onSelect(hex);
                    } else {
                        hideOverlay();
                    }
                    break;
                case 1:
                    if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size() && _onSendGps) {
                        std::string hex = _am->nodes()[nodeIdx].hash.toHex();
                        hideOverlay();
                        _onSendGps(hex);
                    } else {
                        hideOverlay();
                        if (_ui) _ui->lvStatusBar().showToast("Send GPS unavailable", 1500);
                    }
                    break;
                case 2:
                    if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size() && _onSendVoice) {
                        std::string hex = _am->nodes()[nodeIdx].hash.toHex();
                        hideOverlay();
                        _onSendVoice(hex);
                    } else {
                        hideOverlay();
                        if (_ui) _ui->lvStatusBar().showToast("Send Voice unavailable", 1500);
                    }
                    break;
                case 3:
                    showNicknameInput();
                    break;
                case 4:
                    if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
                        _am->deleteContact(nodeIdx);
                        if (_ui) _ui->lvStatusBar().showToast("Contact removed", 1200);
                        hideOverlay();
                        rebuildList();
                    } else {
                        hideOverlay();
                    }
                    break;
                default:
                    hideOverlay();
                    break;
            }
            return true;
        }
        if (event.character == 0x1B) { hideOverlay(); return true; }
        return true;
    }

    if (_confirmDelete) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_deleteIdx >= 0 && _deleteIdx < (int)_contactIndices.size()) {
                int nodeIdx = _contactIndices[_deleteIdx];
                if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
                    _am->deleteContact(nodeIdx);
                    if (_ui) _ui->lvStatusBar().showToast("Contact removed", 1200);
                    rebuildList();
                }
            }
            _confirmDelete = false;
            return true;
        }
        _confirmDelete = false;
        if (_ui) _ui->lvStatusBar().showToast("Kept contact", 800);
        return true;
    }

    if (!_focusActive && (event.up || event.down || event.enter)) {
        _focusActive = true;
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_add_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        return true;
    }

    // Enter on focused contact row → action menu (not direct message)
    if (event.enter || event.character == '\n' || event.character == '\r') {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) {
            int idx = (int)(intptr_t)lv_obj_get_user_data(focused);
            if (idx >= 0 && idx < (int)_contactIndices.size()) {
                showActionMenu(idx);
                return true;
            }
        }
    }

    return false;
}
