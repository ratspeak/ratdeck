#pragma once

#include <Arduino.h>
#include <Identity.h>
#include <vector>
#include <string>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"

struct IdentitySlot {
    std::string hash;       // hex hash (first 32 chars)
    String displayName;     // per-identity display name
    String keyPath;         // storage path to .key file
    bool active;            // currently loaded identity
};

class IdentityManager {
public:
    bool begin(FlashStore* flash, SDStore* sd = nullptr);

    // List all stored identities
    const std::vector<IdentitySlot>& identities() const { return _slots; }
    int count() const { return (int)_slots.size(); }

    // Current active identity index (-1 if none)
    int activeIndex() const { return _activeIdx; }

    // Create a new identity, returns index
    int createIdentity(const String& displayName = "");

    // Delete identity at index (cannot delete last one)
    bool deleteIdentity(int index);

    // Switch to identity at index — returns the loaded Identity
    // Caller must reinitialize Destination after switching
    bool switchTo(int index, RNS::Identity& outIdentity);

    // Save display name for identity at index
    void setDisplayName(int index, const String& name);

    // Get the display name for identity at index
    String getDisplayName(int index) const;

    // Sync active identity's display name from/to config
    // Call on boot: loads active slot's name into outName
    // Returns true if outName was set
    bool syncNameFromActive(String& outName) const;

    // Refresh slot list from storage
    void refresh();

private:
    void loadSlotMeta();
    void saveSlotMeta();
    String slotKeyPath(int slotNum) const;

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    std::vector<IdentitySlot> _slots;
    int _activeIdx = -1;

    static constexpr int MAX_IDENTITIES = 8;
    static constexpr const char* META_PATH = "/identity/slots.json";
    static constexpr const char* ID_DIR = "/identity";
};
