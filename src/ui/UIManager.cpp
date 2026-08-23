#include "UIManager.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include "util/PerfTrace.h"

// --- LvScreen base ---

void LvScreen::destroyUI() {
    // Content is owned by UIManager's _lvContent — just clear our pointers.
    // The UIManager calls lv_obj_clean(_lvContent) before creating the next screen.
    _screen = nullptr;
}

// --- UIManager ---

void UIManager::begin() {
    // Initialize LVGL theme and create persistent UI structure
    lv_obj_t* scr = lv_scr_act();
    LvTheme::init(lv_disp_get_default());

    // Apply screen background style
    lv_obj_add_style(scr, LvTheme::styleScreen(), 0);

    // Create LVGL status bar (top)
    _lvStatusBar.create(scr);

    // Create LVGL tab bar (bottom)
    _lvTabBar.create(scr);

    // Create content area between status bar and tab bar
    _lvContent = lv_obj_create(scr);
    lv_obj_set_pos(_lvContent, 0, Theme::STATUS_BAR_H);
    lv_obj_set_size(_lvContent, Theme::CONTENT_W, Theme::CONTENT_H);
    lv_obj_set_style_bg_color(_lvContent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_lvContent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_lvContent, 0, 0);
    lv_obj_set_style_pad_all(_lvContent, 0, 0);
    lv_obj_set_style_radius(_lvContent, 0, 0);

    Serial.println("[UI] LVGL UI structure created");
}

void UIManager::setScreen(LvScreen* screen) {
    if (_currentLvScreen == screen) return;
    const char* fromTitle = _currentLvScreen ? _currentLvScreen->title() : "none";
    const char* toTitle = screen ? screen->title() : "none";
    unsigned long startMs = PerfTrace::nowMs();
    unsigned long exitMs = 0;
    unsigned long destroyMs = 0;
    unsigned long showMs = 0;
    unsigned long cleanMs = 0;
    unsigned long createMs = 0;
    unsigned long enterMs = 0;

    // Transition from previous LVGL screen
    if (_currentLvScreen) {
        unsigned long phaseMs = millis();
        _currentLvScreen->onExit();
        exitMs = millis() - phaseMs;
        phaseMs = millis();
        _currentLvScreen->destroyUI();
        destroyMs = millis() - phaseMs;
    }

    _currentLvScreen = screen;

    // Show LVGL layers. Status bar always shows in normal mode; tab bar
    // visibility is governed by setTabBarVisible(). This split lets
    // "app-style" screens (Peers opened from the Apps tab) keep the
    // status bar but reclaim the bottom 26 px for content.
    unsigned long phaseMs = millis();
    if (!_bootMode) {
        lv_obj_clear_flag(_lvStatusBar.obj(), LV_OBJ_FLAG_HIDDEN);
        applyTabBarVisibility();
    }
    lv_obj_clear_flag(_lvContent, LV_OBJ_FLAG_HIDDEN);
    showMs = millis() - phaseMs;

    if (_currentLvScreen) {
        // Clean content area
        phaseMs = millis();
        lv_obj_clean(_lvContent);
        cleanMs = millis() - phaseMs;
        phaseMs = millis();
        _currentLvScreen->createUI(_lvContent);
        createMs = millis() - phaseMs;
        phaseMs = millis();
        _currentLvScreen->onEnter();
        enterMs = millis() - phaseMs;
    }

    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
        Serial.printf("[PERF] UI transition: %s -> %s total=%lums exit=%lums destroy=%lums show=%lums clean=%lums create=%lums enter=%lums boot=%s\n",
                      fromTitle, toTitle, elapsed, exitMs, destroyMs, showMs,
                      cleanMs, createMs, enterMs, _bootMode ? "yes" : "no");
    }
}

void UIManager::setBootMode(bool boot) {
    _bootMode = boot;
    if (boot) {
        lv_obj_add_flag(_lvStatusBar.obj(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_lvTabBar.obj(), LV_OBJ_FLAG_HIDDEN);
        // In boot mode, content area is full screen
        lv_obj_set_pos(_lvContent, 0, 0);
        lv_obj_set_size(_lvContent, Theme::SCREEN_W, Theme::SCREEN_H);
    } else {
        lv_obj_clear_flag(_lvStatusBar.obj(), LV_OBJ_FLAG_HIDDEN);
        // Tab bar follows the visibility flag, not boot-mode restore, so
        // leaving boot mode while a Peers-as-app session is active keeps
        // the tab bar hidden.
        applyTabBarVisibility();
        lv_obj_set_pos(_lvContent, 0, Theme::STATUS_BAR_H);
    }
}

void UIManager::setTabBarVisible(bool visible) {
    if (_tabBarVisible == visible) return;
    _tabBarVisible = visible;
    if (_bootMode) {
        // Boot mode owns its own visibility; nothing to do until we exit.
        return;
    }
    applyTabBarVisibility();
}

void UIManager::applyTabBarVisibility() {
    if (!_lvContent) return;
    if (_tabBarVisible) {
        lv_obj_clear_flag(_lvTabBar.obj(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_lvContent, 0, Theme::STATUS_BAR_H);
        lv_obj_set_size(_lvContent, Theme::CONTENT_W, Theme::CONTENT_H);
    } else {
        lv_obj_add_flag(_lvTabBar.obj(), LV_OBJ_FLAG_HIDDEN);
        // Keep status bar visible; reclaim the bottom 26 px for the
        // current screen. CONTENT_Y unchanged.
        lv_obj_set_pos(_lvContent, 0, Theme::STATUS_BAR_H);
        lv_obj_set_size(_lvContent, Theme::CONTENT_W,
                        Theme::SCREEN_H - Theme::STATUS_BAR_H);
    }
}

void UIManager::update() {
    _lvStatusBar.update();
    if (_currentLvScreen) _currentLvScreen->refreshUI();
}

void UIManager::forceRedraw() {
    lv_obj_invalidate(lv_scr_act());
}

void UIManager::applyTheme() {
    LvTheme::refresh();
    if (_lvContent) lv_obj_set_style_bg_color(_lvContent, lv_color_hex(Theme::BG), 0);
    _lvStatusBar.applyTheme();
    _lvTabBar.refreshTabs();
    LvInput::applyTheme();
    forceRedraw();
}

bool UIManager::handleKey(const KeyEvent& event) {
    if (_currentLvScreen) {
        return _currentLvScreen->handleKey(event);
    }
    return false;
}

bool UIManager::handleLongPress() {
    if (_currentLvScreen) {
        return _currentLvScreen->handleLongPress();
    }
    return false;
}
