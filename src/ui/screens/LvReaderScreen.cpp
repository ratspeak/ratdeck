#include "LvReaderScreen.h"
#include "LvFilesScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "storage/SDStore.h"
#include <Arduino.h>
#include <SD.h>
#include "fonts/fonts.h"

namespace {

constexpr lv_coord_t kHeaderH   = 22;
constexpr lv_coord_t kHeaderPad = 2;
constexpr lv_coord_t kMapY      = kHeaderH + kHeaderPad;  // 24

constexpr lv_coord_t kBackX = 4;
constexpr lv_coord_t kBackY = 2;
constexpr lv_coord_t kBackW = 64;
constexpr lv_coord_t kBackH = kHeaderH - 4;

constexpr lv_coord_t kMetaH = 14;
constexpr lv_coord_t kAppContentH = Theme::SCREEN_H - Theme::STATUS_BAR_H;  // 220

constexpr const char* kRoot = "/Files";

static const char* basenameOf(const char* path) {
    if (!path || !path[0]) return "READER";
    const char* slash = strrchr(path, '/');
    return (slash && slash[1]) ? (slash + 1) : path;
}

}  // namespace

bool LvReaderScreen::pathUnderRoot(const char* path) const {
    if (!path || path[0] != '/') return false;
    size_t rl = strlen(kRoot);
    if (strncmp(path, kRoot, rl) != 0) return false;
    if (path[rl] != 0 && path[rl] != '/') return false;
    const char* p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == 0) &&
            (p == path || p[-1] == '/')) {
            return false;
        }
        p++;
    }
    return true;
}

void LvReaderScreen::close() {
    if (_body) {
        free(_body);
        _body = nullptr;
    }
    _bodyLen = 0;
    _truncated = false;
    _loaded = false;
}

void LvReaderScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_t* backLbl = lv_label_create(_backBtn);
    lv_obj_set_style_text_font(backLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(backLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(backLbl, "< BACK");
    lv_obj_center(backLbl);
    lv_obj_add_event_cb(_backBtn, [](lv_event_t* e) {
        auto* self = (LvReaderScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);

    _titleLbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_titleLbl, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_titleLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_titleLbl, Theme::CONTENT_W - kBackW - 16);
    lv_obj_set_pos(_titleLbl, kBackX + kBackW + 8, kBackY + 1);
    lv_label_set_text(_titleLbl, "READER");

    // Body container — scrollable, full remaining height minus meta strip.
    const lv_coord_t bodyH = kAppContentH - kMapY - kMetaH;
    _bodyCont = lv_obj_create(parent);
    lv_obj_set_size(_bodyCont, Theme::CONTENT_W, bodyH);
    lv_obj_set_pos(_bodyCont, 0, kMapY);
    lv_obj_set_style_bg_color(_bodyCont, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_bodyCont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bodyCont, 0, 0);
    lv_obj_set_style_pad_left(_bodyCont, 8, 0);
    lv_obj_set_style_pad_right(_bodyCont, 8, 0);
    lv_obj_set_style_pad_top(_bodyCont, 4, 0);
    lv_obj_set_style_pad_bottom(_bodyCont, 8, 0);
    lv_obj_set_scroll_dir(_bodyCont, LV_DIR_VER);
    lv_obj_add_flag(_bodyCont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_bodyCont, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    _bodyLbl = lv_label_create(_bodyCont);
    lv_obj_set_style_text_font(_bodyLbl, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_bodyLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_obj_set_width(_bodyLbl, Theme::CONTENT_W - 16);
    lv_label_set_long_mode(_bodyLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_bodyLbl, "");

    _metaLbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_metaLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_metaLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_obj_set_style_text_align(_metaLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_metaLbl, Theme::CONTENT_W);
    lv_obj_set_pos(_metaLbl, 0, kAppContentH - kMetaH);
    lv_label_set_text(_metaLbl, "");
}

void LvReaderScreen::destroyUI() {
    close();
    _backBtn = nullptr;
    _titleLbl = nullptr;
    _bodyCont = nullptr;
    _bodyLbl = nullptr;
    _metaLbl = nullptr;
    LvScreen::destroyUI();
}

void LvReaderScreen::onEnter() {
    loadBody();
}

void LvReaderScreen::onExit() {
    close();
}

void LvReaderScreen::refreshUI() {
    // Static once loaded.
}

void LvReaderScreen::showError(const char* msg) {
    if (_bodyLbl) lv_label_set_text(_bodyLbl, msg ? msg : "Error");
    if (_metaLbl) lv_label_set_text(_metaLbl, "");
    _loaded = false;
}

bool LvReaderScreen::loadBody() {
    close();

    const char* path = _path.c_str();
    if (!path || !path[0] || !pathUnderRoot(path)) {
        showError("Bad path");
        return false;
    }

    const char* base = basenameOf(path);
    if (!LvFilesScreen::isReadableName(base)) {
        showError("Unsupported");
        if (_titleLbl) lv_label_set_text(_titleLbl, base);
        return false;
    }
    if (_titleLbl) lv_label_set_text(_titleLbl, base);

    if (!_sd || !_sd->isReady()) {
        showError("No SD card");
        return false;
    }

    File f = _sd->openFile(path);
    if (!(bool)f) {
        showError("Missing");
        return false;
    }

    size_t fsz = f.size();
    // Prefer PSRAM for the body buffer.
#if defined(BOARD_HAS_PSRAM)
    _body = (char*)ps_malloc(BODY_CAP);
#else
    _body = (char*)malloc(BODY_CAP);
#endif
    if (!_body) {
        f.close();
        showError("No memory");
        return false;
    }

    // Leave 1 byte for NUL. Cap read at BODY_CAP - 1.
    const size_t maxRead = BODY_CAP - 1;
    size_t toRead = fsz > maxRead ? maxRead : fsz;
    size_t got = f.read((uint8_t*)_body, toRead);
    f.close();

    _bodyLen = got;
    _body[got] = 0;
    _truncated = (fsz > maxRead);

    // Sanitize: NULs → space (LVGL labels are C-string based).
    for (size_t i = 0; i < _bodyLen; i++) {
        if (_body[i] == 0) _body[i] = ' ';
        // Strip CR for cleaner wrap.
        if (_body[i] == '\r') _body[i] = ' ';
    }

    if (_bodyLbl) lv_label_set_text(_bodyLbl, _body);

    if (_metaLbl) {
        char meta[48];
        if (_truncated) {
            snprintf(meta, sizeof(meta), "%u KB shown (truncated)",
                     (unsigned)((_bodyLen + 512) / 1024));
        } else if (_bodyLen < 1024) {
            snprintf(meta, sizeof(meta), "%u B", (unsigned)_bodyLen);
        } else {
            snprintf(meta, sizeof(meta), "%u KB",
                     (unsigned)((_bodyLen + 512) / 1024));
        }
        lv_label_set_text(_metaLbl, meta);
    }

    _loaded = true;
    return true;
}

bool LvReaderScreen::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) _onBack();
        return true;
    }
    return false;
}
