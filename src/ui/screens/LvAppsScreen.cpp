#include "LvAppsScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include <Arduino.h>
#include "fonts/fonts.h"

namespace {

// =============================================================================
// Layout — 320×194 content area (with tab bar visible). Five tiles fit
// in a 2-col, 3-row grid; bottom-right cell intentionally empty.
//
// Tightened from the prior numbers (kPad=6, kTitleH=22, kTileH=52, kGap=6)
// so the last row (Encrypt) sits ~16 px above the tab bar instead of
// being visually flush with it. Theme's base spacing unit is 4 px
// (Theme::SP_1=4), so kPad/kGap use that unit directly for visual rhythm.
// =============================================================================
constexpr lv_coord_t kPad         = 4;
constexpr lv_coord_t kTitleH      = 18;     // title strip (label sits at y=2)
constexpr lv_coord_t kGridY       = kTitleH + 2;   // 2 px gutter under title
constexpr lv_coord_t kGap         = 4;
constexpr lv_coord_t kCols        = 2;
constexpr lv_coord_t kTileH       = 50;
constexpr lv_coord_t kTileW       = (Theme::CONTENT_W - (kPad * 2) - kGap) / kCols;  // 154

// Bottom-of-row-3 = kGridY + 2*(kTileH+kGap) + kTileH = 20 + 2*54 + 50 = 178.
// CONTENT_H=194 → 16 px clearance to the tab bar (>= the 6-8 px the task asked for).

// Tile names + soon-flag — declared in display order (top→bottom,
// left→right) so the on-screen grid mirrors this array.
struct TileSpec {
    const char* title;
    bool soon;
};

constexpr TileSpec kSpecs[LvAppsScreen::TILE_COUNT] = {
    {"Map",     false},
    {"Notes",   false},   // live — see LvNotesListScreen / LvNotesEditScreen
    {"Files",   false},   // live — see LvFilesScreen / LvReaderScreen
    {"GPS",     true},
    {"Encrypt", true},
};

}  // namespace

void LvAppsScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Title strip — single line, mirrors the "Apps" tab label so the
    // visible header agrees with the tab bar selection. Font 10 (was 12)
    // shrinks the strip and shifts the tile grid up by a few px, leaving
    // breathing room above the tab bar.
    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, Theme::CONTENT_W - (kPad * 2));
    lv_obj_set_pos(title, kPad, 2);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_label_set_text(title, "APPS");

    // Build tiles in declaration order — grid coordinates derived from
    // the index so reordering the enum reshuffles the screen.
    for (int i = 0; i < TILE_COUNT; i++) {
        int col = i % kCols;
        int row = i / kCols;
        lv_coord_t x = kPad + col * (kTileW + kGap);
        lv_coord_t y = kGridY + row * (kTileH + kGap);

        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, kTileW, kTileH);
        lv_obj_set_style_bg_color(btn, lv_color_hex(Theme::BG_ELEVATED), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);

        // Title label — slightly inset so the SOON badge can sit top-right.
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kTileW - 16);
        lv_label_set_text(lbl, kSpecs[i].title);
        lv_obj_set_pos(lbl, 8, 16);

        // Optional "SOON" tag — distinguishes placeholder tiles from the
        // live Map tile at a glance without forcing the user to tap.
        lv_obj_t* badge = nullptr;
        if (kSpecs[i].soon) {
            badge = lv_label_create(btn);
            lv_obj_set_style_text_font(badge, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(badge, lv_color_hex(Theme::TEXT_MUTED), 0);
            lv_obj_set_style_text_letter_space(badge, 1, 0);
            lv_label_set_text(badge, "SOON");
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 4);
        }

        _tiles[i].btn = btn;
        _tiles[i].titleLbl = lbl;
        _tiles[i].badgeLbl = badge;

        lv_group_add_obj(LvInput::group(), btn);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = (LvAppsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            // Mirror the handleKey() activation path exactly so touch
            // and Enter don't diverge in side-effects.
            self->focusTile(idx);
            switch (idx) {
                case TILE_MAP:     if (self->_onOpenMap)     self->_onOpenMap();     break;
                case TILE_NOTES:   if (self->_onOpenNotes)   self->_onOpenNotes();
                                    else if (self->_ui) self->_ui->lvStatusBar().showToast("Coming soon", 1500);
                                    break;
                case TILE_FILES:   if (self->_onOpenFiles)   self->_onOpenFiles();
                                    else if (self->_ui) self->_ui->lvStatusBar().showToast("Coming soon", 1500);
                                    break;
                case TILE_GPS:     if (self->_onOpenGps)     self->_onOpenGps();
                                    else if (self->_ui) self->_ui->lvStatusBar().showToast("Coming soon", 1500);
                                    break;
                case TILE_ENCRYPT: if (self->_onOpenEncrypt) self->_onOpenEncrypt();
                                    else if (self->_ui) self->_ui->lvStatusBar().showToast("Coming soon", 1500);
                                    break;
                default: break;
            }
        }, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);
    }

    refreshFocusStyles();
}

void LvAppsScreen::destroyUI() {
    for (int i = 0; i < TILE_COUNT; i++) {
        _tiles[i].btn = nullptr;
        _tiles[i].titleLbl = nullptr;
        _tiles[i].badgeLbl = nullptr;
    }
    LvScreen::destroyUI();
}

void LvAppsScreen::onEnter() {
    _focusIdx = -1;
    refreshFocusStyles();
}

void LvAppsScreen::refreshUI() {
    // The grid is static — no per-tick data updates. Refresh focus visuals
    // so a programmatic tab-cycle leaves the screen in a consistent state.
    refreshFocusStyles();
}

void LvAppsScreen::focusTile(int idx) {
    if (idx < 0 || idx >= TILE_COUNT) return;
    _focusIdx = idx;
    refreshFocusStyles();
}

void LvAppsScreen::refreshFocusStyles() {
    for (int i = 0; i < TILE_COUNT; i++) {
        lv_obj_t* btn = _tiles[i].btn;
        if (!btn) continue;
        bool sel = (i == _focusIdx);
        lv_obj_set_style_border_color(btn, lv_color_hex(
            sel ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(
            sel ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED), 0);
        if (sel) lv_obj_add_state(btn, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        else lv_obj_clear_state(btn, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    }
}

bool LvAppsScreen::handleKey(const KeyEvent& event) {
    // First key press promotes focus to the (currently) first tile so
    // the user doesn't have to guess where the cursor is. After that,
    // arrows move tile-by-tile, Enter activates, no other keys consumed.
    if (_focusIdx < 0 && (event.up || event.down || event.left || event.right || event.enter)) {
        focusTile(0);
        return true;
    }

    if (event.left && _focusIdx > 0 && (_focusIdx % kCols) > 0) {
        focusTile(_focusIdx - 1);
        return true;
    }
    if (event.right && _focusIdx >= 0 && (_focusIdx % kCols) < (kCols - 1)
        && (_focusIdx + 1) < TILE_COUNT) {
        focusTile(_focusIdx + 1);
        return true;
    }
    if (event.up && _focusIdx >= kCols) {
        focusTile(_focusIdx - kCols);
        return true;
    }
    if (event.down && _focusIdx >= 0 && (_focusIdx + kCols) < TILE_COUNT) {
        focusTile(_focusIdx + kCols);
        return true;
    }

    if (event.enter || event.character == '\n' || event.character == '\r') {
        int idx = _focusIdx >= 0 ? _focusIdx : 0;
        focusTile(idx);
        switch (idx) {
            case TILE_MAP:     if (_onOpenMap) _onOpenMap();     break;
            case TILE_NOTES:   if (_onOpenNotes) _onOpenNotes();
                                else if (_ui) _ui->lvStatusBar().showToast("Coming soon", 1500);
                                break;
            case TILE_FILES:   if (_onOpenFiles) _onOpenFiles();
                                else if (_ui) _ui->lvStatusBar().showToast("Coming soon", 1500);
                                break;
            case TILE_GPS:     if (_onOpenGps) _onOpenGps();
                                else if (_ui) _ui->lvStatusBar().showToast("Coming soon", 1500);
                                break;
            case TILE_ENCRYPT: if (_onOpenEncrypt) _onOpenEncrypt();
                                else if (_ui) _ui->lvStatusBar().showToast("Coming soon", 1500);
                                break;
            default: break;
        }
        return true;
    }

    return false;
}
