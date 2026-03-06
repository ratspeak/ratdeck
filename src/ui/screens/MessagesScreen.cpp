#include "MessagesScreen.h"
#include "ui/Theme.h"
#include "hal/Display.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>

void MessagesScreen::onEnter() {
    _lastConvCount = -1;
    _selectedIdx = 0;
    markDirty();
}

void MessagesScreen::update() {
    if (!_lxmf) return;
    int convCount = (int)_lxmf->conversations().size();
    if (convCount != _lastConvCount) {
        _lastConvCount = convCount;
        markDirty();
    }
}

void MessagesScreen::draw(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);

    if (!_lxmf || _lxmf->conversations().empty()) {
        gfx.setTextColor(Theme::MUTED, Theme::BG);
        const char* msg = "No conversations";
        int tw = strlen(msg) * 6;
        gfx.setCursor(Theme::SCREEN_W / 2 - tw / 2, Theme::CONTENT_Y + Theme::CONTENT_H / 2 - 4);
        gfx.print(msg);
        return;
    }

    const auto& convs = _lxmf->conversations();
    int y = Theme::CONTENT_Y + 2;
    int rowH = 20;

    for (size_t i = 0; i < convs.size(); i++) {
        if (y + rowH > Theme::SCREEN_H - Theme::TAB_BAR_H) break;

        const auto& peerHex = convs[i];
        int unread = _lxmf->unreadCount(peerHex);

        // Selection highlight
        if ((int)i == _selectedIdx) {
            gfx.fillRect(0, y, Theme::SCREEN_W, rowH, Theme::SELECTION_BG);
        }

        // Peer name (lookup from AnnounceManager) or fallback to hex
        std::string displayName;
        if (_am) {
            const DiscoveredNode* node = _am->findNodeByHex(peerHex);
            if (node && !node->name.empty()) displayName = node->name;
        }
        if (displayName.empty()) displayName = peerHex.substr(0, 16);

        gfx.setTextColor(Theme::PRIMARY, (int)i == _selectedIdx ? Theme::SELECTION_BG : Theme::BG);
        gfx.setCursor(4, y + 6);
        gfx.print(displayName.c_str());

        // Unread badge
        if (unread > 0) {
            char badge[8];
            snprintf(badge, sizeof(badge), "(%d)", unread);
            gfx.setTextColor(Theme::BADGE_BG, (int)i == _selectedIdx ? Theme::SELECTION_BG : Theme::BG);
            gfx.setCursor(Theme::SCREEN_W - 30, y + 6);
            gfx.print(badge);
        }

        // Separator
        gfx.drawFastHLine(0, y + rowH - 1, Theme::SCREEN_W, Theme::BORDER);
        y += rowH;
    }
}

bool MessagesScreen::handleKey(const KeyEvent& event) {
    if (!_lxmf) return false;
    int count = (int)_lxmf->conversations().size();
    if (count == 0) return false;

    if (event.up) {
        if (_selectedIdx > 0) {
            _selectedIdx--;
            markDirty();
        }
        return true;
    }
    if (event.down) {
        if (_selectedIdx < count - 1) {
            _selectedIdx++;
            markDirty();
        }
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_selectedIdx < count && _onOpen) {
            _onOpen(_lxmf->conversations()[_selectedIdx]);
        }
        return true;
    }
    return false;
}
