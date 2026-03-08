#pragma once

#include <Arduino.h>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "reticulum/LXMFMessage.h"
#include <vector>
#include <string>
#include <map>

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

private:
    String conversationDir(const std::string& peerHex) const;
    String sdConversationDir(const std::string& peerHex) const;
    void enforceFlashLimit(const std::string& peerHex);
    void enforceSDLimit(const std::string& peerHex);
    void migrateFlashToSD();
    void migrateTruncatedDirs();

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    std::vector<std::string> _conversations;
};
