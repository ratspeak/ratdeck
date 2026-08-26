#pragma once

// =============================================================================
// TileCache.h — chunked tile-loading state machine + fixed PSRAM pool
// =============================================================================
//
// Design constraints (from project brief):
//   * LoRa RX is a single-"latest-packet" pointer; if loop() stalls too long
//     an unread packet is overwritten before drain. Budget per pump() call:
//     ~20–25 ms (well below the ~30 ms fastest LoRa packet airtime).
//   * LVGL's decoder framework is OFF (LV_USE_PNG == 0). We build
//     lv_img_dsc_t manually and feed LVGL via lv_img_set_src().
//   * LV_COLOR_16_SWAP == 1 — pixel bytes must be stored swap-correct so the
//     display fetches them in the right order. We use lv_color_make().full,
//     which already applies the swap at compile time.
//   * Pool allocated ONCE lazily on first use, never freed. PSRAM is the only
//     allocation target (heap_caps_malloc, MALLOC_CAP_SPIRAM).
//   * Negative cache of ~64 (z,x,y) keys confirmed missing on SD, so fast
//     panning doesn't keep calling exists().
//   * LVGL render happens on the same main loop; the only "blocking" interior
//     in a single pump() call is one SD read + one pngle_feed.
//   * packetAvailable guard: skipped by design (TODO comment) — the chunk
//     budget is the primary safety property, and wiring SX1262::packetAvailable
//     into TileCache would couple unrelated layers just to add a redundant
//     guard.
// =============================================================================

#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <cstdint>

#include "storage/SDStore.h"

extern "C" {
#include "pngle.h"
}

// Forward-declare to avoid pulling miniz.h into the public surface.
struct _pngle_t;

class TileCache {
public:
    // Tunable: bytes of compressed PNG data fed per pump() call. Lower =
    // shorter per-call latency / more responsive to LoRa. Too low = thousands
    // of pump() calls per tile.
    //
    // IMPORTANT: the dominant per-pump cost on ESP32-S3 is NOT the SD read
    // (~2-3 ms for this size at 16 MHz SPI), it is pngle_feed() —
    // specifically pngle_draw_pixels() running at ~5 us/pixel for ~10 K
    // pixels when pngle's 32 KB lz_buf fills in one call (measured ~55 ms
    // on real hw, including a no-op user callback — the cost is almost
    // entirely in pngle's own per-pixel pipeline: read_pixel_value +
    // adjust_color + draw_callback invocation, not the user callback body
    // itself). The burst is intrinsic to pngle — to spread it across
    // multiple per-call flushes would require modifying lib/pngle/pngle.c
    // to force-flush the lz_buf on smaller batches, but my attempts to
    // patch tinfl_decompress output (capping out_bytes, or force-flushing
    // mid-call) corrupted the inflate state and triggered
    // "Failed to decompress the IDAT stream" errors. So the chunk size
    // here is 1024 (down from 4096) to minimize the number of bytes fed
    // per pump, and the wall-clock time-budget loop inside pump() is the
    // SAFETY NET that prevents any future slow path from wedging the
    // main loop. The realistic worst-case single-pump cost remains ~65-70
    // ms (one iter where pngle's 32 KB lz_buf fill + per-pixel burst
    // fires), but the time-budget check would catch any pump that exceeds
    // ~18 ms wall-clock across multiple iters.
    static constexpr size_t TILE_CHUNK_BYTES = 1024;
    // KNOWN LIMITATION (measured on real hardware, oracle-reviewed, deemed
    // safe to ship): pngle's internal ~32KB deflate LZ77 dictionary
    // (lib/pngle/pngle.c:124 lz_buf[TINFL_LZ_DICT_SIZE]) is NOT a batching
    // buffer we can shrink or externally flush early - it IS the sliding
    // window tinfl_decompress needs for back-references, and both an
    // lz_buf-shrink and an early-flush patch were tried and broke decode
    // (tinfl_status < TINFL_STATUS_DONE). Net effect: pngle_draw_pixels
    // fires in ~60-180ms bursts (measured: ~60-70ms for RGB8, up to ~180ms
    // projected for 8-bit palette tiles at ~10-32K pixels/burst) regardless
    // of our TILE_CHUNK_BYTES feed size, 1-3x per tile. This is safe at the
    // default radio preset (SF11/250kHz, 550ms+ min packet airtime, burst is
    // 8x under that floor) but could theoretically collide with a
    // user-configured fast/short-packet preset (SF7-class, ~30-80ms
    // airtime) if a burst straddles two back-to-back RX completions - see
    // the radio.packetAvailable guard in main.cpp's tileCache.pump() call
    // site, which mitigates by refusing to START a new pump while a packet
    // is already waiting to be drained (does not bound an in-progress
    // burst, only avoids starting one during known-risky moments).
    //
    // FOLLOW-UP FIX (spec'd by oracle review, not yet implemented): pngle.c's
    // decode loop already calls tinfl_decompress() incrementally and gets
    // out_bytes back after every call - it just defers drawing until the
    // 32KB ring buffer fills or the image completes. The correct patch
    // draws newly-produced bytes immediately after EVERY tinfl_decompress
    // return (tracking a separate `drawn` offset) instead of waiting for
    // the ring to fill, while leaving next_out/avail_out ring bookkeeping
    // untouched (still only reset at avail_out==0). This shrinks burst size
    // proportional to feed chunk size without touching tinfl's invariants,
    // unlike the two failed attempts which incorrectly reset next_out
    // early. ~10-15 line change in lib/pngle/pngle.c's drain logic (see the
    // XXX comments near TINFL_LZ_DICT_SIZE usage). Do this before/if the
    // map screen ships with support for fast/short-packet radio presets.

    static constexpr int     TILE_PX         = 256;          // standard XYZ tile
    static constexpr int     SLOT_COUNT      = 12;           // ~1.5 MB PSRAM pool
    static constexpr size_t  SLOT_BYTES      = TILE_PX * TILE_PX * 2;  // RGB565
    static constexpr size_t  STYLE_MAX       = 24;           // incl. NUL
    static constexpr int     REQ_QUEUE_SIZE  = 8;
    static constexpr int     NEG_CACHE_SIZE  = 64;

    enum class SlotState : uint8_t {
        FREE,        // pool slot is unallocated
        LOADING,     // file open, pngle feeding
        READY,       // decoded, lv_img_dsc_t valid
        MISSING      // file confirmed absent on SD
    };

    enum class Priority : uint8_t {
        PRIO_LOW = 0, PRIO_NORMAL = 1, PRIO_HIGH = 2
    };

    struct TileKey {
        char     style[STYLE_MAX];
        int32_t  z;
        int32_t  x;
        int32_t  y;

        bool operator==(const TileKey& o) const {
            return z == o.z && x == o.x && y == o.y && strcmp(style, o.style) == 0;
        }
    };

    void begin(SDStore* sd);

    // Enqueue a tile-load request. Dedupes against in-flight + cached entries.
    // Returns true if the request was queued (or already satisfied).
    bool requestTile(const char* style, int z, int x, int y, Priority prio = Priority::PRIO_NORMAL);

    // Called once per main-loop iteration. Does at most one chunk of work.
    // Either continues an in-progress decode, or starts the next queued
    // request onto a free/LRU-evicted slot.
    void pump();

    // Returns a ready-to-use lv_img_dsc_t pointing into the slot's PSRAM
    // buffer if the tile is cached and READY, else nullptr.
    //
    // IMPORTANT: the returned lv_img_dsc_t is PERMANENT per slot — its
    // address never changes and its pixel buffer is reused when the slot is
    // recycled for a different tile. A consumer that caches the pointer
    // therefore CANNOT detect "same pointer, different tile". Pass outGen to
    // read the slot's generation counter (bumped on every startDecode() /
    // freeSlot()); re-bind + invalidate whenever (dsc, generation) changes,
    // not just when the pointer changes. This is what fixed the "z0 tile
    // rendered, vertically squished, inside a z1 grid cell" symptom.
    const lv_img_dsc_t* getTileIfReady(const char* style, int z, int x, int y,
                                       uint32_t* outGen = nullptr);

    // ---- Pinning (on-screen protection) -------------------------------------
    // A slot whose pinCount > 0 is never LRU-evicted, so its PSRAM buffer
    // cannot be memset + overwritten by a new decode while an lv_img still
    // points at it. Intended usage from the map screen, once per rebuild:
    //   clearAllPins();
    //   for each visible tile key: pinTile(style, z, x, y);
    void clearAllPins();
    // Pins the slot holding this key if it is READY or LOADING. Returns true
    // if a slot was found and pinned.
    bool pinTile(const char* style, int z, int x, int y);

    // Manually evict a tile (e.g. on memory pressure or zoom-out).
    void evict(const char* style, int z, int x, int y);

    // Diagnostic: dump current pool state to serial.
    void dumpStatus() const;

    // Diagnostic: latest per-pump() timing in ms (max across recent calls).
    // Used by the serial-command test to verify the chunk-latency budget.
    uint32_t lastPumpMs() const { return _lastPumpMs; }
    uint32_t maxPumpMs() const  { return _maxPumpMs; }
    uint32_t pumpCount() const { return _pumpCount; }

    // Diagnostic: queue state for serial-test debug (private accessors
    // because the queue head/tail/count are private impl details).
    int reqCount() const { return _reqCount; }
    int reqHead()  const { return _reqHead; }
    int reqTail()  const { return _reqTail; }

private:
    // ---- Pool bookkeeping ----
    SDStore* _sd = nullptr;
    bool _poolInited = false;

    struct Slot {
        SlotState  state          = SlotState::FREE;
        TileKey    key;
        File       file;
        uint16_t*  pxbuf          = nullptr;  // PSRAM, 128 KB, owned
        lv_img_dsc_t dsc          = {};
        pngle_t*   pngle          = nullptr;
        size_t     bytesFed       = 0;       // total bytes read from file
        size_t     fileSize       = 0;
        size_t     ioBufLen       = 0;       // bytes of leftover in the shared ioBuf
        uint32_t   lastTouchMs    = 0;
        uint32_t   chunkCount     = 0;       // pump() calls spent on this slot
        uint32_t   first8[8]      = {0};     // first 8 pixel RGB565 values, for sanity
        int32_t    pxW            = 0;       // pngle IHDR width
        int32_t    pxH            = 0;       // pngle IHDR height
        uint32_t   pixelsWritten  = 0;
        // Bumped on every startDecode() and freeSlot(), i.e. every time the
        // buffer's content identity is reset. `dsc` is permanent, so this is
        // the ONLY way a UI consumer can tell that the pixels behind an
        // already-attached descriptor now belong to a different tile.
        // Wrapping at 2^32 is harmless (a wrap would need 4 billion decodes
        // to land on the exact same value the UI last saw).
        uint32_t   generation     = 0;
        // >0 → slot is on screen; findLruEvictableSlot() must not take it.
        uint8_t    pinCount       = 0;
        // PNG → buffer downsample ratio. Set by onPngInit from IHDR:
        //   1 = standard 256×256 source → 1:1 write to TILE_PX×TILE_PX buffer.
        //   2 = 512×512 source (this mapset ships 512×512 PNGs on SD) → 2:1
        //       nearest-neighbor downsample to the TILE_PX×TILE_PX buffer.
        //   0 = invalid dims (neither 256×256 nor 512×512) — onPngDraw bails,
        //       onPngDone frees the slot, EOF guard neg-caches it.
        // Reset to 1 by startDecode() and freeSlot().
        uint8_t    scale          = 1;
    };
    Slot _slots[SLOT_COUNT] = {};

    // ---- Request queue ----
    struct Request {
        bool    valid = false;
        TileKey key;
        Priority prio = Priority::PRIO_NORMAL;
        uint32_t enqMs = 0;
    };
    Request _reqQ[REQ_QUEUE_SIZE] = {};
    int _reqHead = 0;  // pop from head
    int _reqTail = 0;  // push to tail
    int _reqCount = 0;

    // ---- Negative cache (linear, replace-on-evict oldest) ----
    struct NegEntry {
        bool    valid = false;
        TileKey key;
        uint32_t touchMs = 0;
    };
    NegEntry _negCache[NEG_CACHE_SIZE] = {};

    // ---- Stats ----
    uint32_t _lastPumpMs = 0;
    uint32_t _maxPumpMs  = 0;
    uint32_t _pumpCount  = 0;

    // ---- Internal helpers ----
    void   ensurePool();                                          // allocate PSRAM lazily
    Slot*  findReadySlot(const char* style, int z, int x, int y);
    Slot*  findSlotByKey(const char* style, int z, int x, int y);  // READY or LOADING
    Slot*  pickFreeSlot();                                        // or LRU-evict
    Slot*  pickLoadingSlot();                                     // for pump() continuation
    bool   startDecode(Slot& s, const char* style, int z, int x, int y);
    static void closeDecodeStatic(Slot& s);                       // close file, free pngle — callable from static PNGLE callbacks
    void   closeDecode(Slot& s) { closeDecodeStatic(s); }        // thin wrapper
    void   freeSlot(Slot& s);                                     // recycle to FREE
    bool   isQueuedOrCached(const TileKey& k);
    bool   enqueueRequest(const TileKey& k, Priority p);
    bool   isNegCached(const TileKey& k);
    void   addNegCache(const TileKey& k);
    static int  findLruEvictableSlot(Slot slots[], int n);
    static int  findLoadingSlotIdx(Slot slots[], int n);
    static int  findFreeSlotIdx(Slot slots[], int n);
    static bool keyEq(const TileKey& a, const TileKey& b);
    static void onPngInit(pngle_t* p, uint32_t w, uint32_t h);
    static void onPngDraw(pngle_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]);
    static void onPngDone(pngle_t* p);

    // ---- diagnostics ----
    static void logPsramBudget(const char* tag);
};
