#pragma once

#include <Arduino.h>
#include "config/BoardConfig.h"

class Power {
public:
    // Enable peripheral power (GPIO 10 HIGH) — call first in setup
    static void enablePeripherals();

    void begin();
    void loop();

    // Strong activity = keyboard/touch — wakes from any state
    void activity();
    // Weak activity = trackball — wakes from DIM only, not SCREEN_OFF
    void weakActivity();
    // Manual screen-off; no-op if activity() just woke from SCREEN_OFF this tick
    // so a single keypress can't both wake and re-sleep.
    void forceScreenOff();

    // Battery
    float batteryVoltage() const;
    int batteryPercent() const;
    bool isCharging() const;
    void setBatteryModel(uint8_t model);  // 0=lipo, 1=linear
    void setChargeThreshold(float v);
    void setFullBatteryVoltage(float v);
    void setAdcDividerRatio(float ratio);
    float adcDividerRatio() const { return _adcDividerRatio; }
    // Raw ADC reading on BAT_ADC_PIN, unscaled — for calibration only.
    int batteryRawAdc() const;

    // Display backlight — accepts percentage 1-100
    void setBrightness(uint8_t percent);
    void setDimTimeout(uint16_t seconds) {
        _dimTimeout = seconds * 1000UL;
        if (_offTimeout > 0 && _offTimeout <= _dimTimeout) {
            _offTimeout = _dimTimeout + 10000UL;
        }
    }
    void setOffTimeout(uint16_t seconds) {
        _offTimeout = seconds * 1000UL;
        if (_offTimeout > 0 && _offTimeout <= _dimTimeout) {
            _offTimeout = _dimTimeout + 10000UL;
        }
    }

    // Keyboard backlight — accepts percentage 0-100 (0 = off)
    void setKbBrightness(uint8_t percent, bool apply=false);
    void setKbAutoOn(bool enable) { _kbAutoOn = enable; }
    void setKbAutoOff(bool enable) { _kbAutoOff = enable; }

    enum State { ACTIVE, DIMMED, SCREEN_OFF };
    State state() const { return _state; }
    bool isScreenOn() const { return _state != SCREEN_OFF; }
    bool isDimmed() const { return _state == DIMMED; }

private:
    void setState(State newState);
    uint8_t percentToPWM(uint8_t pct) const;

    State _state = ACTIVE;
    unsigned long _lastActivity = 0;
    unsigned long _dimTimeout = 30000;
    unsigned long _offTimeout = 60000;
    uint8_t _brightnessPct = 100;  // User brightness as 1-100%
    static constexpr uint8_t DIM_PWM = 40;  // ~15% PWM when dimmed
    bool _kbAutoOn = false;
    bool _kbAutoOff = false;
    bool _kbLitBeforeOff = false;
    bool _justWokeFromOff = false;

    // Battery
    uint8_t _batteryModel = 0;
    float _chargeThreshold = 4.18f;
    float _fullBatteryV = 3.9f;
    float _adcDividerRatio = 2.0f;
};
