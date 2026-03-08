#include "FlashStore.h"

bool FlashStore::begin() {
    // Partition label must match partitions_16MB.csv ("littlefs")
    // Arduino ESP32 LittleFS defaults to "spiffs" label, so we must specify it
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("[FLASH] LittleFS mount failed, formatting...");
        LittleFS.format();
        if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
            Serial.println("[FLASH] LittleFS failed after format!");
            return false;
        }
    }
    _ready = true;

    ensureDir("/identity");
    ensureDir("/transport");
    ensureDir("/config");
    ensureDir("/contacts");
    ensureDir("/messages");

    Serial.printf("[FLASH] LittleFS ready, total=%lu, used=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());
    return true;
}

void FlashStore::end() {
    LittleFS.end();
    _ready = false;
}

bool FlashStore::ensureDir(const char* path) {
    if (!_ready) return false;
    if (LittleFS.exists(path)) return true;
    // Create parent directories recursively
    String pathStr = String(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = pathStr.substring(0, lastSlash);
        if (!LittleFS.exists(parent.c_str())) {
            ensureDir(parent.c_str());
        }
    }
    return LittleFS.mkdir(path);
}

bool FlashStore::exists(const char* path) {
    if (!_ready) return false;
    return LittleFS.exists(path);
}

bool FlashStore::remove(const char* path) {
    if (!_ready) return false;
    return LittleFS.remove(path);
}

bool FlashStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    if (!_ready) return false;

    String tmpPath = String(path) + ".tmp";
    String bakPath = String(path) + ".bak";

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) return false;
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    File verify = LittleFS.open(tmpPath.c_str(), "r");
    if (!verify || verify.size() != len) {
        if (verify) verify.close();
        LittleFS.remove(tmpPath.c_str());
        return false;
    }
    verify.close();

    if (LittleFS.exists(path)) {
        LittleFS.remove(bakPath.c_str());
        LittleFS.rename(path, bakPath.c_str());
    }

    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        if (LittleFS.exists(bakPath.c_str())) {
            LittleFS.rename(bakPath.c_str(), path);
        }
        return false;
    }

    // Clean up backup file after successful write
    LittleFS.remove(bakPath.c_str());

    return true;
}

bool FlashStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    if (!_ready) return false;

    File f = LittleFS.open(path, "r");
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return false;
        Serial.printf("[FLASH] Restored from backup: %s\n", path);
    }

    bytesRead = f.readBytes((char*)buffer, maxLen);
    f.close();
    return bytesRead > 0;
}

bool FlashStore::writeString(const char* path, const String& data) {
    return writeAtomic(path, (const uint8_t*)data.c_str(), data.length());
}

String FlashStore::readString(const char* path) {
    if (!_ready) return "";
    File f = LittleFS.open(path, "r");
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return "";
    }
    String result = f.readString();
    f.close();
    return result;
}

bool FlashStore::format() {
    Serial.println("[FLASH] Formatting LittleFS...");
    LittleFS.end();
    bool ok = LittleFS.format();
    if (ok) {
        ok = begin();
    }
    return ok;
}
