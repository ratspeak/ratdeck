#pragma once

#include "ui/UIManager.h"
#include "transport/WiFiInterface.h"
#include "config/UserConfig.h"
#include <string>
#include <vector>
#include <functional>
class FlashStore;
class SDStore;
class SX1262;
class AudioNotify;
class Power;
class WiFiInterface;
class TCPClientInterface;
class ReticulumManager;
class IdentityManager;

// Reuse existing SettingType, SettingItem, SettingsCategory from SettingsScreen.h
#include "SettingsScreen.h"

class LvSettingsScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void onEnter() override;
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
    void setDestinationHash(const String& hash) { _destinationHash = hash; }
    void setSaveCallback(std::function<bool()> cb) { _saveCallback = cb; }
    void setTCPChangeCallback(std::function<void()> cb) { _tcpChangeCb = cb; }

    const char* title() const override { return "Settings"; }

private:
    void buildItems();
    void applyAndSave();
    void applyPreset(int presetIdx);
    int detectPreset() const;
    bool isEditable(int idx) const;
    void skipToNextEditable(int dir);

    void showCategoryList();
    void showItemList(int catIdx);
    void showWifiPicker();
    void rebuildCategoryList();
    void rebuildItemList();
    void rebuildWifiList();

    void enterCategory(int catIdx);
    void exitToCategories();
    void updateCategorySelection(int oldIdx, int newIdx);
    void updateItemSelection(int oldIdx, int newIdx);
    void updateWifiSelection(int oldIdx, int newIdx);

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
    String _destinationHash;
    std::function<bool()> _saveCallback;
    std::function<void()> _tcpChangeCb;

    SettingsView _view = SettingsView::CATEGORY_LIST;
    std::vector<SettingsCategory> _categories;
    std::vector<SettingItem> _items;
    int _categoryIdx = 0;
    int _selectedIdx = 0;
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

    // Reboot-required tracking
    bool _rebootNeeded = false;
    struct RebootSnapshot {
        RatWiFiMode wifiMode;
        String wifiSTASSID;
        String wifiSTAPassword;
        bool bleEnabled;
        bool transportEnabled;
    };
    RebootSnapshot _rebootSnap;
    void snapshotRebootSettings();
    bool rebootSettingsChanged() const;

    // TCP change detection
    String _tcpSnapHost;
    uint16_t _tcpSnapPort = 0;
    void snapshotTCPSettings();
    bool tcpSettingsChanged() const;

    // LVGL widgets
    lv_obj_t* _scrollContainer = nullptr;
    std::vector<lv_obj_t*> _rowObjs;
};
