#include "LvNotesListScreen.h"
#include "ui/Theme.h"
#include "ui/LvTheme.h"
#include "ui/LvInput.h"
#include "ui/UIManager.h"
#include "storage/SDStore.h"
#include <Arduino.h>
#include <SD.h>
#include "fonts/fonts.h"

namespace {

// App-mode chrome — matches LvMapScreen's strip exactly so the user gets
// consistent placement when jumping between Apps tiles. The list lives
// inside the area below the strip (kMapY = 24 in app mode).
constexpr lv_coord_t kHeaderH   = 22;
constexpr lv_coord_t kHeaderPad = 2;
constexpr lv_coord_t kMapY      = kHeaderH + kHeaderPad;   // 24

// Header widget geometry (content-area coords, app-mode)
constexpr lv_coord_t kBackX = 4;
constexpr lv_coord_t kBackY = 2;
constexpr lv_coord_t kBackW = 64;
constexpr lv_coord_t kBackH = kHeaderH - 4;   // 18

constexpr lv_coord_t kNewX  = Theme::CONTENT_W - 4 - 64;   // 252
constexpr lv_coord_t kNewY  = kBackY;
constexpr lv_coord_t kNewW  = 64;
constexpr lv_coord_t kNewH  = kBackH;

// List row geometry (inside the map area)
constexpr lv_coord_t kRowGap  = 2;
constexpr lv_coord_t kRowH    = 32;
constexpr lv_coord_t kRowPadX = 8;

// SD paths. Mirrors Pro for portability — same SD card should work in
// either device's Notes app.
constexpr const char* kDirFiles = "/Files";
constexpr const char* kDirNotes = "/Files/notes";

// Case-insensitive ASCII lowercase — avoids pulling in <strings.h>'s
// strcasecmp and keeps the comparison local to this file.
static char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// True if `name` ends with the 4-byte case-insensitive ".txt" extension.
// FAT on ESP32 can return either the LFN (preserving the lowercase we
// wrote) or the 8.3 short name (typically UPPERCASE, e.g. NOTE_XX~1.TXT)
// depending on the VFS layer and the card. The previous case-sensitive
// check filtered out every short-name entry, which is why a freshly
// saved note wouldn't appear in the list. This now accepts either.
static bool endsWithTxt(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 4) return false;
    const char* ext = name + n - 4;
    return asciiLower(ext[0]) == '.' &&
           asciiLower(ext[1]) == 't' &&
           asciiLower(ext[2]) == 'x' &&
           asciiLower(ext[3]) == 't';
}

// True if `name` ends with .tmp or .bak (case-insensitive). These are
// the suffixes SDStore::writeAtomic uses for the in-flight and previous
// versions of a note; we skip them so a partial save never shows up as
// a stale "note" in the list. They wouldn't match endsWithTxt() anyway,
// but the explicit check makes the intent obvious and protects against
// future filename changes.
static bool endsWithTempExt(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    auto hasExt = [&](const char* ext) {
        size_t el = strlen(ext);
        if (n < el) return false;
        const char* p = name + n - el;
        for (size_t i = 0; i < el; ++i) {
            if (asciiLower(p[i]) != ext[i]) return false;
        }
        return true;
    };
    return hasExt(".tmp") || hasExt(".bak");
}

}  // namespace

void LvNotesListScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header strip ----
    // BACK pill top-left (matches LvMapScreen app-mode BACK).
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
        auto* self = (LvNotesListScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);
    _backBtnX1 = kBackX;
    _backBtnY1 = kBackY + Theme::STATUS_BAR_H;
    _backBtnX2 = kBackX + kBackW - 1;
    _backBtnY2 = kBackY + kBackH - 1 + Theme::STATUS_BAR_H;

    // NEW (+) pill top-right.
    _newBtn = lv_btn_create(parent);
    lv_obj_set_size(_newBtn, kNewW, kNewH);
    lv_obj_set_pos(_newBtn, kNewX, kNewY);
    lv_obj_add_style(_newBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_newBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_newBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_newBtn, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_border_width(_newBtn, 1, 0);
    lv_obj_set_style_radius(_newBtn, 3, 0);
    lv_obj_set_style_pad_all(_newBtn, 0, 0);
    lv_obj_set_style_shadow_width(_newBtn, 0, 0);
    lv_obj_clear_flag(_newBtn, LV_OBJ_FLAG_SCROLLABLE);
    _newLbl = lv_label_create(_newBtn);
    lv_obj_set_style_text_font(_newLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_newLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_text_color(_newLbl, lv_color_hex(Theme::ACCENT), LV_STATE_PRESSED);
    lv_label_set_text(_newLbl, "+ NEW");
    lv_obj_center(_newLbl);
    lv_obj_add_event_cb(_newBtn, [](lv_event_t* e) {
        auto* self = (LvNotesListScreen*)lv_event_get_user_data(e);
        // Empty filename → caller assigns the actual timestamp name
        // (the screen has no clock access).
        if (self->_onOpenEdit) self->_onOpenEdit(String(""));
    }, LV_EVENT_CLICKED, this);
    _newBtnX1 = kNewX;
    _newBtnY1 = kNewY + Theme::STATUS_BAR_H;
    _newBtnX2 = kNewX + kNewW - 1;
    _newBtnY2 = kNewY + kNewH - 1 + Theme::STATUS_BAR_H;

    // NOTES title — centered between BACK and NEW.
    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_set_style_text_letter_space(_title, 1, 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_title, Theme::CONTENT_W - (kBackX + kBackW + kNewW + 8));
    lv_obj_set_pos(_title, kBackX + kBackW + 4, kBackY + 1);
    lv_label_set_text(_title, "NOTES");

    // ---- Note list ----
    // Flex column below the header, full width, scrollable when >6 rows.
    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, Theme::CONTENT_W, Theme::CONTENT_H - kMapY);
    lv_obj_set_pos(_list, 0, kMapY);
    lv_obj_set_style_bg_color(_list, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_gap(_list, kRowGap, 0);
    lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_SCROLLABLE);

    _emptyState = nullptr;
    _focusIdx = -2;
    _nNotes = 0;
    _rowNames.clear();
    for (int i = 0; i < MAX_NOTE_COUNT; i++) {
        _rowBtns[i] = nullptr;
        _rowLbls[i] = nullptr;
    }

    // Defer rebuildList() to onEnter(). setScreen() calls createUI()
    // then onEnter() unconditionally, so running it here would do the
    // SD walk twice AND re-enter rebuildList() on a just-emptied _list,
    // which historically hit a use-after-free on _emptyState (see
    // rebuildList's lv_obj_clean + _emptyState handling).
}

void LvNotesListScreen::destroyUI() {
    _backBtn = nullptr; _backLbl = nullptr;
    _newBtn = nullptr;  _newLbl = nullptr;
    _title = nullptr;
    _list = nullptr;
    _emptyState = nullptr;
    for (int i = 0; i < MAX_NOTE_COUNT; i++) {
        _rowBtns[i] = nullptr;
        _rowLbls[i] = nullptr;
    }
    _nNotes = 0;
    _rowNames.clear();
    LvScreen::destroyUI();
}

void LvNotesListScreen::onEnter() {
    rebuildList();
}

void LvNotesListScreen::refreshUI() {
    // No periodic data flow — list rebuilds on entry only.
}

void LvNotesListScreen::rebuildList() {
    if (!_list) return;
    _nNotes = 0;
    _rowNames.clear();

    // Wipe previous children of the list. lv_obj_clean() deletes every
    // child widget (rows and the empty-state placeholder). Our C
    // pointers (_emptyState, _rowBtns[], _rowLbls[]) are now dangling —
    // null them BEFORE doing anything that might inspect them, so a
    // later showEmptyState() / if (_emptyState) check is safe. Skipping
    // this null-out was the original crash: onEnter() re-ran rebuildList,
    // the dangling _emptyState was passed to lv_obj_del() → hard fault.
    lv_obj_clean(_list);
    _emptyState = nullptr;
    for (int i = 0; i < MAX_NOTE_COUNT; i++) {
        _rowBtns[i] = nullptr;
        _rowLbls[i] = nullptr;
    }

    // Null-guard _sd. The wiring in main.cpp runs setSDStore() at boot,
    // so the pointer is normally set, but a defensive check keeps us
    // safe if the screen is somehow reached before main has finished
    // wiring. Same pattern is used in LvSettingsScreen::loadFlash().
    if (!_sd) {
        showEmptyState(/*noSd=*/true);
        if (_focusIdx == -2) _focusIdx = -1;
        return;
    }
    _sdReady = _sd->isReady();
    if (!_sdReady) {
        showEmptyState(/*noSd=*/true);
        if (_focusIdx == -2) _focusIdx = -1;
        return;
    }

    // Ensure both directories exist before we walk them. ensureDir is
    // recursive and idempotent. Mirrors the save path in main.cpp so
    // a fresh card (no /Files tree yet) shows the list without errors.
    _sd->ensureDir(kDirFiles);
    _sd->ensureDir(kDirNotes);

    // Walk /Files/notes and collect .txt files. We sort by mtime DESC
    // (newest first) so the most-recently-edited note is at the top —
    // matches the user's mental model from Messages/Contacts.
    struct Entry {
        char name[64];   // generous; names are basename, generated to <40 chars
        unsigned long mtime;
        uint32_t bytes;
    } entries[MAX_NOTE_COUNT];
    int n = 0;

    File dir = _sd->openDir(kDirNotes);
    bool dirOpened = (bool)dir && dir.isDirectory();
    int scanned = 0;        // every non-dir entry we saw (regardless of ext)
    int skippedTmp = 0;     // .tmp / .bak leftovers from a partial save
    int skippedOther = 0;   // anything else in the folder (e.g. hidden files)
    if (dirOpened) {
        File f = dir.openNextFile();
        while (f && n < MAX_NOTE_COUNT) {
            if (!f.isDirectory()) {
                scanned++;
                // Use basename explicitly. Arduino SD's f.name() on ESP32
                // returns just the filename, but some other VFS layers
                // (and even the same lib with a full path) can return
                // the full path — strip it to be sure we never overflow
                // entries[].name with a long path.
                const char* raw = f.name();
                const char* base = raw ? raw : "";
                if (*base) {
                    const char* slash = strrchr(base, '/');
                    if (slash && *(slash + 1)) base = slash + 1;
                }
                if (endsWithTempExt(base)) {
                    // Partial-save residue from writeAtomic's .tmp / .bak
                    // staging. Not a real note; never show it.
                    skippedTmp++;
                } else if (endsWithTxt(base)) {
                    size_t nameLen = strnlen(base, sizeof(entries[n].name) - 1);
                    memcpy(entries[n].name, base, nameLen);
                    entries[n].name[nameLen] = '\0';
                    entries[n].mtime = f.getLastWrite();
                    entries[n].bytes = f.size();
                    n++;
                } else {
                    skippedOther++;
                }
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    // Diagnostic: log what we saw so a "list is empty after save" is
    // debuggable from serial. The body is never logged — only counts and
    // the directory's open status, so a recovered user note doesn't leak.
    Serial.printf("[NOTES] dir=%s open=%s scanned=%d notes=%d tmp/bak=%d other=%d\n",
                  kDirNotes, dirOpened ? "yes" : "no",
                  scanned, n, skippedTmp, skippedOther);

    if (n == 0) {
        showEmptyState(/*noSd=*/false);
        if (_focusIdx == -2) _focusIdx = -1;
        return;
    }

    // Simple insertion sort — n is bounded by MAX_NOTE_COUNT (32), so
    // O(n²) is fine and avoids pulling in <algorithm> / std::sort with
    // its PSRAM allocator concerns.
    for (int i = 1; i < n; ++i) {
        Entry cur = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].mtime < cur.mtime) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = cur;
    }

    for (int i = 0; i < n; ++i) {
        _rowNames.push_back(String(entries[i].name));

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
        lv_obj_set_user_data(row, (void*)(intptr_t)i);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, Theme::CONTENT_W - 80);
        lv_obj_set_pos(lbl, kRowPadX, (kRowH - 14) / 2);
        lv_label_set_text(lbl, entries[i].name);

        lv_obj_t* sizeLbl = lv_label_create(row);
        lv_obj_set_style_text_font(sizeLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(sizeLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_obj_set_style_text_align(sizeLbl, LV_TEXT_ALIGN_RIGHT, 0);
        char sizeBuf[12];
        if (entries[i].bytes < 1024) {
            snprintf(sizeBuf, sizeof(sizeBuf), "%uB", (unsigned)entries[i].bytes);
        } else {
            snprintf(sizeBuf, sizeof(sizeBuf), "%uK", (unsigned)(entries[i].bytes / 1024));
        }
        lv_obj_set_width(sizeLbl, 60);
        lv_obj_set_pos(sizeLbl, Theme::CONTENT_W - 70, (kRowH - 12) / 2);
        lv_label_set_text(sizeLbl, sizeBuf);

        lv_group_add_obj(LvInput::group(), row);
        // Click → open the editor for this row. We re-read the index
        // from user_data; _rowNames holds the matching filename.
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvNotesListScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (idx < 0 || idx >= (int)self->_rowNames.size()) return;
            if (!self->_onOpenEdit) return;
            self->_onOpenEdit(self->_rowNames[idx]);
        }, LV_EVENT_CLICKED, this);

        _rowBtns[i] = row;
        _rowLbls[i] = lbl;
    }

    _nNotes = n;
    if (_focusIdx == -2 || _focusIdx >= _nNotes + 1) {
        _focusIdx = 0;   // first row by default
    }
}

void LvNotesListScreen::showEmptyState(bool noSd) {
    if (!_list) return;
    if (_emptyState) {
        lv_obj_del(_emptyState);
        _emptyState = nullptr;
    }
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
    lv_label_set_text(title, noSd ? "No SD card" : "No notes yet");

    lv_obj_t* hint = lv_label_create(_emptyState);
    lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(hint, noSd
        ? "Insert an SD card to use notes"
        : "Tap + NEW to create one");
}

void LvNotesListScreen::ensureFocus(int /*dirFromTop*/) {
    // Reserved for future keyboard-driven focus on BACK / NEW pills.
    // For MVP the LVGL group already includes those pills, so trackball
    // nav reaches them without extra work here.
}

void LvNotesListScreen::clearFocus() {
    // Reserved for future keyboard-driven focus reset.
}

bool LvNotesListScreen::handleKey(const KeyEvent& event) {
    if (!_list) return false;

    // Esc / back → Apps hub.
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) _onBack();
        return true;
    }
    return false;
}

bool LvNotesListScreen::handleLongPress() {
    return false;
}
