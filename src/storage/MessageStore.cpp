#include "MessageStore.h"
#include "config/Config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// Helper: check if filename ends with ".json"
static bool isJsonFile(const char* name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

bool MessageStore::begin(FlashStore* flash, SDStore* sd) {
    _flash = flash;
    _sd = sd;
    _flash->ensureDir(PATH_MESSAGES);

    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratputer");
        _sd->ensureDir("/ratputer/messages");
        migrateFlashToSD();
    }

    migrateTruncatedDirs();
    refreshConversations();
    Serial.printf("[MSGSTORE] %d conversations found\n", (int)_conversations.size());
    return true;
}

void MessageStore::migrateFlashToSD() {
    if (!_sd || !_sd->isReady() || !_flash) return;

    File dir = LittleFS.open(PATH_MESSAGES);
    if (!dir || !dir.isDirectory()) return;

    int migrated = 0;
    File peerDir = dir.openNextFile();
    while (peerDir) {
        if (peerDir.isDirectory()) {
            std::string peerHex = peerDir.name();
            String sdDir = sdConversationDir(peerHex);
            _sd->ensureDir(sdDir.c_str());

            File entry = peerDir.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && isJsonFile(entry.name())) {
                    String sdPath = sdDir + "/" + entry.name();
                    if (!_sd->exists(sdPath.c_str())) {
                        size_t size = entry.size();
                        if (size > 0 && size < 4096) {
                            String json = entry.readString();
                            _sd->writeString(sdPath.c_str(), json);
                            migrated++;
                            yield();
                        }
                    }
                }
                entry = peerDir.openNextFile();
            }
            enforceFlashLimit(peerHex);
        }
        peerDir = dir.openNextFile();
    }

    if (migrated > 0) {
        Serial.printf("[MSGSTORE] Migrated %d messages from flash to SD\n", migrated);
    }
}

// Migrate old 16-char truncated directories to full 32-char hex names
void MessageStore::migrateTruncatedDirs() {
    auto migrateInDir = [&](auto openFn, auto renameFn, auto readStringFn, const char* basePath) {
        File dir = openFn(basePath);
        if (!dir || !dir.isDirectory()) return;

        // Collect dirs that need renaming (can't rename while iterating)
        std::vector<std::pair<String, String>> renames; // old path -> new path

        File entry = dir.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                std::string dirName = entry.name();
                // Old dirs are exactly 16 hex chars; new ones are 32
                if (dirName.length() == 16) {
                    // Read first JSON file inside to get the full hash
                    String oldDir = String(basePath) + dirName.c_str();
                    File inner = openFn(oldDir.c_str());
                    if (inner && inner.isDirectory()) {
                        File jsonFile = inner.openNextFile();
                        std::string fullHash;
                        while (jsonFile) {
                            if (!jsonFile.isDirectory() && isJsonFile(jsonFile.name())) {
                                String jsonPath = oldDir + "/" + jsonFile.name();
                                String json = readStringFn(jsonPath.c_str());
                                if (json.length() > 0) {
                                    JsonDocument doc;
                                    if (!deserializeJson(doc, json)) {
                                        // Use src for incoming, dst for outgoing
                                        bool incoming = doc["incoming"] | false;
                                        std::string hash = incoming ?
                                            (doc["src"] | "") : (doc["dst"] | "");
                                        if (hash.length() == 32) {
                                            fullHash = hash;
                                        }
                                    }
                                }
                                jsonFile.close();
                                break;
                            }
                            jsonFile.close();
                            jsonFile = inner.openNextFile();
                        }
                        inner.close();

                        if (!fullHash.empty() && fullHash.substr(0, 16) == dirName) {
                            String newDir = String(basePath) + fullHash.c_str();
                            renames.push_back({oldDir, newDir});
                        }
                    }
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();

        for (auto& [oldPath, newPath] : renames) {
            if (renameFn(oldPath.c_str(), newPath.c_str())) {
                Serial.printf("[MSGSTORE] Migrated %s -> %s\n", oldPath.c_str(), newPath.c_str());
            }
        }
    };

    // Migrate flash directories
    migrateInDir(
        [](const char* p) { return LittleFS.open(p); },
        [](const char* a, const char* b) { return LittleFS.rename(a, b); },
        [this](const char* p) { return _flash ? _flash->readString(p) : String(""); },
        PATH_MESSAGES
    );

    // Migrate SD directories
    if (_sd && _sd->isReady()) {
        migrateInDir(
            [this](const char* p) { return _sd->openDir(p); },
            [](const char* a, const char* b) { return SD.rename(a, b); },
            [this](const char* p) { return _sd->readString(p); },
            SD_PATH_MESSAGES
        );
    }
}

void MessageStore::refreshConversations() {
    _conversations.clear();

    if (_sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    _conversations.push_back(entry.name());
                }
                entry = dir.openNextFile();
            }
        }
    }

    File dir = LittleFS.open(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                std::string name = entry.name();
                bool found = false;
                for (auto& c : _conversations) {
                    if (c == name) { found = true; break; }
                }
                if (!found) _conversations.push_back(name);
            }
            entry = dir.openNextFile();
        }
    }
}

bool MessageStore::saveMessage(const LXMFMessage& msg) {
    if (!_flash) return false;

    std::string peerHex = msg.incoming ?
        msg.sourceHash.toHex() : msg.destHash.toHex();

    JsonDocument doc;
    doc["src"] = msg.sourceHash.toHex();
    doc["dst"] = msg.destHash.toHex();
    doc["ts"] = msg.timestamp;
    doc["content"] = msg.content;
    doc["title"] = msg.title;
    doc["incoming"] = msg.incoming;
    doc["status"] = (int)msg.status;
    doc["read"] = msg.incoming ? msg.read : true;
    if (msg.messageId.size() > 0) {
        doc["msgid"] = msg.messageId.toHex();
    }

    String json;
    serializeJson(doc, json);

    // Use messageId hash prefix in filename for dedup (same message = same file)
    char filename[64];
    if (msg.messageId.size() > 0) {
        std::string idHex = msg.messageId.toHex();
        snprintf(filename, sizeof(filename), "%s_%c.json",
                 idHex.substr(0, 16).c_str(),
                 msg.incoming ? 'i' : 'o');
    } else {
        snprintf(filename, sizeof(filename), "%lu_%c.json",
                 (unsigned long)(msg.timestamp * 1000),
                 msg.incoming ? 'i' : 'o');
    }

    bool sdOk = false;
    bool flashOk = false;

    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        _sd->ensureDir(sdDir.c_str());
        String sdPath = sdDir + "/" + filename;
        sdOk = _sd->writeString(sdPath.c_str(), json);
    }

    String flashDir = conversationDir(peerHex);
    _flash->ensureDir(flashDir.c_str());
    String flashPath = flashDir + "/" + filename;
    flashOk = _flash->writeString(flashPath.c_str(), json);

    bool found = false;
    for (auto& c : _conversations) {
        if (c == peerHex) { found = true; break; }
    }
    if (!found) _conversations.push_back(peerHex);

    if (sdOk) enforceSDLimit(peerHex);
    if (flashOk) enforceFlashLimit(peerHex);

    return sdOk || flashOk;
}

std::vector<LXMFMessage> MessageStore::loadConversation(const std::string& peerHex) const {
    std::vector<LXMFMessage> messages;

    auto loadFromDir = [&](File& d) {
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                size_t size = entry.size();
                if (size > 0 && size < 4096) {
                    String json = entry.readString();
                    JsonDocument doc;
                    if (!deserializeJson(doc, json)) {
                        LXMFMessage msg;
                        std::string srcHex = doc["src"] | "";
                        std::string dstHex = doc["dst"] | "";
                        if (!srcHex.empty()) {
                            msg.sourceHash = RNS::Bytes();
                            msg.sourceHash.assignHex(srcHex.c_str());
                        }
                        if (!dstHex.empty()) {
                            msg.destHash = RNS::Bytes();
                            msg.destHash.assignHex(dstHex.c_str());
                        }
                        msg.timestamp = doc["ts"] | 0.0;
                        msg.content = doc["content"] | "";
                        msg.title = doc["title"] | "";
                        msg.incoming = doc["incoming"] | false;
                        msg.status = (LXMFStatus)(doc["status"] | 0);
                        msg.read = doc["read"] | false;
                        messages.push_back(msg);
                    }
                }
            }
            entry = d.openNextFile();
        }
    };

    bool loadedFromSD = false;
    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            loadFromDir(d);
            loadedFromSD = true;
        }
    }

    if (!loadedFromSD && _flash) {
        String dir = conversationDir(peerHex);
        File d = LittleFS.open(dir);
        if (d && d.isDirectory()) {
            loadFromDir(d);
        }
    }

    std::sort(messages.begin(), messages.end(),
              [](const LXMFMessage& a, const LXMFMessage& b) {
                  return a.timestamp < b.timestamp;
              });

    return messages;
}

int MessageStore::messageCount(const std::string& peerHex) const {
    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            int count = 0;
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && isJsonFile(entry.name())) count++;
                entry = d.openNextFile();
            }
            return count;
        }
    }
    String dir = conversationDir(peerHex);
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return 0;
    int count = 0;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory() && isJsonFile(entry.name())) count++;
        entry = d.openNextFile();
    }
    return count;
}

bool MessageStore::deleteConversation(const std::string& peerHex) {
    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                String path = sdDir + "/" + entry.name();
                entry.close();
                _sd->remove(path.c_str());
                entry = d.openNextFile();
            }
        }
        _sd->removeDir(sdDir.c_str());
    }

    String dir = conversationDir(peerHex);
    File d = LittleFS.open(dir);
    if (d && d.isDirectory()) {
        File entry = d.openNextFile();
        while (entry) {
            String path = String(dir) + "/" + entry.name();
            entry.close();
            LittleFS.remove(path);
            entry = d.openNextFile();
        }
    }
    LittleFS.rmdir(dir);

    _conversations.erase(
        std::remove(_conversations.begin(), _conversations.end(), peerHex),
        _conversations.end());
    return true;
}

void MessageStore::markConversationRead(const std::string& peerHex) {
    auto markInDir = [&](auto openFn, auto writeFn, const String& dir) {
        File d = openFn(dir.c_str());
        if (!d || !d.isDirectory()) return;
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                size_t size = entry.size();
                if (size > 0 && size < 4096) {
                    String json = entry.readString();
                    JsonDocument doc;
                    if (!deserializeJson(doc, json)) {
                        bool incoming = doc["incoming"] | false;
                        bool isRead = doc["read"] | false;
                        if (incoming && !isRead) {
                            doc["read"] = true;
                            String updated;
                            serializeJson(doc, updated);
                            String path = dir + "/" + entry.name();
                            writeFn(path.c_str(), updated);
                        }
                    }
                }
            }
            entry = d.openNextFile();
        }
    };

    if (_sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        markInDir([&](const char* p) { return _sd->openDir(p); },
                  [&](const char* p, const String& d) { _sd->writeString(p, d); return true; },
                  sdDir);
    }

    if (_flash) {
        String dir = conversationDir(peerHex);
        markInDir([](const char* p) { return LittleFS.open(p); },
                  [&](const char* p, const String& d) { _flash->writeString(p, d); return true; },
                  dir);
    }
}

String MessageStore::conversationDir(const std::string& peerHex) const {
    return String(PATH_MESSAGES) + peerHex.c_str();
}

String MessageStore::sdConversationDir(const std::string& peerHex) const {
    return String(SD_PATH_MESSAGES) + peerHex.c_str();
}

void MessageStore::enforceFlashLimit(const std::string& peerHex) {
    String dir = conversationDir(peerHex);
    std::vector<String> files;
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            if (isJsonFile(entry.name())) {
                files.push_back(String(dir) + "/" + entry.name());
            } else {
                // Clean up stale .bak/.tmp files
                String junk = String(dir) + "/" + entry.name();
                LittleFS.remove(junk);
            }
        }
        entry = d.openNextFile();
    }
    int limit = (_sd && _sd->isReady()) ? FLASH_MSG_CACHE_LIMIT : RATDECK_MAX_MESSAGES_PER_CONV;
    if ((int)files.size() <= limit) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - limit;
    for (int i = 0; i < excess; i++) {
        LittleFS.remove(files[i]);
    }
}

void MessageStore::enforceSDLimit(const std::string& peerHex) {
    if (!_sd || !_sd->isReady()) return;
    String dir = sdConversationDir(peerHex);
    std::vector<String> files;
    File d = _sd->openDir(dir.c_str());
    if (!d || !d.isDirectory()) return;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            if (isJsonFile(entry.name())) {
                files.push_back(dir + "/" + entry.name());
            } else {
                // Clean up stale .bak/.tmp files
                String junk = dir + "/" + entry.name();
                _sd->remove(junk.c_str());
            }
        }
        entry = d.openNextFile();
    }
    if ((int)files.size() <= RATDECK_MAX_MESSAGES_PER_CONV) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - RATDECK_MAX_MESSAGES_PER_CONV;
    for (int i = 0; i < excess; i++) {
        _sd->remove(files[i].c_str());
    }
}
