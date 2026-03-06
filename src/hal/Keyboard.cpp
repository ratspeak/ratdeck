#include "Keyboard.h"

Keyboard* Keyboard::_instance = nullptr;
int Keyboard::_debugCount = 0;

bool Keyboard::begin() {
    _instance = this;
    _mode = InputMode::Navigation;
    _hasEvent = false;

    // KB interrupt pin
    pinMode(KB_INT, INPUT_PULLUP);

    // Verify I2C communication with keyboard controller
    Wire.beginTransmission(KB_I2C_ADDR);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[KEYBOARD] ESP32-C3 not found at 0x%02X (err=%d)\n", KB_I2C_ADDR, err);
        return false;
    }

    Serial.println("[KEYBOARD] ESP32-C3 keyboard ready");
    return true;
}

uint8_t Keyboard::readKey(uint8_t* modOut) {
    *modOut = 0;
    // Read 2 bytes: byte 1 = key, byte 2 = potential modifier flags
    Wire.requestFrom((uint8_t)KB_I2C_ADDR, (uint8_t)2);
    uint8_t key = 0;
    if (Wire.available()) key = Wire.read();
    if (Wire.available()) *modOut = Wire.read();
    return key;
}

void Keyboard::update() {
    _hasEvent = false;

    uint8_t mod = 0;
    uint8_t key = readKey(&mod);
    if (key == 0 || key == _lastKey) {
        if (key == 0) _lastKey = 0;
        return;
    }
    _lastKey = key;

    // Debug logging for first 50 keypresses to help diagnose key mapping
    if (_debugCount < 50) {
        _debugCount++;
        Serial.printf("[KB] raw: key=0x%02X ('%c') mod=0x%02X\n",
                      key, (key >= 0x20 && key < 0x7F) ? (char)key : '?', mod);
    }

    _event = {};

    // Check for Alt in modifier byte (try common bit positions)
    // BBQ-style keyboards: bit 1=Alt, bit 2=Sym, bit 0=Ctrl
    // Also try bit 3, bit 4 as some firmwares use those
    bool altFromMod = (mod & 0x02) || (mod & 0x08);

    // Track Alt state: if the keyboard sends Alt as a standalone keypress,
    // it might come as a specific byte. Common values:
    // 0x1B = Esc (unlikely to be Alt), but some controllers use high bytes
    // The T-Deck Alt key might send no byte at all when pressed alone.

    if (altFromMod) {
        _event.alt = true;
    }

    // Standard key decoding
    if (key == 0x0D || key == '\n') {
        _event.enter = true;
        _event.character = '\n';
    } else if (key == 0x08 || key == 0x7F) {
        _event.del = true;
        _event.character = 0x08;
    } else if (key == 0x09) {
        _event.tab = true;
    } else if (key == 0x1B) {
        _event.character = 27;  // ESC
    } else if (key == ' ') {
        _event.space = true;
        _event.character = ' ';
    } else if (key >= 0x01 && key <= 0x1A) {
        // Ctrl+A through Ctrl+Z
        _event.ctrl = true;
        _event.character = key + 'a' - 1;
    } else if (key >= 0x20 && key <= 0x7E) {
        _event.character = key;
    }

    _hasEvent = true;
}
