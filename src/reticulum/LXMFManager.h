#pragma once

#include "LXMFMessage.h"
#include "ReticulumManager.h"
#include "storage/MessageStore.h"
#include <Destination.h>
#include <Packet.h>
#include <Link.h>
#include <Identity.h>
#include <functional>
#include <deque>
#include <set>

class LXMFManager {
public:
    using MessageCallback = std::function<void(const LXMFMessage&)>;
    using StatusCallback = std::function<void(const std::string& peerHex, double timestamp, LXMFStatus status)>;
    void setStatusCallback(StatusCallback cb) { _statusCb = cb; }

    bool begin(ReticulumManager* rns, MessageStore* store);
    void loop();

    bool sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title = "");
    void setMessageCallback(MessageCallback cb) { _onMessage = cb; }
    int queuedCount() const { return _outQueue.size(); }
    const std::vector<std::string>& conversations() const;
    std::vector<LXMFMessage> getMessages(const std::string& peerHex) const;
    int unreadCount(const std::string& peerHex = "") const;
    void markRead(const std::string& peerHex);

private:
    bool sendDirect(LXMFMessage& msg);
    void processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash);
    static void onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet);
    static void onLinkEstablished(RNS::Link& link);

    ReticulumManager* _rns = nullptr;
    MessageStore* _store = nullptr;
    MessageCallback _onMessage;
    StatusCallback _statusCb;
    std::deque<LXMFMessage> _outQueue;

    void computeUnreadFromDisk();
    mutable bool _unreadComputed = false;
    mutable std::map<std::string, int> _unread;

    // Deduplication: recently seen message IDs
    std::set<std::string> _seenMessageIds;
    static constexpr int MAX_SEEN_IDS = 100;

    static LXMFManager* _instance;
};
