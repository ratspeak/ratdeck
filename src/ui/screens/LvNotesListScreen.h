#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <vector>

// =============================================================================
// Notes list — full-screen (tab bar hidden) launcher for the Notes app.
// Reads .txt files from /Files/notes via SDStore, shows a scrollable list of
// names with mtime + size. Tap row → edit that file. NEW button → empty edit.
// BACK pill → Apps hub (caller restores tab bar in the back callback).
//
// MVP scope: cleartext .txt on SD, no encryption, no rename, no delete UI.
// Body cap and per-folder cap are enforced by LvNotesEditScreen + the
// directory-walk cap below; this screen never writes.
// =============================================================================

class SDStore;

class LvNotesListScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;

    void setSDStore(SDStore* sd) { _sd = sd; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    // Callback fired when the user picks a file or taps NEW. `filename` is
    // empty for "create new" — main.cpp decides the actual filename in
    // that case (timestamp generation needs clock access).
    using OpenEditCallback = std::function<void(const String& filename)>;
    void setOpenEditCallback(OpenEditCallback cb) { _onOpenEdit = cb; }

    // Callback fired when the user hits BACK (top-left pill or Esc).
    // Caller typically restores the tab bar and routes back to Apps.
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Notes"; }

    // Limits enforced by this screen + the editor. Kept here so both
    // screens see the same numbers without re-declaring.
    static constexpr int MAX_NOTE_COUNT = 32;       // hard cap on notes per device
    static constexpr int MAX_BODY_LEN   = 4096;    // bytes — matches spec upper bound

private:
    void rebuildList();
    void showEmptyState(bool noSd);
    void ensureFocus(int dirFromTop);
    void clearFocus();

    class UIManager* _ui = nullptr;
    SDStore* _sd = nullptr;
    OpenEditCallback _onOpenEdit;
    BackCallback _onBack;

    // UI handles
    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _backLbl = nullptr;
    lv_obj_t* _newBtn = nullptr;
    lv_obj_t* _newLbl = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _list = nullptr;        // flex column of note rows
    lv_obj_t* _emptyState = nullptr;  // "No notes" / "No SD card"
    lv_obj_t* _rowBtns[MAX_NOTE_COUNT] = {nullptr};
    lv_obj_t* _rowLbls[MAX_NOTE_COUNT] = {nullptr};

    // Per-row filename carrier. Indexed parallel to _rowBtns[]; populated
    // each rebuildList() pass and consumed by the row click callback.
    std::vector<String> _rowNames;

    // Back-pill bounds cached for touch suppression (mirrors LvMapScreen).
    int16_t _backBtnX1 = 0, _backBtnY1 = 0, _backBtnX2 = 0, _backBtnY2 = 0;
    int16_t _newBtnX1 = 0, _newBtnY1 = 0, _newBtnX2 = 0, _newBtnY2 = 0;

    // Focus state — -1 means BACK, 0..nNotes-1 = row, nNotes = NEW.
    int _focusIdx = -2;   // -2 = uninitialized
    int _nNotes = 0;
    bool _sdReady = false;
};
