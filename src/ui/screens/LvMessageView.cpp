#include "LvMessageView.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvTabBar.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <time.h>

std::string LvMessageView::getPeerName() {
    if (_am) {
        std::string name = _am->lookupName(_peerHex);
        if (!name.empty()) return name;
    }
    return _peerHex.substr(0, 12);
}

void LvMessageView::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    // Use flex column layout: header, messages (grows), input
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    const lv_font_t* font = &lv_font_montserrat_12;
    int headerH = 22;
    int inputH = 28;

    // Header bar (top)
    _header = lv_obj_create(parent);
    lv_obj_set_size(_header, lv_pct(100), headerH);
    lv_obj_set_style_bg_color(_header, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_header, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_header, 1, 0);
    lv_obj_set_style_border_side(_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    _lblHeader = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeader, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblHeader, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_align(_lblHeader, LV_ALIGN_LEFT_MID, 4, 0);

    // Message scroll area (middle, grows to fill)
    _msgScroll = lv_obj_create(parent);
    lv_obj_set_width(_msgScroll, lv_pct(100));
    lv_obj_set_flex_grow(_msgScroll, 1);
    lv_obj_set_style_bg_color(_msgScroll, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_msgScroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_msgScroll, 0, 0);
    lv_obj_set_style_pad_all(_msgScroll, 4, 0);
    lv_obj_set_style_pad_row(_msgScroll, 6, 0);
    lv_obj_set_style_radius(_msgScroll, 0, 0);
    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);

    // Input row (bottom, just above tab bar)
    _inputRow = lv_obj_create(parent);
    lv_obj_set_size(_inputRow, lv_pct(100), inputH);
    lv_obj_set_style_bg_color(_inputRow, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_inputRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_inputRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_inputRow, 1, 0);
    lv_obj_set_style_border_side(_inputRow, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(_inputRow, 3, 0);
    lv_obj_set_style_radius(_inputRow, 0, 0);
    lv_obj_clear_flag(_inputRow, LV_OBJ_FLAG_SCROLLABLE);

    _textarea = lv_textarea_create(_inputRow);
    lv_obj_set_size(_textarea, Theme::CONTENT_W - 50, 22);
    lv_obj_align(_textarea, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(_textarea, true);
    lv_textarea_set_placeholder_text(_textarea, "Type message...");
    lv_obj_set_style_bg_color(_textarea, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_border_width(_textarea, 0, 0);
    lv_obj_set_style_text_color(_textarea, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_text_font(_textarea, font, 0);
    lv_obj_set_style_pad_all(_textarea, 2, 0);

    _btnSend = lv_btn_create(_inputRow);
    lv_obj_set_size(_btnSend, 40, 22);
    lv_obj_align(_btnSend, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_btnSend, lv_color_hex(Theme::SELECTION_BG), 0);
    lv_obj_set_style_radius(_btnSend, 3, 0);
    lv_obj_set_style_pad_all(_btnSend, 0, 0);
    lv_obj_t* sendLbl = lv_label_create(_btnSend);
    lv_obj_set_style_text_font(sendLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(sendLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(sendLbl, "Send");
    lv_obj_center(sendLbl);
}

void LvMessageView::destroyUI() {
    _header = nullptr;
    _lblHeader = nullptr;
    _msgScroll = nullptr;
    _inputRow = nullptr;
    _textarea = nullptr;
    _btnSend = nullptr;
    LvScreen::destroyUI();
}

void LvMessageView::onEnter() {
    if (_lxmf) {
        _lxmf->markRead(_peerHex);
        // Update unread badge on Messages tab
        if (_ui) _ui->lvTabBar().setUnreadCount(LvTabBar::TAB_MSGS, _lxmf->unreadCount());
        // Register status callback for live updates
        std::string peer = _peerHex;
        _lxmf->setStatusCallback([this, peer](const std::string& peerHex, double, LXMFStatus) {
            if (peerHex == peer) _lastRefreshMs = 0; // Force rebuild on next refreshUI
        });
    }
    _lastMsgCount = -1;
    _lastRefreshMs = 0;
    _inputText.clear();

    if (_lblHeader) {
        char header[48];
        snprintf(header, sizeof(header), "< %s", getPeerName().c_str());
        lv_label_set_text(_lblHeader, header);
    }
    if (_textarea) {
        lv_textarea_set_text(_textarea, "");
    }
    rebuildMessages();
}

void LvMessageView::onExit() {
    if (_lxmf) _lxmf->setStatusCallback(nullptr);
    _inputText.clear();
    _cachedMsgs.clear();
}

void LvMessageView::refreshUI() {
    if (!_lxmf) return;
    unsigned long now = millis();
    if (now - _lastRefreshMs >= REFRESH_INTERVAL_MS) {
        auto newMsgs = _lxmf->getMessages(_peerHex);
        _lastRefreshMs = now;
        // Rebuild if count changed or any status changed
        bool changed = (newMsgs.size() != _cachedMsgs.size());
        if (!changed) {
            for (size_t i = 0; i < newMsgs.size(); i++) {
                if (newMsgs[i].status != _cachedMsgs[i].status) { changed = true; break; }
            }
        }
        if (changed) {
            _cachedMsgs = newMsgs;
            _lastMsgCount = (int)_cachedMsgs.size();
            rebuildMessages();
        }
    }
}

void LvMessageView::rebuildMessages() {
    if (!_lxmf || !_msgScroll) return;

    _cachedMsgs = _lxmf->getMessages(_peerHex);
    _lastRefreshMs = millis();
    lv_obj_clean(_msgScroll);

    const lv_font_t* font = &lv_font_montserrat_12;
    int maxBubbleW = Theme::CONTENT_W * 3 / 4;

    for (const auto& msg : _cachedMsgs) {
        // Bubble container
        lv_obj_t* bubble = lv_obj_create(_msgScroll);
        lv_obj_set_width(bubble, Theme::CONTENT_W - 12);
        lv_obj_set_style_pad_all(bubble, 0, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(bubble, LV_SIZE_CONTENT);

        // Message text in a rounded box
        lv_obj_t* box = lv_obj_create(bubble);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_set_style_pad_all(box, 5, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_width(box, LV_SIZE_CONTENT);
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        if (msg.incoming) {
            lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_IN_BG), 0);
            lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
        } else {
            lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_OUT_BG), 0);
            lv_obj_align(box, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);

        // Message label with word wrap — status-based colors for outgoing
        uint32_t textColor = Theme::ACCENT; // incoming default
        if (!msg.incoming) {
            switch (msg.status) {
                case LXMFStatus::QUEUED:
                case LXMFStatus::SENDING:
                    textColor = Theme::WARNING_CLR; break;
                case LXMFStatus::SENT:
                case LXMFStatus::DELIVERED:
                    textColor = Theme::PRIMARY; break;
                case LXMFStatus::FAILED:
                    textColor = Theme::ERROR_CLR; break;
                default:
                    textColor = Theme::PRIMARY; break;
            }
        }
        lv_obj_t* lbl = lv_label_create(box);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(textColor), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, maxBubbleW - 14);
        lv_label_set_text(lbl, msg.content.c_str());

        // Status indicator for outgoing
        if (!msg.incoming) {
            const char* ind = "~";
            uint32_t indColor = Theme::MUTED;
            if (msg.status == LXMFStatus::SENT || msg.status == LXMFStatus::DELIVERED) {
                ind = "*"; indColor = Theme::ACCENT;
            } else if (msg.status == LXMFStatus::FAILED) {
                ind = "!"; indColor = Theme::ERROR_CLR;
            }
            lv_obj_t* statusLbl = lv_label_create(box);
            lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(statusLbl, lv_color_hex(indColor), 0);
            lv_label_set_text(statusLbl, ind);
            lv_obj_align(statusLbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }

        // Timestamp below bubble
        if (msg.timestamp > 1000000) {
            time_t t = (time_t)msg.timestamp;
            struct tm* tm = localtime(&t);
            if (tm) {
                char timeBuf[8];
                snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm->tm_hour, tm->tm_min);
                lv_obj_t* timeLbl = lv_label_create(bubble);
                lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);
                lv_obj_set_style_text_color(timeLbl, lv_color_hex(Theme::MUTED), 0);
                lv_label_set_text(timeLbl, timeBuf);
                if (msg.incoming) {
                    lv_obj_align_to(timeLbl, box, LV_ALIGN_OUT_BOTTOM_LEFT, 2, 1);
                } else {
                    lv_obj_align_to(timeLbl, box, LV_ALIGN_OUT_BOTTOM_RIGHT, -2, 1);
                }
            }
        }
    }

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(_msgScroll, LV_COORD_MAX, LV_ANIM_OFF);
}

void LvMessageView::sendCurrentMessage() {
    if (!_lxmf || _peerHex.empty() || _inputText.empty()) return;

    RNS::Bytes destHash;
    destHash.assignHex(_peerHex.c_str());
    _lxmf->sendMessage(destHash, _inputText.c_str());

    _inputText.clear();
    if (_textarea) lv_textarea_set_text(_textarea, "");
    rebuildMessages();
}

bool LvMessageView::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B) {
        if (_onBack) _onBack();
        return true;
    }

    if (event.del || event.character == 0x08) {
        if (!_inputText.empty()) {
            _inputText.pop_back();
            if (_textarea) lv_textarea_set_text(_textarea, _inputText.c_str());
        } else {
            if (_onBack) _onBack();
        }
        return true;
    }

    if (event.enter || event.character == '\n' || event.character == '\r') {
        sendCurrentMessage();
        return true;
    }

    // Scroll
    if (event.up) {
        if (_msgScroll) lv_obj_scroll_to_y(_msgScroll,
            lv_obj_get_scroll_y(_msgScroll) - 30, LV_ANIM_OFF);
        return true;
    }
    if (event.down) {
        if (_msgScroll) lv_obj_scroll_to_y(_msgScroll,
            lv_obj_get_scroll_y(_msgScroll) + 30, LV_ANIM_OFF);
        return true;
    }

    if (event.character >= 0x20 && event.character < 0x7F) {
        _inputText += (char)event.character;
        if (_textarea) lv_textarea_set_text(_textarea, _inputText.c_str());
        return true;
    }

    return false;
}
