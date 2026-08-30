// =============================================================================
// SX1262 LoRa Radio Driver — Direct port from Ratputer
// Only change: pin assignments via BoardConfig.h (T-Deck Plus)
// =============================================================================

#include "SX1262.h"
#include "config/BoardConfig.h"
#include "platform/CoreSync.h"

SX1262* SX1262::_instance = nullptr;

namespace {
constexpr uint8_t IMAGE_CAL_UNSUPPORTED = 0xFF;

uint8_t imageCalParams(uint32_t frequency, uint8_t out[2]) {
    if      (frequency >= 430000000UL && frequency <= 440000000UL) { out[0] = 0x6B; out[1] = 0x6F; return 0; }
    else if (frequency >= 470000000UL && frequency <= 510000000UL) { out[0] = 0x75; out[1] = 0x81; return 1; }
    else if (frequency >= 779000000UL && frequency <= 787000000UL) { out[0] = 0xC1; out[1] = 0xC5; return 2; }
    else if (frequency >= 863000000UL && frequency <= 870000000UL) { out[0] = 0xD7; out[1] = 0xDB; return 3; }
    else if (frequency >= 902000000UL && frequency <= 928000000UL) { out[0] = 0xE1; out[1] = 0xE9; return 4; }
    return IMAGE_CAL_UNSUPPORTED;
}

const char* packetTypeName(uint8_t packetType) {
    switch (packetType) {
        case 0x00: return "GFSK";
        case 0x01: return "LoRa";
        case 0x02: return "LR-FHSS";
        default: return "unknown";
    }
}
}

SX1262::SX1262(SPIClass* spi, int ss, int sclk, int mosi, int miso,
               int reset, int irq, int busy, int rxen,
               bool tcxo, bool dio2_as_rf_switch)
    : _spiSettings(SPI_FREQUENCY, MSBFIRST, SPI_MODE0),
      _spiModem(spi), _ss(ss), _sclk(sclk), _mosi(mosi), _miso(miso),
      _reset(reset), _irq(irq), _busy(busy), _rxen(rxen),
      _frequency(0), _sf(0x07), _bw(0x04), _cr(0x01),
      _ldro(false), _preambleLength(LORA_PREAMBLE_SYMBOLS_MIN),
      _packetIndex(0), _implicitHeaderMode(0),
      _payloadLength(255), _crcMode(1),
      _fifo_tx_addr_ptr(0), _fifo_rx_addr_ptr(0),
      _preinitDone(false), _radioOnline(false),
      _tcxo(tcxo), _dio2_as_rf_switch(dio2_as_rf_switch),
      _onReceive(nullptr), _preambleDetectedAt(0),
      _loraPreambleTimeMs(0), _loraHeaderTimeMs(0), _loraSymbolTimeMs(0)
{
    _txp = 14;
    _instance = this;
    memset(_packet, 0, sizeof(_packet));
}

bool SX1262::preInit() {
    pinMode(_ss, OUTPUT);
    digitalWrite(_ss, HIGH);

    // SPI bus is initialized by main.cpp — do NOT call _spiModem->begin() here

    long start = millis();
    uint8_t syncmsb = 0, synclsb = 0;
    int probes = 0;
    while (((millis() - start) < 2000) && (millis() >= start)) {
        syncmsb = readRegister(REG_SYNC_WORD_MSB_6X);
        synclsb = readRegister(REG_SYNC_WORD_LSB_6X);
        uint16_t sw = (uint16_t)(syncmsb << 8 | synclsb);
        probes++;
        Serial.printf("[SX1262] preInit probe %d: syncword=0x%04X\n", probes, sw);
        if (sw == 0x1424 || sw == 0x4434) {
            break;
        }
        delay(100);
    }

    uint16_t sw = (uint16_t)(syncmsb << 8 | synclsb);
    if (sw != 0x1424 && sw != 0x4434) {
        Serial.printf("[SX1262] preInit FAILED: syncword=0x%04X after %d probes\n", sw, probes);
        return false;
    }

    Serial.printf("[SX1262] preInit OK: syncword=0x%04X\n", sw);
    _preinitDone = true;
    return true;
}

uint8_t IRAM_ATTR SX1262::readRegister(uint16_t address) {
    return singleTransfer(OP_READ_REGISTER_6X, address, 0x00);
}

void SX1262::writeRegister(uint16_t address, uint8_t value) {
    singleTransfer(OP_WRITE_REGISTER_6X, address, value);
}

uint8_t IRAM_ATTR SX1262::singleTransfer(uint8_t opcode, uint16_t address, uint8_t value) {
    waitOnBusy();
    uint8_t response;
    CoreSync::SpiBusGuard busGuard;  // serialize against display/SD on shared FSPI bus
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    _spiModem->transfer((address & 0xFF00) >> 8);
    _spiModem->transfer(address & 0x00FF);
    if (opcode == OP_READ_REGISTER_6X) {
        _spiModem->transfer(0x00);
    }
    response = _spiModem->transfer(value);
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
    return response;
}

void SX1262::executeOpcode(uint8_t opcode, uint8_t* buffer, uint8_t size) {
    waitOnBusy();
    CoreSync::SpiBusGuard busGuard;
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    for (int i = 0; i < size; i++) {
        _spiModem->transfer(buffer[i]);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::executeOpcodeRead(uint8_t opcode, uint8_t* buffer, uint8_t size) {
    waitOnBusy();
    CoreSync::SpiBusGuard busGuard;
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    _spiModem->transfer(0x00);
    for (int i = 0; i < size; i++) {
        buffer[i] = _spiModem->transfer(0x00);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::writeBuffer(const uint8_t* buffer, size_t size) {
    waitOnBusy();
    CoreSync::SpiBusGuard busGuard;
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(OP_FIFO_WRITE_6X);
    _spiModem->transfer(_fifo_tx_addr_ptr);
    for (size_t i = 0; i < size; i++) {
        _spiModem->transfer(buffer[i]);
        _fifo_tx_addr_ptr++;
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::readBuffer(uint8_t* buffer, size_t size) {
    waitOnBusy();
    CoreSync::SpiBusGuard busGuard;
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(OP_FIFO_READ_6X);
    _spiModem->transfer(_fifo_rx_addr_ptr);
    _spiModem->transfer(0x00);
    for (size_t i = 0; i < size; i++) {
        buffer[i] = _spiModem->transfer(0x00);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::waitOnBusy() {
    unsigned long t = millis();
    if (_busy != -1) {
        while (digitalRead(_busy) == HIGH) {
            if (millis() >= (t + 100)) break;
            if (_yieldCb) _yieldCb();
        }
    }
}

void SX1262::reset() {
    if (_reset != -1) {
        pinMode(_reset, OUTPUT);
        digitalWrite(_reset, LOW);
        delay(10);
        digitalWrite(_reset, HIGH);
        delay(100);  // Allow TCXO + shared SPI to stabilize
    }
}

void SX1262::calibrate() {
    // Calibrate must be issued from STDBY_RC per datasheet.
    // TCXO is already configured via DIO3 with sufficient timeout,
    // so the 32MHz reference is available for PLL calibration.
    uint8_t mode_byte = MODE_STDBY_RC_6X;
    executeOpcode(OP_STANDBY_6X, &mode_byte, 1);
    uint8_t cal = MASK_CALIBRATE_ALL;
    executeOpcode(OP_CALIBRATE_6X, &cal, 1);
    delay(5);
    waitOnBusy();
}

bool SX1262::calibrate_image(uint32_t frequency) {
    uint8_t image_freq[2] = {0};
    uint8_t band = imageCalParams(frequency, image_freq);

    if (band == IMAGE_CAL_UNSUPPORTED) {
        Serial.printf("[SX1262] No image calibration table for %lu Hz\n",
                      (unsigned long)frequency);
        return false;
    }

    if (_imageCalBand == band) return true;

    standby();
    executeOpcode(OP_CALIBRATE_IMAGE_6X, image_freq, 2);
    waitOnBusy();
    _imageCalBand = band;
    Serial.printf("[SX1262] Image calibrated for %lu Hz (0x%02X 0x%02X)\n",
                  (unsigned long)frequency, image_freq[0], image_freq[1]);
    return true;
}

void SX1262::enableTCXO() {
    if (_tcxo) {
        // Timeout: how long SX1262 waits for TCXO to stabilize when entering
        // STDBY_XOSC/TX/RX. Units = 15.625µs. 0x00A000 = 640ms (matches RadioLib).
        // If too short, chip stays in STDBY_RC and calibration uses RC oscillator.
        uint8_t buf[4] = {LORA_TCXO_VOLTAGE, 0x00, 0xA0, 0x00};
        executeOpcode(OP_DIO3_TCXO_CTRL_6X, buf, 4);
    }
}

void SX1262::enableDio2RfSwitch() {
    if (_dio2_as_rf_switch) {
        uint8_t byte = 0x01;
        executeOpcode(OP_DIO2_RF_CTRL_6X, &byte, 1);
    }
}

bool SX1262::loraMode() {
    uint8_t mode = MODE_LONG_RANGE_MODE_6X;
    for (int attempt = 0; attempt < 5; attempt++) {
        executeOpcode(OP_PACKET_TYPE_6X, &mode, 1);
        delay(2);

        uint8_t packetType = getPacketType();
        if (packetType == MODE_LONG_RANGE_MODE_6X) {
            if (attempt > 0) {
                Serial.printf("[SX1262] LoRa packet type set after %d attempts\n", attempt + 1);
            }
            return true;
        }

        Serial.printf("[SX1262] SetPacketType LoRa attempt %d/5 read back 0x%02X (%s)\n",
                      attempt + 1, packetType, packetTypeName(packetType));
    }

    Serial.println("[SX1262] Failed to enter LoRa packet mode");
    return false;
}

bool SX1262::ensureLoRaMode(const char* context) {
    uint8_t packetType = getPacketType();
    if (packetType == MODE_LONG_RANGE_MODE_6X) return true;

    Serial.printf("[SX1262] %s found packet_type=0x%02X (%s), reasserting LoRa\n",
                  context ? context : "config", packetType, packetTypeName(packetType));
    standby();
    return loraMode();
}

void SX1262::rxAntEnable() {
    if (_rxen != -1) {
        digitalWrite(_rxen, HIGH);
    }
}

bool SX1262::begin(uint32_t frequency) {
    _frequency = frequency;
    reset();
    _imageCalBand = IMAGE_CAL_UNSUPPORTED;
    if (_busy != -1) { pinMode(_busy, INPUT); }
    if (!_preinitDone) {
        if (!preInit()) {
            return false;
        }
    }
    if (_rxen != -1) { pinMode(_rxen, OUTPUT); }

    // Match RadioLib's proven SX1262 init sequence:
    // 1. Configure TCXO via DIO3 (with generous timeout)
    // 2. Enter STDBY_XOSC to actually start the TCXO
    // 3. Set regulator mode
    // 4. Calibrate from STDBY_RC (TCXO stays powered via DIO3)
    enableTCXO();
    delay(10);

    // Force STDBY_XOSC to start the TCXO oscillator
    standby();

    // Set regulator mode. The T-Deck Plus integrated radio works in DC-DC
    // mode; cap-style modules may need board-specific LDO-only mode.
    uint8_t regMode = LORA_USE_DCDC_REGULATOR ? 0x01 : 0x00;  // 0x00=LDO, 0x01=DC-DC
    Serial.printf("[SX1262] Regulator mode: %s\n",
                  LORA_USE_DCDC_REGULATOR ? "DC-DC" : "LDO");
    executeOpcode(OP_REGULATOR_MODE_6X, &regMode, 1);

    // Calibrate from STDBY_RC with TCXO already running
    calibrate();
    calibrate_image(_frequency);

    // Set LoRa packet type and return to STDBY_XOSC
    if (!loraMode()) {
        return false;
    }
    standby();

    // Post-calibration diagnostic
    uint16_t postCalErr = getDeviceErrors();
    uint8_t iqReg = readRegister(REG_IQ_POLARITY_6X);
    Serial.printf("[SX1262] Post-cal DevErrors: 0x%04X%s IQ_REG=0x%02X\n",
        postCalErr, (postCalErr & 0x40) ? " *** PLL FAIL ***" : " OK", iqReg);
    clearDeviceErrors();

    setSyncWord(SYNC_WORD_6X);

    enableDio2RfSwitch();

    rxAntEnable();
    setFrequency(_frequency);
    setTxPower(_txp);
    enableCrc();
    writeRegister(REG_LNA_6X, 0x96);

    uint8_t basebuf[2] = {0};
    executeOpcode(OP_BUFFER_BASE_ADDR_6X, basebuf, 2);
    setModulationParams(_sf, _bw, _cr, _ldro);
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);

    uint8_t irqBuf[8];
    irqBuf[0] = 0xFF; irqBuf[1] = 0xFF;
    irqBuf[2] = 0x00; irqBuf[3] = IRQ_RX_DONE_MASK_6X;
    irqBuf[4] = 0x00; irqBuf[5] = 0x00;
    irqBuf[6] = 0x00; irqBuf[7] = 0x00;
    executeOpcode(OP_SET_IRQ_FLAGS_6X, irqBuf, 8);
    enableDio2RfSwitch();

    // Keep TCXO running between TX/RX transitions (don't fall back to RC oscillator)
    uint8_t fallback = MODE_FALLBACK_STDBY_XOSC_6X;
    executeOpcode(OP_RX_TX_FALLBACK_MODE_6X, &fallback, 1);

    clearDeviceErrors();
    _radioOnline = true;
    return true;
}

void SX1262::end() {
    sleep();
    _spiModem->end();
    _radioOnline = false;
    _preinitDone = false;
}

int SX1262::beginPacket(int implicitHeader) {
    standby();
    if (!ensureLoRaMode("beginPacket")) return 0;

    if (implicitHeader) { implicitHeaderMode(); } else { explicitHeaderMode(); }
    _payloadLength = 0;
    _fifo_tx_addr_ptr = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    return 1;
}

int SX1262::endPacket(bool async) {
    if (!ensureLoRaMode("endPacket")) return 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    enableDio2RfSwitch();

    uint8_t timeout[3] = {0};
    _txStartMs = millis();
    _txTimeoutMs = _txStartMs + (uint32_t)(getAirtime(_payloadLength) * MODEM_TIMEOUT_MULT) + 2000;
    executeOpcode(OP_TX_6X, timeout, 3);

    if (async) {
        _txActive = true;
        Serial.printf("[SX1262] TX ASYNC: payload=%d calc=%.0fms\n",
                      _payloadLength, getAirtime(_payloadLength));
        return 1;
    }

    // Blocking mode: wait for TX completion
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);

    while ((millis() < _txTimeoutMs) && ((buf[1] & IRQ_TX_DONE_MASK_6X) == 0)) {
        buf[0] = 0x00; buf[1] = 0x00;
        executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
        yield();
        if (_yieldCb) _yieldCb();
    }
    uint32_t txActual = millis() - _txStartMs;
    bool timed_out = millis() > _txTimeoutMs;

    if (timed_out) {
        Serial.printf("[SX1262] TX TIMEOUT: payload=%d actual=%dms calc=%.0fms\n",
                      _payloadLength, txActual, getAirtime(_payloadLength));
    } else {
        Serial.printf("[SX1262] TX OK: payload=%d actual=%dms calc=%.0fms\n",
                      _payloadLength, txActual, getAirtime(_payloadLength));
    }

    uint8_t mask[2] = {0x00, IRQ_TX_DONE_MASK_6X};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, mask, 2);
    return !timed_out;
}

bool SX1262::isTxBusy() {
    if (!_txActive) return false;

    // Check for timeout
    if (millis() > _txTimeoutMs) {
        Serial.printf("[SX1262] TX ASYNC TIMEOUT after %dms\n",
                      (int)(millis() - _txStartMs));
        uint8_t mask[2] = {0x00, IRQ_TX_DONE_MASK_6X};
        executeOpcode(OP_CLEAR_IRQ_STATUS_6X, mask, 2);
        _txActive = false;
        return false;
    }

    // Poll IRQ status
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    if (buf[1] & IRQ_TX_DONE_MASK_6X) {
        Serial.printf("[SX1262] TX ASYNC OK: %dms\n",
                      (int)(millis() - _txStartMs));
        uint8_t mask[2] = {0x00, IRQ_TX_DONE_MASK_6X};
        executeOpcode(OP_CLEAR_IRQ_STATUS_6X, mask, 2);
        _txActive = false;
        return false;
    }

    return true;  // Still transmitting
}

size_t SX1262::write(uint8_t byte) { return write(&byte, 1); }

size_t SX1262::write(const uint8_t* buffer, size_t size) {
    if ((_payloadLength + size) > MAX_PACKET_SIZE) {
        Serial.printf("[SX1262] WARNING: write() truncating %d->%d bytes (payload=%d max=%d)\n",
                      (int)size, (int)(MAX_PACKET_SIZE - _payloadLength),
                      (int)_payloadLength, (int)MAX_PACKET_SIZE);
        size = MAX_PACKET_SIZE - _payloadLength;
    }
    writeBuffer(buffer, size);
    _payloadLength += size;
    return size;
}

void SX1262::receive(int size) {
    if (!ensureLoRaMode("receive")) return;

    uint8_t clear[2] = {0xFF, 0xFF};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, clear, 2);

    if (size > 0) {
        implicitHeaderMode();
        _payloadLength = size;
        setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    } else {
        explicitHeaderMode();
    }

    // Set up DIO1 interrupt for RX done (enables packetAvailable flag)
    if (!_onReceive) {
        pinMode(_irq, INPUT);
        uint8_t irqBuf[8] = {0xFF, 0xFF, 0x00, IRQ_RX_DONE_MASK_6X, 0x00, 0x00, 0x00, 0x00};
        executeOpcode(OP_SET_IRQ_FLAGS_6X, irqBuf, 8);
        enableDio2RfSwitch();
        attachInterrupt(digitalPinToInterrupt(_irq), onDio0Rise, RISING);
    }

    packetAvailable = false;

    if (_rxen != -1) { rxAntEnable(); }
    uint8_t mode[3] = {0xFF, 0xFF, 0xFF};
    executeOpcode(OP_RX_6X, mode, 3);
}

int SX1262::parsePacket(int size) {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    if ((buf[1] & IRQ_RX_DONE_MASK_6X) == 0) { return 0; }

    uint8_t mask[2] = {0x00, IRQ_RX_DONE_MASK_6X};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, mask, 2);

    // Read buffer info
    uint8_t rxinfo[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxinfo, 2);
    int pktLen = rxinfo[0];
    _fifo_rx_addr_ptr = rxinfo[1];

    // Read RSSI/SNR before clearing IRQ
    uint8_t pktStat[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, pktStat, 3);
    float rssi = -float(pktStat[0]) / 2.0;
    float snr  = float((int8_t)pktStat[1]) * 0.25;

    bool crcOk = getPacketValidity();

    // Always read FIFO data for diagnostics
    _packetIndex = 0;
    readBuffer(_packet, pktLen);

    if (!crcOk) {
        Serial.printf("[SX1262] RX CRC FAIL: %d bytes RSSI=%.0f SNR=%.1f\n",
                      pktLen, rssi, snr);
        // Full hex dump for diagnosis
        for (int i = 0; i < pktLen; i++) {
            Serial.printf("%02X ", _packet[i]);
            if ((i & 0x1F) == 0x1F) Serial.println();
        }
        Serial.println();
        receive();
        return 0;
    }

    return pktLen;
}

int IRAM_ATTR SX1262::available() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, buf, 2);
    return buf[0] - _packetIndex;
}

int IRAM_ATTR SX1262::read() {
    if (!available()) { return -1; }
    if (_packetIndex == 0) {
        uint8_t rxbuf[2] = {0};
        executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
        int size = rxbuf[0];
        _fifo_rx_addr_ptr = rxbuf[1];
        readBuffer(_packet, size);
    }
    uint8_t byte = _packet[_packetIndex];
    _packetIndex++;
    return byte;
}

int SX1262::peek() {
    if (!available()) { return -1; }
    if (_packetIndex == 0) {
        uint8_t rxbuf[2] = {0};
        executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
        int size = rxbuf[0];
        _fifo_rx_addr_ptr = rxbuf[1];
        readBuffer(_packet, size);
    }
    return _packet[_packetIndex];
}

void SX1262::readBytes(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        int b = read();
        if (b < 0) break;
        buffer[i] = (uint8_t)b;
    }
}

int IRAM_ATTR SX1262::currentRssi() {
    uint8_t byte = 0;
    executeOpcodeRead(OP_CURRENT_RSSI_6X, &byte, 1);
    return -(int(byte)) / 2;
}

int SX1262::packetRssi() {
    uint8_t buf[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, buf, 3);
    return -buf[0] / 2;
}

float SX1262::packetSnr() {
    uint8_t buf[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, buf, 3);
    return float((int8_t)buf[1]) * 0.25;
}

uint16_t SX1262::getDeviceErrors() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_DEVICE_ERRORS_6X, buf, 2);
    return (uint16_t)(buf[0] << 8 | buf[1]);
}

void SX1262::clearDeviceErrors() {
    uint8_t buf[2] = {0x00, 0x00};
    executeOpcode(OP_CLEAR_DEVICE_ERRORS_6X, buf, 2);
}

uint8_t SX1262::getStatus() {
    uint8_t buf[1] = {0};
    executeOpcodeRead(OP_STATUS_6X, buf, 1);
    return buf[0];
}

uint8_t SX1262::getPacketType() {
    uint8_t buf[1] = {0xFF};
    executeOpcodeRead(OP_GET_PACKET_TYPE_6X, buf, 1);
    return buf[0];
}

uint16_t SX1262::getIrqFlags() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    return (uint16_t)(buf[0] << 8 | buf[1]);
}

bool IRAM_ATTR SX1262::getPacketValidity() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, buf, 2);
    return (buf[1] & IRQ_PAYLOAD_CRC_ERROR_MASK_6X) == 0;
}

void SX1262::setFrequency(uint32_t frequency) {
    calibrate_image(frequency);
    standby();
    _frequency = frequency;
    uint32_t freq = (uint32_t)((double)frequency / (double)FREQ_STEP_6X);
    uint8_t buf[4];
    buf[0] = ((freq >> 24) & 0xFF);
    buf[1] = ((freq >> 16) & 0xFF);
    buf[2] = ((freq >> 8) & 0xFF);
    buf[3] = (freq & 0xFF);
    executeOpcode(OP_RF_FREQ_6X, buf, 4);
}

uint32_t SX1262::getFrequency() { return _frequency; }

void SX1262::setTxPower(int level) {
    writeRegister(REG_TX_CLAMP_CONFIG_6X, readRegister(REG_TX_CLAMP_CONFIG_6X) | (0x0F << 1));
    uint8_t pa_buf[4];
    pa_buf[0] = 0x04; pa_buf[1] = 0x07; pa_buf[2] = 0x00; pa_buf[3] = 0x01;
    executeOpcode(OP_PA_CONFIG_6X, pa_buf, 4);
    if (level > 22) level = 22;
    else if (level < -9) level = -9;
    _txp = level;
    writeRegister(REG_OCP_6X, LORA_OCP_TUNED);
    uint8_t tx_buf[2];
    tx_buf[0] = level;
    tx_buf[1] = 0x02;  // PA ramp: 40us
    executeOpcode(OP_TX_PARAMS_6X, tx_buf, 2);
}

int8_t SX1262::getTxPower() { return _txp; }

void SX1262::setSpreadingFactor(int sf) {
    if (sf < 5) sf = 5;
    else if (sf > 12) sf = 12;
    _sf = sf;
    handleLowDataRate();
    setModulationParams(sf, _bw, _cr, _ldro);
}

uint8_t SX1262::getSpreadingFactor() { return _sf; }

uint32_t SX1262::getSignalBandwidth() {
    switch (_bw) {
        case 0x00: return 7800;   case 0x01: return 15600;
        case 0x02: return 31250;  case 0x03: return 62500;
        case 0x04: return 125000; case 0x05: return 250000;
        case 0x06: return 500000; case 0x08: return 10400;
        case 0x09: return 20800;  case 0x0A: return 41700;
    }
    return 0;
}

void SX1262::setSignalBandwidth(uint32_t sbw) {
    if      (sbw <= 7800)   _bw = 0x00;
    else if (sbw <= 10400)  _bw = 0x08;
    else if (sbw <= 15600)  _bw = 0x01;
    else if (sbw <= 20800)  _bw = 0x09;
    else if (sbw <= 31250)  _bw = 0x02;
    else if (sbw <= 41700)  _bw = 0x0A;
    else if (sbw <= 62500)  _bw = 0x03;
    else if (sbw <= 125000) _bw = 0x04;
    else if (sbw <= 250000) _bw = 0x05;
    else                    _bw = 0x06;
    handleLowDataRate();
    setModulationParams(_sf, _bw, _cr, _ldro);
}

void SX1262::setCodingRate4(int denominator) {
    if (denominator < 5) denominator = 5;
    else if (denominator > 8) denominator = 8;
    _cr = denominator - 4;
    setModulationParams(_sf, _bw, _cr, _ldro);
}

uint8_t SX1262::getCodingRate4() { return _cr + 4; }

void SX1262::setPreambleLength(long length) {
    _preambleLength = length;
    setPacketParams(length, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::setInvertIQ(bool invert) {
    _invertIq = invert;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::enableCrc() {
    _crcMode = 1;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::disableCrc() {
    _crcMode = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::setModulationParams(uint8_t sf, uint8_t bw, uint8_t cr, int ldro) {
    // SetModulationParams is only valid in STDBY mode (SX1262 DS Table 11-2).
    // Calling from RX/TX mode is silently rejected by the hardware.
    standby();
    if (!ensureLoRaMode("setModulationParams")) return;

    // Match the RNode SX1262 driver: write the complete LoRa parameter block,
    // including reserved trailing bytes, so modem state is not left stale.
    uint8_t buf[8] = {sf, bw, cr, (uint8_t)ldro, 0x00, 0x00, 0x00, 0x00};
    executeOpcode(OP_MODULATION_PARAMS_6X, buf, 8);
}

void SX1262::setPacketParams(uint32_t preamble, uint8_t headermode, uint8_t length, uint8_t crc) {
    if (!ensureLoRaMode("setPacketParams")) return;

    // Match the RNode SX1262 driver: write the complete packet parameter block.
    uint8_t buf[9];
    buf[0] = (uint8_t)((preamble & 0xFF00) >> 8);
    buf[1] = (uint8_t)(preamble & 0x00FF);
    buf[2] = headermode;
    buf[3] = length;
    buf[4] = crc;
    buf[5] = _invertIq ? 0x01 : 0x00;
    buf[6] = 0x00;
    buf[7] = 0x00;
    buf[8] = 0x00;
    executeOpcode(OP_PACKET_PARAMS_6X, buf, 9);

    // SX1262 errata 15.1: IQ polarity register must be corrected after SetPacketParams.
    // For standard IQ (no inversion), bit 2 of register 0x0736 must be SET.
    // For inverted IQ, bit 2 must be CLEARED. (RadioLib: SX126x.cpp)
    uint8_t iqReg = readRegister(REG_IQ_POLARITY_6X);
    if (_invertIq) {
        iqReg &= ~0x04;
    } else {
        iqReg |= 0x04;
    }
    writeRegister(REG_IQ_POLARITY_6X, iqReg);
}

void SX1262::setSyncWord(uint16_t sw) {
    writeRegister(REG_SYNC_WORD_MSB_6X, 0x14);
    writeRegister(REG_SYNC_WORD_LSB_6X, 0x24);
}

void SX1262::explicitHeaderMode() {
    _implicitHeaderMode = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::implicitHeaderMode() {
    _implicitHeaderMode = 1;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::handleLowDataRate() {
    uint32_t bw = getSignalBandwidth();
    if (_sf == 0 || bw == 0) {
        _ldro = false;
        return;
    }
    float symbolTimeMs = 1000.0f * (float)(1UL << _sf) / (float)bw;
    _ldro = symbolTimeMs > 16.0f;
}

void SX1262::standby() {
    uint8_t byte = _tcxo ? MODE_STDBY_XOSC_6X : MODE_STDBY_RC_6X;
    executeOpcode(OP_STANDBY_6X, &byte, 1);
}

void SX1262::sleep() {
    uint8_t byte = 0x00;
    executeOpcode(OP_SLEEP_6X, &byte, 1);
}

float SX1262::getAirtime(uint16_t written) {
    if (!_radioOnline) return 0;
    uint32_t bw = getSignalBandwidth();
    uint8_t crDen = getCodingRate4();
    if (_sf == 0 || bw == 0 || crDen < 5 || crDen > 8) return 0;

    float symbolRate = (float)bw / (float)(1UL << _sf);
    float symbolTimeMs = 1000.0 / symbolRate;
    float payloadSymbols;
    if (_sf >= 7) {
        payloadSymbols = (8.0 * written + PHY_CRC_LORA_BITS - 4.0 * _sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        payloadSymbols /= 4.0 * (_sf - 2 * (_ldro ? 1 : 0));
        if (payloadSymbols < 0) payloadSymbols = 0;
        payloadSymbols = ceil(payloadSymbols) * crDen;
        payloadSymbols += _preambleLength + 0.25 + 8;
    } else {
        payloadSymbols = (8.0 * written + PHY_CRC_LORA_BITS - 4.0 * _sf + PHY_HEADER_LORA_SYMBOLS);
        payloadSymbols /= 4.0 * _sf;
        if (payloadSymbols < 0) payloadSymbols = 0;
        payloadSymbols = ceil(payloadSymbols) * crDen;
        payloadSymbols += _preambleLength + 2.25 + 8;
    }
    return payloadSymbols * symbolTimeMs;
}

uint32_t SX1262::getBitrate() {
    uint32_t bw = getSignalBandwidth();
    uint8_t crDen = getCodingRate4();
    if (_sf == 0 || bw == 0 || crDen < 5 || crDen > 8) return 0;

    float bitrate = (float)_sf * (4.0f / (float)crDen) * (float)bw / (float)(1UL << _sf);
    if (bitrate < 1.0f) return 1;
    return (uint32_t)bitrate;
}

void IRAM_ATTR SX1262::onDio0Rise() {
    if (_instance) {
        _instance->packetAvailable = true;
        // Don't call handleDio0Rise() from ISR — it does SPI which deadlocks
        // on the shared bus when the display holds the SPI mutex (causes
        // "Interrupt wdt timeout on CPU1" crash). LoRaInterface::loop() polls
        // packetAvailable and reads the radio buffer from main loop context.
    }
}

void SX1262::handleDio0Rise() {
    _packetIndex = 0;
    uint8_t rxbuf[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
    int packetLength = rxbuf[0];
    if (_onReceive) { _onReceive(packetLength); }
}

void SX1262::onReceive(void(*callback)(int)) {
    _onReceive = callback;
    if (callback) {
        pinMode(_irq, INPUT);
        uint8_t buf[8] = {0xFF, 0xFF, 0x00, IRQ_RX_DONE_MASK_6X, 0x00, 0x00, 0x00, 0x00};
        executeOpcode(OP_SET_IRQ_FLAGS_6X, buf, 8);
        enableDio2RfSwitch();
        attachInterrupt(digitalPinToInterrupt(_irq), onDio0Rise, RISING);
    } else {
        detachInterrupt(digitalPinToInterrupt(_irq));
    }
}

uint8_t SX1262::random() { return readRegister(REG_RANDOM_GEN_6X); }
