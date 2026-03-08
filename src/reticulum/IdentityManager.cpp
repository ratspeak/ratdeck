#include "IdentityManager.h"
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
        if (_flash->exists("/identity/identity.key")) {
            IdentitySlot slot;
            slot.keyPath = "/identity/identity.key";
            slot.active = true;

            // Load the key to get its hash
            RNS::Bytes keyData;
            if (RNS::Utilities::OS::read_file(slot.keyPath.c_str(), keyData) > 0) {
                RNS::Identity id(false);
                if (id.load_private_key(keyData)) {
                    slot.hash = id.hexhash();
                }
            }
            _slots.push_back(slot);
            _activeIdx = 0;
            saveSlotMeta();
        }
    } else {
        // Find active slot
        for (int i = 0; i < (int)_slots.size(); i++) {
            if (_slots[i].active) { _activeIdx = i; break; }
        }
    }

    Serial.printf("[IDMGR] %d identities, active=%d\n", (int)_slots.size(), _activeIdx);
    return true;
}

String IdentityManager::slotKeyPath(int slotNum) const {
    char path[48];
    snprintf(path, sizeof(path), "/identity/id_%d.key", slotNum);
    return String(path);
}

int IdentityManager::createIdentity(const String& displayName) {
    if ((int)_slots.size() >= MAX_IDENTITIES) return -1;

    // Generate new identity
    RNS::Identity newId;
    RNS::Bytes privKey = newId.get_private_key();
    if (privKey.size() == 0) return -1;

    int slotNum = (int)_slots.size();
    String keyPath = slotKeyPath(slotNum);

    // Save key file
    _flash->writeAtomic(keyPath.c_str(), privKey.data(), privKey.size());

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

    // Mark old active as inactive
    if (_activeIdx >= 0 && _activeIdx < (int)_slots.size()) {
        _slots[_activeIdx].active = false;
    }

    _slots[index].active = true;
    _activeIdx = index;

    // Copy the active key to the standard identity path for ReticulumManager
    _flash->writeAtomic("/identity/identity.key", keyData.data(), keyData.size());
    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratputer/identity");
        _sd->writeAtomic("/ratputer/identity/identity.key", keyData.data(), keyData.size());
    }

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
