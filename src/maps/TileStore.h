#pragma once

// =============================================================================
// TileStore.h — path building + chunked SD access for slippy-map PNG tiles
// =============================================================================
//
// On-disk layout under /maps/<mapset>/ can be either of:
//
//   ZXY (Meshtastic MUI / MUIMapBuilder canonical):
//      /maps/<mapset>/<z>/<x>/<y>.png
//      e.g. /maps/Basemapsxyz-OSM/10/512/340.png
//
//   XYZ (alternate nesting some cards use):
//      /maps/<mapset>/<x>/<y>/<z>.png
//      e.g. /maps/Basemapsxyz-OSM/512/340/10.png
//
// At low zoom many keys collide under both layouts (z0 always 0/0/0.png;
// z1 tile (1,1) is 1/1/1.png either way). That made a wrong primary layout
// look "half working": z0 perfect, z1 SE quadrant perfect, other quadrants
// either missing or the wrong geography (path components reinterpreted).
//
// openTile() / tileExists() try the sticky preferred layout first, then the
// alternate, and lock onto whichever hits. Callers always pass logical
// (z, x, y) tile indices — never path-order-dependent.
// =============================================================================

#include <Arduino.h>
#include <SD.h>
#include "storage/SDStore.h"

class TileStore {
public:
    enum class Layout : uint8_t {
        Unknown = 0,
        ZXY     = 1,  // /maps/<style>/<z>/<x>/<y>.png  (Meshtastic canonical)
        XYZ     = 2,  // /maps/<style>/<x>/<y>/<z>.png
    };

    // Last layout that successfully opened a tile (Unknown until first hit).
    static Layout detectedLayout() { return s_layout; }

    static const char* layoutName(Layout L) {
        switch (L) {
            case Layout::ZXY: return "z/x/y";
            case Layout::XYZ: return "x/y/z";
            default:          return "unknown";
        }
    }

    // Build path for a specific layout. Callers that only need "the" path
    // should prefer openTile()/tileExists() so dual-layout probing runs.
    static String tilePathLayout(Layout L, const char* style, int z, int x, int y) {
        String p = "/maps/";
        p += style;
        p += '/';
        if (L == Layout::XYZ) {
            // /maps/<style>/<x>/<y>/<z>.png
            p += x;
            p += '/';
            p += y;
            p += '/';
            p += z;
        } else {
            // Default / unknown → ZXY form
            // /maps/<style>/<z>/<x>/<y>.png
            p += z;
            p += '/';
            p += x;
            p += '/';
            p += y;
        }
        p += ".png";
        return p;
    }

    // Convenience: path in the currently detected layout (ZXY if still unknown).
    static String tilePath(const char* style, int z, int x, int y) {
        Layout L = (s_layout == Layout::Unknown) ? Layout::ZXY : s_layout;
        return tilePathLayout(L, style, z, x, y);
    }

    static bool tileExists(SDStore& sd, const char* style, int z, int x, int y) {
        return openExistingPath(sd, style, z, x, y).length() > 0;
    }

    // Open a tile file for incremental chunked reading. Caller MUST close().
    // Returns an invalid File if SD is not ready or neither layout has the file.
    static File openTile(SDStore& sd, const char* style, int z, int x, int y) {
        String path = openExistingPath(sd, style, z, x, y);
        if (path.length() == 0) return File();
        return sd.openFile(path.c_str());
    }

    static size_t tileSize(SDStore& sd, const char* style, int z, int x, int y) {
        File f = openTile(sd, style, z, x, y);
        if (!f) return 0;
        size_t s = f.size();
        f.close();
        return s;
    }

    // Force re-probe on next open (e.g. after SD remount / mapset change).
    static void resetLayoutDetection() { s_layout = Layout::Unknown; }

private:
    // Sticky across opens so we only double-stat until the first hit.
    static Layout s_layout;

    // Returns the existing path string, or empty if neither layout has the file.
    // Updates s_layout on the first successful probe.
    static String openExistingPath(SDStore& sd, const char* style, int z, int x, int y) {
        // Preferred order once known: only that layout.
        if (s_layout == Layout::ZXY || s_layout == Layout::XYZ) {
            String p = tilePathLayout(s_layout, style, z, x, y);
            if (sd.exists(p.c_str())) return p;
            // Sticky miss: still try the other layout once in case the card
            // mixed layouts or detection was wrong from a colliding key
            // (0/0/0.png). Do not flip sticky on a single miss — only on hit.
            Layout other = (s_layout == Layout::ZXY) ? Layout::XYZ : Layout::ZXY;
            String p2 = tilePathLayout(other, style, z, x, y);
            if (sd.exists(p2.c_str())) {
                // Disambiguating hit on the other layout — lock to it.
                // (Happens if first lock was from a colliding path like z0.)
                if (z > 0 || x > 0 || y > 0) {
                    s_layout = other;
                    Serial.printf("[TILE] path layout -> %s (re-probed on z=%d x=%d y=%d)\n",
                                  layoutName(s_layout), z, x, y);
                }
                return p2;
            }
            return String();
        }

        // Unknown: try Meshtastic canonical ZXY first, then XYZ.
        String pz = tilePathLayout(Layout::ZXY, style, z, x, y);
        if (sd.exists(pz.c_str())) {
            // Only lock on a non-colliding key. z0 (0,0) and any tile where
            // z==x && x==y produce identical paths under both layouts, so a
            // hit there does not tell us which nesting the card uses.
            if (!(z == x && x == y)) {
                s_layout = Layout::ZXY;
                Serial.printf("[TILE] path layout -> z/x/y (first hit z=%d x=%d y=%d)\n",
                              z, x, y);
            }
            return pz;
        }
        String px = tilePathLayout(Layout::XYZ, style, z, x, y);
        if (sd.exists(px.c_str())) {
            if (!(z == x && x == y)) {
                s_layout = Layout::XYZ;
                Serial.printf("[TILE] path layout -> x/y/z (first hit z=%d x=%d y=%d)\n",
                              z, x, y);
            }
            return px;
        }
        return String();
    }
};

// One definition for the sticky layout flag (header-only class).
inline TileStore::Layout TileStore::s_layout = TileStore::Layout::Unknown;
