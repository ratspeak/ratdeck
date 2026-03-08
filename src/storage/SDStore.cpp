#include "SDStore.h"
#include "config/Config.h"

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

bool SDStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    if (!_ready) return false;

    String tmpPath = String(path) + ".tmp";
    String bakPath = String(path) + ".bak";

    File f = SD.open(tmpPath.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] writeAtomic: failed to open tmp %s\n", tmpPath.c_str());
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        Serial.printf("[SD] writeAtomic: write incomplete (%d/%d)\n", (int)written, (int)len);
        SD.remove(tmpPath.c_str());
        return false;
    }

    File verify = SD.open(tmpPath.c_str(), FILE_READ);
    if (!verify || verify.size() != len) {
        Serial.println("[SD] writeAtomic: verify failed");
        if (verify) verify.close();
        SD.remove(tmpPath.c_str());
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
        return false;
    }
    return true;
}

bool SDStore::writeSimple(const char* path, const uint8_t* data, size_t len) {
    if (!_ready) return false;

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] writeSimple: failed to open %s\n", path);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        Serial.printf("[SD] writeSimple: write incomplete (%d/%d)\n", (int)written, (int)len);
        return false;
    }
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

bool SDStore::wipeRatputer() {
    if (!_ready) return false;
    Serial.println("[SD] Wiping /ratputer/ ...");
    wipeDir("/ratputer/messages");
    wipeDir("/ratputer/contacts");
    wipeDir("/ratputer/identity");
    wipeDir("/ratputer/config");
    wipeDir("/ratputer/transport");
    SD.rmdir("/ratputer");
    Serial.println("[SD] Wipe complete, recreating dirs...");
    return formatForRatputer();
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

bool SDStore::formatForRatputer() {
    if (!_ready) return false;
    Serial.println("[SD] Creating Ratputer directory structure...");
    bool ok = true;
    ok &= ensureDir("/ratputer");
    ok &= ensureDir("/ratputer/config");
    ok &= ensureDir("/ratputer/messages");
    ok &= ensureDir("/ratputer/contacts");
    ok &= ensureDir("/ratputer/identity");
    ok &= ensureDir("/ratputer/transport");
    if (ok) Serial.println("[SD] Directory structure ready");
    return ok;
}
