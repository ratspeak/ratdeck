#pragma once

#include <Transport.h>
#include <Identity.h>
#include <Bytes.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>

class SDStore;
class FlashStore;
class LoRaInterface;

struct DiscoveredNode {
    RNS::Bytes hash;
    std::string name;
    std::string identityHex;
    int rssi = 0;
    float snr = 0;
    uint8_t hops = 0;
    unsigned long lastSeen = 0;
    bool saved = false;
    // Peer-on-map (rsDeck #64): saved contacts that have explicitly shared
    // lat/lon via LXMF message parsing. hasLocation is the only "show on map"
    // gate for the UI; lat/lon are in WGS84 degrees. locTs is the boot-relative
    // millis() at the moment the value was set (debug-only; not persisted).
    double lat = 0.0;
    double lon = 0.0;
    bool hasLocation = false;
    unsigned long locTs = 0;
};

class AnnounceManager : public RNS::AnnounceHandler {
public:
    AnnounceManager(const char* aspectFilter = nullptr);
    virtual ~AnnounceManager() = default;

    virtual void received_announce(
        const RNS::Bytes& destination_hash,
        const RNS::Identity& announced_identity,
        const RNS::Bytes& app_data) override;

    void setStorage(SDStore* sd, FlashStore* flash);
    void setLoRaInterface(LoRaInterface* li) { _loraIf = li; }
    void setLocalDestHash(const RNS::Bytes& hash) { _localDestHash = hash; }
    void saveContacts();
    void loadContacts();
    bool deleteContact(int nodeIdx);
    void loop();  // Call from main loop — handles deferred saves

    // Name cache: persists hash→name mappings so names survive reboots
    std::string lookupName(const std::string& hexHash) const;
    void saveNameCache();
    void loadNameCache();

    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    int nodeCount() const { return _nodes.size(); }
    int nodesOnlineSince(unsigned long maxAgeMs) const;
    const DiscoveredNode* findNode(const RNS::Bytes& hash) const;
    const DiscoveredNode* findNodeByHex(const std::string& hexHash) const;
    void addManualContact(const std::string& hexHash, const std::string& name);
    // Peer-on-map (rsDeck #64). Ensure a node exists as a saved contact (so
    // map UI can show a pin for it). Creates one if missing. No-ops silently
    // if the hash can't be decoded. Returns true iff a saved contact with
    // this hash is present after the call.
    bool ensureSavedContact(const std::string& hexHash, const std::string& nameHint = "");
    // Set location for a saved contact. On parseable inbound location share,
    // call ensureSavedContact() then setLocation() — this enforces the
    // "saved contacts only" map-pin rule without needing a parallel store.
    // Returns true iff the value was stored (i.e. node is a saved contact).
    bool setLocation(const std::string& hexHash, double lat, double lon);
    void evictStale(unsigned long maxAgeMs = 3600000);
    void clearTransientNodes();
    void clearAll();
    void rebuildIndex();

private:
    void saveContact(const DiscoveredNode& node);
    void removeContact(const std::string& hexHash);
    void persistKnownDestinationsAfterAnnounce(const char* reason, bool force);

    std::vector<DiscoveredNode> _nodes;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    LoRaInterface* _loraIf = nullptr;
    RNS::Bytes _localDestHash;
    bool _contactsDirty = false;
    bool _nameCacheDirty = false;
    unsigned long _lastContactSave = 0;
    unsigned long _lastAnnounceProcessed = 0;
    std::map<std::string, std::string> _nameCache;  // hexHash → displayName
    unsigned long _globalAnnounceWindowStart = 0;
    unsigned int _globalAnnounceCount = 0;
    // Keep app-layer processing light under I2P/TCP announce floods so the
    // main loop can still poll GT911 touch + LVGL every frame.
    static constexpr unsigned int MAX_GLOBAL_ANNOUNCES_PER_SEC = 5;
    static constexpr int MAX_NODES = 100;
    static constexpr int MAX_NAME_CACHE = 300;
    static constexpr unsigned long CONTACT_SAVE_INTERVAL_MS = 30000;
    static constexpr unsigned long KNOWN_DESTINATION_PERSIST_MIN_INTERVAL_MS = 15000;
    static constexpr unsigned long ANNOUNCE_MIN_INTERVAL_MS = 500;  // per-peer re-announce floor
    unsigned long _lastKnownDestinationsPersist = 0;

    std::unordered_map<std::string, int> _hashIndex;  // raw hash bytes → _nodes index

    static std::string makeKey(const RNS::Bytes& hash) {
        return std::string((const char*)hash.data(), hash.size());
    }
};
