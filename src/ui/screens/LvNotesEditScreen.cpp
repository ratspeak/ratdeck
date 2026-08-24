#include "LvNotesEditScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "storage/SDStore.h"
#include <Arduino.h>
#include "fonts/fonts.h"

namespace {

// App-mode chrome — same dimensions as LvNotesListScreen and the Map
// BACK pill so all the apps feel consistent. The textarea starts at
// y=kMapY and fills the rest of the content area.
constexpr lv_coord_t kHeaderH   = 22;
constexpr lv_coord_t kHeaderPad = 2;
constexpr lv_coord_t kMapY      = kHeaderH + kHeaderPad;   // 24

constexpr lv_coord_t kBackX = 4;
constexpr lv_coord_t kBackY = 2;
constexpr lv_coord_t kBackW = 64;
constexpr lv_coord_t kBackH = kHeaderH - 4;   // 18

constexpr lv_coord_t kSaveX = Theme::CONTENT_W - 4 - 64;   // 252
constexpr lv_coord_t kSaveY = kBackY;
constexpr lv_coord_t kSaveW = 64;
constexpr lv_coord_t kSaveH = kBackH;

}  // namespace

void LvNotesEditScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header strip (BACK + SAVE pills, filename label centered) ----
    _backBtn = lv_btn_create(parent);
    lv_obj_set_size(_backBtn, kBackW, kBackH);
    lv_obj_set_pos(_backBtn, kBackX, kBackY);
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
        auto* self = (LvNotesEditScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);
    _backBtnX1 = kBackX;
    _backBtnY1 = kBackY + Theme::STATUS_BAR_H;
    _backBtnX2 = kBackX + kBackW - 1;
    _backBtnY2 = kBackY + kBackH - 1 + Theme::STATUS_BAR_H;

    _saveBtn = lv_btn_create(parent);
    lv_obj_set_size(_saveBtn, kSaveW, kSaveH);
    lv_obj_set_pos(_saveBtn, kSaveX, kSaveY);
    lv_obj_add_style(_saveBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_saveBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_saveBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_saveBtn, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_border_width(_saveBtn, 1, 0);
    lv_obj_set_style_radius(_saveBtn, 3, 0);
    lv_obj_set_style_pad_all(_saveBtn, 0, 0);
    lv_obj_set_style_shadow_width(_saveBtn, 0, 0);
    lv_obj_clear_flag(_saveBtn, LV_OBJ_FLAG_SCROLLABLE);
    _saveLbl = lv_label_create(_saveBtn);
    lv_obj_set_style_text_font(_saveLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_saveLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_text_color(_saveLbl, lv_color_hex(Theme::ACCENT), LV_STATE_PRESSED);
    lv_label_set_text(_saveLbl, "SAVE");
    lv_obj_center(_saveLbl);
    lv_obj_add_event_cb(_saveBtn, [](lv_event_t* e) {
        auto* self = (LvNotesEditScreen*)lv_event_get_user_data(e);
        if (self->_onSave) self->_onSave();
    }, LV_EVENT_CLICKED, this);
    _saveBtnX1 = kSaveX;
    _saveBtnY1 = kSaveY + Theme::STATUS_BAR_H;
    _saveBtnX2 = kSaveX + kSaveW - 1;
    _saveBtnY2 = kSaveY + kSaveH - 1 + Theme::STATUS_BAR_H;

    _headerLbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_headerLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_headerLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_headerLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_headerLbl, Theme::CONTENT_W - (kBackX + kBackW + kSaveW + 8));
    lv_obj_set_pos(_headerLbl, kBackX + kBackW + 4, kBackY + 2);

    // ---- Body (multiline textarea) ----
    _textarea = lv_textarea_create(parent);
    lv_obj_set_size(_textarea, Theme::CONTENT_W, Theme::CONTENT_H - kMapY);
    lv_obj_set_pos(_textarea, 0, kMapY);
    lv_obj_add_style(_textarea, LvTheme::styleTextarea(), 0);
    lv_obj_add_style(_textarea, LvTheme::styleTextareaFocused(), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_textarea, &lv_font_rsdeck_12, 0);
    lv_textarea_set_max_length(_textarea, MAX_BODY_LEN);
    lv_textarea_set_one_line(_textarea, false);   // multiline
    lv_textarea_set_placeholder_text(_textarea, "Write here...");
    lv_group_add_obj(LvInput::group(), _textarea);
    lv_group_focus_obj(_textarea);

    paintHeader();
    loadFromSD();
}

void LvNotesEditScreen::destroyUI() {
    _backBtn = nullptr; _backLbl = nullptr;
    _saveBtn = nullptr;  _saveLbl = nullptr;
    _headerLbl = nullptr;
    _textarea = nullptr;
    _loaded = false;
    LvScreen::destroyUI();
}

void LvNotesEditScreen::setFilename(const String& filename) {
    _filename = filename;
    paintHeader();
    // If the screen is already created, reload body from the new path
    // immediately so the user sees the right content if they re-enter.
    if (_loaded) loadFromSD();
}

void LvNotesEditScreen::onEnter() {
    paintHeader();
    loadFromSD();
}

void LvNotesEditScreen::refreshUI() {
    // Nothing periodic — body is in the LVGL textarea widget directly.
}

String LvNotesEditScreen::body() const {
    if (!_textarea) return String("");
    const char* text = lv_textarea_get_text(_textarea);
    return String(text ? text : "");
}

void LvNotesEditScreen::paintHeader() {
    if (!_headerLbl) return;
    if (_filename.length() == 0) {
        lv_label_set_text(_headerLbl, "New note");
    } else {
        lv_label_set_text(_headerLbl, _filename.c_str());
    }
}

void LvNotesEditScreen::loadFromSD() {
    if (!_textarea || _loaded) return;
    if (_filename.length() == 0 || !_sd || !_sd->isReady()) {
        // Empty new note, or no SD card to read from — nothing to load.
        // Show a hint so the user knows why the editor is blank. We
        // don't show "No SD" for a brand-new note (length==0) because
        // the textarea is legitimately empty in that case.
        if (_filename.length() != 0 && (!_sd || !_sd->isReady())) {
            if (_headerLbl) lv_label_set_text(_headerLbl, "No SD card");
        }
        _loaded = true;
        return;
    }
    String path = String("/Files/notes/") + _filename;
    String contents = _sd->readString(path.c_str());
    // SDStore::readString returns "" on miss or read failure; if we got
    // "" here the file is genuinely empty OR the SD couldn't read it.
    // We don't surface an error toast for the MVP — the user can hit
    // BACK and see the file isn't in the list if it doesn't exist.
    // Clamp to MAX_BODY_LEN so an oversized file (somehow) can't blow
    // past the textarea cap. lv_textarea_set_text would also truncate
    // at max_length, but doing it here keeps the body() round-trip in
    // sync (no surprise lost bytes on save).
    if ((int)contents.length() > MAX_BODY_LEN) {
        contents = contents.substring(0, MAX_BODY_LEN);
    }
    lv_textarea_set_text(_textarea, contents.c_str());
    lv_textarea_set_cursor_pos(_textarea, LV_TEXTAREA_CURSOR_LAST);
    _loaded = true;
}

bool LvNotesEditScreen::handleKey(const KeyEvent& event) {
    if (!_textarea) return false;

    // Esc / back → list. We don't currently warn about unsaved changes
    // (MVP — discard is acceptable).
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) _onBack();
        return true;
    }

    // Ctrl+S / Cmd+S → save. Standard editor shortcut, also avoids the
    // user having to trackball-drag to the SAVE pill for a quick save.
    // ASCII 0x13 = ^S, 0x19 = ^Y (some keyboards map Cmd to Ctrl).
    if (event.character == 0x13 || event.character == 0x19) {
        if (_onSave) _onSave();
        return true;
    }

    // Printable chars → forward to the LVGL textarea (which updates its
    // buffer + cursor). Same pattern as LvNameInputScreen.
    if (event.character >= 0x20 && event.character <= 0x7E) {
        char buf[2] = {event.character, 0};
        lv_textarea_add_text(_textarea, buf);
        return true;
    }

    // Newline (multiline textarea — Enter inserts a newline character).
    if (event.character == '\n' || event.character == '\r') {
        // LVGL handles Enter on a focused textarea by inserting newline
        // automatically; we don't need to inject it ourselves. Let
        // LVGL have the event (return false) so its built-in handling
        // runs.
        return false;
    }

    // Backspace inside the textarea body — same delegation to LVGL.
    // Note: 'del' (the KeyEvent flag) is the keyboard's BACKSPACE; we
    // already consume it for Esc above. The 0x08 char path also maps
    // to backspace in some layouts.
    return false;
}

bool LvNotesEditScreen::handleLongPress() {
    // No long-press action in the editor — global default (screen off)
    // stays.
    return false;
}
