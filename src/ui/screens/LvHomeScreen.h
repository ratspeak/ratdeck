#pragma once

#include "ui/UIManager.h"
#include "hal/GPSManager.h"
#include <Arduino.h>
#include <functional>
#include <vector>

class ReticulumManager;
class SX1262;
class UserConfig;
class LXMFManager;
class AnnounceManager;
class TCPClientInterface;

class LvHomeScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setReticulumManager(ReticulumManager* rns) { _rns = rns; }
    void setRadio(SX1262* radio) { _radio = radio; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setRadioOnline(bool online) { _radioOnline = online; }
    void setTCPClients(std::vector<TCPClientInterface*>* clients) { _tcpClients = clients; }
    void setAnnounceCallback(std::function<void()> cb) { _announceCb = cb; }
    void setAudioToggleCallback(std::function<void()> cb) { _audioToggleCb = cb; }
    void setLoraToggleCallback(std::function<void()> cb) { _loraToggleCb = cb; }
    void setTCPToggleCallback(std::function<void()> cb) { _tcpToggleCb = cb; }
    void setWiFiToggleCallback(std::function<void()> cb) { _wifiToggleCb = cb; }
    void setGPSToggleCallback(std::function<void()> cb) { _gpsToggleCb = cb; }
    void setPeersCallback(std::function<void()> cb) { _peersCb = cb; }
    void setGPSManager(GPSManager* gps) { _gps = gps; }

    const char* title() const override { return "Home"; }

private:
    ReticulumManager* _rns = nullptr;
    SX1262* _radio = nullptr;
    UserConfig* _cfg = nullptr;
    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    std::vector<TCPClientInterface*>* _tcpClients = nullptr;
    bool _radioOnline = false;
    std::function<void()> _announceCb;
    std::function<void()> _audioToggleCb;
    std::function<void()> _loraToggleCb;
    std::function<void()> _tcpToggleCb;
    std::function<void()> _wifiToggleCb;
    std::function<void()> _gpsToggleCb;
    std::function<void()> _peersCb;
    GPSManager* _gps = nullptr;
    unsigned long _lastRefreshMs = 0;
    unsigned long _lastUptime = 0;
    uint32_t _lastHeap = 0;
    String _avatarSeed;
    std::vector<uint8_t> _avatarBuffer;

    lv_obj_t* _identityPanel = nullptr;
    lv_obj_t* _avatarBox = nullptr;
    lv_obj_t* _avatarCanvas = nullptr;
    lv_obj_t* _chipLora = nullptr;
    lv_obj_t* _chipTcp = nullptr;
    lv_obj_t* _chipWifi = nullptr;
    lv_obj_t* _statNodes = nullptr;
    lv_obj_t* _statPaths = nullptr;
    lv_obj_t* _statLinks = nullptr;

    lv_obj_t* _lblConsoleTitle = nullptr;
    lv_obj_t* _lblName = nullptr;
    lv_obj_t* _lblId = nullptr;
    lv_obj_t* _lblIdentity = nullptr;
    lv_obj_t* _lblStatus = nullptr;
    lv_obj_t* _lblLoraState = nullptr;
    lv_obj_t* _lblTcpState = nullptr;
    lv_obj_t* _lblWifiState = nullptr;
    lv_obj_t* _lblNodes = nullptr;
    lv_obj_t* _lblPaths = nullptr;
    lv_obj_t* _lblLinks = nullptr;
    lv_obj_t* _lblSummary = nullptr;
    lv_obj_t* _lblLastAnnounce = nullptr;
    lv_obj_t* _btnAnnounce = nullptr;
    lv_obj_t* _lblAnnounceAction = nullptr;

    void toggleAudio();
    void toggleLora();
    void toggleTcp();
    void toggleWiFi();
    void toggleGPS();
    void openPeers();
    void forceRefresh();
    void renderAvatar(const String& seed);
};
