#include "SDStore.h"
#include "config/Config.h"
#include "util/PerfTrace.h"

bool SDStore::begin(SPIClass* spi, int csPin) {
    if (!spi) return false;

    // Deassert CS, then try mounting at conservative 4MHz first
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    delay(10);

    if (!SD.begin(csPin, *spi, 4000000)) {
        Serial.printf("[SD] Mount failed (CS=%d), retrying...\n", csPin);
        delay(100);
        // Second attempt
        if (!SD.begin(csPin, *spi, 4000000)) {
            Serial.println("[SD] Card not detected or mount failed");
            _ready = false;
            return false;
        }
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] No card inserted");
        _ready = false;
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC)  typeStr = "MMC";
    if (cardType == CARD_SD)   typeStr = "SD";
    if (cardType == CARD_SDHC) typeStr = "SDHC";

    _ready = true;

    // Increase SPI speed for faster I/O after stable mount at 4MHz
    SD.end();
    if (!SD.begin(csPin, *spi, 16000000)) {
        // 16MHz failed, fall back to 4MHz
        if (!SD.begin(csPin, *spi, 4000000)) {
            Serial.println("[SD] 16MHz failed and 4MHz fallback failed");
            _ready = false;
            return false;
        }
        _ready = true;
        Serial.println("[SD] 16MHz failed, using 4MHz");
    } else {
        _ready = true;
        Serial.println("[SD] SPI speed: 16MHz");
    }

    Serial.printf("[SD] %s card ready, total=%llu MB, used=%llu MB\n",
                  typeStr, totalBytes() / (1024 * 1024), usedBytes() / (1024 * 1024));
    return true;
}

void SDStore::end() { SD.end(); _ready = false; }

uint64_t SDStore::totalBytes() const { return _ready ? SD.totalBytes() : 0; }
uint64_t SDStore::usedBytes() const { return _ready ? SD.usedBytes() : 0; }

bool SDStore::ensureDir(const char* path) {
    if (!_ready) return false;
    if (SD.exists(path)) return true;
    // Create parent directories recursively
    String pathStr = String(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = pathStr.substring(0, lastSlash);
        if (!SD.exists(parent.c_str())) {
            ensureDir(parent.c_str());
        }
    }
    return SD.mkdir(path);
}

bool SDStore::exists(const char* path) { return _ready ? SD.exists(path) : false; }
bool SDStore::remove(const char* path) { return _ready ? SD.remove(path) : false; }
File SDStore::openDir(const char* path) { return _ready ? SD.open(path) : File(); }
bool SDStore::removeDir(const char* path) { return _ready ? SD.rmdir(path) : false; }

bool SDStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    bytesRead = 0;
    if (!_ready) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    if (size > maxLen) { f.close(); return false; }
    bytesRead = f.read(buffer, size);
    f.close();
    return bytesRead == size;
}

File SDStore::openFile(const char* path) {
    if (!_ready) return File();
    return SD.open(path, FILE_READ);
}

bool SDStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    unsigned long startMs = PerfTrace::nowMs();
    if (!_ready) {
        PerfTrace::write("sd", "atomic", path, len, startMs, false);
        return false;
    }

    String tmpPath = String(path) + ".tmp";
    String bakPath = String(path) + ".bak";

    File f = SD.open(tmpPath.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] writeAtomic: failed to open tmp %s\n", tmpPath.c_str());
        PerfTrace::write("sd", "atomic", path, len, startMs, false);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        Serial.printf("[SD] writeAtomic: write incomplete (%d/%d)\n", (int)written, (int)len);
        SD.remove(tmpPath.c_str());
        PerfTrace::write("sd", "atomic", path, len, startMs, false);
        return false;
    }

    File verify = SD.open(tmpPath.c_str(), FILE_READ);
    if (!verify || verify.size() != len) {
        Serial.println("[SD] writeAtomic: verify failed");
        if (verify) verify.close();
        SD.remove(tmpPath.c_str());
        PerfTrace::write("sd", "atomic", path, len, startMs, false);
        return false;
    }
    verify.close();

    if (SD.exists(path)) {
        SD.remove(bakPath.c_str());
        SD.rename(path, bakPath.c_str());
    }

    // ESP32 FatFs f_rename() fails with FR_EXIST if destination exists.
    // Remove destination first to ensure rename succeeds.
    SD.remove(path);

    if (!SD.rename(tmpPath.c_str(), path)) {
        Serial.printf("[SD] writeAtomic: rename failed %s -> %s\n", tmpPath.c_str(), path);
        if (SD.exists(bakPath.c_str())) { SD.rename(bakPath.c_str(), path); }
        PerfTrace::write("sd", "atomic", path, len, startMs, false);
        return false;
    }
    SD.remove(bakPath.c_str());
    PerfTrace::write("sd", "atomic", path, len, startMs, true);
    return true;
}

bool SDStore::writeSimple(const char* path, const uint8_t* data, size_t len) {
    unsigned long startMs = PerfTrace::nowMs();
    if (!_ready) {
        PerfTrace::write("sd", "simple", path, len, startMs, false);
        return false;
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] writeSimple: failed to open %s\n", path);
        PerfTrace::write("sd", "simple", path, len, startMs, false);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        Serial.printf("[SD] writeSimple: write incomplete (%d/%d)\n", (int)written, (int)len);
        PerfTrace::write("sd", "simple", path, len, startMs, false);
        return false;
    }
    PerfTrace::write("sd", "simple", path, len, startMs, true);
    return true;
}

bool SDStore::writeString(const char* path, const String& data) {
    if (writeAtomic(path, (const uint8_t*)data.c_str(), data.length())) {
        return true;
    }
    Serial.println("[SD] writeAtomic failed, trying writeSimple fallback");
    return writeSimple(path, (const uint8_t*)data.c_str(), data.length());
}

String SDStore::readString(const char* path) {
    if (!_ready) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = SD.open(bakPath.c_str(), FILE_READ);
        if (!f) return "";
    }
    String result = f.readString();
    f.close();
    return result;
}

bool SDStore::wipeRsDeck() {
    if (!_ready) return false;
    Serial.println("[SD] Wiping rsDeck legacy /ratdeck/ data ...");
    wipeDir("/ratdeck/messages");
    wipeDir("/ratdeck/contacts");
    wipeDir("/ratdeck/identity");
    wipeDir("/ratdeck/config");
    wipeDir("/ratdeck/transport");
    SD.rmdir("/ratdeck");
    Serial.println("[SD] Wipe complete, recreating dirs...");
    return formatForRsDeck();
}

void SDStore::wipeDir(const char* path) {
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) return;
    File entry = dir.openNextFile();
    while (entry) {
        String fullPath = String(path) + "/" + entry.name();
        if (entry.isDirectory()) {
            wipeDir(fullPath.c_str());
            SD.rmdir(fullPath.c_str());
        } else {
            SD.remove(fullPath.c_str());
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

bool SDStore::hasExistingData() {
    if (!_ready) return false;
    if (SD.exists(SD_PATH_USER_CONFIG)) return true;
    if (SD.exists(SD_PATH_IDENTITY)) return true;
    File dir = SD.open("/ratdeck/messages");
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        bool found = (bool)entry;
        if (entry) entry.close();
        dir.close();
        if (found) return true;
    }
    return false;
}

bool SDStore::formatForRsDeck() {
    if (!_ready) return false;
    Serial.println("[SD] Creating rsDeck legacy /ratdeck/ directory structure...");
    bool ok = true;
    ok &= ensureDir("/ratdeck");
    ok &= ensureDir("/ratdeck/config");
    ok &= ensureDir("/ratdeck/messages");
    ok &= ensureDir("/ratdeck/contacts");
    ok &= ensureDir("/ratdeck/identity");
    ok &= ensureDir("/ratdeck/transport");
    if (ok) Serial.println("[SD] Directory structure ready");
    return ok;
}
