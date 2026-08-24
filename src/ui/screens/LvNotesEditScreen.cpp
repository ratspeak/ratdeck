#include "LvNotesEditScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "storage/SDStore.h"
#include "util/FileCrypto.h"
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

// SAVE stays top-right; the LOCK/PLAIN pill sits immediately to its
// left. With Theme::CONTENT_W = 320 and a 64-px SAVE pill, the LOCK
// pill at width 56 leaves a 4-px gap and starts at x=188.
constexpr lv_coord_t kSaveW = 64;
constexpr lv_coord_t kLockW = 56;
constexpr lv_coord_t kSaveX = Theme::CONTENT_W - 4 - kSaveW;     // 252
constexpr lv_coord_t kLockX = kSaveX - 4 - kLockW;               // 192
constexpr lv_coord_t kBackSaveY = kBackY;
constexpr lv_coord_t kBackSaveH = kBackH;

// Modal geometry — centered, leaves room for the textarea header.
constexpr lv_coord_t kModalW = 280;
constexpr lv_coord_t kModalH = 156;
constexpr lv_coord_t kModalTitleY  = 8;
constexpr lv_coord_t kModalHintY   = 32;
constexpr lv_coord_t kModalPassY   = 56;
constexpr lv_coord_t kModalPassH   = 32;
constexpr lv_coord_t kModalErrY    = 92;
constexpr lv_coord_t kModalBtnY    = 116;
constexpr lv_coord_t kModalBtnW    = 80;
constexpr lv_coord_t kModalBtnH    = 26;

// Case-insensitive ASCII lower for the .note.enc detector — local to
// this TU so it doesn't conflict with the same helper in the list
// screen (which is in its own anonymous namespace).
char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool endsWithNoteEnc(const char* name) {
    if (!name) return false;
    static const char kExt[] = ".note.enc";
    constexpr size_t el = sizeof(kExt) - 1;
    size_t n = strlen(name);
    if (n < el) return false;
    const char* p = name + n - el;
    for (size_t i = 0; i < el; ++i) {
        if (asciiLower(p[i]) != kExt[i]) return false;
    }
    return true;
}

}  // namespace

void LvNotesEditScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header strip ----
    // BACK (top-left)
    _backBtn = lv_btn_create(parent);
    lv_obj_set_size(_backBtn, kBackW, kBackSaveH);
    lv_obj_set_pos(_backBtn, kBackX, kBackSaveY);
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
        // Discard unsaved edits (MVP). Wipe any pending passphrase
        // before returning so we don't leave it lying in our buffers.
        self->wipePassBuf();
        if (self->_modal) self->hideModal();
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);
    _backBtnX1 = kBackX;
    _backBtnY1 = kBackY + Theme::STATUS_BAR_H;
    _backBtnX2 = kBackX + kBackW - 1;
    _backBtnY2 = kBackY + kBackSaveH - 1 + Theme::STATUS_BAR_H;

    // LOCK/PLAIN toggle (between BACK and SAVE). Hidden while an
    // existing encrypted file forces _locked=true — see setFilename().
    _lockBtn = lv_btn_create(parent);
    lv_obj_set_size(_lockBtn, kLockW, kBackSaveH);
    lv_obj_set_pos(_lockBtn, kLockX, kBackSaveY);
    lv_obj_add_style(_lockBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_lockBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_lockBtn, LV_OPA_70, 0);
    // Border reflects the current mode: PRIMARY (signal green) when
    // locked, BORDER when plain. Updated in paintLockPill().
    lv_obj_set_style_border_color(_lockBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_lockBtn, 1, 0);
    lv_obj_set_style_radius(_lockBtn, 3, 0);
    lv_obj_set_style_pad_all(_lockBtn, 0, 0);
    lv_obj_set_style_shadow_width(_lockBtn, 0, 0);
    lv_obj_clear_flag(_lockBtn, LV_OBJ_FLAG_SCROLLABLE);
    _lockLbl = lv_label_create(_lockBtn);
    lv_obj_set_style_text_font(_lockLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_lockLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_obj_center(_lockLbl);
    lv_obj_add_event_cb(_lockBtn, [](lv_event_t* e) {
        auto* self = (LvNotesEditScreen*)lv_event_get_user_data(e);
        self->lockToggleClicked();
    }, LV_EVENT_CLICKED, this);
    _lockBtnX1 = kLockX;
    _lockBtnY1 = kBackSaveY + Theme::STATUS_BAR_H;
    _lockBtnX2 = kLockX + kLockW - 1;
    _lockBtnY2 = kBackSaveY + kBackSaveH - 1 + Theme::STATUS_BAR_H;

    // SAVE (top-right)
    _saveBtn = lv_btn_create(parent);
    lv_obj_set_size(_saveBtn, kSaveW, kBackSaveH);
    lv_obj_set_pos(_saveBtn, kSaveX, kBackSaveY);
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
        self->startSave();
    }, LV_EVENT_CLICKED, this);
    _saveBtnX1 = kSaveX;
    _saveBtnY1 = kBackSaveY + Theme::STATUS_BAR_H;
    _saveBtnX2 = kSaveX + kSaveW - 1;
    _saveBtnY2 = kBackSaveY + kBackSaveH - 1 + Theme::STATUS_BAR_H;

    // Centered filename / status text. Width accommodates BACK + LOCK
    // + SAVE plus gaps.
    _headerLbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_headerLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_headerLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_headerLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_headerLbl, kLockX - (kBackX + kBackW) - 8);
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

    // Initial mode = plain. paintLockPill() is called from setFilename
    // and lockToggleClicked(); we draw the initial state here too.
    paintHeader();
    loadFromSD();
}

void LvNotesEditScreen::destroyUI() {
    wipePassBuf();
    hideModal();

    _backBtn = nullptr; _backLbl = nullptr;
    _saveBtn = nullptr;  _saveLbl = nullptr;
    _lockBtn = nullptr;  _lockLbl = nullptr;
    _headerLbl = nullptr;
    _textarea = nullptr;
    _loaded = false;
    LvScreen::destroyUI();
}

void LvNotesEditScreen::setFilename(const String& filename) {
    _filename = filename;
    _loaded = false;
    // If the file is encrypted, the editor forces LOCKED mode and the
    // toggle is hidden — opening a .note.enc should never offer the
    // user a "PLAIN" option that would re-save the file unencrypted
    // and lose its protection. Body remains empty until the user
    // supplies a matching passphrase in the modal.
    bool fileLocked = endsWithNoteEnc(_filename.c_str());
    if (fileLocked) {
        _locked = true;
    } else if (_filename.length() > 0) {
        // Plain text existing file → open in PLAIN mode. (A user who
        // wants to re-save it encrypted can tap LOCK before saving.)
        _locked = false;
    }
    paintHeader();
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
        lv_label_set_text(_headerLbl, _locked ? "New locked note" : "New note");
    } else {
        lv_label_set_text(_headerLbl, _filename.c_str());
    }

    if (_lockBtn && _lockLbl) {
        if (_locked) {
            lv_label_set_text(_lockLbl, "LOCK");
            lv_obj_set_style_border_color(_lockBtn, lv_color_hex(Theme::PRIMARY), 0);
            lv_obj_set_style_text_color(_lockLbl, lv_color_hex(Theme::PRIMARY), 0);
            // For an existing encrypted file, the toggle is read-only.
            // We hide the pill so the user has a clear visual cue that
            // the mode is forced; lockToggleClicked() also rejects
            // any clicks defensively.
            bool isExistingEnc = (_filename.length() > 0) && endsWithNoteEnc(_filename.c_str());
            if (isExistingEnc) {
                lv_obj_add_flag(_lockBtn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(_lockBtn, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(_lockLbl, "PLAIN");
            lv_obj_set_style_border_color(_lockBtn, lv_color_hex(Theme::BORDER), 0);
            lv_obj_set_style_text_color(_lockLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
            lv_obj_clear_flag(_lockBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LvNotesEditScreen::lockToggleClicked() {
    // Reject toggle while modal is up (defensive — pill is hidden
    // behind the modal in z-order anyway).
    if (_modalMode != ModalMode::None) return;
    // Reject toggle on existing encrypted files (defensive; UI also
    // hides the pill).
    if (_filename.length() > 0 && endsWithNoteEnc(_filename.c_str())) return;
    _locked = !_locked;
    paintHeader();
    if (_ui) _ui->lvStatusBar().showToast(_locked ? "Mode: locked" : "Mode: plain", 900);
}

void LvNotesEditScreen::loadFromSD() {
    if (!_textarea || _loaded) return;

    // New note (no filename) — leave textarea blank.
    if (_filename.length() == 0) {
        _loaded = true;
        return;
    }
    if (!_sd || !_sd->isReady()) {
        if (_headerLbl) lv_label_set_text(_headerLbl, "No SD card");
        _loaded = true;
        return;
    }

    String path = String("/Files/notes/") + _filename;

    if (endsWithNoteEnc(_filename.c_str())) {
        // Encrypted file: do NOT load plaintext. Surface a hint in
        // the header so the user knows the body is gated, and let
        // them trigger the unlock via tapping the LOCK pill (which is
        // hidden, so the practical trigger is just hitting the SAVE
        // button... we instead auto-show the prompt on entry).
        if (_headerLbl) lv_label_set_text(_headerLbl, "Locked — enter passphrase");
        // Auto-prompt immediately so the user doesn't have to guess
        // what to do.
        if (_modalMode == ModalMode::None) {
            showModal("Unlock note",
                      "Enter passphrase to decrypt.",
                      /*passwordMode=*/true,
                      "UNLOCK");
            _modalMode = ModalMode::OpenEncrypted;
        }
        _loaded = true;
        return;
    }

    // Plain file — read text.
    String contents = _sd->readString(path.c_str());
    if ((int)contents.length() > MAX_BODY_LEN) {
        contents = contents.substring(0, MAX_BODY_LEN);
    }
    applyLoadedBody(contents);
}

void LvNotesEditScreen::applyLoadedBody(const String& body) {
    if (!_textarea) return;
    lv_textarea_set_text(_textarea, body.c_str());
    lv_textarea_set_cursor_pos(_textarea, LV_TEXTAREA_CURSOR_LAST);
    _loaded = true;
}

void LvNotesEditScreen::tryOpenDecrypted(const char* pass, size_t passLen) {
    if (!_sd || !_sd->isReady() || !pass || passLen == 0) {
        if (_ui) _ui->lvStatusBar().showToast("Open failed", 1500);
        return;
    }
    String path = String("/Files/notes/") + _filename;
    // Heap buffers — stack ~8KB (blob+pt) overflowed the LVGL task and
    // hard-faulted on unlock. FileCrypto::decrypt enforces its own bounds.
    const size_t blobCap = MAX_BODY_LEN + FileCrypto::HEADER_SIZE + FileCrypto::TAG_SIZE;
    uint8_t* blob = (uint8_t*)malloc(blobCap);
    uint8_t* pt = (uint8_t*)malloc(MAX_BODY_LEN);
    if (!blob || !pt) {
        free(blob);
        free(pt);
        if (_ui) _ui->lvStatusBar().showToast("Out of memory", 1500);
        return;
    }
    size_t bytesRead = 0;
    if (!_sd->readFile(path.c_str(), blob, blobCap, bytesRead)) {
        FileCrypto::wipeSensitive(blob, blobCap);
        free(blob);
        free(pt);
        if (_ui) _ui->lvStatusBar().showToast("Read failed", 1500);
        return;
    }

    size_t ptLen = 0;
    bool ok = FileCrypto::decrypt(pass, passLen,
                                  blob, bytesRead,
                                  pt, MAX_BODY_LEN, &ptLen);
    FileCrypto::wipeSensitive(blob, blobCap);
    free(blob);
    blob = nullptr;
    if (!ok) {
        FileCrypto::wipeSensitive(pt, MAX_BODY_LEN);
        free(pt);
        if (_modal && _modalMode == ModalMode::OpenEncrypted) {
            setModalError("Wrong passphrase");
        } else if (_ui) {
            _ui->lvStatusBar().showToast("Wrong passphrase", 1500);
        }
        return;
    }

    // Convert decrypted bytes to String for the textarea.
    while (ptLen > 0 && pt[ptLen - 1] == 0) ptLen--;
    String body;
    body.reserve(ptLen + 1);
    for (size_t i = 0; i < ptLen; ++i) {
        body += (char)(pt[i] == 0 ? ' ' : pt[i]);
    }
    FileCrypto::wipeSensitive(pt, MAX_BODY_LEN);
    free(pt);

    if ((int)body.length() > MAX_BODY_LEN) {
        body = body.substring(0, MAX_BODY_LEN);
    }
    applyLoadedBody(body);
    if (_headerLbl) lv_label_set_text(_headerLbl, _filename.c_str());
}

// =============================================================================
// Modal — passphrase prompt
// =============================================================================

void LvNotesEditScreen::buildModal() {
    if (_modal) return;
    _modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_modal, kModalW, kModalH);
    lv_obj_center(_modal);
    lv_obj_add_style(_modal, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_modal, 8, 0);
    lv_obj_set_style_pad_row(_modal, 2, 0);
    lv_obj_set_style_border_width(_modal, 1, 0);
    lv_obj_clear_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);
    // Block clicks from passing through to widgets behind the modal.
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_CLICKABLE);

    _modalTitle = lv_label_create(_modal);
    lv_obj_set_style_text_font(_modalTitle, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_modalTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_set_width(_modalTitle, kModalW - 16);
    lv_obj_set_pos(_modalTitle, 8, kModalTitleY);
    lv_label_set_long_mode(_modalTitle, LV_LABEL_LONG_CLIP);

    _modalHint = lv_label_create(_modal);
    lv_obj_set_style_text_font(_modalHint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_modalHint, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_obj_set_width(_modalHint, kModalW - 16);
    lv_obj_set_pos(_modalHint, 8, kModalHintY);
    lv_label_set_long_mode(_modalHint, LV_LABEL_LONG_WRAP);

    _modalPass = lv_textarea_create(_modal);
    lv_obj_set_size(_modalPass, kModalW - 16, kModalPassH);
    lv_obj_set_pos(_modalPass, 8, kModalPassY);
    lv_obj_add_style(_modalPass, LvTheme::styleTextarea(), 0);
    lv_obj_add_style(_modalPass, LvTheme::styleTextareaFocused(), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_modalPass, &lv_font_rsdeck_12, 0);
    lv_textarea_set_one_line(_modalPass, true);
    lv_textarea_set_max_length(_modalPass, MAX_PASS_LEN - 1);
    // Bullets hide what the user types — same default LVGL uses.
    lv_textarea_set_password_mode(_modalPass, true);
    lv_textarea_set_placeholder_text(_modalPass, "Passphrase");

    _modalError = lv_label_create(_modal);
    lv_obj_set_style_text_font(_modalError, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_modalError, lv_color_hex(Theme::ERROR_CLR), 0);
    lv_obj_set_width(_modalError, kModalW - 16);
    lv_obj_set_pos(_modalError, 8, kModalErrY);
    lv_label_set_long_mode(_modalError, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_modalError, "");

    // OK button — right. We patch the label per-mode in showModal().
    _modalOkBtn = lv_btn_create(_modal);
    lv_obj_set_size(_modalOkBtn, kModalBtnW, kModalBtnH);
    lv_obj_set_pos(_modalOkBtn, kModalW - kModalBtnW - 8, kModalBtnY);
    lv_obj_add_style(_modalOkBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_modalOkBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(_modalOkBtn, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_border_width(_modalOkBtn, 1, 0);
    lv_obj_set_style_radius(_modalOkBtn, 3, 0);
    lv_obj_set_style_pad_all(_modalOkBtn, 0, 0);
    lv_obj_set_style_shadow_width(_modalOkBtn, 0, 0);
    lv_obj_t* okLbl = lv_label_create(_modalOkBtn);
    lv_obj_set_style_text_font(okLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(okLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(okLbl, "OK");
    lv_obj_center(okLbl);
    lv_obj_add_event_cb(_modalOkBtn, [](lv_event_t* e) {
        auto* self = (LvNotesEditScreen*)lv_event_get_user_data(e);
        self->handleModalOk();
    }, LV_EVENT_CLICKED, this);

    _modalCancelBtn = lv_btn_create(_modal);
    lv_obj_set_size(_modalCancelBtn, kModalBtnW, kModalBtnH);
    lv_obj_set_pos(_modalCancelBtn, 8, kModalBtnY);
    lv_obj_add_style(_modalCancelBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_modalCancelBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(_modalCancelBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_modalCancelBtn, 1, 0);
    lv_obj_set_style_radius(_modalCancelBtn, 3, 0);
    lv_obj_set_style_pad_all(_modalCancelBtn, 0, 0);
    lv_obj_set_style_shadow_width(_modalCancelBtn, 0, 0);
    lv_obj_t* cancelLbl = lv_label_create(_modalCancelBtn);
    lv_obj_set_style_text_font(cancelLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(cancelLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_label_set_text(cancelLbl, "CANCEL");
    lv_obj_center(cancelLbl);
    lv_obj_add_event_cb(_modalCancelBtn, [](lv_event_t* e) {
        auto* self = (LvNotesEditScreen*)lv_event_get_user_data(e);
        self->handleModalCancel();
    }, LV_EVENT_CLICKED, this);

    lv_obj_add_flag(_modal, LV_OBJ_FLAG_HIDDEN);
}

void LvNotesEditScreen::showModal(const char* title, const char* hint,
                                  bool passwordMode, const char* confirmText) {
    if (!_modal) buildModal();
    if (!_modal) return;

    lv_label_set_text(_modalTitle, title ? title : "");
    lv_label_set_text(_modalHint, hint ? hint : "");
    lv_label_set_text(_modalError, "");
    lv_textarea_set_text(_modalPass, "");
    lv_textarea_set_password_mode(_modalPass, passwordMode);

    // Re-label OK button if caller asked for one (e.g. "UNLOCK").
    if (confirmText && _modalOkBtn) {
        lv_obj_t* lbl = lv_obj_get_child(_modalOkBtn, 0);
        if (lbl) lv_label_set_text(lbl, confirmText);
    }

    lv_obj_clear_flag(_modal, LV_OBJ_FLAG_HIDDEN);
    // Move keyboard focus to the password field so the user can just
    // start typing.
    if (_modalPass) {
        lv_group_add_obj(LvInput::group(), _modalPass);
        lv_group_focus_obj(_modalPass);
    }
}

void LvNotesEditScreen::hideModal() {
    if (!_modal) return;
    // Remove the password textarea from the LVGL group so keyboard
    // focus returns to the main editor textarea. Without this, the
    // next keypress would still target the hidden modal widget and
    // nothing would happen.
    if (_modalPass) lv_group_remove_obj(_modalPass);
    if (_textarea)  lv_group_focus_obj(_textarea);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_HIDDEN);
    _modalMode = ModalMode::None;
}

void LvNotesEditScreen::setModalError(const char* msg) {
    if (!_modalError) return;
    lv_label_set_text(_modalError, msg ? msg : "");
}

void LvNotesEditScreen::wipePassBuf() {
    if (_passBufActive) {
        FileCrypto::wipeSensitive(_passBuf, sizeof(_passBuf));
        _passBufActive = false;
    }
    // Also clear the modal textarea if it exists — passphrase chars
    // live in the LVGL widget too.
    if (_modalPass) lv_textarea_set_text(_modalPass, "");
}

// =============================================================================
// State machine
// =============================================================================

void LvNotesEditScreen::startSave() {
    if (!_onSave) return;
    if (!_ui) return;
    if (!_sd || !_sd->isReady()) {
        _ui->lvStatusBar().showToast("No SD card", 1500);
        return;
    }
    if (!_locked) {
        // Plain path — no passphrase. Hand the body to the caller
        // immediately. We do NOT call body() here; main.cpp re-reads
        // it after generating the timestamp name so the round-trip
        // includes any post-validation edits. To preserve that
        // contract we invoke _onSave with empty passphrase and the
        // current body.
        if (_validator && !_validator(body())) {
            _ui->lvStatusBar().showToast("Invalid note", 1500);
            return;
        }
        _onSave(body(), /*locked=*/false, /*pass=*/nullptr, /*passLen=*/0);
        return;
    }
    // Locked → enter passphrase, then confirm.
    _passBufActive = false;
    wipePassBuf();
    showModal("Encrypt note",
              "Enter passphrase (min 4 chars).",
              /*passwordMode=*/true,
              "NEXT");
    _modalMode = ModalMode::SaveEnter;
}

void LvNotesEditScreen::handleModalOk() {
    if (!_modalPass) return;
    // Swallow the Enter/OK that just advanced SaveEnter→SaveConfirm so
    // an empty confirm is not submitted on the same keypress.
    if (_modalIgnoreOk) {
        _modalIgnoreOk = false;
        return;
    }
    const char* text = lv_textarea_get_text(_modalPass);
    size_t len = text ? strlen(text) : 0;

    switch (_modalMode) {
    case ModalMode::OpenEncrypted: {
        if (len == 0) {
            setModalError("Passphrase required");
            return;
        }
        tryOpenDecrypted(text, len);
        // Clear the textarea — the passphrase shouldn't sit in LVGL's
        // buffer any longer than necessary.
        lv_textarea_set_text(_modalPass, "");
        // tryOpenDecrypted will either succeed (body loaded) or show
        // "Wrong passphrase" via setModalError. We only close the
        // modal on success.
        if (_loaded && _textarea) {
            String b = body();
            if (b.length() > 0) {
                hideModal();
            }
        }
        return;
    }
    case ModalMode::SaveEnter: {
        if (len < 4) {
            setModalError("Min 4 characters");
            return;
        }
        // Stash into _passBuf for the confirm step. Do NOT call
        // wipePassBuf() first — it clears _passBufActive and would
        // race if showModal ever wiped the stash.
        size_t copy = (len < (size_t)(MAX_PASS_LEN - 1)) ? len : (size_t)(MAX_PASS_LEN - 1);
        memcpy(_passBuf, text, copy);
        _passBuf[copy] = '\0';
        _passBufActive = true;
        showModal("Confirm passphrase",
                  "Re-enter to confirm.",
                  /*passwordMode=*/true,
                  "SAVE");
        _modalMode = ModalMode::SaveConfirm;
        _modalIgnoreOk = true;  // drop residual Enter from NEXT
        return;
    }
    case ModalMode::SaveConfirm: {
        if (!_passBufActive) {
            // Should not happen — defensive: fall back to SaveEnter.
            showModal("Encrypt note", "Enter passphrase (min 4 chars).",
                      true, "NEXT");
            _modalMode = ModalMode::SaveEnter;
            return;
        }
        // Empty confirm (often a double-Enter) — stay on confirm, don't loop.
        if (len == 0) {
            setModalError("Re-enter passphrase");
            return;
        }
        size_t confirmLen = len;
        size_t storedLen = strlen(_passBuf);
        if (confirmLen != storedLen || memcmp(_passBuf, text, storedLen) != 0) {
            setModalError("Doesn't match — try again");
            // Restart at SaveEnter with a fresh prompt.
            wipePassBuf();
            showModal("Encrypt note", "Enter passphrase (min 4 chars).",
                      true, "NEXT");
            _modalMode = ModalMode::SaveEnter;
            return;
        }
        // Match — fire save callback. main.cpp will pick the filename
        // and do the encryption + write. We hide the modal first so
        // it doesn't block the "Saved" toast.
        String savedBody = body();
        if (_validator && !_validator(savedBody)) {
            wipePassBuf();
            hideModal();
            if (_ui) _ui->lvStatusBar().showToast("Invalid note", 1500);
            return;
        }
        hideModal();
        // Hand off the passphrase by VALUE so main.cpp can use it
        // without depending on our internal buffer (which we wipe
        // right after). The callback is responsible for not logging
        // it; we document this in the header.
        if (_onSave) _onSave(savedBody, /*locked=*/true, _passBuf, storedLen);
        wipePassBuf();
        return;
    }
    case ModalMode::None:
    default:
        return;
    }
}

void LvNotesEditScreen::handleModalCancel() {
    wipePassBuf();
    hideModal();
    // The body textarea remains in its current state (empty for
    // encrypted files, populated for plain ones). For a still-locked
    // existing encrypted file, the header text says "Locked — enter
    // passphrase" so the user can re-trigger via... we also leave the
    // body empty. They can press BACK to leave the screen or retap
    // BACK then re-open the file. Simpler than a "retry" button on
    // a 320x240 surface.
    if (_filename.length() > 0 && endsWithNoteEnc(_filename.c_str())) {
        if (_headerLbl) lv_label_set_text(_headerLbl, "Locked — cancel");
    }
}

bool LvNotesEditScreen::handleKey(const KeyEvent& event) {
    if (!_textarea) return false;

    // ---- Modal open → route keys to the modal password textarea ----
    if (_modalMode != ModalMode::None && _modalPass) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            handleModalOk();
            return true;
        }
        if (event.character == 0x1B) {  // Esc
            handleModalCancel();
            return true;
        }
        if (event.del || event.character == 0x08) {
            lv_textarea_del_char(_modalPass);
            return true;
        }
        if (event.character >= 0x20 && event.character <= 0x7E) {
            _modalIgnoreOk = false;  // user typed — OK is live again
            char buf[2] = {event.character, 0};
            lv_textarea_add_text(_modalPass, buf);
            return true;
        }
        return false;
    }

    // ---- Normal editor ----
    // Esc / back → list. We don't currently warn about unsaved changes
    // (MVP — discard is acceptable). Wipe pass buffer first as a
    // belt-and-suspenders against any straggler state.
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        wipePassBuf();
        if (_onBack) _onBack();
        return true;
    }

    // Ctrl+S / Cmd+S → save.
    if (event.character == 0x13 || event.character == 0x19) {
        startSave();
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
        return false;  // Let LVGL handle
    }

    return false;
}

bool LvNotesEditScreen::handleLongPress() {
    // No long-press action in the editor — global default (screen off)
    // stays.
    return false;
}
