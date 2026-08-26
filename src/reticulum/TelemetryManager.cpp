// =============================================================================
// TelemetryManager.cpp — see TelemetryManager.h.
//
// Stage 1 rsDeck #64 (thin hybrid): on-demand + optional periodic
// opportunistic LXMF push of a Sideband-shaped FIELD_TELEMETRY value to
// a configured collector.
//
// Critical constraints (per the issue brief):
//   * Two triggers: serial `g` (on-demand) and a periodic tick driven
//     by UserSettings::gpsTelemetryIntervalS (issue #64 "configurable by
//     interval" privacy requirement). 0 = periodic disabled (default).
//   * Privacy gate refuses if no fresh fix (≤ FRESH_FIX_MAX_AGE_MS old)
//     for both triggers — periodic must never force-send with a stale
//     or absent fix; the periodic tick just skips and retries on the
//     next interval.
//   * No edits to LXMFManager.{h,cpp} — the chat path is untouched.
//   * No edits to the microReticulum library — the sign + wrap is a
//     clone of LXMFMessage::packFull() from upstream, kept inline here
//     because packContent() in this fork does not yet expose the
//     `fields` map slot.
//
// TODO: DELETE the inline signAndWrap once LXMFMessage::packContent
// grows a `fields` arg — until then we reproduce the wire layout
// exactly (see microReticulum/src/LXMFMessage.cpp ~lines 103-136).
// =============================================================================

#include "TelemetryManager.h"

#include <Arduino.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <Destination.h>
#include <Identity.h>
#include <Packet.h>
#include <Transport.h>

#include "config/BoardConfig.h"
#include "config/UserConfig.h"
#include "ReticulumManager.h"
#if HAS_GPS
#include "hal/GPSManager.h"
#endif
#include "hal/Power.h"

// =============================================================================
// Construction / wiring
// =============================================================================

TelemetryManager::TelemetryManager() = default;

void TelemetryManager::begin(ReticulumManager* rns, GPSManager* gps, Power* power) {
    _rns   = rns;
    _gps   = gps;
    _power = power;
    // If the bound UserConfig has a valid hub hash, override the
    // compiled-in default — the manager still works standalone
    // (with its baked-in Lyra collector default) if UserConfig is
    // never wired.
    applyUserConfigHubHash();
    loadHubHashIfNeeded();
    Serial.printf("[TELEMETRY] default hub=%s\n", _hubHex);
}

void TelemetryManager::applyUserConfigHubHash() {
    if (!_cfg) return;
    const String& fromCfg = _cfg->settings().gpsTelemetryHubHash;
    if (fromCfg.isEmpty()) return;
    if (fromCfg.length() == sizeof(_hubHex) - 1) {
        // setHubHashHex validates hex + length; bail silently if the
        // stored value is malformed (sanitizeSettings should already
        // have caught that, but be defensive).
        setHubHashHex(fromCfg.c_str());
    }
}

void TelemetryManager::loadHubHashIfNeeded() {
    if (_hubHashLoaded) return;
    _hubHash.assignHex(_hubHex);
    if (_hubHash.size() != 16) {
        Serial.printf("[TELEMETRY] WARN: hub hex did not decode to 16 bytes (got %u)\n",
                      (unsigned)_hubHash.size());
        _hubHash.clear();
    }
    _hubHashLoaded = true;
}

// =============================================================================
// Public API
// =============================================================================

bool TelemetryManager::sendNow() {
    if (_state != State::IDLE) {
        Serial.println("[TELEMETRY] sendNow refused: attempt already in flight");
        return false;
    }
    if (!_rns || !_gps || !_power) {
        Serial.println("[TELEMETRY] sendNow refused: subsystems not bound");
        return false;
    }

    // --- Privacy gate (rsDeck #64 specification) ---
    // User opt-in must be enabled in Settings > Time & Location >
    // "Send Telemetry". Off by default — refuse loudly so the user
    // gets an obvious hint pointing at the setting.
    if (_cfg && !_cfg->settings().gpsTelemetryEnabled) {
        Serial.println("[TELEMETRY] refused: GPS telemetry disabled in Settings > Time & Location > Send Telemetry");
        return false;
    }

    // Re-pull the hub hash from UserConfig on every send so live edits
    // in the Settings UI take effect without a callback. If the bound
    // UserConfig is empty/malformed, fall back to whatever is already
    // baked into _hubHex (the compiled-in default, or a previously
    // applied value).
    if (_cfg) {
        const String& h = _cfg->settings().gpsTelemetryHubHash;
        if (h.length() == sizeof(_hubHex) - 1) {
            // Cheap string compare against current hub — skip the
            // log spam of setHubHashHex when the value is unchanged.
            if (strncmp(_hubHex, h.c_str(), sizeof(_hubHex) - 1) != 0) {
                setHubHashHex(h.c_str());
            }
        }
    }

    loadHubHashIfNeeded();
    if (_hubHash.size() != 16) {
        Serial.println("[TELEMETRY] sendNow refused: hub hash unset/invalid");
        return false;
    }

    const LocSample loc = readLocation();
    if (!loc.valid) {
        // Distinguish "no fix at all" vs "fix exists but too old" for
        // the diagnostic counters. Log message kept identical to
        // existing behavior (the combined !loc.valid check catches
        // both cases at the same point). The counters are purely
        // additive — no change to gating decision.
#if HAS_GPS
        if (_gps && _gps->hasLocationFix()) _refusedStale++;
        else                                _refusedNoFix++;
#else
        _refusedNoFix++;
#endif
        Serial.println("[TELEMETRY] refused: no GPS fix");
        return false;
    }
    if (loc.age_ms > FRESH_FIX_MAX_AGE_MS) {
        _refusedStale++;
        Serial.printf("[TELEMETRY] refused: GPS fix stale (%lu ms > %lu ms)\n",
                      (unsigned long)loc.age_ms,
                      (unsigned long)FRESH_FIX_MAX_AGE_MS);
        return false;
    }
    if (!_rns->isTransportActive()) {
        Serial.println("[TELEMETRY] refused: Reticulum transport inactive");
        return false;
    }

    // Start the discovery state machine. We don't try to pack & send in
    // the same call — that lets loop() handle the rare case where
    // Identity::recall() returns false the first time after a reset.
    Serial.printf("[TELEMETRY] requested send to %s (fix=%lu ms)\n",
                  _hubHex, (unsigned long)loc.age_ms);
    _state = State::WAIT_IDENTITY;
    _attempts = 0;
    _stateAtMs = millis();
    return true;
}

bool TelemetryManager::hasFreshFix() const {
    if (!_gps) return false;
#if HAS_GPS
    if (!_gps->hasLocationFix()) return false;
    return _gps->fixAgeMs() <= FRESH_FIX_MAX_AGE_MS;
#else
    return false;
#endif
}

void TelemetryManager::_logPeriodicSkipOnce(const char* reason) {
    if (_periodicSkipLogged) return;
    _periodicSkipLogged = true;
    Serial.printf("[TELEMETRY] periodic disabled: %s\n", reason);
}

void TelemetryManager::checkPeriodicSend() {
    // Configuration gate. All four conditions must hold for a periodic
    // tick to fire:
    //   1. UserConfig is bound (so we have a live opt-in / interval).
    //   2. gpsTelemetryEnabled is true (master opt-in switch).
    //   3. gpsTelemetryIntervalS > 0 (0 = on-demand only; periodic must
    //      never start silently just because the user enabled the
    //      toggle).
    //   4. Enough time has elapsed since the last periodic attempt.
    // sendNow() re-reads _cfg->settings() on every call and enforces its
    // own privacy gate (fresh fix ≤ FRESH_FIX_MAX_AGE_MS); we do NOT
    // re-implement that gate here so the two paths cannot drift.
    if (!_cfg) {
        _logPeriodicSkipOnce("cfg not bound");
        return;
    }
    const UserSettings& s = _cfg->settings();
    if (!s.gpsTelemetryEnabled) {
        _logPeriodicSkipOnce("gpsTelemetryEnabled=false");
        return;
    }
    const int intervalS = s.gpsTelemetryIntervalS;
    if (intervalS <= 0) {
        _logPeriodicSkipOnce("gpsTelemetryIntervalS<=0 (Off)");
        return;
    }
    _periodicSkipLogged = false; // config is valid; re-arm the one-shot skip log

    const unsigned long intervalMs = (unsigned long)intervalS * 1000UL;
    if (millis() - _lastPeriodicSendMs < intervalMs) return;

    // Advance the timer *before* sendNow() so a refused send (e.g. fix
    // not fresh, transport inactive) does not retry on every loop()
    // iteration — next periodic attempt happens at the configured
    // cadence regardless of refusal reason. sendNow() logs its own
    // refusal reason, so the serial console still surfaces why the
    // periodic tick was skipped without becoming a per-loop() spam.
    _lastPeriodicSendMs = millis();
    _ticksFired++;

    Serial.printf("[TELEMETRY] periodic tick firing (interval=%ds)\n", intervalS);
    sendNow();
}

const char* TelemetryManager::stateName() const {
    switch (_state) {
        case State::IDLE:           return "IDLE";
        case State::WAIT_IDENTITY:  return "WAIT_IDENTITY";
        case State::WAIT_PATH:      return "WAIT_PATH";
        case State::SENDING:        return "SENDING";
    }
    return "?";
}

// =============================================================================
// State machine (driven from main loop)
// =============================================================================

void TelemetryManager::loop() {
    // Periodic send tick runs only while the state machine is IDLE — an
    // in-flight on-demand send (or a periodic send that just started)
    // must not have another periodic attempt piled on top of it. The
    // check itself is gated on the live UserConfig so toggling
    // gpsTelemetryEnabled off or changing the interval takes effect
    // immediately at the next loop() tick without needing a callback.
    if (_state == State::IDLE) {
        checkPeriodicSend();
        return;
    }
    // Unsigned-subtraction idiom matches LXMFManager — safe as long as
    // STATE_RETRY_INTERVAL_MS is small relative to millis() wrap
    // (~49 days).
    if (millis() - _stateAtMs < STATE_RETRY_INTERVAL_MS) return;
    _stateAtMs = millis();

    switch (_state) {
        case State::IDLE:
            return;
        case State::WAIT_IDENTITY:
            driveIdentityDiscovery();
            return;
        case State::WAIT_PATH:
            drivePathDiscovery();
            return;
        case State::SENDING:
            // One-shot: buildAndSendSnapshot runs synchronously inside
            // drivePathDiscovery() before flipping state back to IDLE.
            return;
    }
}

void TelemetryManager::driveIdentityDiscovery() {
    _attempts++;
    RNS::Identity recipient = RNS::Identity::recall(_hubHash);
    if (!recipient) {
        Serial.printf("[TELEMETRY] no identity yet for %s — requesting path (attempt %u/%u)\n",
                      _hubHex, (unsigned)_attempts, (unsigned)DISCOVERY_MAX_ATTEMPTS);
        RNS::Transport::request_path(_hubHash);
        if (_attempts >= DISCOVERY_MAX_ATTEMPTS) {
            resetToIdle("no identity after retries");
        }
        return;
    }
    Serial.printf("[TELEMETRY] identity recalled (%s) for %s\n",
                  recipient.hexhash().c_str(), _hubHex);

    // Advance to the path check. _stateAtMs=0 lets the next loop tick
    // re-enter within the same iteration budget.
    _state = State::WAIT_PATH;
    _attempts = 0;
    _stateAtMs = millis() - STATE_RETRY_INTERVAL_MS;
    RNS::Transport::request_path(_hubHash);
}

void TelemetryManager::drivePathDiscovery() {
    _attempts++;
    if (!RNS::Transport::has_path(_hubHash)) {
        Serial.printf("[TELEMETRY] no path yet — requesting (attempt %u/%u)\n",
                      (unsigned)_attempts, (unsigned)DISCOVERY_MAX_ATTEMPTS);
        RNS::Transport::request_path(_hubHash);
        if (_attempts >= DISCOVERY_MAX_ATTEMPTS) {
            resetToIdle("no path after retries");
        }
        return;
    }
    Serial.printf("[TELEMETRY] path OK (%d hops) — building packet\n",
                  RNS::Transport::hops_to(_hubHash));

    _state = State::SENDING;
    bool ok = false;
    try {
        ok = buildAndSendSnapshot();
    } catch (const std::exception& e) {
        Serial.printf("[TELEMETRY] exception during build/send: %s\n", e.what());
        ok = false;
    }
    if (ok) {
        Serial.println("[TELEMETRY] send complete");
    } else {
        Serial.println("[TELEMETRY] build/send failed");
    }
    _state = State::IDLE;
}

// =============================================================================
// Build + sign + wrap + send
// =============================================================================
//
// The wire layout is identical to what microReticulum's
// LXMFMessage::packFull produces for chat messages, just with the
// optional `fields` map populated instead of empty — but that field is
// not yet reachable in packContent(), so we hand-roll the same steps
// here. See TODO at the top of this file.
//
// DELETE this function when upstream packContent gains a fields arg.
bool TelemetryManager::buildAndSendSnapshot() {
#if !HAS_GPS
    Serial.println("[TELEMETRY] GPS not compiled in — refusing");
    return false;
#else
    if (!_gps || !_rns) return false;

    const LocSample loc = readLocation();
    if (!loc.valid || loc.age_ms > FRESH_FIX_MAX_AGE_MS) {
        Serial.println("[TELEMETRY] re-gate fail: fix no longer fresh");
        return false;
    }

    const RNS::Identity recipient = RNS::Identity::recall(_hubHash);
    if (!recipient) {
        Serial.println("[TELEMETRY] identity vanished mid-send (rare)");
        return false;
    }

    // --- Build the snapshot (Sideband Telemeter shape) ---
    //
    // We may have to shrink the snapshot to fit the encrypted packet
    // inside the LoRa single-frame budget (254 bytes raw). The fallback
    // ladder below tries progressively smaller shapes until one packs
    // under the limit, or refuses to send (split-frame TX is known
    // broken on this 2-hop LoRa path).
    const double battery_pct   = (double)_power->batteryPercent();
    const bool   battery_chrg  = _power->isCharging();
    // HDOP × ~2.5 m is a rough lower bound on horizontal accuracy (DOP
    // multiplies baseline pseudorange error). The MIA-M10Q spec sheet
    // quotes ~1 m CEP under good skies; we just expose what GPS gives.
    const double accuracy_m    = isfinite(_gps->hdop()) ? (_gps->hdop() * 2.5) : NAN;
    const uint32_t now_s        = (uint32_t)epochSecondsOrUptime();
    const uint64_t now_full_s   = (uint64_t)epochSecondsOrUptime();

    const RNS::Identity& self = _rns->identity();
    RNS::Bytes srcHash = RNS::Destination::hash(self, "lxmf", "delivery");
    if (srcHash.size() != 16) {
        Serial.println("[TELEMETRY] unexpected src_hash length");
        return false;
    }

    // RNS::Destination::OUT/SINGLE on a (recipient, "lxmf", "delivery")
    // tuple. Packet::pack() will Token-encrypt the payload to the
    // recipient. We then call packet.send() through Transport which
    // picks the right interface (LoRa, TCP, AutoInterface, ...).
    RNS::Destination outDest(recipient,
                             RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE,
                             "lxmf", "delivery");

    // --- Hand-roll signAndWrap() ---
    //
    // We have to reproduce the upstream LXMFMessage::packFull here
    // because packContent() in this microReticulum fork does not yet
    // accept a fields map. The structure is identical — only the
    // payload array differs in element [3].
    //
    //   packed_content = msgpack [
    //       timestamp_f64,
    //       title_bin  "",         // empty
    //       content_bin "",         // empty
    //       fields_map              // 1 entry: 0x02 → bin(snapshot)
    //   ]
    //
    //   signable   = dest_hash(16) || src_hash(16) || packed_content
    //   msgHash    = SHA256(signable)
    //   signed     = signable || msgHash
    //   signature  = Ed25519(signed)        (64 bytes)
    //   body       = src_hash(16) || signature(64) || packed_content
    //
    // TODO: DELETE when microReticulum packContent gains fields support.

    struct Variant {
        const char* label;
        bool include_time;
        bool include_battery;
    };
    // Order matters: most informative first; smallest last. If even the
    // "location-only" variant overflows, we refuse rather than split.
    const Variant variants[] = {
        { "full",       true,  true  },
        { "no-battery", true,  false },
        { "location",   false, false },
    };

    size_t lastRawLen = 0;
    const char* lastLabel = "?";

    for (const auto& v : variants) {
        const double b_val = v.include_battery ? battery_pct : NAN;

        std::vector<uint8_t> snapshot = TelemetryEncoder::buildSnapshot(
            loc.lat,
            loc.lon,
            loc.alt,
            loc.speed,
            loc.bearing,
            accuracy_m,
            now_s,
            now_full_s,
            b_val,
            battery_chrg,
            v.include_time
        );

        std::vector<uint8_t> fields = TelemetryEncoder::buildFieldsBin(snapshot);
        Serial.printf("[TELEMETRY] [%s] snapshot=%uB fields=%uB\n",
                      v.label, (unsigned)snapshot.size(), (unsigned)fields.size());

        // Build packed_content: fixarray(4) [ts_f64, bin"", bin"", fields_map]
        std::vector<uint8_t> packed;
        packed.reserve(32 + fields.size());
        packed.push_back(0x94);                  // fixarray(4)
        // ts_f64 — prefer real epoch seconds when the system clock is valid,
        // otherwise monotonic uptime (Sideband renders against its own
        // receive clock in either case — this is metadata only).
        {
            uint64_t bits = 0;
            double   ts   = (double)epochSecondsOrUptime();
            memcpy(&bits, &ts, sizeof(bits));
            packed.push_back(0xcb);              // float64 marker
            for (int s = 56; s >= 0; s -= 8) packed.push_back((uint8_t)(bits >> s));
        }
        // title bin "" and content bin "" (bin8 len 0)
        packed.push_back(0xc4); packed.push_back(0x00);
        packed.push_back(0xc4); packed.push_back(0x00);
        // fields map — paste the prebuilt map bytes verbatim
        packed.insert(packed.end(), fields.begin(), fields.end());

        // Prepare `signed` = dest(16) || src(16) || packed
        std::vector<uint8_t> hashedPart;
        hashedPart.reserve(32 + packed.size());
        hashedPart.insert(hashedPart.end(), _hubHash.data(), _hubHash.data() + 16);
        hashedPart.insert(hashedPart.end(), srcHash.data(),  srcHash.data()  + 16);
        hashedPart.insert(hashedPart.end(), packed.begin(), packed.end());

        RNS::Bytes hashedBytes(hashedPart.data(), hashedPart.size());
        RNS::Bytes msgHash = RNS::Identity::full_hash(hashedBytes);

        std::vector<uint8_t> signedPart;
        signedPart.reserve(hashedPart.size() + msgHash.size());
        signedPart.insert(signedPart.end(), hashedPart.begin(), hashedPart.end());
        signedPart.insert(signedPart.end(), msgHash.data(), msgHash.data() + msgHash.size());

        RNS::Bytes sigBytes(self.sign(RNS::Bytes(signedPart.data(), signedPart.size())));
        if (sigBytes.size() < 64) {
            Serial.printf("[TELEMETRY] [%s] signature short (%u bytes) — refusing\n",
                          v.label, (unsigned)sigBytes.size());
            return false;
        }

        // Wire body = src(16) || sig(64) || packed
        std::vector<uint8_t> payload;
        payload.reserve(16 + 64 + packed.size());
        payload.insert(payload.end(), srcHash.data(), srcHash.data() + 16);
        payload.insert(payload.end(), sigBytes.data(), sigBytes.data() + 64);
        payload.insert(payload.end(), packed.begin(), packed.end());

        Serial.printf("[TELEMETRY] [%s] payload=%uB (src:16 + sig:64 + packed:%u)\n",
                      v.label, (unsigned)payload.size(), (unsigned)packed.size());

        RNS::Bytes payloadBytes(payload.data(), payload.size());
        RNS::Packet packet(outDest, payloadBytes);

        // Pack first so we can warn the user if the encrypted raw packet
        // would exceed the LoRa single-frame budget before we burn airtime.
        size_t rawLen = 0;
        try {
            packet.pack();
            rawLen = packet.raw().size();
        } catch (const std::exception& e) {
            Serial.printf("[TELEMETRY] [%s] pack failed: %s\n", v.label, e.what());
            return false;
        }
        lastRawLen = rawLen;
        lastLabel  = v.label;
        Serial.printf("[TELEMETRY] [%s] packed raw=%uB (single-frame LoRa limit=%u)\n",
                      v.label, (unsigned)rawLen,
                      (unsigned)RSDECK_RNODE_SINGLE_FRAME_RAW_MAX);

        if (rawLen <= RSDECK_RNODE_SINGLE_FRAME_RAW_MAX) {
            // Fits — send it and we're done.
            RNS::PacketReceipt receipt = packet.send();
            if (!receipt) {
                Serial.println("[TELEMETRY] Packet::send: no interface accepted the packet");
                return false;
            }
            _txAccepted++;
            Serial.println("[TELEMETRY] packet accepted by Transport");
            return true;
        }

        // Over budget — pick the next variant (or refuse at the end of
        // the loop). Don't call packet.send() on an oversized packet:
        // split-frame TX has proven unreliable on the current 2-hop LoRa
        // path (see Stage 1 proof 2026-08-12).
        if (v.include_time && v.include_battery) {
            Serial.println("[TELEMETRY] over budget — retrying without SID_BATTERY");
        } else if (v.include_time) {
            Serial.println("[TELEMETRY] over budget — retrying without SID_TIME (location-only)");
        } else {
            // We already tried everything; refuse cleanly below.
        }
    }

    Serial.printf("[TELEMETRY] refusing send: [%s] raw=%uB still exceeds single-frame LoRa budget (%uB) — no split-frame TX on this path\n",
                  lastLabel, (unsigned)lastRawLen,
                  (unsigned)RSDECK_RNODE_SINGLE_FRAME_RAW_MAX);
    return false;
#endif // HAS_GPS
}

void TelemetryManager::resetToIdle(const char* reason) {
    Serial.printf("[TELEMETRY] aborting send: %s\n", reason ? reason : "?");
    _state = State::IDLE;
    _attempts = 0;
    // resetToIdle() is only invoked from the WAIT_IDENTITY / WAIT_PATH
    // timeout branches (driveIdentityDiscovery / drivePathDiscovery);
    // the SENDING → IDLE transition is a direct assignment, not a call
    // here. So this counter precisely tracks discovery-timeout aborts
    // (i.e. discovery that never reached SENDING), per spec.
    _discoveryAborted++;
}

// =============================================================================
// Helpers
// =============================================================================

TelemetryManager::LocSample TelemetryManager::readLocation() const {
    LocSample s;
#if HAS_GPS
    if (!_gps || !_gps->hasLocationFix()) return s;
    s.age_ms    = _gps->fixAgeMs();
    s.valid     = s.age_ms <= FRESH_FIX_MAX_AGE_MS;
    s.lat       = _gps->latitude();
    s.lon       = _gps->longitude();
    s.alt       = _gps->altitude();
    // rsDeck's GPS driver does not expose speed/bearing — leave NaN so
    // the SID_LOCATION encoder emits zeros for those slots while still
    // publishing the position (matches RLR's behaviour for unknown
    // velocity).
    s.speed     = NAN;
    s.bearing   = NAN;
#endif
    return s;
}

uint64_t TelemetryManager::epochSecondsOrUptime() const {
    // Prefer real epoch when the system clock is plausibly set (≥
    // 2024-01-01 = 1704067200). Otherwise fall back to monotonic
    // uptime, same way the RLR reference does on hardware without RTC.
    time_t now = time(nullptr);
    if (now >= 1704067200) return (uint64_t)now;
    return (uint64_t)(millis() / 1000UL);
}

// =============================================================================
// Hub-hash mutation (serial `G` command)
// =============================================================================

void TelemetryManager::setHubHashHex(const char* hex32) {
    if (!hex32) return;
    size_t len = strlen(hex32);
    if (len != 32) {
        Serial.printf("[TELEMETRY] setHubHashHex: need 32 hex chars (got %u)\n",
                      (unsigned)len);
        return;
    }
    for (size_t i = 0; i < 32; i++) {
        if (!std::isxdigit((unsigned char)hex32[i])) {
            Serial.printf("[TELEMETRY] setHubHashHex: invalid hex at index %u\n",
                          (unsigned)i);
            return;
        }
    }
    memcpy(_hubHex, hex32, 32);
    _hubHex[32] = '\0';
    _hubHash.clear();
    _hubHashLoaded = false;
    loadHubHashIfNeeded();
    Serial.printf("[TELEMETRY] hub set to %s\n", _hubHex);
}

void TelemetryManager::resetHubHashToDefault() {
    memcpy(_hubHex, "da424e0f47657d7575df58a2b83b111b", 32);
    _hubHex[32] = '\0';
    _hubHash.clear();
    _hubHashLoaded = false;
    loadHubHashIfNeeded();
    Serial.printf("[TELEMETRY] hub reset to default %s\n", _hubHex);
}

// =============================================================================
// Serial glue — handleSerial() is invoked by main.cpp for the single-char
// commands; the line-mode `G<32hex>` form is handled directly in main.cpp
// (where the 32-hex arg is already on the buffer) and lands in
// setHubHashHex().
// =============================================================================

bool TelemetryManager::handleSerial(char c, const char* /*line*/) {
    switch (c) {
        case 'g':
            // Lowercase 'g' = trigger sendNow (privacy-gated).
            sendNow();
            return true;
        case 'V':
            // Uppercase 'V' = telemetry status dump (counters + state).
            // 'T' was avoided because it is already mapped to the raw
            // radio-test hotkey (onHotkeyRadioTest) in main.cpp's
            // handleSerialCommands switch — see the 'V' override
            // rationale in the orchestrator task.
            printStatus();
            return true;
        default:
            return false;
    }
}

void TelemetryManager::printHelp() const {
    Serial.println("[TELEMETRY] g send-GPS-now       (privacy-gated)");
    Serial.println("[TELEMETRY] G<32hex> set-hub-hash (default  da424e0f47657d7575df58a2b83b111b)");
    Serial.println("[TELEMETRY] V telemetry-status   (counters + state)");
}

void TelemetryManager::printStatus() const {
    const Counters c = counters();
    // Pull interval + enabled from live UserConfig (same source
    // checkPeriodicSend() uses), so the dump reflects what the
    // periodic gate actually sees right now.
    int intervalS = 0;
    char enabled = '?';
    if (_cfg) {
        intervalS = _cfg->settings().gpsTelemetryIntervalS;
        enabled   = _cfg->settings().gpsTelemetryEnabled ? 'Y' : 'N';
    } else {
        enabled = 'N';
    }
    Serial.printf("[TELEMETRY-STATUS] ticks=%lu noFix=%lu stale=%lu discoveryAbort=%lu txOk=%lu interval=%ds enabled=%c state=%s\n",
                  (unsigned long)c.ticksFired,
                  (unsigned long)c.refusedNoFix,
                  (unsigned long)c.refusedStale,
                  (unsigned long)c.discoveryAborted,
                  (unsigned long)c.txAccepted,
                  intervalS,
                  enabled,
                  stateName());
}
