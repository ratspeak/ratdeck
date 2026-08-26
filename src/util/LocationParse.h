#pragma once

// Parse WGS84 lat/lon from free-text LXMF content.
// Conservative: only clear decimal formats. No Plus Code / MGRS yet.
// Never logs coordinates.

#include <cstddef>

namespace LocationParse {

// Returns true and writes lat/lon if a single plausible pair is found.
// Accepted forms (case-insensitive keywords optional):
//   33.7490, -84.3880
//   33.7490 -84.3880
//   lat:33.7490 lon:-84.3880
//   lat=33.7490, lon=-84.3880
//   LOC 33.7490 -84.3880
//   LOC:33.7490,-84.3880
// Rejects out-of-range values and ambiguous multi-pair blobs.
bool tryParse(const char* text, size_t len, double& latOut, double& lonOut);

inline bool tryParse(const char* text, double& latOut, double& lonOut) {
  if (!text) return false;
  size_t n = 0;
  while (text[n]) n++;
  return tryParse(text, n, latOut, lonOut);
}

}  // namespace LocationParse
