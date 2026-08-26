#pragma once

#include "ui/UIManager.h"
#include <functional>

// =============================================================================
// Apps tab — grid launcher that hosts app tiles. Map is the live tile;
// Notes / Files / GPS / Encrypt are placeholders ("SOON") until those
// modules ship. Peers is a primary tab (TAB_PEERS), not an Apps tile.
// =============================================================================

class LvAppsScreen : public LvScreen {
public:
    // Tile identifiers — kept in declaration order so the on-screen grid
    // (top-to-bottom, left-to-right) matches this enum. Exposed because
    // click / key handlers in the .cpp switch on them.
    enum Tile {
        TILE_MAP = 0,
        TILE_NOTES,
        TILE_FILES,
        TILE_GPS,
        TILE_ENCRYPT,
        TILE_COUNT
    };

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setUIManager(class UIManager* ui) { _ui = ui; }

    // Tappable tile callbacks. Wired by main.cpp because each opens a
    // different module (or toasts "Coming soon" for unimplemented tiles).
    using TileCallback = std::function<void()>;
    void setOpenMapCallback(TileCallback cb)     { _onOpenMap = cb; }
    void setOpenNotesCallback(TileCallback cb)   { _onOpenNotes = cb; }
    void setOpenFilesCallback(TileCallback cb)   { _onOpenFiles = cb; }
    void setOpenGpsCallback(TileCallback cb)     { _onOpenGps = cb; }
    void setOpenEncryptCallback(TileCallback cb) { _onOpenEncrypt = cb; }

    const char* title() const override { return "Apps"; }

private:
    // Per-tile widget handles for focus tracking and visual updates.
    struct TileWidgets {
        lv_obj_t* btn = nullptr;
        lv_obj_t* titleLbl = nullptr;
        lv_obj_t* badgeLbl = nullptr;  // null when no status badge
    };

    void focusTile(int idx);
    void refreshFocusStyles();

    class UIManager* _ui = nullptr;
    TileCallback _onOpenMap;
    TileCallback _onOpenNotes;
    TileCallback _onOpenFiles;
    TileCallback _onOpenGps;
    TileCallback _onOpenEncrypt;

    TileWidgets _tiles[TILE_COUNT] = {};
    int _focusIdx = -1;   // -1 until the user navigates
};
