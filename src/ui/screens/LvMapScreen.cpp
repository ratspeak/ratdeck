#include "LvMapScreen.h"

#include "maps/TileCache.h"
#include "maps/SlippyMath.h"
#include "hal/GPSManager.h"
#include "hal/Trackball.h"
#include "hal/TouchInput.h"
#include "reticulum/AnnounceManager.h"
#include "ui/Theme.h"
#include "ui/UIManager.h"
#include "ui/LvTabBar.h"
#include "ui/LvTheme.h"
#include <Arduino.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "fonts/fonts.h"

// Shared hardware singletons — same `extern` pattern as Power.cpp uses
// for Display/Keyboard. The map screen is the only screen that reads
// raw trackball/touch state outside the LVGL input drivers, so an
// extern is cleaner than passing a setter from main.cpp's globals.
extern Trackball trackball;
extern TouchInput touch;

namespace {

// ---- HUD positioning helpers ----
//
// All HUD labels live on the clipped content area's top/bottom edges,
// overlaid on the tile grid. Coordinates are relative to the content
// parent (which UIManager sets at y=STATUS_BAR_H=20).
//
// Top labels sit in the top ~16 px so they're always above any tile
// imagery. Bottom labels are positioned so they sit just inside the
// content area's bottom edge (CONTENT_H = 194, label h = 14, so y=174
// puts the bottom at y=188 — 6 px above the tab bar at y=194..240).
// After the tile-grid formula fix in rebuildTiles() (so the 2-row grid
// covers the FULL visible vertical range with a buffer row below), the
// bottom labels are reliably ON TOP of the tile grid, not in empty
// space below it.

constexpr int kHudZoomX = 4;
constexpr int kHudZoomY = 2;
constexpr int kHudMapsetX = 4;
constexpr int kHudMapsetY = 174;     // CONTENT_H - 20 - 0 = 174

constexpr int kHudFollowX = 244;
constexpr int kHudFollowY = 2;
constexpr int kHudGpsX = 196;
constexpr int kHudGpsY = 174;

constexpr int kMarkerSize = 14;

// ---- Nav overlay layout ----
//
// Six buttons (4 directional arrows + 2 zoom) clustered in the
// bottom-right corner of the content area. Free area there is bounded
// by the FOLLOW HUD at the top (y=2..16) and the GPS HUD at the bottom
// (y=174..188), so we have roughly y=20..172 to work with.
//
// Layout (each button 28x28 square — touch-friendly on the 320x240
// resistive touch panel; 2-px gaps):
//
//         [   UP   ]                  (col 1, row 0)
//         [ L ][ R ]                  (col 0 / col 2, row 1)
//         [  DN  ]                    (col 1, row 2)
//         [  +  ]                    (col 1, row 3)
//         [  -  ]                    (col 1, row 4)
//
// Cluster origin (content-area coords) and footprint:
//   origin = (kNavOriginX, kNavOriginY)
//   width  = 3 * 28 + 2 * 2 = 88
//   height = 5 * 28 + 4 * 2 = 148
//
// Right edge at 228 + 88 = 316 leaves a 4 px gap to CONTENT_W=320.
// Bottom edge at  22 + 148 = 170 leaves a 4 px gap to GPS HUD at y=174.
// Top   edge at  22 leaves a 6 px gap below FOLLOW HUD bottom edge y=16.
// Vertical stride between rows = kNavBtnH + kNavBtnGap = 30.
constexpr int kNavBtnW     = 28;
constexpr int kNavBtnH     = 28;
constexpr int kNavBtnGap   = 2;
constexpr int kNavOriginX  = 228;
constexpr int kNavOriginY  = 22;
constexpr int kNavColCenter = kNavOriginX + kNavBtnW + kNavBtnGap;       // 258
constexpr int kNavColLeft   = kNavOriginX;                              // 228
constexpr int kNavColRight  = kNavOriginX + 2 * (kNavBtnW + kNavBtnGap);  // 288
constexpr int kNavRow0 = kNavOriginY;                                   //  22  (UP)
constexpr int kNavRow1 = kNavOriginY + 1 * (kNavBtnH + kNavBtnGap);      //  52  (L,R)
constexpr int kNavRow2 = kNavOriginY + 2 * (kNavBtnH + kNavBtnGap);      //  82  (DN)
constexpr int kNavRow3 = kNavOriginY + 3 * (kNavBtnH + kNavBtnGap);      // 112  (+)
constexpr int kNavRow4 = kNavOriginY + 4 * (kNavBtnH + kNavBtnGap);      // 142  (-)

// ---- Debug logging gate ----
//
// Set LV_MAP_DEBUG=1 to enable per-frame Serial logging of the map
// screen's tile grid / request flow. Defaults OFF so production builds
// stay quiet (one Serial.printf per refreshUI() tick at ~60 Hz would
// flood the 115200 baud line and slow the main loop). Toggle from
// platformio.ini build_flags with `-DLV_MAP_DEBUG=1` when investigating
// "no tiles render" reports on hardware.
#ifndef LV_MAP_DEBUG
#define LV_MAP_DEBUG 0
#endif

#if LV_MAP_DEBUG
#define MAP_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define MAP_LOG(...) ((void)0)
#endif

lv_obj_t* makeHudLabel(lv_obj_t* parent, int x, int y, int w, int h,
                       const lv_font_t* font, uint32_t color,
                       const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(lbl, 3, 0);
    lv_obj_set_style_pad_right(lbl, 3, 0);
    lv_obj_set_style_pad_top(lbl, 1, 0);
    lv_obj_set_style_pad_bottom(lbl, 1, 0);
    lv_obj_set_style_radius(lbl, 2, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lbl, text);
    return lbl;
}

}  // namespace

void LvMapScreen::createUI(lv_obj_t* parent) {
    _screen = parent;

    // Full-screen clipped container — LVGL handles clipping of tile imgs
    // positioned outside the viewport, so we don't re-blit per frame.
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Tile container — in tab mode sits at (0,0) covering the full
    // 320×194 content area; in app mode shifts down by kAppHeaderH +
    // kAppMapPad to make room for the BACK pill strip. We don't enable
    // parent-level clipping here because the content parent itself
    // clips (UIManager's _lvContent is the boundary), and each tile img
    // renders normally outside its own bounds (LVGL clips at the
    // rendering stage).
    const int mapY = _appMode ? (kAppHeaderH + kAppMapPad) : 0;
    _mapContainer = lv_obj_create(parent);
    lv_obj_set_size(_mapContainer, VIEW_W, viewH());
    lv_obj_set_pos(_mapContainer, 0, mapY);
    lv_obj_set_style_bg_opa(_mapContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_mapContainer, 0, 0);
    lv_obj_set_style_pad_all(_mapContainer, 0, 0);
    lv_obj_clear_flag(_mapContainer, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Tile slots (placeholders + image widgets) ----
    for (int i = 0; i < SLOT_COUNT; ++i) {
        TileSlot& s = _slots[i];

        // Gray placeholder — shown until the tile is READY. We size it
        // exactly 256x256; positioning is done by rebuildTiles().
        s.bg = lv_obj_create(_mapContainer);
        lv_obj_set_size(s.bg, TILE_PX, TILE_PX);
        lv_obj_set_pos(s.bg, 0, 0);
        lv_obj_set_style_bg_color(s.bg, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(s.bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s.bg, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(s.bg, 1, 0);
        lv_obj_set_style_radius(s.bg, 0, 0);
        lv_obj_set_style_pad_all(s.bg, 0, 0);
        lv_obj_clear_flag(s.bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Tile image — sits exactly on top of the placeholder. When the
        // tile is READY, lv_img_set_src() points it at the TileCache's
        // pre-decoded lv_img_dsc_t; LVGL handles blitting. When no
        // tile is ready, the img is HIDDEN so the placeholder shows.
        s.img = lv_img_create(_mapContainer);
        lv_obj_set_size(s.img, TILE_PX, TILE_PX);
        lv_obj_set_pos(s.img, 0, 0);
        lv_img_set_antialias(s.img, false);
        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s.img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    // ---- HUD (drawn on top of tiles) ----
    // All four HUD labels sit at the corners of the MAP area (not the
    // content area) so they follow the map when the viewport expands in
    // app mode. Tab mode previously anchored them at content-relative y=2
    // (top) and y=174 (bottom) — those numbers were fine when mapY=0,
    // but in app mode the map sits at y=24..220 so the labels must
    // shift down by mapY (24) to remain on the map. Concretely:
    //   tab : hudTopY=2,   hudBotY=174 (kHudMapsetY/kHudGpsY constants)
    //   app : hudTopY=26,  hudBotY=200 (=mapY+viewH()-20)
    // The constants kHudZoomY/kHudFollowY/kHudMapsetY/kHudGpsY are kept
    // for tab-mode tests but no longer used at runtime; the formulas
    // below are the single source of truth.
    const int hudTopY = mapY + 2;
    const int hudBotY = mapY + viewH() - 20;
    _hudZoom = makeHudLabel(parent, kHudZoomX, hudTopY, 60, 14,
                            &lv_font_rsdeck_10, Theme::TEXT_PRIMARY, "z-");
    _hudMapset = makeHudLabel(parent, kHudMapsetX, hudBotY, 180, 14,
                              &lv_font_rsdeck_10, Theme::TEXT_MUTED, MAPSET_NAME);
    _hudGps = makeHudLabel(parent, kHudGpsX, hudBotY, 120, 14,
                           &lv_font_rsdeck_10, Theme::TEXT_MUTED, "GPS --");
    _hudFollow = makeHudLabel(parent, kHudFollowX, hudTopY, 70, 14,
                              &lv_font_rsdeck_10, Theme::TEXT_SECONDARY,
                              _followGPS ? "FOLLOW" : "MANUAL");

    // ---- Peer-on-map contact pin pool (rsDeck #64) ----
    // Created BEFORE nav buttons so pan/zoom controls stay above pins in
    // z-order (pins must never cover the D-pad). GPS marker is created
    // last so self stays on top of everything. Pool is reused across
    // refreshes: updateContactMarkers() hides all, then assigns slots.
    {
        static constexpr int kDiamondSize = 10;       // ~5px radius
        static constexpr int kBadgeW = 30;            // up to 4 chars + pad
        static constexpr int kBadgeH = 12;            // matches font height + pad
        static constexpr int kRibbonW = 28;           // 4 chars * ~6 + 2*2 pad
        static constexpr int kRibbonH = 12;
        for (int i = 0; i < MAX_CONTACT_MARKERS; ++i) {
            ContactPinWidget& cp = _contactPins[i];
            // Diamond: rounded square approximation (LVGL has no cheap
            // 45° rotate). 10x10 + radius 2 reads as a pin at this size.
            cp.diamond = lv_obj_create(parent);
            lv_obj_set_size(cp.diamond, kDiamondSize, kDiamondSize);
            lv_obj_set_pos(cp.diamond, -9999, -9999);  // off-screen until placed
            lv_obj_set_style_radius(cp.diamond, 2, 0);
            lv_obj_set_style_bg_color(cp.diamond, lv_color_hex(Theme::ACCENT), 0);
            lv_obj_set_style_bg_opa(cp.diamond, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cp.diamond, lv_color_hex(Theme::TEXT_PRIMARY), 0);
            lv_obj_set_style_border_width(cp.diamond, 1, 0);
            lv_obj_set_style_pad_all(cp.diamond, 0, 0);
            lv_obj_set_style_shadow_width(cp.diamond, 0, 0);
            lv_obj_add_flag(cp.diamond, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cp.diamond, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            cp.badge = lv_label_create(parent);
            lv_obj_set_size(cp.badge, kBadgeW, kBadgeH);
            lv_obj_set_pos(cp.badge, -9999, -9999);
            lv_obj_set_style_text_font(cp.badge, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(cp.badge, lv_color_hex(Theme::TEXT_PRIMARY), 0);
            lv_obj_set_style_bg_color(cp.badge, lv_color_hex(Theme::BG_SURFACE), 0);
            lv_obj_set_style_bg_opa(cp.badge, LV_OPA_70, 0);
            lv_obj_set_style_pad_left(cp.badge, 2, 0);
            lv_obj_set_style_pad_right(cp.badge, 2, 0);
            lv_obj_set_style_pad_top(cp.badge, 0, 0);
            lv_obj_set_style_pad_bottom(cp.badge, 0, 0);
            lv_obj_set_style_text_align(cp.badge, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_radius(cp.badge, 2, 0);
            lv_obj_set_style_border_width(cp.badge, 0, 0);
            lv_obj_add_flag(cp.badge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cp.badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            for (int r = 0; r < MAX_RIBBON_LABELS; ++r) {
                cp.ribbons[r] = lv_label_create(parent);
                lv_obj_t* rib = cp.ribbons[r];
                lv_obj_set_size(rib, kRibbonW, kRibbonH);
                lv_obj_set_pos(rib, -9999, -9999);
                lv_obj_set_style_text_font(rib, &lv_font_rsdeck_10, 0);
                lv_obj_set_style_text_color(rib, lv_color_hex(Theme::TEXT_PRIMARY), 0);
                lv_obj_set_style_bg_color(rib, lv_color_hex(Theme::BG_SURFACE), 0);
                lv_obj_set_style_bg_opa(rib, LV_OPA_70, 0);
                lv_obj_set_style_pad_left(rib, 2, 0);
                lv_obj_set_style_pad_right(rib, 2, 0);
                lv_obj_set_style_pad_top(rib, 0, 0);
                lv_obj_set_style_pad_bottom(rib, 0, 0);
                lv_obj_set_style_text_align(rib, LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_style_radius(rib, 2, 0);
                lv_obj_set_style_border_width(rib, 0, 0);
                lv_obj_add_flag(rib, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(rib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    // ---- Nav overlay (D-pad + zoom) ----
    // AFTER contact pins so controls stay discoverable above peer pins.
    // GPS marker is created last (below) so self still wins z-order.
    // The cluster sits in the top-right of the MAP area, offset by
    // mapY. kNavOriginY=22 was tuned for tab mode (mapY=0); in app mode
    // mapY=24 so the cluster shifts down to y=46 and stays just below
    // the BACK pill strip with a small gap.
    const int navOriginY = mapY + kNavOriginY;
    const int navRow0 = navOriginY;
    const int navRow1 = navOriginY + 1 * (kNavBtnH + kNavBtnGap);
    const int navRow2 = navOriginY + 2 * (kNavBtnH + kNavBtnGap);
    const int navRow3 = navOriginY + 3 * (kNavBtnH + kNavBtnGap);
    const int navRow4 = navOriginY + 4 * (kNavBtnH + kNavBtnGap);

    auto makeNavBtn = [&](int idx, int x, int y, const char* sym, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(parent);
        lv_obj_set_size(btn, kNavBtnW, kNavBtnH);
        lv_obj_set_pos(btn, x, y);
        lv_obj_add_style(btn, LvTheme::styleBtn(), 0);
        lv_obj_add_style(btn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::ACCENT), LV_STATE_PRESSED);
        lv_label_set_text(lbl, sym);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
        _navBtns[idx] = btn;
        _navBtnX1[idx] = x;
        _navBtnY1[idx] = y + Theme::STATUS_BAR_H;
        _navBtnX2[idx] = x + kNavBtnW - 1;
        _navBtnY2[idx] = y + kNavBtnH - 1 + Theme::STATUS_BAR_H;
        return btn;
    };
    // Sign convention: LEFT moves viewport center left (same as trackball).
    makeNavBtn(NAV_BTN_LEFT,  kNavColLeft,   navRow1, LV_SYMBOL_LEFT,  [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->panBy(-32, 0); self->rebuildTiles();
    });
    makeNavBtn(NAV_BTN_UP,    kNavColCenter, navRow0, LV_SYMBOL_UP,    [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->panBy(0, -32); self->rebuildTiles();
    });
    makeNavBtn(NAV_BTN_RIGHT, kNavColRight,  navRow1, LV_SYMBOL_RIGHT, [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->panBy( 32, 0); self->rebuildTiles();
    });
    makeNavBtn(NAV_BTN_DOWN,  kNavColCenter, navRow2, LV_SYMBOL_DOWN,  [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->panBy(0,  32); self->rebuildTiles();
    });
    makeNavBtn(NAV_BTN_ZIN,   kNavColCenter, navRow3, LV_SYMBOL_PLUS,  [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->zoomIn(); self->rebuildTiles();
    });
    makeNavBtn(NAV_BTN_ZOUT,  kNavColCenter, navRow4, LV_SYMBOL_MINUS, [](lv_event_t* e){
        auto* self = (LvMapScreen*)lv_event_get_user_data(e);
        self->zoomOut(); self->rebuildTiles();
    });

    // ---- GPS marker (separate overlay, NOT baked into the tile grid) ----
    // Created LAST so it sits on top of tiles, HUD, contact pins, and nav.
    _marker = lv_obj_create(parent);
    lv_obj_set_size(_marker, kMarkerSize, kMarkerSize);
    lv_obj_set_pos(_marker, 0, 0);
    lv_obj_set_style_radius(_marker, kMarkerSize / 2, 0);
    lv_obj_set_style_bg_color(_marker, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_bg_opa(_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_marker, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_obj_set_style_border_width(_marker, 1, 0);
    lv_obj_set_style_pad_all(_marker, 0, 0);
    lv_obj_set_style_shadow_width(_marker, 0, 0);
    lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_marker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ---- App-mode BACK pill (only when _appMode) ----
    // Top-left, just below the status bar. Mirrors the Pro MapScreen
    // "<- BACK" pill. Owns its CLICKED handler so a touch or Enter fires
    // _onBack directly; the manual touch handler in refreshUI() also
    // suppresses pan/double-tap when the touch-down lands on the pill
    // (via isTouchOnBack), so neither path can pan while the user is
    // trying to back out.
    if (_appMode) {
        _backBtn = lv_btn_create(parent);
        lv_obj_set_size(_backBtn, kBackBtnW, kBackBtnH);
        lv_obj_set_pos(_backBtn, kBackBtnX, kBackBtnY);
        lv_obj_add_style(_backBtn, LvTheme::styleBtn(), 0);
        lv_obj_add_style(_backBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_backBtn, LV_OPA_70, 0);
        lv_obj_set_style_border_color(_backBtn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(_backBtn, 1, 0);
        lv_obj_set_style_radius(_backBtn, 3, 0);
        lv_obj_set_style_pad_all(_backBtn, 0, 0);
        lv_obj_set_style_shadow_width(_backBtn, 0, 0);
        lv_obj_clear_flag(_backBtn, LV_OBJ_FLAG_SCROLLABLE);
        _backLbl = lv_label_create(_backBtn);
        lv_obj_set_style_text_font(_backLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(_backLbl, lv_color_hex(Theme::ACCENT), 0);
        lv_obj_set_style_text_color(_backLbl, lv_color_hex(Theme::PRIMARY), LV_STATE_PRESSED);
        lv_label_set_text(_backLbl, "< BACK");
        lv_obj_center(_backLbl);
        lv_obj_add_event_cb(_backBtn, [](lv_event_t* e) {
            auto* self = (LvMapScreen*)lv_event_get_user_data(e);
            if (self->_onBack) self->_onBack();
        }, LV_EVENT_CLICKED, this);

        // Cache screen-absolute bounds for the manual touch-handler bail
        // (refreshUI() reads raw touch coords and shouldn't treat a touch
        // on the BACK pill as a pan / double-tap on the map).
        _backBtnX1 = kBackBtnX;
        _backBtnY1 = kBackBtnY + Theme::STATUS_BAR_H;
        _backBtnX2 = kBackBtnX + kBackBtnW - 1;
        _backBtnY2 = kBackBtnY + kBackBtnH - 1 + Theme::STATUS_BAR_H;
    } else {
        _backBtn = nullptr;
        _backLbl = nullptr;
        _backBtnX1 = _backBtnY1 = _backBtnX2 = _backBtnY2 = 0;
    }

    // Initial layout. Note: onEnter() (called by UIManager right after
    // createUI() returns) is the canonical place for "first entry" work
    // (GPS auto-center, etc). The two rebuildTiles() calls here are for
    // the case where GPS is already wired but the screen's persistent
    // state doesn't yet have the GPS fix (rare race; harmless if
    // onEnter() does the same thing a few ms later).
    rebuildTiles();
    if (_gps && _gps->hasLocationFix() && !_everCenteredOnGps) {
        centerOnGpsIfAvailable();
        _everCenteredOnGps = true;
        _followGPS = true;
        rebuildTiles();
    }
    updateHud();
    requestVisibleTiles();
}

void LvMapScreen::destroyUI() {
    // UIManager will lv_obj_clean(_lvContent) immediately after this —
    // every child widget we created on `parent` is about to be deleted.
    // Just null out our local pointers so a stray refreshUI() between
    // now and the clean doesn't dereference a tombstone.
    _mapContainer = nullptr;
    _marker = nullptr;
    _hudZoom = nullptr;
    _hudMapset = nullptr;
    _hudGps = nullptr;
    _hudFollow = nullptr;
    _backBtn = nullptr;
    _backLbl = nullptr;
    _backBtnX1 = _backBtnY1 = _backBtnX2 = _backBtnY2 = 0;
    for (int i = 0; i < NAV_BTN_COUNT; ++i) {
        _navBtns[i] = nullptr;
    }
    for (int i = 0; i < SLOT_COUNT; ++i) {
        _slots[i].bg = nullptr;
        _slots[i].img = nullptr;
        _slots[i].curDsc = nullptr;
        _slots[i].curGen = 0;
        _slots[i].tx = INT32_MIN;
        _slots[i].ty = INT32_MIN;
        _slots[i].tz = -1;
    }
    for (int i = 0; i < MAX_CONTACT_MARKERS; ++i) {
        _contactPins[i].diamond = nullptr;
        _contactPins[i].badge = nullptr;
        for (int r = 0; r < MAX_RIBBON_LABELS; ++r) {
            _contactPins[i].ribbons[r] = nullptr;
        }
    }
    // No lv_img references our cache buffers any more — release the pins so
    // the pool is fully evictable while the map screen is closed.
    if (_tileCache) _tileCache->clearAllPins();
    LvScreen::destroyUI();
}

void LvMapScreen::onEnter() {
    _lastMarkerMs = 0;
    _lastTileRequestMs = 0;
    _lastHudRefreshMs = 0;
    _noTilesToastPendingMs = 0;
    _touchActive = false;
    _touchDoubleArmed = false;
    _touchConsumedByNav = false;

    // Remember which tab the user came from so Esc can put them back.
    if (_ui) _prevTab = _ui->lvTabBar().getActiveTab();

    // On first entry (the screen's persistent state still reflects the
    // initialized default, not any user-panned view), if a GPS fix is
    // available, arm follow-GPS AND center on it. This means the user
    // who has a working fix sees their location immediately instead of
    // the default demo view.
    //
    // On subsequent entries the user's pan/zoom is preserved: the
    // existing "if (_followGPS) centerOnGpsIfAvailable();" branch handles
    // re-centering for users who were already in follow mode.
    if (!_everCenteredOnGps && _gps && _gps->hasLocationFix()) {
        _followGPS = true;
        centerOnGpsIfAvailable();
        _everCenteredOnGps = true;
        MAP_LOG("[MAP] onEnter: GPS fix available, follow-GPS armed at z=%d (%lld, %lld)\n",
                _zoom, (long long)_centerWorldX, (long long)_centerWorldY);
    } else if (_followGPS) {
        centerOnGpsIfAvailable();
        MAP_LOG("[MAP] onEnter: re-centering on GPS (follow mode preserved)\n");
    } else {
        MAP_LOG("[MAP] onEnter: keeping prior view z=%d (%lld, %lld) follow=%d\n",
                _zoom, (long long)_centerWorldX, (long long)_centerWorldY, _followGPS);
    }
    rebuildTiles();
    updateHud();
    requestVisibleTiles();
}

void LvMapScreen::refreshUI() {
    if (!_mapContainer) return;

    unsigned long now = millis();

    // 1. Drain accumulated trackball deltas into pan offset. Trackball
    //    ISR fires per-tick; we sample + accumulate here and flush every
    //    PAN_FLUSH_MS so each flush is a discrete move rather than
    //    single-tick micro-pans.
    static int8_t accumX = 0;
    static int8_t accumY = 0;
    static unsigned long lastPanMs = 0;
    int8_t dx = trackball.lastDeltaX();
    int8_t dy = trackball.lastDeltaY();
    if (dx != 0 || dy != 0) {
        accumX += dx;
        accumY += dy;
        if (accumX > 16) accumX = 16;
        if (accumX < -16) accumX = -16;
        if (accumY > 16) accumY = 16;
        if (accumY < -16) accumY = -16;
    }
    if ((accumX != 0 || accumY != 0) && now - lastPanMs >= PAN_FLUSH_MS) {
        lastPanMs = now;
        int8_t ax = accumX; accumX = 0;
        int8_t ay = accumY; accumY = 0;
        panBy((int32_t)ax * PAN_SPEED, (int32_t)ay * PAN_SPEED);
        rebuildTiles();
    }

    // 2. Touch handling — single-point GT911, no pinch possible.
    //    Drag = pan (delta of touch.x/y while isTouched). Two taps
    //    within DOUBLE_TAP_MS / DOUBLE_TAP_RADIUS_PX = zoom in.
    //
    //    The nav overlay buttons consume their own touches via LVGL's
    //    CLICKED event. We also need to suppress the manual pan/double-tap
    //    logic for touches that landed on a nav button, otherwise a quick
    //    double-tap on a button could be misinterpreted as a map-screen
    //    double-tap and trigger an unwanted zoom-in.
    static bool wasTouched = false;
    static int16_t downX = 0, downY = 0;
    static unsigned long downMs = 0;
    static int16_t lastTouchX = 0, lastTouchY = 0;
    bool touched = touch.isTouched();
    if (touched) {
        int16_t tx = touch.x();
        int16_t ty = touch.y();
        if (!wasTouched) {
            // Touch-down event
            wasTouched = true;
            downX = tx;
            downY = ty;
            downMs = now;
            lastTouchX = tx;
            lastTouchY = ty;
            _touchActive = true;
            // Mark this touch as owned by the nav overlay if it landed
            // on a button. The LVGL CLICKED event will fire on release
            // (button handler does the actual pan/zoom); we just need to
            // keep our manual pan/double-tap logic out of the way for
            // the duration of this touch. App-mode BACK pill uses the
            // same suppression pattern (its CLICKED handler does the
            // back navigation).
            _touchConsumedByNav = isTouchOnNavButton(tx, ty) ||
                                  isTouchOnBack(tx, ty);
            if (_touchConsumedByNav) {
                // Skip double-tap arming so a quick tap on a button
                // doesn't count toward a future map-screen double-tap.
                return;
            }

            if (_touchDoubleArmed &&
                now - _touchDoubleMs <= DOUBLE_TAP_MS &&
                abs(tx - _touchDoubleX) <= DOUBLE_TAP_RADIUS_PX &&
                abs(ty - _touchDoubleY) <= DOUBLE_TAP_RADIUS_PX) {
                zoomIn();
                rebuildTiles();
                _touchDoubleArmed = false;  // consumed
            } else {
                _touchDoubleArmed = true;
                _touchDoubleX = tx;
                _touchDoubleY = ty;
                _touchDoubleMs = now;
            }
        } else {
            // Touch-move: pan based on delta from last position.
            // Skip if the touch-down landed on a nav button — even if
            // the user drags off the button mid-touch (LVGL cancels
            // the click event in that case), we don't want the pan
            // logic to kick in.
            if (_touchConsumedByNav) {
                lastTouchX = tx;
                lastTouchY = ty;
                return;
            }
            int16_t dxT = tx - lastTouchX;
            int16_t dyT = ty - lastTouchY;
            if (dxT != 0 || dyT != 0) {
                // Touch drags move the map OPPOSITE to the finger motion
                // (finger moves right = world content moves right = camera
                //  moves left in world px).
                panBy(-(int32_t)dxT, -(int32_t)dyT);
                rebuildTiles();
            }
            lastTouchX = tx;
            lastTouchY = ty;
        }
    } else if (wasTouched) {
        // Touch-up — disarm the double-tap if the touch was held too long
        // (so a long press doesn't get misinterpreted as two taps).
        wasTouched = false;
        _touchActive = false;
        _touchConsumedByNav = false;
        if (now - downMs > DOUBLE_TAP_MS) {
            _touchDoubleArmed = false;
        }
    }

    // 3. Marker + follow-GPS — throttled to 1Hz.
    if (now - _lastMarkerMs >= MARKER_REFRESH_MS) {
        _lastMarkerMs = now;
        bool didRecenter = false;
        if (_followGPS) {
            centerOnGpsIfAvailable();
            didRecenter = true;
        }
        updateMarker();
        if (didRecenter) rebuildTiles();
    }

    // 4. Tile re-arm — every 250ms. rebuildTiles() FIRST so tiles whose
    //    decode finished since the last pass actually get attached to
    //    their slot (requestVisibleTiles() only enqueues — without the
    //    rebuild the view stays on gray placeholders until the next
    //    pan/zoom/GPS recenter), then request whatever is still missing.
    if (now - _lastTileRequestMs >= TILE_REQUEST_INTERVAL_MS) {
        _lastTileRequestMs = now;
        rebuildTiles();
        requestVisibleTiles();
    }

    // 5. HUD refresh — every 500ms is plenty for these static labels.
    if (now - _lastHudRefreshMs >= HUD_REFRESH_MS) {
        _lastHudRefreshMs = now;
        updateHud();
    }

    // 6. "No tiles for this area" toast — fire once per view if the
    //    visible tile range has produced zero READY slots for >2.5s
    //    after the request was queued. This is a UX hint that the issue
    //    is data coverage (SD card has no tiles for this z/x/y) rather
    //    than a rendering bug. Reset on pan/zoom/recenter so a different
    //    area with tiles doesn't keep the toast suppressed.
    if (_noTilesToastPendingMs == 0) {
        _noTilesToastPendingMs = now;  // start the timer now
    } else if (!_noTilesToastShown && (now - _noTilesToastPendingMs) > 2500 &&
               _anySlotReadySinceRebuild == false) {
        // The TileCache has a built-in dumpStatus() that can show
        // pool/queue state for deeper inspection; this toast is the
        // user-facing summary.
        if (_ui) {
            _ui->lvStatusBar().showToast("No tiles for this area", 2500);
        }
        _noTilesToastShown = true;
        MAP_LOG("[MAP] NO TILES for view z=%d (%lld, %lld) after %lums — likely data coverage issue\n",
                _zoom, (long long)_centerWorldX, (long long)_centerWorldY,
                (unsigned long)(now - _noTilesToastPendingMs));
    }
}

bool LvMapScreen::handleKey(const KeyEvent& event) {
    // 'c' / 'C' — re-enable follow-GPS mode (snap to current fix).
    // Bare 'c' is unused across all screens (hotkey bindings are
    // Ctrl+letter, all in main.cpp's HotkeyManager — none use bare 'c'),
    // so this won't collide.
    if (event.character == 'c' || event.character == 'C') {
        _followGPS = true;
        centerOnGpsIfAvailable();
        rebuildTiles();
        updateHud();
        if (_ui) _ui->lvStatusBar().showToast("Following GPS", 800);
        return true;
    }

    // '+' / '=' — zoom in
    if (event.character == '+' || event.character == '=') {
        zoomIn();
        rebuildTiles();
        return true;
    }
    // '-' / '_' — zoom out
    if (event.character == '-' || event.character == '_') {
        zoomOut();
        rebuildTiles();
        return true;
    }

    // Arrow keys — pan in 32 px increments. Sign convention matches the
    // nav-overlay buttons above (see comment at the button handlers):
    // pressing LEFT makes the viewport center move LEFT (camera follows
    // key direction). The touch-drag handler in refreshUI() uses the
    // opposite convention and is intentionally different.
    if (event.up)    { panBy(0, -32); rebuildTiles(); return true; }
    if (event.down)  { panBy(0,  32); rebuildTiles(); return true; }
    if (event.left)  { panBy(-32, 0); rebuildTiles(); return true; }
    if (event.right) { panBy( 32, 0); rebuildTiles(); return true; }

    // Enter — zoom in (mirrors trackball click behavior)
    if (event.enter || event.character == '\n' || event.character == '\r') {
        zoomIn();
        rebuildTiles();
        return true;
    }

    // Esc / back — app mode fires the back callback (returns to Apps).
    // Tab mode falls through to the legacy _prevTab path so Esc / Del /
    // BS behaves identically to before the app-mode work.
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_appMode) {
            if (_onBack) _onBack();
            return true;
        }
        if (_ui) {
            _ui->lvTabBar().setActiveTab(_prevTab);
            // setActiveTab triggers _tabCb which routes via lvTabScreens
            // back to the appropriate screen — no manual setScreen call
            // needed, and it keeps the tab bar's visual state in sync.
        }
        return true;
    }

    // '/' / ',' — app mode also fires _onBack (the user is on the map
    // full-screen and shouldn't be able to advance the tab cycle from
    // here). Tab mode: legacy _prevTab restore.
    if (event.character == ',' || event.character == '/') {
        if (_appMode) {
            if (_onBack) _onBack();
        } else if (_ui) {
            _ui->lvTabBar().setActiveTab(_prevTab);
        }
        return true;
    }

    return false;
}

bool LvMapScreen::handleLongPress() {
    // Long-press on the map screen → zoom out (clamped). Returning true
    // prevents the global default in main.cpp from blanking the screen.
    zoomOut();
    rebuildTiles();
    return true;
}

// ---- Tile grid ----

void LvMapScreen::viewportOriginWorldPx(int64_t& outX, int64_t& outY) const {
    outX = _centerWorldX - viewHalfW();
    outY = _centerWorldY - viewHalfH();
}

void LvMapScreen::tileScreenPos(int32_t tx, int32_t ty,
                                int32_t& outX, int32_t& outY) const {
    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    outX = (int32_t)((int64_t)tx * (int64_t)TILE_PX - ox);
    outY = (int32_t)((int64_t)ty * (int64_t)TILE_PX - oy);
}

void LvMapScreen::rebuildTiles() {
    if (!_mapContainer) return;

    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    SlippyMath::TileXY tl = SlippyMath::worldPxToTile({ox, oy});

    // Visible tile range with buffer tiles on the right/bottom edges.
    //
    // GRID_COLS=3, GRID_ROWS=2 (6 slots). With 256-px tiles and a
    // 320x194 content area, the visible width is 1.25 tiles and the
    // visible height is 0.76 tiles. Grid covers columns tl.x .. tl.x+2,
    // which is the worst-case visible span for VIEW_W=320: at fractional
    // alignment up to 319 px can spill past the tile boundary at tl.x+1,
    // so the third column is the right-edge buffer. Anchoring on tl (drop
    // the -1) keeps the grid on the visible region plus a one-tile buffer
    // past the right/bottom edges — symmetric with the tyMin = tl.y
    // bottom-buffer behavior below.
    //
    // The previous formula (txMin = tl.x - 1) put the buffer column on the
    // LEFT instead of the right. With 256-px tiles and VIEW_W=320 the right
    // edge can show up to 64 px of the next column while the left edge was
    // off-screen by however many pixels the viewport origin was offset into
    // tl.x — same kind of "one edge is uncovered" bug that the old tyMin =
    // tl.y - 1 produced at the bottom (z=5 center=(0,0) → visible rows
    // y=-1 AND y=0, but the old grid only showed y=-2 and y=-1, leaving the
    // bottom 50% gray). Anchoring on tl covers the full visible span.
    //
    // For high zooms (e.g. z=15) the visible is 1-2 cols x 1 row; the grid
    // just has 1-2 buffers around that, same as before.
    int32_t txMin = tl.x;
    int32_t tyMin = tl.y;

    // At low zoom the world is smaller than the viewport AND smaller
    // than the 3x2 buffer grid. The buffer formula above yields
    // tile (tx,ty) keys that are outside the actual world — e.g. at
    // z=0 the world is just one tile (0,0), but txMin can be -1 and
    // tyMin can be 1, so we end up requesting tiles (-1,0), (1,0),
    // (-1,1), (0,1), (1,1) that CANNOT exist on the SD card. The
    // visible symptom was: the world tile (0,0) loaded and rendered
    // fine, but the rest of the viewport showed gray placeholders
    // (because the slots were "missing") — looking to the user like
    // "tiles should exist but don't load".
    //
    // We can't just clamp the grid down to a 1x1 at z=0 without losing
    // the buffer behavior at higher zooms, but we CAN:
    //   1. Skip out-of-world tiles entirely (no request, no probe).
    //   2. Hide the placeholder for those slots so they don't cover the
    //      viewport with confusing gray boxes.
    // This still places the in-world tiles correctly via the same buffer
    // formula; it just turns off the slots that fall outside the world.
    const int32_t worldTiles = (_zoom >= 0 && _zoom < 31) ? ((int32_t)1 << _zoom) : 1;
    auto isInWorld = [&](int32_t tx, int32_t ty) {
        return tx >= 0 && ty >= 0 && tx < worldTiles && ty < worldTiles;
    };

    // Reset the "saw any ready tile this rebuild" flag. Used by the
    // "no tiles for this area" toast in refreshUI() — fires if the
    // current view has no READY tiles for >2.5s after the request was
    // queued (likely data coverage issue, not a rendering bug).
    _anySlotReadySinceRebuild = false;

    // Drop last pass's pins; every in-world slot below re-pins its key so
    // TileCache can't LRU-evict (memset + re-decode) a PSRAM buffer that an
    // on-screen lv_img is still blitting. Pins on tiles that just scrolled
    // out of view are released here, which is what keeps the pool churning
    // normally while panning.
    if (_tileCache) _tileCache->clearAllPins();

#if LV_MAP_DEBUG
    int32_t visColMax = tl.x;
    int32_t visRowMax = tl.y;
    {
        int64_t cx = ox + VIEW_W;
        int64_t cy = oy + viewH();
        SlippyMath::TileXY br = SlippyMath::worldPxToTile({cx, cy});
        visColMax = br.x;
        visRowMax = br.y;
    }
#endif

    for (int row = 0; row < GRID_ROWS; ++row) {
        for (int col = 0; col < GRID_COLS; ++col) {
            int idx = row * GRID_COLS + col;
            TileSlot& s = _slots[idx];
            int32_t tx = txMin + col;
            int32_t ty = tyMin + row;

            // Low-zoom guard: tiles outside the world at this zoom are
            // physically impossible (the SD can never have a /x/y/z.png
            // for them). Hide the slot entirely so the user doesn't see
            // a confusing gray placeholder where the world doesn't even
            // extend to. requestVisibleTiles() also skips these, so the
            // negative cache stays clean.
            //
            // Defense-in-depth: in addition to setting the HIDDEN flag,
            // explicitly reposition both bg and img far off-screen (-9999,
            // -9999). If the HIDDEN flag were ever not honored (LVGL quirk,
            // race, etc.), the widget cannot accidentally cover a different
            // slot's on-screen position from a previous render — without
            // this reposition, an OOW slot would stay at its last in-world
            // (sx, sy), and a stale position could visually collide with
            // another slot's tile.
            if (!isInWorld(tx, ty)) {
                if (s.bg) {
                    if (!lv_obj_has_flag(s.bg, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(s.bg, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_obj_set_pos(s.bg, -9999, -9999);
                }
                if (s.img) {
                    if (!lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_obj_set_pos(s.img, -9999, -9999);
                }
                s.curDsc = nullptr;
                s.curGen = 0;
                // Keep tz == _zoom so requestVisibleTiles()'s "slot is being
                // repopulated this tick" check still works; the out-of-world
                // skip happens in requestVisibleTiles() via isInWorld().
                s.tx = tx;
                s.ty = ty;
                s.tz = _zoom;
                MAP_LOG("[MAP] slot %d z=%d x=%d y=%d -> OUT-OF-WORLD (hidden, parked off-screen)\n",
                        idx, _zoom, tx, ty);
                continue;
            }

            int32_t sx, sy;
            tileScreenPos(tx, ty, sx, sy);

            // Make sure bg/img are un-hidden when transitioning back
            // into the world (e.g. zoom-in from a low zoom where this
            // slot was hidden). The BG un-hides unconditionally; the
            // IMG stays hidden until the dsc block (below) attaches a
            // ready tile — that block clears HIDDEN when a new dsc is
            // set, and re-hides the IMG when no dsc is ready yet. We
            // intentionally do NOT un-hide the IMG here, because an
            // empty src (curDsc == nullptr) would show garbage from a
            // previous src.
            if (s.bg && lv_obj_has_flag(s.bg, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(s.bg, LV_OBJ_FLAG_HIDDEN);
            }
            if (s.img && lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN) &&
                s.curDsc == nullptr) {
                // curDsc is null → no dsc to show → IMG intentionally
                // stays hidden so we don't render a stale src. The dsc
                // block (below) will clear the flag when it attaches a
                // real dsc.
            }

            // Move bg + img to the new screen position. Tiles outside
            // the viewport get negative or >VIEW_W coords — LVGL's
            // content-area parent clips them.
            if (s.bg) lv_obj_set_pos(s.bg, sx, sy);
            if (s.img) lv_obj_set_pos(s.img, sx, sy);

            s.tx = tx;
            s.ty = ty;
            s.tz = _zoom;

            // Try to attach a ready tile. If not ready yet, the placeholder
            // stays visible. requestVisibleTiles() will queue a load.
            const lv_img_dsc_t* dsc = nullptr;
            uint32_t gen = 0;
            if (_tileCache) {
                dsc = _tileCache->getTileIfReady(MAPSET_NAME, _zoom, tx, ty, &gen);
                // Pin the key whether it's READY or still LOADING — this slot
                // is on screen, so its cache buffer must not be recycled out
                // from under LVGL, and pinning the in-flight decode also stops
                // request thrashing while panning.
                _tileCache->pinTile(MAPSET_NAME, _zoom, tx, ty);
            }
#if LV_MAP_DEBUG
            bool isInVisibleRange =
                (tx >= tl.x && tx <= visColMax && ty >= tl.y && ty <= visRowMax);
#endif
            if (dsc) {
                // Re-bind on ANY identity change, not just a pointer change.
                // TileCache hands out a permanent lv_img_dsc_t per cache slot,
                // so a recycled slot returns the SAME pointer with different
                // pixels (and possibly a different header w/h). The old
                // pointer-only check skipped set_src in that case, leaving
                // LVGL drawing stale geography with a stale header — the
                // "vertically squished z0 North America inside a z1 cell".
                if (s.img && (s.curDsc != dsc || s.curGen != gen)) {
                    // Drop any cached decoder entry keyed on this pointer so
                    // LVGL re-reads the header instead of reusing the old
                    // one. No-op when LV_IMG_CACHE_DEF_SIZE == 0 (current
                    // config), but correct if the cache is ever enabled.
                    lv_img_cache_invalidate_src(dsc);
                    lv_img_set_src(s.img, dsc);
                    s.curDsc = dsc;
                    s.curGen = gen;
                    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_invalidate(s.img);
                } else if (s.img && lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN)) {
                    // Same tile, same generation — already bound, just make
                    // sure it's visible (e.g. it was hidden while loading).
                    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                }
                _anySlotReadySinceRebuild = true;
                MAP_LOG("[MAP] slot %d z=%d x=%d y=%d -> READY gen=%lu visible=%s\n",
                        idx, _zoom, tx, ty, (unsigned long)gen,
                        isInVisibleRange ? "yes" : "no");
            } else {
                // No tile yet — hide the img so the placeholder shows.
                // We avoid lv_img_set_src(nullptr) here because LVGL
                // logs a warning on unknown src type; toggling HIDDEN
                // produces the same visual effect without the warning.
                if (s.img) {
                    if (!lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    }
                    s.curDsc = nullptr;
                    s.curGen = 0;
                }
                MAP_LOG("[MAP] slot %d z=%d x=%d y=%d -> placeholder (no dsc) visible=%s\n",
                        idx, _zoom, tx, ty,
                        isInVisibleRange ? "yes" : "no");
            }
        }
    }
}

void LvMapScreen::requestVisibleTiles() {
    if (!_tileCache) return;
    // Mirror rebuildTiles()'s world-bounds check so we don't fill the
    // negative cache with bogus out-of-world (z,x,y) keys at low zoom.
    const int32_t worldTiles = (_zoom >= 0 && _zoom < 31) ? ((int32_t)1 << _zoom) : 1;
    int requested = 0;
    int deduped = 0;
    int skippedOOW = 0;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        TileSlot& s = _slots[i];
        if (s.tz != _zoom) continue;  // slot is being repopulated this tick
        if (s.tx < 0 || s.ty < 0 || s.tx >= worldTiles || s.ty >= worldTiles) {
            ++skippedOOW;
            continue;  // out-of-world: never exists on SD
        }
        // requestTile() returns false only if the request was deduped OR
        // the (z,x,y) is in the negative cache. Both are "no work needed",
        // but they're different from "queue full" so we don't fail loudly.
        if (_tileCache->requestTile(MAPSET_NAME, s.tz, s.tx, s.ty,
                                    TileCache::Priority::PRIO_NORMAL)) {
            ++requested;
        } else {
            ++deduped;
        }
    }
    MAP_LOG("[MAP] requestVisibleTiles z=%d requested=%d deduped_or_neg=%d out_of_world=%d\n",
            _zoom, requested, deduped, skippedOOW);
}

// ---- Nav overlay hit test ----

bool LvMapScreen::isTouchOnNavButton(int16_t tx, int16_t ty) const {
    // Bounds are populated once in createUI() and are screen-absolute.
    // No allocation, no LVGL calls — safe to invoke from refreshUI()'s
    // touch path on every iteration.
    for (int i = 0; i < NAV_BTN_COUNT; ++i) {
        if (!_navBtns[i]) continue;
        if (tx >= _navBtnX1[i] && tx <= _navBtnX2[i] &&
            ty >= _navBtnY1[i] && ty <= _navBtnY2[i]) {
            return true;
        }
    }
    return false;
}

bool LvMapScreen::isTouchOnBack(int16_t tx, int16_t ty) const {
    // _backBtn is null in tab mode (no pill); bounds are zero-initialized
    // so the rect test is trivially false. Cheap enough to call every tick.
    if (!_backBtn) return false;
    return tx >= _backBtnX1 && tx <= _backBtnX2 &&
           ty >= _backBtnY1 && ty <= _backBtnY2;
}

void LvMapScreen::rebuildNavOverlay() {
    // Currently a no-op — button layout is fixed and doesn't depend on
    // zoom/pan/theme. Kept for future use (e.g. hiding the overlay in
    // a follow-GPS-on-wide-view mode, or repositioning when an overlay
    // toast is visible).
}

// ---- GPS marker / follow ----

void LvMapScreen::centerOnGpsIfAvailable() {
    if (!_gps || !_gps->hasLocationFix()) return;
    SlippyMath::WorldPx wp = SlippyMath::lonLatToWorldPx(
        _gps->longitude(), _gps->latitude(), _zoom);
    _centerWorldX = wp.x;
    _centerWorldY = wp.y;
    clampCenterToWorld();
    // A new GPS-centered view gets its own 2.5s grace period before
    // the "no tiles" toast can fire.
    _noTilesToastPendingMs = 0;
    _noTilesToastShown = false;
}

void LvMapScreen::updateMarker() {
    if (!_marker) return;

    if (!_gps || !_gps->hasLocationFix()) {
        lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
        if (_hudGps) lv_label_set_text(_hudGps, "GPS no fix");
        return;
    }

    // Project current GPS lat/lon to screen-space at the current zoom.
    // IMPORTANT: always use the same origin as contact pins (viewportOrigin).
    // Do NOT force the self marker to screen-center when follow is on —
    // clampCenterToWorld() can shift the center away from true GPS at low
    // zoom; pinning self to center while contacts use true projection made
    // peer pins look NE/SE-flipped relative to "me" below ~z6.
    SlippyMath::WorldPx wp = SlippyMath::lonLatToWorldPx(
        _gps->longitude(), _gps->latitude(), _zoom);
    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    int sx = (int)(wp.x - ox) - kMarkerSize / 2;
    int sy = (int)(wp.y - oy) - kMarkerSize / 2;

    // Clip the marker to the content area. If the marker would be drawn
    // outside the viewport, hide it — there's no value in showing a dot
    // that's not where the user expects it.
    if (sx < -kMarkerSize || sx >= VIEW_W ||
        sy < -kMarkerSize || sy >= viewH()) {
        lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(_marker, LV_OBJ_FLAG_HIDDEN);
        // _marker is parented to the content area, not to _mapContainer,
        // so we have to add mapOriginY() to translate the map-local
        // projection (sx, sy ∈ [0..VIEW_W] × [0..viewH()]) into a
        // content-area coord. Tab mode: mapOriginY()=0 → unchanged.
        // App mode: mapOriginY()=24 → marker shifts down with the map.
        lv_obj_set_pos(_marker, sx, sy + mapOriginY());
    }

    if (_hudGps) {
        char buf[24];
        uint8_t fq = _gps->fixQuality();
        const char* mode = (fq >= 1) ? "3D" : "2D";
        snprintf(buf, sizeof(buf), "GPS %s %dsat", mode, _gps->satellites());
        lv_label_set_text(_hudGps, buf);
    }

    // Peer-on-map (rsDeck #64): re-project contact pins against the
    // current viewport. Same 1Hz cadence as the self marker so a moving
    // map (panning, follow-GPS) keeps pins glued to their geography.
    updateContactMarkers();
}

// ---- Peer-on-map (rsDeck #64) ----

// Short label for a contact. Display name preferred, else first 4 chars of
// the dest hex. Result is NUL-terminated in out[0..3] (out[4]); out[5] is
// the buffer size to keep callers from overrunning.
void LvMapScreen::shortLabel(const DiscoveredNode& n, char out[5]) {
    out[0] = out[1] = out[2] = out[3] = out[4] = 0;
    const char* src = nullptr;
    if (!n.name.empty()) {
        src = n.name.c_str();
    } else {
        // Fallback to hex hash prefix (toHex is expensive — but we only
        // call this for visible contacts and the pool is bounded to 16).
        std::string hex = n.hash.toHex();
        src = hex.c_str();
    }
    if (!src || !src[0]) {
        out[0] = '?';
        return;
    }
    size_t i = 0;
    for (; i < 4 && src[i] && src[i] != ' ' && src[i] != '\t'; i++) {
        out[i] = (char)toupper((unsigned char)src[i]);
    }
    // If the source was empty after skipping whitespace, mark as unknown.
    if (i == 0) out[0] = '?';
}

void LvMapScreen::updateContactMarkers() {
    // Hide the entire pool first — widgets assigned this refresh show
    // again below; widgets NOT assigned stay hidden so old pins don't
    // linger after a contact's location is cleared.
    for (int i = 0; i < MAX_CONTACT_MARKERS; ++i) {
        ContactPinWidget& cp = _contactPins[i];
        if (cp.diamond) lv_obj_add_flag(cp.diamond, LV_OBJ_FLAG_HIDDEN);
        if (cp.badge)   lv_obj_add_flag(cp.badge,   LV_OBJ_FLAG_HIDDEN);
        for (int r = 0; r < MAX_RIBBON_LABELS; ++r) {
            if (cp.ribbons[r]) lv_obj_add_flag(cp.ribbons[r], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!_am) return;
    if (!_contactPins[0].diamond) return;  // createUI() not yet run

    // Project every saved+located contact to screen-space at the current
    // zoom. Skip if the projection falls outside the viewport — Pro
    // MapScreen does the same and avoids drawing pins the user can't see.
    struct Pin {
        int16_t sx, sy;
        const DiscoveredNode* n;
        bool used = false;
    };
    Pin pins[MAX_CONTACT_MARKERS];
    int nPins = 0;
    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    const auto& nodes = _am->nodes();
    for (const auto& n : nodes) {
        if (!n.saved || !n.hasLocation) continue;
        if (nPins >= MAX_CONTACT_MARKERS) break;
        SlippyMath::WorldPx wp = SlippyMath::lonLatToWorldPx(n.lon, n.lat, _zoom);
        int sx = (int)(wp.x - ox);
        int sy = (int)(wp.y - oy);
        if (sx < 0 || sx >= VIEW_W || sy < 0 || sy >= viewH()) continue;
        pins[nPins].sx = (int16_t)sx;
        pins[nPins].sy = (int16_t)sy;
        pins[nPins].n = &n;
        pins[nPins].used = false;
        nPins++;
    }
    if (nPins == 0) return;

    // Self screen pos (for the "nudge if stack sits on self" rule). The
    // self marker can be hidden if GPS has no fix — that's fine, we just
    // skip the nudge in that case. Computed BEFORE the diagnostic log
    // below so the log can include both pins' positions side by side.
    int16_t selfSx = -9999, selfSy = -9999;
    bool selfOnMap = false;
    if (_gps && _gps->hasLocationFix()) {
        SlippyMath::WorldPx swp = SlippyMath::lonLatToWorldPx(
            _gps->longitude(), _gps->latitude(), _zoom);
        int sxs = (int)(swp.x - ox);
        int sys = (int)(swp.y - oy);
        if (sxs >= 0 && sxs < VIEW_W && sys >= 0 && sys < viewH()) {
            selfSx = (int16_t)sxs;
            selfSy = (int16_t)sys;
            selfOnMap = true;
        }
    }

    // ---- Diagnostic: contact pin projection once per zoom change ----
    // The user reported "contact pin looks in SC at z1-5, correct at z6+"
    // with the self pin OK. Both pins share the same lonLatToWorldPx()
    // math and the same viewportOriginWorldPx() so the only way one can
    // be wrong and the other right is if the *inputs* differ. This log
    // emits tile x/y + screen sx/sy (no raw lat/lon) for the first
    // contact pin and (when available) the self pin so we can correlate
    // the projection against the basemap tile's geographic content at
    // that screen position. Fires at most once per zoom change. Kept
    // out of LV_MAP_DEBUG gating so it runs in production builds by
    // default — the per-zoom cadence keeps the serial line quiet.
    if (_zoom != _contactPinLogZoom) {
        _contactPinLogZoom = _zoom;
        const Pin& p0 = pins[0];
        int64_t wx = (int64_t)p0.sx + ox;
        int64_t wy = (int64_t)p0.sy + oy;
        int32_t tx = (int32_t)(wx / TILE_PX);
        int32_t ty = (int32_t)(wy / TILE_PX);
        if (selfOnMap) {
            Serial.printf("[map] pin z=%d contact sx=%d sy=%d tx=%d ty=%d self sx=%d sy=%d ox=%lld oy=%lld cwx=%lld cwy=%lld nPins=%d\n",
                          _zoom, (int)p0.sx, (int)p0.sy, (int)tx, (int)ty,
                          (int)selfSx, (int)selfSy,
                          (long long)ox, (long long)oy,
                          (long long)_centerWorldX, (long long)_centerWorldY,
                          nPins);
        } else {
            Serial.printf("[map] pin z=%d contact sx=%d sy=%d tx=%d ty=%d self=(no fix) ox=%lld oy=%lld cwx=%lld cwy=%lld nPins=%d\n",
                          _zoom, (int)p0.sx, (int)p0.sy, (int)tx, (int)ty,
                          (long long)ox, (long long)oy,
                          (long long)_centerWorldX, (long long)_centerWorldY,
                          nPins);
        }
    }

    int poolIdx = 0;
    for (int i = 0; i < nPins && poolIdx < MAX_CONTACT_MARKERS; ++i) {
        if (pins[i].used) continue;

        // Build a cluster around pin i — all un-used pins within STACK_PX
        // (squared distance test) share one diamond + label stack.
        int members[MAX_CONTACT_MARKERS];
        int nMem = 0;
        int32_t sumX = 0, sumY = 0;
        for (int j = i; j < nPins; ++j) {
            if (pins[j].used) continue;
            int dx = (int)pins[j].sx - (int)pins[i].sx;
            int dy = (int)pins[j].sy - (int)pins[i].sy;
            if (dx * dx + dy * dy > STACK_PX * STACK_PX) continue;
            pins[j].used = true;
            members[nMem++] = j;
            sumX += pins[j].sx;
            sumY += pins[j].sy;
        }
        if (nMem == 0) continue;

        const int16_t cx = (int16_t)(sumX / nMem);
        const int16_t cy = (int16_t)(sumY / nMem);

        // Nudge the diamond if the cluster sits on top of the self
        // marker so the green dot stays readable — but ALWAYS along the
        // true bearing from self → contact. A previous hard-coded
        // (+8,-8) push forced every near contact to screen-NE of self,
        // which at z1–z6 (metro pairs collapse inside STACK_PX≈14) made
        // a true-SE peer look NE until zoom separated them past 14px.
        int16_t pinX = cx, pinY = cy;
        bool onSelf = false;
        if (selfOnMap) {
            int dx = (int)cx - (int)selfSx;
            int dy = (int)cy - (int)selfSy;
            if (dx * dx + dy * dy <= STACK_PX * STACK_PX) {
                onSelf = true;
                // Unit vector along true offset; if coincident, fall back
                // to SE (+x,+y) which matches "south/east of me" default.
                float len = sqrtf((float)(dx * dx + dy * dy));
                float ux, uy;
                if (len < 0.5f) {
                    ux = 0.7071f;
                    uy = 0.7071f;
                } else {
                    ux = (float)dx / len;
                    uy = (float)dy / len;
                }
                const float NUDGE = 10.0f;
                pinX = (int16_t)lroundf((float)selfSx + ux * NUDGE);
                pinY = (int16_t)lroundf((float)selfSy + uy * NUDGE);
                // Clip to content; if we hit an edge, flip that axis only.
                if (pinX < 6)              pinX = (int16_t)(selfSx + (int)lroundf(fabsf(ux) * NUDGE));
                if (pinX >= VIEW_W - 6)    pinX = (int16_t)(selfSx - (int)lroundf(fabsf(ux) * NUDGE));
                if (pinY < 6)              pinY = (int16_t)(selfSy + (int)lroundf(fabsf(uy) * NUDGE));
                if (pinY >= viewH() - 6)   pinY = (int16_t)(selfSy - (int)lroundf(fabsf(uy) * NUDGE));
            }
        }

        ContactPinWidget& cp = _contactPins[poolIdx++];

        // Diamond is centered on (pinX, pinY). The widget origin is
        // top-left so we offset by half the diamond size (5 px).
        // Same +mapOriginY() pattern as _marker: contact-pin widgets
        // live on the content parent so the projection's map-local Y
        // needs the content-area offset.
        if (cp.diamond) {
            lv_obj_clear_flag(cp.diamond, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cp.diamond, pinX - 5, pinY - 5 + mapOriginY());
        }

        // Hide leftover ribbon labels from a previous refresh in case
        // this same pool slot is reused with a smaller cluster — the
        // earlier "hide all" pass already did this once, but doing it
        // again here per-cluster is cheap insurance.
        for (int r = 0; r < MAX_RIBBON_LABELS; ++r) {
            if (cp.ribbons[r]) lv_obj_add_flag(cp.ribbons[r], LV_OBJ_FLAG_HIDDEN);
        }

        if (nMem == 1) {
            // Single-member cluster: 4-char label next to the diamond.
            char lab[5];
            shortLabel(*pins[members[0]].n, lab);
            if (cp.badge) {
                lv_obj_clear_flag(cp.badge, LV_OBJ_FLAG_HIDDEN);
                // Place to the right of the diamond; flip left if it
                // would clip. Same convention as Pro MapScreen.
                int16_t bx = (int16_t)(pinX + 5 + 2);   // diamond half + 2 px gap
                int16_t by = (int16_t)(pinY - 6);       // vertically centered
                if (bx + 30 > VIEW_W) bx = (int16_t)(pinX - 5 - 2 - 30);
                if (bx < 0) bx = 0;
                if (by < 0) by = 0;
                if (by + 12 > viewH()) by = (int16_t)(viewH() - 12);
                // +mapOriginY(): badge widgets live on the content parent
                // (not on _mapContainer) so we translate map-local by to
                // content-area by. Tab mode adds 0 (unchanged).
                lv_obj_set_pos(cp.badge, bx, by + mapOriginY());
                lv_label_set_text(cp.badge, lab);
            }
        } else {
            // Multi-member cluster: count badge above the diamond +
            // ribbon stack of labels to the side opposite self.
            char badge[4];
            if (nMem > 9) snprintf(badge, sizeof(badge), "9+");
            else snprintf(badge, sizeof(badge), "%d", nMem);
            if (cp.badge) {
                lv_obj_clear_flag(cp.badge, LV_OBJ_FLAG_HIDDEN);
                int16_t bx = (int16_t)(pinX - 15);
                int16_t by = (int16_t)(pinY - 5 - 12 - 1);  // above diamond
                if (by < 0) by = (int16_t)(pinY + 5 + 1);   // flip below
                if (bx < 0) bx = 0;
                if (bx + 30 > VIEW_W) bx = (int16_t)(VIEW_W - 30);
                lv_obj_set_pos(cp.badge, bx, by + mapOriginY());
                lv_label_set_text(cp.badge, badge);
            }

            const int show = nMem < MAX_RIBBON_LABELS ? nMem : MAX_RIBBON_LABELS;
            // Ribbons prefer the LEFT side if the pin sits in the right
            // half of the screen, OR if the cluster was nudged because it
            // sits on self (the diagonal nudge keeps the diamond on the
            // upper-left of self, so left-side ribbons don't collide).
            bool ribbonsLeft = onSelf ? true : (pinX > VIEW_W / 2);
            int16_t ribbonX = ribbonsLeft
                ? (int16_t)(pinX - 5 - 2 - 28)   // left of diamond
                : (int16_t)(pinX + 5 + 2);       // right of diamond
            int16_t ribbonY0 = (int16_t)(pinY - (show * 12) / 2);
            if (ribbonY0 < 0) ribbonY0 = 0;

            for (int k = 0; k < show; ++k) {
                if (!cp.ribbons[k]) continue;
                char lab[5];
                shortLabel(*pins[members[k]].n, lab);
                lv_obj_clear_flag(cp.ribbons[k], LV_OBJ_FLAG_HIDDEN);
                int16_t by = (int16_t)(ribbonY0 + k * 12);
                if (by + 12 > viewH()) break;
                // Flip to right side if left would clip the edge.
                int16_t bx = ribbonX;
                if (bx < 0) bx = (int16_t)(pinX + 5 + 2);
                if (bx + 28 > VIEW_W) bx = (int16_t)(VIEW_W - 28);
                lv_obj_set_pos(cp.ribbons[k], bx, by + mapOriginY());
                lv_label_set_text(cp.ribbons[k], lab);
            }
            // "+N" overflow — only if there are MORE members than the
            // ribbon stack can show. Stacks under the ribbonY0+show*12
            // line; clipped if it would fall off the bottom.
            if (nMem > show) {
                char more[8];
                snprintf(more, sizeof(more), "+%d", nMem - show);
                int16_t by = (int16_t)(ribbonY0 + show * 12);
                if (by + 12 <= viewH() && cp.ribbons[show]) {
                    lv_obj_clear_flag(cp.ribbons[show], LV_OBJ_FLAG_HIDDEN);
                    int16_t bx = ribbonX;
                    if (bx < 0) bx = (int16_t)(pinX + 5 + 2);
                    if (bx + 28 > VIEW_W) bx = (int16_t)(VIEW_W - 28);
                    lv_obj_set_pos(cp.ribbons[show], bx, by + mapOriginY());
                    lv_label_set_text(cp.ribbons[show], more);
                }
            }
        }
    }
}

// ---- HUD ----

void LvMapScreen::updateHud() {
    if (_hudZoom) {
        char buf[8];
        snprintf(buf, sizeof(buf), "z%d", _zoom);
        lv_label_set_text(_hudZoom, buf);
    }
    if (_hudFollow) {
        lv_label_set_text(_hudFollow, _followGPS ? "FOLLOW" : "MANUAL");
        lv_obj_set_style_text_color(_hudFollow,
            lv_color_hex(_followGPS ? Theme::SUCCESS : Theme::TEXT_SECONDARY), 0);
    }
    // _hudMapset is static (hardcoded for v1) — no update needed.
    // _hudGps is updated in updateMarker(); don't overwrite here.
}

// ---- Pan / zoom primitives ----

void LvMapScreen::panBy(int32_t dxPx, int32_t dyPx) {
    if (dxPx == 0 && dyPx == 0) return;
    _centerWorldX += dxPx;
    _centerWorldY += dyPx;
    clampCenterToWorld();
    // Any manual pan disables follow-GPS. (The 'c' key re-arms it.)
    _followGPS = false;
    // Reset the "no tiles" toast timer so a new view gets its own
    // 2.5s grace period before the toast can fire.
    _noTilesToastPendingMs = 0;
    _noTilesToastShown = false;
}

void LvMapScreen::clampCenterToWorld() {
    // World is worldPx = TILE_PX * 2^zoom pixels square.
    //
    // When follow-GPS is on, do NOT pull the center away from the true GPS
    // world-px just to keep the viewport inside the world. That clamp was
    // shifting the basemap under a correctly-projected self pin and made
    // contact pins look directionally wrong (NE vs SE) at low zoom while
    // z6+ (no clamp bite) looked fine. Allow empty margins past world edges
    // instead; out-of-world tile slots are already hidden in rebuildTiles().
    //
    // Manual pan still clamps so the user can't lose the map entirely.
    const int64_t worldPx = (int64_t)TILE_PX << _zoom;

    if (_followGPS) {
        // Soft clamp: keep center inside the world square only (not
        // viewport-inset). Pins and tiles stay geographically consistent.
        if (_centerWorldX < 0) _centerWorldX = 0;
        if (_centerWorldY < 0) _centerWorldY = 0;
        if (_centerWorldX > worldPx) _centerWorldX = worldPx;
        if (_centerWorldY > worldPx) _centerWorldY = worldPx;
        return;
    }

    if (worldPx > VIEW_W) {
        if (_centerWorldX < viewHalfW())           _centerWorldX = viewHalfW();
        if (_centerWorldX > worldPx - viewHalfW()) _centerWorldX = worldPx - viewHalfW();
    } else {
        _centerWorldX = worldPx / 2;
    }

    if (worldPx > viewH()) {
        if (_centerWorldY < viewHalfH())           _centerWorldY = viewHalfH();
        if (_centerWorldY > worldPx - viewHalfH()) _centerWorldY = worldPx - viewHalfH();
    } else {
        _centerWorldY = worldPx / 2;
    }
}

void LvMapScreen::clampZoom() {
    if (_zoom < ZOOM_MIN) _zoom = ZOOM_MIN;
    if (_zoom > ZOOM_MAX) _zoom = ZOOM_MAX;
}

void LvMapScreen::zoomIn() {
    int prevZoom = _zoom;
    _zoom++;
    clampZoom();
    if (_zoom == prevZoom) return;  // already at max

    // Scale the center so the viewport center stays put. This is the
    // common slippy-map feel for keyboard/button zoom (no anchor cursor
    // available — map screen doesn't use a focus group).
    if (prevZoom >= 0 && _zoom > prevZoom) {
        double ratio = (double)(1ULL << _zoom) / (double)(1ULL << prevZoom);
        _centerWorldX = (int64_t)((double)_centerWorldX * ratio);
        _centerWorldY = (int64_t)((double)_centerWorldY * ratio);
    }
    clampCenterToWorld();

    // Zoom does NOT clear follow-GPS — manual pans still do (see panBy()).
    // If we were following, snap the view back to the current fix so the
    // marker stays centered after the zoom's world-px rescale. Without
    // this, the marker drifted to the corner of the viewport at low zoom
    // (clamps forced the center to the world midpoint while the marker
    // kept projecting GPS lat/lon), making the self-pin look like it was
    // in the wrong state.
    if (_followGPS && _gps && _gps->hasLocationFix()) {
        centerOnGpsIfAvailable();
    }
    _noTilesToastPendingMs = 0;
    _noTilesToastShown = false;
}

void LvMapScreen::zoomOut() {
    int prevZoom = _zoom;
    _zoom--;
    clampZoom();
    if (_zoom == prevZoom) return;

    if (prevZoom > 0 && _zoom < prevZoom) {
        double ratio = (double)(1ULL << _zoom) / (double)(1ULL << prevZoom);
        _centerWorldX = (int64_t)((double)_centerWorldX * ratio);
        _centerWorldY = (int64_t)((double)_centerWorldY * ratio);
    }
    clampCenterToWorld();

    // Zoom does NOT clear follow-GPS — manual pans still do (see panBy()).
    // If we were following, snap the view back to the current fix so the
    // marker stays centered after the zoom's world-px rescale. See
    // zoomIn() for the rationale.
    if (_followGPS && _gps && _gps->hasLocationFix()) {
        centerOnGpsIfAvailable();
    }
    _noTilesToastPendingMs = 0;
    _noTilesToastShown = false;
}