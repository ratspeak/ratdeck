#include "LvMessageView.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvTabBar.h"
#include "platform/CoreSync.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "util/PerfTrace.h"
#include <Arduino.h>
#include <time.h>
#include <cmath>
#include "fonts/fonts.h"

namespace {

constexpr int kHeaderH = 36;
constexpr int kInputH = 31;
constexpr int kComposerButtonW = 44;
constexpr int kBubbleMaxW = Theme::CONTENT_W * 3 / 4;
constexpr unsigned long kFreshNodeMs = 10UL * 60UL * 1000UL;
constexpr const char* kComposerPlaceholder = "Message...";

bool isPendingStatus(LXMFStatus status) {
    return status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING;
}

uint32_t bubbleBorderColor(const LXMFMessage& msg) {
    if (msg.incoming) return Theme::DIVIDER;
    if (msg.status == LXMFStatus::FAILED) return Theme::ERROR_CLR;
    if (isPendingStatus(msg.status)) return Theme::WARNING_CLR;
    if (msg.status == LXMFStatus::DELIVERED) return Theme::PRIMARY_MUTED;
    return Theme::BORDER;
}

uint32_t bubbleBorderColor(LXMFStatus status) {
    if (status == LXMFStatus::FAILED) return Theme::ERROR_CLR;
    if (isPendingStatus(status)) return Theme::WARNING_CLR;
    if (status == LXMFStatus::DELIVERED) return Theme::PRIMARY_MUTED;
    return Theme::BORDER;
}

bool formatClock(double ts, char* out, size_t outLen) {
    if (!out || outLen == 0 || ts <= 1700000000) return false;
    time_t t = (time_t)ts;
    struct tm* tm = localtime(&t);
    if (!tm) return false;
    snprintf(out, outLen, "%02d:%02d", tm->tm_hour, tm->tm_min);
    return true;
}

int textWidthForBubble(const std::string& content) {
    size_t longest = 0;
    size_t current = 0;
    for (char ch : content) {
        if (ch == '\n' || ch == '\r') {
            if (current > longest) longest = current;
            current = 0;
        } else {
            current++;
        }
    }
    if (current > longest) longest = current;
    if (content.size() > 34 || longest > 28) return kBubbleMaxW - 18;
    int width = (int)longest * 7 + 12;
    if (width < 54) width = 54;
    int maxW = kBubbleMaxW - 18;
    if (width > maxW) width = maxW;
    return width;
}

void makeTransparent(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

std::string LvMessageView::getPeerName() {
    if (_am) {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return _peerHex.substr(0, 12);
        std::string name = _am->lookupName(_peerHex);
        if (!name.empty()) return name;
    }
    return _peerHex.substr(0, 12);
}

void LvMessageView::updateHeader() {
    if (!_lblHeader) return;

    std::string name = getPeerName();
    lv_label_set_text(_lblHeader, name.c_str());

    const DiscoveredNode* node = nullptr;
    CoreSync::RnsTryGuard backendGuard(0);
    if (_am && backendGuard.held()) {
        node = _am->findNodeByHex(_peerHex);
    }
    const char* state = "UNKNOWN";
    uint32_t stateColor = Theme::TEXT_MUTED;

    if (node) {
        bool fresh = node->lastSeen != 0 && (millis() - node->lastSeen) <= kFreshNodeMs;
        state = fresh ? "ONLINE" : (node->saved ? "FRIEND" : "STALE");
        stateColor = fresh ? Theme::SUCCESS : (node->saved ? Theme::PRIMARY : Theme::TEXT_SECONDARY);
    }

    if (_lblHeaderMeta) {
        lv_label_set_text(_lblHeaderMeta, _peerHex.c_str());
    }
    if (_lblHeaderState) {
        lv_label_set_text(_lblHeaderState, state);
        lv_obj_set_style_text_color(_lblHeaderState, lv_color_hex(stateColor), 0);
    }
}

void LvMessageView::markVisibleConversationRead() {
    if (!_markReadPending || !_lxmf) return;

    CoreSync::RnsTryGuard backendGuard(0);
    if (!backendGuard.held()) return;

    unsigned long startMs = PerfTrace::nowMs();
    _lxmf->markRead(_peerHex);
    _markReadPending = false;
    if (_ui) {
        _ui->lvTabBar().setUnreadCount(LvTabBar::TAB_MSGS, _lxmf->unreadCount());
    }
    unsigned long elapsed = PerfTrace::elapsedMs(startMs);
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS)) {
        Serial.printf("[PERF] Chat markRead: peer=%s total=%lums\n",
                      _peerHex.substr(0, 8).c_str(), elapsed);
    }
}

void LvMessageView::updateComposerState() {
    if (!_btnSend) return;
    bool hasText = !_inputText.empty();
    lv_obj_set_style_border_color(_btnSend, lv_color_hex(hasText ? Theme::PRIMARY : Theme::BORDER), 0);
    lv_obj_set_style_bg_color(_btnSend, lv_color_hex(hasText ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED), 0);
    if (_textarea) {
        lv_obj_set_style_border_color(_textarea, lv_color_hex(hasText ? Theme::PRIMARY_MUTED : Theme::BORDER), 0);
    }
    refreshComposerPlaceholder();
}

void LvMessageView::refreshComposerPlaceholder() {
    if (!_textarea) return;
    bool focused = lv_obj_has_state(_textarea, LV_STATE_FOCUSED);
    lv_textarea_set_placeholder_text(_textarea,
        (_inputText.empty() && !focused) ? kComposerPlaceholder : "");
    updateComposerText();
}

void LvMessageView::updateComposerText() {
    if (!_textarea) return;

    bool focused = lv_obj_has_state(_textarea, LV_STATE_FOCUSED);
    bool showCaret = focused || !_inputText.empty();
    if (!showCaret) {
        lv_textarea_set_text(_textarea, "");
        return;
    }

    std::string display = _inputText;
    display += "_";
    lv_textarea_set_text(_textarea, display.c_str());
    lv_textarea_set_cursor_pos(_textarea, LV_TEXTAREA_CURSOR_LAST);
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

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Header bar (top)
    _header = lv_obj_create(parent);
    lv_obj_set_size(_header, lv_pct(100), kHeaderH);
    lv_obj_set_style_bg_color(_header, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_header, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_header, 1, 0);
    lv_obj_set_style_border_side(_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* backLbl = lv_label_create(_header);
    lv_obj_set_style_text_font(backLbl, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(backLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(backLbl, "<");
    lv_obj_align(backLbl, LV_ALIGN_LEFT_MID, 6, 0);

    _lblHeader = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeader, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_lblHeader, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_lblHeader, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_lblHeader, 188);
    lv_obj_set_pos(_lblHeader, 22, 3);

    _lblHeaderMeta = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeaderMeta, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_lblHeaderMeta, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_lblHeaderMeta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_lblHeaderMeta, Theme::CONTENT_W - 30);
    lv_obj_set_pos(_lblHeaderMeta, 22, 21);

    _lblHeaderState = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeaderState, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_lblHeaderState, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(_lblHeaderState, "UNKNOWN");
    lv_obj_align(_lblHeaderState, LV_ALIGN_TOP_RIGHT, -8, 8);

    // Make header tappable for back navigation
    lv_obj_add_flag(_header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_header, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);

    // Message scroll area (middle, grows to fill)
    _msgScroll = lv_obj_create(parent);
    lv_obj_set_width(_msgScroll, lv_pct(100));
    lv_obj_set_flex_grow(_msgScroll, 1);
    lv_obj_set_style_bg_color(_msgScroll, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_msgScroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_msgScroll, 0, 0);
    lv_obj_set_style_pad_all(_msgScroll, 6, 0);
    lv_obj_set_style_pad_row(_msgScroll, 7, 0);
    lv_obj_set_style_radius(_msgScroll, 0, 0);
    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(_msgScroll, LvTheme::styleScrollbar(), LV_PART_SCROLLBAR);

    // Input row (bottom, just above tab bar)
    _inputRow = lv_obj_create(parent);
    lv_obj_set_size(_inputRow, lv_pct(100), kInputH);
    lv_obj_set_style_bg_color(_inputRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_inputRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_inputRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_inputRow, 1, 0);
    lv_obj_set_style_border_side(_inputRow, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(_inputRow, 3, 0);
    lv_obj_set_style_radius(_inputRow, 0, 0);
    lv_obj_clear_flag(_inputRow, LV_OBJ_FLAG_SCROLLABLE);

    _textarea = lv_textarea_create(_inputRow);
    lv_obj_set_size(_textarea, Theme::CONTENT_W - kComposerButtonW - 12, 23);
    lv_obj_align(_textarea, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(_textarea, true);
    lv_textarea_set_max_length(_textarea, MAX_COMPOSER_CHARS + 1);
    lv_textarea_set_placeholder_text(_textarea, kComposerPlaceholder);
    lv_obj_add_style(_textarea, LvTheme::styleTextarea(), 0);
    lv_obj_add_style(_textarea, LvTheme::styleTextareaFocused(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_textarea, 1, 0);
    lv_obj_set_style_text_font(_textarea, font, 0);
    lv_obj_set_style_pad_all(_textarea, 2, 0);
    lv_obj_add_event_cb(_textarea, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED) {
            self->refreshComposerPlaceholder();
        } else if (code == LV_EVENT_DEFOCUSED) {
            self->refreshComposerPlaceholder();
        }
    }, LV_EVENT_ALL, this);

    _btnSend = lv_btn_create(_inputRow);
    lv_obj_set_size(_btnSend, kComposerButtonW, 23);
    lv_obj_align(_btnSend, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(_btnSend, LvTheme::styleBtn(), 0);
    lv_obj_set_style_pad_all(_btnSend, 0, 0);
    lv_obj_t* sendLbl = lv_label_create(_btnSend);
    lv_obj_set_style_text_font(sendLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(sendLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(sendLbl, "SEND");
    lv_obj_center(sendLbl);
    lv_obj_add_event_cb(_btnSend, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        if (self->_suppressNextSendClick) {
            self->_suppressNextSendClick = false;
            return;
        }
        self->sendCurrentMessage(false);
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_btnSend, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        self->_suppressNextSendClick = true;
        self->showSendModeMenu();
    }, LV_EVENT_LONG_PRESSED, this);

    updateHeader();
    updateComposerState();
}

void LvMessageView::destroyUI() {
    hideSendModeMenu();
    _header = nullptr;
    _lblHeader = nullptr;
    _lblHeaderMeta = nullptr;
    _lblHeaderState = nullptr;
    _msgScroll = nullptr;
    _inputRow = nullptr;
    _textarea = nullptr;
    _btnSend = nullptr;
    _statusLabels.clear();
    _textLabels.clear();
    _bubbleBoxes.clear();
    for (int i = 0; i < 3; i++) {
        _sendRows[i] = nullptr;
        _sendLabels[i] = nullptr;
    }
    LvScreen::destroyUI();
}

void LvMessageView::onEnter() {
    unsigned long startMs = PerfTrace::nowMs();
    unsigned long markReadMs = 0;
    unsigned long tabBadgeMs = 0;
    unsigned long callbackMs = 0;
    unsigned long resetMs = 0;
    unsigned long headerMs = 0;
    unsigned long rebuildMs = 0;
    if (_lxmf) {
        _markReadPending = true;
        _statusRefreshPending.store(false);
        // The callback runs on the network core; defer all cache/LVGL work to
        // refreshUI() on the UI core.
        std::string peer = _peerHex;
        unsigned long phaseMs = millis();
        {
            CoreSync::RnsGuard backendGuard;
            _lxmf->setStatusCallback([this, peer](const std::string& peerHex, double, uint32_t, LXMFStatus) {
                if (peerHex == peer) _statusRefreshPending.store(true);
            });
        }
        callbackMs = millis() - phaseMs;
    }
    unsigned long phaseMs = millis();
    _lastMsgCount = -1;
    _knownTotalCount = -1;
    _lastRefreshMs = 0;
    _inputText.clear();
    hideSendModeMenu();

    if (_textarea) {
        updateComposerText();
    }
    resetMs = millis() - phaseMs;
    phaseMs = millis();
    updateHeader();
    updateComposerState();
    headerMs = millis() - phaseMs;
    _cachedMsgs.clear();  // Force fresh load
    phaseMs = millis();
    rebuildMessages();
    rebuildMs = millis() - phaseMs;

    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
        Serial.printf("[PERF] Chat onEnter: peer=%s msgs=%d total=%lums markRead=%lums tab=%lums callback=%lums reset=%lums header=%lums rebuild=%lums\n",
                      _peerHex.substr(0, 8).c_str(), (int)_cachedMsgs.size(), elapsed,
                      markReadMs, tabBadgeMs, callbackMs, resetMs, headerMs, rebuildMs);
    }
}

void LvMessageView::onExit() {
    if (_lxmf) {
        CoreSync::RnsGuard backendGuard;
        _lxmf->setStatusCallback(nullptr);
    }
    _markReadPending = false;
    _statusRefreshPending.store(false);
    hideSendModeMenu();
    _inputText.clear();
    _cachedMsgs.clear();
    _statusLabels.clear();
    _textLabels.clear();
    _bubbleBoxes.clear();
}

void LvMessageView::refreshUI() {
    if (!_lxmf) return;
    unsigned long now = millis();
    markVisibleConversationRead();
    if (now - _lastRefreshMs < REFRESH_INTERVAL_MS) return;
    _lastRefreshMs = now;
    updateHeader();

    // Only reload from disk when message count changes (new messages arrive)
    CoreSync::RnsTryGuard backendGuard(0);
    if (!backendGuard.held()) return;
    bool statusChanged = _statusRefreshPending.exchange(false);
    auto* summary = _lxmf->getConversationSummary(_peerHex);
    int totalCount = summary ? summary->totalCount : -1;
    if (!statusChanged && summary && totalCount == _knownTotalCount) return;

    auto newMsgs = _lxmf->getRecentMessages(_peerHex, CHAT_VIEW_MAX_MESSAGES);
    int newKnownTotal = summary ? totalCount : (int)newMsgs.size();
    if (statusChanged || newKnownTotal != _knownTotalCount || newMsgs.size() != _cachedMsgs.size()) {
        bool canAppend = !statusChanged && !_cachedMsgs.empty() &&
            _cachedMsgs.size() < CHAT_VIEW_MAX_MESSAGES &&
            newMsgs.size() > _cachedMsgs.size();
        if (canAppend) {
            // Incremental append - only create widgets for new messages
            size_t oldCount = _cachedMsgs.size();
            _cachedMsgs = std::move(newMsgs);
            _lastMsgCount = (int)_cachedMsgs.size();
            _lastRefreshMs = millis();
            if (oldCount == 0) {
                rebuildMessages();
            } else {
                for (size_t i = oldCount; i < _cachedMsgs.size(); i++) {
                    appendMessage(_cachedMsgs[i]);
                }
            }
            lv_obj_scroll_to_y(_msgScroll, LV_COORD_MAX, LV_ANIM_OFF);
        } else {
            // Tail window shifted, count decreased, or cache was empty - full visible-window rebuild.
            _cachedMsgs = std::move(newMsgs);
            _lastMsgCount = (int)_cachedMsgs.size();
            rebuildMessages();
        }
        _knownTotalCount = newKnownTotal;
        // Mark as read since user is actively viewing this conversation
        _markReadPending = true;
        markVisibleConversationRead();
    }
}

void LvMessageView::appendMessage(const LXMFMessage& msg) {
    if (!_msgScroll) return;

    const lv_font_t* font = &lv_font_rsdeck_12;
    int textW = textWidthForBubble(msg.content);
    if (!msg.incoming && textW < 96) textW = 96;
    int boxW = textW + 16;

    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* bubble = lv_obj_create(_msgScroll);
    lv_obj_set_width(bubble, Theme::CONTENT_W - 12);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    makeTransparent(bubble);

    lv_obj_t* box = lv_obj_create(bubble);
    lv_obj_set_width(box, boxW);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(box, 7, 0);
    lv_obj_set_style_pad_right(box, 7, 0);
    lv_obj_set_style_pad_top(box, 5, 0);
    lv_obj_set_style_pad_bottom(box, 5, 0);
    lv_obj_set_style_pad_row(box, 3, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(bubbleBorderColor(msg)), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    if (msg.incoming) {
        lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_IN_BG), 0);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_OUT_BG), 0);
        lv_obj_align(box, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);

    // Message text color - incoming is plain text, outgoing reflects delivery status
    uint32_t textColor = Theme::TEXT_PRIMARY; // incoming default
    if (!msg.incoming) {
        switch (msg.status) {
            case LXMFStatus::QUEUED:
            case LXMFStatus::SENDING:
                textColor = Theme::TEXT_SECONDARY; break;
            case LXMFStatus::SENT:
            case LXMFStatus::DELIVERED:
                textColor = Theme::TEXT_PRIMARY; break;
            case LXMFStatus::FAILED:
                textColor = Theme::ERROR_CLR; break;
            default:
                textColor = Theme::TEXT_PRIMARY; break;
        }
    }
    lv_obj_t* lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(textColor), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, textW);
    lv_label_set_text(lbl, msg.content.c_str());

    char timeBuf[8] = {0};
    bool hasTime = formatClock(msg.timestamp, timeBuf, sizeof(timeBuf));
    bool needsMeta = hasTime || !msg.incoming;
    lv_obj_t* statusLbl = nullptr;
    if (needsMeta) {
        lv_obj_t* meta = lv_obj_create(box);
        lv_obj_set_size(meta, textW, 12);
        makeTransparent(meta);

        if (hasTime) {
            lv_obj_t* timeLbl = lv_label_create(meta);
            lv_obj_set_style_text_font(timeLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(timeLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
            lv_label_set_text(timeLbl, timeBuf);
            lv_obj_align(timeLbl, LV_ALIGN_LEFT_MID, 0, 0);
        }

        if (!msg.incoming) {
            statusLbl = lv_label_create(meta);
            lv_obj_set_style_text_font(statusLbl, &lv_font_rsdeck_10, 0);
            applyStatusGlyph(statusLbl, msg.status);
            lv_obj_align(statusLbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    if (!msg.incoming) {
        _statusLabels.push_back(statusLbl);
        _textLabels.push_back(lbl);
        _bubbleBoxes.push_back(box);
    } else {
        _statusLabels.push_back(nullptr);
        _textLabels.push_back(nullptr);
        _bubbleBoxes.push_back(nullptr);
    }

    lv_obj_update_layout(box);
    lv_obj_set_height(bubble, lv_obj_get_height(box));
}

void LvMessageView::rebuildMessages() {
    if (!_lxmf || !_msgScroll) return;
    unsigned long startMs = millis();
    unsigned long loadMs = 0;
    unsigned long cleanMs = 0;
    unsigned long emptyMs = 0;
    unsigned long appendMs = 0;
    unsigned long scrollMs = 0;
    bool loadedFromStore = false;

    // Only load from disk if _cachedMsgs is empty (first call or after send)
    if (_cachedMsgs.empty()) {
        CoreSync::RnsTryGuard backendGuard(0);
        if (!backendGuard.held()) return;
        unsigned long phaseMs = millis();
        _cachedMsgs = _lxmf->getRecentMessages(_peerHex, CHAT_VIEW_MAX_MESSAGES);
        loadMs = millis() - phaseMs;
        loadedFromStore = true;
    }
    if (_lxmf) {
        CoreSync::RnsTryGuard backendGuard(0);
        if (backendGuard.held()) {
            auto* summary = _lxmf->getConversationSummary(_peerHex);
            _knownTotalCount = summary ? summary->totalCount : (int)_cachedMsgs.size();
        } else if (_knownTotalCount < 0) {
            _knownTotalCount = (int)_cachedMsgs.size();
        }
    }
    _lastMsgCount = (int)_cachedMsgs.size();
    _lastRefreshMs = millis();
    unsigned long phaseMs = millis();
    lv_obj_clean(_msgScroll);
    cleanMs = millis() - phaseMs;
    _statusLabels.clear();
    _textLabels.clear();
    _bubbleBoxes.clear();

    if (_cachedMsgs.empty()) {
        phaseMs = millis();
        lv_obj_set_layout(_msgScroll, 0);

        lv_obj_t* empty = lv_obj_create(_msgScroll);
        lv_obj_set_size(empty, 264, 88);
        lv_obj_center(empty);
        lv_obj_set_style_bg_color(empty, lv_color_hex(Theme::BG_ELEVATED), 0);
        lv_obj_set_style_bg_opa(empty, LV_OPA_70, 0);
        lv_obj_set_style_border_color(empty, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(empty, 1, 0);
        lv_obj_set_style_radius(empty, 6, 0);
        lv_obj_set_style_pad_all(empty, 0, 0);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* icon = lv_label_create(empty);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(icon, LV_SYMBOL_ENVELOPE);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 13);

        lv_obj_t* title = lv_label_create(empty);
        lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_label_set_text(title, "No messages yet");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

        lv_obj_t* sub = lv_label_create(empty);
        lv_obj_set_style_text_font(sub, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_text(sub, "Thread is quiet");
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 54);
        emptyMs = millis() - phaseMs;
        unsigned long elapsed = millis() - startMs;
        if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
            Serial.printf("[PERF] Chat rebuild: peer=%s msgs=0 loaded=%s load=%lums clean=%lums empty=%lums append=0ms scroll=0ms total=%lums\n",
                          _peerHex.substr(0, 8).c_str(), loadedFromStore ? "yes" : "no",
                          loadMs, cleanMs, emptyMs, elapsed);
        }
        return;
    }

    phaseMs = millis();
    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);

    for (const auto& msg : _cachedMsgs) {
        appendMessage(msg);
    }
    appendMs = millis() - phaseMs;

    // Auto-scroll to bottom
    phaseMs = millis();
    lv_obj_scroll_to_y(_msgScroll, LV_COORD_MAX, LV_ANIM_OFF);
    scrollMs = millis() - phaseMs;

    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
        Serial.printf("[PERF] Chat rebuild: peer=%s msgs=%d loaded=%s load=%lums clean=%lums empty=0ms append=%lums scroll=%lums total=%lums\n",
                      _peerHex.substr(0, 8).c_str(), (int)_cachedMsgs.size(),
                      loadedFromStore ? "yes" : "no", loadMs, cleanMs,
                      appendMs, scrollMs, elapsed);
    }
}

void LvMessageView::updateMessageStatus(int msgIdx, LXMFStatus status) {
    if (msgIdx < 0 || msgIdx >= (int)_statusLabels.size()) return;
    lv_obj_t* statusLbl = _statusLabels[msgIdx];
    lv_obj_t* textLbl = _textLabels[msgIdx];
    lv_obj_t* bubbleBox = msgIdx < (int)_bubbleBoxes.size() ? _bubbleBoxes[msgIdx] : nullptr;
    if (!statusLbl) return;  // Incoming message, no status label

    applyStatusGlyph(statusLbl, status);
    if (bubbleBox) {
        lv_obj_set_style_border_color(bubbleBox, lv_color_hex(bubbleBorderColor(status)), 0);
    }

    // Update text color to match status
    if (textLbl) {
        uint32_t textColor = Theme::TEXT_PRIMARY;
        if (status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING) {
            textColor = Theme::TEXT_SECONDARY;
        } else if (status == LXMFStatus::FAILED) {
            textColor = Theme::ERROR_CLR;
        }
        lv_obj_set_style_text_color(textLbl, lv_color_hex(textColor), 0);
    }
}

void LvMessageView::applyStatusGlyph(lv_obj_t* lbl, LXMFStatus status) {
    if (!lbl) return;
    const char* glyph;
    uint32_t color;
    switch (status) {
        case LXMFStatus::DELIVERED:
            glyph = "sent";
            color = Theme::SUCCESS;
            break;
        case LXMFStatus::SENT:
            glyph = "sent";
            color = Theme::TEXT_MUTED;
            break;
        case LXMFStatus::FAILED:
            glyph = "failed";
            color = Theme::ERROR_CLR;
            break;
        case LXMFStatus::SENDING:
            glyph = "sending";
            color = Theme::WARNING_CLR;
            break;
        case LXMFStatus::QUEUED:
            glyph = "queued";
            color = Theme::WARNING_CLR;
            break;
        default:
            glyph = "draft";
            color = Theme::TEXT_MUTED;
            break;
    }
    lv_label_set_text(lbl, glyph);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

void LvMessageView::sendCurrentMessage(bool viaLink) {
    unsigned long startMs = PerfTrace::nowMs();
    if (!_lxmf || _peerHex.empty() || _inputText.empty()) return;
    size_t inputBytes = _inputText.size();
    if (_inputText.size() > MAX_COMPOSER_CHARS) {
#if RSDECK_PERF_TRACE
        Serial.printf("[PERF] Chat send: peer=%s bytes=%u link=%s queued=no reason=too_long total=%lums\n",
                      _peerHex.substr(0, 8).c_str(), (unsigned)inputBytes,
                      viaLink ? "yes" : "no", PerfTrace::elapsedMs(startMs));
#endif
        if (_ui) _ui->lvStatusBar().showToast("Message too long", 1500);
        return;
    }

    unsigned long hashMs = millis();
    RNS::Bytes destHash;
    destHash.assignHex(_peerHex.c_str());
    hashMs = millis() - hashMs;
    unsigned long queueStartMs = millis();
    bool queued;
    {
        CoreSync::RnsGuard backendGuard;
        queued = viaLink
            ? _lxmf->sendMessageViaLink(destHash, _inputText.c_str())
            : _lxmf->sendMessage(destHash, _inputText.c_str());
    }
    unsigned long queueMs = millis() - queueStartMs;
    if (!queued) {
#if RSDECK_PERF_TRACE
        Serial.printf("[PERF] Chat send: peer=%s bytes=%u link=%s queued=no reason=queue_full hash=%lums queue=%lums total=%lums\n",
                      _peerHex.substr(0, 8).c_str(), (unsigned)inputBytes,
                      viaLink ? "yes" : "no", hashMs, queueMs,
                      PerfTrace::elapsedMs(startMs));
#endif
        if (_ui) _ui->lvStatusBar().showToast("Message queue full", 1500);
        return;
    }
    if (viaLink && _ui) _ui->lvStatusBar().showToast("Link send queued", 1200);

    unsigned long composerStartMs = millis();
    _inputText.clear();
    updateComposerState();
    _cachedMsgs.clear();  // Force fresh load in rebuildMessages
    _knownTotalCount = -1;
    unsigned long composerMs = millis() - composerStartMs;
    unsigned long rebuildStartMs = millis();
    rebuildMessages();
    unsigned long rebuildMs = millis() - rebuildStartMs;
    unsigned long elapsed = millis() - startMs;
#if RSDECK_PERF_TRACE
    Serial.printf("[PERF] Chat send: peer=%s bytes=%u link=%s queued=yes hash=%lums queue=%lums composer=%lums rebuild=%lums msgs=%d total=%lums\n",
                  _peerHex.substr(0, 8).c_str(), (unsigned)inputBytes,
                  viaLink ? "yes" : "no", hashMs, queueMs, composerMs,
                  rebuildMs, (int)_cachedMsgs.size(), elapsed);
#endif
}

bool LvMessageView::handleKey(const KeyEvent& event) {
    if (_sendOverlay) {
        if (event.character == 0x1B || event.del || event.character == 0x08) {
            hideSendModeMenu();
            return true;
        }
        if (event.up || event.left) {
            _sendMenuIdx = (_sendMenuIdx + 2) % 3;
            updateSendModeMenu();
            return true;
        }
        if (event.down || event.right || event.tab) {
            _sendMenuIdx = (_sendMenuIdx + 1) % 3;
            updateSendModeMenu();
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            chooseSendMode(_sendMenuIdx);
            return true;
        }
        return true;
    }

    if (event.character == 0x1B) {
        if (_onBack) _onBack();
        return true;
    }

    if (event.del || event.character == 0x08) {
        if (!_inputText.empty()) {
            _inputText.pop_back();
            updateComposerState();
        } else {
            if (_onBack) _onBack();
        }
        return true;
    }

    if (event.enter || event.character == '\n' || event.character == '\r') {
        sendCurrentMessage(false);
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

    // Keep horizontal trackball movement and Tab inside the chat view so
    // composing or reading a thread does not accidentally cycle global tabs.
    if (event.left || event.right || event.tab) {
        return true;
    }

    if (event.character >= 0x20 && event.character < 0x7F) {
        if (_inputText.size() >= MAX_COMPOSER_CHARS) {
            if (_ui) _ui->lvStatusBar().showToast("Message too long", 900);
            return true;
        }
        unsigned long inputStartMs = millis();
        _inputText += (char)event.character;
        updateComposerState();
        unsigned long elapsed = millis() - inputStartMs;
        if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
            Serial.printf("[PERF] Chat input: peer=%s chars=%u total=%lums\n",
                          _peerHex.substr(0, 8).c_str(), (unsigned)_inputText.size(), elapsed);
        }
        return true;
    }

    return false;
}

bool LvMessageView::handleLongPress() {
    if (_inputText.empty()) return false;
    showSendModeMenu();
    return true;
}

void LvMessageView::showSendModeMenu() {
    if (_inputText.empty()) return;
    hideSendModeMenu();
    _sendMenuIdx = 0;

    _sendOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_sendOverlay, 244, 118);
    lv_obj_center(_sendOverlay);
    lv_obj_add_style(_sendOverlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_sendOverlay, 8, 0);
    lv_obj_clear_flag(_sendOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_sendOverlay);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(title, "Send mode");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    static const char* labels[3] = {"Send normally", "Send as link", "Cancel"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = lv_obj_create(_sendOverlay);
        lv_obj_set_size(row, 220, 24);
        lv_obj_set_pos(row, 12, 24 + i * 28);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvMessageView*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->chooseSendMode(idx);
        }, LV_EVENT_CLICKED, this);

        _sendLabels[i] = lv_label_create(row);
        lv_obj_set_style_text_font(_sendLabels[i], &lv_font_rsdeck_12, 0);
        lv_label_set_text(_sendLabels[i], labels[i]);
        lv_obj_center(_sendLabels[i]);
        _sendRows[i] = row;
    }

    updateSendModeMenu();
}

void LvMessageView::hideSendModeMenu() {
    if (_sendOverlay) {
        lv_obj_del_async(_sendOverlay);
        _sendOverlay = nullptr;
    }
    for (int i = 0; i < 3; i++) {
        _sendRows[i] = nullptr;
        _sendLabels[i] = nullptr;
    }
}

void LvMessageView::updateSendModeMenu() {
    for (int i = 0; i < 3; i++) {
        if (!_sendRows[i] || !_sendLabels[i]) continue;
        bool selected = i == _sendMenuIdx;
        lv_obj_set_style_bg_color(_sendRows[i],
            lv_color_hex(selected ? Theme::PRIMARY_SUBTLE : Theme::BG_SURFACE), 0);
        lv_obj_set_style_border_color(_sendRows[i],
            lv_color_hex(selected ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
        lv_obj_set_style_text_color(_sendLabels[i],
            lv_color_hex(selected ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
    }
}

void LvMessageView::chooseSendMode(int idx) {
    bool viaLink = idx == 1;
    if (idx == 2) {
        hideSendModeMenu();
        return;
    }
    hideSendModeMenu();
    sendCurrentMessage(viaLink);
}
