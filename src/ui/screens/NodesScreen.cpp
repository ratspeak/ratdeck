#include "NodesScreen.h"
#include "ui/Theme.h"
#include "hal/Display.h"
#include "reticulum/AnnounceManager.h"
#include <Arduino.h>

void NodesScreen::onEnter() {
    _lastNodeCount = -1;
    _selectedIdx = 0;
    markDirty();
}

void NodesScreen::update() {
    if (!_am) return;
    if (_am->nodeCount() != _lastNodeCount) {
        _lastNodeCount = _am->nodeCount();
        markDirty();
    }
}

void NodesScreen::draw(LGFX_TDeck& gfx) {
    gfx.setTextSize(1);

    if (!_am || _am->nodeCount() == 0) {
        gfx.setTextColor(Theme::MUTED, Theme::BG);
        const char* msg = "No nodes discovered";
        int tw = strlen(msg) * 6;
        gfx.setCursor(Theme::SCREEN_W / 2 - tw / 2, Theme::CONTENT_Y + Theme::CONTENT_H / 2 - 4);
        gfx.print(msg);
        return;
    }

    const auto& nodes = _am->nodes();
    int y = Theme::CONTENT_Y + 2;
    int rowH = 18;

    for (size_t i = 0; i < nodes.size(); i++) {
        if (y + rowH > Theme::SCREEN_H - Theme::TAB_BAR_H) break;

        const auto& node = nodes[i];

        // Selection highlight
        if ((int)i == _selectedIdx) {
            gfx.fillRect(0, y, Theme::SCREEN_W, rowH, Theme::SELECTION_BG);
        }

        uint32_t bgCol = (int)i == _selectedIdx ? Theme::SELECTION_BG : Theme::BG;

        // Name + identity hash (formatted with colons like own identity)
        std::string displayHash;
        if (!node.identityHex.empty() && node.identityHex.size() >= 12) {
            displayHash = node.identityHex.substr(0, 4) + ":" +
                          node.identityHex.substr(4, 4) + ":" +
                          node.identityHex.substr(8, 4);
        } else {
            displayHash = node.hash.toHex().substr(0, 8);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s [%s]", node.name.c_str(), displayHash.c_str());
        gfx.setTextColor(node.saved ? Theme::ACCENT : Theme::PRIMARY, bgCol);
        gfx.setCursor(4, y + 5);
        gfx.print(buf);

        // Hops + age (right side)
        unsigned long ageSec = (millis() - node.lastSeen) / 1000;
        char infoBuf[24];
        if (ageSec < 60) snprintf(infoBuf, sizeof(infoBuf), "%dhop %lus", node.hops, ageSec);
        else snprintf(infoBuf, sizeof(infoBuf), "%dhop %lum", node.hops, ageSec / 60);
        int tw = strlen(infoBuf) * 6;
        gfx.setTextColor(Theme::SECONDARY, bgCol);
        gfx.setCursor(Theme::SCREEN_W - tw - 4, y + 5);
        gfx.print(infoBuf);

        y += rowH;
    }
}

bool NodesScreen::handleKey(const KeyEvent& event) {
    if (!_am) return false;
    int count = _am->nodeCount();
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
        if (_selectedIdx < count && _onSelect) {
            _onSelect(_am->nodes()[_selectedIdx].hash.toHex());
        }
        return true;
    }
    return false;
}
