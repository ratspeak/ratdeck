#include "MessageView.h"
#include "ui/Theme.h"
#include "hal/Display.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>

void MessageView::onEnter() {
    if (_lxmf) _lxmf->markRead(_peerHex);
    _lastMsgCount = -1;
    _scrollOffset = 0;
    markDirty();
}

void MessageView::onExit() {
    _inputText.clear();
}

void MessageView::update() {
    if (!_lxmf) return;
    auto msgs = _lxmf->getMessages(_peerHex);
    if ((int)msgs.size() != _lastMsgCount) {
        _lastMsgCount = (int)msgs.size();
        markDirty();
    }
}

void MessageView::draw(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);

    // Header — show node name if known
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

    // Divider under header
    int headerBottom = Theme::CONTENT_Y + 12;
    gfx.drawFastHLine(0, headerBottom, Theme::SCREEN_W, Theme::BORDER);

    // Input area (bottom of content)
    int inputY = Theme::SCREEN_H - Theme::TAB_BAR_H - 16;
    gfx.drawFastHLine(0, inputY - 2, Theme::SCREEN_W, Theme::BORDER);
    gfx.fillRect(0, inputY, Theme::SCREEN_W, 16, Theme::BG);

    // Input text
    gfx.setTextColor(Theme::PRIMARY, Theme::BG);
    gfx.setCursor(4, inputY + 4);
    if (_inputText.empty()) {
        gfx.setTextColor(Theme::MUTED, Theme::BG);
        gfx.print("Type message...");
    } else {
        // Show last ~48 chars if too long
        if (_inputText.length() > 48) {
            gfx.print(_inputText.substr(_inputText.length() - 48).c_str());
        } else {
            gfx.print(_inputText.c_str());
        }
    }

    // Cursor blink
    int cursorX = 4 + (int)std::min(_inputText.length(), (size_t)48) * 6;
    if ((millis() / 500) % 2 == 0) {
        gfx.fillRect(cursorX, inputY + 2, 2, 10, Theme::ACCENT);
    }

    // [Send] label
    gfx.setTextColor(Theme::PRIMARY, Theme::SELECTION_BG);
    gfx.fillRect(Theme::SCREEN_W - 32, inputY, 30, 14, Theme::SELECTION_BG);
    gfx.setCursor(Theme::SCREEN_W - 30, inputY + 3);
    gfx.print("Send");

    // Message area
    if (!_lxmf) return;
    auto msgs = _lxmf->getMessages(_peerHex);
    int msgAreaTop = headerBottom + 2;
    int msgAreaBottom = inputY - 4;
    int lineH = 12;
    int maxLines = (msgAreaBottom - msgAreaTop) / lineH;

    // Draw messages from bottom up
    int startIdx = (int)msgs.size() - maxLines - _scrollOffset;
    if (startIdx < 0) startIdx = 0;
    int endIdx = startIdx + maxLines;
    if (endIdx > (int)msgs.size()) endIdx = (int)msgs.size();

    int y = msgAreaTop;
    for (int i = startIdx; i < endIdx; i++) {
        const auto& msg = msgs[i];
        if (msg.incoming) {
            gfx.setTextColor(Theme::ACCENT, Theme::BG);
            gfx.setCursor(4, y);
        } else {
            // Status indicator: * = sent, ! = failed, ~ = queued/sending
            const char* indicator = "~";
            if (msg.status == LXMFStatus::SENT || msg.status == LXMFStatus::DELIVERED) indicator = "*";
            else if (msg.status == LXMFStatus::FAILED) indicator = "!";

            gfx.setTextColor(Theme::PRIMARY, Theme::BG);
            // Right-align outgoing + status
            int contentLen = std::min((int)msg.content.length(), 38);
            int tw = (contentLen + 2) * 6;  // +2 for space + indicator
            gfx.setCursor(Theme::SCREEN_W - tw - 4, y);
        }

        // Truncate long messages to one line for now
        if (msg.content.length() > 48) {
            gfx.print(msg.content.substr(0, 45).c_str());
            gfx.print("...");
        } else {
            gfx.print(msg.content.c_str());
        }

        // Show status indicator for outgoing messages
        if (!msg.incoming) {
            const char* ind = "~";
            uint32_t indColor = Theme::MUTED;
            if (msg.status == LXMFStatus::SENT || msg.status == LXMFStatus::DELIVERED) {
                ind = "*"; indColor = Theme::ACCENT;
            } else if (msg.status == LXMFStatus::FAILED) {
                ind = "!"; indColor = TFT_RED;
            }
            gfx.setTextColor(indColor, Theme::BG);
            gfx.print(" ");
            gfx.print(ind);
        }
        y += lineH;
    }
}

void MessageView::sendCurrentMessage() {
    if (!_lxmf || _peerHex.empty() || _inputText.empty()) return;

    RNS::Bytes destHash;
    destHash.assignHex(_peerHex.c_str());
    _lxmf->sendMessage(destHash, _inputText.c_str());

    _inputText.clear();
    markDirty();
}

bool MessageView::handleKey(const KeyEvent& event) {
    // Escape goes back
    if (event.character == 0x1B) {
        if (_onBack) _onBack();
        return true;
    }

    // Backspace: delete text, or go back if input is empty
    if (event.del || event.character == 0x08) {
        if (!_inputText.empty()) {
            _inputText.pop_back();
            markDirty();
        } else {
            if (_onBack) _onBack();
        }
        return true;
    }

    // Enter sends message
    if (event.enter || event.character == '\n' || event.character == '\r') {
        sendCurrentMessage();
        return true;
    }

    // Arrow keys scroll messages (Alt+I/M)
    if (event.up) {
        if (_scrollOffset < _lastMsgCount - 5) {
            _scrollOffset++;
            markDirty();
        }
        return true;
    }
    if (event.down) {
        if (_scrollOffset > 0) {
            _scrollOffset--;
            markDirty();
        }
        return true;
    }

    // Printable characters → text input
    if (event.character >= 0x20 && event.character < 0x7F) {
        _inputText += (char)event.character;
        markDirty();
        return true;
    }

    return false;
}
