// LocationParse — extract one lat/lon pair from message text.

#include "util/LocationParse.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace LocationParse {

static bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char* skipSpace(const char* p, const char* end) {
  while (p < end && isSpace(*p)) p++;
  return p;
}

// Case-insensitive match of keyword at p; returns past keyword or nullptr.
static const char* matchKey(const char* p, const char* end, const char* key) {
  size_t klen = strlen(key);
  if ((size_t)(end - p) < klen) return nullptr;
  for (size_t i = 0; i < klen; i++) {
    char a = p[i];
    char b = key[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return nullptr;
  }
  return p + klen;
}

static bool parseFloat(const char*& p, const char* end, double& out) {
  p = skipSpace(p, end);
  if (p >= end) return false;
  char* stop = nullptr;
  // strtod needs a C string; copy a bounded slice
  char buf[48];
  size_t n = 0;
  const char* s = p;
  if (*s == '+' || *s == '-') {
    buf[n++] = *s++;
  }
  bool sawDigit = false;
  bool sawDot = false;
  while (s < end && n + 1 < sizeof(buf)) {
    char c = *s;
    if (c >= '0' && c <= '9') {
      sawDigit = true;
      buf[n++] = c;
      s++;
    } else if (c == '.' && !sawDot) {
      sawDot = true;
      buf[n++] = c;
      s++;
    } else {
      break;
    }
  }
  if (!sawDigit) return false;
  buf[n] = 0;
  out = strtod(buf, &stop);
  if (stop == buf) return false;
  p = s;
  return true;
}

static bool validPair(double lat, double lon) {
  if (lat < -90.0 || lat > 90.0) return false;
  if (lon < -180.0 || lon > 180.0) return false;
  // Reject (0,0) as almost always noise for this product
  if (lat == 0.0 && lon == 0.0) return false;
  // Require some precision (not integers that look like years/counts alone)
  // Allow integers if both look geographic (e.g. 34, -84)
  return true;
}

// Try keyword form: lat ... lon ...
static bool tryKeyword(const char* text, size_t len, double& latOut, double& lonOut) {
  const char* end = text + len;
  const char* p = text;
  while (p < end) {
    const char* latK = matchKey(p, end, "lat");
    if (!latK) {
      p++;
      continue;
    }
    latK = skipSpace(latK, end);
    if (latK < end && (*latK == ':' || *latK == '=')) latK++;
    double lat = 0, lon = 0;
    const char* q = latK;
    if (!parseFloat(q, end, lat)) {
      p++;
      continue;
    }
    q = skipSpace(q, end);
    if (q < end && (*q == ',' || *q == ';')) q++;
    q = skipSpace(q, end);
    const char* lonK = matchKey(q, end, "lon");
    if (!lonK) lonK = matchKey(q, end, "lng");
    if (!lonK) {
      p++;
      continue;
    }
    lonK = skipSpace(lonK, end);
    if (lonK < end && (*lonK == ':' || *lonK == '=')) lonK++;
    if (!parseFloat(lonK, end, lon)) {
      p++;
      continue;
    }
    if (validPair(lat, lon)) {
      latOut = lat;
      lonOut = lon;
      return true;
    }
    p++;
  }
  return false;
}

// LOC prefix then two floats
static bool tryLocPrefix(const char* text, size_t len, double& latOut, double& lonOut) {
  const char* end = text + len;
  const char* p = skipSpace(text, end);
  const char* after = matchKey(p, end, "loc");
  if (!after) return false;
  after = skipSpace(after, end);
  if (after < end && (*after == ':' || *after == '=')) after++;
  double lat = 0, lon = 0;
  if (!parseFloat(after, end, lat)) return false;
  after = skipSpace(after, end);
  if (after < end && (*after == ',' || *after == ';')) after++;
  if (!parseFloat(after, end, lon)) return false;
  if (!validPair(lat, lon)) return false;
  latOut = lat;
  lonOut = lon;
  return true;
}

// Bare two floats (optionally comma-separated), whole message or first line
static bool tryBarePair(const char* text, size_t len, double& latOut, double& lonOut) {
  // Use first line only for bare pair to avoid false positives in long chat
  size_t lineLen = len;
  for (size_t i = 0; i < len; i++) {
    if (text[i] == '\n' || text[i] == '\r') {
      lineLen = i;
      break;
    }
  }
  const char* end = text + lineLen;
  const char* p = skipSpace(text, end);
  double lat = 0, lon = 0;
  if (!parseFloat(p, end, lat)) return false;
  p = skipSpace(p, end);
  if (p < end && (*p == ',' || *p == ';')) p++;
  if (!parseFloat(p, end, lon)) return false;
  p = skipSpace(p, end);
  // Rest of line must be empty or a simple unit suffix
  if (p < end) {
    // allow trailing "N" "W" style? skip for MVP — require end
    return false;
  }
  if (!validPair(lat, lon)) return false;
  // Heuristic: lat magnitude usually < 90 already checked; prefer |lat|<=85
  // and |lon| often larger for US — no extra filter
  latOut = lat;
  lonOut = lon;
  return true;
}

bool tryParse(const char* text, size_t len, double& latOut, double& lonOut) {
  if (!text || len == 0) return false;
  // Cap scan length
  if (len > 512) len = 512;

  if (tryKeyword(text, len, latOut, lonOut)) return true;
  if (tryLocPrefix(text, len, latOut, lonOut)) return true;
  if (tryBarePair(text, len, latOut, lonOut)) return true;
  return false;
}

}  // namespace LocationParse
