#pragma once

#include "ui/UIManager.h"
#include <functional>
#include <vector>

// =============================================================================
// Files browser — full-screen (tab bar hidden) chrooted to /Files.
// Lists dirs + files (dirs first). Tap dir → enter. Tap .txt/.md → Reader.
// Other extensions toast "Unsupported". BACK → Apps hub.
// Mirrors Pro ScreenFiles product scope (PDF no-go v1).
// =============================================================================

class SDStore;

class LvFilesScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setSDStore(SDStore* sd) { _sd = sd; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    // Open a readable file (absolute path under /Files).
    using OpenFileCallback = std::function<void(const String& absPath)>;
    void setOpenFileCallback(OpenFileCallback cb) { _onOpenFile = cb; }

    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    // Reset cwd to /Files (call when opening from Apps tile).
    void resetToRoot();

    const char* title() const override { return "Files"; }

    static constexpr int MAX_ENTRIES = 64;
    static constexpr size_t PATH_MAX_LEN = 160;

    // True if basename ends with .txt or .md (case-insensitive).
    static bool isReadableName(const char* name);

private:
    struct Ent {
        String name;
        bool isDir = false;
        uint32_t size = 0;
    };

    void rebuildList();
    void showEmptyState(bool noSd, bool empty);
    bool pathUnderRoot(const char* path) const;
    bool isRootCwd() const;
    bool joinUnderRoot(char* out, size_t cap, const char* base, const char* name) const;
    bool parentOf(const char* path, char* out, size_t cap) const;
    void enterDir(const char* name);  // name is basename, or ".." for parent
    void onRowTap(int idx);

    class UIManager* _ui = nullptr;
    SDStore* _sd = nullptr;
    OpenFileCallback _onOpenFile;
    BackCallback _onBack;

    char _cwd[PATH_MAX_LEN] = "/Files";

    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _crumb = nullptr;
    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;

    std::vector<Ent> _ents;
    bool _hasParent = false;  // synthetic ".." as row 0
    bool _sdReady = false;
};
