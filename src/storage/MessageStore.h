#pragma once

#include <Arduino.h>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "reticulum/LXMFMessage.h"
#include <vector>
#include <string>
#include <map>

struct ConversationSummary {
    double lastTimestamp = 0;
    std::string lastPreview;    // first ~20 chars of last message
    bool lastIncoming = false;
    int unreadCount = 0;
    int totalCount = 0;
};

class MessageStore {
public:
    bool begin(FlashStore* flash, SDStore* sd = nullptr);

    bool saveMessage(const LXMFMessage& msg);
    std::vector<LXMFMessage> loadConversation(const std::string& peerHex) const;
    const std::vector<std::string>& conversations() const { return _conversations; }
    void refreshConversations();
    int messageCount(const std::string& peerHex) const;
    bool deleteConversation(const std::string& peerHex);
    void markConversationRead(const std::string& peerHex);
    bool updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus);

    const ConversationSummary* getSummary(const std::string& peerHex) const;
    int totalUnreadCount() const;

private:
    String conversationDir(const std::string& peerHex) const;
    String sdConversationDir(const std::string& peerHex) const;
    void enforceFlashLimit(const std::string& peerHex);
    void enforceSDLimit(const std::string& peerHex);
    void migrateFlashToSD();
    void migrateTruncatedDirs();
    void initReceiveCounter();
    void buildSummaries();

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    std::vector<std::string> _conversations;
    std::map<std::string, ConversationSummary> _summaries;
    uint32_t _nextReceiveCounter = 0;
};
