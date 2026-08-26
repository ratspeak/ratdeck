#pragma once

#include "ui/UIManager.h"
#include <functional>

// =============================================================================
// Reader — full-screen plain-text viewer for /Files .txt/.md.
// Loads up to BODY_CAP bytes (48 KB), scrollable body. BACK → Files.
// PDF / other types are rejected by the Files browser before open.
// =============================================================================

class SDStore;

class LvReaderScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;

    void setSDStore(SDStore* sd) { _sd = sd; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    // Absolute path under /Files. Call before setScreen.
    void setPath(const String& absPath) { _path = absPath; }
    const String& path() const { return _path; }

    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    // Free body buffer (also called from onExit / destroyUI).
    void close();

    const char* title() const override { return "Reader"; }

    static constexpr size_t BODY_CAP = 48 * 1024;  // 48 KB incl. NUL room
    static constexpr size_t PATH_MAX_LEN = 160;

private:
    bool loadBody();
    bool pathUnderRoot(const char* path) const;
    void showError(const char* msg);

    class UIManager* _ui = nullptr;
    SDStore* _sd = nullptr;
    BackCallback _onBack;

    String _path;
    char* _body = nullptr;
    size_t _bodyLen = 0;
    bool _truncated = false;
    bool _loaded = false;

    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _titleLbl = nullptr;
    lv_obj_t* _bodyCont = nullptr;
    lv_obj_t* _bodyLbl = nullptr;
    lv_obj_t* _metaLbl = nullptr;
};
