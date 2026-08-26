#include "Power.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"


// Forward declarations — display & keyboard instances provided externally
extern Display display;
extern Keyboard keyboard;

// Battery type lookup table for getting voltage vs percent
namespace {

    struct VoltPoint { float v; int pct; };
    static constexpr VoltPoint LIPO_CURVE[] = {
        {3.90f, 100},
        {3.80f, 90},
        {3.72f, 80},
        {3.65f, 70},
        {3.59f, 60},
        {3.53f, 50},
        {3.48f, 40},
        {3.44f, 30},
        {3.40f, 20},
        {3.36f, 15},
        {3.30f, 10},
        {3.15f, 5},
        {3.00f, 0},
    };
    constexpr int LIPO_CURVE_N = sizeof(LIPO_CURVE) / sizeof(LIPO_CURVE[0]);
}






void Power::enablePeripherals() {
    // CRITICAL: GPIO 10 must be HIGH to enable all T-Deck Plus peripherals
    pinMode(BOARD_POWER_PIN, OUTPUT);
    digitalWrite(BOARD_POWER_PIN, HIGH);
    delay(10);  // Allow peripherals to stabilize
}

void Power::begin() {
    _lastActivity = millis();
    _state = ACTIVE;

    // Configure battery ADC
    pinMode(BAT_ADC_PIN, INPUT);
    analogReadResolution(12);

    Serial.println("[POWER] Power manager initialized");
}

namespace {
// Oversample the raw ADC to smooth out transient noise/sag - a single
// analogRead() can catch a momentary dip (e.g. inrush current right at
// boot while the display/radio power up) that doesn't reflect the
// battery's actual resting voltage. 8 samples with a short settling delay
// between each is cheap (<1ms total) and removes most single-sample spikes
// without meaningfully slowing down callers.
constexpr int BATTERY_ADC_SAMPLES = 8;

int readBatteryRawAveraged() {
    long sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        sum += analogRead(BAT_ADC_PIN);
        if (i < BATTERY_ADC_SAMPLES - 1) delayMicroseconds(100);
    }
    return (int)(sum / BATTERY_ADC_SAMPLES);
}
}  // namespace

float Power::batteryVoltage() const {
    // T-Deck Plus: voltage divider on GPIO 4
    int raw = readBatteryRawAveraged();
    // Voltage divider ratio, 3.3V reference, 12-bit ADC. Stock hardware is
    // ~2.0x; aftermarket/extended battery packs can be wired through a
    // different sense-line divider network, so this is user-calibratable
    // via setAdcDividerRatio() rather than hardcoded. Calibrate with a
    // multimeter on the actual battery terminals:
    //   correct_ratio = measured_mv / raw_mv_at_current_ratio * current_ratio
    return (raw / 4095.0f) * 3.3f * _adcDividerRatio;
}

int Power::batteryRawAdc() const {
    return readBatteryRawAveraged();
}

int Power::batteryPercent() const {
    float v = batteryVoltage();

    // Charging: voltage exceeds the discharge curve's top (about 3.9V = full).
    // A discharging cell never gets there, so treat this as full.
    if (isCharging())
        return 100;

    // Compensate for load-induced voltage drop when running on battery.
    // Calculates an offset and adds it to real voltage for to be able using default LiPo lookup table
    v += (3.9f - _fullBatteryV);

    // Clamp to valid curve range.
    v = constrain(v, 3.0f, 4.2f);

    if (_batteryModel == 1) {
        // Linear: distribution across 3.0–4.2V.
        return constrain((int)((v - 3.0f) / 1.2f * 100.0f), 0, 100);
    }

    // LiPo: interpolate between nearest table entries. The table's top
    // entry (3.90V=100%) is well below the LiPo absolute max (~4.2V), so
    // any v above 3.90V - not otherwise caught by isCharging() - would
    // extrapolate past the top entry pair without a clamp (e.g. 119% at
    // 4.09V observed on real hardware after raising the charge-detection
    // threshold). Clamp every return path to a valid 0-100 percent.
    for (int i = 0; i < LIPO_CURVE_N - 1; i++) {
        if (v >= LIPO_CURVE[i + 1].v) {
            float t = (v - LIPO_CURVE[i + 1].v) / (LIPO_CURVE[i].v - LIPO_CURVE[i + 1].v);
            int pct = (int)(LIPO_CURVE[i + 1].pct + t * (LIPO_CURVE[i].pct - LIPO_CURVE[i + 1].pct));
            return constrain(pct, 0, 100);
        }
    }
    return 0;
}

void Power::setBatteryModel(uint8_t model) {
    _batteryModel = model;
}

bool Power::isCharging() const {
    return batteryVoltage() >= _chargeThreshold;
}

void Power::setChargeThreshold(float v)   {
    _chargeThreshold = v;
}

void Power::setFullBatteryVoltage(float v) {
    _fullBatteryV = v;
}

void Power::setAdcDividerRatio(float ratio) {
    _adcDividerRatio = ratio;
}


uint8_t Power::percentToPWM(uint8_t pct) const {
    if (pct == 0) return 0;
    if (pct >= 100) return 255;
    // Map 1-100 to ~6-255 (minimum visible PWM ~6)
    return (uint8_t)(6 + (uint16_t)(pct - 1) * 249 / 99);
}

void Power::activity() {
    _lastActivity = millis();
    if (_state == SCREEN_OFF) {
        _justWokeFromOff = true;
    }
    if (_state != ACTIVE) {
        setState(ACTIVE);
    }
}

void Power::forceScreenOff() {
    if (_justWokeFromOff) {
        _justWokeFromOff = false;
        return;
    }
    setState(SCREEN_OFF);
}

void Power::weakActivity() {
    _lastActivity = millis();
    // Trackball wakes from DIM but not from SCREEN_OFF
    if (_state == DIMMED) {
        setState(ACTIVE);
    }
}

void Power::setBrightness(uint8_t percent) {
    _brightnessPct = constrain(percent, 1, 100);
    if (_state == ACTIVE) {
        display.setBrightness(percentToPWM(_brightnessPct));
    }
}

void Power::setKbBrightness(uint8_t percent, bool apply) {
    percent = constrain(percent, 0, 100);
    keyboard.setBacklightBrightness(percent);
    if (percent == 0) {
        keyboard.backlightOff();
    } else if (apply) { // Show the new brightness
        keyboard.backlightOn();
    }
}

void Power::loop() {
    unsigned long elapsed = millis() - _lastActivity;

    switch (_state) {
        case ACTIVE:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            } else if (_dimTimeout > 0 && elapsed >= _dimTimeout) {
                setState(DIMMED);
            }
            break;

        case DIMMED:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            }
            break;

        case SCREEN_OFF:
            break;
    }

    _justWokeFromOff = false;
}

void Power::setState(State newState) {
    if (newState == _state) return;
    const char* names[] = {"ACTIVE", "DIMMED", "SCREEN_OFF"};
    Serial.printf("[POWER] %s -> %s\n", names[_state], names[newState]);
    State oldState = _state;
    _state = newState;

    switch (_state) {
        case ACTIVE:
            if (oldState == SCREEN_OFF) {
                // Pre-load correct brightness into LovyanGFX state before wakeup.
                // wakeup() sends SLPOUT then restores LGFX's internal _brightness
                // to the LEDC — with this ordering, it restores the correct value
                // instead of a stale 0, eliminating the rapid 0→0→correct triple-
                // write that can cause missed LEDC duty updates on ESP32-S3.
                display.setBrightness(percentToPWM(_brightnessPct));
                display.wakeup();
            } else {
                display.setBrightness(percentToPWM(_brightnessPct));
            }
            // On wake, relight only what screen-off forced dark (or per auto-on) —
            // never force-enable for users who keep the kb light off.
            if (_kbAutoOn || (oldState == SCREEN_OFF && _kbLitBeforeOff)) {
                keyboard.backlightOn();
            }
            break;
        case DIMMED:
            display.setBrightness(DIM_PWM);
            if (_kbAutoOff) {
                keyboard.backlightOff();
            }
            break;
        case SCREEN_OFF:
            // LovyanGFX sleep() sets brightness to 0 internally — no
            // need to call setBrightness(0) beforehand.
            display.sleep();
            // Kb backlight always follows screen-off — the timeout exists to save battery.
            _kbLitBeforeOff = keyboard.backlightIsLit();
            keyboard.backlightOff();
            break;
    }
}
