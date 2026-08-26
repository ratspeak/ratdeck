// Direct port from Ratputer — LXMF messaging protocol
#include "LXMFManager.h"
#include "config/Config.h"
#include <Transport.h>
#include <time.h>
#include <algorithm>
#include <exception>

LXMFManager* LXMFManager::_instance = nullptr;
std::map<std::string, LXMFManager::PendingProof> LXMFManager::_pendingProofs;

namespace {
constexpr unsigned long LXMF_DISCOVERY_RETRY_INTERVAL_MS = 10000;
constexpr int LXMF_DISCOVERY_MAX_ATTEMPTS = 7;  // immediate attempt + six 10s retries ~= 60s
constexpr unsigned long LXMF_LINK_ESTABLISH_TIMEOUT_MS = 30000;
constexpr uint8_t LXMF_LINK_MAX_WAIT_ATTEMPTS = 6;
}

bool LXMFManager::begin(ReticulumManager* rns, MessageStore* store) {
    _rns = rns; _store = store; _instance = this;
    RNS::Destination& dest = _rns->destination();
    dest.set_packet_callback(onPacketReceived);
    dest.set_link_established_callback(onLinkEstablished);
    if (_store) {
        for (const auto& id : _store->startupRecentMessageIds(MAX_SEEN_IDS)) {
            rememberMessageId(id);
        }
        const std::vector<LXMFMessage>& pending = _store->startupPendingOutgoing();
        for (auto msg : pending) {
            if ((int)_outQueue.size() >= RSDECK_MAX_OUTQUEUE) break;
            msg.lastRetryMs = 0;
            _outQueue.push_back(msg);
        }
        if (!pending.empty()) {
            Serial.printf("[LXMF] Restored %d pending outgoing messages\n", (int)_outQueue.size());
        }
    }
    Serial.println("[LXMF] Manager started");
    return true;
}

void LXMFManager::rememberMessageId(const std::string& msgIdHex) {
    if (msgIdHex.empty() || _seenMessageIds.count(msgIdHex)) return;
    _seenMessageIds.insert(msgIdHex);
    _seenMessageOrder.push_back(msgIdHex);
    while ((int)_seenMessageOrder.size() > MAX_SEEN_IDS) {
        _seenMessageIds.erase(_seenMessageOrder.front());
        _seenMessageOrder.pop_front();
    }
}

void LXMFManager::loop() {
    unsigned long now = millis();
    for (auto it = _pendingProofs.begin(); it != _pendingProofs.end(); ) {
        if ((now - it->second.createdMs) > 65000UL) {
            std::string rh = it->first;
            ++it;
            handleProofTimeoutHash(rh);
        } else {
            ++it;
        }
    }

    if (_outQueue.empty()) return;
    int processed = 0;

    for (auto it = _outQueue.begin(); it != _outQueue.end(); ) {
        LXMFMessage& msg = *it;

        // Time-budgeted drain: process up to 3 messages within 10ms so a
        // burst send doesn't starve the UI / packet RX. LoRa pacing is
        // handled inside LoRaInterface (TX_QUEUE_MAX + reject-newest).
        if (processed >= 3 || (processed > 0 && millis() - now >= 10)) break;

        // Keep unresolved peers from churning the UI loop or LoRa airtime.
        // The first attempt is immediate; later path/identity retries happen
        // every 10s and are capped in attemptOutboundDelivery().
        if (msg.retries > 0 && (millis() - msg.lastRetryMs) < LXMF_DISCOVERY_RETRY_INTERVAL_MS) {
            ++it;
            continue;
        }

        msg.lastRetryMs = millis();

        if (attemptOutboundDelivery(msg)) {
            processed++;
            Serial.printf("[LXMF] Queue drain: status=%s dest=%s\n",
                          msg.statusStr(), msg.destHash.toHex().substr(0, 8).c_str());

            // Persist updated status to disk so reloads don't revert to QUEUED
            std::string peerHex = msg.destHash.toHex();
            if (_store && msg.savedCounter > 0) {
                _store->updateMessageStatusByCounter(peerHex, msg.savedCounter, false, msg.status);
            } else if (_store) {
                _store->updateMessageStatus(peerHex, msg.timestamp, false, msg.status);
            }

            if (_statusCb) {
                _statusCb(peerHex, msg.timestamp, msg.savedCounter, msg.status);
            }
            clearDeliveryPreference(msg);
            it = _outQueue.erase(it);
        } else {
            // attemptOutboundDelivery returned false — message stays in queue, try next
            ++it;
        }
    }
}

bool LXMFManager::sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title,
                              DeliveryPreference preference) {
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

    if ((int)_outQueue.size() >= RSDECK_MAX_OUTQUEUE) {
        Serial.println("[LXMF] Outbound queue full; refusing new message");
        return false;
    }

    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) {
        Serial.println("[LXMF] Message pack failed");
        return false;
    }
    if (preference == DeliveryPreference::Link) {
        _linkRequiredIds.insert(msg.messageId.toHex());
        Serial.printf("[LXMF] Message %s queued for link delivery\n",
                      msg.messageId.toHex().substr(0, 8).c_str());
    }

    _outQueue.push_back(msg);
    // Persist immediately so getMessages() reflects outbound text right
    // away (status flips QUEUED → SENT → DELIVERED on the stored row).
    if (_store) { _store->saveMessage(_outQueue.back()); }

    if (!RNS::Transport::has_path(destHash)) {
        Serial.printf("[LXMF] Message queued for %s; path discovery will retry every 10s\n",
                      destHash.toHex().substr(0, 8).c_str());
    }
    return true;
}

bool LXMFManager::sendMessageViaLink(const RNS::Bytes& destHash, const std::string& content,
                                     const std::string& title) {
    return sendMessage(destHash, content, title, DeliveryPreference::Link);
}

void LXMFManager::clearDeliveryPreference(const LXMFMessage& msg) {
    std::string msgId = msg.messageId.toHex();
    if (msgId.empty()) return;
    _linkRequiredIds.erase(msgId);
    _linkWaitAttempts.erase(msgId);
}

bool LXMFManager::ensureOutboundLink(const RNS::Destination& dest, const RNS::Bytes& destHash,
                                     const char* reason) {
    if (_outLink && _outLinkDestHash == destHash
        && _outLink.status() == RNS::Type::Link::ACTIVE) {
        return true;
    }

    unsigned long now = millis();
    bool samePending = _outLinkPending && _outLinkPendingHash == destHash;
    bool pendingAlive = samePending && _outLink
        && _outLink.status() != RNS::Type::Link::CLOSED
        && (now - _outLinkPendingSinceMs) < LXMF_LINK_ESTABLISH_TIMEOUT_MS;
    if (pendingAlive) return false;

    _outLinkPendingHash = destHash;
    _outLinkDestHash = destHash;
    _outLinkPending = true;
    _outLinkPendingSinceMs = now;
    Serial.printf("[LXMF] Establishing link to %s for %s\n",
                  destHash.toHex().substr(0, 8).c_str(), reason ? reason : "delivery");
    RNS::Link newLink(dest, onOutLinkEstablished, onOutLinkClosed);
    _outLink = newLink;
    return false;
}

bool LXMFManager::attemptOutboundDelivery(LXMFMessage& msg) {
    Serial.printf("[LXMF] attemptOutboundDelivery: dest=%s link=%s pending=%s\n",
        msg.destHash.toHex().substr(0, 12).c_str(),
        _outLink ? (_outLink.status() == RNS::Type::Link::ACTIVE ? "ACTIVE" : "INACTIVE") : "NONE",
        _outLinkPending ? "yes" : "no");

    // Reset stale link-pending state: if pending but link never became ACTIVE,
    // allow a new link attempt. Covers: link object destroyed (NONE), timed out, or failed.
    if (_outLinkPending) {
        bool linkReady = _outLink && _outLink.status() == RNS::Type::Link::ACTIVE;
        bool linkClosed = !_outLink || _outLink.status() == RNS::Type::Link::CLOSED;
        bool linkTimedOut = _outLinkPendingSinceMs > 0
            && (millis() - _outLinkPendingSinceMs) >= LXMF_LINK_ESTABLISH_TIMEOUT_MS;
        if (linkReady) {
            _outLinkPending = false;
        } else if (linkClosed || linkTimedOut) {
            _outLinkPending = false;
            _outLink = {RNS::Type::NONE};
            _outLinkPendingSinceMs = 0;
            Serial.println("[LXMF] Clearing stale link-pending");
        }
    }

    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) {
        msg.retries++;
        Serial.printf("[LXMF] Requesting path/identity for %s (attempt %d/%d)\n",
                      msg.destHash.toHex().substr(0, 8).c_str(),
                      msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
        RNS::Transport::request_path(msg.destHash);
        if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
            Serial.printf("[LXMF] No identity/path for %s after ~60s — marking FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        Serial.printf("[LXMF] recall pending for %s — identity not known yet\n",
                      msg.destHash.toHex().substr(0, 8).c_str());
        return false;  // keep in queue, retry next loop
    }
    Serial.printf("[LXMF] recall OK: identity=%s\n", recipientId.hexhash().c_str());

    // Ensure path exists — without a path, Transport::outbound() broadcasts as
    // Header1 which the Python hub silently drops
    if (!RNS::Transport::has_path(msg.destHash)) {
        msg.retries++;
        Serial.printf("[LXMF] No path for %s, requesting (attempt %d/%d)\n",
                      msg.destHash.toHex().substr(0, 8).c_str(),
                      msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
        RNS::Transport::request_path(msg.destHash);
        if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
            Serial.printf("[LXMF] No path for %s after ~60s — FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        return false;  // keep in queue, retry later
    }

    Serial.printf("[LXMF] path OK: %s hops=%d\n",
                  msg.destHash.toHex().substr(0, 8).c_str(),
                  RNS::Transport::hops_to(msg.destHash));

    RNS::Destination outDest(recipientId, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    Serial.printf("[DIAG] LXMF: outDest=%s msgDest=%s match=%s\n",
        outDest.hash().toHex().c_str(), msg.destHash.toHex().c_str(),
        (outDest.hash() == msg.destHash) ? "YES" : "NO");

    // packFull returns opportunistic format: [src:16][sig:64][msgpack]
    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) { Serial.println("[LXMF] packFull returned empty!"); msg.status = LXMFStatus::FAILED; return true; }
    const std::string msgIdHex = msg.messageId.toHex();
    const bool requireLink = _linkRequiredIds.count(msgIdHex) > 0;

    msg.status = LXMFStatus::SENDING;
    bool sent = false;

    // Try link-based delivery if we have an active link to this peer
    if (_outLink && _outLinkDestHash == msg.destHash
        && _outLink.status() == RNS::Type::Link::ACTIVE) {
        // Link delivery: prepend dest_hash (Python DIRECT format)
        std::vector<uint8_t> linkPayload;
        linkPayload.reserve(16 + payload.size());
        linkPayload.insert(linkPayload.end(), msg.destHash.data(), msg.destHash.data() + 16);
        linkPayload.insert(linkPayload.end(), payload.begin(), payload.end());
        RNS::Bytes linkBytes(linkPayload.data(), linkPayload.size());
        if (linkBytes.size() <= RNS::Type::Reticulum::MDU) {
            // Small enough for single link packet
            Serial.printf("[LXMF] sending via link packet: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Packet packet(_outLink, linkBytes);
            RNS::PacketReceipt receipt = packet.send();
            if (receipt) {
                // microReticulum currently does not validate link-packet delivery
                // proofs, so keep link sends at SENT until upstream proof callbacks
                // are protocol-correct.
                sent = true;
            }
        } else {
            // Too large for single packet — use Resource transfer (chunked).
            // No DELIVERED transition for this path: microReticulum's Transport
            // skips receipt generation for RESOURCE-context packets, and
            // Link::start_resource_transfer doesn't surface a concluded
            // callback. Message stays at SENT. lxmf-py wires this via
            // Resource.set_callback — port that when the lib exposes it.
            Serial.printf("[LXMF] sending via link resource: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            if (_outLink.start_resource_transfer(linkBytes)) {
                sent = true;
            } else {
                Serial.println("[LXMF] resource transfer failed to start");
            }
        }
    }

    // Fallback: opportunistic, or wait for a link if the UI explicitly requested it.
    if (!sent) {
        RNS::Bytes payloadBytes(payload.data(), payload.size());
        if (requireLink) {
            ensureOutboundLink(outDest, msg.destHash, "requested link send");
            uint8_t attempts = ++_linkWaitAttempts[msgIdHex];
            msg.retries++;
            if (attempts >= LXMF_LINK_MAX_WAIT_ATTEMPTS) {
                Serial.printf("[LXMF] Link for %s not established after %u attempts — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(), (unsigned)attempts);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
            Serial.printf("[LXMF] Waiting for link to %s before send (%u/%u)\n",
                          msg.destHash.toHex().substr(0, 8).c_str(),
                          (unsigned)attempts, (unsigned)LXMF_LINK_MAX_WAIT_ATTEMPTS);
            return false;
        }

        // Use opportunistic only when the LXMF payload fits in a normal
        // Reticulum packet and, on LoRa, the final raw packet fits in one
        // RNode RF frame. The 254-byte RNode limit applies after encryption.
        const bool opportunisticPayloadFits = payloadBytes.size() <= RNS::Type::Reticulum::MDU;
        bool packetPacked = false;
        size_t rawLen = 0;
        size_t estimatedLoRaRawLen = 0;
        const bool loraPath = _rns && _rns->isLoRaNextHop(msg.destHash);
        const uint8_t hops = RNS::Transport::hops_to(msg.destHash);

        if (!opportunisticPayloadFits) {
            Serial.printf("[LXMF] Opportunistic payload exceeds MDU: payload=%d mdu=%u\n",
                          (int)payloadBytes.size(), (unsigned)RNS::Type::Reticulum::MDU);
        } else {
            RNS::Packet packet(outDest, payloadBytes);
            const bool transportWrapsHeader2 = loraPath && hops > 1
                && packet.header_type() == RNS::Type::Packet::HEADER_1;

            try {
                packet.pack();
                packetPacked = true;
                rawLen = packet.raw().size();
                estimatedLoRaRawLen = rawLen
                    + (transportWrapsHeader2 ? RNS::Type::Reticulum::DESTINATION_LENGTH : 0);
            } catch (const std::exception& e) {
                Serial.printf("[LXMF] Opportunistic pack failed (%s); trying link delivery\n", e.what());
            }

            const size_t singleFrameCap = RSDECK_RNODE_SINGLE_FRAME_RAW_MAX;
            const bool singleFrameSafe = packetPacked && (!loraPath
                || estimatedLoRaRawLen <= singleFrameCap);

            if (singleFrameSafe) {
                Serial.printf("[LXMF] sending opportunistic: payload=%d raw=%u lora_raw=%u lora=%s hops=%u to %s\n",
                              (int)payloadBytes.size(), (unsigned)rawLen,
                              (unsigned)estimatedLoRaRawLen,
                              loraPath ? "yes" : "no", (unsigned)hops,
                              outDest.hash().toHex().substr(0, 12).c_str());
                RNS::PacketReceipt receipt = packet.send();
                if (receipt) { sent = true; registerProofTracking(receipt, msg); }
            }
        }

        if (!sent) {
            // Too large for single-frame LoRa, or too large for an opportunistic
            // Reticulum packet. Use link/resource delivery so packet loss is
            // recoverable above the physical RNode split-frame layer.
            const size_t singleFrameCap = RSDECK_RNODE_SINGLE_FRAME_RAW_MAX;
            Serial.printf("[LXMF] Message needs link delivery: payload=%d raw=%u lora_raw=%u limit=%u lora=%s hops=%u retry=%d\n",
                          (int)payloadBytes.size(), (unsigned)rawLen,
                          (unsigned)estimatedLoRaRawLen,
                          (unsigned)singleFrameCap,
                          loraPath ? "yes" : "no", (unsigned)hops, msg.retries);
            if (msg.retries % 3 == 0 && (!_outLink || _outLinkDestHash != msg.destHash
                || _outLink.status() != RNS::Type::Link::ACTIVE)) {
                ensureOutboundLink(outDest, msg.destHash, loraPath ? "single-frame LoRa overflow" : "resource transfer");
            }
            msg.retries++;
            if (msg.retries >= 30) {
                Serial.printf("[LXMF] Link for %s not established after %d retries — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
            return false;  // Keep in queue, retry when link is established
        }
    }

    if (sent) {
        msg.status = LXMFStatus::SENT;
        Serial.printf("[LXMF] SENT OK: msgId=%s\n", msg.messageId.toHex().substr(0, 8).c_str());
    } else {
        Serial.println("[LXMF] send FAILED: no receipt");
        msg.status = LXMFStatus::FAILED;
    }

    // Link establishment is now only triggered on-demand when a message
    // is too large for opportunistic delivery (see large-message path above).
    // Speculative background links cause collisions on LoRa when both
    // devices try to establish simultaneously after exchanging short messages.

    return true;
}

void LXMFManager::onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet) {
    if (!_instance) return;
    // Non-link delivery: dest_hash is NOT in LXMF payload (it's in the RNS packet header).
    // Reconstruct full format by prepending it, matching Python LXMRouter.delivery_packet().
    const RNS::Bytes& destHash = packet.destination_hash();
    std::vector<uint8_t> fullData;
    fullData.reserve(destHash.size() + data.size());
    fullData.insert(fullData.end(), destHash.data(), destHash.data() + destHash.size());
    fullData.insert(fullData.end(), data.data(), data.data() + data.size());
    _instance->processIncoming(fullData.data(), fullData.size(), destHash);
}

void LXMFManager::onOutLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    // Accept inbound resources on this link too (peer may send a file
    // or large message back via resource transfer).
    link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
    _instance->_outLink = link;
    _instance->_outLinkDestHash = _instance->_outLinkPendingHash;
    _instance->_outLinkPending = false;
    _instance->_outLinkPendingSinceMs = 0;
    Serial.printf("[LXMF] Outbound link established to %s\n",
                  _instance->_outLinkDestHash.toHex().substr(0, 8).c_str());
}

void LXMFManager::onOutLinkClosed(RNS::Link& link) {
    if (!_instance) return;
    _instance->_outLink = {RNS::Type::NONE};
    _instance->_outLinkPending = false;
    _instance->_outLinkPendingSinceMs = 0;
    Serial.println("[LXMF] Outbound link closed");
}

void LXMFManager::onLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    Serial.printf("[LXMF-DIAG] onLinkEstablished fired! link_id=%s status=%d\n",
        link.link_id().toHex().substr(0, 16).c_str(), (int)link.status());
    // Default link resource_strategy is ACCEPT_NONE — payloads larger
    // than the Link MDU arrive as RESOURCE_ADV and are silently dropped
    // unless we accept them. ACCEPT_ALL is required so resource transfers
    // for large files/text messages round-trip.
    link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
    link.set_packet_callback([](const RNS::Bytes& data, const RNS::Packet& packet) {
        if (!_instance) return;
        Serial.printf("[LXMF-DIAG] Link packet received! %d bytes pkt_dest=%s\n",
            (int)data.size(), packet.destination_hash().toHex().substr(0, 16).c_str());
        // Link delivery: data already contains [dest:16][src:16][sig:64][msgpack]
        // Do NOT use packet.destination_hash() — that's the link_id, not the LXMF dest.
        // Pass empty Bytes so processIncoming uses the destHash from unpackFull().
        _instance->processIncoming(data.data(), data.size(), RNS::Bytes());
    });
}

void LXMFManager::processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash) {
    Serial.printf("[LXMF-DIAG] processIncoming: %d bytes callerDestHash=%s\n",
        (int)len, destHash.size() > 0 ? destHash.toHex().substr(0, 16).c_str() : "EMPTY(link)");
    LXMFMessage msg;
    if (!LXMFMessage::unpackFull(data, len, msg)) {
        Serial.printf("[LXMF] Failed to unpack incoming message (%d bytes)\n", (int)len);
        return;
    }
    if (_rns && msg.sourceHash == _rns->destination().hash()) return;

    Serial.printf("[LXMF-DIAG] unpackFull OK: src=%s dest=%s content_len=%d\n",
        msg.sourceHash.toHex().substr(0, 8).c_str(),
        msg.destHash.toHex().substr(0, 8).c_str(), (int)msg.content.size());

    // Deduplication: skip messages we've already processed
    std::string msgIdHex = msg.messageId.toHex();
    if (_seenMessageIds.count(msgIdHex)) {
        Serial.printf("[LXMF] Duplicate message from %s (already seen)\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str());
        return;
    }
    rememberMessageId(msgIdHex);

    // Only overwrite destHash if caller provided a real one (non-link delivery).
    // For link delivery, unpackFull already parsed the correct destHash from the payload.
    if (destHash.size() > 0) {
        msg.destHash = destHash;
    }
    if (_rns && msg.destHash != _rns->destination().hash()) {
        Serial.printf("[LXMF] Dropping message for wrong destination %s\n",
                      msg.destHash.toHex().substr(0, 8).c_str());
        return;
    }
    // Use local receive time for incoming messages so all timestamps in
    // the conversation reflect THIS device's clock, not the sender's.
    // Prevents confusing display when a peer's clock is wrong/unsynced.
    {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            msg.timestamp = (double)now;
        }
    }
    Serial.printf("[LXMF] Message from %s (%d bytes) content_len=%d\n",
                  msg.sourceHash.toHex().substr(0, 8).c_str(), (int)len, (int)msg.content.size());
    // Fire the listener first so the UI (location extraction, notifications)
    // sees the message before it lands in the store; then persist.
    if (_onMessage) { _onMessage(msg); }
    if (_store) { _store->saveMessage(msg); }
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

std::vector<LXMFMessage> LXMFManager::getRecentMessages(const std::string& peerHex, size_t maxMessages) const {
    if (_store) return _store->loadConversationTail(peerHex, maxMessages);
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

bool LXMFManager::deleteConversation(const std::string& peerHex) {
    for (auto it = _outQueue.begin(); it != _outQueue.end(); ) {
        if (it->destHash.toHex() == peerHex) it = _outQueue.erase(it);
        else ++it;
    }
    for (auto it = _pendingProofs.begin(); it != _pendingProofs.end(); ) {
        if (it->second.peerHex == peerHex) it = _pendingProofs.erase(it);
        else ++it;
    }
    return _store ? _store->deleteConversation(peerHex) : false;
}

void LXMFManager::registerProofTracking(RNS::PacketReceipt& receipt, const LXMFMessage& msg) {
    if (!receipt) return;
    std::string rh = receipt.hash().toHex();
    PendingProof pending;
    pending.peerHex = msg.destHash.toHex();
    pending.timestamp = msg.timestamp;
    pending.savedCounter = msg.savedCounter;
    pending.createdMs = millis();
    pending.proofAttempts = (uint8_t)std::min(msg.retries, 255);
    pending.msg = msg;
    pending.msg.status = LXMFStatus::SENT;
    _pendingProofs[rh] = pending;
    receipt.set_delivery_callback(&LXMFManager::onProofDelivered);
    receipt.set_timeout(60);
    receipt.set_timeout_callback(&LXMFManager::onProofTimeout);
}

void LXMFManager::onProofDelivered(const RNS::PacketReceipt& r) {
    if (!_instance) return;
    std::string rh = r.hash().toHex();
    auto it = _pendingProofs.find(rh);
    if (it == _pendingProofs.end()) return;
    PendingProof p = it->second;
    _pendingProofs.erase(it);

    if (_instance->_store) {
        if (p.savedCounter > 0) {
            _instance->_store->updateMessageStatusByCounter(p.peerHex, p.savedCounter, false, LXMFStatus::DELIVERED);
        } else {
            _instance->_store->updateMessageStatus(p.peerHex, p.timestamp, false, LXMFStatus::DELIVERED);
        }
    }
    if (_instance->_statusCb) {
        _instance->_statusCb(p.peerHex, p.timestamp, p.savedCounter, LXMFStatus::DELIVERED);
    }
    Serial.printf("[LXMF] DELIVERED proof for %s @ %.0f\n",
                  p.peerHex.substr(0, 12).c_str(), p.timestamp);
}

void LXMFManager::onProofTimeout(const RNS::PacketReceipt& r) {
    handleProofTimeoutHash(r.hash().toHex());
}

void LXMFManager::handleProofTimeoutHash(const std::string& receiptHash) {
    if (!_instance) return;
    auto it = _pendingProofs.find(receiptHash);
    if (it == _pendingProofs.end()) return;
    PendingProof p = it->second;
    _pendingProofs.erase(it);

    p.proofAttempts++;
    bool retry = p.proofAttempts < 3 && (int)_instance->_outQueue.size() < RSDECK_MAX_OUTQUEUE;
    LXMFStatus nextStatus = retry ? LXMFStatus::QUEUED : LXMFStatus::FAILED;

    if (_instance->_store) {
        if (p.savedCounter > 0) {
            _instance->_store->updateMessageStatusByCounter(p.peerHex, p.savedCounter, false, nextStatus);
        } else {
            _instance->_store->updateMessageStatus(p.peerHex, p.timestamp, false, nextStatus);
        }
    }
    if (_instance->_statusCb) {
        _instance->_statusCb(p.peerHex, p.timestamp, p.savedCounter, nextStatus);
    }

    if (retry) {
        p.msg.status = LXMFStatus::QUEUED;
        p.msg.retries = p.proofAttempts;
        p.msg.lastRetryMs = 0;
        _instance->_outQueue.push_back(p.msg);
        Serial.printf("[LXMF] Proof timeout; requeued %s attempt %u/3\n",
                      p.peerHex.substr(0, 8).c_str(), (unsigned)p.proofAttempts + 1);
    } else {
        Serial.printf("[LXMF] Proof timeout; marking FAILED for %s\n",
                      p.peerHex.substr(0, 8).c_str());
    }
}
