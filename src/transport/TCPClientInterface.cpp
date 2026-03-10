#include "TCPClientInterface.h"
#include "config/Config.h"

TCPClientInterface::TCPClientInterface(const char* host, uint16_t port, const char* name)
    : RNS::InterfaceImpl(name), _host(host), _port(port)
{
    _IN = true;
    _OUT = true;
    _bitrate = 1000000;
    _HW_MTU = 500;
}

TCPClientInterface::~TCPClientInterface() {
    stop();
}

bool TCPClientInterface::start() {
    _online = true;
    tryConnect();
    return true;
}

void TCPClientInterface::stop() {
    _online = false;
    if (_client.connected()) {
        _client.stop();
        Serial.printf("[TCP] Disconnected from %s:%d\n", _host.c_str(), _port);
    }
}

void TCPClientInterface::tryConnect() {
    _lastAttempt = millis();
    Serial.printf("[TCP] Connecting to %s:%d...\n", _host.c_str(), _port);

    if (_client.connect(_host.c_str(), _port, TCP_CONNECT_TIMEOUT_MS)) {
        // Reset HDLC frame state for new connection
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
        _lastRxTime = millis();

        // Set TCP write timeout to prevent blocking on half-open sockets
        _client.setTimeout(5);  // 5 second write timeout

        Serial.printf("[TCP] Connected to %s:%d\n", _host.c_str(), _port);
    } else {
        Serial.printf("[TCP] Failed to connect to %s:%d\n", _host.c_str(), _port);
    }
}

void TCPClientInterface::loop() {
    if (!_online) return;

    // Auto-reconnect (only if WiFi is connected)
    if (!_client.connected()) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (millis() - _lastAttempt >= TCP_RECONNECT_INTERVAL_MS) {
            tryConnect();
        }
        return;
    }

    // Keepalive: if no RX for 5 minutes, force reconnect (NAT timeout detection)
    if (_lastRxTime > 0 && millis() - _lastRxTime >= TCP_KEEPALIVE_TIMEOUT_MS) {
        Serial.printf("[TCP] No RX for %lus, forcing reconnect to %s:%d\n",
                      (millis() - _lastRxTime) / 1000, _host.c_str(), _port);
        _client.stop();
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
        return;  // Will reconnect on next loop iteration
    }

    // Drain multiple incoming frames per loop (up to 10, time-boxed)
    unsigned long tcpStart = millis();
    for (int i = 0; i < 10 && _client.available() && (millis() - tcpStart < TCP_LOOP_BUDGET_MS); i++) {
        unsigned long rxStart = millis();
        int len = readFrame();
        if (len > 0) {
            _lastRxTime = millis();
            RNS::Bytes data(_rxBuffer, len);
            Serial.printf("[TCP] RX %d bytes from %s:%d (%lums)\n",
                          len, _host.c_str(), _port, millis() - rxStart);
            InterfaceImpl::handle_incoming(data);
        } else {
            break;  // Incomplete frame, wait for more data
        }
    }
}

void TCPClientInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online) {
        Serial.printf("[TCP] TX BLOCKED (offline) %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return;
    }
    if (!_client.connected()) {
        Serial.printf("[TCP] TX BLOCKED (disconnected) %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return;
    }

    sendFrame(data.data(), data.size());
    Serial.printf("[TCP] TX %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
    InterfaceImpl::handle_outgoing(data);
}

// HDLC-like framing: [0x7E] [escaped data] [0x7E]
void TCPClientInterface::sendFrame(const uint8_t* data, size_t len) {
    _client.write(FRAME_START);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == FRAME_START || data[i] == FRAME_ESC) {
            _client.write(FRAME_ESC);
            _client.write(data[i] ^ FRAME_XOR);
        } else {
            _client.write(data[i]);
        }
    }
    _client.write(FRAME_START);
    _client.flush();
}

int TCPClientInterface::readFrame() {
    if (!_client.available()) return 0;

    // Uses persistent member state: _inFrame, _escaped, _rxPos
    // This allows frames split across TCP segments to be reassembled correctly
    int bytesRead = 0;
    constexpr int MAX_BYTES_PER_CALL = 1024;

    while (_client.available() && _rxPos < sizeof(_rxBuffer) && bytesRead < MAX_BYTES_PER_CALL) {
        uint8_t b = _client.read();
        bytesRead++;

        if (b == FRAME_START) {
            if (_inFrame && _rxPos > 0) {
                // End of frame — return length, caller reads from _rxBuffer
                size_t frameLen = _rxPos;
                _inFrame = false;
                _escaped = false;
                _rxPos = 0;
                return frameLen;
            }
            _inFrame = true;
            _rxPos = 0;
            _escaped = false;
            continue;
        }

        if (!_inFrame) continue;

        if (b == FRAME_ESC) {
            _escaped = true;
            continue;
        }

        if (_escaped) {
            _rxBuffer[_rxPos++] = b ^ FRAME_XOR;
            _escaped = false;
        } else {
            _rxBuffer[_rxPos++] = b;
        }
    }

    // Buffer overflow protection
    if (_rxPos >= sizeof(_rxBuffer)) {
        Serial.printf("[TCP] Frame too large (%d bytes), dropping\n", (int)_rxPos);
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
    }

    return 0;  // Incomplete frame — state preserved for next call
}
