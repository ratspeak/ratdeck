#include "MessageView.h"
#include "ui/Theme.h"
#include "hal/Display.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>
#include <time.h>

void MessageView::onEnter() {
    if (_lxmf) _lxmf->markRead(_peerHex);
    _lastMsgCount = -1;
    _scrollPixels = 0;
    _lastRefreshMs = 0;
    refreshMessages();
    rebuildLayout();
    markDirty();
}

void MessageView::onExit() {
    _inputText.clear();
    _cachedMsgs.clear();
    _layout.clear();
}

void MessageView::refreshMessages() {
    if (!_lxmf) return;
    _cachedMsgs = _lxmf->getMessages(_peerHex);
    _lastRefreshMs = millis();
}

void MessageView::update() {
    if (!_lxmf) return;
    unsigned long now = millis();
    if (now - _lastRefreshMs >= REFRESH_INTERVAL_MS) {
        int oldCount = (int)_cachedMsgs.size();
        refreshMessages();
        if ((int)_cachedMsgs.size() != oldCount) {
            _lastMsgCount = (int)_cachedMsgs.size();
            rebuildLayout();
            _scrollPixels = 0;  // snap to bottom on new message
            markDirty();
        }
    }
}

// =============================================================================
// Word wrap + layout
// =============================================================================
std::vector<std::string> MessageView::wordWrap(const std::string& text, int maxChars) {
    std::vector<std::string> lines;
    if (text.empty()) { lines.push_back(""); return lines; }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t remaining = text.size() - pos;
        if ((int)remaining <= maxChars) {
            lines.push_back(text.substr(pos));
            break;
        }
        // Find last space within maxChars
        int breakAt = maxChars;
        for (int i = maxChars; i > maxChars / 2; i--) {
            if (text[pos + i] == ' ') { breakAt = i; break; }
        }
        lines.push_back(text.substr(pos, breakAt));
        pos += breakAt;
        // Skip the space we broke at
        if (pos < text.size() && text[pos] == ' ') pos++;
    }
    return lines;
}

std::string MessageView::formatTime(double timestamp) {
    if (timestamp < 1000000) return "";
    time_t t = (time_t)timestamp;
    struct tm* tm = localtime(&t);
    if (!tm) return "";
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    return buf;
}

void MessageView::rebuildLayout() {
    _layout.clear();
    _totalContentH = 0;

    for (int i = 0; i < (int)_cachedMsgs.size(); i++) {
        const auto& msg = _cachedMsgs[i];
        MsgLayout ml;
        ml.msgIdx = i;
        ml.incoming = msg.incoming;
        ml.lines = wordWrap(msg.content, MAX_BUBBLE_CHARS);
        ml.timeStr = formatTime(msg.timestamp);

        int textH = (int)ml.lines.size() * LINE_H;
        ml.totalHeight = textH + BUBBLE_PAD * 2 + BUBBLE_GAP;
        // Add time line height if we have a timestamp
        if (!ml.timeStr.empty()) ml.totalHeight += 8;

        _totalContentH += ml.totalHeight;
        _layout.push_back(ml);
    }
}

// =============================================================================
// Drawing
// =============================================================================
void MessageView::draw(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);

    // Header — peer name
    gfx.setTextColor(Theme::ACCENT, Theme::BG);
    gfx.setCursor(4, Theme::CONTENT_Y + 2);
    std::string headerName;
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_peerHex);
        if (node && !node->name.empty()) headerName = node->name;
    }
    if (headerName.empty()) headerName = _peerHex.substr(0, 12);
    char header[48];
    snprintf(header, sizeof(header), "< %s", headerName.c_str());
    gfx.print(header);

    int headerBottom = Theme::CONTENT_Y + 12;
    gfx.drawFastHLine(0, headerBottom, Theme::SCREEN_W, Theme::BORDER);

    // Input area
    int inputY = Theme::SCREEN_H - Theme::TAB_BAR_H - 16;
    gfx.drawFastHLine(0, inputY - 2, Theme::SCREEN_W, Theme::BORDER);
    gfx.fillRect(0, inputY, Theme::SCREEN_W, 16, Theme::BG);

    gfx.setCursor(4, inputY + 4);
    if (_inputText.empty()) {
        gfx.setTextColor(Theme::MUTED, Theme::BG);
        gfx.print("Type message...");
    } else {
        gfx.setTextColor(Theme::PRIMARY, Theme::BG);
        int maxShow = (Theme::SCREEN_W - 44) / CHAR_W;
        if ((int)_inputText.length() > maxShow) {
            gfx.print(_inputText.substr(_inputText.length() - maxShow).c_str());
        } else {
            gfx.print(_inputText.c_str());
        }
    }

    // Cursor blink
    int maxShow = (Theme::SCREEN_W - 44) / CHAR_W;
    int cursorX = 4 + (int)std::min(_inputText.length(), (size_t)maxShow) * CHAR_W;
    if ((millis() / 500) % 2 == 0) {
        gfx.fillRect(cursorX, inputY + 2, 2, 10, Theme::ACCENT);
    }

    // Send button
    gfx.setTextColor(Theme::PRIMARY, Theme::SELECTION_BG);
    gfx.fillRect(Theme::SCREEN_W - 32, inputY, 30, 14, Theme::SELECTION_BG);
    gfx.setCursor(Theme::SCREEN_W - 30, inputY + 3);
    gfx.print("Send");

    // Message area with bubbles
    int msgTop = headerBottom + 2;
    int msgBottom = inputY - 3;
    int msgAreaH = msgBottom - msgTop;

    if (_layout.empty()) return;

    // Calculate visible region — draw from bottom up
    // _scrollPixels == 0 means showing the latest messages at bottom
    int contentBottom = _totalContentH;
    int viewBottom = contentBottom - _scrollPixels;
    int viewTop = viewBottom - msgAreaH;

    int y = msgTop;  // screen Y cursor
    int contentY = 0;  // content space Y cursor

    for (int li = 0; li < (int)_layout.size(); li++) {
        const auto& ml = _layout[li];
        int blockTop = contentY;
        int blockBottom = contentY + ml.totalHeight;

        // Skip blocks entirely above viewport
        if (blockBottom <= viewTop) {
            contentY = blockBottom;
            continue;
        }
        // Stop if we've passed the viewport
        if (blockTop >= viewBottom) break;

        // Map content Y to screen Y
        int screenY = msgTop + (blockTop - viewTop);

        const auto& msg = _cachedMsgs[ml.msgIdx];
        int textH = (int)ml.lines.size() * LINE_H;
        int bubbleH = textH + BUBBLE_PAD * 2;

        // Calculate bubble width from longest line
        int maxLineLen = 0;
        for (auto& line : ml.lines) {
            if ((int)line.length() > maxLineLen) maxLineLen = line.length();
        }
        int bubbleW = maxLineLen * CHAR_W + BUBBLE_PAD * 2 + 2;
        if (bubbleW < 30) bubbleW = 30;

        int bubbleX;
        uint32_t bubbleBg, textColor;

        if (ml.incoming) {
            bubbleX = 4;
            bubbleBg = Theme::MSG_IN_BG;
            textColor = Theme::ACCENT;
        } else {
            bubbleX = Theme::SCREEN_W - bubbleW - 4;
            bubbleBg = Theme::MSG_OUT_BG;
            textColor = Theme::PRIMARY;
        }

        // Clip to message area
        if (screenY >= msgTop && screenY + bubbleH <= msgBottom) {
            // Draw bubble background
            gfx.fillRoundRect(bubbleX, screenY, bubbleW, bubbleH, 3, bubbleBg);

            // Draw text lines
            gfx.setTextColor(textColor, bubbleBg);
            for (int j = 0; j < (int)ml.lines.size(); j++) {
                gfx.setCursor(bubbleX + BUBBLE_PAD + 1, screenY + BUBBLE_PAD + j * LINE_H);
                gfx.print(ml.lines[j].c_str());
            }

            // Status indicator for outgoing
            if (!ml.incoming) {
                const char* ind = "~";
                uint32_t indColor = Theme::MUTED;
                if (msg.status == LXMFStatus::SENT || msg.status == LXMFStatus::DELIVERED) {
                    ind = "*"; indColor = Theme::ACCENT;
                } else if (msg.status == LXMFStatus::FAILED) {
                    ind = "!"; indColor = Theme::ERROR_CLR;
                }
                gfx.setTextColor(indColor, bubbleBg);
                gfx.setCursor(bubbleX + bubbleW - CHAR_W - BUBBLE_PAD, screenY + bubbleH - LINE_H - BUBBLE_PAD + 1);
                gfx.print(ind);
            }

            // Timestamp below bubble
            if (!ml.timeStr.empty()) {
                int timeX = ml.incoming ? bubbleX + 2 : bubbleX + bubbleW - (int)ml.timeStr.length() * CHAR_W - 2;
                gfx.setTextColor(Theme::MUTED, Theme::BG);
                gfx.setCursor(timeX, screenY + bubbleH + 1);
                gfx.print(ml.timeStr.c_str());
            }
        }

        contentY = blockBottom;
    }

    // Scroll indicator
    if (_totalContentH > msgAreaH) {
        int thumbH = std::max(8, msgAreaH * msgAreaH / _totalContentH);
        int maxScroll = _totalContentH - msgAreaH;
        int thumbY = msgTop + (msgAreaH - thumbH) * (_totalContentH - _scrollPixels - msgAreaH) / std::max(1, maxScroll);
        thumbY = std::max(msgTop, std::min(thumbY, msgBottom - thumbH));
        gfx.fillRect(Theme::SCREEN_W - 2, msgTop, 2, msgAreaH, Theme::BORDER);
        gfx.fillRect(Theme::SCREEN_W - 2, thumbY, 2, thumbH, Theme::SECONDARY);
    }
}

// =============================================================================
// Input handling
// =============================================================================
void MessageView::sendCurrentMessage() {
    if (!_lxmf || _peerHex.empty() || _inputText.empty()) return;

    RNS::Bytes destHash;
    destHash.assignHex(_peerHex.c_str());
    _lxmf->sendMessage(destHash, _inputText.c_str());

    _inputText.clear();
    // Immediately refresh to show sent message
    refreshMessages();
    rebuildLayout();
    _scrollPixels = 0;
    markDirty();
}

bool MessageView::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B) {
        if (_onBack) _onBack();
        return true;
    }

    if (event.del || event.character == 0x08) {
        if (!_inputText.empty()) {
            _inputText.pop_back();
            markDirty();
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
        int maxScroll = std::max(0, _totalContentH - 100);
        if (_scrollPixels < maxScroll) {
            _scrollPixels += 20;
            markDirty();
        }
        return true;
    }
    if (event.down) {
        if (_scrollPixels > 0) {
            _scrollPixels -= 20;
            if (_scrollPixels < 0) _scrollPixels = 0;
            markDirty();
        }
        return true;
    }

    if (event.character >= 0x20 && event.character < 0x7F) {
        _inputText += (char)event.character;
        markDirty();
        return true;
    }

    return false;
}
