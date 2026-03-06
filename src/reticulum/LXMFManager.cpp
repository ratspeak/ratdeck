// Direct port from Ratputer — LXMF messaging protocol
#include "LXMFManager.h"
#include "config/Config.h"
#include <Transport.h>

LXMFManager* LXMFManager::_instance = nullptr;

bool LXMFManager::begin(ReticulumManager* rns, MessageStore* store) {
    _rns = rns; _store = store; _instance = this;
    RNS::Destination& dest = _rns->destination();
    dest.set_packet_callback(onPacketReceived);
    dest.set_link_established_callback(onLinkEstablished);
    Serial.println("[LXMF] Manager started");
    return true;
}

void LXMFManager::loop() {
    if (_outQueue.empty()) return;
    LXMFMessage& msg = _outQueue.front();
    if (sendDirect(msg)) {
        Serial.printf("[LXMF] Queue drain: status=%s dest=%s\n",
                      msg.statusStr(), msg.destHash.toHex().substr(0, 8).c_str());
        if (_store) { _store->saveMessage(msg); }
        _outQueue.pop_front();
    }
}

bool LXMFManager::sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title) {
    LXMFMessage msg;
    msg.sourceHash = _rns->destination().hash();
    msg.destHash = destHash;
    msg.timestamp = millis() / 1000.0;
    msg.content = content;
    msg.title = title;
    msg.incoming = false;
    msg.status = LXMFStatus::QUEUED;
    if ((int)_outQueue.size() >= RATDECK_MAX_OUTQUEUE) { _outQueue.pop_front(); }
    _outQueue.push_back(msg);
    if (_store) { _store->saveMessage(msg); }
    return true;
}

bool LXMFManager::sendDirect(LXMFMessage& msg) {
    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) {
        msg.retries++;
        if (msg.retries >= 5) {
            Serial.printf("[LXMF] recall failed for %s after %d retries — marking FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        Serial.printf("[LXMF] recall failed for %s (retry %d/5) — keeping queued\n",
                      msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
        return false;  // keep in queue, retry next loop
    }
    RNS::Destination outDest(recipientId, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) { msg.status = LXMFStatus::FAILED; return true; }
    RNS::Bytes payloadBytes(payload.data(), payload.size());
    if (payloadBytes.size() > RNS::Type::Reticulum::MDU) { msg.status = LXMFStatus::FAILED; return true; }
    msg.status = LXMFStatus::SENDING;
    RNS::Packet packet(outDest, payloadBytes);
    RNS::PacketReceipt receipt = packet.send();
    if (receipt) {
        msg.status = LXMFStatus::SENT;
        msg.messageId = RNS::Identity::full_hash(payloadBytes);
        Serial.printf("[LXMF] Sent %d bytes\n", (int)payloadBytes.size());
    } else {
        msg.status = LXMFStatus::FAILED;
    }
    return true;
}

void LXMFManager::onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet) {
    if (!_instance) return;
    _instance->processIncoming(data.data(), data.size(), packet.destination_hash());
}

void LXMFManager::onLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    link.set_packet_callback([](const RNS::Bytes& data, const RNS::Packet& packet) {
        if (!_instance) return;
        _instance->processIncoming(data.data(), data.size(), packet.destination_hash());
    });
}

void LXMFManager::processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash) {
    LXMFMessage msg;
    if (!LXMFMessage::unpackFull(data, len, msg)) {
        Serial.printf("[LXMF] Failed to unpack incoming message (%d bytes)\n", (int)len);
        return;
    }
    if (_rns && msg.sourceHash == _rns->destination().hash()) return;
    msg.destHash = destHash;
    Serial.printf("[LXMF] Message from %s (%d bytes) content_len=%d\n",
                  msg.sourceHash.toHex().substr(0, 8).c_str(), (int)len, (int)msg.content.size());
    if (_store) { _store->saveMessage(msg); }
    std::string peerHex = msg.sourceHash.toHex();
    _unread[peerHex]++;
    if (_onMessage) { _onMessage(msg); }
}

const std::vector<std::string>& LXMFManager::conversations() const {
    if (_store) return _store->conversations();
    static std::vector<std::string> empty;
    return empty;
}

std::vector<LXMFMessage> LXMFManager::getMessages(const std::string& peerHex) const {
    if (_store) return _store->loadConversation(peerHex);
    return {};
}

int LXMFManager::unreadCount(const std::string& peerHex) const {
    if (!_unreadComputed) { const_cast<LXMFManager*>(this)->computeUnreadFromDisk(); }
    if (peerHex.empty()) {
        int total = 0;
        for (auto& kv : _unread) total += kv.second;
        return total;
    }
    auto it = _unread.find(peerHex);
    return (it != _unread.end()) ? it->second : 0;
}

void LXMFManager::computeUnreadFromDisk() {
    _unreadComputed = true;
    if (!_store) return;
    for (auto& conv : _store->conversations()) {
        auto msgs = _store->loadConversation(conv);
        int count = 0;
        for (auto& m : msgs) { if (m.incoming && !m.read) count++; }
        if (count > 0) _unread[conv] = count;
    }
}

void LXMFManager::markRead(const std::string& peerHex) {
    _unread[peerHex] = 0;
    if (_store) { _store->markConversationRead(peerHex); }
}
