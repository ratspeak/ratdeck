#pragma once

#include <lvgl.h>
#include "LvStatusBar.h"
#include "LvTabBar.h"
#include "hal/Keyboard.h"

// LVGL screen base class
class LvScreen {
public:
    virtual ~LvScreen() = default;
    virtual void createUI(lv_obj_t* parent) = 0;
    virtual void destroyUI();
    virtual void refreshUI() {}
    virtual void onEnter() {}
    virtual void onExit() {}
    virtual bool handleKey(const KeyEvent& event) { return false; }
    virtual bool handleLongPress() { return false; }
    virtual const char* title() const = 0;

    lv_obj_t* screen() const { return _screen; }

protected:
    lv_obj_t* _screen = nullptr;
};

class UIManager {
public:
    void begin();

    // Screen management
    void setScreen(LvScreen* screen);
    LvScreen* getScreen() { return _currentLvScreen; }

    // Component access
    LvStatusBar& lvStatusBar() { return _lvStatusBar; }
    LvTabBar& lvTabBar() { return _lvTabBar; }

    // Update data (called periodically)
    void update();

    // Force full redraw
    void forceRedraw();

    // Re-apply palette to shared styles and persistent shell after a theme switch
    void applyTheme();

    // Handle key event — routes to current screen
    bool handleKey(const KeyEvent& event);
    bool handleLongPress();

    // Boot mode — hides status bar and tab bar
    void setBootMode(bool boot);
    bool isBootMode() const { return _bootMode; }

    // LVGL content area parent (between status bar and tab bar)
    lv_obj_t* contentParent() { return _lvContent; }

    // Tab bar visibility — used when opening a screen "as an app" from
    // the Apps tab (e.g. Peers) so the tab bar doesn't compete for the
    // tiny 320x240 surface. Status bar stays visible (radio/time/peer
    // count remain reachable). Defaults to visible. The flag is honored
    // by setScreen() so navigating to another tab restores it.
    void setTabBarVisible(bool visible);
    bool tabBarVisible() const { return _tabBarVisible; }

private:
    // LVGL components
    LvStatusBar _lvStatusBar;
    LvTabBar _lvTabBar;
    LvScreen* _currentLvScreen = nullptr;
    lv_obj_t* _lvContent = nullptr;

    bool _bootMode = false;
    bool _tabBarVisible = true;

    // Apply the current tab-bar visibility flag to the LVGL shell, also
    // resizing the content area to fill the freed row. Split out of
    // setScreen/setBootMode so each path can call it at the right moment
    // without duplicating the resize math.
    void applyTabBarVisibility();
};
