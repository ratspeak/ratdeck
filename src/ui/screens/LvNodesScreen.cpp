#include "LvNodesScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "reticulum/AnnounceManager.h"
#include "config/UserConfig.h"
#include <Arduino.h>
#include <algorithm>
#include <climits>
#include "fonts/fonts.h"

namespace {

constexpr int kPeerRowH = 36;
constexpr int kPeerHeaderH = 20;
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

std::string peerMetaFor(const DiscoveredNode& node, unsigned long ageMs, bool devMode) {
    std::string meta = compactAge(ageMs);
    if (devMode && node.rssi != 0) {
        char buf[14];
        snprintf(buf, sizeof(buf), " %ddB", node.rssi);
        meta += buf;
    }
    return meta;
}

lv_obj_t* createEmptyState(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_size(box, 252, 94);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* pip = lv_obj_create(box);
        lv_obj_set_size(pip, 6, 6);
        lv_obj_set_pos(pip, 104 + i * 13, 14);
        lv_obj_set_style_radius(pip, 3, 0);
        lv_obj_set_style_bg_color(pip, lv_color_hex(i == 0 ? Theme::TEXT_MUTED : Theme::BORDER), 0);
        lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pip, 0, 0);
        lv_obj_set_style_pad_all(pip, 0, 0);
        lv_obj_clear_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t* title = lv_label_create(box);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(title, "No peers heard");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t* hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(hint, "Listening for announces");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 55);

    return box;
}

}  // namespace

void LvNodesScreen::createUI(lv_obj_t* parent) {
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

    rebuildList();

    // --- Action modal overlay (on top layer, centered) ---
    // Sized to fit up to 4 menu rows (saved-contact path adds Send GPS).
    // Unsaved peers use only 3 rows; the 4th is hidden at showActionMenu
    // time so the menu collapses visually back to its old layout.
    constexpr int kOverlayW = 260;
    constexpr int kOverlayH = 184;     // 3 rows=158, +1 row + extra margin
    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, kOverlayW, kOverlayH);
    lv_obj_set_pos(_overlay, (Theme::SCREEN_W - kOverlayW) / 2, Theme::STATUS_BAR_H + (Theme::CONTENT_H - kOverlayH) / 2);
    lv_obj_add_style(_overlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
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

    // Menu rows — up to MAX_MENU_ENTRIES. showActionMenu() sets the
    // labels + visibility per peer (saved vs unsaved). Default labels
    // here are placeholders; they get overwritten before showActionMenu
    // ever clears the overlay hidden flag.
    const char* menuText[MAX_MENU_ENTRIES] = {"Save Contact", "Message", "Close", ""};
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
            auto* self = (LvNodesScreen*)lv_event_get_user_data(e);
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

    // Nickname input widgets
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
    lv_obj_set_width(_nicknameLbl, 232);
    lv_label_set_text(_nicknameLbl, "_");
    lv_obj_set_pos(_nicknameLbl, 0, 22);

    _nicknameHint = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(_nicknameHint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_nicknameHint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(_nicknameHint, "Enter saves / Esc keeps");
    lv_obj_set_pos(_nicknameHint, 0, 52);
}

void LvNodesScreen::destroyUI() {
    if (_overlay) { lv_obj_del(_overlay); _overlay = nullptr; }
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) { _menuBtns[i] = nullptr; _menuLabels[i] = nullptr; }
    _overlayTitle = nullptr; _overlayMeta = nullptr; _overlayReach = nullptr;
    _nicknameBox = nullptr; _nicknameLbl = nullptr; _nicknameHint = nullptr;
    _list = nullptr; _emptyState = nullptr;
    LvScreen::destroyUI();
}

void LvNodesScreen::onEnter() {
    _lastNodeCount = -1;
    _lastContactCount = -1;
    _focusActive = false;
    _confirmDelete = false;
    hideOverlay();
    rebuildList();
}

void LvNodesScreen::refreshUI() {
    if (!_am) return;
    unsigned long now = millis();
    if (now - _lastRebuild < REBUILD_INTERVAL_MS) return;
    int contacts = 0;
    for (const auto& n : _am->nodes()) { if (n.saved) contacts++; }
    int countDelta = abs(_am->nodeCount() - _lastNodeCount);
    int contactDelta = abs(contacts - _lastContactCount);
    bool ageRefresh = now - _lastRebuild >= AGE_REBUILD_INTERVAL_MS;
    if (countDelta > 0 || contactDelta > 0 || ageRefresh) {
        _lastRebuild = now;
        rebuildList();
    }
}

void LvNodesScreen::rebuildList() {
    if (!_am || !_list) return;
    _lastRebuild = millis();
    // Preserve scroll position across rebuilds
    lv_coord_t scrollY = lv_obj_get_scroll_y(_list);
    lv_obj_clean(_list);
    _sortedContactIndices.clear();
    _sortedOnlineIndices.clear();

    const auto& nodes = _am->nodes();
    int count = (int)nodes.size();
    _lastNodeCount = count;
    unsigned long now = millis();

    for (int i = 0; i < count; i++) {
        if (nodes[i].saved) _sortedContactIndices.push_back(i);
        else _sortedOnlineIndices.push_back(i);
    }
    _lastContactCount = (int)_sortedContactIndices.size();

    std::sort(_sortedContactIndices.begin(), _sortedContactIndices.end(), [&nodes, now](int a, int b) {
        unsigned long ageA = nodeAgeMs(nodes[a], now);
        unsigned long ageB = nodeAgeMs(nodes[b], now);
        if (ageA != ageB) return ageA < ageB;
        std::string nameA = displayNameFor(nodes[a]);
        std::string nameB = displayNameFor(nodes[b]);
        if (nameA == nameB) return nodes[a].hash.toHex() < nodes[b].hash.toHex();
        return nameA < nameB;
    });

    std::sort(_sortedOnlineIndices.begin(), _sortedOnlineIndices.end(), [&nodes](int a, int b) {
        return nodes[a].lastSeen > nodes[b].lastSeen;
    });

    if (_sortedContactIndices.empty() && _sortedOnlineIndices.empty()) {
        if (_emptyState) lv_obj_clear_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (_emptyState) lv_obj_add_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);

    bool devMode = _cfg && _cfg->settings().devMode;

    auto addHeader = [&](const char* text) {
        lv_obj_t* hdr = lv_obj_create(_list);
        lv_obj_set_size(hdr, Theme::CONTENT_W, kPeerHeaderH);
        lv_obj_add_style(hdr, LvTheme::styleSectionHeader(), 0);
        lv_obj_set_style_radius(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* lbl = lv_label_create(hdr);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::ACCENT), 0);
        lv_label_set_text(lbl, text);
        lv_obj_set_pos(lbl, 8, 5);
    };

    auto addNodeRow = [&](int nodeIdx) {
        const auto& node = nodes[nodeIdx];
        unsigned long age = nodeAgeMs(node, now);

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, kPeerRowH);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)nodeIdx);

        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvNodesScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (idx >= 0 && idx < (int)self->_am->nodes().size()) {
                self->showActionMenu(idx);
            }
        }, LV_EVENT_CLICKED, this);

        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(
            node.saved ? Theme::ACCENT : Theme::PRIMARY), 0);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nameLbl, Theme::CONTENT_W - 132);
        std::string name = displayNameFor(node);
        lv_label_set_text(nameLbl, name.c_str());
        lv_obj_set_pos(nameLbl, 8, 3);

        lv_obj_t* infoLbl = lv_label_create(row);
        lv_obj_set_style_text_font(infoLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(infoLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(infoLbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(infoLbl, 116);
        std::string meta = peerMetaFor(node, age, devMode);
        lv_label_set_text(infoLbl, meta.c_str());
        lv_obj_set_pos(infoLbl, Theme::CONTENT_W - 124, 5);

        lv_obj_t* idLbl = lv_label_create(row);
        lv_obj_set_style_text_font(idLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(idLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_long_mode(idLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(idLbl, Theme::CONTENT_W - 16);
        std::string identity = identityLineFor(node);
        lv_label_set_text(idLbl, identity.c_str());
        lv_obj_set_pos(idLbl, 8, 20);
    };

    // Build list: Contacts section, then Online section
    if (!_sortedContactIndices.empty()) {
        char hdrBuf[32];
        snprintf(hdrBuf, sizeof(hdrBuf), "TRUSTED CONTACTS (%d)", (int)_sortedContactIndices.size());
        addHeader(hdrBuf);
        for (int idx : _sortedContactIndices) addNodeRow(idx);
    }

    {
        char hdrBuf[32];
        snprintf(hdrBuf, sizeof(hdrBuf), "RECENT PEERS (%d)", (int)_sortedOnlineIndices.size());
        addHeader(hdrBuf);
        for (int idx : _sortedOnlineIndices) addNodeRow(idx);
    }

    // Restore scroll position so the list doesn't jump to top on refresh
    if (scrollY > 0) {
        lv_obj_update_layout(_list);
        lv_obj_scroll_to_y(_list, scrollY, LV_ANIM_OFF);
    }

    // Clear auto-focus if user hasn't started navigating yet
    if (!_focusActive) {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_clear_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }
}

int LvNodesScreen::getFocusedNodeIdx() const {
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (!focused) return -1;
    return (int)(intptr_t)lv_obj_get_user_data(focused);
}

// --- Action modal helpers ---

void LvNodesScreen::updateOverlayDetails(const char* title) {
    if (!_overlayTitle || !_overlayMeta || !_overlayReach) return;

    if (!_am || _actionNodeIdx < 0 || _actionNodeIdx >= (int)_am->nodes().size()) {
        lv_label_set_text(_overlayTitle, title ? title : "Peer");
        lv_label_set_text(_overlayMeta, "ID: unavailable");
        lv_label_set_text(_overlayReach, "No route data");
        return;
    }

    const auto& node = _am->nodes()[_actionNodeIdx];
    unsigned long age = nodeAgeMs(node, millis());
    bool devMode = _cfg && _cfg->settings().devMode;

    std::string heading = title ? title : displayNameFor(node);
    std::string identity = identityLineFor(node);
    std::string details = node.saved ? "Saved contact / " : "Peer / ";
    details += peerMetaFor(node, age, devMode);

    lv_obj_set_style_text_color(_overlayTitle, lv_color_hex(node.saved ? Theme::ACCENT : Theme::PRIMARY), 0);
    lv_obj_set_style_text_color(_overlayReach, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(_overlayTitle, heading.c_str());
    lv_label_set_text(_overlayMeta, identity.c_str());
    lv_label_set_text(_overlayReach, details.c_str());
}

void LvNodesScreen::showActionMenu(int nodeIdx) {
    _actionNodeIdx = nodeIdx;
    _menuIdx = 0;
    _actionState = NodeAction::ACTION_MENU;
    _nicknameText = "";
    if (_overlay) {
        // Per-peer menu shape:
        //   Unsaved: [Save Contact] [Message]            [Close]   (3 entries)
        //   Saved:   [Message] [Send GPS] [Edit Name] [Close]     (4 entries)
        // Send GPS is meaningful only for saved peers — anonymous
        // "spread my GPS to a stranger" wouldn't match the contact-based
        // model. Edit Name is only relevant for saved peers (unsaved
        // peers use the Save Contact path to set the name).
        bool isSaved = _am && nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()
                       && _am->nodes()[nodeIdx].saved;
        if (isSaved) {
            lv_label_set_text(_menuLabels[0], "Message");
            lv_label_set_text(_menuLabels[1], "Send GPS");
            lv_label_set_text(_menuLabels[2], "Edit Name");
            lv_label_set_text(_menuLabels[3], "Close");
            for (int i = 0; i < MAX_MENU_ENTRIES; i++) {
                lv_obj_clear_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(_menuLabels[0], "Save Contact");
            lv_label_set_text(_menuLabels[1], "Message");
            lv_label_set_text(_menuLabels[2], "Close");
            // Hide the 4th row (no Edit Name on an unsaved peer — they
            // have no name yet and Save Contact covers that flow).
            lv_obj_add_flag(_menuBtns[3], LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                lv_obj_clear_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        updateOverlayDetails(nullptr);
        lv_obj_add_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
        updateMenuSelection();
    }
}

int LvNodesScreen::menuEntryCount() const {
    if (_am && _actionNodeIdx >= 0 && _actionNodeIdx < (int)_am->nodes().size()
        && _am->nodes()[_actionNodeIdx].saved) {
        return 4;   // Message / Send GPS / Edit Name / Close
    }
    return 3;       // Save Contact / Message / Close
}

void LvNodesScreen::hideOverlay() {
    _actionState = NodeAction::BROWSE;
    _actionNodeIdx = -1;
    _nicknameText = "";
    if (_overlay) lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LvNodesScreen::showNicknameInput() {
    _actionState = NodeAction::NICKNAME_INPUT;
    if (_am && _actionNodeIdx >= 0 && _actionNodeIdx < (int)_am->nodes().size()) {
        _nicknameText = String(_am->nodes()[_actionNodeIdx].name.c_str());
    }
    updateOverlayDetails("Set contact name");
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) lv_obj_add_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
    updateNicknameDisplay();
}

void LvNodesScreen::updateMenuSelection() {
    for (int i = 0; i < MAX_MENU_ENTRIES; i++) {
        bool sel = (i == _menuIdx);
        // Style every row including hidden ones — cheap, and keeps the
        // loop uniform. Hidden rows aren't drawn so the unsel colors
        // are invisible to the user.
        lv_obj_set_style_text_color(_menuLabels[i], lv_color_hex(
            sel ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_color(_menuBtns[i], lv_color_hex(
            sel ? Theme::PRIMARY_SUBTLE : Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(_menuBtns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(_menuBtns[i], lv_color_hex(sel ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
    }
}

void LvNodesScreen::updateNicknameDisplay() {
    if (_nicknameLbl) {
        String display = _nicknameText + "_";
        lv_label_set_text(_nicknameLbl, display.c_str());
    }
}

bool LvNodesScreen::handleLongPress() {
    if (!_am) return false;
    // Only consume long-press once the user has actively navigated into
    // the list. Otherwise let main.cpp blank the screen.
    if (!_focusActive) return false;
    int nodeIdx = getFocusedNodeIdx();
    if (nodeIdx < 0 || nodeIdx >= (int)_am->nodes().size()) return false;
    const auto& node = _am->nodes()[nodeIdx];
    if (node.saved) {
        _confirmDelete = true;
        _actionNodeIdx = nodeIdx;
        if (_ui) _ui->lvStatusBar().showToast("Remove? Enter=Remove Esc=Keep", 5000);
    } else {
        showActionMenu(nodeIdx);
    }
    return true;
}

bool LvNodesScreen::handleKey(const KeyEvent& event) {
    if (!_am) return false;

    // --- Back navigation (app-mode only) ---
    // Only active when no modal overlay is consuming keys. When the
    // caller wired a back-callback (Peers opened as an Apps tile), Esc
    // / back hands control back so the caller can restore the tab bar
    // and return to the Apps hub. If no callback is wired, leave the
    // key for the global tab-cycle / tab-bar to handle.
    if (_actionState == NodeAction::BROWSE && !_confirmDelete && _onBack) {
        if (event.character == 0x1B || event.del || event.character == 0x08) {
            _onBack();
            return true;
        }
    }

    // --- Focus activation guard (only in browse mode) ---
    if (_actionState == NodeAction::BROWSE && !_confirmDelete &&
        !_focusActive && (event.up || event.down || event.enter)) {
        _focusActive = true;
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_add_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        return true;
    }

    // --- Nickname input mode ---
    if (_actionState == NodeAction::NICKNAME_INPUT) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_actionNodeIdx >= 0 && _actionNodeIdx < (int)_am->nodes().size()) {
                auto& node = const_cast<DiscoveredNode&>(_am->nodes()[_actionNodeIdx]);
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

    // --- Action menu mode ---
    if (_actionState == NodeAction::ACTION_MENU) {
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
            bool isSaved = _am && _actionNodeIdx >= 0 && _actionNodeIdx < (int)_am->nodes().size()
                           && _am->nodes()[_actionNodeIdx].saved;
            // Saved peers:  [0]=Message, [1]=Send GPS, [2]=Edit Name, [3]=Close
            // Unsaved:       [0]=Save Contact, [1]=Message, [2]=Close
            // Map _menuIdx into the action regardless of layout so the
            // switch below is uniform.
            enum class Act { KMessage, KSendGps, KSaveOrEditName, KClose };
            Act act;
            int targetIdx = _actionNodeIdx;
            if (isSaved) {
                switch (_menuIdx) {
                    case 0: act = Act::KMessage; break;
                    case 1: act = Act::KSendGps; break;
                    case 2: act = Act::KSaveOrEditName; break;
                    default: act = Act::KClose; break;
                }
            } else {
                switch (_menuIdx) {
                    case 0: act = Act::KSaveOrEditName; break;
                    case 1: act = Act::KMessage; break;
                    default: act = Act::KClose; break;
                }
            }

            switch (act) {
                case Act::KMessage:
                    if (targetIdx >= 0 && targetIdx < (int)_am->nodes().size() && _onSelect) {
                        std::string hex = _am->nodes()[targetIdx].hash.toHex();
                        hideOverlay();
                        _onSelect(hex);
                    } else {
                        hideOverlay();
                    }
                    break;
                case Act::KSendGps:
                    if (targetIdx >= 0 && targetIdx < (int)_am->nodes().size() && _onSendGps) {
                        std::string hex = _am->nodes()[targetIdx].hash.toHex();
                        hideOverlay();
                        _onSendGps(hex);
                    } else {
                        // No callback wired — just close and let the
                        // caller know via a toast.
                        hideOverlay();
                        if (_ui) _ui->lvStatusBar().showToast("Send GPS unavailable", 1500);
                    }
                    break;
                case Act::KSaveOrEditName:
                    showNicknameInput();
                    break;
                case Act::KClose:
                default:
                    hideOverlay();
                    break;
            }
            return true;
        }
        if (event.character == 0x1B) { hideOverlay(); return true; }
        return true;
    }

    // --- Confirm delete mode ---
    if (_confirmDelete) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_actionNodeIdx >= 0 && _actionNodeIdx < (int)_am->nodes().size()) {
                _am->deleteContact(_actionNodeIdx);
                if (_ui) _ui->lvStatusBar().showToast("Contact removed", 1200);
                rebuildList();
            }
            _confirmDelete = false;
            return true;
        }
        _confirmDelete = false;
        if (_ui) _ui->lvStatusBar().showToast("Kept contact", 800);
        return true;
    }

    // 's' or 'S' to save/unsave contact
    if (event.character == 's' || event.character == 'S') {
        int nodeIdx = getFocusedNodeIdx();
        if (nodeIdx >= 0 && nodeIdx < (int)_am->nodes().size()) {
            auto& node = const_cast<DiscoveredNode&>(_am->nodes()[nodeIdx]);
            node.saved = !node.saved;
            if (node.saved) _am->saveContacts();
            rebuildList();
        }
        return true;
    }

    // Let LVGL focus group handle up/down/enter navigation
    return false;
}
