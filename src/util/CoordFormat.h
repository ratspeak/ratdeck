#pragma once

// Format WGS84 lat/lon for on-device GPS UI.
// Runtime display only — never log or commit real coordinates.

#include <stddef.h>

namespace CoordFormat {

// Each formatter writes a multi-line or single-line string into buf
// (NUL-terminated). Returns bytes written excluding NUL, or 0 on error.
// lat/lon in decimal degrees (negative = S / W).

// "34.052235 N, 84.123456 W"
size_t formatDD(char* buf, size_t n, double lat, double lon);

// "34° 03.1341' N, 84° 07.4074' W"
size_t formatDM(char* buf, size_t n, double lat, double lon);

// "34° 03' 08.0\" N, 84° 07' 24.4\" W"
size_t formatDMS(char* buf, size_t n, double lat, double lon);

// Open Location Code (Plus Code), length 11 e.g. "86FV3R2X+2V7"
size_t formatPlusCode(char* buf, size_t n, double lat, double lon);

// "16S 741234 3765432" (zone+band, easting m, northing m)
size_t formatUTM(char* buf, size_t n, double lat, double lon);

}  // namespace CoordFormat
