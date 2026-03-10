#pragma once

#include <Transport.h>
#include <Identity.h>
#include <Bytes.h>
#include <vector>
#include <string>
#include <map>

class SDStore;
class FlashStore;

struct DiscoveredNode {
    RNS::Bytes hash;
    std::string name;
    std::string identityHex;
    int rssi = 0;
    float snr = 0;
    uint8_t hops = 0;
    unsigned long lastSeen = 0;
    bool saved = false;
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
    void setLocalDestHash(const RNS::Bytes& hash) { _localDestHash = hash; }
    void saveContacts();
    void loadContacts();
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
    void evictStale(unsigned long maxAgeMs = 3600000);
    void clearTransientNodes();
    void clearAll();

private:
    void saveContact(const DiscoveredNode& node);
    void removeContact(const std::string& hexHash);

    std::vector<DiscoveredNode> _nodes;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    RNS::Bytes _localDestHash;
    bool _contactsDirty = false;
    bool _nameCacheDirty = false;
    unsigned long _lastContactSave = 0;
    unsigned long _lastAnnounceProcessed = 0;
    std::map<std::string, std::string> _nameCache;  // hexHash → displayName
    unsigned long _globalAnnounceWindowStart = 0;
    unsigned int _globalAnnounceCount = 0;
    static constexpr unsigned int MAX_GLOBAL_ANNOUNCES_PER_SEC = 3;
    static constexpr int MAX_NODES = 30;
    static constexpr unsigned long CONTACT_SAVE_INTERVAL_MS = 30000;
    static constexpr unsigned long ANNOUNCE_MIN_INTERVAL_MS = 200;  // Rate-limit announce processing
};
