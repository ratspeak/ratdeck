// Direct port from Ratputer — microReticulum integration
#include "ReticulumManager.h"
#include "config/Config.h"
#include "platform/CoreSync.h"
#include "util/PerfTrace.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <cstring>
#include <unordered_map>
#include <string>

uint32_t ReticulumManager::_announceFilterCount = 0;

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
    unsigned long startMs = PerfTrace::nowMs();
    String path = String(p);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!LittleFS.exists(dir.c_str())) { LittleFS.mkdir(dir.c_str()); }
    }
    File f = LittleFS.open(p, "w");
    if (!f) {
        PerfTrace::write("flash", "rns-write", p, data.size(), startMs, false);
        return 0;
    }
    size_t w = f.write(data.data(), data.size());
    f.close();
    PerfTrace::write("flash", "rns-write", p, data.size(), startMs, w == data.size());
    return w;
}

// Arduino-LittleFS wrapper for RNS::FileStreamImpl. Needed by microReticulum's
// Persistence::serialize<std::map<...>> streaming path in Transport::write_path_table.
// Without this, write_path_table always logs "serialize failed" because the
// default stub returns NONE, and the path table never persists across reboots.
namespace {
constexpr size_t RNS_COPY_CHUNK = 512;

bool copySDToFlash(const char* sdPath, const char* flashPath) {
    CoreSync::SpiBusGuard busGuard;
    File in = SD.open(sdPath, FILE_READ);
    if (!in) return false;

    String path = String(flashPath);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!LittleFS.exists(dir.c_str())) LittleFS.mkdir(dir.c_str());
    }

    File out = LittleFS.open(flashPath, "w");
    if (!out) { in.close(); return false; }

    uint8_t buf[RNS_COPY_CHUNK];
    bool ok = true;
    while (in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        if (n == 0) break;
        if (out.write(buf, n) != n) { ok = false; break; }
        RNS::Utilities::OS::reset_watchdog();
    }
    in.close();
    out.close();
    if (!ok) LittleFS.remove(flashPath);
    return ok;
}

bool copyFlashToSD(SDStore* sd, const char* flashPath, const char* sdPath) {
    if (!sd || !sd->isReady()) return false;
    CoreSync::SpiBusGuard busGuard;
    File in = LittleFS.open(flashPath, "r");
    if (!in || in.size() == 0) { if (in) in.close(); return false; }
    unsigned long startMs = PerfTrace::nowMs();
    size_t copiedBytes = 0;

    String path = String(sdPath);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!sd->ensureDir(dir.c_str())) {
            in.close();
            PerfTrace::write("sd", "rns-copy", sdPath, copiedBytes, startMs, false);
            return false;
        }
    }

    String tmpPath = path + ".tmp";
    String bakPath = path + ".bak";
    SD.remove(tmpPath.c_str());
    File out = SD.open(tmpPath.c_str(), FILE_WRITE);
    if (!out) {
        in.close();
        PerfTrace::write("sd", "rns-copy", sdPath, copiedBytes, startMs, false);
        return false;
    }

    uint8_t buf[RNS_COPY_CHUNK];
    bool ok = true;
    while (in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        if (n == 0) break;
        if (out.write(buf, n) != n) { ok = false; break; }
        copiedBytes += n;
        RNS::Utilities::OS::reset_watchdog();
    }
    in.close();
    out.close();

    if (!ok) {
        SD.remove(tmpPath.c_str());
        PerfTrace::write("sd", "rns-copy", sdPath, copiedBytes, startMs, false);
        return false;
    }
    if (SD.exists(sdPath)) {
        SD.remove(bakPath.c_str());
        SD.rename(sdPath, bakPath.c_str());
    }
    SD.remove(sdPath);
    if (!SD.rename(tmpPath.c_str(), sdPath)) {
        if (SD.exists(bakPath.c_str())) SD.rename(bakPath.c_str(), sdPath);
        SD.remove(tmpPath.c_str());
        PerfTrace::write("sd", "rns-copy", sdPath, copiedBytes, startMs, false);
        return false;
    }
    SD.remove(bakPath.c_str());
    PerfTrace::write("sd", "rns-copy", sdPath, copiedBytes, startMs, true);
    return true;
}

class LittleFSStreamImpl : public RNS::FileStreamImpl {
public:
    LittleFSStreamImpl(File&& f, const char* path, RNS::FileStream::MODE mode)
        : _f(std::move(f)), _mode(mode), _startMs(PerfTrace::nowMs()) {
        strncpy(_path, path ? path : "?", sizeof(_path) - 1);
        _path[sizeof(_path) - 1] = '\0';
    }
    ~LittleFSStreamImpl() override { close(); }

protected:
    const char* name() override { return _path; }
    size_t size() override { return _f ? _f.size() : 0; }
    void close() override {
        if (!_f) return;
        if (!_logged && _mode != RNS::FileStream::MODE_READ) {
            PerfTrace::write("flash",
                             _mode == RNS::FileStream::MODE_APPEND ? "rns-stream-append" : "rns-stream",
                             _path, _bytesWritten, _startMs, !_writeFailed);
            _logged = true;
        }
        _f.close();
    }

    size_t write(uint8_t byte) override {
        size_t written = _f ? _f.write(byte) : 0;
        if (_mode != RNS::FileStream::MODE_READ) {
            _bytesWritten += written;
            if (written != 1) _writeFailed = true;
        }
        return written;
    }
    size_t write(const uint8_t* buffer, size_t len) override {
        size_t written = _f ? _f.write(buffer, len) : 0;
        if (_mode != RNS::FileStream::MODE_READ) {
            _bytesWritten += written;
            if (written != len) _writeFailed = true;
        }
        return written;
    }

    int available() override { return _f ? _f.available() : 0; }
    int read() override { return _f ? _f.read() : -1; }
    int peek() override { return _f ? _f.peek() : -1; }
    void flush() override { if (_f) _f.flush(); }

private:
    File _f;
    RNS::FileStream::MODE _mode;
    unsigned long _startMs = 0;
    size_t _bytesWritten = 0;
    bool _writeFailed = false;
    bool _logged = false;
    char _path[64] = {};
};
}  // namespace

RNS::FileStream LittleFSFileSystem::open_file(const char* path, RNS::FileStream::MODE mode) {
    const char* openMode = "r";
    if (mode == RNS::FileStream::MODE_WRITE)  openMode = "w";
    if (mode == RNS::FileStream::MODE_APPEND) openMode = "a";
    if (mode != RNS::FileStream::MODE_READ) {
        String p(path);
        int lastSlash = p.lastIndexOf('/');
        if (lastSlash > 0 && !LittleFS.exists(p.substring(0, lastSlash).c_str())) {
            LittleFS.mkdir(p.substring(0, lastSlash).c_str());
        }
    }
    File f = LittleFS.open(path, openMode);
    if (!f) return {RNS::Type::NONE};
    return RNS::FileStream(new LittleFSStreamImpl(std::move(f), path, mode));
}
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

bool ReticulumManager::begin(SX1262* radio, FlashStore* flash, bool loraEnabled) {
    _flash = flash;

    LittleFSFileSystem* fsImpl = new LittleFSFileSystem();
    RNS::FileSystem fs(fsImpl);
    fs.init();
    RNS::Utilities::OS::register_filesystem(fs);
    Serial.println("[RNS] Filesystem registered");

    // Endpoint mode learns paths live from announces/path requests. Upstream
    // Reticulum only restores destination_table in transport mode, so do not
    // resurrect stale endpoint path caches from SD or flash.
    if (LittleFS.exists("/destination_table")) LittleFS.remove("/destination_table");
    if (LittleFS.exists("/packet_hashlist")) LittleFS.remove("/packet_hashlist");

    // Restore known destination identities from SD if missing on flash.
    if (_sd && _sd->isReady()) {
        static const char* files[] = {"/known_destinations"};
        for (const char* name : files) {
            if (!LittleFS.exists(name)) {
                char sdPath[64];
                snprintf(sdPath, sizeof(sdPath), "/ratdeck/transport%s", name);
                if (copySDToFlash(sdPath, name)) {
                    File restored = LittleFS.open(name, "r");
                    size_t len = restored ? restored.size() : 0;
                    if (restored) restored.close();
                    Serial.printf("[RNS] Restored %s from SD (%d bytes)\n", name, (int)len);
                }
            }
        }
    }

    if (loraEnabled) {
        _loraImpl = new LoRaInterface(radio, "LoRa");
        _loraIface = _loraImpl;
        _loraIface.mode(RNS::Type::Interface::MODE_ROAMING);
        RNS::Transport::register_interface(_loraIface);
        if (!_loraImpl->start()) {
            Serial.println("[RNS] WARNING: LoRa interface failed to start");
        }
    } else {
        Serial.println("[RNS] LoRa interface disabled by config");
    }

    _reticulum = RNS::Reticulum();
    // Suppress verbose microReticulum logging — LOG_TRACE floods serial at 115200 baud,
    // blocking the CPU for hundreds of ms. Change to LOG_TRACE or LOG_DEBUG for protocol debugging.
    RNS::loglevel(RNS::LOG_WARNING);
    RNS::Reticulum::transport_enabled(false);
    RNS::Reticulum::probe_destination_enabled(true);
    RNS::Transport::path_table_maxsize(256);
    RNS::Transport::announce_table_maxsize(128);
    _reticulum.start();
    Serial.println("[RNS] Reticulum started (Endpoint)");

    // Layer 1: Transport-level announce filter — runs BEFORE Ed25519 verify
    RNS::Transport::set_filter_packet_callback([](const RNS::Packet& packet) -> bool {
        if (packet.packet_type() != RNS::Type::Packet::ANNOUNCE) return true;

        unsigned long now = millis();

        // Rate limit window (per-second)
        static unsigned long windowStart = 0;
        static unsigned int count = 0;
        if (now - windowStart >= 1000) { windowStart = now; count = 0; }

        if (packet.context() == RNS::Type::Packet::PATH_RESPONSE) return true;

        // Adaptive rate: tighter during first 60s boot flood, then normal
        unsigned int maxRate = (now < 60000) ? 3 : RSDECK_MAX_ANNOUNCES_PER_SEC;
        if (++count > maxRate) { ReticulumManager::_announceFilterCount++; return false; }

        return true;
    });

    // Load persisted known destinations so Identity::recall() works
    // immediately after reboot for previously-seen nodes.
    RNS::Identity::load_known_destinations();

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
    Serial.println("[RNS] Endpoint active");
    return true;
}

bool ReticulumManager::isLoRaNextHop(const RNS::Bytes& destHash) const {
    if (!_loraImpl) return false;
    RNS::Interface nextHop = RNS::Transport::next_hop_interface(destHash);
    return nextHop && nextHop.get() == _loraImpl;
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

    if (_sd && _sd->isReady() && _sd->exists(SD_PATH_IDENTITY)) {
        Serial.println("[RNS] SD identity backup present; automatic import disabled");
    }

    // Tier 2: NVS (ESP32 Preferences — always available)
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
    unsigned long now = millis();
    if (now - _lastPersist >= PATH_PERSIST_INTERVAL_MS) {
        _lastPersist = now;
        persistData();
    }
}

// Synchronous persist — one cycle per call to spread I/O across intervals.
// Runs inside the network task's RNS guard so microReticulum's single-threaded
// transport state cannot be accessed concurrently. rsDeck is an endpoint, not
// a transport node, so path tables are intentionally not persisted across boots.
void ReticulumManager::persistData() {
    unsigned long start = millis();
    unsigned long now = millis();
    unsigned long identityPersistMs = 0;
    unsigned long sdMirrorMs = 0;
    size_t sdMirrorBytes = 0;
    bool sdMirrorAttempted = false;
    bool sdMirrorOk = false;
    const char* cycleName = "identity-skip";
    switch (_persistCycle) {
        case 0:
        {
            if (_lastIdentityPersist == 0 ||
                now - _lastIdentityPersist >= IDENTITY_PERSIST_MIN_INTERVAL_MS) {
                cycleName = "identity";
                unsigned long phaseMs = millis();
                RNS::Identity::persist_data();
                identityPersistMs = millis() - phaseMs;
                _lastIdentityPersist = now;
            }
            break;
        }
        case 1:
            cycleName = "sd-mirror";
            if (_sd && _sd->isReady()) {
                static const char* files[] = {"/known_destinations"};
                for (const char* name : files) {
                    File f = LittleFS.open(name, "r");
                    if (f && f.size() > 0) {
                        size_t len = f.size();
                        f.close();
                        char sdPath[64];
                        snprintf(sdPath, sizeof(sdPath), "/ratdeck/transport%s", name);
                        unsigned long phaseMs = millis();
                        bool ok = copyFlashToSD(_sd, name, sdPath);
                        sdMirrorMs += millis() - phaseMs;
                        sdMirrorBytes += ok ? len : 0;
                        sdMirrorAttempted = true;
                        sdMirrorOk = sdMirrorOk || ok;
                    } else {
                        if (f) f.close();
                    }
                }
            }
            break;
    }
    unsigned long dur = millis() - start;
    Serial.printf("[PERSIST] Cycle %d %s done total=%lums identity=%lums sd_copy=%lums sd_bytes=%u sd=%s\n",
                  _persistCycle, cycleName, dur, identityPersistMs, sdMirrorMs,
                  (unsigned)sdMirrorBytes,
                  sdMirrorAttempted ? (sdMirrorOk ? "ok" : "fail") : "skip");
    if (dur > 500) {
        Serial.printf("[PERSIST] WARNING: Cycle %d blocked for %lums!\n", _persistCycle, dur);
    }
    _persistCycle = (_persistCycle + 1) % 2;
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

String ReticulumManager::destinationHashStr() const {
    if (!_destination) return "unknown";
    std::string hex = _destination.hash().toHex();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 4) + ":" + hex.substr(4, 4) + ":" + hex.substr(8, 4)).c_str());
    }
    return String(hex.c_str());
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
