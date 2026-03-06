// Direct port from Ratputer — node discovery and contact persistence
#include "AnnounceManager.h"
#include "config/Config.h"
#include "storage/SDStore.h"
#include "storage/FlashStore.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static std::string extractMsgPackName(const uint8_t* data, size_t len) {
    if (len < 2) return "";
    uint8_t b = data[0];
    size_t pos = 0;
    if ((b & 0xF0) == 0x90) { if ((b & 0x0F) == 0) return ""; pos = 1; }
    else if (b == 0xDC && len >= 3) { pos = 3; }
    else return "";
    if (pos >= len) return "";
    b = data[pos];
    size_t slen = 0;
    if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; pos++; }
    else if (b == 0xD9 && pos + 1 < len) { slen = data[pos+1]; pos += 2; }
    else if (b == 0xDA && pos + 2 < len) { slen = ((size_t)data[pos+1] << 8) | data[pos+2]; pos += 3; }
    else return "";
    if (pos + slen > len) return "";
    return std::string((const char*)&data[pos], slen);
}

static std::string sanitizeName(const std::string& raw, size_t maxLen = 16) {
    std::string clean;
    clean.reserve(std::min(raw.size(), maxLen));
    for (char c : raw) {
        if (clean.size() >= maxLen) break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'') {
            clean += c;
        }
    }
    size_t start = clean.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = clean.find_last_not_of(' ');
    return clean.substr(start, end - start + 1);
}

AnnounceManager::AnnounceManager(const char* aspectFilter) : RNS::AnnounceHandler(aspectFilter) {}

void AnnounceManager::setStorage(SDStore* sd, FlashStore* flash) { _sd = sd; _flash = flash; }

void AnnounceManager::received_announce(
    const RNS::Bytes& destination_hash,
    const RNS::Identity& announced_identity,
    const RNS::Bytes& app_data)
{
    std::string name;
    if (app_data.size() > 0) {
        std::string rawName = extractMsgPackName(app_data.data(), app_data.size());
        if (rawName.empty()) rawName = app_data.toString();
        name = sanitizeName(rawName);
    }
    // Filter out own announces
    if (_localDestHash.size() > 0 && destination_hash == _localDestHash) return;

    Serial.printf("[ANNOUNCE] From: %s name=\"%s\"\n", destination_hash.toHex().c_str(), name.c_str());

    std::string idHex = announced_identity.hexhash();

    for (auto& node : _nodes) {
        if (node.hash == destination_hash) {
            if (!name.empty()) node.name = name;
            if (!idHex.empty()) node.identityHex = idHex;
            node.lastSeen = millis();
            node.hops = RNS::Transport::hops_to(destination_hash);
            if (node.saved) saveContact(node);
            return;
        }
    }

    if ((int)_nodes.size() >= MAX_NODES) {
        evictStale();
        if ((int)_nodes.size() >= MAX_NODES) {
            unsigned long oldest = ULONG_MAX;
            int oldestIdx = -1;
            for (int i = 0; i < (int)_nodes.size(); i++) {
                if (!_nodes[i].saved && _nodes[i].lastSeen < oldest) {
                    oldest = _nodes[i].lastSeen;
                    oldestIdx = i;
                }
            }
            if (oldestIdx >= 0) _nodes.erase(_nodes.begin() + oldestIdx);
        }
    }
    if ((int)_nodes.size() >= MAX_NODES) return;

    DiscoveredNode node;
    node.hash = destination_hash;
    node.name = name.empty() ? destination_hash.toHex().substr(0, 12) : name;
    node.identityHex = idHex;
    node.lastSeen = millis();
    node.hops = RNS::Transport::hops_to(destination_hash);
    _nodes.push_back(node);
}

const DiscoveredNode* AnnounceManager::findNode(const RNS::Bytes& hash) const {
    for (const auto& n : _nodes) { if (n.hash == hash) return &n; }
    return nullptr;
}

const DiscoveredNode* AnnounceManager::findNodeByHex(const std::string& hexHash) const {
    for (const auto& n : _nodes) { if (n.hash.toHex() == hexHash) return &n; }
    return nullptr;
}

void AnnounceManager::addManualContact(const std::string& hexHash, const std::string& name) {
    RNS::Bytes hash;
    hash.assignHex(hexHash.c_str());
    std::string safeName = sanitizeName(name);
    for (auto& n : _nodes) {
        if (n.hash == hash) {
            if (!safeName.empty()) n.name = safeName;
            n.saved = true;
            saveContact(n);
            return;
        }
    }
    DiscoveredNode node;
    node.hash = hash;
    node.name = safeName.empty() ? hexHash.substr(0, 12) : safeName;
    node.lastSeen = millis();
    node.saved = true;
    _nodes.push_back(node);
    saveContact(node);
}

void AnnounceManager::evictStale(unsigned long maxAgeMs) {
    unsigned long now = millis();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [now, maxAgeMs](const DiscoveredNode& n) {
            return !n.saved && (now - n.lastSeen > maxAgeMs);
        }), _nodes.end());
}

void AnnounceManager::saveContact(const DiscoveredNode& node) {
    std::string hexHash = node.hash.toHex();
    JsonDocument doc;
    doc["hash"] = hexHash; doc["name"] = node.name;
    doc["rssi"] = node.rssi; doc["snr"] = node.snr;
    doc["hops"] = node.hops; doc["lastSeen"] = node.lastSeen;
    String json;
    serializeJson(doc, json);
    String filename = hexHash.substr(0, 16).c_str();
    filename += ".json";
    if (_sd && _sd->isReady()) { _sd->writeString((String(SD_PATH_CONTACTS) + filename).c_str(), json); }
    if (_flash) { _flash->writeString((String(PATH_CONTACTS) + filename).c_str(), json); }
}

void AnnounceManager::removeContact(const std::string& hexHash) {
    String filename = hexHash.substr(0, 16).c_str();
    filename += ".json";
    if (_sd && _sd->isReady()) { _sd->remove((String(SD_PATH_CONTACTS) + filename).c_str()); }
    if (_flash) { _flash->remove((String(PATH_CONTACTS) + filename).c_str()); }
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
                            bool dup = false;
                            for (auto& n : _nodes) { if (n.hash == hash) { dup = true; break; } }
                            if (!dup) {
                                DiscoveredNode node;
                                node.hash = hash;
                                node.name = sanitizeName(doc["name"] | "");
                                if (node.name.empty()) node.name = hexHash.substr(0, 12);
                                node.rssi = doc["rssi"] | 0;
                                node.snr = doc["snr"] | 0.0f;
                                node.hops = doc["hops"] | 0;
                                node.lastSeen = doc["lastSeen"] | (unsigned long)millis();
                                node.saved = true;
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
