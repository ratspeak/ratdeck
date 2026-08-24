#include "util/CoordFormat.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace CoordFormat {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;

static double clampLat(double lat) {
  if (lat > 90.0) return 90.0;
  if (lat < -90.0) return -90.0;
  return lat;
}

static double clampLon(double lon) {
  // Normalize to (-180, 180]
  while (lon > 180.0) lon -= 360.0;
  while (lon <= -180.0) lon += 360.0;
  return lon;
}

static void hemi(double lat, double lon, char& ns, char& ew, double& alat,
                 double& alon) {
  ns = (lat >= 0.0) ? 'N' : 'S';
  ew = (lon >= 0.0) ? 'E' : 'W';
  alat = fabs(lat);
  alon = fabs(lon);
}

// ---- Open Location Code (Plus Code) encode, length 11 --------------------
// Alphabet and pair encoding match the public OLC spec (Google).

static const char kOlcAlphabet[] = "23456789CFGHJMPQRVWX";
static constexpr int kOlcCodeLen = 11;
static constexpr int kOlcPairCodeLen = 10;
static constexpr double kOlcLatMax = 90.0;
static constexpr double kOlcLonMax = 180.0;
static constexpr double kOlcGridRows = 5.0;
static constexpr double kOlcGridCols = 4.0;

static size_t encodePlusCode(char* buf, size_t n, double lat, double lon) {
  if (!buf || n < 13) return 0;  // 11 + '+' + NUL worst case layout

  lat = clampLat(lat);
  lon = clampLon(lon);
  // OLC uses [0,180) lat and [0,360) lon after offset.
  double latVal = lat + kOlcLatMax;
  double lonVal = lon + kOlcLonMax;
  // Clip lat just below 180 so it stays in range.
  if (latVal >= 2.0 * kOlcLatMax) latVal = 2.0 * kOlcLatMax - 1e-10;

  char code[16];
  memset(code, 0, sizeof(code));

  double pairRes = 20.0;  // initial pair resolution (degrees)
  int idx = 0;
  for (int i = 0; i < kOlcPairCodeLen / 2; i++) {
    pairRes /= 20.0;
    int latDigit = (int)(latVal / pairRes);
    int lonDigit = (int)(lonVal / pairRes);
    if (latDigit > 19) latDigit = 19;
    if (lonDigit > 19) lonDigit = 19;
    code[idx++] = kOlcAlphabet[latDigit];
    code[idx++] = kOlcAlphabet[lonDigit];
    latVal -= latDigit * pairRes;
    lonVal -= lonDigit * pairRes;
  }

  // Final grid refinement for code length 11 (one extra char).
  if (kOlcCodeLen > kOlcPairCodeLen) {
    double rowSize = pairRes / kOlcGridRows;
    double colSize = pairRes / kOlcGridCols;
    int row = (int)(latVal / rowSize);
    int col = (int)(lonVal / colSize);
    if (row > 4) row = 4;
    if (col > 3) col = 3;
    code[idx++] = kOlcAlphabet[row * (int)kOlcGridCols + col];
  }

  // Insert '+' after 8 characters.
  char out[16];
  int o = 0;
  for (int i = 0; i < idx; i++) {
    if (i == 8) out[o++] = '+';
    out[o++] = code[i];
  }
  out[o] = '\0';

  if ((size_t)o + 1 > n) return 0;
  memcpy(buf, out, (size_t)o + 1);
  return (size_t)o;
}

// ---- UTM (WGS84) ---------------------------------------------------------

static char utmBand(double lat) {
  static const char bands[] = "CDEFGHJKLMNPQRSTUVWX";
  if (lat < -80.0 || lat > 84.0) return 'Z';  // polar — not standard UTM
  int i = (int)((lat + 80.0) / 8.0);
  if (i < 0) i = 0;
  if (i > 19) i = 19;
  return bands[i];
}

static int utmZone(double lat, double lon) {
  int zone = (int)((lon + 180.0) / 6.0) + 1;
  // Norway special
  if (lat >= 56.0 && lat < 64.0 && lon >= 3.0 && lon < 12.0) zone = 32;
  // Svalbard specials
  if (lat >= 72.0 && lat < 84.0) {
    if (lon >= 0.0 && lon < 9.0) zone = 31;
    else if (lon >= 9.0 && lon < 21.0) zone = 33;
    else if (lon >= 21.0 && lon < 33.0) zone = 35;
    else if (lon >= 33.0 && lon < 42.0) zone = 37;
  }
  if (zone < 1) zone = 1;
  if (zone > 60) zone = 60;
  return zone;
}

static void latLonToUtm(double lat, double lon, int& zone, char& band,
                        long& easting, long& northing) {
  zone = utmZone(lat, lon);
  band = utmBand(lat);

  constexpr double a = 6378137.0;             // WGS84
  constexpr double f = 1.0 / 298.257223563;
  constexpr double k0 = 0.9996;
  const double e2 = f * (2.0 - f);
  const double ep2 = e2 / (1.0 - e2);

  const double latRad = lat * kDeg2Rad;
  const double lonRad = lon * kDeg2Rad;
  const double lon0 = ((zone - 1) * 6 - 180 + 3) * kDeg2Rad;

  const double N = a / sqrt(1.0 - e2 * sin(latRad) * sin(latRad));
  const double T = tan(latRad) * tan(latRad);
  const double C = ep2 * cos(latRad) * cos(latRad);
  const double A = cos(latRad) * (lonRad - lon0);

  const double M =
      a *
      ((1.0 - e2 / 4.0 - 3.0 * e2 * e2 / 64.0 - 5.0 * e2 * e2 * e2 / 256.0) *
           latRad -
       (3.0 * e2 / 8.0 + 3.0 * e2 * e2 / 32.0 + 45.0 * e2 * e2 * e2 / 1024.0) *
           sin(2.0 * latRad) +
       (15.0 * e2 * e2 / 256.0 + 45.0 * e2 * e2 * e2 / 1024.0) *
           sin(4.0 * latRad) -
       (35.0 * e2 * e2 * e2 / 3072.0) * sin(6.0 * latRad));

  double x =
      k0 * N *
          (A + (1.0 - T + C) * A * A * A / 6.0 +
           (5.0 - 18.0 * T + T * T + 72.0 * C - 58.0 * ep2) * A * A * A * A *
               A / 120.0) +
      500000.0;
  double y =
      k0 *
      (M +
       N * tan(latRad) *
           (A * A / 2.0 + (5.0 - T + 9.0 * C + 4.0 * C * C) * A * A * A * A /
                              24.0 +
            (61.0 - 58.0 * T + T * T + 600.0 * C - 330.0 * ep2) * A * A * A *
                A * A * A / 720.0));
  if (lat < 0.0) y += 10000000.0;

  easting = (long)lround(x);
  northing = (long)lround(y);
}

}  // namespace

size_t formatDD(char* buf, size_t n, double lat, double lon) {
  if (!buf || n < 8) return 0;
  char ns, ew;
  double alat, alon;
  hemi(lat, lon, ns, ew, alat, alon);
  int w = snprintf(buf, n, "%.6f %c, %.6f %c", alat, ns, alon, ew);
  if (w < 0 || (size_t)w >= n) return 0;
  return (size_t)w;
}

size_t formatDM(char* buf, size_t n, double lat, double lon) {
  if (!buf || n < 8) return 0;
  char ns, ew;
  double alat, alon;
  hemi(lat, lon, ns, ew, alat, alon);

  int latD = (int)alat;
  double latM = (alat - latD) * 60.0;
  int lonD = (int)alon;
  double lonM = (alon - lonD) * 60.0;

  int w = snprintf(buf, n, "%d\xC2\xB0 %07.4f' %c, %d\xC2\xB0 %07.4f' %c",
                   latD, latM, ns, lonD, lonM, ew);
  if (w < 0 || (size_t)w >= n) return 0;
  return (size_t)w;
}

size_t formatDMS(char* buf, size_t n, double lat, double lon) {
  if (!buf || n < 8) return 0;
  char ns, ew;
  double alat, alon;
  hemi(lat, lon, ns, ew, alat, alon);

  auto split = [](double absDeg, int& d, int& m, double& s) {
    d = (int)absDeg;
    double mf = (absDeg - d) * 60.0;
    m = (int)mf;
    s = (mf - m) * 60.0;
    // Carry rounding
    if (s >= 59.95) {
      s = 0.0;
      m += 1;
    }
    if (m >= 60) {
      m = 0;
      d += 1;
    }
  };

  int latD, latM, lonD, lonM;
  double latS, lonS;
  split(alat, latD, latM, latS);
  split(alon, lonD, lonM, lonS);

  int w = snprintf(buf, n,
                   "%d\xC2\xB0 %02d' %04.1f\" %c, %d\xC2\xB0 %02d' %04.1f\" %c",
                   latD, latM, latS, ns, lonD, lonM, lonS, ew);
  if (w < 0 || (size_t)w >= n) return 0;
  return (size_t)w;
}

size_t formatPlusCode(char* buf, size_t n, double lat, double lon) {
  return encodePlusCode(buf, n, lat, lon);
}

size_t formatUTM(char* buf, size_t n, double lat, double lon) {
  if (!buf || n < 8) return 0;
  lat = clampLat(lat);
  lon = clampLon(lon);
  if (lat < -80.0 || lat > 84.0) {
    int w = snprintf(buf, n, "(polar — use DD)");
    if (w < 0 || (size_t)w >= n) return 0;
    return (size_t)w;
  }
  int zone;
  char band;
  long e, nn;
  latLonToUtm(lat, lon, zone, band, e, nn);
  int w = snprintf(buf, n, "%d%c %ld %ld", zone, band, e, nn);
  if (w < 0 || (size_t)w >= n) return 0;
  return (size_t)w;
}

}  // namespace CoordFormat
