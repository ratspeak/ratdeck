// =============================================================================
// TelemetryEncoder.cpp — see TelemetryEncoder.h. Hand-rolled msgpack writer
// tuned for the few LXMF / Telemeter shapes rsDeck needs in Stage 1.
//
// Mirrors the writer used by the RLR/Sideband telemetry reference (see
// /tmp/opencode/rak/reticulum-lora-repeater/src/Msgpack.cpp) and emits
// the canonical big-endian envelopes Sideband signs against.
//
// We only encode the shapes that show up in the Stage 1 Telemeter
// snapshot — there is no need to drag in a full msgpack dependency for
// three map entries.
// =============================================================================
#include "TelemetryEncoder.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace TelemetryEncoder {
namespace {

// ------------------------------------------------------------------
//   Big-endian byte writers
// ------------------------------------------------------------------
inline void put_u8 (std::vector<uint8_t>& b, uint8_t  v) { b.push_back(v); }
inline void put_be16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)(v));
}
inline void put_be32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 24));
    b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
}
inline void put_be64(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
}

// ------------------------------------------------------------------
//   Msgpack scalar/structure emitters (canonical smallest-envelope)
// ------------------------------------------------------------------
void emit_uint(std::vector<uint8_t>& b, uint64_t v) {
    if (v < 0x80) {
        b.push_back((uint8_t)v);                          // positive fixint
    } else if (v <= 0xff) {
        b.push_back(0xcc); b.push_back((uint8_t)v);       // uint8
    } else if (v <= 0xffff) {
        b.push_back(0xcd); put_be16(b, (uint16_t)v);      // uint16
    } else if (v <= 0xffffffffULL) {
        b.push_back(0xce); put_be32(b, (uint32_t)v);      // uint32
    } else {
        b.push_back(0xcf); put_be64(b, v);                // uint64
    }
}

void emit_fixarray(std::vector<uint8_t>& b, size_t n) {
    if (n < 16) {
        b.push_back((uint8_t)(0x90 | (n & 0x0f)));        // fixarray
    } else if (n < 0x10000) {
        b.push_back(0xdc); put_be16(b, (uint16_t)n);      // array16
    } else {
        b.push_back(0xdd); put_be32(b, (uint32_t)n);      // array32
    }
}

void emit_fixmap(std::vector<uint8_t>& b, size_t n) {
    if (n < 16) {
        b.push_back((uint8_t)(0x80 | (n & 0x0f)));        // fixmap
    } else if (n < 0x10000) {
        b.push_back(0xde); put_be16(b, (uint16_t)n);      // map16
    } else {
        b.push_back(0xdf); put_be32(b, (uint32_t)n);      // map32
    }
}

void emit_nil(std::vector<uint8_t>& b)            { b.push_back(0xc0); }
void emit_bool(std::vector<uint8_t>& b, bool v)   { b.push_back(v ? 0xc3 : 0xc2); }

void emit_float64(std::vector<uint8_t>& b, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    b.push_back(0xcb);
    put_be64(b, bits);
}

void emit_bin(std::vector<uint8_t>& b, const uint8_t* data, size_t len) {
    if (len < 0x100) {
        b.push_back(0xc4); b.push_back((uint8_t)len);    // bin8
    } else if (len < 0x10000) {
        b.push_back(0xc5); put_be16(b, (uint16_t)len);   // bin16
    } else {
        b.push_back(0xc6); put_be32(b, (uint32_t)len);   // bin32
    }
    if (len) b.insert(b.end(), data, data + len);
}

// Emit a big-endian struct int wrapped as a msgpack bin — matches the
// upstream Python `sbapp/sideband/sense.py` which wraps struct.pack
// output as a Python bytes (msgpack bin).
void emit_be32_bin(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t tmp[4] = {
        (uint8_t)(v >> 24), (uint8_t)(v >> 16),
        (uint8_t)(v >> 8),  (uint8_t)v
    };
    emit_bin(b, tmp, sizeof(tmp));
}
void emit_be16_bin(std::vector<uint8_t>& b, uint16_t v) {
    uint8_t tmp[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    emit_bin(b, tmp, sizeof(tmp));
}

// NaN-aware "double is sane" check — the rsDeck GPS HAL exposes the
// current lat/lon as raw doubles and we refuse if they're stale, so the
// privacy gate passes a real 0.0 (== "no location" in Sideband sense)
// by leaving the value as NaN here. This isolates the encoder from the
// gating logic.
inline bool is_sane(double v) {
    return !std::isnan(v) && !std::isinf(v);
}

} // anonymous namespace

// ------------------------------------------------------------------
//   Public API
// ------------------------------------------------------------------

std::vector<uint8_t> buildSnapshot(double lat_deg,
                                   double lon_deg,
                                   double alt_m,
                                   double speed_mps,
                                   double bearing_deg,
                                   double accuracy_m,
                                   uint32_t last_update_s,
                                   uint64_t time_sid,
                                   double battery_val,
                                   bool battery_charging,
                                   bool include_time) {
    // Count present sensors first so we can emit the correct map header
    // size — Sideband/umsgpack default-size maps (0x80..0x8f) round-trip
    // the receiver's iter order the same way, but the LYRA collector grid
    // is happier with explicit count.
    size_t n = include_time ? 1 : 0;
    const bool have_loc = is_sane(lat_deg) && is_sane(lon_deg);
    const bool have_bat = is_sane(battery_val);
    if (have_loc) n++;
    if (have_bat) n++;

    std::vector<uint8_t> b;
    b.reserve(40 + (have_loc ? 32 : 0) + (have_bat ? 16 : 0));
    emit_fixmap(b, n);

    // SID_TIME — Unix epoch seconds when GPS is valid; uptime as fallback.
    // May be omitted when callers need to fit the encrypted packet into a
    // single-frame LoRa budget (the SID_LOCATION entry carries its own
    // `last_update_s` so the collector still has a usable timestamp).
    if (include_time) {
        emit_uint(b, SID_TIME);
        emit_uint(b, time_sid);
    }

    if (have_loc) {
        // SID_LOCATION → [lat_be32, lon_be32, alt_be32, speed_be32,
        //                  bearing_be32, accuracy_be16, last_update_uint]
        emit_uint(b, SID_LOCATION);
        emit_fixarray(b, 7);

        // lat / lon are stored as int32 of (deg * 1e6), per Sideband sense.py
        const int32_t lat_udeg = (int32_t)lround(lat_deg  * 1e6);
        const int32_t lon_udeg = (int32_t)lround(lon_deg  * 1e6);
        emit_be32_bin(b, (uint32_t)lat_udeg);
        emit_be32_bin(b, (uint32_t)lon_udeg);

        // alt_m × 100 as int32 (one centimetre). NaN/inf are filtered by
        // the speed/bearing check above; here they reach us as 0.0 when
        // GPS has no altitude fix — Sideband renders 0 m as "unknown".
        if (is_sane(alt_m)) {
            const int32_t alt_cm = (int32_t)lround(alt_m * 100.0);
            emit_be32_bin(b, (uint32_t)alt_cm);
        } else {
            emit_be32_bin(b, 0);
        }

        // speed_mps × 100 as int32 — 0 if unavailable (no live speed on the
        // rsDeck's MIA-M10Q unless the user enables course-over-ground NMEA).
        if (is_sane(speed_mps)) {
            const int32_t speed_cms = (int32_t)lround(speed_mps * 100.0);
            emit_be32_bin(b, (uint32_t)speed_cms);
        } else {
            emit_be32_bin(b, 0);
        }

        // bearing_deg × 100 — 0 if unavailable.
        if (is_sane(bearing_deg)) {
            const int32_t bearing_cdeg = (int32_t)lround(bearing_deg * 100.0);
            emit_be32_bin(b, (uint32_t)bearing_cdeg);
        } else {
            emit_be32_bin(b, 0);
        }

        // accuracy: HDOP-derived metres × 100, 0 if unknown. Collectors
        // treat 0 as "do not draw accuracy circle".
        if (is_sane(accuracy_m)) {
            const uint16_t accuracy_cm = (uint16_t)lround(accuracy_m * 100.0);
            emit_be16_bin(b, accuracy_cm);
        } else {
            emit_be16_bin(b, 0);
        }

        emit_uint(b, last_update_s);
    }

    if (have_bat) {
        // SID_BATTERY → [charge_percent_or_voltage_f64, charging_bool, nil]
        // The (f64, bool, nil) triple matches RLR/Sideband shape — the
        // third slot is reserved for temperature and left nil here since
        // the rsDeck has no battery thermistor exposed.
        emit_uint(b, SID_BATTERY);
        emit_fixarray(b, 3);
        emit_float64(b, battery_val);
        emit_bool(b, battery_charging);
        emit_nil(b);
    }

    return b;
}

std::vector<uint8_t> buildFieldsBin(const std::vector<uint8_t>& snapshot) {
    std::vector<uint8_t> b;
    b.reserve(8 + snapshot.size());
    emit_fixmap(b, 1);
    emit_uint(b, FIELD_TELEMETRY);
    emit_bin(b, snapshot.empty() ? nullptr : snapshot.data(), snapshot.size());
    return b;
}

} // namespace TelemetryEncoder
