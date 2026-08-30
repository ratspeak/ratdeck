#include "LvMessagesScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "ui/LxmFaceAvatar.h"
#include "platform/CoreSync.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "storage/MessageStore.h"
#include <Arduino.h>
#include <time.h>
#include <algorithm>
#include "fonts/fonts.h"

namespace {

constexpr int kRowH = 58;
constexpr int kRailW = 5;
constexpr int kChatAvatar = 32;
constexpr int kChatTextX = 54;
bool isPendingStatus(LXMFStatus status) {
    return status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING;
}

uint32_t statusColor(LXMFStatus status) {
    switch (status) {
        case LXMFStatus::DELIVERED: return Theme::SUCCESS;
        case LXMFStatus::SENT:      return Theme::TEXT_MUTED;
        case LXMFStatus::FAILED:    return Theme::ERROR_CLR;
        case LXMFStatus::QUEUED:
        case LXMFStatus::SENDING:   return Theme::WARNING_CLR;
        default:                    return Theme::TEXT_MUTED;
    }
}

const char* statusLabel(LXMFStatus status) {
    switch (status) {
        case LXMFStatus::DELIVERED: return "SENT";
        case LXMFStatus::SENT:      return "SENT";
        case LXMFStatus::FAILED:    return "FAILED";
        case LXMFStatus::SENDING:   return "SENDING";
        case LXMFStatus::QUEUED:    return "QUEUED";
        default:                    return "";
    }
}

std::string shortText(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) return text;
    if (maxLen <= 3) return text.substr(0, maxLen);
    return text.substr(0, maxLen - 3) + "...";
}

std::string chatPreviewText(const std::string& text, size_t maxLen) {
    if (text.empty()) return text;

    std::string firstLine;
    firstLine.reserve(std::min(text.size(), maxLen));
    bool truncated = false;
    for (char c : text) {
        if (c == '\r' || c == '\n') {
            truncated = true;
            break;
        }
        firstLine += (c == '\t') ? ' ' : c;
    }

    while (!firstLine.empty() && firstLine.back() == ' ') {
        firstLine.pop_back();
    }

    if (firstLine.size() > maxLen) {
        firstLine = shortText(firstLine, maxLen);
    } else if (truncated && firstLine.size() + 3 <= maxLen) {
        firstLine += "...";
    } else if (truncated && firstLine.size() > 3) {
        firstLine = firstLine.substr(0, maxLen - 3) + "...";
    }

    return firstLine;
}

std::string displayNameForPeer(AnnounceManager* am, const std::string& peerHex) {
    if (am) {
        std::string peerName = am->lookupName(peerHex);
        if (!peerName.empty()) return shortText(peerName, 18);
    }
    return shortText(peerHex, 12);
}

bool formatClock(double ts, char* out, size_t outLen) {
    if (!out || outLen == 0 || ts <= 1700000000) return false;
    time_t t = (time_t)ts;
    struct tm* tm = localtime(&t);
    if (!tm) return false;
    snprintf(out, outLen, "%02d:%02d", tm->tm_hour, tm->tm_min);
    return true;
}

}  // namespace

void LvMessagesScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    _lblEmpty = lv_obj_create(parent);
    lv_obj_set_size(_lblEmpty, 272, 104);
    lv_obj_set_style_bg_color(_lblEmpty, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_lblEmpty, LV_OPA_80, 0);
    lv_obj_set_style_border_color(_lblEmpty, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_lblEmpty, 1, 0);
    lv_obj_set_style_radius(_lblEmpty, 6, 0);
    lv_obj_set_style_pad_all(_lblEmpty, 0, 0);
    lv_obj_clear_flag(_lblEmpty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(_lblEmpty);

    lv_obj_t* emptyIcon = lv_label_create(_lblEmpty);
    lv_obj_set_style_text_font(emptyIcon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(emptyIcon, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(emptyIcon, LV_SYMBOL_ENVELOPE);
    lv_obj_align(emptyIcon, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t* emptyTitle = lv_label_create(_lblEmpty);
    lv_obj_set_style_text_font(emptyTitle, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(emptyTitle, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_label_set_text(emptyTitle, "No conversations");
    lv_obj_align(emptyTitle, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t* emptySub = lv_label_create(_lblEmpty);
    lv_obj_set_style_text_font(emptySub, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(emptySub, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(emptySub, "LXMF inbox is clear");
    lv_obj_align(emptySub, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t* emptyLine = lv_obj_create(_lblEmpty);
    lv_obj_set_size(emptyLine, 160, 1);
    lv_obj_set_style_bg_color(emptyLine, lv_color_hex(Theme::DIVIDER), 0);
    lv_obj_set_style_bg_opa(emptyLine, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(emptyLine, 0, 0);
    lv_obj_align(emptyLine, LV_ALIGN_BOTTOM_MID, 0, -18);

    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, lv_pct(100), lv_pct(100));
    lv_obj_add_style(_list, LvTheme::styleList(), 0);
    lv_obj_add_style(_list, LvTheme::styleScrollbar(), LV_PART_SCROLLBAR);
    lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

    _lastConvCount = -1;
}

void LvMessagesScreen::onEnter() {
    _lastConvCount = -1;
    _focusActive = false;
    hideActionMenu();
    rebuildList();
}

void LvMessagesScreen::onExit() {
    hideActionMenu();
}

void LvMessagesScreen::refreshUI() {
    if (!_lxmf) return;
    if (_lpState != LP_NONE) return;
    bool changed;
    {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return;
        int count = (int)_lxmf->conversations().size();
        int unread = _lxmf->unreadCount();
        int queued = _lxmf->queuedCount();
        uint32_t revision = _lxmf->storeRevision();
        changed = count != _lastConvCount || unread != _lastUnreadTotal ||
            queued != _lastQueuedCount || revision != _lastStoreRevision;
    }
    if (changed) rebuildList();
}

void LvMessagesScreen::rebuildList() {
    if (!_lxmf || !_list) return;
    unsigned long startMs = millis();
    int count = 0;

    {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return;

        const auto& convs = _lxmf->conversations();
        count = (int)convs.size();
        _lastConvCount = count;
        _lastUnreadTotal = _lxmf->unreadCount();
        _lastQueuedCount = _lxmf->queuedCount();
        _lastStoreRevision = _lxmf->storeRevision();
        _sortedPeers.clear();
        _sortedConvs.clear();
        _sortedConvs.reserve(count);

        for (int i = 0; i < count; i++) {
            ConvInfo ci;
            ci.peerHex = convs[i];
            auto* summary = _lxmf->getConversationSummary(ci.peerHex);
            if (summary) {
                ci.lastTs = summary->lastTimestamp;
                ci.preview = chatPreviewText(summary->lastPreview, 56);
                ci.lastIncoming = summary->lastIncoming;
                ci.unreadCount = summary->unreadCount;
                ci.totalCount = summary->totalCount;
                ci.hasUnread = ci.unreadCount > 0;
                ci.hasOutgoing = summary->hasOutgoing;
                ci.hasPending = summary->hasPending;
                ci.hasFailed = summary->hasFailed;
                ci.lastOutgoingStatus = summary->lastOutgoingStatus;
            }
            ci.displayName = displayNameForPeer(_am, ci.peerHex);

            if (_am) {
                const DiscoveredNode* node = _am->findNodeByHex(ci.peerHex);
                if (node) {
                    ci.knownNode = true;
                    ci.savedNode = node->saved;
                    ci.rssi = node->rssi;
                    ci.snr = node->snr;
                    ci.hops = node->hops;
                    ci.lastSeen = node->lastSeen;
                }
            }

            _sortedConvs.push_back(ci);
        }
    }

    lv_obj_clean(_list);
    _avatarBuffers.clear();

    if (count == 0) {
        lv_obj_clear_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(_lblEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);

    _avatarBuffers.reserve(count);

    std::sort(_sortedConvs.begin(), _sortedConvs.end(), [](const ConvInfo& a, const ConvInfo& b) {
        return a.lastTs > b.lastTs;
    });

    for (auto& ci : _sortedConvs) _sortedPeers.push_back(ci.peerHex);

    // Build list items with focus group support
    const lv_font_t* nameFont = &lv_font_rsdeck_14;
    const lv_font_t* smallFont = &lv_font_rsdeck_12;

    for (int i = 0; i < count; i++) {
        const auto& ci = _sortedConvs[i];
        bool latestOutgoingFailed = ci.hasOutgoing && ci.lastOutgoingStatus == LXMFStatus::FAILED;

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, kRowH);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ci.hasUnread ? Theme::PRIMARY_SUBTLE : Theme::BG), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);

        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvMessagesScreen*)lv_event_get_user_data(e);
            if (self->_lpState != LP_NONE) return;
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (idx < (int)self->_sortedPeers.size() && self->_onOpen) {
                self->_onOpen(self->_sortedPeers[idx]);
            }
        }, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvMessagesScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->showActionMenu(idx);
        }, LV_EVENT_LONG_PRESSED, this);

        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        uint32_t railColor = Theme::BORDER;
        if (latestOutgoingFailed) railColor = Theme::ERROR_CLR;
        else if (ci.hasPending) railColor = Theme::WARNING_CLR;
        else if (ci.hasUnread) railColor = Theme::PRIMARY;
        else if (ci.savedNode) railColor = Theme::TEXT_SECONDARY;
        else if (ci.knownNode) railColor = Theme::TEXT_MUTED;

        lv_obj_t* rail = lv_obj_create(row);
        lv_obj_set_size(rail, kRailW, kRowH);
        lv_obj_set_pos(rail, 0, 0);
        lv_obj_set_style_bg_color(rail, lv_color_hex(railColor), 0);
        lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rail, 0, 0);
        lv_obj_set_style_radius(rail, 0, 0);
        lv_obj_set_style_pad_all(rail, 0, 0);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

        _avatarBuffers.emplace_back(LxmFaceAvatar::bufferSize(kChatAvatar));
        auto avatar = LxmFaceAvatar::create(row, 13, 13, kChatAvatar,
                                            _avatarBuffers.back().data(),
                                            Theme::PRIMARY_SUBTLE,
                                            ci.hasUnread ? Theme::PRIMARY : Theme::BORDER);
        LxmFaceAvatar::render(avatar.canvas, String(ci.peerHex.c_str()));

        int leftPad = kChatTextX;

        // Name (top-left, first line)
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, nameFont, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(ci.hasUnread ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, Theme::CONTENT_W - leftPad - 70);
        lv_label_set_text(nameLbl, ci.displayName.c_str());
        lv_obj_set_pos(nameLbl, leftPad, 6);

        // Time (top-right)
        char timeBuf[8];
        if (formatClock(ci.lastTs, timeBuf, sizeof(timeBuf))) {
            lv_obj_t* timeLbl = lv_label_create(row);
            lv_obj_set_style_text_font(timeLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(timeLbl, lv_color_hex(ci.hasUnread ? Theme::PRIMARY : Theme::TEXT_MUTED), 0);
            lv_label_set_text(timeLbl, timeBuf);
            lv_obj_align(timeLbl, LV_ALIGN_TOP_RIGHT, -8, 7);
        }

        // Preview (second line, below name)
        lv_obj_t* prevLbl = lv_label_create(row);
        lv_obj_set_style_text_font(prevLbl, smallFont, 0);
        uint32_t previewColor = Theme::TEXT_SECONDARY;
        if (latestOutgoingFailed) previewColor = Theme::ERROR_CLR;
        else if (ci.hasPending) previewColor = Theme::WARNING_CLR;
        else if (ci.hasUnread) previewColor = Theme::TEXT_PRIMARY;
        lv_obj_set_style_text_color(prevLbl, lv_color_hex(previewColor), 0);
        lv_label_set_long_mode(prevLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(prevLbl, Theme::CONTENT_W - leftPad - 76);
        lv_label_set_text(prevLbl, ci.preview.empty() ? "No preview" : ci.preview.c_str());
        lv_obj_set_pos(prevLbl, leftPad, 28);

        char chipBuf[16] = {0};
        uint32_t chipColor = Theme::TEXT_MUTED;
        if (latestOutgoingFailed) {
            snprintf(chipBuf, sizeof(chipBuf), "FAILED");
            chipColor = Theme::ERROR_CLR;
        } else if (ci.hasPending) {
            const char* pending = statusLabel(ci.lastOutgoingStatus);
            snprintf(chipBuf, sizeof(chipBuf), "%s", pending[0] ? pending : "QUEUED");
            chipColor = Theme::WARNING_CLR;
        } else if (ci.hasUnread) {
            if (ci.unreadCount > 9) snprintf(chipBuf, sizeof(chipBuf), "NEW 9+");
            else snprintf(chipBuf, sizeof(chipBuf), "NEW %d", ci.unreadCount);
            chipColor = Theme::PRIMARY;
        } else if (!ci.lastIncoming && ci.hasOutgoing &&
                   (ci.lastOutgoingStatus == LXMFStatus::SENT || ci.lastOutgoingStatus == LXMFStatus::DELIVERED)) {
            snprintf(chipBuf, sizeof(chipBuf), "%s", statusLabel(ci.lastOutgoingStatus));
            chipColor = statusColor(ci.lastOutgoingStatus);
        }

        if (chipBuf[0] != '\0') {
            lv_obj_t* chip = lv_obj_create(row);
            lv_obj_set_size(chip, 58, 15);
            lv_obj_align(chip, LV_ALIGN_BOTTOM_RIGHT, -7, -8);
            lv_obj_set_style_bg_color(chip, lv_color_hex(Theme::BG), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_80, 0);
            lv_obj_set_style_border_color(chip, lv_color_hex(chipColor), 0);
            lv_obj_set_style_border_width(chip, 1, 0);
            lv_obj_set_style_radius(chip, 4, 0);
            lv_obj_set_style_pad_all(chip, 0, 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* chipLbl = lv_label_create(chip);
            lv_obj_set_style_text_font(chipLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(chipLbl, lv_color_hex(chipColor), 0);
            lv_label_set_text(chipLbl, chipBuf);
            lv_obj_center(chipLbl);
        }
    }

    // Clear auto-focus if user hasn't started navigating yet
    if (!_focusActive) {
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_clear_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }

    unsigned long elapsed = millis() - startMs;
    if (elapsed > 25) {
        Serial.printf("[PERF] Chats rebuild: %d convs in %lums\n",
                      count, (unsigned long)elapsed);
    }
}

int LvMessagesScreen::getFocusedPeerIdx() const {
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (!focused) return -1;
    return (int)(intptr_t)lv_obj_get_user_data(focused);
}

void LvMessagesScreen::hideActionMenu() {
    if (_actionOverlay) {
        lv_obj_del_async(_actionOverlay);
        _actionOverlay = nullptr;
    }
    for (int i = 0; i < 3; i++) {
        _actionRows[i] = nullptr;
        _actionLabels[i] = nullptr;
    }
    _lpState = LP_NONE;
    _lpPeerIdx = -1;
    _menuIdx = 0;
    _menuCount = 0;
}

void LvMessagesScreen::rebuildActionOverlay(const char* title, const char* const* labels, int count) {
    if (_actionOverlay) {
        lv_obj_del_async(_actionOverlay);
        _actionOverlay = nullptr;
    }
    for (int i = 0; i < 3; i++) {
        _actionRows[i] = nullptr;
        _actionLabels[i] = nullptr;
    }

    _menuCount = count;
    _actionOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_actionOverlay, 244, 118);
    lv_obj_center(_actionOverlay);
    lv_obj_add_style(_actionOverlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_actionOverlay, 8, 0);
    lv_obj_clear_flag(_actionOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLbl = lv_label_create(_actionOverlay);
    lv_obj_set_style_text_font(titleLbl, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLbl, 220);
    lv_label_set_text(titleLbl, title);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 0);

    for (int i = 0; i < count && i < 3; i++) {
        lv_obj_t* row = lv_obj_create(_actionOverlay);
        lv_obj_set_size(row, 220, 24);
        lv_obj_set_pos(row, 4, 24 + i * 28);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == _menuIdx ? Theme::BG_HOVER : Theme::BG), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvMessagesScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (!self) return;
            self->_menuIdx = idx;
            KeyEvent enter = {};
            enter.enter = true;
            self->handleKey(enter);
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 200);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_center(lbl);
        _actionRows[i] = row;
        _actionLabels[i] = lbl;
    }
}

void LvMessagesScreen::showActionMenu(int peerIdx) {
    if (!_lxmf || peerIdx < 0 || peerIdx >= (int)_sortedPeers.size()) return;
    _focusActive = true;
    _lpPeerIdx = peerIdx;
    _lpState = LP_MENU;
    _menuIdx = 0;
    static const char* labels[] = {"Add Friend", "Delete Chat", "Cancel"};
    rebuildActionOverlay("CHAT ACTION", labels, 3);
}

void LvMessagesScreen::showDeleteConfirm() {
    _lpState = LP_CONFIRM_DELETE;
    _menuIdx = 0;
    static const char* labels[] = {"Delete Chat", "Cancel"};
    rebuildActionOverlay("DELETE CHAT?", labels, 2);
}

void LvMessagesScreen::addFocusedPeerToContacts() {
    if (!_am || _lpPeerIdx < 0 || _lpPeerIdx >= (int)_sortedPeers.size()) return;
    const auto& peerHex = _sortedPeers[_lpPeerIdx];
    bool added = false;
    bool alreadySaved = false;
    {
        CoreSync::RnsGuard backendGuard;
        const DiscoveredNode* existing = _am->findNodeByHex(peerHex);
        if (existing && !existing->saved) {
            auto& node = const_cast<DiscoveredNode&>(*existing);
            node.saved = true;
            _am->saveContacts();
            added = true;
        } else if (!existing) {
            _am->addManualContact(peerHex, "");
            added = true;
        } else {
            alreadySaved = true;
        }
    }
    if (added) {
        if (_ui) _ui->lvStatusBar().showToast("Added to friends", 1200);
    } else if (alreadySaved && _ui) {
        _ui->lvStatusBar().showToast("Already a friend", 1200);
    }
}

void LvMessagesScreen::deleteFocusedConversation() {
    if (!_lxmf || _lpPeerIdx < 0 || _lpPeerIdx >= (int)_sortedPeers.size()) return;
    const auto& peerHex = _sortedPeers[_lpPeerIdx];
    int unread = 0;
    {
        CoreSync::RnsGuard backendGuard;
        _lxmf->markRead(peerHex);
        _lxmf->deleteConversation(peerHex);
        unread = _lxmf->unreadCount();
    }
    if (_ui) {
        _ui->lvStatusBar().showToast("Chat deleted", 1200);
        _ui->lvTabBar().setUnreadCount(LvTabBar::TAB_MSGS, unread);
    }
    _lastConvCount = -1;
    rebuildList();
}

bool LvMessagesScreen::handleLongPress() {
    if (!_lxmf) return false;
    // Only consume long-press once the user has actively navigated into
    // the list. Otherwise let main.cpp blank the screen.
    if (!_focusActive) return false;
    int idx = getFocusedPeerIdx();
    if (idx < 0 || idx >= (int)_sortedPeers.size()) return false;
    showActionMenu(idx);
    return true;
}

bool LvMessagesScreen::handleKey(const KeyEvent& event) {
    if (!_lxmf) return false;

    if (!_focusActive && (event.up || event.down || event.enter)) {
        _focusActive = true;
        lv_obj_t* focused = lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_add_state(focused, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        return true;
    }

    // Long-press menu mode
    if (_lpState == LP_MENU) {
        if (event.up || event.down) {
            _menuIdx = (_menuIdx + (event.down ? 1 : -1) + _menuCount) % _menuCount;
            static const char* labels[] = {"Add Friend", "Delete Chat", "Cancel"};
            rebuildActionOverlay("CHAT ACTION", labels, 3);
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_menuIdx == 0) {
                addFocusedPeerToContacts();
            } else if (_menuIdx == 1) {
                showDeleteConfirm();
                return true;
            } else {
                if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
            }
            hideActionMenu();
            return true;
        }
        if (event.del || event.character == 8 || event.character == 0x1B) {
            hideActionMenu();
            if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
            return true;
        }
        return true;
    }

    // Confirm delete mode
    if (_lpState == LP_CONFIRM_DELETE) {
        if (event.up || event.down) {
            _menuIdx = (_menuIdx + 1) % _menuCount;
            static const char* labels[] = {"Delete Chat", "Cancel"};
            rebuildActionOverlay("DELETE CHAT?", labels, 2);
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (_menuIdx == 0) {
                deleteFocusedConversation();
            } else if (_ui) {
                _ui->lvStatusBar().showToast("Cancelled", 800);
            }
            hideActionMenu();
            return true;
        }
        hideActionMenu();
        if (_ui) _ui->lvStatusBar().showToast("Cancelled", 800);
        return true;
    }

    // Let LVGL focus group handle up/down/enter navigation
    return false;
}
