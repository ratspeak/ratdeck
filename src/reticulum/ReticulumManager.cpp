// Direct port from Ratputer — microReticulum integration
#include "ReticulumManager.h"
#include "config/Config.h"
#include <LittleFS.h>
#include <Preferences.h>

bool LittleFSFileSystem::init() { return true; }
bool LittleFSFileSystem::file_exists(const char* p) { return LittleFS.exists(p); }

size_t LittleFSFileSystem::read_file(const char* p, RNS::Bytes& data) {
    File f = LittleFS.open(p, "r");
    if (!f) return 0;
    size_t s = f.size();
    data = RNS::Bytes(s);
    f.readBytes((char*)data.writable(s), s);
    f.close();
    return s;
}

size_t LittleFSFileSystem::write_file(const char* p, const RNS::Bytes& data) {
    String path = String(p);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!LittleFS.exists(dir.c_str())) { LittleFS.mkdir(dir.c_str()); }
    }
    File f = LittleFS.open(p, "w");
    if (!f) return 0;
    size_t w = f.write(data.data(), data.size());
    f.close();
    return w;
}

RNS::FileStream LittleFSFileSystem::open_file(const char*, RNS::FileStream::MODE) { return {RNS::Type::NONE}; }
bool LittleFSFileSystem::remove_file(const char* p) { return LittleFS.remove(p); }
bool LittleFSFileSystem::rename_file(const char* f, const char* t) { return LittleFS.rename(f, t); }
bool LittleFSFileSystem::directory_exists(const char* p) { return LittleFS.exists(p); }
bool LittleFSFileSystem::create_directory(const char* p) { return LittleFS.mkdir(p); }
bool LittleFSFileSystem::remove_directory(const char* p) { return LittleFS.rmdir(p); }

std::list<std::string> LittleFSFileSystem::list_directory(const char* p, Callbacks::DirectoryListing callback) {
    std::list<std::string> entries;
    File dir = LittleFS.open(p);
    if (!dir || !dir.isDirectory()) return entries;
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
        entries.push_back(name);
        if (callback) callback(name);
        f = dir.openNextFile();
    }
    return entries;
}

size_t LittleFSFileSystem::storage_size() { return LittleFS.totalBytes(); }
size_t LittleFSFileSystem::storage_available() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

bool ReticulumManager::begin(SX1262* radio, FlashStore* flash) {
    _flash = flash;

    LittleFSFileSystem* fsImpl = new LittleFSFileSystem();
    RNS::FileSystem fs(fsImpl);
    fs.init();
    RNS::Utilities::OS::register_filesystem(fs);
    Serial.println("[RNS] Filesystem registered");

    // Restore routing tables from SD if missing on flash
    if (_sd && _sd->isReady()) {
        static const char* files[] = {"/destination_table", "/packet_hashlist"};
        for (const char* name : files) {
            if (!LittleFS.exists(name)) {
                char sdPath[64];
                snprintf(sdPath, sizeof(sdPath), "/ratputer/transport%s", name);
                uint8_t buf[4096];
                size_t len = 0;
                if (_sd->readFile(sdPath, buf, sizeof(buf), len) && len > 0) {
                    File f = LittleFS.open(name, "w");
                    if (f) { f.write(buf, len); f.close(); }
                    Serial.printf("[RNS] Restored %s from SD (%d bytes)\n", name, (int)len);
                }
            }
        }
    }

    _loraImpl = new LoRaInterface(radio, "LoRa.915");
    _loraIface = _loraImpl;
    _loraIface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(_loraIface);
    if (!_loraImpl->start()) {
        Serial.println("[RNS] WARNING: LoRa interface failed to start");
    }

    _reticulum = RNS::Reticulum();
    RNS::Reticulum::transport_enabled(_transportEnabled);
    RNS::Reticulum::probe_destination_enabled(true);
    if (_transportEnabled) {
        // Transport node: larger tables for routing
        RNS::Transport::path_table_maxsize(64);
        RNS::Transport::announce_table_maxsize(64);
    } else {
        // Endpoint: smaller tables, no rebroadcasting
        RNS::Transport::path_table_maxsize(32);
        RNS::Transport::announce_table_maxsize(32);
    }
    _reticulum.start();
    Serial.printf("[RNS] Reticulum started (%s)\n", _transportEnabled ? "Transport Node" : "Endpoint");

    if (!loadOrCreateIdentity()) {
        Serial.println("[RNS] ERROR: Identity creation failed!");
        return false;
    }

    _destination = RNS::Destination(
        _identity,
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );
    _destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
    _destination.accepts_links(true);

    _transportActive = true;
    Serial.printf("[RNS] %s active\n", _transportEnabled ? "Transport node" : "Endpoint");
    return true;
}

bool ReticulumManager::loadOrCreateIdentity() {
    // Tier 1: Flash (LittleFS)
    if (_flash->exists(PATH_IDENTITY)) {
        RNS::Bytes keyData;
        if (RNS::Utilities::OS::read_file(PATH_IDENTITY, keyData) > 0) {
            _identity = RNS::Identity(false);
            if (_identity.load_private_key(keyData)) {
                Serial.printf("[RNS] Identity loaded from flash: %s\n", _identity.hexhash().c_str());
                saveIdentityToAll(keyData);
                return true;
            }
        }
    }

    // Tier 2: SD card
    if (_sd && _sd->isReady() && _sd->exists(SD_PATH_IDENTITY)) {
        uint8_t keyBuf[128];
        size_t keyLen = 0;
        if (_sd->readFile(SD_PATH_IDENTITY, keyBuf, sizeof(keyBuf), keyLen) && keyLen > 0) {
            RNS::Bytes keyData(keyBuf, keyLen);
            _identity = RNS::Identity(false);
            if (_identity.load_private_key(keyData)) {
                Serial.printf("[RNS] Identity restored from SD: %s\n", _identity.hexhash().c_str());
                saveIdentityToAll(keyData);
                return true;
            }
        }
    }

    // Tier 3: NVS (ESP32 Preferences — always available)
    {
        Preferences prefs;
        if (prefs.begin("ratdeck_id", true)) {
            size_t keyLen = prefs.getBytesLength("privkey");
            if (keyLen > 0 && keyLen <= 128) {
                uint8_t keyBuf[128];
                prefs.getBytes("privkey", keyBuf, keyLen);
                prefs.end();
                RNS::Bytes keyData(keyBuf, keyLen);
                _identity = RNS::Identity(false);
                if (_identity.load_private_key(keyData)) {
                    Serial.printf("[RNS] Identity restored from NVS: %s\n", _identity.hexhash().c_str());
                    saveIdentityToAll(keyData);
                    return true;
                }
            } else {
                prefs.end();
            }
        }
    }

    // No identity found anywhere — create new
    _identity = RNS::Identity();
    Serial.printf("[RNS] New identity created: %s\n", _identity.hexhash().c_str());

    RNS::Bytes privKey = _identity.get_private_key();
    if (privKey.size() > 0) {
        saveIdentityToAll(privKey);
    }
    return true;
}

void ReticulumManager::saveIdentityToAll(const RNS::Bytes& keyData) {
    // Flash
    _flash->writeAtomic(PATH_IDENTITY, keyData.data(), keyData.size());
    // SD
    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratputer/identity");
        _sd->writeAtomic(SD_PATH_IDENTITY, keyData.data(), keyData.size());
    }
    // NVS (always available, survives flash/SD failures)
    Preferences prefs;
    if (prefs.begin("ratdeck_id", false)) {
        prefs.putBytes("privkey", keyData.data(), keyData.size());
        prefs.end();
        Serial.println("[RNS] Identity saved to NVS");
    }
}

void ReticulumManager::loop() {
    if (!_transportActive) return;
    _reticulum.loop();
    if (_loraImpl) { _loraImpl->loop(); }
    unsigned long now = millis();
    if (now - _lastPersist >= PATH_PERSIST_INTERVAL_MS) {
        _lastPersist = now;
        persistData();
    }
}

void ReticulumManager::persistData() {
    RNS::Transport::persist_data();
    // Backup routing tables to SD
    if (_sd && _sd->isReady()) {
        static const char* files[] = {"/destination_table", "/packet_hashlist"};
        for (const char* name : files) {
            File f = LittleFS.open(name, "r");
            if (f && f.size() > 0) {
                size_t sz = f.size();
                uint8_t* buf = (uint8_t*)malloc(sz);
                if (buf) {
                    f.readBytes((char*)buf, sz);
                    char sdPath[64];
                    snprintf(sdPath, sizeof(sdPath), "/ratputer/transport%s", name);
                    _sd->ensureDir("/ratputer/transport");
                    _sd->writeSimple(sdPath, buf, sz);
                    free(buf);
                }
            }
            if (f) f.close();
        }
    }
}

String ReticulumManager::identityHash() const {
    if (!_identity) return "unknown";
    std::string hex = _identity.hexhash();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 4) + ":" + hex.substr(4, 4) + ":" + hex.substr(8, 4)).c_str());
    }
    return String(hex.c_str());
}

String ReticulumManager::destinationHashHex() const {
    if (!_destination) return "unknown";
    return String(_destination.hash().toHex().c_str());
}

size_t ReticulumManager::pathCount() const { return _reticulum.get_path_table().size(); }
size_t ReticulumManager::linkCount() const { return _reticulum.get_link_count(); }

void ReticulumManager::announce(const RNS::Bytes& appData) {
    if (!_transportActive) return;
    Serial.println("[ANNOUNCE-TX] === Starting ===");
    Serial.printf("[ANNOUNCE-TX] dest_hash:     %s\n", _destination.hash().toHex().c_str());
    Serial.printf("[ANNOUNCE-TX] identity_hash: %s\n", _identity.hexhash().c_str());
    Serial.printf("[ANNOUNCE-TX] app_data size: %d bytes\n", (int)appData.size());
    if (appData.size() > 0) {
        Serial.printf("[ANNOUNCE-TX] app_data hex:  %s\n", appData.toHex().c_str());
    }
    // Log registered interfaces
    auto& ifaces = RNS::Transport::get_interfaces();
    Serial.printf("[ANNOUNCE-TX] registered interfaces: %d\n", (int)ifaces.size());
    for (const auto& [hash, iface] : ifaces) {
        Serial.printf("[ANNOUNCE-TX]   iface: %s OUT=%d online=%d mode=%d\n",
            iface.toString().c_str(), iface.OUT(), iface.online(), (int)iface.mode());
    }
    unsigned long startMs = millis();
    _destination.announce(appData);
    _lastAnnounceTime = millis();
    Serial.printf("[ANNOUNCE-TX] === Complete === (%lu ms)\n", millis() - startMs);
}
