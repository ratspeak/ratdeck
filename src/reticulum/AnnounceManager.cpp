// Direct port from Ratputer — node discovery and contact persistence
#include "AnnounceManager.h"
#include "config/Config.h"
#include "storage/SDStore.h"
#include "storage/FlashStore.h"
#include "transport/LoRaInterface.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// Skip one MsgPack value at data[pos], return new pos (or len on error)
static size_t mpSkipValue(const uint8_t* data, size_t len, size_t pos) {
    if (pos >= len) return len;
    uint8_t b = data[pos];
    if (b <= 0x7F || b >= 0xE0) return pos + 1;
    if ((b & 0xF0) == 0x80) {
        size_t n = (b & 0x0F) * 2;
        pos++;
        for (size_t j = 0; j < n && pos < len; j++) pos = mpSkipValue(data, len, pos);
        return pos;
    }
    if ((b & 0xF0) == 0x90) {
        size_t n = b & 0x0F;
        pos++;
        for (size_t j = 0; j < n && pos < len; j++) pos = mpSkipValue(data, len, pos);
        return pos;
    }
    if ((b & 0xE0) == 0xA0) return pos + 1 + (b & 0x1F);
    if (b == 0xC0 || b == 0xC2 || b == 0xC3) return pos + 1;
    if (b == 0xC4 && pos + 1 < len) return pos + 2 + data[pos + 1];
    if (b == 0xC5 && pos + 2 < len) return pos + 3 + ((size_t)data[pos + 1] << 8 | data[pos + 2]);
    if (b == 0xCA) return pos + 5;
    if (b == 0xCB) return pos + 9;
    if (b == 0xCC || b == 0xD0) return pos + 2;
    if (b == 0xCD || b == 0xD1) return pos + 3;
    if (b == 0xCE || b == 0xD2) return pos + 5;
    if (b == 0xCF || b == 0xD3) return pos + 9;
    if (b == 0xD9 && pos + 1 < len) return pos + 2 + data[pos + 1];
    if (b == 0xDA && pos + 2 < len) return pos + 3 + ((size_t)data[pos + 1] << 8 | data[pos + 2]);
    return len;
}

// Two-pass: prefer str elements (display name), fall back to bin (NomadNet compat).
static std::string extractMsgPackName(const uint8_t* data, size_t len) {
    if (len < 2) return "";
    uint8_t b = data[0];
    size_t pos = 0;
    size_t arrLen = 0;
    if ((b & 0xF0) == 0x90) { arrLen = b & 0x0F; if (arrLen == 0) return ""; pos = 1; }
    else if (b == 0xDC && len >= 3) { arrLen = ((size_t)data[1] << 8) | data[2]; pos = 3; }
    else return "";

    // Pass 1: scan for first non-empty STR element
    size_t savedPos = pos;
    for (size_t i = 0; i < arrLen && pos < len; i++) {
        b = data[pos];
        size_t slen = 0;
        if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; pos++; }
        else if (b == 0xD9 && pos + 1 < len) { slen = data[pos + 1]; pos += 2; }
        else if (b == 0xDA && pos + 2 < len) { slen = ((size_t)data[pos + 1] << 8) | data[pos + 2]; pos += 3; }
        else { pos = mpSkipValue(data, len, pos); continue; }
        if (slen > 0 && pos + slen <= len) return std::string((const char*)&data[pos], slen);
        pos += slen;
    }

    // Pass 2: scan for first non-empty BIN element (fallback)
    pos = savedPos;
    for (size_t i = 0; i < arrLen && pos < len; i++) {
        b = data[pos];
        size_t slen = 0;
        if (b == 0xC4 && pos + 1 < len) { slen = data[pos + 1]; pos += 2; }
        else if (b == 0xC5 && pos + 2 < len) { slen = ((size_t)data[pos + 1] << 8) | data[pos + 2]; pos += 3; }
        else { pos = mpSkipValue(data, len, pos); continue; }
        if (slen > 0 && pos + slen <= len) return std::string((const char*)&data[pos], slen);
        pos += slen;
    }
    return "";
}

static std::string sanitizeName(const std::string& raw, size_t maxLen = 16) {
    std::string clean;
    clean.reserve(std::min(raw.size(), maxLen));
    for (char c : raw) {
        if (clean.size() >= maxLen) break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'' || c == '/') {
            clean += c;
        }
    }
    size_t start = clean.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = clean.find_last_not_of(' ');
    return clean.substr(start, end - start + 1);
}

AnnounceManager::AnnounceManager(const char* aspectFilter) : RNS::AnnounceHandler(aspectFilter) {
    _nodes.reserve(MAX_NODES);
    _hashIndex.reserve(MAX_NODES);
}

void AnnounceManager::setStorage(SDStore* sd, FlashStore* flash) { _sd = sd; _flash = flash; }

void AnnounceManager::received_announce(
    const RNS::Bytes& destination_hash,
    const RNS::Identity& announced_identity,
    const RNS::Bytes& app_data)
{
    std::string name;
    if (app_data.size() > 0) {
        Serial.printf("[ANNOUNCE-RX] app_data: %d bytes hex: ", (int)app_data.size());
        for (size_t i = 0; i < std::min((size_t)16, app_data.size()); i++)
            Serial.printf("%02X", app_data.data()[i]);
        Serial.println();
        std::string rawName = extractMsgPackName(app_data.data(), app_data.size());
        if (rawName.empty()) {
            bool isText = app_data.size() > 0 && app_data.size() <= 32;
            for (size_t i = 0; isText && i < app_data.size(); i++) {
                uint8_t c = app_data.data()[i];
                if (c < 0x20 || c > 0x7E) isText = false;
            }
            if (isText) {
                rawName = app_data.toString();
            } else {
                Serial.printf("[ANNOUNCE] Unknown app_data format (%d bytes): ", (int)app_data.size());
                for (size_t i = 0; i < std::min((size_t)32, app_data.size()); i++) {
                    Serial.printf("%02X ", app_data.data()[i]);
                }
                Serial.println();
            }
        }
        name = sanitizeName(rawName);
    }
    // Filter out own announces
    if (_localDestHash.size() > 0 && destination_hash == _localDestHash) return;

    // Layer 3: Global announce rate limit — cap application-layer processing
    {
        unsigned long now = millis();
        if (now - _globalAnnounceWindowStart >= 1000) {
            _globalAnnounceWindowStart = now;
            _globalAnnounceCount = 0;
        }
        if (++_globalAnnounceCount > MAX_GLOBAL_ANNOUNCES_PER_SEC) return;
    }

    std::string key = makeKey(destination_hash);
    std::string idHex = announced_identity ? announced_identity.hexhash() : "";

    unsigned long now = millis();
    // O(1) lookup for existing node
    auto it = _hashIndex.find(key);
    if (it != _hashIndex.end()) {
        auto& node = _nodes[it->second];
        if (node.lastSeen != 0 && now >= node.lastSeen &&
            now - node.lastSeen < ANNOUNCE_MIN_INTERVAL_MS) return;
        // Saved contacts own their local alias. Incoming announce app_data is
        // still cached below, but must not reset the user-chosen contact name.
        if (!name.empty() && !node.saved) node.name = name;
        if (!idHex.empty()) node.identityHex = idHex;
        node.lastSeen = now;
        // hops_to() is expensive (linear routing table scan) — only call for saved contacts
        if (node.saved) node.hops = RNS::Transport::hops_to(destination_hash);
        if (_loraIf) { node.rssi = _loraIf->lastRxRssi(); node.snr = _loraIf->lastRxSnr(); }
        // Name cache update — skip expensive toHex() for unnamed re-announces
        if (!name.empty()) {
            std::string destHex = destination_hash.toHex();
            auto nc = _nameCache.find(destHex);
            if (nc == _nameCache.end() || nc->second != name) {
                _nameCache[destHex] = name;
                _nameCacheDirty = true;
            }
        }
        if (!idHex.empty()) {
            _knownDestinationsDirty = true;
        }
        return;
    }

    // New node — toHex needed for log + name cache
    std::string destHex = destination_hash.toHex();
    Serial.printf("[ANNOUNCE] New: %s name=\"%s\"\n", destHex.c_str(), name.c_str());

    if (!name.empty()) {
        auto nc = _nameCache.find(destHex);
        if (nc == _nameCache.end() || nc->second != name) {
            _nameCache[destHex] = name;
            _nameCacheDirty = true;
            // Cap name cache size: prune entries not in _nodes or saved contacts
            if ((int)_nameCache.size() > MAX_NAME_CACHE) {
                for (auto it = _nameCache.begin(); it != _nameCache.end() && (int)_nameCache.size() > MAX_NAME_CACHE; ) {
                    RNS::Bytes h; h.assignHex(it->first.c_str());
                    if (!findNode(h)) {
                        it = _nameCache.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    }

    // Add new node
    if ((int)_nodes.size() >= MAX_NODES) {
        evictStale();
        if ((int)_nodes.size() >= MAX_NODES) {
            uint8_t maxHops = 0;
            unsigned long oldest = ULONG_MAX;
            int evictIdx = -1;
            for (int i = 0; i < (int)_nodes.size(); i++) {
                if (_nodes[i].saved) continue;
                if (_nodes[i].hops > maxHops ||
                    (_nodes[i].hops == maxHops && _nodes[i].lastSeen < oldest)) {
                    maxHops = _nodes[i].hops;
                    oldest = _nodes[i].lastSeen;
                    evictIdx = i;
                }
            }
            if (evictIdx >= 0) {
                // Swap-and-pop for O(1) eviction
                int lastIdx = (int)_nodes.size() - 1;
                if (evictIdx != lastIdx) {
                    std::string swapKey = makeKey(_nodes[lastIdx].hash);
                    _hashIndex[swapKey] = evictIdx;
                    std::swap(_nodes[evictIdx], _nodes[lastIdx]);
                }
                _hashIndex.erase(makeKey(_nodes[lastIdx].hash));
                _nodes.pop_back();
            }
        }
    }
    if ((int)_nodes.size() >= MAX_NODES) return;

    DiscoveredNode node;
    node.hash = destination_hash;
    if (!name.empty()) {
        node.name = name;
    } else {
        auto cached = _nameCache.find(destHex);
        node.name = (cached != _nameCache.end() && !cached->second.empty())
            ? cached->second
            : destHex.substr(0, 12);
    }
    node.identityHex = idHex;
    node.lastSeen = millis();
    // Skip hops_to() for new (unsaved) nodes — resolved when added as contact
    if (_loraIf) { node.rssi = _loraIf->lastRxRssi(); node.snr = _loraIf->lastRxSnr(); }
    _hashIndex[key] = (int)_nodes.size();
    _nodes.push_back(node);
    if (!idHex.empty()) {
        _knownDestinationsDirty = true;
    }
}

void AnnounceManager::loop() {
    unsigned long now = millis();
    if (_contactsDirty && now - _lastContactSave >= CONTACT_SAVE_INTERVAL_MS) {
        _contactsDirty = false;
        _lastContactSave = now;
        saveContacts();
        Serial.println("[ANNOUNCE] Deferred contact save complete");
    }
    if (_nameCacheDirty && now - _lastNameCacheSave >= CONTACT_SAVE_INTERVAL_MS) {
        _nameCacheDirty = false;
        _lastNameCacheSave = now;
        saveNameCache();
    }

    if (_knownDestinationsDirty) {
        if (persistKnownDestinationsAfterAnnounce("announce batch", false)) {
            _knownDestinationsDirty = false;
        }
    }
}

bool AnnounceManager::persistKnownDestinationsAfterAnnounce(const char* reason, bool force) {
    unsigned long now = millis();
    if (!force && _lastKnownDestinationsPersist != 0 &&
        now - _lastKnownDestinationsPersist < KNOWN_DESTINATION_PERSIST_MIN_INTERVAL_MS) {
        return false;
    }

    _lastKnownDestinationsPersist = now;
    unsigned long startMs = millis();
    RNS::Identity::persist_data();
    unsigned long elapsed = millis() - startMs;
    Serial.printf("[ANNOUNCE] Known destinations persisted after %s (force=%s in %lums)\n",
                  reason ? reason : "announce", force ? "yes" : "no", elapsed);
    return true;
}

int AnnounceManager::nodesOnlineSince(unsigned long maxAgeMs) const {
    unsigned long now = millis();
    int count = 0;
    for (const auto& n : _nodes) {
        if (n.lastSeen != 0 && now >= n.lastSeen && now - n.lastSeen <= maxAgeMs) count++;
    }
    return count;
}

const DiscoveredNode* AnnounceManager::findNode(const RNS::Bytes& hash) const {
    auto it = _hashIndex.find(makeKey(hash));
    if (it != _hashIndex.end()) return &_nodes[it->second];
    return nullptr;
}

const DiscoveredNode* AnnounceManager::findNodeByHex(const std::string& hexHash) const {
    RNS::Bytes target;
    target.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(target));
    if (it != _hashIndex.end()) return &_nodes[it->second];
    // Prefix match fallback (for truncated 16-char conversation hashes)
    for (const auto& n : _nodes) {
        std::string nodeHex = n.hash.toHex();
        if (hexHash.length() < nodeHex.length() &&
            nodeHex.substr(0, hexHash.length()) == hexHash) return &n;
    }
    return nullptr;
}

void AnnounceManager::addManualContact(const std::string& hexHash, const std::string& name) {
    RNS::Bytes hash;
    hash.assignHex(hexHash.c_str());
    std::string safeName = sanitizeName(name);
    std::string key = makeKey(hash);

    auto it = _hashIndex.find(key);
    if (it != _hashIndex.end()) {
        auto& node = _nodes[it->second];
        if (!safeName.empty()) node.name = safeName;
        node.saved = true;
        saveContact(node);
        return;
    }

    DiscoveredNode node;
    node.hash = hash;
    if (!safeName.empty()) {
        node.name = safeName;
    } else {
        auto cached = _nameCache.find(hexHash);
        node.name = (cached != _nameCache.end() && !cached->second.empty())
            ? cached->second
            : hexHash.substr(0, 12);
    }
    node.lastSeen = millis();
    node.saved = true;
    _hashIndex[key] = (int)_nodes.size();
    _nodes.push_back(node);
    saveContact(node);
}

void AnnounceManager::evictStale(unsigned long maxAgeMs) {
    unsigned long now = millis();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [now, maxAgeMs](const DiscoveredNode& n) {
            return !n.saved && (n.lastSeen == 0 || now < n.lastSeen || now - n.lastSeen > maxAgeMs);
        }), _nodes.end());
    rebuildIndex();
}

void AnnounceManager::clearTransientNodes() {
    int before = _nodes.size();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [](const DiscoveredNode& n) { return !n.saved; }), _nodes.end());
    int removed = before - (int)_nodes.size();
    if (removed > 0) {
        Serial.printf("[ANNOUNCE] Cleared %d transient nodes\n", removed);
    }
    rebuildIndex();
}

void AnnounceManager::clearAll() {
    _nodes.clear();
    _hashIndex.clear();
    _nameCache.clear();
    _contactsDirty = false;
    _nameCacheDirty = false;
    Serial.println("[ANNOUNCE] Cleared all nodes and name cache");
}

void AnnounceManager::rebuildIndex() {
    _hashIndex.clear();
    for (int i = 0; i < (int)_nodes.size(); i++) {
        _hashIndex[makeKey(_nodes[i].hash)] = i;
    }
}

void AnnounceManager::saveContact(const DiscoveredNode& node) {
    std::string hexHash = node.hash.toHex();
    JsonDocument doc;
    doc["hash"] = hexHash; doc["name"] = node.name;
    String json;
    serializeJson(doc, json);
    String filename = hexHash.substr(0, 16).c_str();
    filename += ".json";
    if (_sd && _sd->isReady()) { _sd->writeString((String(SD_PATH_CONTACTS) + "/" + filename).c_str(), json); }
    if (_flash) { _flash->writeString((String(PATH_CONTACTS) + "/" + filename).c_str(), json); }
}

void AnnounceManager::removeContact(const std::string& hexHash) {
    String filename = hexHash.substr(0, 16).c_str();
    filename += ".json";
    if (_sd && _sd->isReady()) { _sd->remove((String(SD_PATH_CONTACTS) + "/" + filename).c_str()); }
    if (_flash) { _flash->remove((String(PATH_CONTACTS) + "/" + filename).c_str()); }
}

bool AnnounceManager::deleteContact(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= (int)_nodes.size()) return false;
    std::string hexHash = _nodes[nodeIdx].hash.toHex();
    removeContact(hexHash);
    _nameCache.erase(hexHash);
    _nameCacheDirty = true;
    _nodes.erase(_nodes.begin() + nodeIdx);
    rebuildIndex();
    Serial.printf("[ANNOUNCE] Deleted contact %s\n", hexHash.substr(0, 12).c_str());
    return true;
}

void AnnounceManager::loadContacts() {
    int loaded = 0;
    auto loadFromDir = [&](File& dir) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory() && String(entry.name()).endsWith(".json")) {
                size_t size = entry.size();
                if (size > 0 && size < 2048) {
                    String json = entry.readString();
                    JsonDocument doc;
                    if (!deserializeJson(doc, json)) {
                        std::string hexHash = doc["hash"] | "";
                        if (!hexHash.empty()) {
                            RNS::Bytes hash; hash.assignHex(hexHash.c_str());
                            std::string key = makeKey(hash);
                            if (_hashIndex.find(key) == _hashIndex.end()) {
                                DiscoveredNode node;
                                node.hash = hash;
                                node.name = sanitizeName(doc["name"] | "");
                                if (node.name.empty()) node.name = hexHash.substr(0, 12);
                                // Contacts persist address-book data only. Radio/path state
                                // is boot-relative and becomes misleading after restart.
                                node.rssi = 0;
                                node.snr = 0.0f;
                                node.hops = 0;
                                node.lastSeen = 0;
                                node.saved = true;
                                _hashIndex[key] = (int)_nodes.size();
                                _nodes.push_back(node);
                                loaded++;
                            }
                        }
                    }
                }
            }
            entry = dir.openNextFile();
        }
    };
    if (_sd && _sd->isReady()) { File dir = _sd->openDir(SD_PATH_CONTACTS); if (dir && dir.isDirectory()) loadFromDir(dir); }
    if (_flash) { File dir = LittleFS.open(PATH_CONTACTS); if (dir && dir.isDirectory()) loadFromDir(dir); }
    if (loaded > 0) Serial.printf("[ANNOUNCE] Loaded %d saved contacts\n", loaded);
}

void AnnounceManager::saveContacts() {
    for (const auto& n : _nodes) { if (n.saved) saveContact(n); }
}

std::string AnnounceManager::lookupName(const std::string& hexHash) const {
    // Check live nodes first
    const DiscoveredNode* node = findNodeByHex(hexHash);
    if (node && !node->name.empty()) return node->name;
    // Fall back to cached names
    auto it = _nameCache.find(hexHash);
    if (it != _nameCache.end()) return it->second;
    return "";
}

void AnnounceManager::saveNameCache() {
    unsigned long startMs = millis();
    JsonDocument doc;
    for (auto& kv : _nameCache) {
        doc[kv.first] = kv.second;
    }
    String json;
    unsigned long serializeStartMs = millis();
    serializeJson(doc, json);
    unsigned long serializeMs = millis() - serializeStartMs;
    size_t bytes = json.length();
    bool sdOk = false;
    bool flashOk = false;
    unsigned long sdMs = 0;
    unsigned long flashMs = 0;
    if (_sd && _sd->isReady()) {
        unsigned long writeStartMs = millis();
        sdOk = _sd->writeString("/ratdeck/config/names.json", json);
        sdMs = millis() - writeStartMs;
    }
    if (_flash) {
        unsigned long writeStartMs = millis();
        flashOk = _flash->writeString("/config/names.json", json);
        flashMs = millis() - writeStartMs;
    }
    unsigned long elapsed = millis() - startMs;
    Serial.printf("[ANNOUNCE] Name cache saved (%d entries, bytes=%u, serialize=%lums sd=%s/%lums flash=%s/%lums total=%lums)\n",
                  (int)_nameCache.size(), (unsigned)bytes, serializeMs,
                  (_sd && _sd->isReady()) ? (sdOk ? "ok" : "fail") : "skip", sdMs,
                  _flash ? (flashOk ? "ok" : "fail") : "skip", flashMs,
                  elapsed);
}

void AnnounceManager::loadNameCache() {
    String json;
    if (_sd && _sd->isReady()) {
        json = _sd->readString("/ratdeck/config/names.json");
    }
    if (json.isEmpty() && _flash) {
        json = _flash->readString("/config/names.json");
    }
    if (json.isEmpty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    for (JsonPair kv : doc.as<JsonObject>()) {
        _nameCache[kv.key().c_str()] = kv.value().as<std::string>();
    }
    Serial.printf("[ANNOUNCE] Name cache loaded (%d entries)\n", (int)_nameCache.size());
}
