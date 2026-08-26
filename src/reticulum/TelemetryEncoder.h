// =============================================================================
// TelemetryEncoder.h — pure msgpack builders for Sideband-compatible
// Telemeter snapshot + LXMF FIELD_TELEMETRY wrapping.
//
// Stage 1 (rsDeck #64 / thin hybrid): the snapshot values come from the
// rsDeck HAL (GPS + Power); the shape and field codes match Sideband's
// Telemeter.packed() (sbapp/sideband/sense.py). All functions are pure
// and have no Reticulum / microReticulum dependency — easy to unit test
// on host.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace TelemetryEncoder {

// --- Sideband sensor IDs (sbapp/sideband/sense.py) ---
constexpr uint8_t SID_TIME         = 0x01;
constexpr uint8_t SID_LOCATION     = 0x02;
constexpr uint8_t SID_BATTERY      = 0x04;

// --- LXMF field key (reticulum-specifications SPEC.md §5.9.1) ---
constexpr uint8_t FIELD_TELEMETRY  = 0x02;

// Build the Sideband Telemeter snapshot msgpack map bytes.
//
// Fields are emitted only when data is sane:
//   SID_TIME        → uint UNIX seconds (or uptime seconds if unavailable)
//   SID_LOCATION    → 7-element array of msgpack bins wrapping big-endian
//                     struct ints (matches sbapp struct.pack("!i"/"!I"/"!H"))
//   SID_BATTERY     → [percent_or_voltage_f64, charging_bool, nil]
//
// Any of lat/lon/alt/speed/bearing/accuracy_m may be NaN to omit the
// SID_LOCATION entry (used by the privacy gate when there is no fresh
// GPS fix). Functions take simple doubles — callers convert from the
// rsDeck units (degrees / metres / m·s⁻¹).
//
// `include_time` (default true) controls whether the SID_TIME key/value
// pair is emitted. Callers that need to fit a single-frame LoRa budget
// can pass false as a fallback after dropping optional sensors — the
// SID_LOCATION entry carries its own `last_update_s` so the collector
// still gets a usable timestamp.
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
                                   bool include_time = true);

// Wrap the snapshot bytes as the LXMF FIELD_TELEMETRY value inside a
// one-entry fields map:
//
//     msgpack { 0x02: bin(snapshot) }
//
// That fields map is the value `LXMFMessage.packed()` puts in slot [3] of
// the outer [timestamp, title, content, fields] array.
//
// `snapshot` may be empty — in that case the fields map is still emitted
// with `0x02 -> bin "" ` so the message structure stays valid even when
// the privacy gate stripped every optional sensor.
std::vector<uint8_t> buildFieldsBin(const std::vector<uint8_t>& snapshot);

} // namespace TelemetryEncoder
