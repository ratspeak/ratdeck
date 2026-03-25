#include "LvNameInputScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "config/Config.h"

void LvNameInputScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);

    // Title: "RATSPEAK"
    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(title, "RATSPEAK");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // Subtitle
    lv_obj_t* sub = lv_label_create(parent);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(sub, "ratspeak.org");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 42);

    // Prompt
    lv_obj_t* prompt = lv_label_create(parent);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(prompt, lv_color_hex(Theme::SECONDARY), 0);
    lv_label_set_text(prompt, "Enter your display name:");
    lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 70);

    // Text area
    _textarea = lv_textarea_create(parent);
    lv_obj_set_size(_textarea, 220, 36);
    lv_obj_align(_textarea, LV_ALIGN_TOP_MID, 0, 95);
    lv_textarea_set_max_length(_textarea, MAX_NAME_LEN);
    lv_textarea_set_one_line(_textarea, true);
    lv_textarea_set_placeholder_text(_textarea, "Optional");
    lv_obj_add_style(_textarea, LvTheme::styleTextarea(), 0);
    lv_obj_set_style_text_font(_textarea, &lv_font_montserrat_14, 0);

    // Hint
    lv_obj_t* hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(hint, "[Enter] OK");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 140);

    // Version
    lv_obj_t* ver = lv_label_create(parent);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(Theme::MUTED), 0);
    char verBuf[32];
    snprintf(verBuf, sizeof(verBuf), "v%s", RATDECK_VERSION_STRING);
    lv_label_set_text(ver, verBuf);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -10);
}

bool LvNameInputScreen::handleKey(const KeyEvent& event) {
    if (!_textarea) return false;

    if (event.enter || event.character == '\n' || event.character == '\r') {
        // Guard: ignore Enter for first 600ms to prevent stale keypress from factory reset
        if (millis() - _enterTime < ENTER_GUARD_MS) return true;
        const char* text = lv_textarea_get_text(_textarea);
        if (_doneCb) {
            _doneCb(String(text && strlen(text) > 0 ? text : ""));
        }
        return true;
    }
    if (event.del || event.character == 8) {
        lv_textarea_del_char(_textarea);
        return true;
    }
    if (event.character >= 0x20 && event.character <= 0x7E) {
        char buf[2] = {event.character, 0};
        lv_textarea_add_text(_textarea, buf);
        return true;
    }
    return true;  // Consume all keys
}
