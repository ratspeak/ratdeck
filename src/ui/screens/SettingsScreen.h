#pragma once

#include "ui/UIManager.h"
#include "transport/WiFiInterface.h"
#include <string>
#include <vector>
#include <functional>

class UserConfig;
class FlashStore;
class SDStore;
class SX1262;
class AudioNotify;
class Power;
class WiFiInterface;
class TCPClientInterface;
class ReticulumManager;
class IdentityManager;

enum class SettingType : uint8_t {
    READONLY,
    INTEGER,
    TOGGLE,
    ENUM_CHOICE,
    ACTION,
    TEXT_INPUT
};

enum class SettingsView : uint8_t {
    CATEGORY_LIST,
    ITEM_LIST,
    WIFI_PICKER
};

struct SettingItem {
    const char* label;
    SettingType type;
    std::function<int()> getter;
    std::function<void(int)> setter;
    std::function<String(int)> formatter;
    int minVal = 0;
    int maxVal = 1;
    int step = 1;
    std::vector<const char*> enumLabels;
    std::function<void()> action;
    std::function<String()> textGetter;
    std::function<void(const String&)> textSetter;
    int maxTextLen = 16;
};

struct SettingsCategory {
    const char* name;
    int startIdx;
    int count;
    std::function<String()> summary;
};

class SettingsScreen : public Screen {
public:
    void onEnter() override;
    void update() override;
    bool handleKey(const KeyEvent& event) override;

    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setFlashStore(FlashStore* fs) { _flash = fs; }
    void setSDStore(SDStore* sd) { _sd = sd; }
    void setRadio(SX1262* radio) { _radio = radio; }
    void setAudio(AudioNotify* audio) { _audio = audio; }
    void setPower(Power* power) { _power = power; }
    void setWiFi(WiFiInterface* wifi) { _wifi = wifi; }
    void setTCPClients(std::vector<TCPClientInterface*>* tcp) { _tcp = tcp; }
    void setRNS(ReticulumManager* rns) { _rns = rns; }
    void setIdentityManager(IdentityManager* idm) { _idMgr = idm; }
    void setUIManager(UIManager* ui) { _ui = ui; }
    void setIdentityHash(const String& hash) { _identityHash = hash; }
    void setSaveCallback(std::function<bool()> cb) { _saveCallback = cb; }

    const char* title() const override { return "Settings"; }
    void draw(LGFX_TDeck& gfx) override;

private:
    void buildItems();
    void applyAndSave();
    void applyPreset(int presetIdx);
    int detectPreset() const;

    void drawCategoryList(LGFX_TDeck& gfx);
    void drawItemList(LGFX_TDeck& gfx);
    void drawWifiPicker(LGFX_TDeck& gfx);

    bool handleCategoryKeys(const KeyEvent& event);
    bool handleItemKeys(const KeyEvent& event);
    bool handleWifiPickerKeys(const KeyEvent& event);

    void enterCategory(int catIdx);
    void exitToCategories();
    void skipToNextEditable(int dir);
    bool isEditable(int idx) const;

    UserConfig* _cfg = nullptr;
    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    SX1262* _radio = nullptr;
    AudioNotify* _audio = nullptr;
    Power* _power = nullptr;
    WiFiInterface* _wifi = nullptr;
    std::vector<TCPClientInterface*>* _tcp = nullptr;
    ReticulumManager* _rns = nullptr;
    IdentityManager* _idMgr = nullptr;
    UIManager* _ui = nullptr;
    String _identityHash;

    std::function<bool()> _saveCallback;

    // View state
    SettingsView _view = SettingsView::CATEGORY_LIST;

    // Categories
    std::vector<SettingsCategory> _categories;
    int _categoryIdx = 0;
    int _categoryScroll = 0;

    // Items (within current category)
    std::vector<SettingItem> _items;
    int _selectedIdx = 0;
    int _itemScroll = 0;
    int _catRangeStart = 0;
    int _catRangeEnd = 0;

    // Edit state
    bool _editing = false;
    int _editValue = 0;
    bool _textEditing = false;
    String _editText;
    bool _confirmingReset = false;

    // WiFi picker
    std::vector<WiFiInterface::ScanResult> _wifiResults;
    int _wifiPickerIdx = 0;
    int _wifiPickerScroll = 0;
    bool _wifiScanning = false;
};
