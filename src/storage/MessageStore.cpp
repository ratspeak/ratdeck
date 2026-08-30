#include "MessageStore.h"
#include "config/Config.h"
#include "platform/CoreSync.h"
#include "util/PerfTrace.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Helper: check if filename ends with ".json"
static bool isJsonFile(const char* name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

static bool hasDirectionSuffix(const String& name, char suffix) {
    int len = name.length();
    return len >= 7 && name[len - 6] == suffix;
}

static bool isPendingStatus(LXMFStatus status) {
    return status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING;
}

static uint32_t counterFromFilename(const String& name) {
    return (uint32_t)strtoul(name.c_str(), nullptr, 10);
}

bool MessageStore::begin(FlashStore* flash, SDStore* sd, bool externalStorageEnabled) {
    _flash = flash;
    _sd = sd;
    _externalStorageEnabled = externalStorageEnabled;
    unsigned long beginMs = millis();
    _flash->ensureDir(PATH_MESSAGES);

    unsigned long phaseMs = millis();
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        _sd->ensureDir("/ratdeck");
        _sd->ensureDir("/ratdeck/messages");
        migrateFlashToSD();
    }
    unsigned long migrateMs = millis() - phaseMs;

    phaseMs = millis();
    migrateTruncatedDirs();
    unsigned long truncMs = millis() - phaseMs;

    phaseMs = millis();
    initReceiveCounter();
    unsigned long counterMs = millis() - phaseMs;

    phaseMs = millis();
    refreshConversations();
    unsigned long refreshMs = millis() - phaseMs;

    phaseMs = millis();
    buildSummaries();
    unsigned long summaryMs = millis() - phaseMs;
    unsigned long totalMs = millis() - beginMs;
#if RSDECK_PERF_TRACE
    Serial.printf("[PERF] MSG begin: total=%lums migrate=%lums trunc=%lums counter=%lums refresh=%lums summaries=%lums convs=%d summary_count=%d ext=%s sd=%s\n",
                  totalMs, migrateMs, truncMs, counterMs, refreshMs, summaryMs,
                  (int)_conversations.size(), (int)_summaries.size(),
                  _externalStorageEnabled ? "on" : "off",
                  (_sd && _sd->isReady()) ? "ready" : "no");
#endif
    Serial.printf("[MSGSTORE] %d conversations found, receive counter=%lu\n",
                  (int)_conversations.size(), (unsigned long)_nextReceiveCounter);
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

void MessageStore::initReceiveCounter() {
    Preferences prefs;
    prefs.begin("ratdeck_msg", true);
    _nextReceiveCounter = prefs.getUInt("msgctr", 0);
    prefs.end();

    if (_nextReceiveCounter > 0) {
        Serial.printf("[MSGSTORE] receive counter=%lu (from NVS)\n",
                      (unsigned long)_nextReceiveCounter);
        return;
    }

    // NVS has no counter — scan existing files to find highest prefix (first boot only)
    uint32_t maxPrefix = 0;

    auto scanDir = [&](File& dir) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = entry.name();
                unsigned long val = strtoul(name.c_str(), nullptr, 10);
                if (val > maxPrefix && val < 1000000000) maxPrefix = (uint32_t)val;
            }
            entry = dir.openNextFile();
        }
    };

    // Scan SD conversations
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File peerDir = dir.openNextFile();
            while (peerDir) {
                if (peerDir.isDirectory()) scanDir(peerDir);
                peerDir = dir.openNextFile();
            }
        }
    }

    // Scan flash conversations
    File dir = LittleFS.open(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File peerDir = dir.openNextFile();
        while (peerDir) {
            if (peerDir.isDirectory()) scanDir(peerDir);
            peerDir = dir.openNextFile();
        }
    }

    _nextReceiveCounter = maxPrefix + 1;

    Preferences p;
    p.begin("ratdeck_msg", false);
    p.putUInt("msgctr", _nextReceiveCounter);
    p.end();

    Serial.printf("[MSGSTORE] Initialized receive counter to %lu from existing files\n",
                  (unsigned long)_nextReceiveCounter);
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
                    String oldDir = String(basePath) + "/" + dirName.c_str();
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
                            String newDir = String(basePath) + "/" + fullHash.c_str();
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
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
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

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
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

bool MessageStore::saveMessage(LXMFMessage& msg) {
    if (!_flash) return false;
    unsigned long startMs = millis();

    std::string peerHex = msg.incoming ?
        msg.sourceHash.toHex() : msg.destHash.toHex();

    unsigned long serializeStartMs = millis();
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
    unsigned long serializeMs = millis() - serializeStartMs;
    size_t jsonBytes = json.length();

    // Counter-based filename: unique, monotonic, sorts correctly
    uint32_t counter = _nextReceiveCounter++;
    msg.savedCounter = counter;
    char filename[64];
    snprintf(filename, sizeof(filename), "%013lu_%c.json",
             (unsigned long)counter, msg.incoming ? 'i' : 'o');

    // Persist counter to NVS
    unsigned long nvsStartMs = millis();
    {
        Preferences p;
        p.begin("ratdeck_msg", false);
        p.putUInt("msgctr", _nextReceiveCounter);
        p.end();
    }
    unsigned long nvsMs = millis() - nvsStartMs;

    bool sdOk = false;
    bool flashOk = false;
    bool sdAttempted = false;
    unsigned long sdMs = 0;
    unsigned long flashMs = 0;

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        sdAttempted = true;
        unsigned long sdStartMs = millis();
        String sdDir = sdConversationDir(peerHex);
        _sd->ensureDir(sdDir.c_str());
        String sdPath = sdDir + "/" + filename;
        sdOk = _sd->writeString(sdPath.c_str(), json);
        sdMs = millis() - sdStartMs;
    }

    unsigned long flashStartMs = millis();
    String flashDir = conversationDir(peerHex);
    _flash->ensureDir(flashDir.c_str());
    String flashPath = flashDir + "/" + filename;
    flashOk = _flash->writeString(flashPath.c_str(), json);
    flashMs = millis() - flashStartMs;
    bool saved = sdOk || flashOk;

    bool found = false;
    for (auto& c : _conversations) {
        if (c == peerHex) { found = true; break; }
    }
    if (!found) _conversations.push_back(peerHex);

    unsigned long enforceStartMs = millis();
    if (sdOk) enforceSDLimit(peerHex);
    if (flashOk) enforceFlashLimit(peerHex);
    unsigned long enforceMs = millis() - enforceStartMs;

    // Update summary cache
    unsigned long summaryStartMs = millis();
    {
        auto& s = _summaries[peerHex];
        s.lastTimestamp = msg.timestamp;
        s.lastIncoming = msg.incoming;
        std::string prefix = msg.incoming ? "Them: " : "You: ";
        std::string content = msg.content;
        if (content.size() > 15) content = content.substr(0, 15) + "...";
        s.lastPreview = prefix + content;
        s.totalCount++;
        if (msg.incoming && !msg.read) s.unreadCount++;
        if (!msg.incoming) {
            s.hasOutgoing = true;
            s.lastOutgoingStatus = msg.status;
            s.lastOutgoingCounter = counter;
            if (isPendingStatus(msg.status)) s.hasPending = true;
            if (msg.status == LXMFStatus::FAILED) s.hasFailed = true;
            if (isPendingStatus(msg.status) && s.pendingCount < UINT16_MAX) s.pendingCount++;
            if (msg.status == LXMFStatus::FAILED && s.failedCount < UINT16_MAX) s.failedCount++;
        }
        int limit = sdOk ? RSDECK_MAX_MESSAGES_PER_CONV
                         : ((_externalStorageEnabled && _sd && _sd->isReady()) ? FLASH_MSG_CACHE_LIMIT : RSDECK_MAX_MESSAGES_PER_CONV);
        if (s.totalCount > limit) {
            rebuildSummary(peerHex);
        }
    }
    unsigned long summaryMs = millis() - summaryStartMs;

    if (saved) bumpRevision();
    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS) ||
        PerfTrace::shouldLog(sdMs, RSDECK_PERF_WRITE_TRACE_MS) ||
        PerfTrace::shouldLog(flashMs, RSDECK_PERF_WRITE_TRACE_MS) ||
        !saved) {
        Serial.printf("[PERF] MSG save: peer=%s dir=%c bytes=%u counter=%lu sd=%s/%lums flash=%s/%lums serialize=%lums nvs=%lums enforce=%lums summary=%lums total=%lums\n",
                      peerHex.substr(0, 8).c_str(), msg.incoming ? 'i' : 'o',
                      (unsigned)jsonBytes, (unsigned long)counter,
                      sdAttempted ? (sdOk ? "ok" : "fail") : "skip", sdMs,
                      flashOk ? "ok" : "fail", flashMs,
                      serializeMs, nvsMs, enforceMs, summaryMs, elapsed);
    }
    return saved;
}

std::vector<LXMFMessage> MessageStore::loadConversation(const std::string& peerHex) const {
    return loadConversationTail(peerHex, 0);
}

std::vector<LXMFMessage> MessageStore::loadConversationTail(const std::string& peerHex, size_t maxMessages) const {
    std::vector<LXMFMessage> messages;
    unsigned long startMs = millis();
    unsigned long collectMs = 0;
    unsigned long sortMs = 0;
    unsigned long readMs = 0;
    unsigned long parseMs = 0;
    int filesSeen = 0;
    int parsedMessages = 0;
    int parseFailures = 0;
    size_t bytesRead = 0;
    const char* backend = "none";

    auto loadFromDir = [&](File& d, auto readFileFn, const char* source) {
        backend = source;
        // Collect filenames first, then sort alphabetically (counter prefix = insertion order)
        std::vector<String> filenames;
        unsigned long phaseMs = millis();
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                filenames.push_back(entry.name());
            }
            entry = d.openNextFile();
        }
        collectMs += millis() - phaseMs;
        filesSeen += (int)filenames.size();

        phaseMs = millis();
        std::sort(filenames.begin(), filenames.end());
        sortMs += millis() - phaseMs;

        size_t startIndex = 0;
        if (maxMessages > 0 && filenames.size() > maxMessages) {
            startIndex = filenames.size() - maxMessages;
        }

        for (size_t i = startIndex; i < filenames.size(); i++) {
            const auto& fname = filenames[i];
            unsigned long readStartMs = millis();
            String json = readFileFn(fname);
            readMs += millis() - readStartMs;
            if (json.length() == 0) continue;
            bytesRead += json.length();
            JsonDocument doc;
            unsigned long parseStartMs = millis();
            DeserializationError err = deserializeJson(doc, json);
            parseMs += millis() - parseStartMs;
            if (!err) {
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
                msg.savedCounter = counterFromFilename(fname);
                std::string msgIdHex = doc["msgid"] | "";
                if (!msgIdHex.empty()) {
                    msg.messageId = RNS::Bytes();
                    msg.messageId.assignHex(msgIdHex.c_str());
                }
                messages.push_back(msg);
                parsedMessages++;
            } else {
                parseFailures++;
            }
        }
    };

    bool loadedFromSD = false;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            loadFromDir(d, [&](const String& fname) {
                String path = sdDir + "/" + fname;
                return _sd->readString(path.c_str());
            }, "sd");
            loadedFromSD = true;
        }
    }

    if (!loadedFromSD && _flash) {
        String dir = conversationDir(peerHex);
        File d = LittleFS.open(dir);
        if (d && d.isDirectory()) {
            loadFromDir(d, [&](const String& fname) {
                String path = dir + "/" + fname;
                File f = LittleFS.open(path);
                if (f && !f.isDirectory()) {
                    size_t size = f.size();
                    if (size > 0 && size < 4096) return f.readString();
                }
                return String("");
            }, "flash");
        }
    }

    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS) || filesSeen > 64) {
        Serial.printf("[PERF] MSG loadConversation: peer=%s backend=%s files=%d msgs=%d cap=%u bytes=%u collect=%lums sort=%lums read=%lums parse=%lums parse_fail=%d total=%lums\n",
                      peerHex.substr(0, 8).c_str(), backend, filesSeen,
                      parsedMessages, (unsigned)maxMessages, (unsigned)bytesRead,
                      collectMs, sortMs, readMs, parseMs, parseFailures, elapsed);
    }
    return messages;
}

std::vector<LXMFMessage> MessageStore::loadPendingOutgoing() const {
    std::vector<LXMFMessage> pending;
    for (const auto& peerHex : _conversations) {
        std::vector<LXMFMessage> messages = loadConversation(peerHex);
        for (auto& msg : messages) {
            if (msg.incoming) continue;
            if (msg.status == LXMFStatus::QUEUED || msg.status == LXMFStatus::SENDING) {
                msg.status = LXMFStatus::QUEUED;
                pending.push_back(msg);
            }
        }
    }
    std::sort(pending.begin(), pending.end(), [](const LXMFMessage& a, const LXMFMessage& b) {
        return a.savedCounter < b.savedCounter;
    });
    return pending;
}

std::vector<std::string> MessageStore::loadRecentMessageIds(size_t maxIds) const {
    std::vector<std::pair<uint32_t, std::string>> ordered;
    for (const auto& peerHex : _conversations) {
        std::vector<LXMFMessage> messages = loadConversation(peerHex);
        for (const auto& msg : messages) {
            if (msg.messageId.size() == 0) continue;
            ordered.push_back({msg.savedCounter, msg.messageId.toHex()});
        }
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    if (ordered.size() > maxIds) {
        ordered.erase(ordered.begin(), ordered.end() - maxIds);
    }

    std::set<std::string> seen;
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (const auto& item : ordered) {
        if (seen.insert(item.second).second) ids.push_back(item.second);
    }
    return ids;
}

int MessageStore::messageCount(const std::string& peerHex) const {
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
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
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
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
    _summaries.erase(peerHex);
    bumpRevision();
    return true;
}

void MessageStore::markConversationRead(const std::string& peerHex) {
    unsigned long startMs = millis();
    int scannedFiles = 0;
    int rewrittenFiles = 0;
    int sdWrites = 0;
    int flashWrites = 0;
    size_t rewrittenBytes = 0;
    unsigned long collectMs = 0;
    unsigned long readMs = 0;
    unsigned long parseMs = 0;
    unsigned long writeMs = 0;
    auto markInDir = [&](auto openFn, auto writeFn, const String& dir, const char* backend) {
        // Collect only incoming (_i.json) filenames
        std::vector<String> incomingFiles;
        File d = openFn(dir.c_str());
        if (!d || !d.isDirectory()) return;
        unsigned long phaseMs = millis();
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                String name = entry.name();
                // Check for _i.json suffix (incoming)
                if (hasDirectionSuffix(name, 'i')) {
                    incomingFiles.push_back(name);
                }
            }
            entry = d.openNextFile();
        }
        collectMs += millis() - phaseMs;

        // Sort descending (newest first) to stop early at first already-read
        std::sort(incomingFiles.begin(), incomingFiles.end(),
                  [](const String& a, const String& b) { return a > b; });

        for (const auto& fname : incomingFiles) {
            String path = dir + "/" + fname;
            // Read file via the appropriate storage
            String json;
            unsigned long readStartMs = millis();
            File f = openFn(path.c_str());
            if (f && !f.isDirectory()) {
                size_t size = f.size();
                if (size > 0 && size < 4096) json = f.readString();
                f.close();
            }
            readMs += millis() - readStartMs;
            if (json.length() == 0) continue;
            scannedFiles++;

            JsonDocument doc;
            unsigned long parseStartMs = millis();
            DeserializationError err = deserializeJson(doc, json);
            parseMs += millis() - parseStartMs;
            if (err) continue;
            bool isRead = doc["read"] | false;
            if (isRead) break; // all older must be read too
            doc["read"] = true;
            String updated;
            serializeJson(doc, updated);
            unsigned long writeStartMs = millis();
            writeFn(path.c_str(), updated);
            writeMs += millis() - writeStartMs;
            rewrittenBytes += updated.length();
            rewrittenFiles++;
            if (backend && backend[0] == 's') sdWrites++;
            else flashWrites++;
        }
    };

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
        String sdDir = sdConversationDir(peerHex);
        markInDir([&](const char* p) { return _sd->openDir(p); },
                  [&](const char* p, const String& d) { _sd->writeString(p, d); return true; },
                  sdDir, "sd");
    }

    if (_flash) {
        String dir = conversationDir(peerHex);
        markInDir([](const char* p) { return LittleFS.open(p); },
                  [&](const char* p, const String& d) { _flash->writeString(p, d); return true; },
                  dir, "flash");
    }

    bool changed = false;
    auto it = _summaries.find(peerHex);
    if (it != _summaries.end()) {
        changed = it->second.unreadCount > 0;
        it->second.unreadCount = 0;
    } else {
        rebuildSummary(peerHex);
        changed = true;
    }
    if (changed) bumpRevision();
    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS) || rewrittenFiles > 0) {
        Serial.printf("[PERF] MSG markRead: peer=%s scanned=%d wrote=%d sd_writes=%d flash_writes=%d bytes=%u collect=%lums read=%lums parse=%lums write=%lums total=%lums\n",
                      peerHex.substr(0, 8).c_str(), scannedFiles, rewrittenFiles,
                      sdWrites, flashWrites, (unsigned)rewrittenBytes,
                      collectMs, readMs, parseMs, writeMs, elapsed);
    }
}

bool MessageStore::updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus) {
    char suffix = incoming ? 'i' : 'o';
    LXMFStatus oldStatus = LXMFStatus::DRAFT;
    uint32_t updatedCounter = 0;
    bool capturedOldStatus = false;

    auto updateInDir = [&](auto openFn, auto readFn, auto writeFn, const String& dir) -> bool {
        File d = openFn(dir.c_str());
        if (!d || !d.isDirectory()) return false;

        // Collect matching files (by direction suffix)
        std::vector<String> candidates;
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                String name = entry.name();
                if (hasDirectionSuffix(name, suffix)) {
                    candidates.push_back(name);
                }
            }
            entry = d.openNextFile();
        }

        // Search newest-first for the matching timestamp
        std::sort(candidates.begin(), candidates.end(), [](const String& a, const String& b) { return a > b; });

        for (const auto& fname : candidates) {
            String path = dir + "/" + fname;
            String json = readFn(path.c_str());
            if (json.length() == 0) continue;

            JsonDocument doc;
            if (deserializeJson(doc, json)) continue;

            double ts = doc["ts"] | 0.0;
            if (ts == timestamp) {
                LXMFStatus before = (LXMFStatus)(doc["status"] | 0);
                if (!capturedOldStatus) {
                    oldStatus = before;
                    updatedCounter = counterFromFilename(fname);
                    capturedOldStatus = true;
                }
                doc["status"] = (int)newStatus;
                String updated;
                serializeJson(doc, updated);
                writeFn(path.c_str(), updated);
                return true;
            }
        }
        return false;
    };

    bool updated = false;

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
        String sdDir = sdConversationDir(peerHex);
        updated = updateInDir(
            [&](const char* p) { return _sd->openDir(p); },
            [&](const char* p) { return _sd->readString(p); },
            [&](const char* p, const String& d) { _sd->writeString(p, d); },
            sdDir);
    }

    if (_flash) {
        String dir = conversationDir(peerHex);
        bool flashUpdated = updateInDir(
            [](const char* p) { return LittleFS.open(p); },
            [this](const char* p) { return _flash->readString(p); },
            [this](const char* p, const String& d) { _flash->writeString(p, d); },
            dir);
        updated = updated || flashUpdated;
    }

    if (updated && !incoming) updateSummaryStatus(peerHex, updatedCounter, oldStatus, newStatus);
    else if (updated) bumpRevision();
    return updated;
}

bool MessageStore::updateMessageStatusByCounter(const std::string& peerHex, uint32_t counter, bool incoming, LXMFStatus newStatus) {
    if (counter == 0) return false;  // Not saved yet
    char filename[64];
    snprintf(filename, sizeof(filename), "%013lu_%c.json",
             (unsigned long)counter, incoming ? 'i' : 'o');

    LXMFStatus oldStatus = LXMFStatus::DRAFT;
    bool capturedOldStatus = false;

    auto readModifyWrite = [&](auto readFn, auto writeFn, const String& dir) -> bool {
        String path = dir + "/" + filename;
        String json = readFn(path.c_str());
        if (json.length() == 0) return false;
        JsonDocument doc;
        if (deserializeJson(doc, json)) return false;
        LXMFStatus before = (LXMFStatus)(doc["status"] | 0);
        if (!capturedOldStatus) {
            oldStatus = before;
            capturedOldStatus = true;
        }
        doc["status"] = (int)newStatus;
        String updated;
        serializeJson(doc, updated);
        writeFn(path.c_str(), updated);
        return true;
    };

    bool updated = false;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        updated = readModifyWrite(
            [&](const char* p) { return _sd->readString(p); },
            [&](const char* p, const String& d) { _sd->writeString(p, d); },
            sdDir);
    }
    if (_flash) {
        String dir = conversationDir(peerHex);
        bool flashUpdated = readModifyWrite(
            [this](const char* p) { return _flash->readString(p); },
            [this](const char* p, const String& d) { _flash->writeString(p, d); },
            dir);
        updated = updated || flashUpdated;
    }
    if (updated && !incoming) updateSummaryStatus(peerHex, counter, oldStatus, newStatus);
    else if (updated) bumpRevision();
    return updated;
}

ConversationSummary MessageStore::buildSummaryForPeer(
    const std::string& peerHex,
    std::vector<LXMFMessage>* pendingOut,
    std::vector<std::pair<uint32_t, std::string>>* recentIds) const {
    unsigned long startMs = millis();
    ConversationSummary summary;

    std::vector<String> files;
    auto collectFiles = [&](File& d) {
        File entry = d.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && isJsonFile(entry.name())) {
                files.push_back(entry.name());
            }
            entry = d.openNextFile();
        }
    };

    bool loadedFromSD = false;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        CoreSync::SpiBusGuard busGuard;
        String sdDir = sdConversationDir(peerHex);
        File d = _sd->openDir(sdDir.c_str());
        if (d && d.isDirectory()) {
            collectFiles(d);
            loadedFromSD = true;
        }
    }
    if (!loadedFromSD && _flash) {
        String dir = conversationDir(peerHex);
        File d = LittleFS.open(dir);
        if (d && d.isDirectory()) {
            collectFiles(d);
        }
    }

    summary.totalCount = (int)files.size();
    if (files.empty()) return summary;

    std::sort(files.begin(), files.end());
    String basePath = loadedFromSD ? sdConversationDir(peerHex) : conversationDir(peerHex);
    const bool collectStartup = pendingOut || recentIds;

    auto readJsonFile = [&](const String& path) -> String {
        if (loadedFromSD && _sd && _sd->isReady()) return _sd->readString(path.c_str());
        if (_flash) return _flash->readString(path.c_str());
        return String("");
    };

    bool lastDone = false;
    bool unreadDone = false;
    for (int i = (int)files.size() - 1; i >= 0; i--) {
        const String& fname = files[i];
        const bool incomingFile = hasDirectionSuffix(fname, 'i');
        const bool outgoingFile = hasDirectionSuffix(fname, 'o');
        const bool needIncoming = incomingFile && !unreadDone;
        const bool needOutgoing = outgoingFile;
        const bool needLast = !lastDone;
        if (!collectStartup && !needLast && !needIncoming && !needOutgoing) continue;

        String fjson = readJsonFile(basePath + "/" + fname);
        if (fjson.length() == 0) continue;

        JsonDocument fdoc;
        if (deserializeJson(fdoc, fjson)) continue;
        uint32_t counter = counterFromFilename(fname);

        if (needLast) {
            summary.lastTimestamp = fdoc["ts"] | 0.0;
            std::string content = fdoc["content"] | "";
            summary.lastIncoming = fdoc["incoming"] | false;
            std::string prefix = summary.lastIncoming ? "Them: " : "You: ";
            if (content.size() > 15) content = content.substr(0, 15) + "...";
            summary.lastPreview = prefix + content;
            lastDone = true;
        }

        if (recentIds) {
            std::string msgIdHex = fdoc["msgid"] | "";
            if (!msgIdHex.empty()) {
                recentIds->push_back({counter, msgIdHex});
            }
        }

        if (needIncoming) {
            bool isRead = fdoc["read"] | false;
            if (isRead) unreadDone = true;
            else summary.unreadCount++;
        }

        if (needOutgoing) {
            LXMFStatus status = (LXMFStatus)(fdoc["status"] | 0);
            if (!summary.hasOutgoing) {
                summary.hasOutgoing = true;
                summary.lastOutgoingStatus = status;
                summary.lastOutgoingCounter = counter;
            }
            if (isPendingStatus(status)) {
                summary.hasPending = true;
                if (summary.pendingCount < UINT16_MAX) summary.pendingCount++;
                if (pendingOut) {
                    LXMFMessage msg;
                    std::string srcHex = fdoc["src"] | "";
                    std::string dstHex = fdoc["dst"] | "";
                    if (!srcHex.empty()) {
                        msg.sourceHash = RNS::Bytes();
                        msg.sourceHash.assignHex(srcHex.c_str());
                    }
                    if (!dstHex.empty()) {
                        msg.destHash = RNS::Bytes();
                        msg.destHash.assignHex(dstHex.c_str());
                    }
                    msg.timestamp = fdoc["ts"] | 0.0;
                    msg.content = fdoc["content"] | "";
                    msg.title = fdoc["title"] | "";
                    msg.incoming = fdoc["incoming"] | false;
                    msg.status = status;
                    msg.read = fdoc["read"] | false;
                    msg.savedCounter = counter;
                    std::string msgIdHex = fdoc["msgid"] | "";
                    if (!msgIdHex.empty()) {
                        msg.messageId = RNS::Bytes();
                        msg.messageId.assignHex(msgIdHex.c_str());
                    }
                    pendingOut->push_back(msg);
                }
            }
            if (status == LXMFStatus::FAILED) {
                summary.hasFailed = true;
                if (summary.failedCount < UINT16_MAX) summary.failedCount++;
            }
        }
    }

    unsigned long elapsed = millis() - startMs;
    if (elapsed > 25) {
        Serial.printf("[PERF] MSG summary: %s files=%d unread=%d pending=%u failed=%u in %lums\n",
                      peerHex.substr(0, 8).c_str(), (int)files.size(), summary.unreadCount,
                      (unsigned)summary.pendingCount, (unsigned)summary.failedCount,
                      (unsigned long)elapsed);
    }
    return summary;
}

void MessageStore::rebuildSummary(const std::string& peerHex) {
    _summaries[peerHex] = buildSummaryForPeer(peerHex);
}

void MessageStore::updateSummaryStatus(const std::string& peerHex, uint32_t counter,
                                       LXMFStatus oldStatus, LXMFStatus newStatus) {
    if (oldStatus == newStatus) {
        return;
    }

    auto it = _summaries.find(peerHex);
    if (it == _summaries.end() || counter == 0) {
        rebuildSummary(peerHex);
        bumpRevision();
        return;
    }

    ConversationSummary& s = it->second;
    if (isPendingStatus(oldStatus) && s.pendingCount > 0) s.pendingCount--;
    if (oldStatus == LXMFStatus::FAILED && s.failedCount > 0) s.failedCount--;
    if (isPendingStatus(newStatus) && s.pendingCount < UINT16_MAX) s.pendingCount++;
    if (newStatus == LXMFStatus::FAILED && s.failedCount < UINT16_MAX) s.failedCount++;

    s.hasPending = s.pendingCount > 0;
    s.hasFailed = s.failedCount > 0;
    if (counter >= s.lastOutgoingCounter) {
        s.hasOutgoing = true;
        s.lastOutgoingCounter = counter;
        s.lastOutgoingStatus = newStatus;
    }
    bumpRevision();
}

void MessageStore::buildSummaries() {
    _summaries.clear();
    _startupPendingOutgoing.clear();
    _startupRecentMessageIds.clear();
    std::vector<std::pair<uint32_t, std::string>> recentIds;
    for (const auto& peerHex : _conversations) {
        _summaries[peerHex] = buildSummaryForPeer(peerHex, &_startupPendingOutgoing, &recentIds);
        yield();
    }

    std::sort(_startupPendingOutgoing.begin(), _startupPendingOutgoing.end(),
        [](const LXMFMessage& a, const LXMFMessage& b) {
            return a.savedCounter < b.savedCounter;
        });

    std::sort(recentIds.begin(), recentIds.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::string> seen;
    for (const auto& item : recentIds) {
        if (seen.insert(item.second).second) _startupRecentMessageIds.push_back(item.second);
    }

    Serial.printf("[MSGSTORE] Built summaries for %d conversations\n", (int)_summaries.size());
}

std::vector<std::string> MessageStore::startupRecentMessageIds(size_t maxIds) const {
    if (maxIds == 0 || _startupRecentMessageIds.size() <= maxIds) {
        return _startupRecentMessageIds;
    }
    return std::vector<std::string>(
        _startupRecentMessageIds.end() - maxIds,
        _startupRecentMessageIds.end());
}

const ConversationSummary* MessageStore::getSummary(const std::string& peerHex) const {
    auto it = _summaries.find(peerHex);
    return (it != _summaries.end()) ? &it->second : nullptr;
}

int MessageStore::totalUnreadCount() const {
    int total = 0;
    for (const auto& kv : _summaries) total += kv.second.unreadCount;
    return total;
}

void MessageStore::bumpRevision() {
    _revision++;
    if (_revision == 0) _revision = 1;
}

String MessageStore::conversationDir(const std::string& peerHex) const {
    return String(PATH_MESSAGES) + "/" + peerHex.c_str();
}

String MessageStore::sdConversationDir(const std::string& peerHex) const {
    return String(SD_PATH_MESSAGES) + "/" + peerHex.c_str();
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
    int limit = (_externalStorageEnabled && _sd && _sd->isReady()) ? FLASH_MSG_CACHE_LIMIT : RSDECK_MAX_MESSAGES_PER_CONV;
    if ((int)files.size() <= limit) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - limit;
    for (int i = 0; i < excess; i++) {
        LittleFS.remove(files[i]);
    }
}

void MessageStore::enforceSDLimit(const std::string& peerHex) {
    if (!_externalStorageEnabled || !_sd || !_sd->isReady()) return;
    CoreSync::SpiBusGuard busGuard;
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
    if ((int)files.size() <= RSDECK_MAX_MESSAGES_PER_CONV) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - RSDECK_MAX_MESSAGES_PER_CONV;
    for (int i = 0; i < excess; i++) {
        _sd->remove(files[i].c_str());
    }
}
