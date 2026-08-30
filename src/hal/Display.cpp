#include "Display.h"
#include <lvgl.h>
#include "../platform/CoreSync.h"

// Double-buffered 10-line strips in PSRAM for DMA flush
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;
static LGFX_TDeck* s_gfx = nullptr;

static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    // Hold the shared FSPI bus for the whole strip write. In the single-threaded
    // build this is a no-op; under RSDECK_UI_CORE_SPLIT it keeps the flush from
    // interleaving with an SX1262/SD transaction driven by the network core.
    CoreSync::SpiBusGuard busGuard;
    s_gfx->startWrite();
    s_gfx->setAddrWindow(area->x1, area->y1, w, h);
    // Blocking pushPixels prevents SPI bus contention with SX1262 radio on shared FSPI bus
    s_gfx->pushPixels((lgfx::swap565_t*)&color_p->full, w * h);
    s_gfx->endWrite();
    lv_disp_flush_ready(drv);
}

bool Display::begin() {
    _gfx.init();
    _gfx.setRotation(1);  // Landscape: 320x240
    _gfx.setBrightness(0);
    _gfx.fillScreen(TFT_BLACK);

    Serial.printf("[DISPLAY] Initialized: %dx%d (rotation=1, LovyanGFX direct)\n",
                  _gfx.width(), _gfx.height());

    return true;
}

bool Display::beginLVGL() {
    s_gfx = &_gfx;

    lv_init();

    // Allocate double-buffered 20-line strips in PSRAM (halves DMA flush ops per redraw)
    const uint32_t bufSize = 320 * 20;
    s_buf1 = (lv_color_t*)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_buf2 = (lv_color_t*)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    // Fall back to PSRAM if DMA-capable memory not available
    if (!s_buf1) s_buf1 = (lv_color_t*)ps_malloc(bufSize * sizeof(lv_color_t));
    if (!s_buf2) s_buf2 = (lv_color_t*)ps_malloc(bufSize * sizeof(lv_color_t));

    if (!s_buf1 || !s_buf2) {
        Serial.println("[LVGL] FATAL: buffer allocation failed!");
        return false;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf1, s_buf2, bufSize);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 320;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    Serial.println("[LVGL] Display driver registered (320x240, double-buffered 20-line DMA)");
    return true;
}

void Display::setBrightness(uint8_t level) {
    _gfx.setBrightness(level);
}

void Display::sleep() {
    CoreSync::SpiBusGuard busGuard;
    _gfx.sleep();
}

void Display::wakeup() {
    CoreSync::SpiBusGuard busGuard;
    _gfx.wakeup();
}
