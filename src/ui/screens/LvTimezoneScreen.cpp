#include "LvTimezoneScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "config/Config.h"

void LvTimezoneScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);

    const lv_font_t* font12 = &lv_font_montserrat_12;
    const lv_font_t* font14 = &lv_font_montserrat_14;

    // Title
    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(title, "SELECT TIMEZONE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Subtitle
    lv_obj_t* sub = lv_label_create(parent);
    lv_obj_set_style_text_font(sub, font12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(Theme::SECONDARY), 0);
    lv_label_set_text(sub, "Choose your nearest city:");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 30);

    // Scrollable list container
    int listY = 50;
    int listH = VISIBLE_ROWS * ROW_H;
    _scrollContainer = lv_obj_create(parent);
    lv_obj_set_size(_scrollContainer, Theme::SCREEN_W - 20, listH);
    lv_obj_set_pos(_scrollContainer, 10, listY);
    lv_obj_set_style_bg_color(_scrollContainer, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_scrollContainer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scrollContainer, 1, 0);
    lv_obj_set_style_border_color(_scrollContainer, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_pad_all(_scrollContainer, 0, 0);
    lv_obj_set_style_radius(_scrollContainer, 4, 0);
    lv_obj_set_layout(_scrollContainer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_scrollContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_snap_y(_scrollContainer, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(_scrollContainer, LV_SCROLLBAR_MODE_OFF);

    // Create rows
    for (int i = 0; i < TIMEZONE_COUNT; i++) {
        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::SCREEN_W - 24, ROW_H);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 0, 0);
        lv_obj_set_style_pad_bottom(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool selected = (i == _selectedIdx);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? Theme::SELECTION_BG : Theme::BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);

        // City name (left)
        lv_obj_t* lblCity = lv_label_create(row);
        lv_obj_set_style_text_font(lblCity, font14, 0);
        lv_obj_set_style_text_color(lblCity, lv_color_hex(selected ? Theme::BG : Theme::PRIMARY), 0);
        lv_label_set_text(lblCity, TIMEZONE_TABLE[i].label);
        lv_obj_align(lblCity, LV_ALIGN_LEFT_MID, 0, 0);

        // UTC offset (right)
        lv_obj_t* lblOffset = lv_label_create(row);
        lv_obj_set_style_text_font(lblOffset, font12, 0);
        lv_obj_set_style_text_color(lblOffset, lv_color_hex(selected ? Theme::BG : Theme::ACCENT), 0);
        char buf[12];
        int8_t off = TIMEZONE_TABLE[i].baseOffset;
        snprintf(buf, sizeof(buf), "UTC%+d", off);
        lv_label_set_text(lblOffset, buf);
        lv_obj_align(lblOffset, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // Scroll initial selection into view
    if (_selectedIdx >= 0 && _selectedIdx < TIMEZONE_COUNT) {
        lv_obj_t* selRow = lv_obj_get_child(_scrollContainer, _selectedIdx);
        if (selRow) lv_obj_scroll_to_view(selRow, LV_ANIM_OFF);
    }

    // Hint at bottom
    lv_obj_t* hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, font12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(hint, "[Enter] Select");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void LvTimezoneScreen::updateSelection(int oldIdx, int newIdx) {
    if (!_scrollContainer) return;
    uint32_t count = lv_obj_get_child_cnt(_scrollContainer);

    const lv_font_t* font14 = &lv_font_montserrat_14;
    const lv_font_t* font12 = &lv_font_montserrat_12;

    // Deselect old
    if (oldIdx >= 0 && oldIdx < (int)count) {
        lv_obj_t* row = lv_obj_get_child(_scrollContainer, oldIdx);
        lv_obj_set_style_bg_color(row, lv_color_hex(Theme::BG), 0);
        // Update text colors
        lv_obj_t* lblCity = lv_obj_get_child(row, 0);
        lv_obj_t* lblOff = lv_obj_get_child(row, 1);
        if (lblCity) lv_obj_set_style_text_color(lblCity, lv_color_hex(Theme::PRIMARY), 0);
        if (lblOff) lv_obj_set_style_text_color(lblOff, lv_color_hex(Theme::ACCENT), 0);
    }

    // Select new
    if (newIdx >= 0 && newIdx < (int)count) {
        lv_obj_t* row = lv_obj_get_child(_scrollContainer, newIdx);
        lv_obj_set_style_bg_color(row, lv_color_hex(Theme::SELECTION_BG), 0);
        lv_obj_t* lblCity = lv_obj_get_child(row, 0);
        lv_obj_t* lblOff = lv_obj_get_child(row, 1);
        if (lblCity) lv_obj_set_style_text_color(lblCity, lv_color_hex(Theme::BG), 0);
        if (lblOff) lv_obj_set_style_text_color(lblOff, lv_color_hex(Theme::BG), 0);

        // Scroll to make visible
        lv_obj_scroll_to_view(row, LV_ANIM_ON);
    }
}

bool LvTimezoneScreen::handleKey(const KeyEvent& event) {
    if (event.enter || event.character == '\n' || event.character == '\r') {
        // Guard: ignore Enter for first 600ms to prevent stale keypress bleed
        if (millis() - _enterTime < ENTER_GUARD_MS) return true;
        if (_doneCb) _doneCb(_selectedIdx);
        return true;
    }

    if (event.up) {
        if (_selectedIdx > 0) {
            int old = _selectedIdx;
            _selectedIdx--;
            updateSelection(old, _selectedIdx);
        }
        return true;
    }

    if (event.down) {
        if (_selectedIdx < TIMEZONE_COUNT - 1) {
            int old = _selectedIdx;
            _selectedIdx++;
            updateSelection(old, _selectedIdx);
        }
        return true;
    }

    return true;  // Consume all keys
}
