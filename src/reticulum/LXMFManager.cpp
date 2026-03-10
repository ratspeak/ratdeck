// Direct port from Ratputer — LXMF messaging protocol
#include "LXMFManager.h"
#include "config/Config.h"
#include <Transport.h>
#include <time.h>

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

    // Throttle retries — wait 2 seconds between attempts
    unsigned long now = millis();
    if (msg.retries > 0 && (now - _lastRetryMs) < 2000) return;
    _lastRetryMs = now;

    if (sendDirect(msg)) {
        Serial.printf("[LXMF] Queue drain: status=%s dest=%s\n",
                      msg.statusStr(), msg.destHash.toHex().substr(0, 8).c_str());

        // Persist updated status to disk so reloads don't revert to QUEUED
        std::string peerHex = msg.destHash.toHex();
        if (_store) {
            _store->updateMessageStatus(peerHex, msg.timestamp, false, msg.status);
        }

        // Fire status callback so UI can refresh
        if (_statusCb) {
            _statusCb(peerHex, msg.timestamp, msg.status);
        }
        _outQueue.pop_front();
    }
}

bool LXMFManager::sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title) {
    LXMFMessage msg;
    msg.sourceHash = _rns->destination().hash();
    msg.destHash = destHash;
    // Use real epoch time when NTP is synced, uptime fallback otherwise
    time_t now = time(nullptr);
    if (now > 1700000000) {
        msg.timestamp = (double)now;
    } else {
        msg.timestamp = millis() / 1000.0;
    }
    msg.content = content;
    msg.title = title;
    msg.incoming = false;
    msg.status = LXMFStatus::QUEUED;
    if ((int)_outQueue.size() >= RATDECK_MAX_OUTQUEUE) { _outQueue.pop_front(); }
    _outQueue.push_back(msg);
    // Immediately save with QUEUED status so it appears in getMessages() right away
    if (_store) { _store->saveMessage(msg); }
    return true;
}

bool LXMFManager::sendDirect(LXMFMessage& msg) {
    Serial.printf("[LXMF] sendDirect: dest=%s\n", msg.destHash.toHex().substr(0, 12).c_str());
    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) {
        msg.retries++;
        // Request path on first retry and every 10 retries
        if (msg.retries == 1 || msg.retries % 10 == 0) {
            Serial.printf("[LXMF] Requesting path for %s\n",
                          msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Transport::request_path(msg.destHash);
        }
        if (msg.retries >= 30) {
            Serial.printf("[LXMF] recall FAILED for %s after %d retries — marking FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        Serial.printf("[LXMF] recall pending for %s (retry %d/30) — identity not known yet\n",
                      msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
        return false;  // keep in queue, retry next loop
    }
    Serial.printf("[LXMF] recall OK: identity=%s\n", recipientId.hexhash().c_str());
    RNS::Destination outDest(recipientId, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    Serial.printf("[LXMF] outDest hash: %s\n", outDest.hash().toHex().substr(0, 12).c_str());
    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) { Serial.println("[LXMF] packFull returned empty!"); msg.status = LXMFStatus::FAILED; return true; }
    RNS::Bytes payloadBytes(payload.data(), payload.size());
    if (payloadBytes.size() > RNS::Type::Reticulum::MDU) {
        Serial.printf("[LXMF] payload too large: %d > MDU\n", (int)payloadBytes.size());
        msg.status = LXMFStatus::FAILED; return true;
    }
    msg.status = LXMFStatus::SENDING;
    Serial.printf("[LXMF] sending packet: %d bytes to %s\n", (int)payloadBytes.size(), outDest.hash().toHex().substr(0, 12).c_str());
    RNS::Packet packet(outDest, payloadBytes);
    RNS::PacketReceipt receipt = packet.send();
    if (receipt) {
        msg.status = LXMFStatus::SENT;
        msg.messageId = RNS::Identity::full_hash(payloadBytes);
        Serial.printf("[LXMF] SENT OK: %d bytes, msgId=%s\n", (int)payloadBytes.size(), msg.messageId.toHex().substr(0, 8).c_str());
    } else {
        Serial.println("[LXMF] send FAILED: no receipt");
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

    // Deduplication: skip messages we've already processed
    std::string msgIdHex = msg.messageId.toHex();
    if (_seenMessageIds.count(msgIdHex)) {
        Serial.printf("[LXMF] Duplicate message from %s (already seen)\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str());
        return;
    }
    _seenMessageIds.insert(msgIdHex);
    if ((int)_seenMessageIds.size() > MAX_SEEN_IDS) {
        _seenMessageIds.erase(_seenMessageIds.begin());
    }

    msg.destHash = destHash;
    Serial.printf("[LXMF] Message from %s (%d bytes) content_len=%d\n",
                  msg.sourceHash.toHex().substr(0, 8).c_str(), (int)len, (int)msg.content.size());
    if (_store) { _store->saveMessage(msg); }
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
    if (!_store) return 0;
    if (peerHex.empty()) return _store->totalUnreadCount();
    const ConversationSummary* s = _store->getSummary(peerHex);
    return s ? s->unreadCount : 0;
}

const ConversationSummary* LXMFManager::getConversationSummary(const std::string& peerHex) const {
    if (!_store) return nullptr;
    return _store->getSummary(peerHex);
}

void LXMFManager::markRead(const std::string& peerHex) {
    if (_store) { _store->markConversationRead(peerHex); }
}
