#include "maps/TileCache.h"
#include "maps/TileStore.h"

#include <esp_heap_caps.h>
#include <string.h>

// =============================================================================
// TileCache.cpp — see TileCache.h for the design rationale.
// =============================================================================

namespace {

constexpr uint32_t ABSOLUTE_MAX_PUMP_MS = 200;  // warn if a single pump() misses budget badly

// Per-pump wall-clock budget (microseconds). Loop() runs at ~60 Hz; to leave
// headroom for LoRa RX FIFO drain + LVGL + telemetry, we want pump() back
// inside ~20-25 ms. PUMP_BUDGET_US=18000 keeps us ~7 ms under one full loop
// tick on the typical 16 ms LVGL tick, and 7 ms under the worst-case 25 ms
// budget for LoRa. The time-budget check inside pump() is a SAFETY NET —
// measured single-pump cost at TILE_CHUNK_BYTES=1024 varies from ~5 ms (most
// iters, just SD-read + tiny inflate) to ~70 ms (one iter where pngle's
// 32 KB lz_buf fills and the per-pixel callback fires for ~10 K pixels).
// The budget check fires only on multi-iter pumps that exceed 18 ms wall-clock.
constexpr uint32_t PUMP_BUDGET_US = 18000;

// ---- DEBUG: sub-step timing for chunk-latency investigation ----
// Set to 1 to print one diagnostic line per pump() call that actually does
// decode work (SD-read + pngle_feed). Used to identify whether the 67-71 ms
// per-pump latency on real hardware is dominated by SD read time or pngle
// (deflate-inflate + per-pixel callback) time. Set to 0 to silence for
// production. RC1 fix: kept at 0 — the per-iter [TILE-ITER] line plus the
// per-pump [TILE-SUB] summary is too noisy on serial during live map panning
// and was hiding the real diagnostic signal. Re-enable for a targeted chunk-
// latency investigation only.
constexpr int TILE_DEBUG_SUBSTEP_TIMING = 0;

}  // namespace

// =============================================================================
// Static helpers (need access to private Slot, so they're class statics)
// =============================================================================

int TileCache::findLruEvictableSlot(TileCache::Slot slots[], int n) {
    // Eviction order, strictest-first:
    //   1. FREE            — costs nothing.
    //   2. MISSING (LRU)   — no pixels anyone can be looking at.
    //   3. READY   (LRU)   — real data, but nothing on screen references it.
    // NEVER evict:
    //   * LOADING          — a decode is in flight into that buffer.
    //   * pinCount > 0     — an lv_img on screen is still blitting that
    //                        exact PSRAM buffer. Recycling it here would
    //                        memset + overwrite pixels under LVGL, which is
    //                        what produced wrong geography / torn tiles /
    //                        "a squished z0 tile inside a z1 grid cell".
    int pickedMissing = -1;
    uint32_t oldestMissing = 0xFFFFFFFFu;
    int pickedReady = -1;
    uint32_t oldestReady = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::LOADING) continue;
        if (slots[i].pinCount > 0) continue;
        if (slots[i].state == TileCache::SlotState::FREE) return i;
        if (slots[i].state == TileCache::SlotState::MISSING) {
            if (slots[i].lastTouchMs < oldestMissing) {
                oldestMissing = slots[i].lastTouchMs;
                pickedMissing = i;
            }
        } else {  // READY
            if (slots[i].lastTouchMs < oldestReady) {
                oldestReady = slots[i].lastTouchMs;
                pickedReady = i;
            }
        }
    }
    if (pickedMissing >= 0) return pickedMissing;
    return pickedReady;  // -1 if every slot is LOADING or pinned
}

int TileCache::findLoadingSlotIdx(TileCache::Slot slots[], int n) {
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::LOADING) return i;
    }
    return -1;
}

int TileCache::findFreeSlotIdx(TileCache::Slot slots[], int n) {
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::FREE) return i;
    }
    return -1;
}

bool TileCache::keyEq(const TileCache::TileKey& a, const TileCache::TileKey& b) {
    return a.z == b.z && a.x == b.x && a.y == b.y && strcmp(a.style, b.style) == 0;
}

// =============================================================================
// Lifecycle
// =============================================================================

void TileCache::begin(SDStore* sd) {
    _sd = sd;
    // Pool is allocated lazily on first pump() so that we don't steal 1.5 MB
    // of PSRAM at boot if the map screen is never opened.
}

void TileCache::ensurePool() {
    if (_poolInited) return;
    TileCache::logPsramBudget("[TILE] before-pool");
    for (int i = 0; i < SLOT_COUNT; ++i) {
        _slots[i].pxbuf = (uint16_t*)heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM);
        if (!_slots[i].pxbuf) {
            Serial.printf("[TILE] FATAL: pool alloc failed at slot %d (%u bytes)\n", i, (unsigned)SLOT_BYTES);
            // Continue; that slot just won't be usable. We'll check on use.
            continue;
        }
        memset(_slots[i].pxbuf, 0, SLOT_BYTES);
        // Build a permanent lv_img_dsc_t pointing at the slot buffer. Header
        // values are filled when the slot becomes READY.
        _slots[i].dsc.header.always_zero = 0;
        _slots[i].dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
        _slots[i].dsc.header.w            = TILE_PX;
        _slots[i].dsc.header.h            = TILE_PX;
        _slots[i].dsc.data_size           = SLOT_BYTES;
        _slots[i].dsc.data                = (const uint8_t*)_slots[i].pxbuf;
    }
    _poolInited = true;
    TileCache::logPsramBudget("[TILE] after-pool");
}

void TileCache::logPsramBudget(const char* tag) {
    Serial.printf("%s free_psram=%u largest_psram_block=%u\n",
                  tag,
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// =============================================================================
// Negative cache (small, linear, evict-oldest-on-insert)
// =============================================================================

bool TileCache::isNegCached(const TileKey& k) {
    for (int i = 0; i < NEG_CACHE_SIZE; ++i) {
        if (_negCache[i].valid && keyEq(_negCache[i].key, k)) return true;
    }
    return false;
}

void TileCache::addNegCache(const TileKey& k) {
    int oldest = 0;
    uint32_t oldestMs = 0xFFFFFFFFu;
    for (int i = 0; i < NEG_CACHE_SIZE; ++i) {
        if (!_negCache[i].valid) {
            _negCache[i].valid = true;
            _negCache[i].key = k;
            _negCache[i].touchMs = millis();
            return;
        }
        if (_negCache[i].touchMs < oldestMs) {
            oldestMs = _negCache[i].touchMs;
            oldest = i;
        }
    }
    _negCache[oldest].valid = true;
    _negCache[oldest].key = k;
    _negCache[oldest].touchMs = millis();
}

// =============================================================================
// Request queue (small ring, dedup-on-push)
// =============================================================================

bool TileCache::isQueuedOrCached(const TileKey& k) {
    // In queue?
    for (int i = 0; i < _reqCount; ++i) {
        int idx = (_reqHead + i) % REQ_QUEUE_SIZE;
        if (_reqQ[idx].valid && keyEq(_reqQ[idx].key, k)) return true;
    }
    // Cached?
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY && keyEq(_slots[i].key, k)) return true;
    }
    return false;
}

bool TileCache::enqueueRequest(const TileKey& k, Priority p) {
    if (_reqCount >= REQ_QUEUE_SIZE) return false;
    int idx = _reqTail;
    _reqQ[idx].valid = true;
    _reqQ[idx].key = k;
    _reqQ[idx].prio = p;
    _reqQ[idx].enqMs = millis();
    _reqTail = (_reqTail + 1) % REQ_QUEUE_SIZE;
    ++_reqCount;
    return true;
}

bool TileCache::requestTile(const char* style, int z, int x, int y, Priority prio) {
    if (!_sd || !style) return false;
    TileKey k;
    strncpy(k.style, style, STYLE_MAX - 1);
    k.style[STYLE_MAX - 1] = '\0';
    k.z = z; k.x = x; k.y = y;
    if (isNegCached(k)) return false;
    if (isQueuedOrCached(k)) return true;
    return enqueueRequest(k, prio);
}

// =============================================================================
// Slot lookup
// =============================================================================

TileCache::Slot* TileCache::findReadySlot(const char* style, int z, int x, int y) {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY &&
            _slots[i].key.z == z && _slots[i].key.x == x && _slots[i].key.y == y &&
            strcmp(_slots[i].key.style, style) == 0) {
            _slots[i].lastTouchMs = millis();
            return &_slots[i];
        }
    }
    return nullptr;
}

TileCache::Slot* TileCache::findSlotByKey(const char* style, int z, int x, int y) {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if ((_slots[i].state == SlotState::READY ||
             _slots[i].state == SlotState::LOADING) &&
            _slots[i].key.z == z && _slots[i].key.x == x && _slots[i].key.y == y &&
            strcmp(_slots[i].key.style, style) == 0) {
            return &_slots[i];
        }
    }
    return nullptr;
}

TileCache::Slot* TileCache::pickFreeSlot() {
    int idx = findFreeSlotIdx(_slots, SLOT_COUNT);
    if (idx < 0) {
        idx = findLruEvictableSlot(_slots, SLOT_COUNT);
        // Every slot is either LOADING or pinned on screen. Return nullptr so
        // pump() leaves the request queued instead of yanking a buffer out
        // from under a visible lv_img.
        if (idx < 0) return nullptr;
        freeSlot(_slots[idx]);
    }
    return &_slots[idx];
}

TileCache::Slot* TileCache::pickLoadingSlot() {
    int idx = findLoadingSlotIdx(_slots, SLOT_COUNT);
    return idx < 0 ? nullptr : &_slots[idx];
}

// =============================================================================
// Decode driver
// =============================================================================

bool TileCache::startDecode(Slot& s, const char* style, int z, int x, int y) {
    if (!s.pxbuf) return false;
    // New content in this buffer → new generation. Bump BEFORE any pixel is
    // written so a UI consumer that re-reads the slot mid-decode already sees
    // the new identity and re-binds instead of trusting its cached dsc.
    ++s.generation;
    // The slot came from FREE (pickFreeSlot), so any pin belonged to the
    // previous tenant. Visible keys are re-pinned by the map screen on its
    // next rebuild.
    s.pinCount = 0;
    s.key.z = z; s.key.x = x; s.key.y = y;
    strncpy(s.key.style, style, STYLE_MAX - 1);
    s.key.style[STYLE_MAX - 1] = '\0';
    s.file = TileStore::openTile(*_sd, style, z, x, y);
    if (!s.file) {
        addNegCache(s.key);
        s.state = SlotState::MISSING;
        return false;
    }
    s.fileSize = s.file.size();
    s.bytesFed = 0;
    s.ioBufLen = 0;
    s.chunkCount = 0;
    s.pixelsWritten = 0;
    s.pxW = 0;
    s.pxH = 0;
    // Default to 1:1 source; onPngInit overwrites this with 2 for 512×512
    // sources or 0 for a non-standard-dims reject.
    s.scale = 1;
    memset(s.first8, 0, sizeof(s.first8));
    // Clear the slot buffer to black so partially-decoded tiles look sensible
    // (and so any out-of-range pixels render as void).
    memset(s.pxbuf, 0, SLOT_BYTES);

    s.pngle = pngle_new();
    if (!s.pngle) {
        Serial.println("[TILE] pngle_new() failed");
        s.file.close();
        s.state = SlotState::MISSING;
        return false;
    }
    pngle_set_user_data(s.pngle, &s);
    pngle_set_init_callback(s.pngle, &TileCache::onPngInit);
    pngle_set_draw_callback(s.pngle, &TileCache::onPngDraw);
    pngle_set_done_callback(s.pngle, &TileCache::onPngDone);
    s.state = SlotState::LOADING;
    s.lastTouchMs = millis();
    return true;
}

void TileCache::closeDecodeStatic(Slot& s) {
    if (s.file) s.file.close();
    if (s.pngle) {
        pngle_destroy(s.pngle);
        s.pngle = nullptr;
    }
}

void TileCache::freeSlot(Slot& s) {
    closeDecode(s);
    s.state = SlotState::FREE;
    s.key = {};
    s.bytesFed = 0;
    s.fileSize = 0;
    s.ioBufLen = 0;
    s.pxW = 0;
    s.pxH = 0;
    s.pixelsWritten = 0;
    // Reset the PNG → buffer downsample ratio so a recycled slot starts as
    // a vanilla 1:1 source. onPngInit on the next decode overwrites this
    // with 2 for a 512×512 source or 0 for a non-standard-dims reject.
    s.scale = 1;
    // Content identity is gone — anything holding this dsc must re-bind.
    ++s.generation;
    s.pinCount = 0;
}

void TileCache::evict(const char* style, int z, int x, int y) {
    if (!style) return;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY &&
            _slots[i].key.z == z && _slots[i].key.x == x && _slots[i].key.y == y &&
            strcmp(_slots[i].key.style, style) == 0) {
            freeSlot(_slots[i]);
            return;
        }
    }
}

// =============================================================================
// PNGLE callbacks (static, route via user_data)
// =============================================================================

void TileCache::onPngInit(pngle_t* p, uint32_t w, uint32_t h) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s) return;
    s->pxW = (int32_t)w;
    s->pxH = (int32_t)h;

    // Determine the PNG → buffer downsample ratio. This mapset ships 512×512
    // PNGs on SD but the LVGL buffer (and the placeholder/image widgets on
    // screen) is fixed at TILE_PX×TILE_PX. The pre-fix code assumed 256×256
    // and stuffed the top-left quadrant into the buffer while telling LVGL
    // the image was 512×512 — that mismatch is the root cause of the
    // "stacked N Americas, squished, half-black" render. We now handle two
    // valid source sizes via s->scale:
    //   scale = 1: 256×256 source → 1:1 write
    //   scale = 2: 512×512 source → 2:1 nearest-neighbor downsample
    // Anything else is rejected (scale = 0) rather than published with wrong
    // dims — see onPngDraw bail / onPngDone free path below.
    if (w == 512u && h == 512u) {
        s->scale = 2;
        // One-line log so serial confirms RC1: a 512→256 downsample fired.
        Serial.printf("[TILE] 512x512 source, downsampling 2:1 to %dx%d\n",
                      TILE_PX, TILE_PX);
    } else if (w == (uint32_t)TILE_PX && h == (uint32_t)TILE_PX) {
        s->scale = 1;
    } else {
        // Non-standard dimensions — reject rather than publish wrong dims.
        // The old code logged a warning and then stored 512×512 into a 256×256
        // buffer; we now bail cleanly so the EOF guard neg-caches this tile
        // (no retry storm on every pan) and the slot stays FREE for another
        // request. onPngDraw sees scale==0 and skips; onPngDone frees the slot.
        Serial.printf("[TILE] WARNING: non-standard tile %ux%u (expected %dx%d or 512x512), rejecting\n",
                      (unsigned)w, (unsigned)h, TILE_PX, TILE_PX);
        s->scale = 0;
    }

    // ALWAYS describe the BUFFER, not the source PNG. The slot buffer is
    // TILE_PX×TILE_PX RGB565 regardless of source dimensions, and LVGL must
    // blit at the buffer's stride — if we publish 512×512 here while writing
    // only the top-left quadrant at stride TILE_PX, LVGL reads 512 rows of
    // 512-px data from a 256-row PSRAM buffer and shows whatever follows in
    // PSRAM as the "second half" of the tile (PSRAM neighbor bleed — that's
    // the "stacked Americas / puzzle tiles" symptom).
    s->dsc.header.always_zero = 0;
    s->dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    s->dsc.header.w           = (uint32_t)TILE_PX;
    s->dsc.header.h           = (uint32_t)TILE_PX;
    s->dsc.data_size          = (uint32_t)SLOT_BYTES;
}

void TileCache::onPngDraw(pngle_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s || !s->pxbuf) return;
    // scale==0 marks a non-standard dims reject from onPngInit — skip every
    // pixel so pixelsWritten stays 0, the EOF guard sees 0 < expectedPx and
    // neg-caches / frees the slot.
    if (s->scale == 0) return;

    // LV_COLOR_16_SWAP is applied at compile time by lv_color_make, so just
    // pack RGB565 and write to the buffer as if it were a uint16_t array.
    // On little-endian ESP32 + LV_COLOR_16_SWAP=1, the lv_color_make result
    // already encodes the swap inside the .full uint16_t.
    const uint16_t color = lv_color_make(rgba[0], rgba[1], rgba[2]).full;

    if (s->scale == 2) {
        // 512×512 source → 2:1 nearest-neighbor downsample into the
        // TILE_PX×TILE_PX buffer. Keep only pixels where both x and y are
        // even (sample 1 of every 2×2 block). w/h > 1 only for interlaced
        // PNGs; for non-interlaced (the common case) it's 1×1 and the inner
        // loop is a single iteration.
        //
        // Source coords are in [0, 512). Destination coords are in [0, 256).
        // We still clip the destination defensively even though the math
        // shouldn't trip — guards against any future source size where
        // pxW/2 > TILE_PX.
        for (uint32_t yy = y; yy < y + h; ++yy) {
            if (yy >= (uint32_t)TILE_PX * 2u) break;
            if ((yy & 1u) != 0u) continue;   // skip odd rows
            const uint32_t dy = yy >> 1;
            for (uint32_t xx = x; xx < x + w; ++xx) {
                if (xx >= (uint32_t)TILE_PX * 2u) break;
                if ((xx & 1u) != 0u) continue;  // skip odd cols
                const uint32_t dx = xx >> 1;
                if (dx >= (uint32_t)TILE_PX) break;
                if (dy >= (uint32_t)TILE_PX) break;
                s->pxbuf[dy * TILE_PX + dx] = color;
                if (s->pixelsWritten < 8) {
                    s->first8[s->pixelsWritten] = color;
                }
                ++s->pixelsWritten;
            }
        }
    } else {
        // scale == 1: standard 256×256 source, 1:1 write.
        for (uint32_t yy = y; yy < y + h; ++yy) {
            if (yy >= (uint32_t)TILE_PX) break;
            for (uint32_t xx = x; xx < x + w; ++xx) {
                if (xx >= (uint32_t)TILE_PX) break;
                s->pxbuf[yy * TILE_PX + xx] = color;
                if (s->pixelsWritten < 8) {
                    s->first8[s->pixelsWritten] = color;
                }
                ++s->pixelsWritten;
            }
        }
    }
}

void TileCache::onPngDone(pngle_t* p) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s) return;

    // scale==0 is a non-standard dims reject from onPngInit. Don't publish a
    // tile with garbage content. We can't call addNegCache/freeSlot here
    // directly because onPngDone is a static callback (no `this` access to
    // the instance's _negCache / slot pool). Instead, mark the slot MISSING
    // and release the pngle/file resources; pump()'s post-loop handler sees
    // state==MISSING && scale==0 and does the neg-cache + free.
    if (s->scale == 0) {
        Serial.printf("[TILE] non-standard dims, rejecting z=%ld x=%ld y=%ld\n",
                      (long)s->key.z, (long)s->key.x, (long)s->key.y);
        closeDecodeStatic(*s);
        s->state = SlotState::MISSING;
        return;
    }

    // The dsc was set in onPngInit to describe the BUFFER (TILE_PX×TILE_PX,
    // SLOT_BYTES). Do NOT republish w/h/data_size from pxW/pxH here — for
    // 512×512 sources that would tell LVGL the image is 512×512 while the
    // buffer is only 256×256 RGB565, causing the LVGL stride mismatch that
    // produced stacked N Americas / squished / puzzle-tile / half-black
    // renders (and the PSRAM-neighbor bleed that made the "second half" of
    // a 512×512 blit show random memory contents).
    s->dsc.header.always_zero = 0;
    s->dsc.header.w           = (uint32_t)TILE_PX;
    s->dsc.header.h           = (uint32_t)TILE_PX;
    s->dsc.data_size          = (uint32_t)SLOT_BYTES;

    closeDecodeStatic(*s);
    s->state = SlotState::READY;
    s->lastTouchMs = millis();
}

// =============================================================================
// pump() — called once per main-loop iteration
// =============================================================================

void TileCache::pump() {
    if (!_sd) return;
    ensurePool();
    if (!_poolInited) return;  // pool alloc failed; nothing we can do

    unsigned long pumpStart = millis();

    // 1. Continue an in-progress decode if any.
    if (Slot* s = pickLoadingSlot()) {
        // Shared IO buffer in BSS (not on the stack — 2 KB would still fit but
        // we keep the 2x multiplier so a partial deflate-block carry-over
        // always has room). Only one slot is LOADING at a time, so a single
        // buffer is safe. Leftover bytes from pngle_feed are tracked per-slot
        // via s->ioBufLen.
        static uint8_t buf[TILE_CHUNK_BYTES * 2];

        // Wall-clock budget for this pump. We measure elapsed micros() and
        // break out of the inner loop as soon as we exceed it. This is a
        // SAFETY NET — with TILE_CHUNK_BYTES=1024 a single read+feed iteration
        // naturally finishes in ~17 ms, so we only bail early on outlier
        // chunks (very dense IDAT, slow SPI bus contention, etc).
        unsigned long budgetStart = micros();
        // ---- DEBUG: sub-step timing accumulators ----
        unsigned long totalSdUs = 0;
        unsigned long totalFeedUs = 0;
        unsigned long totalReadBytes = 0;
        unsigned long totalFeedBytes = 0;
        int          iterations = 0;
        // ----------------------------------------------

        // Inner loop: do (read up to TILE_CHUNK_BYTES) + (pngle_feed over
        // accumulated buf), check wall-clock, repeat until budget exhausted
        // or file finished. The single read per iteration caps each pngle_feed
        // call to roughly TILE_CHUNK_BYTES+TILE_CHUNK_BYTES bytes of input
        // (carry-over + new) which bounds the inflate cost per iteration.
        while (s->state == SlotState::LOADING &&
               s->bytesFed < s->fileSize &&
               (micros() - budgetStart) < PUMP_BUDGET_US) {

            size_t room = sizeof(buf) - s->ioBufLen;
            if (room > TILE_CHUNK_BYTES) room = TILE_CHUNK_BYTES;
            size_t remaining = s->fileSize - s->bytesFed;
            size_t want = (room < remaining) ? room : remaining;

            size_t got = 0;
            // ---- DEBUG: sub-step timing ----
            unsigned long tSdStart = 0, tSdEnd = 0, tFeedStart = 0, tFeedEnd = 0;
            // -------------------------------
            if (want > 0) {
                tSdStart = micros();
                got = s->file.read(buf + s->ioBufLen, want);
                tSdEnd = micros();
                s->bytesFed += got;
            }
            size_t totalLen = s->ioBufLen + got;
            int fed = 0;
            if (totalLen > 0) {
                tFeedStart = micros();
                fed = pngle_feed(s->pngle, buf, totalLen);
                tFeedEnd = micros();
                if (fed < 0) {
                    // Decode error — release the slot, mark MISSING.
                    Serial.printf("[TILE] decode err z=%ld x=%ld y=%ld: %s\n",
                                  (long)s->key.z, (long)s->key.x, (long)s->key.y,
                                  pngle_error(s->pngle));
                    addNegCache(s->key);
                    freeSlot(*s);
                    break;
                }
                size_t leftover = (size_t)totalLen - (size_t)fed;
                if (leftover > 0) {
                    memmove(buf, buf + fed, leftover);
                }
                s->ioBufLen = leftover;
            }
            // ---- DEBUG: per-iteration timing accumulation ----
            totalSdUs    += (tSdEnd - tSdStart);
            totalFeedUs  += (tFeedEnd - tFeedStart);
            totalReadBytes += got;
            totalFeedBytes += (unsigned long)fed;
            ++iterations;
            if (TILE_DEBUG_SUBSTEP_TIMING) {
                Serial.printf("[TILE-ITER] i=%d sd=%luus feed=%luus got=%u fed=%d elapsed=%luus\n",
                              iterations,
                              (unsigned long)(tSdEnd - tSdStart),
                              (unsigned long)(tFeedEnd - tFeedStart),
                              (unsigned)got,
                              fed,
                              (unsigned long)(micros() - budgetStart));
            }
            // --------------------------------------------------
            // If pngle finished (done callback marked slot READY), bail out
            // of the inner loop immediately — no point burning budget.
            if (s->state != SlotState::LOADING) break;
        }
        // EOF check: file exhausted, no leftover, pngle still never fired its
        // DONE callback. The old code marked the slot READY anyway, which
        // published a half-filled buffer to the UI — that is the "top half is
        // the world, bottom half is black" render. Only accept the tile if
        // virtually every pixel actually landed (a few missing pixels can
        // come from pngle's interlace/rounding edge cases); otherwise treat
        // it as what it is — a truncated/corrupt decode — and neg-cache it so
        // we don't retry it on every pan.
        bool slotFreed = false;
        // Non-standard dims reject from onPngDone — the static callback
        // couldn't addNegCache/freeSlot itself, so it just set state=MISSING
        // and closed the pngle. Handle the cleanup here (post inner-loop)
        // where we have instance access.
        if (s->state == SlotState::MISSING && s->scale == 0) {
            Serial.printf("[TILE] neg-caching rejected tile z=%ld x=%ld y=%ld\n",
                          (long)s->key.z, (long)s->key.x, (long)s->key.y);
            addNegCache(s->key);
            freeSlot(*s);
            slotFreed = true;
        }
        if (s->state == SlotState::LOADING &&
            s->file.available() == 0 && s->bytesFed >= s->fileSize &&
            s->ioBufLen == 0) {
            // Expected pixel count is what onPngDraw could actually have
            // WRITTEN TO THE BUFFER, not what the source PNG contained.
            // For a 512×512 source with scale==2, onPngDraw sampled 1 of
            // every 2×2 block, so it landed (512/2)*(512/2) = TILE_PX²
            // pixels in the buffer — same as a standard 256×256 source.
            // For a non-standard pxW/pxH (with scale==0 → onPngDraw bails)
            // we still want expectedPx > 0 so the 0-pixel decode triggers
            // the truncated/discard branch; effScale=1 makes the formula
            // "pxW * pxH" which still rejects scale==0 (pixelsWritten=0 vs
            // any positive expectedPx). For scale==1 with an unusual small
            // IHDR we clip pxW/pxH to TILE_PX (writes past that are dropped).
            const uint32_t effScale = (s->scale >= 2) ? s->scale : 1u;
            const uint32_t expW = (s->pxW > 0)
                ? ((uint32_t)((uint32_t)s->pxW / effScale) > (uint32_t)TILE_PX
                    ? (uint32_t)TILE_PX
                    : (uint32_t)((uint32_t)s->pxW / effScale))
                : (uint32_t)TILE_PX;
            const uint32_t expH = (s->pxH > 0)
                ? ((uint32_t)((uint32_t)s->pxH / effScale) > (uint32_t)TILE_PX
                    ? (uint32_t)TILE_PX
                    : (uint32_t)((uint32_t)s->pxH / effScale))
                : (uint32_t)TILE_PX;
            const uint32_t expectedPx = expW * expH;
            const uint32_t slackPx    = 256;  // one row of tolerance
            if (s->pixelsWritten + slackPx >= expectedPx) {
                Serial.printf("[TILE] warn: tile exhausted without DONE, accepting (px=%u/%u) z=%ld x=%ld y=%ld\n",
                              (unsigned)s->pixelsWritten, (unsigned)expectedPx,
                              (long)s->key.z, (long)s->key.x, (long)s->key.y);
                closeDecodeStatic(*s);
                s->state = SlotState::READY;
                s->lastTouchMs = millis();
            } else {
                Serial.printf("[TILE] truncated tile, discarding (px=%u/%u) z=%ld x=%ld y=%ld\n",
                              (unsigned)s->pixelsWritten, (unsigned)expectedPx,
                              (long)s->key.z, (long)s->key.x, (long)s->key.y);
                addNegCache(s->key);
                freeSlot(*s);
                slotFreed = true;
            }
        }
        if (!slotFreed) {
            ++s->chunkCount;
            // ---- DEBUG: sub-step timing print (gated) ----
            if (TILE_DEBUG_SUBSTEP_TIMING && iterations > 0) {
                // Single line per pump: SD-read us / pngle-feed us / bytes-read / bytes-fed / iterations
                Serial.printf("[TILE-SUB] sd=%luus feed=%luus read=%lu fed=%lu iters=%d budget_left=%luus fed_total=%u/%u\n",
                              totalSdUs, totalFeedUs,
                              totalReadBytes, totalFeedBytes,
                              iterations,
                              (unsigned long)(PUMP_BUDGET_US - (micros() - budgetStart)),
                              (unsigned)s->bytesFed,
                              (unsigned)s->fileSize);
            }
            // ---------------------------------------------
        }
    }
    // 2. Otherwise, start the next request.
    else if (_reqCount > 0) {
        // Find highest-priority request.
        int bestIdx = -1;
        Priority bestPrio = Priority::PRIO_LOW;
        uint32_t bestEnq = 0xFFFFFFFFu;
        for (int i = 0; i < _reqCount; ++i) {
            int idx = (_reqHead + i) % REQ_QUEUE_SIZE;
            if (!_reqQ[idx].valid) continue;
            if ((int)_reqQ[idx].prio > (int)bestPrio ||
                ((int)_reqQ[idx].prio == (int)bestPrio && _reqQ[idx].enqMs < bestEnq)) {
                bestPrio = _reqQ[idx].prio;
                bestEnq  = _reqQ[idx].enqMs;
                bestIdx  = idx;
            }
        }
        if (bestIdx >= 0) {
            const TileKey& k = _reqQ[bestIdx].key;
            Slot* s = pickFreeSlot();
            if (s) {
                if (!startDecode(*s, k.style, k.z, k.x, k.y)) {
                    // Failed to start (missing file, alloc failure, etc) — the
                    // slot is already marked MISSING/FREE. Don't requeue.
                }
                // Pop ONLY the winning entry. bestIdx is not necessarily the
                // head (priority beats FIFO), so the old unconditional
                // "_reqHead++" stepped over still-valid entries and corrupted
                // the ring — multi-tile views (z>=1) silently dropped requests
                // and stayed on gray placeholders.
                // The rest of the ring logic (dedup scan, enqueueRequest's
                // tail push) assumes the live entries occupy the contiguous
                // window [_reqHead, _reqHead+_reqCount), so backfill the
                // popped slot with the head entry instead of leaving a hole.
                // Queue order is irrelevant — selection is by priority /
                // enqueue time, not by position.
                if (bestIdx != _reqHead) {
                    _reqQ[bestIdx] = _reqQ[_reqHead];
                }
                _reqQ[_reqHead].valid = false;
                _reqHead = (_reqHead + 1) % REQ_QUEUE_SIZE;
                --_reqCount;
                // Defensive: skip any stale invalid slot at the front so the
                // scan window stays aligned with the live entries.
                while (_reqCount > 0 && !_reqQ[_reqHead].valid) {
                    _reqHead = (_reqHead + 1) % REQ_QUEUE_SIZE;
                }
                if (_reqCount == 0) {
                    // Empty queue — normalize head to the next write position.
                    _reqHead = _reqTail;
                }
            }
            // No free slot (all LOADING) — leave the request queued and retry
            // on the next pump(). Dropping it here would lose the tile.
        }
    }

    unsigned long pumpElapsed = millis() - pumpStart;
    _lastPumpMs = (uint32_t)pumpElapsed;
    if (_lastPumpMs > _maxPumpMs) _maxPumpMs = _lastPumpMs;
    ++_pumpCount;
    if (pumpElapsed > ABSOLUTE_MAX_PUMP_MS) {
        Serial.printf("[TILE] pump() exceeded soft budget: %lu ms\n", pumpElapsed);
    }
}

// =============================================================================
// Public read API
// =============================================================================

const lv_img_dsc_t* TileCache::getTileIfReady(const char* style, int z, int x, int y,
                                              uint32_t* outGen) {
    if (outGen) *outGen = 0;
    if (!style) return nullptr;
    if (!_poolInited) return nullptr;
    Slot* s = findReadySlot(style, z, x, y);
    if (!s) return nullptr;
    if (outGen) *outGen = s->generation;
    return &s->dsc;
}

void TileCache::clearAllPins() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        _slots[i].pinCount = 0;
    }
}

bool TileCache::pinTile(const char* style, int z, int x, int y) {
    if (!style || !_poolInited) return false;
    Slot* s = findSlotByKey(style, z, x, y);
    if (!s) return false;
    // Simple set-pin (not a true refcount): the map screen clears every pin
    // and re-pins the visible set on each rebuild, so saturating at 1 is
    // enough and can't leak a pin that never gets released.
    s->pinCount = 1;
    s->lastTouchMs = millis();
    return true;
}

// =============================================================================
// Diagnostics
// =============================================================================

void TileCache::dumpStatus() const {
    Serial.printf("[TILE] pool=%s slots: ", _poolInited ? "ok" : "UNINIT");
    int ready = 0, loading = 0, missing = 0, free = 0;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        switch (_slots[i].state) {
            case SlotState::READY:   ++ready;   break;
            case SlotState::LOADING: ++loading; break;
            case SlotState::MISSING: ++missing;  break;
            case SlotState::FREE:    ++free;    break;
        }
    }
    Serial.printf("ready=%d loading=%d missing=%d free=%d last=%lums max=%lums pumps=%lu\n",
                  ready, loading, missing, free,
                  (unsigned long)_lastPumpMs, (unsigned long)_maxPumpMs,
                  (unsigned long)_pumpCount);
    if (loading > 0) {
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (_slots[i].state == SlotState::LOADING) {
                Serial.printf("[TILE]   loading %s/%ld/%ld/%ld pngle=%p bytes=%lu/%lu chunks=%lu px=%lu\n",
                              _slots[i].key.style, (long)_slots[i].key.z,
                              (long)_slots[i].key.x, (long)_slots[i].key.y,
                              (const void*)_slots[i].pngle,
                              (unsigned long)_slots[i].bytesFed,
                              (unsigned long)_slots[i].fileSize,
                              (unsigned long)_slots[i].chunkCount,
                              (unsigned long)_slots[i].pixelsWritten);
            }
        }
    }
}
