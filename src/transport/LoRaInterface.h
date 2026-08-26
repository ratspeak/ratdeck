#pragma once

#include <Interface.h>
#include "radio/SX1262.h"
#include <deque>

class LoRaInterface : public RNS::InterfaceImpl {
public:
    LoRaInterface(SX1262* radio, const char* name = "LoRaInterface");
    virtual ~LoRaInterface();

    virtual bool start() override;
    virtual void stop() override;
    virtual void loop() override;

    virtual inline std::string toString() const override {
        return "LoRaInterface[" + _name + "]";
    }

    float airtimeUtilization() const;

    // Last received packet signal quality
    int lastRxRssi() const { return _lastRxRssi; }
    float lastRxSnr() const { return _lastRxSnr; }
    bool isOnline() const { return _online; }
    unsigned long splitRxTimeoutMs() const { return _splitRxTimeoutMs; }
    float singleFrameAirtimeMs() const { return _singleFrameAirtimeMs; }
    uint32_t bitrate() const { return _bitrate; }

    // TX queue depth probes — LXMFManager paces the outbound drain so
    // burst sends don't push faster than the radio can air them.
    int txQueueDepth() const { return (int)_txQueue.size(); }
    int txQueueFree() const { return TX_QUEUE_MAX - (int)_txQueue.size(); }

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override;

private:
    void transmitNow(const RNS::Bytes& data);
    void refreshRadioTiming(bool forceLog = false);
    unsigned long computeSplitRxTimeoutMs(float frameAirtimeMs) const;

    SX1262* _radio;
    bool _txPending = false;
    RNS::Bytes _txData;

    // TX queue: buffer packets when radio is busy instead of dropping.
    // 24 is sized for a worst-case burst send of short LXMF messages on
    // LoRa plus headroom. Overflow rejects newest (not drop-oldest) so an
    // in-flight burst stays intact and the caller can retry once the radio
    // drains.
    static constexpr int TX_QUEUE_MAX = 24;
    std::deque<RNS::Bytes> _txQueue;

    // Split-packet TX state: when a packet > 254 bytes, send in two LoRa frames
    bool _splitTxPending = false;
    RNS::Bytes _splitTxRemaining;
    uint8_t _splitTxHeader = 0;

    // Split-packet RX state: reassemble two LoRa frames into one Reticulum packet
    static constexpr unsigned long SPLIT_RX_TIMEOUT_FLOOR_MS = 5000;
    static constexpr unsigned long SPLIT_RX_TIMEOUT_CEIL_MS = 60000;
    static constexpr unsigned long SPLIT_RX_TIMEOUT_MARGIN_MS = 2000;
    static constexpr float SPLIT_RX_TIMEOUT_MULT = 1.5f;
    bool _splitRxPending = false;
    uint8_t _splitRxSeq = 0;
    RNS::Bytes _splitRxBuffer;
    unsigned long _splitRxTimestamp = 0;
    unsigned long _splitRxTimeoutMs = SPLIT_RX_TIMEOUT_FLOOR_MS;
    float _singleFrameAirtimeMs = 0;
    unsigned long _lastTimingRefreshMs = 0;

    int _lastRxRssi = 0;
    float _lastRxSnr = 0;

    unsigned long _airtimeWindowStart = 0;
    float _airtimeAccumMs = 0;
    static constexpr unsigned long AIRTIME_WINDOW_MS = 60000;
public:
    static constexpr float AIRTIME_THROTTLE = 0.25f;
};
