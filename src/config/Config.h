#pragma once

// =============================================================================
// rsDeck — Compile-Time Configuration
// =============================================================================

#define RSDECK_VERSION_MAJOR  2
#define RSDECK_VERSION_MINOR  0
#define RSDECK_VERSION_PATCH  2
#define RSDECK_VERSION_STRING "2.0.2"

// --- Feature Flags ---
#define HAS_DISPLAY     true
#define HAS_KEYBOARD    true
#define HAS_TOUCH       true
#define HAS_TRACKBALL   true
#define HAS_LORA        true
#define HAS_WIFI        true
// TODO(BLE): Revisit as a dedicated Ratspeak BLE Peer transport based on the
// rsReticulum GATT/fragmentation/anti-loop design, after T-Deck Plus memory,
// flash, power, and interop behavior are tested end to end.
#ifndef RSDECK_EXPERIMENTAL_BLE
#define RSDECK_EXPERIMENTAL_BLE 0
#endif
#define HAS_BLE         RSDECK_EXPERIMENTAL_BLE
#define HAS_SD          true
#define HAS_AUDIO       true
#define HAS_GPS         true    // UBlox MIA-M10Q UART GPS

// --- WiFi Defaults ---
#define WIFI_AP_PORT        4242
#define WIFI_AP_PASSWORD    "ratspeak"

// --- Branding ---
// Text shown in the top status bar. Override locally via
// platformio_override.ini (gitignored, never touches tracked source) with:
//   build_flags = -DRSDECK_BRAND_TEXT='"YourTextHere"'
// Left at the project default here so upstream builds/PRs always retain it.
#ifndef RSDECK_BRAND_TEXT
#define RSDECK_BRAND_TEXT "Ratspeak.org"
#endif

// Boot chime. Override to 0 locally via platformio_override.ini with:
//   build_flags = -DRSDECK_PLAY_BOOT_SOUND=0
// Left at the project default (1) here so upstream builds/PRs always retain it.
#ifndef RSDECK_PLAY_BOOT_SOUND
#define RSDECK_PLAY_BOOT_SOUND 1
#endif

// --- Storage Paths ---
#define PATH_IDENTITY       "/identity/identity.key"
#define PATH_IDENTITY_BAK   "/identity/identity.key.bak"
#define PATH_PATHS          "/transport/paths.msgpack"
#define PATH_USER_CONFIG    "/config/user.json"
// Directory paths intentionally have NO trailing slash — some FATFS/VFS
// readdir paths fail to enumerate when given a path ending in '/'.
// Concat sites must add their own '/' before the basename.
#define PATH_CONTACTS       "/contacts"
#define PATH_MESSAGES       "/messages"

// --- SD Card Paths ---
// Legacy path kept intentionally so existing Standalone users keep their data.
// TODO: Migrate to /rsdeck only with an explicit data migration plan.
#define SD_PATH_CONFIG_DIR   "/ratdeck/config"
#define SD_PATH_USER_CONFIG  "/ratdeck/config/user.json"
#define SD_PATH_MESSAGES     "/ratdeck/messages"
#define SD_PATH_CONTACTS     "/ratdeck/contacts"
#define SD_PATH_IDENTITY_DIR "/ratdeck/identity"
#define SD_PATH_IDENTITY     "/ratdeck/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/ratdeck/identity/import.identity"
#define SD_PATH_IMPORT_ID    "/ratdeck/identity/import.key"

// --- TCP Client ---
#define MAX_TCP_CONNECTIONS         4
#define TCP_DEFAULT_PORT            4242
#define TCP_RECONNECT_INTERVAL_MS   15000
#define TCP_CONNECT_TIMEOUT_MS      500

// --- Announce Flood Defense ---
// Transport-level announce drop (before Ed25519 verify). Keep low under
// I2P/TCP floods so main loop still services touch/LVGL.
#define RSDECK_MAX_ANNOUNCES_PER_SEC 4

// --- Limits ---
#define RSDECK_MAX_NODES             100   // Endpoint device, not transport node
#define RSDECK_MAX_MESSAGES_PER_CONV 100
#define FLASH_MSG_CACHE_LIMIT         20
#define RSDECK_MAX_OUTQUEUE          20
#define RSDECK_RNODE_SINGLE_FRAME_RAW_MAX 254  // Raw Reticulum bytes per RNode LoRa RF frame
#define PATH_PERSIST_INTERVAL_MS  60000

// --- Power Management ---
#define SCREEN_DIM_TIMEOUT_MS   30000
#define SCREEN_OFF_TIMEOUT_MS   60000
#define SCREEN_DIM_BRIGHTNESS   64

// --- Radio Regions ---
enum RadioRegion : uint8_t {
    REGION_AMERICAS  = 0,  // 915 MHz (902-928 ISM)
    REGION_EUROPE    = 1,  // 868 MHz (863-870)
    REGION_AUSTRALIA = 2,  // 915 MHz (915-928)
    REGION_ASIA      = 3,  // 923 MHz (AS923)
    REGION_COUNT     = 4
};

static constexpr uint32_t REGION_FREQ[REGION_COUNT] = {
    915000000, 868000000, 915000000, 923000000
};

static const char* const REGION_LABELS[REGION_COUNT] = {
    "Americas (915)", "Europe (868)", "Australia (915)", "Asia (923)"
};

// --- Serial Debug ---
#define SERIAL_BAUD  115200

// --- Shared Utilities (defined in main.cpp) ---
#include <Arduino.h>
#include <Bytes.h>
RNS::Bytes encodeAnnounceName(const String& name);
