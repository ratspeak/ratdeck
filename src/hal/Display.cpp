#include "Display.h"

bool Display::begin() {
    _gfx.init();
    _gfx.setRotation(1);  // Landscape: 320x240
    _gfx.setBrightness(128);
    _gfx.fillScreen(TFT_BLACK);

    Serial.printf("[DISPLAY] Initialized: %dx%d (rotation=1, LovyanGFX direct)\n",
                  _gfx.width(), _gfx.height());

    return true;
}

void Display::setBrightness(uint8_t level) {
    _gfx.setBrightness(level);
}

void Display::sleep() {
    _gfx.sleep();
}

void Display::wakeup() {
    _gfx.wakeup();
}
