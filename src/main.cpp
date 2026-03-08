// =============================================================================
// Ratdeck v1.0 — Main Entry Point
// LilyGo T-Deck Plus: LovyanGFX Direct UI + microReticulum + LXMF Messaging
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>

#include "config/BoardConfig.h"
#include "config/Config.h"
#include "hal/Display.h"
#include "hal/TouchInput.h"
#include "hal/Trackball.h"
#include "hal/Keyboard.h"
#include "hal/Power.h"
#include "radio/SX1262.h"
#include "input/InputManager.h"
#include "input/HotkeyManager.h"
#include "ui/UIManager.h"
#include "ui/LvTabBar.h"
#include "ui/LvInput.h"
#include "ui/screens/BootScreen.h"
#include "ui/screens/HomeScreen.h"
#include "ui/screens/NodesScreen.h"
#include "ui/screens/MessagesScreen.h"
#include "ui/screens/MessageView.h"
#include "ui/screens/SettingsScreen.h"
#include "ui/screens/HelpOverlay.h"
// MapScreen removed
#include "ui/screens/NameInputScreen.h"
#include "ui/screens/LvBootScreen.h"
#include "ui/screens/LvHomeScreen.h"
#include "ui/screens/LvNodesScreen.h"
#include "ui/screens/LvMessagesScreen.h"
#include "ui/screens/LvMessageView.h"
#include "ui/screens/LvContactsScreen.h"
#include "ui/screens/LvSettingsScreen.h"
#include "ui/screens/LvHelpOverlay.h"
// Map screen removed
#include "ui/screens/LvNameInputScreen.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/IdentityManager.h"
#include "transport/LoRaInterface.h"
#include "transport/WiFiInterface.h"
#include "transport/TCPClientInterface.h"
#include "transport/BLEInterface.h"
#include "transport/BLESideband.h"
#include "config/UserConfig.h"
#include "audio/AudioNotify.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <list>
#include <esp_system.h>
#include <freertos/task.h>

// --- Hardware ---
// Single shared SPI bus for display, LoRa, and SD card
// IMPORTANT: On ESP32-S3, Arduino FSPI=0 maps to SPI2 hardware.
// Do NOT use SPI2_HOST (IDF constant = 1) — Arduino treats index 1 as HSPI/SPI3!
SPIClass sharedSPI(FSPI);

SX1262 radio(&sharedSPI,
    LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
    LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
    LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);

Display display;
TouchInput touch;
Trackball trackball;
Keyboard keyboard;

// --- Subsystems ---
InputManager inputManager;
HotkeyManager hotkeys;
UIManager ui;
FlashStore flash;
SDStore sdStore;
MessageStore messageStore;
ReticulumManager rns;
AnnounceManager* announceManager = nullptr;
RNS::HAnnounceHandler announceHandler;
LXMFManager lxmf;
WiFiInterface* wifiImpl = nullptr;
RNS::Interface wifiIface({RNS::Type::NONE});
std::vector<TCPClientInterface*> tcpClients;
std::list<RNS::Interface> tcpIfaces;  // Must persist — Transport stores references (list: no realloc)
BLEInterface bleInterface;
BLESideband bleSideband;
UserConfig userConfig;
Power powerMgr;
AudioNotify audio;
IdentityManager identityMgr;

// --- Legacy Screens (kept for fallback during migration) ---
BootScreen bootScreen;
HomeScreen homeScreen;
NodesScreen nodesScreen;
MessagesScreen messagesScreen;
MessageView messageView;
SettingsScreen settingsScreen;
HelpOverlay helpOverlay;
// MapScreen removed
NameInputScreen nameInputScreen;

// --- LVGL Screens ---
LvBootScreen lvBootScreen;
LvHomeScreen lvHomeScreen;
LvNodesScreen lvNodesScreen;
LvMessagesScreen lvMessagesScreen;
LvContactsScreen lvContactsScreen;
LvMessageView lvMessageView;
LvSettingsScreen lvSettingsScreen;
LvHelpOverlay lvHelpOverlay;
// LvMapScreen removed
LvNameInputScreen lvNameInputScreen;

// Tab-screen mapping (4 tabs) — LVGL versions
LvScreen* lvTabScreens[LvTabBar::TAB_COUNT] = {};

// Legacy tab mapping (kept for reference)
Screen* tabScreens[LvTabBar::TAB_COUNT] = {};

// --- State ---
bool radioOnline = false;
bool bootComplete = false;
bool bootLoopRecovery = false;
bool wifiSTAStarted = false;
bool wifiSTAConnected = false;
unsigned long lastAutoAnnounce = 0;
unsigned long lastStatusUpdate = 0;
constexpr unsigned long ANNOUNCE_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes
constexpr unsigned long STATUS_UPDATE_MS = 1000;                // 1 Hz status bar update
unsigned long lastHeartbeat = 0;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;
unsigned long loopCycleStart = 0;
unsigned long maxLoopTime = 0;

// =============================================================================
// Announce with display name (MessagePack-encoded app_data)
// =============================================================================

static RNS::Bytes encodeAnnounceName(const String& name) {
    if (name.isEmpty()) return {};
    size_t len = name.length();
    if (len > 31) len = 31;
    uint8_t buf[2 + 31];
    buf[0] = 0x91;                     // msgpack fixarray(1)
    buf[1] = 0xA0 | (uint8_t)len;     // msgpack fixstr(len)
    memcpy(buf + 2, name.c_str(), len);
    return RNS::Bytes(buf, 2 + len);
}

static void announceWithName() {
    Serial.println("[ANNOUNCE-TX] announceWithName() entry");
    RNS::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
    rns.announce(appData);
    ui.statusBar().flashAnnounce();
    ui.statusBar().showToast("Announce sent!");
    ui.lvStatusBar().flashAnnounce();
    ui.lvStatusBar().showToast("Announce sent!");
    Serial.println("[ANNOUNCE-TX] announceWithName() exit");
}

// =============================================================================
// TCP client management — stop old clients, create new from config
// =============================================================================

static void reloadTCPClients() {
    // Stop existing clients (don't delete — Transport holds interface references)
    for (auto* tcp : tcpClients) tcp->stop();
    tcpClients.clear();

    // Create new clients from current config
    if (WiFi.status() == WL_CONNECTED) {
        for (auto& ep : userConfig.settings().tcpConnections) {
            if (ep.autoConnect && !ep.host.isEmpty()) {
                char name[32];
                snprintf(name, sizeof(name), "TCP.%s", ep.host.c_str());
                auto* tcp = new TCPClientInterface(ep.host.c_str(), ep.port, name);
                tcpIfaces.emplace_back(tcp);
                tcpIfaces.back().mode(RNS::Type::Interface::MODE_GATEWAY);
                RNS::Transport::register_interface(tcpIfaces.back());
                tcp->start();
                tcpClients.push_back(tcp);
                Serial.printf("[TCP] Created client: %s:%d (registered with Transport, mode=GATEWAY)\n", ep.host.c_str(), ep.port);
                Serial.printf("[TCP] Total interfaces registered: %d\n", (int)RNS::Transport::get_interfaces().size());
            }
        }
    }

    if (tcpClients.empty()) {
        Serial.println("[TCP] No active TCP connections");
    }
}

// =============================================================================
// Hotkey callbacks
// =============================================================================

void onHotkeyHelp() {
    lvHelpOverlay.toggle();
}
void onHotkeyMessages() {
    ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
    ui.setLvScreen(&lvMessagesScreen);
}
void onHotkeyNewMsg() {
    ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
    ui.setLvScreen(&lvMessagesScreen);
}
void onHotkeySettings() {
    ui.lvTabBar().setActiveTab(LvTabBar::TAB_SETTINGS);
    ui.setLvScreen(&lvSettingsScreen);
}
void onHotkeyAnnounce() {
    announceWithName();
}
void onHotkeyDiag() {
    Serial.println("=== DIAGNOSTIC DUMP ===");
    Serial.printf("Device: Ratdeck T-Deck Plus\n");
    Serial.printf("Identity: %s\n", rns.identityHash().c_str());
    Serial.printf("Transport: %s\n", rns.isTransportActive() ? "ACTIVE" : "OFFLINE");
    Serial.printf("Paths: %d  Links: %d\n", (int)rns.pathCount(), (int)rns.linkCount());
    Serial.printf("Radio: %s\n", radioOnline ? "ONLINE" : "OFFLINE");
    if (radioOnline) {
        Serial.printf("Freq: %lu Hz  SF: %d  BW: %lu  CR: 4/%d  TXP: %d dBm\n",
                      (unsigned long)radio.getFrequency(),
                      radio.getSpreadingFactor(),
                      (unsigned long)radio.getSignalBandwidth(),
                      radio.getCodingRate4(),
                      radio.getTxPower());
        Serial.printf("Preamble: %ld symbols\n", radio.getPreambleLength());
        uint16_t devErr = radio.getDeviceErrors();
        uint8_t status = radio.getStatus();
        Serial.printf("DevErrors: 0x%04X  Status: 0x%02X (mode=%d cmd=%d)\n",
            devErr, status, (status >> 4) & 0x07, (status >> 1) & 0x07);
        if (devErr & 0x40) Serial.println("  *** PLL LOCK FAILED ***");
        Serial.printf("Current RSSI: %d dBm\n", radio.currentRssi());
    }
    Serial.printf("Free heap: %lu bytes  PSRAM: %lu bytes\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    Serial.printf("Uptime: %lu s\n", millis() / 1000);
    Serial.println("=======================");
}

volatile bool rssiMonitorActive = false;
void onHotkeyRssiMonitor() {
    if (!radioOnline) { Serial.println("[RSSI] Radio offline"); return; }
    Serial.println("[RSSI] Sampling for 5 seconds...");
    rssiMonitorActive = true;
    int minRssi = 0, maxRssi = -200;
    unsigned long start = millis();
    int samples = 0;
    while (millis() - start < 5000) {
        int rssi = radio.currentRssi();
        if (rssi < minRssi) minRssi = rssi;
        if (rssi > maxRssi) maxRssi = rssi;
        samples++;
        Serial.printf("[RSSI] %d dBm\n", rssi);
        delay(100);
    }
    rssiMonitorActive = false;
    Serial.printf("[RSSI] Done: %d samples, min=%d max=%d dBm\n", samples, minRssi, maxRssi);
}

void onHotkeyRadioTest() {
    Serial.println("[TEST] Sending raw test packet...");
    uint8_t header = 0xA0;
    const char* testPayload = "RATDECK_TEST_1234567890";
    radio.beginPacket();
    radio.write(header);
    radio.write((const uint8_t*)testPayload, strlen(testPayload));
    bool ok = radio.endPacket();
    Serial.printf("[TEST] TX %s (%d bytes)\n", ok ? "OK" : "FAILED", (int)(1 + strlen(testPayload)));
    radio.receive();
}

// =============================================================================
// Helper: render boot screen immediately
// =============================================================================
static void bootRender() {
    // LVGL boot screen calls lv_timer_handler() internally via setProgress()
    // Legacy render kept as fallback
}

// =============================================================================
// Setup — 26-step boot sequence
// =============================================================================

void setup() {
    // Step 1: Power pin — CRITICAL: enables all T-Deck Plus peripherals
    Power::enablePeripherals();

    // Step 2: Serial
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println();
    Serial.println("=================================");
    Serial.printf("  Ratdeck v%s\n", RATDECK_VERSION_STRING);
    Serial.println("  LilyGo T-Deck Plus");
    Serial.println("=================================");

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:   reasonStr = "POWER_ON"; break;
        case ESP_RST_SW:        reasonStr = "SOFTWARE"; break;
        case ESP_RST_PANIC:     reasonStr = "PANIC"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT"; break;
        case ESP_RST_WDT:       reasonStr = "WDT"; break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEP_SLEEP"; break;
        default: break;
    }
    Serial.printf("[BOOT] Reset: %s (%d)\n", reasonStr, (int)reason);
    Serial.printf("[BOOT] Heap: %lu  PSRAM: %lu\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getPsramSize());

    // Step 3: Initialize I2C bus (shared by keyboard + touchscreen)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    // Step 3.5: Initialize shared SPI bus
    sharedSPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    // Deassert all slave CS pins to prevent bus contention
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);

    // Step 4: Radio + SD init BEFORE display
    // Radio and SD must init while SPIClass exclusively owns SPI2_HOST.
    // LovyanGFX's init() later joins the bus via spi_bus_add_device().
    // This avoids any bus re-init dance that would invalidate device handles.
    Serial.println("[BOOT] Initializing radio...");
    if (radio.begin(LORA_DEFAULT_FREQ)) {
        radio.setSpreadingFactor(LORA_DEFAULT_SF);
        radio.setSignalBandwidth(LORA_DEFAULT_BW);
        radio.setCodingRate4(LORA_DEFAULT_CR);
        radio.setTxPower(LORA_DEFAULT_TX_POWER);
        radio.setPreambleLength(LORA_DEFAULT_PREAMBLE);
        radio.receive();
        radioOnline = true;
        Serial.println("[RADIO] SX1262 online at 915 MHz");
    } else {
        Serial.println("[RADIO] SX1262 not detected!");
    }

    // SD card init (shared SPI, right after radio)
    digitalWrite(LORA_CS, HIGH);
    delay(10);
    if (sdStore.begin(&sharedSPI, SD_CS)) {
        sdStore.formatForRatputer();
        Serial.println("[SD] Card ready");
    } else {
        Serial.println("[SD] Not detected");
    }

    // Verify radio SPI still works after SD init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI pre-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }

    // Step 5: Display HAL — LovyanGFX + ST7789V
    // LovyanGFX's Bus_SPI::init() calls spi_bus_initialize() which will
    // return ESP_ERR_INVALID_STATE (bus already owned by SPIClass) and
    // then spi_bus_add_device() to join the existing bus. Both LGFX and
    // SPIClass get valid device handles on the same SPI2_HOST bus.
    display.begin();
    Serial.println("[BOOT] Display initialized (LovyanGFX direct)");

    // Step 5.5: Initialize LVGL display driver
    display.beginLVGL();
    Serial.println("[BOOT] LVGL initialized");

    // Verify radio SPI survives display init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI post-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }

    // Step 6: UI manager (initializes both legacy and LVGL UI layers)
    ui.begin(&display.gfx());
    ui.setBootMode(true);
    ui.setLvScreen(&lvBootScreen);
    ui.statusBar().setLoRaOnline(radioOnline);
    ui.lvStatusBar().setLoRaOnline(radioOnline);
    lvBootScreen.setProgress(0.45f, radioOnline ? "Radio online" : "Radio FAILED");

    // Step 7: Touch HAL — GT911 I2C
    touch.begin();
    lvBootScreen.setProgress(0.50f, "Touch ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 8: Keyboard HAL — ESP32-C3 I2C
    keyboard.begin();
    lvBootScreen.setProgress(0.52f, "Keyboard ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 9: Trackball HAL — GPIO interrupts
    trackball.begin();
    lvBootScreen.setProgress(0.54f, "Trackball ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 10: Input manager
    inputManager.begin(&keyboard, &trackball, &touch);

    // Step 10.5: LVGL input drivers
    LvInput::init(&keyboard, &trackball, &touch);

    lvBootScreen.setProgress(0.55f, "Input ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 11: Register hotkeys
    hotkeys.registerHotkey('h', "Help", onHotkeyHelp);
    hotkeys.registerHotkey('m', "Messages", onHotkeyMessages);
    hotkeys.registerHotkey('n', "New Message", onHotkeyNewMsg);
    hotkeys.registerHotkey('s', "Settings", onHotkeySettings);
    hotkeys.registerHotkey('a', "Announce", onHotkeyAnnounce);
    hotkeys.registerHotkey('d', "Diagnostics", onHotkeyDiag);
    hotkeys.registerHotkey('t', "Radio Test", onHotkeyRadioTest);
    hotkeys.registerHotkey('r', "RSSI Monitor", onHotkeyRssiMonitor);
    hotkeys.setTabCycleCallback([](int dir) {
        ui.lvTabBar().cycleTab(dir);
        int tab = ui.lvTabBar().getActiveTab();
        if (lvTabScreens[tab]) ui.setLvScreen(lvTabScreens[tab]);
    });
    lvBootScreen.setProgress(0.58f, "Hotkeys registered");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 12: Mount LittleFS
    lvBootScreen.setProgress(0.60f, "Mounting flash...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    if (!flash.begin()) {
        Serial.println("[BOOT] Flash init failed, formatting...");
        if (flash.format()) {
            Serial.println("[BOOT] LittleFS formatted and mounted");
        } else {
            Serial.println("[BOOT] LittleFS format failed!");
        }
    } else {
        Serial.println("[BOOT] LittleFS mounted OK");
    }

    // Step 13: Boot loop detection (NVS)
    {
        Preferences prefs;
        if (prefs.begin("ratdeck", false)) {
            int bc = prefs.getInt("bootc", 0);
            prefs.putInt("bootc", bc + 1);
            prefs.end();
            if (bc >= 3) {
                Serial.printf("[BOOT] Boot loop detected (%d failures)\n", bc);
                bootLoopRecovery = true;
            }
        }
    }

    lvBootScreen.setProgress(0.65f, "Starting Reticulum...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    rns.setSDStore(&sdStore);
    // Transport mode is loaded later from config; default is endpoint (no rebroadcast)
    // We load a quick peek at the config to get the transport setting before RNS init
    {
        String json = sdStore.isReady() ? sdStore.readString(SD_PATH_USER_CONFIG) : "";
        if (json.isEmpty()) json = flash.readString(PATH_USER_CONFIG);
        if (!json.isEmpty()) {
            JsonDocument peek;
            if (!deserializeJson(peek, json)) {
                bool transport = peek["transport"] | false;
                rns.setTransportEnabled(transport);
                Serial.printf("[BOOT] Transport mode: %s\n", transport ? "ON" : "OFF");
            }
        }
    }
    if (rns.begin(&radio, &flash)) {
        Serial.printf("[BOOT] Identity: %s\n", rns.identityHash().c_str());
        lvBootScreen.setProgress(0.72f, "Reticulum active");
    } else {
        Serial.println("[BOOT] Reticulum init failed!");
        lvBootScreen.setProgress(0.72f, "RNS: FAILED");
    }
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 15.5: Identity manager
    identityMgr.begin(&flash, &sdStore);

    // Step 16: Message store
    lvBootScreen.setProgress(0.72f, "Starting messaging...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    messageStore.begin(&flash, &sdStore);

    // Step 17: LXMF init
    lxmf.begin(&rns, &messageStore);
    lxmf.setMessageCallback([](const LXMFMessage& msg) {
        Serial.printf("[LXMF] Message from %s\n", msg.sourceHash.toHex().substr(0, 8).c_str());
        ui.tabBar().setUnreadCount(TabBar::TAB_MSGS, lxmf.unreadCount());
        ui.lvTabBar().setUnreadCount(LvTabBar::TAB_MSGS, lxmf.unreadCount());
        audio.playMessage();
    });
    // Pre-cache unread counts so first tab switch to Messages is instant
    lxmf.unreadCount();
    lvBootScreen.setProgress(0.75f, "LXMF ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 18: Announce manager
    lvBootScreen.setProgress(0.78f, "Loading contacts...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    announceManager = new AnnounceManager();
    announceManager->setStorage(&sdStore, &flash);
    announceManager->setLocalDestHash(rns.destination().hash());
    announceManager->loadContacts();
    announceManager->loadNameCache();
    announceHandler = RNS::HAnnounceHandler(announceManager);
    RNS::Transport::register_announce_handler(announceHandler);

    // Step 19: User config load
    lvBootScreen.setProgress(0.82f, "Loading config...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    userConfig.load(sdStore, flash);

    // Sync display name between active identity slot and config.
    // The identity slot is the source of truth for the name.
    {
        String slotName;
        if (identityMgr.syncNameFromActive(slotName)) {
            if (!slotName.isEmpty()) {
                // Slot has a name — use it (overrides any stale config value)
                if (userConfig.settings().displayName != slotName) {
                    Serial.printf("[BOOT] Name from identity slot: '%s'\n", slotName.c_str());
                    userConfig.settings().displayName = slotName;
                    userConfig.save(sdStore, flash);
                }
            } else if (!userConfig.settings().displayName.isEmpty()) {
                // Slot has no name but config does — seed the slot (first boot migration)
                identityMgr.setDisplayName(identityMgr.activeIndex(),
                    userConfig.settings().displayName);
                Serial.printf("[BOOT] Seeded identity slot name: '%s'\n",
                    userConfig.settings().displayName.c_str());
            }
        }
    }

    // Step 20: Boot loop recovery
    if (bootLoopRecovery) {
        userConfig.settings().wifiMode = RAT_WIFI_OFF;
        Serial.println("[BOOT] WiFi forced OFF (boot loop recovery)");
    }
    lvBootScreen.setProgress(0.83f, "Config loaded");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 21: Apply radio config
    if (radioOnline) {
        auto& s = userConfig.settings();
        radio.setFrequency(s.loraFrequency);
        radio.setSpreadingFactor(s.loraSF);
        radio.setSignalBandwidth(s.loraBW);
        radio.setCodingRate4(s.loraCR);
        radio.setTxPower(s.loraTxPower);
        radio.setPreambleLength(s.loraPreamble);
        radio.receive();
        Serial.printf("[BOOT] Radio: %lu Hz, SF%d, BW%lu, CR4/%d, %d dBm, pre=%ld\n",
                      (unsigned long)s.loraFrequency, s.loraSF,
                      (unsigned long)s.loraBW, s.loraCR, s.loraTxPower, s.loraPreamble);
    }
    lvBootScreen.setProgress(0.84f, "Radio configured");
    // (LVGL boot renders via lv_timer_handler in setProgress)

    // Step 22: WiFi start
    RatWiFiMode wifiMode = userConfig.settings().wifiMode;
    ui.lvStatusBar().setWiFiEnabled(wifiMode != RAT_WIFI_OFF);
    if (wifiMode == RAT_WIFI_AP) {
        lvBootScreen.setProgress(0.87f, "Starting WiFi AP...");
        // (LVGL boot renders via lv_timer_handler in setProgress)
        wifiImpl = new WiFiInterface("WiFi.AP");
        if (!userConfig.settings().wifiAPSSID.isEmpty()) {
            wifiImpl->setAPCredentials(
                userConfig.settings().wifiAPSSID.c_str(),
                userConfig.settings().wifiAPPassword.c_str());
        }
        wifiIface = wifiImpl;
        wifiIface.mode(RNS::Type::Interface::MODE_GATEWAY);
        RNS::Transport::register_interface(wifiIface);
        wifiImpl->start();
        ui.statusBar().setWiFiActive(true);
        ui.lvStatusBar().setWiFiActive(true);
    } else if (wifiMode == RAT_WIFI_STA) {
        lvBootScreen.setProgress(0.87f, "WiFi STA starting...");
        // WiFi is enabled but not yet connected — indicator will be yellow
        if (!userConfig.settings().wifiSTASSID.isEmpty()) {
            WiFi.mode(WIFI_STA);
            WiFi.setAutoReconnect(true);
            WiFi.begin(userConfig.settings().wifiSTASSID.c_str(),
                       userConfig.settings().wifiSTAPassword.c_str());
            wifiSTAStarted = true;
            Serial.printf("[WIFI] STA: %s\n", userConfig.settings().wifiSTASSID.c_str());
        }
    } else {
        lvBootScreen.setProgress(0.87f, "WiFi disabled");
        // (LVGL boot renders via lv_timer_handler in setProgress)
    }

    // Step 23: BLE start
    lvBootScreen.setProgress(0.90f, "BLE...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    ui.lvStatusBar().setBLEEnabled(userConfig.settings().bleEnabled);
    if (userConfig.settings().bleEnabled) {
        bleInterface.setSideband(&bleSideband);

        if (bleInterface.start()) {
            static RNS::Interface bleIface(&bleInterface);
            bleIface.mode(RNS::Type::Interface::MODE_GATEWAY);
            RNS::Transport::register_interface(bleIface);

            bleSideband.begin(bleInterface.getServer());
            bleSideband.setPacketCallback([](const uint8_t* data, size_t len) {
                RNS::Bytes pkt(data, len);
                bleInterface.injectIncoming(pkt);
            });

            ui.statusBar().setBLEActive(true);
            ui.lvStatusBar().setBLEActive(true);
            Serial.println("[BLE] Transport + Sideband ready");
        }
    } else {
        Serial.println("[BLE] Disabled by config");
    }

    // Step 24: Power manager
    lvBootScreen.setProgress(0.92f, "Power manager...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    powerMgr.begin();
    powerMgr.setDimTimeout(userConfig.settings().screenDimTimeout);
    powerMgr.setOffTimeout(userConfig.settings().screenOffTimeout);
    powerMgr.setBrightness(userConfig.settings().brightness);

    // Step 25: Audio init
    lvBootScreen.setProgress(0.94f, "Audio...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    audio.setEnabled(userConfig.settings().audioEnabled);
    audio.setVolume(userConfig.settings().audioVolume);
    audio.begin();

    // Boot complete — transition to Home screen
    delay(200);
    lvBootScreen.setProgress(1.0f, "Ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    audio.playBoot();
    delay(400);

    bootComplete = true;
    ui.statusBar().setTransportMode("Ratdeck");
    ui.lvStatusBar().setTransportMode("Ratdeck");

    // Keep UI alive during blocking radio TX (endPacket wait loop)
    // Re-entrancy guard prevents nested lv_timer_handler() calls
    radio.setYieldCallback([]() {
        static bool inYield = false;
        if (inYield) return;
        inYield = true;
        powerMgr.activity();  // Keep screen alive during TX
        if (powerMgr.isScreenOn()) {
            lv_timer_handler();
        }
        inYield = false;
    });

    // Wire up LVGL screen dependencies
    lvHomeScreen.setReticulumManager(&rns);
    lvHomeScreen.setRadio(&radio);
    lvHomeScreen.setUserConfig(&userConfig);
    lvHomeScreen.setLXMFManager(&lxmf);
    lvHomeScreen.setAnnounceManager(announceManager);
    lvHomeScreen.setRadioOnline(radioOnline);
    lvHomeScreen.setAnnounceCallback([]() {
        announceWithName();
        Serial.println("[HOME] Announce triggered via Enter");
    });

    lvContactsScreen.setAnnounceManager(announceManager);
    lvContactsScreen.setUIManager(&ui);
    lvContactsScreen.setNodeSelectedCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
        ui.setLvScreen(&lvMessageView);
    });

    lvNodesScreen.setAnnounceManager(announceManager);
    lvNodesScreen.setUIManager(&ui);
    lvNodesScreen.setNodeSelectedCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
        ui.setLvScreen(&lvMessageView);
    });

    lvMessagesScreen.setLXMFManager(&lxmf);
    lvMessagesScreen.setAnnounceManager(announceManager);
    lvMessagesScreen.setUIManager(&ui);
    lvMessagesScreen.setOpenCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.setLvScreen(&lvMessageView);
    });

    lvMessageView.setLXMFManager(&lxmf);
    lvMessageView.setAnnounceManager(announceManager);
    lvMessageView.setUIManager(&ui);
    lvMessageView.setBackCallback([]() {
        ui.setLvScreen(&lvMessagesScreen);
    });

    lvSettingsScreen.setUserConfig(&userConfig);
    lvSettingsScreen.setFlashStore(&flash);
    lvSettingsScreen.setSDStore(&sdStore);
    lvSettingsScreen.setRadio(&radio);
    lvSettingsScreen.setAudio(&audio);
    lvSettingsScreen.setPower(&powerMgr);
    lvSettingsScreen.setWiFi(wifiImpl);
    lvSettingsScreen.setTCPClients(&tcpClients);
    lvSettingsScreen.setRNS(&rns);
    lvSettingsScreen.setIdentityManager(&identityMgr);
    lvSettingsScreen.setUIManager(&ui);
    lvSettingsScreen.setIdentityHash(rns.identityHash());
    lvSettingsScreen.setDestinationHash(rns.destinationHashHex());
    lvSettingsScreen.setSaveCallback([]() -> bool {
        bool ok = userConfig.save(sdStore, flash);
        Serial.printf("[CONFIG] Save %s\n", ok ? "OK" : "FAILED");
        return ok;
    });
    lvSettingsScreen.setTCPChangeCallback([]() {
        Serial.println("[TCP] Settings changed, reloading...");
        reloadTCPClients();
        if (announceManager) announceManager->clearTransientNodes();
    });

    // LVGL help overlay
    lvHelpOverlay.create();

    // Tab bar callbacks — LVGL
    lvTabScreens[LvTabBar::TAB_HOME]     = &lvHomeScreen;
    lvTabScreens[LvTabBar::TAB_CONTACTS] = &lvContactsScreen;
    lvTabScreens[LvTabBar::TAB_MSGS]     = &lvMessagesScreen;
    lvTabScreens[LvTabBar::TAB_NODES]    = &lvNodesScreen;
    lvTabScreens[LvTabBar::TAB_SETTINGS] = &lvSettingsScreen;

    ui.lvTabBar().setTabCallback([](int tab) {
        if (lvTabScreens[tab]) ui.setLvScreen(lvTabScreens[tab]);
    });

    // Name input screen (first boot only — when no display name is set)
    lvNameInputScreen.setDoneCallback([](const String& name) {
        String finalName = name;
        if (finalName.isEmpty()) {
            // Auto-generate: Ratspeak.org-xxx (first 3 chars of LXMF dest hash)
            String dh = rns.destinationHashHex();
            finalName = "Ratspeak.org-" + dh.substring(0, 3);
        }
        userConfig.settings().displayName = finalName;
        userConfig.save(sdStore, flash);
        // Also save to active identity slot
        if (identityMgr.activeIndex() >= 0) {
            identityMgr.setDisplayName(identityMgr.activeIndex(), finalName);
        }
        Serial.printf("[BOOT] Display name set: '%s'\n", finalName.c_str());

        // Transition to home screen
        ui.setBootMode(false);
        ui.setLvScreen(&lvHomeScreen);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_HOME);

        // Initial announce with name
        RNS::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
        rns.announce(appData);
        lastAutoAnnounce = millis();
        ui.statusBar().flashAnnounce();
        ui.lvStatusBar().flashAnnounce();
        Serial.println("[BOOT] Initial announce sent");
    });

    if (userConfig.settings().displayName.isEmpty()) {
        // Show name input screen (boot mode keeps status/tab bars hidden)
        ui.setLvScreen(&lvNameInputScreen);
        Serial.println("[BOOT] Showing name input screen");
    } else {
        // Name already set — go straight to home
        ui.setBootMode(false);
        ui.setLvScreen(&lvHomeScreen);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_HOME);

        // Initial announce with name
        RNS::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
        rns.announce(appData);
        lastAutoAnnounce = millis();
        Serial.println("[BOOT] Initial announce sent");
    }

    // Clear boot loop counter — we survived!
    {
        Preferences prefs;
        if (prefs.begin("ratdeck", false)) {
            prefs.putInt("bootc", 0);
            prefs.end();
        }
    }

    Serial.println("[BOOT] Ratdeck ready");
    Serial.printf("[BOOT] Summary: radio=%s flash=%s sd=%s\n",
                  radioOnline ? "ONLINE" : "OFFLINE",
                  flash.isReady() ? "OK" : "FAIL",
                  sdStore.isReady() ? "OK" : "FAIL");
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
    // 1. Input polling
    inputManager.update();
    if (inputManager.hadStrongActivity()) {
        powerMgr.activity();       // Keyboard/touch: wake from any state
    } else if (inputManager.hadActivity()) {
        powerMgr.weakActivity();   // Trackball: wake from dim only
    }

    // 2. Long-press dispatch
    if (inputManager.hadLongPress()) {
        ui.handleLongPress();
    }

    // 3. Key event dispatch
    if (inputManager.hasKeyEvent()) {
        const KeyEvent& evt = inputManager.getKeyEvent();

        // Help overlay intercepts all keys when visible
        if (lvHelpOverlay.isVisible()) {
            lvHelpOverlay.handleKey(evt);
        }
        // Ctrl+hotkeys first
        else if (!hotkeys.process(evt)) {
            // Feed key to LVGL input system
            LvInput::feedKey(evt);

            // Screen gets the key next
            bool consumed = ui.handleKey(evt);

            // Tab cycling: ,=left /=right OR trackball left/right (only if screen didn't consume)
            if (!consumed && !evt.ctrl) {
                bool tabLeft  = (evt.character == ',') || evt.left;
                bool tabRight = (evt.character == '/') || evt.right;
                if (tabLeft) {
                    ui.lvTabBar().cycleTab(-1);
                    int tab = ui.lvTabBar().getActiveTab();
                    if (lvTabScreens[tab]) ui.setLvScreen(lvTabScreens[tab]);
                }
                if (tabRight) {
                    ui.lvTabBar().cycleTab(1);
                    int tab = ui.lvTabBar().getActiveTab();
                    if (lvTabScreens[tab]) ui.setLvScreen(lvTabScreens[tab]);
                }
            }
        }
    }

    // 3. LVGL timer handler — run FIRST after input for responsive UI
    if (powerMgr.isScreenOn()) {
        lv_timer_handler();
    }

    // 4. Reticulum loop (radio RX via LoRaInterface) — throttle to ~200Hz
    {
        static unsigned long lastRNS = 0;
        unsigned long now = millis();
        if (now - lastRNS >= 5) {
            lastRNS = now;
            rns.loop();
        }
    }

    // 5. Auto-announce every 5 minutes
    if (bootComplete && millis() - lastAutoAnnounce >= ANNOUNCE_INTERVAL_MS) {
        lastAutoAnnounce = millis();
        RNS::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
        rns.announce(appData);
        ui.statusBar().flashAnnounce();
        ui.lvStatusBar().flashAnnounce();
        Serial.println("[AUTO] Periodic announce");
    }

    // 6. LXMF outgoing queue + announce manager deferred saves
    lxmf.loop();
    if (announceManager) announceManager->loop();

    // 7. WiFi STA connection handler
    if (wifiSTAStarted) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wifiSTAConnected) {
            wifiSTAConnected = true;
            ui.statusBar().setWiFiActive(true);
            ui.lvStatusBar().setWiFiActive(true);
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());

            // Create TCP clients (safe to call multiple times)
            if (tcpClients.empty()) {
                reloadTCPClients();
                // Announce over TCP now that it's available
                if (!tcpClients.empty()) {
                    Serial.println("[TCP] Sending announce over new TCP connection...");
                    RNS::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
                    rns.announce(appData);
                    lastAutoAnnounce = millis();
                }
            }
        } else if (!connected && wifiSTAConnected) {
            wifiSTAConnected = false;
            ui.statusBar().setWiFiActive(false);
            ui.lvStatusBar().setWiFiActive(false);
            Serial.println("[WIFI] STA disconnected");
        }
    }

    // 8. WiFi + TCP loops
    if (wifiImpl) wifiImpl->loop();
    for (auto* tcp : tcpClients) {
        tcp->loop();
        yield();
    }

    // 9. BLE loops
    bleInterface.loop();
    bleSideband.loop();

    // 10. Power management
    powerMgr.loop();

    // 11. Periodic status bar update (1 Hz) + render
    if (millis() - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = millis();
        if (powerMgr.isScreenOn()) {
            ui.statusBar().setBatteryPercent(powerMgr.batteryPercent());
            ui.lvStatusBar().setBatteryPercent(powerMgr.batteryPercent());
            ui.update();
        }
    }

    // 12. Render any dirty regions
    if (powerMgr.isScreenOn()) {
        ui.render();
    }

    // 13. Heartbeat for crash diagnosis
    {
        unsigned long cycleTime = millis() - loopCycleStart;
        if (cycleTime > maxLoopTime) maxLoopTime = cycleTime;

        if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = millis();
            Serial.printf("[HEART] heap=%lu psram=%lu min=%lu loop=%lums nodes=%d paths=%d links=%d lxmfQ=%d up=%lus radio=%s sd=%s flash=%s\n",
                          (unsigned long)ESP.getFreeHeap(),
                          (unsigned long)ESP.getFreePsram(),
                          (unsigned long)ESP.getMinFreeHeap(),
                          maxLoopTime,
                          announceManager ? announceManager->nodeCount() : 0,
                          (int)rns.pathCount(),
                          (int)rns.linkCount(),
                          lxmf.queuedCount(),
                          millis() / 1000,
                          radioOnline ? "ON" : "OFF",
                          sdStore.isReady() ? "OK" : "FAIL",
                          flash.isReady() ? "OK" : "FAIL");
            maxLoopTime = 0;
        }
    }
    loopCycleStart = millis();

    yield();
}
