#pragma once

#include <Transport.h>
#include <Identity.h>
#include <Bytes.h>
#include <vector>
#include <string>

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

    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    int nodeCount() const { return _nodes.size(); }
    const DiscoveredNode* findNode(const RNS::Bytes& hash) const;
    const DiscoveredNode* findNodeByHex(const std::string& hexHash) const;
    void addManualContact(const std::string& hexHash, const std::string& name);
    void evictStale(unsigned long maxAgeMs = 3600000);

private:
    void saveContact(const DiscoveredNode& node);
    void removeContact(const std::string& hexHash);

    std::vector<DiscoveredNode> _nodes;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    RNS::Bytes _localDestHash;
    static constexpr int MAX_NODES = 200;  // PSRAM allows more
};
