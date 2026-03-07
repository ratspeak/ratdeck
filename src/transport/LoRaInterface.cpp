#include "LoRaInterface.h"
#include "config/BoardConfig.h"

// RNode on-air framing constants (from RNode_Firmware_CE Framing.h / Config.h)
// Every LoRa packet has a 1-byte header: upper nibble = random sequence, lower nibble = flags
#define RNODE_HEADER_L      1
#define RNODE_FLAG_SPLIT    0x01
#define RNODE_NIBBLE_SEQ    0xF0

LoRaInterface::LoRaInterface(SX1262* radio, const char* name)
    : RNS::InterfaceImpl(name), _radio(radio)
{
    _IN = true;
    _OUT = true;
    _bitrate = 2000;        // Approximate for SF8/125kHz
    _HW_MTU = MAX_PACKET_SIZE - RNODE_HEADER_L;  // 254 bytes payload (1 byte reserved for RNode header)
}

LoRaInterface::~LoRaInterface() {
    stop();
}

bool LoRaInterface::start() {
    if (!_radio || !_radio->isRadioOnline()) {
        Serial.println("[LORA_IF] Radio not available");
        _online = false;
        return false;
    }
    _online = true;
    _radio->receive();
    Serial.println("[LORA_IF] Interface started");
    return true;
}

void LoRaInterface::stop() {
    _online = false;
    Serial.println("[LORA_IF] Interface stopped");
}

void LoRaInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online || !_radio) return;

    // Build RNode-compatible 1-byte header:
    //   Upper nibble: random sequence number (for split-packet tracking)
    //   Lower nibble: flags (FLAG_SPLIT=0x01 if packet won't fit in single frame)
    uint8_t header = (uint8_t)(random(256)) & RNODE_NIBBLE_SEQ;  // Random upper nibble, flags=0

    Serial.printf("[LORA_IF] TX: sending %d bytes, radio: SF%d BW%lu CR%d preamble=%ld freq=%lu txp=%d\n",
        data.size(),
        _radio->getSpreadingFactor(),
        (unsigned long)_radio->getSignalBandwidth(),
        _radio->getCodingRate4(),
        _radio->getPreambleLength(),
        (unsigned long)_radio->getFrequency(),
        _radio->getTxPower());

    _radio->beginPacket();
    _radio->write(header);                        // 1-byte RNode header
    _radio->write(data.data(), data.size());      // Reticulum packet payload
    bool sent = _radio->endPacket();

    if (sent) {
        Serial.printf("[LORA_IF] TX %d+1 bytes (hdr=0x%02X)\n", data.size(), header);
        InterfaceImpl::handle_outgoing(data);
    } else {
        Serial.println("[LORA_IF] TX failed (timeout)");
    }

    // Return to RX mode
    _radio->receive();
}

void LoRaInterface::loop() {
    if (!_online || !_radio) return;

    // Periodic RX debug: dump RSSI + chip status every 30 seconds
    static unsigned long lastRxDebug = 0;
    if (millis() - lastRxDebug > 30000) {
        lastRxDebug = millis();
        int rssi = _radio->currentRssi();
        uint8_t status = _radio->getStatus();
        uint8_t chipMode = (status >> 4) & 0x07;
        Serial.printf("[LORA_IF] RX: RSSI=%d dBm, status=0x%02X(mode=%d)\n",
            rssi, status, chipMode);
    }

    int packetSize = _radio->parsePacket();
    if (packetSize > RNODE_HEADER_L) {
        // parsePacket() already read the FIFO into packetBuffer() — copy from there
        // (avoid calling readBytes() which would re-read the FIFO via read())
        uint8_t raw[MAX_PACKET_SIZE];
        memcpy(raw, _radio->packetBuffer(), packetSize);

        // Strip the 1-byte RNode header, pass only the Reticulum payload
        uint8_t header = raw[0];
        int payloadSize = packetSize - RNODE_HEADER_L;

        Serial.printf("[LORA_IF] RX %d bytes (hdr=0x%02X, payload=%d), RSSI=%d, SNR=%.1f\n",
                      packetSize, header, payloadSize,
                      _radio->packetRssi(), _radio->packetSnr());

        // Hex dump first 32 bytes for debugging interop
        Serial.printf("[LORA_IF] RX hex: ");
        for (int i = 0; i < packetSize && i < 32; i++) Serial.printf("%02X ", raw[i]);
        Serial.println();

        RNS::Bytes buf(payloadSize);
        memcpy(buf.writable(payloadSize), raw + RNODE_HEADER_L, payloadSize);
        InterfaceImpl::handle_incoming(buf);

        // Re-enter RX
        _radio->receive();
    } else if (packetSize > 0) {
        // Packet too small (only header, no payload) — discard
        Serial.printf("[LORA_IF] RX runt packet (%d bytes), discarding\n", packetSize);
        _radio->receive();
    }
}
