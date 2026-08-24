#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <Arduino.h>

// =============================================================================
// Notes edit — full-screen (tab bar hidden) editor for a single note.
// Top-left BACK pill returns to the list. Top-right SAVE button persists
// via SDStore::writeString. Multiline lv_textarea in the body.
//
// Filename model:
//   * If _filename is non-empty on entry → editor loaded that file.
//   * If _filename is empty → editor started empty; on save, the caller
//     (main.cpp) picks the timestamp name and sets _filename BEFORE the
//     writeString call. We don't generate the timestamp here because
//     it depends on the global RTC configuration and we want one owner.
//
// Body cap: 4 KB (MAX_BODY_LEN). lv_textarea_set_max_length() auto-
// truncates further input at the cap. SAVE writes whatever is currently
// in the textarea.
// =============================================================================

class SDStore;

class LvNotesEditScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;

    void setSDStore(SDStore* sd) { _sd = sd; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    // Filename to load on enter. Empty → new note (caller assigns the
    // actual timestamp name on save). Setter also pre-loads if createUI
    // has already run.
    void setFilename(const String& filename);
    const String& filename() const { return _filename; }

    // Read the current textarea contents — main.cpp uses this right
    // before invoking SDStore::writeString with a freshly-generated
    // filename.
    String body() const;

    // Callback fired when the user taps SAVE. The caller decides the
    // filename (if _filename is empty) and writes the file.
    using SaveCallback = std::function<void()>;
    void setSaveCallback(SaveCallback cb) { _onSave = cb; }

    // Callback fired when the user hits BACK. Caller returns to the
    // list (tabs stay hidden — list is also in app-mode).
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    // Optional pre-save validation hook. Returns true if the body is
    // acceptable (non-empty after trim is recommended; we leave it as a
    // pure pass-through so blank notes are still legal). Provided so
    // main.cpp can override without subclassing.
    using BodyValidator = std::function<bool(const String& body)>;
    void setBodyValidator(BodyValidator fn) { _validator = fn; }

    const char* title() const override { return "Note"; }

    // Mirror the list's constants so callers can read/write to a single
    // source of truth. The list also enforces MAX_NOTE_COUNT; the editor
    // enforces MAX_BODY_LEN.
    static constexpr int MAX_BODY_LEN = 4096;

private:
    void loadFromSD();
    void paintHeader();

    class UIManager* _ui = nullptr;
    SDStore* _sd = nullptr;
    SaveCallback _onSave;
    BackCallback _onBack;
    BodyValidator _validator;

    String _filename;          // "" = unsaved new note
    bool _loaded = false;

    // UI handles
    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _backLbl = nullptr;
    lv_obj_t* _saveBtn = nullptr;
    lv_obj_t* _saveLbl = nullptr;
    lv_obj_t* _headerLbl = nullptr;
    lv_obj_t* _textarea = nullptr;

    // Touch-suppression bounds for BACK and SAVE pills.
    int16_t _backBtnX1 = 0, _backBtnY1 = 0, _backBtnX2 = 0, _backBtnY2 = 0;
    int16_t _saveBtnX1 = 0, _saveBtnY1 = 0, _saveBtnX2 = 0, _saveBtnY2 = 0;
};
