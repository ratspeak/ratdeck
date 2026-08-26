#pragma once

// =============================================================================
// SlippyMath.h — Web Mercator projection helpers for XYZ slippy-map tiles
// =============================================================================
//
// Header-only, pure functions, no Arduino / LVGL / SD dependencies so it can be
// unit-tested on the host. All math operates on 256 px tiles (the standard
// slippy-map convention) and 64-bit pixel coordinates so a full Mercator world
// at zoom 22 still fits comfortably (256 * 2^22 ≈ 1.07 × 10^9, half is ≈5.4×10^8).
//
// Coordinate convention: standard XYZ (y=0 at the NORTH pole, y grows downward).
// This is what OpenStreetMap, Google Maps, Mapbox, etc. serve. If a producer
// uses TMS (y=0 at the SOUTH pole, y grows upward) the tile will render upside
// down. The fix is:
//      y_tms = (1 << zoom) - 1 - y_xyz
// We do NOT apply that inversion here — assume XYZ for now. If the user reports
// tiles showing the southern hemisphere upside down against their MUIMapBuilder
// output, flip the y in lonLatToWorldPx() before converting WorldPx → TileXY.
// =============================================================================

#include <cmath>
#include <cstdint>

namespace SlippyMath {

constexpr int32_t TILE_SIZE = 256;
constexpr double  PI_D      = 3.14159265358979323846;

struct WorldPx {
    int64_t x;
    int64_t y;
};

struct TileXY {
    int32_t x;
    int32_t y;
};

// Floor division for possibly-negative values — C/C++ '/' truncates toward 0
// which is the wrong semantics for converting a pixel coordinate to its tile.
inline int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) --q;
    return q;
}

inline int32_t floorDiv32(int64_t a, int64_t b) {
    return (int32_t)floorDiv(a, b);
}

// World pixel coordinates at the given zoom for a longitude/latitude pair.
// `zoom` is the standard XYZ zoom level (0..22 typical; values outside that
// range are mathematically valid but produce absurdly large coordinates).
inline WorldPx lonLatToWorldPx(double lon, double lat, int zoom) {
    if (zoom < 0) zoom = 0;
    if (zoom > 30) zoom = 30;  // overflow guard
    const double n = (double)(1ULL << zoom) * (double)TILE_SIZE;  // total world pixels
    const double latRad = lat * PI_D / 180.0;
    // Standard Web Mercator: project lat to [-1, 1] then run it through asinh.
    // Clamp to ±(π/2 − ε) so a pole doesn't blow up infinity.
    double clamped = latRad;
    const double maxLat = 1.4844222297453324;  // ≈ 85.05113° in radians
    if (clamped >  maxLat) clamped =  maxLat;
    if (clamped < -maxLat) clamped = -maxLat;
    const double px = (lon + 180.0) / 360.0 * n;
    const double py = (1.0 - std::asinh(std::tan(clamped)) / PI_D) / 2.0 * n;
    return { (int64_t)std::llround(px), (int64_t)std::llround(py) };
}

// Inverse of lonLatToWorldPx — give it world pixels and a zoom, get the
// longitude/latitude of the pixel's top-left corner (i.e. the (x,y) point).
inline void worldPxToLonLat(WorldPx px, int zoom, double& outLon, double& outLat) {
    if (zoom < 0) zoom = 0;
    if (zoom > 30) zoom = 30;
    const double n = (double)(1ULL << zoom) * (double)TILE_SIZE;
    const double lon = (double)px.x / n * 360.0 - 180.0;
    const double y   = (double)px.y / n;
    // Guard against the singularity at y=0 or y=1 (the poles).
    double latRad;
    if (y <= 0.0) {
        latRad = PI_D / 2.0;
    } else if (y >= 1.0) {
        latRad = -PI_D / 2.0;
    } else {
        latRad = std::atan(std::sinh(PI_D * (1.0 - 2.0 * y)));
    }
    outLon = lon;
    outLat = latRad * 180.0 / PI_D;
}

// World pixel → tile index. Floor division (not truncation) so negative
// coordinates map to the correct negative tile.
inline TileXY worldPxToTile(WorldPx px) {
    return {
        floorDiv32(px.x, TILE_SIZE),
        floorDiv32(px.y, TILE_SIZE)
    };
}

}  // namespace SlippyMath
