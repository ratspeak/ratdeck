#pragma once

// =============================================================================
// RatDeck — Compile-Time Configuration
// =============================================================================

#define RATDECK_VERSION_MAJOR  1
#define RATDECK_VERSION_MINOR  6
#define RATDECK_VERSION_PATCH  5
#define RATDECK_VERSION_STRING "1.6.5"

// --- Feature Flags ---
#define HAS_DISPLAY     true
#define HAS_KEYBOARD    true
#define HAS_TOUCH       true
#define HAS_TRACKBALL   true
#define HAS_LORA        true
#define HAS_WIFI        true
#define HAS_BLE         true
#define HAS_SD          true
#define HAS_AUDIO       true
#define HAS_GPS         false   // Deprioritized

// --- WiFi Defaults ---
#define WIFI_AP_PORT        4242
#define WIFI_AP_PASSWORD    "ratspeak"

// --- Storage Paths ---
#define PATH_IDENTITY       "/identity/identity.key"
#define PATH_IDENTITY_BAK   "/identity/identity.key.bak"
#define PATH_PATHS          "/transport/paths.msgpack"
#define PATH_USER_CONFIG    "/config/user.json"
#define PATH_CONTACTS       "/contacts/"
#define PATH_MESSAGES       "/messages/"

// --- SD Card Paths (shared with Ratputer) ---
#define SD_PATH_CONFIG_DIR   "/ratputer/config"
#define SD_PATH_USER_CONFIG  "/ratputer/config/user.json"
#define SD_PATH_MESSAGES     "/ratputer/messages/"
#define SD_PATH_CONTACTS     "/ratputer/contacts/"
#define SD_PATH_IDENTITY     "/ratputer/identity/identity.key"

// --- TCP Client ---
#define MAX_TCP_CONNECTIONS         4
#define TCP_DEFAULT_PORT            4242
#define TCP_RECONNECT_INTERVAL_MS   15000
#define TCP_CONNECT_TIMEOUT_MS      5000

// --- Announce Flood Defense ---
#define RATDECK_MAX_ANNOUNCES_PER_SEC 5     // Transport-level rate limit (before Ed25519 verify)

// --- Limits ---
#define RATDECK_MAX_NODES             200   // PSRAM allows more
#define RATDECK_MAX_MESSAGES_PER_CONV 100
#define FLASH_MSG_CACHE_LIMIT         20
#define RATDECK_MAX_OUTQUEUE          20
#define PATH_PERSIST_INTERVAL_MS  60000

// --- Power Management ---
#define SCREEN_DIM_TIMEOUT_MS   30000
#define SCREEN_OFF_TIMEOUT_MS   60000
#define SCREEN_DIM_BRIGHTNESS   64

// --- Serial Debug ---
#define SERIAL_BAUD  115200

// --- Shared Utilities (defined in main.cpp) ---
#include <Arduino.h>
#include <Bytes.h>
RNS::Bytes encodeAnnounceName(const String& name);
