#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <Arduino.h>

// =============================================================================
// Notes edit — full-screen (tab bar hidden) editor for a single note.
//
// Two save modes:
//   * PLAIN — body is UTF-8 text, written to /Files/notes/note_*.txt
//     via SDStore::writeString (existing path).
//   * LOCKED — body is encrypted to RNE1 (see util/FileCrypto.h) with
//     a user-supplied passphrase, written to
//     /Files/notes/note_*.note.enc via SDStore::writeAtomic.
//
// Switching modes:
//   * Tapping the LOCK/PLAIN toggle pill in the header flips the
//     current intent for the next save. Existing encrypted files stay
//     locked when reopened — opening a .note.enc file forces locked
//     mode and prompts for the passphrase BEFORE showing the body.
//   * The toggle is hidden while an encrypted file is being
//     decrypted (locked is forced) and visible again once the body is
//     unlocked or the user gives up.
//
// Filename model:
//   * If _filename is non-empty on entry → editor loaded that file.
//   * If _filename is empty → editor started empty; on save, the caller
//     (main.cpp) picks the timestamp name and sets _filename BEFORE
//     calling SDStore. We don't generate the timestamp here because it
//     depends on the global RTC configuration and we want one owner.
//
// Passphrase handling:
//   * The passphrase modal reuses the same textarea widget as the
//     editor body but in lv_textarea password mode (bullets shown).
//   * Internal _passBuf (max 128 bytes) holds the passphrase for the
//     duration of a single prompt + confirm cycle. It is wiped
//     (FileCrypto::wipeSensitive) on every code path that exits the
//     prompt: success, cancel, wrong-pass, BACK, screen destroy.
//   * Passphrases are never logged.
//
// Body cap: 4 KB (MAX_BODY_LEN). The plaintext of an encrypted note is
// bounded by the same 4 KB, plus the RNE1 header (38 B) + GCM tag (16 B)
// = 4150 B on disk.
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

    // Filename to load on enter. Empty → new note. If the filename
    // ends with .note.enc the screen enters LOCKED mode immediately
    // and shows the passphrase prompt before revealing the body.
    void setFilename(const String& filename);
    const String& filename() const { return _filename; }

    // Current textarea body — caller passes this to SDStore::writeString
    // for plain saves, or feeds it to FileCrypto::encrypt for locked
    // saves.
    String body() const;

    // Current save-mode intent. True if the next SAVE will encrypt.
    // Returns the forced-locked state when an existing encrypted file
    // is loaded (the toggle is non-functional in that case).
    bool locked() const { return _locked; }

    // Callback fired when the user taps SAVE. Body contains the
    // textarea content. `locked` reflects the editor's current mode.
    // For locked saves `pass`/`passLen` are the user-supplied
    // passphrase (NEVER log this). For plain saves pass is nullptr
    // and passLen is 0. After this returns, the editor wipes its
    // internal passphrase buffer regardless of success/failure.
    using SaveCallback = std::function<void(const String& body,
                                            bool locked,
                                            const char* pass,
                                            size_t passLen)>;
    void setSaveCallback(SaveCallback cb) { _onSave = cb; }

    // Callback fired when the user hits BACK. Caller returns to the
    // list (tabs stay hidden — list is also in app-mode). The editor
    // wipes any pending passphrase before returning.
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    // Optional pre-save validation hook. Returns true if the body is
    // acceptable (we leave it as a pure pass-through so blank notes
    // are still legal). Provided so main.cpp can override without
    // subclassing.
    using BodyValidator = std::function<bool(const String& body)>;
    void setBodyValidator(BodyValidator fn) { _validator = fn; }

    const char* title() const override { return "Note"; }

    // Mirror the list's constants so callers can read/write to a single
    // source of truth.
    static constexpr int MAX_BODY_LEN  = 4096;
    static constexpr int MAX_PASS_LEN  = 128;   // bytes incl. NUL

private:
    // ---- UI build helpers ----
    void paintHeader();
    void buildModal();              // lazy: first time it's needed
    void showModal(const char* title, const char* hint,
                   bool passwordMode, const char* confirmText);
    void hideModal();
    void setModalError(const char* msg);   // small error hint under textarea

    // ---- Passphrase + state machine ----
    enum class ModalMode {
        None,
        OpenEncrypted,    // ask for pass to decrypt the file on disk
        SaveEnter,        // first prompt: enter passphrase for new save
        SaveConfirm,      // second prompt: re-enter to confirm
    };
    void startSave();               // SAVE button handler — picks mode
    void handleModalOk();           // OK button or Enter key
    void handleModalCancel();       // CANCEL button or Esc
    void wipePassBuf();
    void lockToggleClicked();       // LOCK/PLAIN pill handler

    // Body loading
    void loadFromSD();
    void tryOpenDecrypted(const char* pass, size_t passLen);
    void applyLoadedBody(const String& body);

    // UI handles
    class UIManager* _ui = nullptr;
    SDStore* _sd = nullptr;
    SaveCallback _onSave;
    BackCallback _onBack;
    BodyValidator _validator;

    String _filename;          // "" = unsaved new note
    bool _loaded = false;      // body loaded from disk (skips reload on re-enter)
    bool _locked = false;      // current intent / mode

    // Header widgets
    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _backLbl = nullptr;
    lv_obj_t* _saveBtn = nullptr;
    lv_obj_t* _saveLbl = nullptr;
    lv_obj_t* _lockBtn = nullptr;
    lv_obj_t* _lockLbl = nullptr;
    lv_obj_t* _headerLbl = nullptr;
    lv_obj_t* _textarea = nullptr;

    // Modal (passphrase prompt) widgets — built lazily
    lv_obj_t* _modal = nullptr;
    lv_obj_t* _modalTitle = nullptr;
    lv_obj_t* _modalHint = nullptr;
    lv_obj_t* _modalError = nullptr;
    lv_obj_t* _modalPass = nullptr;       // textarea in password mode
    lv_obj_t* _modalOkBtn = nullptr;
    lv_obj_t* _modalCancelBtn = nullptr;
    ModalMode _modalMode = ModalMode::None;
    char _passBuf[MAX_PASS_LEN];
    bool _passBufActive = false;          // true while _passBuf holds a passphrase
    // After SaveEnter→SaveConfirm, ignore one Enter/OK so the same key
    // that submitted "NEXT" cannot immediately submit empty confirm.
    bool _modalIgnoreOk = false;

    // Touch-suppression bounds for BACK / SAVE / LOCK pills.
    int16_t _backBtnX1 = 0, _backBtnY1 = 0, _backBtnX2 = 0, _backBtnY2 = 0;
    int16_t _saveBtnX1 = 0, _saveBtnY1 = 0, _saveBtnX2 = 0, _saveBtnY2 = 0;
    int16_t _lockBtnX1 = 0, _lockBtnY1 = 0, _lockBtnX2 = 0, _lockBtnY2 = 0;
};
