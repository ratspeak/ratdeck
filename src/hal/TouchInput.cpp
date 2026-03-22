#include "TouchInput.h"

TouchInput* TouchInput::_instance = nullptr;

bool TouchInput::begin() {
    _instance = this;

    // GT911 INT pin
    pinMode(TOUCH_INT, INPUT);

    // Try GT911 at both possible addresses
    Wire.beginTransmission(TOUCH_I2C_ADDR_1);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Wire.beginTransmission(TOUCH_I2C_ADDR_2);
        err = Wire.endTransmission();
        if (err != 0) {
            Serial.println("[TOUCH] GT911 not found at 0x5D or 0x14");
            return false;
        }
        _i2cAddress = TOUCH_I2C_ADDR_2;
    } else {
        _i2cAddress = TOUCH_I2C_ADDR_1;
    }
    Serial.print("[TOUCH] GT911 found at 0x");
    Serial.println(_i2cAddress, HEX);

    Serial.println("[TOUCH] Touch input registered");
    return true;
}

void TouchInput::update() {
    readGT911();
}

bool TouchInput::readGT911() {
    // Read touch status register (0x814E)
    Wire.beginTransmission(_i2cAddress);
    Wire.write(0x81);
    Wire.write(0x4E);
    if (Wire.endTransmission() != 0) {
        _touched = false;
        return false;
    }

    Wire.requestFrom(_i2cAddress, (uint8_t)1);
    if (!Wire.available()) {
        _touched = false;
        return false;
    }

    uint8_t status = Wire.read();
    uint8_t touchCount = status & 0x0F;
    bool bufferReady = (status & 0x80) != 0;

    if (!bufferReady || touchCount == 0) {
        _touched = false;
        Wire.beginTransmission(_i2cAddress);
        Wire.write(0x81);
        Wire.write(0x4E);
        Wire.write(0x00);
        Wire.endTransmission();
        return false;
    }

    // Read first touch point (0x814F-0x8155)
    Wire.beginTransmission(_i2cAddress);
    Wire.write(0x81);
    Wire.write(0x4F);
    if (Wire.endTransmission() != 0) {
        _touched = false;
        return false;
    }

    Wire.requestFrom(_i2cAddress, (uint8_t)7);
    if (Wire.available() < 7) {
        _touched = false;
        return false;
    }

    uint8_t trackId = Wire.read();
    _x = Wire.read() | (Wire.read() << 8);
    _y = Wire.read() | (Wire.read() << 8);
    Wire.read() | (Wire.read() << 8); // size (unused)

    _touched = true;

    // Clear buffer status
    Wire.beginTransmission(_i2cAddress);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0x00);
    Wire.endTransmission();

    // TODO: remove
    Serial.write("[TOUCH] id "); Serial.print(trackId); Serial.write(" @ ");
    Serial.print(_x); Serial.write(' '); Serial.println(_y);

    return true;
}
