#pragma once

#include <climits>
#include <cstdint>
#include <functional>
#include "ui/Theme.h"
#include "ui/UIManager.h"

class TileCache;
class GPSManager;
class AnnounceManager;

// LvMapScreen — full pan/zoom offline slippy map.
//
// Consumes TileCache (lazy PSRAM-backed tile store) for XYZ slippy tiles.
// State held in just three things:
//   * _zoom         — current XYZ zoom level (clamped to MAPSET_ZOOM_MIN..MAX)
//   * _centerWorldPx — viewport center in Web Mercator world pixels
//   * _followGPS    — true: re-center on each fresh GPS fix; any manual
//                    pan/zoom clears it
//
// Two display modes:
//
//   * Tab mode (_appMode=false, default):
//       Layout: 320×194 content area (Theme::CONTENT_H). Tab bar visible.
//       No BACK control — Esc / ',' / '/' returns to the previous tab
//       captured by _prevTab.
//
//   * App mode (_appMode=true, set before opening from the Apps tile):
//       Tab bar hidden → content area expands to 320×220 (full screen
//       minus status bar). A BACK pill appears top-left so the user has a
//       discoverable way back to Apps (Esc / Del / BS still work as a
//       keyboard fallback). Map viewport shifts down to fill the area
//       below the BACK strip — no empty bottom strip.
//
// Layout (each mode's per-mode constants at the top of the .cpp):
//   * Tab mode: 3×2 tile grid covering CONTENT_W × CONTENT_H.
//   * App mode: 3×2 tile grid covering CONTENT_W × (CONTENT_W-(header))
//     positioned at (0, kAppHeaderH + kAppMapPad) in content-relative
//     coordinates, below the BACK strip.
//
// Inputs:
//   * Trackball move  → pan (deltas * PAN_SPEED into _centerWorldPx)
//   * Trackball click → zoom in (clamped)
//   * Long-press      → zoom out (clamped) — returns true from
//                       handleLongPress() so the global screen-blank
//                       default does NOT fire
//   * Touch drag      → pan, trackball-equivalent math
//   * Double-tap      → zoom in
//   * 'c' / 'C'       → re-arm follow-GPS (snap to current fix)
//   * Esc / Del / BS  → app mode: invoke _onBack (returns to Apps).
//                       Tab mode: return to _prevTab via the tab callback.
//   * ',' / '/'       → app mode: invoke _onBack. Tab mode: return to
//                       _prevTab (don't accidentally advance the tab
//                       cycle while the user is on the map).
//
// The screen costs ~0 bytes of LVGL widgets when not visible — all UI is
// built in createUI() and torn down by UIManager's lv_obj_clean() when
// the user navigates away. TileCache's own PSRAM pool is also allocated
// lazily on its first pump() invocation, so the map screen costs only
// ~2 KB of LVGL metadata until the user actually opens it.
class LvMapScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;

    void setTileCache(TileCache* tc) { _tileCache = tc; }
    void setGPSManager(GPSManager* gps) { _gps = gps; }
    void setUIManager(class UIManager* mgr) { _ui = mgr; }
    // Peer-on-map (rsDeck #64): announceManager is read-only here — the
    // map screen consumes saved contacts that have lat/lon and renders
    // diamond pins. We never mutate the manager.
    void setAnnounceManager(AnnounceManager* am) { _am = am; }

    // App-mode display. When true:
    //   * Tab bar is hidden (caller drives this via UIManager)
    //   * createUI() shows a BACK pill top-left
    //   * Map viewport expands to fill the content area minus the BACK
    //     strip (no empty bottom strip from the hidden tab bar)
    //   * Esc / Del / BS / ',' / '/' route through _onBack instead of
    //     the tab-callback path
    // Must be set BEFORE setScreen() so createUI() picks it up. Defaults
    // to false (legacy tab-mode display).
    void setAppMode(bool appMode) { _appMode = appMode; }
    bool appMode() const { return _appMode; }
    // Back navigation — wired only for app-mode (where the BACK pill
    // needs a callback). In tab mode the tab-callback handles back.
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Map"; }

private:
    // ---- Layout constants ----
    // 3 cols × 2 rows = 6 tile slots. Buffer tile on each edge covers
    // panning near tile boundaries; LVGL parent clipping hides the rest.
    static constexpr int GRID_COLS = 3;
    static constexpr int GRID_ROWS = 2;
    static constexpr int SLOT_COUNT = GRID_COLS * GRID_ROWS;
    static constexpr int TILE_PX = 256;
    static constexpr int VIEW_W = 320;       // Theme::CONTENT_W (unchanged across modes)

    // App-mode chrome dimensions — referenced in app-mode code paths only.
    // Sized to mirror the Pro reference (kBackH=22 + kBackPad=2 = 24-px
    // top strip). The BACK widget itself is kAppHeaderH-4=18 px tall,
    // centered vertically inside the 22-px strip with 2 px pads.
    static constexpr int kAppHeaderH = 22;
    static constexpr int kAppMapPad  = 2;
    static constexpr int kBackBtnX   = 4;
    static constexpr int kBackBtnY   = 2;
    static constexpr int kBackBtnW   = 64;
    static constexpr int kBackBtnH   = kAppHeaderH - 4;   // 18

    // VIEW_W stays 320 across modes. VIEW_H / VIEW_HALF_H are now
    // runtime-dispatched: tab mode keeps the legacy 194/97; app mode
    // uses the full content area (220) minus the BACK strip (24) → 196/98.
    int viewH() const {
        return _appMode
            ? (Theme::SCREEN_H - Theme::STATUS_BAR_H - kAppHeaderH - kAppMapPad)  // 196
            : Theme::CONTENT_H;                                                    // 194
    }
    int viewHalfH() const { return viewH() / 2; }
    int viewHalfW() const { return VIEW_W / 2; }   // 160
    // Content-area y where the map viewport starts. Tab mode → 0 (the
    // map covers the full content area from the top). App mode →
    // kAppHeaderH + kAppMapPad (the BACK strip + gap, so the map sits
    // below it). HUDs, nav buttons, GPS marker, and contact pins use
    // this to translate their map-local (0..viewH) coordinates into
    // content-area coordinates when their widgets are parented to the
    // content area itself (not to _mapContainer).
    int mapOriginY() const {
        return _appMode ? (kAppHeaderH + kAppMapPad) : 0;
    }

    // ---- Interaction tunables ----
    // 8 px per trackball-tick matches the home-screen "cursor speed 3"
    // feel — fast enough to cross the screen in ~20 ticks, slow enough
    // to land on a tile without overshoot. Easily tweakable; not stored
    // in NVS for v1.
    static constexpr int PAN_SPEED = 8;
    // Double-tap: same screen position within DOUBLE_TAP_RADIUS_PX within
    // DOUBLE_TAP_MS triggers zoom-in. 400ms / 20px is the conservative
    // side of the iOS/Android double-tap thresholds — appropriate given
    // GT911's single-point finger touches (which can drift by a few px
    // between taps).
    static constexpr unsigned long DOUBLE_TAP_MS = 400;
    static constexpr int DOUBLE_TAP_RADIUS_PX = 20;
    // Marker (GPS dot) redraw + follow-GPS re-center cadence.
    static constexpr unsigned long MARKER_REFRESH_MS = 1000;
    // Tile request throttle — re-arm missing tile requests at most this
    // often to avoid hammering TileCache when the user pans quickly.
    static constexpr unsigned long TILE_REQUEST_INTERVAL_MS = 250;
    // Pan accumulator flush — drains accumulated trackball deltas into
    // _centerWorldPx this often so each Tick is one move.
    static constexpr unsigned long PAN_FLUSH_MS = 50;

    // Zoom range. The actual Basemapsxyz-OSM mapset on the user's card
    // has tiles at z=0..5,7..15 (z=6 is missing). For v1 we clamp to
    // the contiguous 0..15 range as a shortcut; per-mapset probing is a
    // TODO (would require a one-time SD walk similar to listSdMaps()).
    static constexpr int ZOOM_MIN = 0;
    static constexpr int ZOOM_MAX = 15;

    // Default startup view. z=1 (continental-blocks scale) is the lowest
    // multi-tile zoom on the user's SD card. Centering on the world-pixel
    // center of the whole z=1 world (256, 256) — the corner shared by all
    // four continental tiles (0,0)/(1,0)/(0,1)/(1,1) — puts real land on
    // screen from the first frame. The old (384, 384) default was the
    // center of tile (1,1) alone, i.e. the Indian Ocean / SE corner of
    // Africa, which looked like an empty/ocean render.
    // If a GPS fix is available at first entry, onEnter() overrides this
    // by arming follow-GPS and centering on the fix (see _everCenteredOnGps).
    static constexpr int DEFAULT_ZOOM = 8;  // metro-scale default
    static constexpr int64_t DEFAULT_CENTER_WORLD_X = 256;  // z=1 world center
    static constexpr int64_t DEFAULT_CENTER_WORLD_Y = 256;  // z=1 world center

    // Hardcoded mapset for v1. Other mapsets on the user's card are
    // detectable via 'X' serial command; cycling between them is a TODO.
    // (see listSdMaps() in main.cpp for the discovery helper).
    static constexpr const char* MAPSET_NAME = "Basemapsxyz-OSM";

    // ---- Tile slot bookkeeping ----
    struct TileSlot {
        lv_obj_t* bg = nullptr;            // gray placeholder, behind img
        lv_obj_t* img = nullptr;           // tile bitmap (lv_img)
        const lv_img_dsc_t* curDsc = nullptr;
        // TileCache slot generation that curDsc was bound from. The dsc
        // pointer is permanent per cache slot and its buffer gets reused for
        // a different tile, so the pointer alone can't tell us the pixels
        // changed identity — compare (curDsc, curGen) before deciding an
        // attached image is still valid.
        uint32_t curGen = 0;
        int32_t tx = INT32_MIN;            // last-rendered tile x
        int32_t ty = INT32_MIN;            // last-rendered tile y
        int32_t tz = -1;                   // last-rendered zoom
    };

    // ---- Private helpers ----
    void rebuildTiles();        // (re)position slots and re-assign tile images
    void updateMarker();        // reposition GPS marker (and re-center if follow)
    void updateHud();           // refresh zoom / follow / mapset labels
    void centerOnGpsIfAvailable();
    void requestVisibleTiles(); // enqueue tile loads for any visible slot
                                // whose key changed since last request
    void panBy(int32_t dxPx, int32_t dyPx);
    void zoomIn();
    void zoomOut();
    void clampZoom();
    // Keep _centerWorldX/Y inside the world so the viewport can never sit
    // (partly) outside [0, worldPx). Called after every pan/zoom/recenter.
    void clampCenterToWorld();

    // Map viewport origin (top-left of content area) in world px.
    // _centerWorldPx is the center; origin = center - (VIEW_HALF_W, VIEW_HALF_H).
    void viewportOriginWorldPx(int64_t& outX, int64_t& outY) const;
    // Screen position (relative to content-area origin) of a tile's top-left.
    void tileScreenPos(int32_t tx, int32_t ty, int32_t& outX, int32_t& outY) const;

    // ---- Dependencies ----
    TileCache* _tileCache = nullptr;
    GPSManager* _gps = nullptr;
    class UIManager* _ui = nullptr;
    // Peer-on-map (rsDeck #64): nullable. Updated set is small (saved +
    // hasLocation). Refreshed every MARKER_REFRESH_MS via updateMarker().
    class AnnounceManager* _am = nullptr;

    // ---- Display mode ----
    // See class header. Defaults to false so legacy callers (none today,
    // but kept for safety) get the original tab-mode layout.
    bool _appMode = false;

    // ---- State (the 3 things from the design) ----
    int _zoom = DEFAULT_ZOOM;
    int64_t _centerWorldX = DEFAULT_CENTER_WORLD_X;
    int64_t _centerWorldY = DEFAULT_CENTER_WORLD_Y;
    bool _followGPS = false;
    // Set to true after onEnter() has armed follow-GPS for the first
    // time. Prevents subsequent entries from re-snapping to a stale
    // GPS fix when the user has explicitly panned away. Persists for
    // the life of the LvMapScreen object (never reset by destroyUI).
    bool _everCenteredOnGps = false;
    // Track last _zoom logged via the contact-pin diagnostic so we only
    // print once per zoom change (avoids per-tick spam). Negative = no
    // log fired yet.
    int _contactPinLogZoom = -1;
    // Timestamp at which the "no tiles for this area" toast becomes
    // eligible (set on each rebuildTiles() and on pan/zoom). 0 = not
    // pending. Used to delay the toast so we don't fire it during the
    // normal "request just queued, not yet decoded" window.
    unsigned long _noTilesToastPendingMs = 0;
    // True once the "no tiles" toast has fired for the current view.
    // Reset to false on any pan/zoom/recenter so the toast can re-fire
    // for a different area that also has no tiles.
    bool _noTilesToastShown = false;
    // Set by rebuildTiles() if ANY of the 6 grid slots had a READY tile
    // descriptor this pass. Used by the "no tiles for this area" toast
    // in refreshUI() — fires only if zero slots were ready for >2.5s.
    bool _anySlotReadySinceRebuild = false;

    // ---- Visual layers ----
    lv_obj_t* _mapContainer = nullptr;  // clipped parent for tile slots
    TileSlot _slots[SLOT_COUNT];

    // HUD overlay (above tiles, below nav buttons)
    lv_obj_t* _hudZoom = nullptr;
    lv_obj_t* _hudMapset = nullptr;
    lv_obj_t* _hudGps = nullptr;
    lv_obj_t* _hudFollow = nullptr;

    // App-mode BACK pill — top-left, sized to fit inside kAppHeaderH=22
    // with a 2-px top pad. Holds "< BACK" label. Wires its own CLICKED
    // handler so touch works directly (no manual touch-handler plumbing
    // needed). Cached screen-absolute bounds feed isTouchOnBack() so
    // the manual pan/double-tap logic in refreshUI() doesn't fire over
    // the pill (mirrors the nav-button suppression pattern).
    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _backLbl = nullptr;
    int16_t _backBtnX1 = 0, _backBtnY1 = 0, _backBtnX2 = 0, _backBtnY2 = 0;
    BackCallback _onBack;

    // ---- Nav overlay (D-pad + zoom) — small buttons over the map so the
    // user has a discoverable pan/zoom alternative to the trackball and
    // touch-drag. Six buttons total: 4 directional arrows + 2 zoom. Wire
    // each LV_EVENT_CLICKED to the existing panBy/zoomIn/zoomOut methods
    // (no reimplementation). Positioning is bottom-right (above the GPS
    // HUD at y=174, below the FOLLOW HUD at y=2) to avoid overlap with
    // HUD labels. Stays BELOW the GPS marker in z-order so a marker on
    // top of a button doesn't get visually clipped.
    static constexpr int NAV_BTN_COUNT = 6;
    enum NavBtn : int {
        NAV_BTN_LEFT = 0,
        NAV_BTN_UP,
        NAV_BTN_RIGHT,
        NAV_BTN_DOWN,
        NAV_BTN_ZIN,
        NAV_BTN_ZOUT,
    };
    lv_obj_t* _navBtns[NAV_BTN_COUNT] = {nullptr};
    // Cache each button's screen-absolute bounds (touch.x()/touch.y()
    // return raw screen coords, not LVGL-parent-relative coords; the
    // nav-button hit test in refreshUI() needs screen-absolute bounds).
    // Recomputed in rebuildNavOverlay().
    int16_t _navBtnX1[NAV_BTN_COUNT] = {0};
    int16_t _navBtnY1[NAV_BTN_COUNT] = {0};
    int16_t _navBtnX2[NAV_BTN_COUNT] = {0};
    int16_t _navBtnY2[NAV_BTN_COUNT] = {0};
    bool isTouchOnNavButton(int16_t tx, int16_t ty) const;
    bool isTouchOnBack(int16_t tx, int16_t ty) const;
    // Set true when the current touch-down was inside a nav button; the
    // manual pan/double-tap handler skips pan and double-tap detection
    // while this is set, so tapping a button doesn't accidentally pan
    // or trigger a double-tap zoom.
    bool _touchConsumedByNav = false;
    void rebuildNavOverlay();

    // GPS marker — separate obj, drawn on top of EVERYTHING (including
    // nav buttons) so a marker under a button still shows through.
    lv_obj_t* _marker = nullptr;

    // ---- Peer-on-map (rsDeck #64) ----
    // Pool of contact-pin widgets. Saved contacts with lat/lon get a
    // diamond + optional badge + up to MAX_RIBBON_LABELS ribbon labels.
    // The pool is sized MAX_CONTACT_MARKERS and reused across refreshes
    // — hide everything, then assign up to that many cluster pins.
    static constexpr int MAX_CONTACT_MARKERS = 16;
    static constexpr int MAX_RIBBON_LABELS = 4;
    // Cluster radius in screen px — mirrors the Pro MapScreen constant.
    // Two saved contacts within STACK_PX of each other share one diamond.
    static constexpr int STACK_PX = 14;
    // Per-cluster pin visuals. `diamond` is always present; `badge` shows
    // either a single 4-char label (single-member cluster) or a count
    // (multi-member cluster); `ribbons` carry per-member labels stacked
    // vertically beside the diamond.
    struct ContactPinWidget {
        lv_obj_t* diamond = nullptr;
        lv_obj_t* badge = nullptr;
        lv_obj_t* ribbons[MAX_RIBBON_LABELS] = {nullptr};
    };
    ContactPinWidget _contactPins[MAX_CONTACT_MARKERS];
    void updateContactMarkers();
    // Short 4-char label for a contact — name preferred, else hash prefix.
    // Result is NUL-terminated at out[4] (out[5] buffer).
    static void shortLabel(const struct DiscoveredNode& n, char out[5]);

    // ---- Input state ----
    bool _touchActive = false;
    int16_t _touchLastX = 0;
    int16_t _touchLastY = 0;
    bool _touchDoubleArmed = false;
    int16_t _touchDoubleX = 0;
    int16_t _touchDoubleY = 0;
    unsigned long _touchDoubleMs = 0;

    // ---- Timing ----
    unsigned long _lastMarkerMs = 0;
    unsigned long _lastTileRequestMs = 0;
    unsigned long _lastHudRefreshMs = 0;
    static constexpr unsigned long HUD_REFRESH_MS = 500;

    // ---- Tab restoration ----
    // Captured in onEnter() so Esc returns the user to whichever tab they
    // came from instead of an arbitrary one.
    int _prevTab = 0;
};
