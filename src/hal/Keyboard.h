#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config/BoardConfig.h"

// Input modes
enum class InputMode {
    Navigation,  // Arrow-like movement, hotkeys active
    TextInput    // Character entry, Esc exits to Navigation
};

// Simplified key event for consumers
struct KeyEvent {
    char character;
    bool ctrl;
    bool shift;
    bool fn;
    bool alt;
    bool opt;
    bool enter;
    bool del;
    bool tab;
    bool space;
    // Directional arrows (from trackball)
    bool up;
    bool down;
    bool left;
    bool right;
};

class Keyboard {
public:
    bool begin();
    void update();

    // Mode control
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }

    // State queries
    bool hasEvent() const { return _hasEvent; }
    const KeyEvent& getEvent() const { return _event; }

private:
    uint8_t readKey(uint8_t* modOut);

    InputMode _mode = InputMode::Navigation;
    KeyEvent _event = {};
    bool _hasEvent = false;
    uint8_t _lastKey = 0;
    bool _altHeld = false;           // Software Alt tracking
    unsigned long _altPressTime = 0; // When Alt was detected

    static Keyboard* _instance;
    static int _debugCount;          // Log first N keypresses
};
