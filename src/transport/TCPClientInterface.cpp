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

    // Drain multiple incoming frames per loop (up to 10)
    for (int i = 0; i < 10 && _client.available(); i++) {
        unsigned long rxStart = millis();
        int len = readFrame(_rxBuffer, sizeof(_rxBuffer));
        if (len > 0) {
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

int TCPClientInterface::readFrame(uint8_t* buffer, size_t maxLen) {
    if (!_client.available()) return 0;

    bool inFrame = false;
    bool escaped = false;
    size_t pos = 0;
    int bytesRead = 0;
    constexpr int MAX_BYTES_PER_CALL = 512;

    while (_client.available() && pos < maxLen && bytesRead < MAX_BYTES_PER_CALL) {
        uint8_t b = _client.read();
        bytesRead++;

        if (b == FRAME_START) {
            if (inFrame && pos > 0) {
                return pos;  // End of frame
            }
            inFrame = true;
            pos = 0;
            continue;
        }

        if (!inFrame) continue;

        if (b == FRAME_ESC) {
            escaped = true;
            continue;
        }

        if (escaped) {
            buffer[pos++] = b ^ FRAME_XOR;
            escaped = false;
        } else {
            buffer[pos++] = b;
        }
    }

    return 0;  // Incomplete frame
}
