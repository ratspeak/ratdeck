#pragma once

#include <cstdint>

// =============================================================================
// rsDeck design constants
// Field-console palette tuned for the 320x240 LVGL surface.
// Colors are runtime-switchable (Dark = original palette, Light = derived);
// the value tables live in Theme.cpp. Identifiers keep their original
// spelling so call sites read the active palette transparently.
// =============================================================================

namespace Theme {

enum class Scheme : uint8_t { DARK = 0, LIGHT = 1 };

// Switch the active palette (boot default: DARK, the historical palette).
void setScheme(Scheme s);
Scheme scheme();

// --- Backgrounds ---
extern uint32_t BG;             // Screen base
extern uint32_t BG_ELEVATED;    // Buttons, inputs, list items
extern uint32_t BG_SURFACE;     // Modals, dropdowns, shell bars
extern uint32_t BG_HOVER;       // Focus/hover background

// --- Text hierarchy ---
extern uint32_t TEXT_PRIMARY;   // Primary copy
extern uint32_t TEXT_SECONDARY; // Metadata and labels
extern uint32_t TEXT_MUTED;     // Disabled, placeholders

// --- Brand / interactive ---
extern uint32_t PRIMARY;        // Signal green
extern uint32_t PRIMARY_MUTED;  // Pressed/active state
extern uint32_t PRIMARY_SUBTLE; // Selection backgrounds
extern uint32_t ACCENT;         // Console cyan accent

// --- Status ---
extern uint32_t SUCCESS;        // Online, delivered, connected
extern uint32_t WARNING_CLR;    // Queued, pending, caution
extern uint32_t ERROR_CLR;      // Failed, error, critical
extern uint32_t ERROR_SUBTLE;   // Armed destructive backgrounds

// --- Structural ---
extern uint32_t BORDER;         // Subtle structural borders
extern uint32_t BORDER_ACTIVE;  // Focus borders
extern uint32_t DIVIDER;        // Hairline separators

// --- Messages ---
extern uint32_t MSG_OUT_BG;     // Outgoing bubble
extern uint32_t MSG_IN_BG;      // Incoming bubble

// --- Shell ---
extern uint32_t STATUS_BG;
extern uint32_t STATUS_FLASH;
extern uint32_t TAB_BG;
extern uint32_t TAB_ACTIVE_BG;
extern uint32_t TAB_ACTIVE;
extern uint32_t TAB_INACTIVE;
extern uint32_t BADGE_BG;
extern uint32_t TOAST_BG;


// --- Layout Metrics ---
constexpr int SCREEN_W       = 320;
constexpr int SCREEN_H       = 240;
constexpr int STATUS_BAR_H   = 20;
constexpr int TAB_BAR_H      = 26;
constexpr int CONTENT_Y      = STATUS_BAR_H;
constexpr int CONTENT_H      = SCREEN_H - STATUS_BAR_H - TAB_BAR_H;
constexpr int CONTENT_W      = SCREEN_W;

// --- Tab Bar ---
    constexpr int TAB_COUNT      = 6;
    constexpr int ICON_TAB_W     = 38;
    constexpr int TEXT_TAB_W     = 57;
constexpr int TAB_BADGE_W    = 18;
constexpr int TAB_BADGE_H    = 10;

// --- Spacing (base unit: 4px) ---
constexpr int SP_1 = 4;
constexpr int SP_2 = 8;
constexpr int SP_3 = 12;
constexpr int SP_4 = 16;
constexpr int SP_6 = 24;

}  // namespace Theme

// LVGL color helper - available when LVGL is included
#ifdef LV_CONF_H
#include <lvgl.h>
inline lv_color_t lvColor(uint32_t rgb888) { return lv_color_hex(rgb888); }
#endif
