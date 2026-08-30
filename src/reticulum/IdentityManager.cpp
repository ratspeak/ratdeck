#include "IdentityManager.h"
#include "config/Config.h"
#include "platform/CoreSync.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Utilities/OS.h>

bool IdentityManager::begin(FlashStore* flash, SDStore* sd) {
    _flash = flash;
    _sd = sd;
    _flash->ensureDir(ID_DIR);
    loadSlotMeta();

    // If no slots exist, import the current active identity as slot 0
    if (_slots.empty()) {
        if (_flash->exists(PATH_IDENTITY)) {
            IdentitySlot slot;
            slot.keyPath = slotKeyPath(0);
            slot.active = true;

            // Load the key to get its hash
            RNS::Bytes keyData;
            if (RNS::Utilities::OS::read_file(PATH_IDENTITY, keyData) > 0) {
                RNS::Identity id(false);
                if (id.load_private_key(keyData)) {
                    slot.hash = id.hexhash();
                }
            }
            if (!slot.hash.empty()
                && _flash->writeAtomic(slot.keyPath.c_str(), keyData.data(), keyData.size())) {
                _slots.push_back(slot);
                _activeIdx = 0;
                saveSlotMeta();
            }
        }
    } else {
        int activeIdx = -1;
        for (int i = 0; i < (int)_slots.size(); i++) {
            if (_slots[i].keyPath == PATH_IDENTITY) {
                String migratedPath = slotKeyPath(i);
                RNS::Bytes keyData;
                if (RNS::Utilities::OS::read_file(PATH_IDENTITY, keyData) > 0) {
                    RNS::Identity id(false);
                    if (id.load_private_key(keyData)
                        && (_slots[i].hash.empty() || _slots[i].hash == id.hexhash())
                        && _flash->writeAtomic(migratedPath.c_str(), keyData.data(), keyData.size())) {
                        _slots[i].keyPath = migratedPath;
                        if (_slots[i].hash.empty()) _slots[i].hash = id.hexhash();
                    }
                }
            }
            if (_slots[i].active && activeIdx < 0) activeIdx = i;
        }
        _activeIdx = activeIdx;
        saveSlotMeta();
    }

    Serial.printf("[IDMGR] %d identities, active=%d\n", (int)_slots.size(), _activeIdx);
    return true;
}

String IdentityManager::slotKeyPath(int slotNum) const {
    char path[48];
    snprintf(path, sizeof(path), "/identity/id_%d.key", slotNum);
    return String(path);
}

String IdentityManager::importIdentityPath() const {
    if (!_sd || !_sd->isReady()) return "";
    CoreSync::SpiBusGuard busGuard;
    if (_sd->exists(SD_PATH_IMPORT_IDENTITY)) return String(SD_PATH_IMPORT_IDENTITY);
    if (_sd->exists(SD_PATH_IMPORT_ID)) return String(SD_PATH_IMPORT_ID);

    File dir = _sd->openDir(SD_PATH_IDENTITY_DIR);
    if (!dir) return "";
    String candidate;
    int candidates = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }
        String name = entry.name();
        entry.close();
        String lower = name;
        lower.toLowerCase();
        bool reservedIdentityFile = lower == "identity.key" || lower.endsWith("/identity.key") ||
            lower == "identity.identity" || lower.endsWith("/identity.identity");
        bool importCandidate = !reservedIdentityFile &&
            (lower.endsWith(".identity") || lower.endsWith(".key"));
        if (importCandidate) {
            candidate = name;
            if (!candidate.startsWith("/")) {
                int slash = candidate.lastIndexOf('/');
                if (slash >= 0) candidate = candidate.substring(slash + 1);
                candidate = String(SD_PATH_IDENTITY_DIR) + "/" + candidate;
            }
            candidates++;
        }
    }
    dir.close();
    if (candidates == 1) return candidate;
    if (candidates > 1) Serial.println("[IDMGR] Multiple import identity files found on SD");
    return "";
}

int IdentityManager::importIdentity(const String& displayName) {
    (void)displayName;
    if (!_sd || !_sd->isReady()) return -1;
    if ((int)_slots.size() >= MAX_IDENTITIES) return -1;

    String importPath = importIdentityPath();
    if (importPath.isEmpty()) {
        Serial.println("[IDMGR] No import identity file found on SD");
        return -1;
    }

    uint8_t buffer[64];
    size_t bytesRead = 0;
    if (!_sd->readFile(importPath.c_str(), buffer, sizeof(buffer), bytesRead) || bytesRead != sizeof(buffer)) {
        Serial.println("[IDMGR] Failed to read 64-byte import identity from SD");
        return -1;
    }

    RNS::Bytes privKey(buffer, sizeof(buffer));
    RNS::Identity imported(false);
    if (!imported.load_private_key(privKey)) {
        Serial.println("[IDMGR] Import identity key failed validation");
        return -1;
    }
    std::string importedHash = imported.hexhash();
    for (const auto& slot : _slots) {
        if (slot.hash == importedHash) {
            Serial.printf("[IDMGR] Imported identity already exists: %s\n",
                          importedHash.substr(0, 16).c_str());
            return -1;
        }
    }

    int slotNum = (int)_slots.size();
    String keyPath = slotKeyPath(slotNum);
    if (!_flash->writeAtomic(keyPath.c_str(), privKey.data(), privKey.size())) {
        Serial.println("[IDMGR] Failed to save imported identity key");
        return -1;
    }

    IdentitySlot slot;
    slot.hash = importedHash;
    slot.displayName = "";
    slot.keyPath = keyPath;
    slot.active = false;
    _slots.push_back(slot);

    saveSlotMeta();
    Serial.printf("[IDMGR] Imported identity %d from %s: %s\n",
                  slotNum, importPath.c_str(), slot.hash.substr(0, 16).c_str());
    return slotNum;
}

int IdentityManager::createIdentity(const String& displayName) {
    if ((int)_slots.size() >= MAX_IDENTITIES) return -1;

    // Generate new identity
    RNS::Identity newId;
    RNS::Bytes privKey = newId.get_private_key();
    if (privKey.size() == 0) return -1;

    int slotNum = (int)_slots.size();
    String keyPath = slotKeyPath(slotNum);

    if (!_flash->writeAtomic(keyPath.c_str(), privKey.data(), privKey.size())) {
        Serial.println("[IDMGR] Failed to save new identity key");
        return -1;
    }

    IdentitySlot slot;
    slot.hash = newId.hexhash();
    slot.displayName = "";  // New identity starts unnamed
    slot.keyPath = keyPath;
    slot.active = false;
    _slots.push_back(slot);

    saveSlotMeta();
    Serial.printf("[IDMGR] Created identity %d: %s\n", slotNum, slot.hash.substr(0, 16).c_str());
    return slotNum;
}

bool IdentityManager::deleteIdentity(int index) {
    if (index < 0 || index >= (int)_slots.size()) return false;
    if ((int)_slots.size() <= 1) return false;  // Can't delete last identity
    if (_slots[index].active) return false;      // Can't delete active identity

    // Remove key file
    _flash->remove(_slots[index].keyPath.c_str());

    _slots.erase(_slots.begin() + index);

    // Adjust active index
    if (_activeIdx > index) _activeIdx--;

    saveSlotMeta();
    Serial.printf("[IDMGR] Deleted identity %d\n", index);
    return true;
}

bool IdentityManager::switchTo(int index, RNS::Identity& outIdentity) {
    if (index < 0 || index >= (int)_slots.size()) return false;
    if (index == _activeIdx) return true;

    // Load the key
    RNS::Bytes keyData;
    if (RNS::Utilities::OS::read_file(_slots[index].keyPath.c_str(), keyData) <= 0) {
        Serial.printf("[IDMGR] Failed to read key for slot %d\n", index);
        return false;
    }

    RNS::Identity id(false);
    if (!id.load_private_key(keyData)) {
        Serial.printf("[IDMGR] Failed to load key for slot %d\n", index);
        return false;
    }
    if (!_slots[index].hash.empty() && _slots[index].hash != id.hexhash()) {
        Serial.printf("[IDMGR] Slot %d key hash mismatch; refusing switch\n", index);
        return false;
    }

    // Copy the active key to the standard internal identity path for ReticulumManager.
    if (!_flash->writeAtomic(PATH_IDENTITY, keyData.data(), keyData.size())) {
        Serial.printf("[IDMGR] Failed to write active identity for slot %d\n", index);
        return false;
    }

    // Mark old active as inactive
    if (_activeIdx >= 0 && _activeIdx < (int)_slots.size()) {
        _slots[_activeIdx].active = false;
    }

    _slots[index].active = true;
    _activeIdx = index;

    outIdentity = id;
    saveSlotMeta();
    Serial.printf("[IDMGR] Switched to identity %d: %s\n", index, _slots[index].hash.substr(0, 16).c_str());
    return true;
}

void IdentityManager::setDisplayName(int index, const String& name) {
    if (index < 0 || index >= (int)_slots.size()) return;
    _slots[index].displayName = name;
    saveSlotMeta();
}

String IdentityManager::getDisplayName(int index) const {
    if (index < 0 || index >= (int)_slots.size()) return "";
    return _slots[index].displayName;
}

bool IdentityManager::syncNameFromActive(String& outName) const {
    if (_activeIdx < 0 || _activeIdx >= (int)_slots.size()) return false;
    outName = _slots[_activeIdx].displayName;
    return true;
}

void IdentityManager::refresh() {
    loadSlotMeta();
}

void IdentityManager::loadSlotMeta() {
    _slots.clear();
    String json = _flash->readString(META_PATH);
    if (json.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;

    JsonArray arr = doc["slots"];
    if (!arr) return;

    for (JsonObject obj : arr) {
        IdentitySlot slot;
        slot.hash = obj["hash"] | "";
        slot.displayName = obj["name"] | "";
        slot.keyPath = obj["path"] | "";
        slot.active = obj["active"] | false;
        if (!slot.keyPath.isEmpty()) {
            _slots.push_back(slot);
        }
    }
}

void IdentityManager::saveSlotMeta() {
    JsonDocument doc;
    JsonArray arr = doc["slots"].to<JsonArray>();

    for (auto& slot : _slots) {
        JsonObject obj = arr.add<JsonObject>();
        obj["hash"] = slot.hash;
        obj["name"] = slot.displayName;
        obj["path"] = slot.keyPath;
        obj["active"] = slot.active;
    }

    String json;
    serializeJson(doc, json);
    _flash->writeString(META_PATH, json);
}
