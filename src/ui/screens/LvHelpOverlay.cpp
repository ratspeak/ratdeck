#include "LvHelpOverlay.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include <lvgl.h>
#include "fonts/fonts.h"

namespace {

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    uint32_t color, lv_coord_t width, lv_text_align_t align,
                    lv_label_long_mode_t mode = LV_LABEL_LONG_DOT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_width(lbl, width);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_long_mode(lbl, mode);
    lv_label_set_text(lbl, text);
    return lbl;
}

lv_obj_t* makeRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 13);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void addHelpRow(lv_obj_t* parent, const char* keys, const char* action) {
    lv_obj_t* row = makeRow(parent);
    makeLabel(row, keys, &lv_font_rsdeck_10, Theme::ACCENT, 72, LV_TEXT_ALIGN_LEFT);
    makeLabel(row, action, &lv_font_rsdeck_10, Theme::TEXT_PRIMARY, 178, LV_TEXT_ALIGN_LEFT);
}

}  // namespace

void LvHelpOverlay::create() {
    if (_overlay) return;

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, 304, 190);
    lv_obj_center(_overlay);
    lv_obj_add_style(_overlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_overlay, 8, 0);
    lv_obj_set_style_pad_row(_overlay, 1, 0);
    lv_obj_set_layout(_overlay, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_overlay, [](lv_event_t* e) {
        auto* self = static_cast<LvHelpOverlay*>(lv_event_get_user_data(e));
        if (self) self->hide();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* header = makeRow(_overlay);
    lv_obj_set_height(header, 16);
    makeLabel(header, "HELP", &lv_font_rsdeck_12, Theme::ACCENT, 70, LV_TEXT_ALIGN_LEFT);
    makeLabel(header, "Ratspeak.org", &lv_font_rsdeck_10,
              Theme::TEXT_SECONDARY, 180, LV_TEXT_ALIGN_RIGHT);

    addHelpRow(_overlay, "Trackball", "Move selection");
    addHelpRow(_overlay, "Enter", "Open or confirm");
    addHelpRow(_overlay, ",  /", "Previous or next tab");
    addHelpRow(_overlay, "Esc", "Back");
    addHelpRow(_overlay, "Ctrl+M", "Messages");
    addHelpRow(_overlay, "Ctrl+N", "New message");
    addHelpRow(_overlay, "Ctrl+S", "Settings");
    addHelpRow(_overlay, "Ctrl+A", "Announce");
    addHelpRow(_overlay, "Ctrl+L", "Offline map");
    addHelpRow(_overlay, "Ctrl+D/I/T/R", "Diagnostics tools");

    lv_obj_t* footer = makeLabel(_overlay, "Any key or tap closes",
                                 &lv_font_rsdeck_10, Theme::TEXT_MUTED,
                                 260, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_pad_top(footer, 2, 0);

    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LvHelpOverlay::show() {
    if (!_overlay) create();
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    _visible = true;
}

void LvHelpOverlay::hide() {
    if (_overlay) lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    _visible = false;
}

void LvHelpOverlay::toggle() {
    if (_visible) hide(); else show();
}

bool LvHelpOverlay::handleKey(const KeyEvent& event) {
    (void)event;
    if (_visible) {
        hide();
        return true;
    }
    return false;
}
