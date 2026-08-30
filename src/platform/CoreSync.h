#pragma once
//
// CoreSync — concurrency primitives for the optional UI/network core split.
//
// The T-Deck runs everything in a single Arduino loopTask by default. When
// RSDECK_UI_CORE_SPLIT is enabled, the Reticulum/radio/network stack is moved
// to its own FreeRTOS task pinned to core 0, leaving LVGL + input on the
// loopTask (core 1). Two subsystems are then shared across cores and must be
// serialized:
//
//   * spiBus  — the display (LovyanGFX), SX1262 radio, and SD card all sit on
//               one physical FSPI bus. Every compound transaction takes
//               spiBusMutex so a display flush can't interleave with a radio
//               read on the wire.
//   * rns     — microReticulum / LXMF / AnnounceManager data structures are not
//               thread-safe. The network task holds rnsMutex while processing;
//               the UI takes it (briefly, try-lock) when reading state to draw.
//
// When RSDECK_UI_CORE_SPLIT is 0 everything is single-threaded and all of the
// helpers below compile down to no-ops — behavior is byte-for-byte the stock
// firmware.
//
#ifndef RSDECK_UI_CORE_SPLIT
#define RSDECK_UI_CORE_SPLIT 0
#endif

#if RSDECK_UI_CORE_SPLIT
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include <stdint.h>
#include <atomic>

namespace CoreSync {

// Create the mutexes. Call once, early in setup(), before Display/radio init.
bool begin();

#if RSDECK_UI_CORE_SPLIT

extern SemaphoreHandle_t spiBusMutex;
extern SemaphoreHandle_t rnsMutex;

inline void lockSpiBus()   { xSemaphoreTakeRecursive(spiBusMutex, portMAX_DELAY); }
inline void unlockSpiBus() { xSemaphoreGiveRecursive(spiBusMutex); }

// Block indefinitely for correctness-critical sections (network task processing).
inline void lockRns()   { xSemaphoreTakeRecursive(rnsMutex, portMAX_DELAY); }
inline void unlockRns() { xSemaphoreGiveRecursive(rnsMutex); }
// Try-lock for the render/refresh path: never let the UI stall on crypto. A
// missed lock simply means "draw stale data this frame, refresh next tick".
inline bool tryLockRns(uint32_t ms) {
    return xSemaphoreTakeRecursive(rnsMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

#else  // single-threaded build — all no-ops

inline void lockSpiBus()   {}
inline void unlockSpiBus() {}
inline void lockRns()      {}
inline void unlockRns()    {}
inline bool tryLockRns(uint32_t) { return true; }

#endif

// RAII guard for the shared SPI bus. Scope it around one compound transaction
// (flush, radio opcode, or a File open→read→close sequence).
struct SpiBusGuard {
    SpiBusGuard()  { lockSpiBus(); }
    ~SpiBusGuard() { unlockSpiBus(); }
    SpiBusGuard(const SpiBusGuard&) = delete;
    SpiBusGuard& operator=(const SpiBusGuard&) = delete;
};

// RAII guard for the RNS/LXMF data structures (blocking). Use in the network
// task and for UI-issued actions that must not be dropped.
struct RnsGuard {
    RnsGuard()  { lockRns(); }
    ~RnsGuard() { unlockRns(); }
    RnsGuard(const RnsGuard&) = delete;
    RnsGuard& operator=(const RnsGuard&) = delete;
};

// RAII try-guard for the RNS data structures used from the render/refresh path.
// Check `held()` before touching shared state; if false, skip this cycle.
struct RnsTryGuard {
    explicit RnsTryGuard(uint32_t ms) : _held(tryLockRns(ms)) {}
    ~RnsTryGuard() { if (_held) unlockRns(); }
    bool held() const { return _held; }
    RnsTryGuard(const RnsTryGuard&) = delete;
    RnsTryGuard& operator=(const RnsTryGuard&) = delete;
private:
    bool _held;
};

// -------------------------------------------------------------------------
// Network -> UI status snapshot.
//
// The network task (core 0) publishes connectivity state here as plain atomics;
// the UI loop (core 1) reads it once per status tick and pushes the values into
// the (change-gated) status bar. This replaces the network stack reaching into
// LVGL directly, which is not safe once the two run on different cores.
//
// Transient one-shot events (announce TX flash, a toast raised from network
// context) are delivered as monotonically-increasing sequence numbers: the UI
// remembers the last value it saw and acts when it changes.
// -------------------------------------------------------------------------
struct NetStatus {
    std::atomic<bool> loraOnline{false};
    std::atomic<bool> wifiEnabled{false};
    std::atomic<bool> wifiActive{false};
    std::atomic<bool> bleEnabled{false};
    std::atomic<bool> bleActive{false};
    std::atomic<bool> tcpConnected{false};
    std::atomic<bool> gpsFix{false};
    std::atomic<int>  autoIfacePeers{-1};   // -1 == AutoInterface offline
    std::atomic<int>  unreadMessages{0};
    std::atomic<uint32_t> announceSeq{0};   // bump on each announce TX -> UI flashes
    std::atomic<uint32_t> messageSeq{0};    // bump on each received message
    std::atomic<uint32_t> toastSeq{0};      // bump to raise a deferred toast
};

extern NetStatus netStatus;

// Raise a toast from any core. The message is copied into a small internal
// buffer under a lightweight lock; the UI loop displays it when toastSeq moves.
void requestToast(const char* msg, uint32_t durationMs);
// Called by the UI loop: if a new toast is pending, copies it into `out`
// (size >= 64) and returns its duration; returns 0 when nothing is pending.
uint32_t takePendingToast(char* out, uint32_t outSize);

}  // namespace CoreSync
