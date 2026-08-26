#include "LvTabBar.h"
#include "Theme.h"
#include "LvTheme.h"
#include <cstdio>
#include "fonts/fonts.h"

constexpr const char* LvTabBar::TAB_NAMES[TAB_COUNT];

static lv_obj_t* tab_cell_from_target(LvTabBar* bar, lv_obj_t* target) {
    while (target && target != bar->obj()) {
        lv_obj_t* parent = lv_obj_get_parent(target);
        if (parent == bar->obj()) return target;
        target = parent;
    }
    return nullptr;
}

static void tab_click_cb(lv_event_t* e) {
    LvTabBar* bar = (LvTabBar*)lv_event_get_user_data(e);
    lv_obj_t* target = tab_cell_from_target(bar, lv_event_get_target(e));
    for (int i = 0; i < LvTabBar::TAB_COUNT; i++) {
        if (target == lv_obj_get_child(bar->obj(), i)) {
            bar->setActiveTab(i);
            break;
        }
    }
}

void LvTabBar::create(lv_obj_t* parent) {
    _bar = lv_obj_create(parent);
    lv_obj_set_size(_bar, Theme::SCREEN_W, Theme::TAB_BAR_H);
    lv_obj_align(_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_style(_bar, LvTheme::styleTabBar(), 0);
    lv_obj_clear_flag(_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(_bar, 0, 0);

    for (int i = 0; i < TAB_COUNT; i++) {
        _cells[i] = lv_obj_create(_bar);
        bool isIconTab = (i == TAB_HOME || i == TAB_SETTINGS);
        int cellW = isIconTab ? Theme::ICON_TAB_W : Theme::TEXT_TAB_W;
        lv_obj_set_size(_cells[i], cellW, Theme::TAB_BAR_H);
        lv_obj_add_style(_cells[i], LvTheme::styleTabCell(), 0);
        lv_obj_add_style(_cells[i], LvTheme::styleTabCellActive(), LV_STATE_CHECKED);
        lv_obj_clear_flag(_cells[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_cells[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_cells[i], tab_click_cb, LV_EVENT_CLICKED, this);

        _labels[i] = lv_label_create(_cells[i]);
        lv_obj_set_size(_labels[i], cellW - 4, 14);
        lv_label_set_long_mode(_labels[i], LV_LABEL_LONG_CLIP);
        if (isIconTab) {
            lv_obj_set_style_text_font(_labels[i], &lv_font_montserrat_12, 0);
            const char* sym = (i == TAB_HOME) ? LV_SYMBOL_HOME : LV_SYMBOL_SETTINGS;
            lv_label_set_text(_labels[i], sym);
        } else {
            lv_obj_set_style_text_font(_labels[i], &lv_font_rsdeck_10, 0);
            lv_label_set_text(_labels[i], TAB_NAMES[i]);
        }
        lv_obj_set_style_text_align(_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(_labels[i], LV_ALIGN_CENTER, 0, 2);

        _badges[i] = lv_obj_create(_cells[i]);
        lv_obj_set_size(_badges[i], Theme::TAB_BADGE_W, Theme::TAB_BADGE_H);
        lv_obj_add_style(_badges[i], LvTheme::styleBadge(), 0);
        lv_obj_clear_flag(_badges[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_badges[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_badges[i], LV_ALIGN_TOP_RIGHT, -3, 2);

        _badgeLabels[i] = lv_label_create(_badges[i]);
        lv_label_set_long_mode(_badgeLabels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(_badgeLabels[i], &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_align(_badgeLabels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(_badgeLabels[i]);
    }

    refreshTabs();
}

void LvTabBar::setActiveTab(int tab) {
    if (tab < 0 || tab >= TAB_COUNT) return;
    _activeTab = tab;
    refreshTabs();
    if (_tabCb) _tabCb(tab);
}

void LvTabBar::cycleTab(int direction) {
    int next = (_activeTab + direction + TAB_COUNT) % TAB_COUNT;
    setActiveTab(next);
}

void LvTabBar::setUnreadCount(int tab, int count) {
    if (tab < 0 || tab >= TAB_COUNT) return;
    if (count < 0) count = 0;
    if (_unread[tab] == count) return;  // No change
    _unread[tab] = count;
    refreshTab(tab);
}

void LvTabBar::refreshTab(int idx) {
    if (idx < 0 || idx >= TAB_COUNT || !_cells[idx] || !_labels[idx]) return;
    bool active = (idx == _activeTab);
    if (active) lv_obj_add_state(_cells[idx], LV_STATE_CHECKED);
    else lv_obj_clear_state(_cells[idx], LV_STATE_CHECKED);

    lv_obj_set_style_text_color(_labels[idx],
        lv_color_hex(active ? Theme::TAB_ACTIVE : Theme::TAB_INACTIVE), 0);
    bool isIconTab = (idx == TAB_HOME || idx == TAB_SETTINGS);
    if (!isIconTab) {
        lv_label_set_text(_labels[idx], TAB_NAMES[idx]);
    }

    if (_badges[idx] && _badgeLabels[idx] && _unread[idx] > 0) {
        char buf[4];
        if (_unread[idx] > 9) snprintf(buf, sizeof(buf), "9+");
        else snprintf(buf, sizeof(buf), "%d", _unread[idx]);
        lv_label_set_text(_badgeLabels[idx], buf);
        lv_obj_clear_flag(_badges[idx], LV_OBJ_FLAG_HIDDEN);
    } else {
        if (_badges[idx]) lv_obj_add_flag(_badges[idx], LV_OBJ_FLAG_HIDDEN);
    }
}

void LvTabBar::refreshTabs() {
    for (int i = 0; i < TAB_COUNT; i++) {
        refreshTab(i);
    }
}
