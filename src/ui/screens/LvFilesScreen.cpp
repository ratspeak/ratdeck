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
constexpr lv_coord_t kBackH = kHeaderH - 4;  // 18

constexpr lv_coord_t kRowGap  = 2;
constexpr lv_coord_t kRowH    = 28;
constexpr lv_coord_t kRowPadX = 8;

constexpr const char* kRoot = "/Files";

// App-mode content height (tab bar hidden).
constexpr lv_coord_t kAppContentH = Theme::SCREEN_H - Theme::STATUS_BAR_H;  // 220

static char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool hasSuffixI(const char* s, const char* suf) {
    if (!s || !suf) return false;
    size_t ls = strlen(s);
    size_t lf = strlen(suf);
    if (lf > ls) return false;
    const char* tail = s + (ls - lf);
    for (size_t i = 0; i < lf; i++) {
        if (asciiLower(tail[i]) != asciiLower(suf[i])) return false;
    }
    return true;
}

static bool safeName(const char* name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strchr(name, '/') != nullptr) return false;
    if (strchr(name, '\\') != nullptr) return false;
    return true;
}

static const char* basenameOf(const char* raw) {
    if (!raw || !raw[0]) return "";
    const char* slash = strrchr(raw, '/');
    return (slash && slash[1]) ? (slash + 1) : raw;
}

static void formatSize(char* buf, size_t cap, uint32_t sz) {
    if (sz < 1024u) {
        snprintf(buf, cap, "%uB", (unsigned)sz);
    } else if (sz < 1024u * 1024u) {
        snprintf(buf, cap, "%uK", (unsigned)((sz + 512u) / 1024u));
    } else {
        snprintf(buf, cap, "%uM", (unsigned)((sz + 512u * 1024u) / (1024u * 1024u)));
    }
}

}  // namespace

bool LvFilesScreen::isReadableName(const char* name) {
    return hasSuffixI(name, ".txt") || hasSuffixI(name, ".md");
}

void LvFilesScreen::resetToRoot() {
    strncpy(_cwd, kRoot, sizeof(_cwd) - 1);
    _cwd[sizeof(_cwd) - 1] = 0;
}

bool LvFilesScreen::pathUnderRoot(const char* path) const {
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

bool LvFilesScreen::isRootCwd() const {
    return strcmp(_cwd, kRoot) == 0;
}

bool LvFilesScreen::joinUnderRoot(char* out, size_t cap, const char* base,
                                  const char* name) const {
    if (!out || cap == 0 || !base || !safeName(name)) return false;
    char tmp[PATH_MAX_LEN];
    int n = snprintf(tmp, sizeof(tmp), "%s/%s", base, name);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;
    if (!pathUnderRoot(tmp)) return false;
    strncpy(out, tmp, cap - 1);
    out[cap - 1] = 0;
    return true;
}

bool LvFilesScreen::parentOf(const char* path, char* out, size_t cap) const {
    if (!path || !out || cap == 0) return false;
    if (!pathUnderRoot(path)) return false;
    if (strcmp(path, kRoot) == 0) {
        strncpy(out, kRoot, cap - 1);
        out[cap - 1] = 0;
        return true;
    }
    const char* slash = strrchr(path, '/');
    if (!slash || slash == path) return false;
    size_t len = (size_t)(slash - path);
    if (len == 0 || len >= cap) return false;
    if (len < strlen(kRoot)) {
        strncpy(out, kRoot, cap - 1);
        out[cap - 1] = 0;
        return true;
    }
    memcpy(out, path, len);
    out[len] = 0;
    if (!pathUnderRoot(out)) {
        strncpy(out, kRoot, cap - 1);
        out[cap - 1] = 0;
    }
    return true;
}

void LvFilesScreen::createUI(lv_obj_t* parent) {
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
        auto* self = (LvFilesScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);

    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_set_style_text_letter_space(_title, 1, 0);
    lv_label_set_text(_title, "FILES");
    lv_obj_set_pos(_title, kBackX + kBackW + 8, kBackY + 1);

    _crumb = lv_label_create(parent);
    lv_obj_set_style_text_font(_crumb, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_crumb, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_long_mode(_crumb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_crumb, 120);
    lv_obj_set_style_text_align(_crumb, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(_crumb, Theme::CONTENT_W - 124, kBackY + 2);
    lv_label_set_text(_crumb, "/");

    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, Theme::CONTENT_W, kAppContentH - kMapY);
    lv_obj_set_pos(_list, 0, kMapY);
    lv_obj_set_style_bg_color(_list, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_gap(_list, kRowGap, 0);
    lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_SCROLLABLE);

    _emptyState = nullptr;
    _ents.clear();
    _hasParent = false;
}

void LvFilesScreen::destroyUI() {
    _backBtn = nullptr;
    _title = nullptr;
    _crumb = nullptr;
    _list = nullptr;
    _emptyState = nullptr;
    _ents.clear();
    LvScreen::destroyUI();
}

void LvFilesScreen::onEnter() {
    if (!pathUnderRoot(_cwd)) resetToRoot();
    rebuildList();
}

void LvFilesScreen::refreshUI() {
    // Static list — rebuild on entry only.
}

void LvFilesScreen::enterDir(const char* name) {
    char next[PATH_MAX_LEN];
    if (name && strcmp(name, "..") == 0) {
        if (!parentOf(_cwd, next, sizeof(next))) return;
        strncpy(_cwd, next, sizeof(_cwd) - 1);
        _cwd[sizeof(_cwd) - 1] = 0;
        rebuildList();
        return;
    }
    if (!joinUnderRoot(next, sizeof(next), _cwd, name)) return;
    if (!_sd || !_sd->exists(next)) {
        if (_ui) _ui->lvStatusBar().showToast("Missing", 1200);
        return;
    }
    strncpy(_cwd, next, sizeof(_cwd) - 1);
    _cwd[sizeof(_cwd) - 1] = 0;
    rebuildList();
}

void LvFilesScreen::onRowTap(int idx) {
    // idx: 0 = ".." if _hasParent, else first entry.
    if (_hasParent && idx == 0) {
        enterDir("..");
        return;
    }
    int ei = _hasParent ? (idx - 1) : idx;
    if (ei < 0 || ei >= (int)_ents.size()) return;
    const Ent& e = _ents[ei];
    if (e.isDir) {
        enterDir(e.name.c_str());
        return;
    }
    if (!isReadableName(e.name.c_str())) {
        if (_ui) _ui->lvStatusBar().showToast("Unsupported", 1500);
        return;
    }
    char abs[PATH_MAX_LEN];
    if (!joinUnderRoot(abs, sizeof(abs), _cwd, e.name.c_str())) {
        if (_ui) _ui->lvStatusBar().showToast("Bad path", 1200);
        return;
    }
    if (_onOpenFile) _onOpenFile(String(abs));
}

void LvFilesScreen::rebuildList() {
    if (!_list) return;
    _ents.clear();
    _hasParent = false;

    lv_obj_clean(_list);
    _emptyState = nullptr;

    if (!_sd) {
        showEmptyState(/*noSd=*/true, /*empty=*/false);
        return;
    }
    _sdReady = _sd->isReady();
    if (!_sdReady) {
        showEmptyState(/*noSd=*/true, /*empty=*/false);
        return;
    }

    _sd->ensureDir(kRoot);
    if (!pathUnderRoot(_cwd)) resetToRoot();

    // Crumb: relative to /Files
    {
        const char* show = _cwd;
        if (strncmp(show, kRoot, 6) == 0) {
            show = show + 6;
            if (show[0] == '/') show++;
            if (!show[0]) show = "/";
        }
        char crumb[40];
        snprintf(crumb, sizeof(crumb), "%.36s", show);
        if (_crumb) lv_label_set_text(_crumb, crumb);
    }

    _hasParent = !isRootCwd();

    File dir = _sd->openDir(_cwd);
    if ((bool)dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && (int)_ents.size() < MAX_ENTRIES) {
            const char* base = basenameOf(f.name());
            if (base[0] && strcmp(base, ".") != 0 && strcmp(base, "..") != 0 &&
                safeName(base)) {
                Ent e;
                e.name = String(base);
                e.isDir = f.isDirectory();
                e.size = e.isDir ? 0 : (uint32_t)f.size();
                _ents.push_back(e);
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    // Sort: dirs first, then alpha (insertion sort; n ≤ 64).
    for (size_t i = 1; i < _ents.size(); ++i) {
        Ent cur = _ents[i];
        int j = (int)i - 1;
        while (j >= 0) {
            bool curFirst = false;
            if (cur.isDir != _ents[j].isDir) {
                curFirst = cur.isDir;
            } else {
                curFirst = cur.name.equalsIgnoreCase(_ents[j].name)
                               ? false
                               : (strcasecmp(cur.name.c_str(), _ents[j].name.c_str()) < 0);
            }
            if (!curFirst) break;
            _ents[j + 1] = _ents[j];
            j--;
        }
        _ents[j + 1] = cur;
    }

    if (_ents.empty() && !_hasParent) {
        showEmptyState(/*noSd=*/false, /*empty=*/true);
        return;
    }

    auto addRow = [&](const char* label, const char* right, bool isDirRow, int userIdx) {
        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, Theme::CONTENT_W, kRowH);
        lv_obj_set_style_bg_color(row, lv_color_hex(Theme::BG_ELEVATED), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)userIdx);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(
            isDirRow ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, Theme::CONTENT_W - 80);
        lv_obj_set_pos(lbl, kRowPadX, (kRowH - 14) / 2);
        lv_label_set_text(lbl, label);

        if (right && right[0]) {
            lv_obj_t* r = lv_label_create(row);
            lv_obj_set_style_text_font(r, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(r, lv_color_hex(Theme::TEXT_MUTED), 0);
            lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(r, 56);
            lv_obj_set_pos(r, Theme::CONTENT_W - 64, (kRowH - 12) / 2);
            lv_label_set_text(r, right);
        }

        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvFilesScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->onRowTap(idx);
        }, LV_EVENT_CLICKED, this);
    };

    int rowIdx = 0;
    if (_hasParent) {
        addRow("../", nullptr, true, rowIdx++);
    }
    for (size_t i = 0; i < _ents.size(); ++i) {
        const Ent& e = _ents[i];
        char left[80];
        char right[12] = {0};
        if (e.isDir) {
            snprintf(left, sizeof(left), "[D] %s/", e.name.c_str());
        } else {
            snprintf(left, sizeof(left), "%s", e.name.c_str());
            formatSize(right, sizeof(right), e.size);
        }
        addRow(left, e.isDir ? nullptr : right, e.isDir, rowIdx++);
    }
}

void LvFilesScreen::showEmptyState(bool noSd, bool empty) {
    if (!_list) return;
    _emptyState = lv_obj_create(_list);
    lv_obj_set_size(_emptyState, Theme::CONTENT_W, 80);
    lv_obj_set_style_bg_opa(_emptyState, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_emptyState, 0, 0);
    lv_obj_set_style_pad_all(_emptyState, 0, 0);
    lv_obj_clear_flag(_emptyState, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(_emptyState);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(title, noSd ? "No SD card" : (empty ? "Empty" : "No files"));

    lv_obj_t* hint = lv_label_create(_emptyState);
    lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(hint, noSd
        ? "Insert an SD card"
        : "Drop files in /Files");
}

bool LvFilesScreen::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) _onBack();
        return true;
    }
    return false;
}
