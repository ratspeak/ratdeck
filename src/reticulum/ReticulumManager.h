#pragma once

#include <Reticulum.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Interface.h>
#include <FileSystem.h>
#include <Utilities/OS.h>

#include "transport/LoRaInterface.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"

class LittleFSFileSystem : public RNS::FileSystemImpl {
public:
    virtual bool init() override;
    virtual bool file_exists(const char* file_path) override;
    virtual size_t read_file(const char* file_path, RNS::Bytes& data) override;
    virtual size_t write_file(const char* file_path, const RNS::Bytes& data) override;
    virtual RNS::FileStream open_file(const char* file_path, RNS::FileStream::MODE file_mode) override;
    virtual bool remove_file(const char* file_path) override;
    virtual bool rename_file(const char* from, const char* to) override;
    virtual bool directory_exists(const char* directory_path) override;
    virtual bool create_directory(const char* directory_path) override;
    virtual bool remove_directory(const char* directory_path) override;
    virtual std::list<std::string> list_directory(const char* directory_path, Callbacks::DirectoryListing callback = nullptr) override;
    virtual size_t storage_size() override;
    virtual size_t storage_available() override;
};

class ReticulumManager {
public:
    ReticulumManager()
        : _reticulum({RNS::Type::NONE}),
          _identity({RNS::Type::NONE}),
          _destination({RNS::Type::NONE}),
          _loraIface({RNS::Type::NONE}) {}

    bool begin(SX1262* radio, FlashStore* flash);
    void setSDStore(SDStore* sd) { _sd = sd; }
    void setTransportEnabled(bool enabled) { _transportEnabled = enabled; }
    void loop();
    void persistData();

    const RNS::Identity& identity() const { return _identity; }
    String identityHash() const;
    String destinationHashHex() const;

    bool isTransportActive() const { return _transportActive; }
    size_t pathCount() const;
    size_t linkCount() const;

    void announce(const RNS::Bytes& appData = {});
    unsigned long lastAnnounceTime() const { return _lastAnnounceTime; }

    RNS::Destination& destination() { return _destination; }
    LoRaInterface* loraInterface() { return _loraImpl; }

private:
    bool loadOrCreateIdentity();
    void saveIdentityToAll(const RNS::Bytes& keyData);

    RNS::Reticulum _reticulum;
    RNS::Identity _identity;
    RNS::Destination _destination;
    RNS::Interface _loraIface;
    LoRaInterface* _loraImpl = nullptr;
    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _transportEnabled = false;  // Default: endpoint mode (no rebroadcast)
    bool _transportActive = false;
    unsigned long _lastPersist = 0;
    unsigned long _lastAnnounceTime = 0;
};
