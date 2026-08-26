// =============================================================================
// TelemetryManager.h — on-demand GPS telemetry sender for rsDeck #64
// (Stage 1 / thin hybrid).
//
// Wire format is one opportunistic LXMF packet to a configured collector
// (default: Lyra telemetry-collector hub). The packet carries an
// LXMF FIELD_TELEMETRY value built by `TelemetryEncoder` whose inner
// snapshot matches Sideband's Telemeter.packed().
//
// Triggers:
//   * Serial console `g` (on-demand, privacy-gated).
//   * Periodic tick driven by `UserSettings::gpsTelemetryIntervalS`
//     (issue #64 "configurable by interval" privacy requirement). 0
//     disables periodic sending — the same on-demand gate (fresh fix ≤
//     FRESH_FIX_MAX_AGE_MS) must pass for both paths. The periodic
//     timer is advanced even on a refused send so a persistently stale
//     fix doesn't trigger a retry-every-loop() spam.
//
// State machine implemented in `loop()` drives the same Reticulum path /
// identity discovery pattern LXMFManager already uses for chat, without
// touching any of the LXMF chat code.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <Identity.h>
#include <Bytes.h>

#include "TelemetryEncoder.h"

class GPSManager;
class Power;
class ReticulumManager;
class UserConfig;

class TelemetryManager {
public:
    TelemetryManager();

    // Wire dependencies at setup time. None may be null when
    // sendNow()/loop() are exercised at runtime.
    void begin(ReticulumManager* rns, GPSManager* gps, Power* power);

    // Bind UserConfig so sendNow() can read the live opt-in flag +
    // hub hash from Settings > Time & Location. Optional — the manager
    // still works standalone with its compiled-in default hub hash if
    // never bound. Reading is fresh on every sendNow() so toggling the
    // "Send Telemetry" item in the UI applies without a live callback.
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }

    // Drive the path/identity discovery retry state machine. Safe to call
    // every loop iteration; cheap when no attempt is in flight.
    void loop();

    // Serial helpers — share a 96-byte input buffer with the main
    // serial parser.
    bool handleSerial(char c, const char* line);
    void printHelp() const;

    // Programmatic API — invoked by the serial `g` command. Returns
    // false if the privacy gate refuses (no fix / stale fix) or if a
    // send is already in flight.
    bool sendNow();

    // 32-hex default hash is set at build time (see .cpp). The serial
    // `G` command can update it for the current session.
    void setHubHashHex(const char* hex32);
    void resetHubHashToDefault();
    const char* hubHashHex() const { return _hubHex; }

    // Status queries (used by serial help / heartbeat diagnostics).
    bool hasFreshFix() const;
    bool isAttemptInFlight() const { return _state != State::IDLE; }
    const char* stateName() const;

    // Diagnostic counters (RAM-only, no persistence; reset on boot).
    // Used to disambiguate two failure hypotheses for periodic GPS
    // telemetry sends: RF-out-of-range vs GPS-fix-staleness-gate mismatch.
    struct Counters {
        uint32_t ticksFired;        // checkPeriodicSend() interval met → attempted act
        uint32_t refusedNoFix;      // sendNow() refused: !hasLocationFix()
        uint32_t refusedStale;      // sendNow() refused: fix age > FRESH_FIX_MAX_AGE_MS
        uint32_t discoveryAborted;  // WAIT_IDENTITY/WAIT_PATH timed out before SENDING
        uint32_t txAccepted;        // buildAndSendSnapshot(): packet.send() succeeded
    };
    Counters counters() const {
        return {
            _ticksFired,
            _refusedNoFix,
            _refusedStale,
            _discoveryAborted,
            _txAccepted,
        };
    }

    // Serial command 'V' handler — prints one-line counter + state dump.
    void printStatus() const;

    // Privacy gate threshold (ms). 30 s per the rsDeck #64 spec.
    static constexpr uint32_t FRESH_FIX_MAX_AGE_MS = 30000UL;
    static constexpr uint8_t  DISCOVERY_MAX_ATTEMPTS = 10;

private:
    // ----- bound subsystems -----
    ReticulumManager* _rns = nullptr;
    GPSManager*       _gps = nullptr;
    Power*            _power = nullptr;
    UserConfig*       _cfg = nullptr;       // optional, gates sendNow() privacy

    // ----- destination / identity -----
    // Default = Lyra telemetry-collector (da424e0f47657d7575df58a2b83b111b).
    char _hubHex[33] = "da424e0f47657d7575df58a2b83b111b";
    RNS::Bytes _hubHash;                  // 16 bytes (truncated dest hash)
    bool _hubHashLoaded = false;

    // ----- send state machine -----
    enum class State : uint8_t {
        IDLE = 0,
        WAIT_IDENTITY,        // requested, polling Identity::recall()
        WAIT_PATH,            // identity known, polling has_path()
        SENDING,              // path & identity present, packing+sending
    };
    State _state = State::IDLE;
    uint8_t  _attempts = 0;
    unsigned long _stateAtMs = 0;
    static constexpr unsigned long STATE_RETRY_INTERVAL_MS = 1000;

    // Helpers
    void loadHubHashIfNeeded();
    void applyUserConfigHubHash();
    void driveIdentityDiscovery();
    void drivePathDiscovery();
    bool buildAndSendSnapshot();
    void resetToIdle(const char* reason);
    uint64_t epochSecondsOrUptime() const;

    // Periodic-send gate. Called from loop() when the state machine is
    // IDLE. Reads the live UserConfig, so toggling "Send Telemetry" or
    // changing the interval takes effect on the next loop tick without a
    // callback. Reuses sendNow() so the privacy gate is enforced
    // identically between on-demand and periodic paths.
    void checkPeriodicSend();

    // One-shot diagnostic log for why checkPeriodicSend() isn't arming
    // (cfg unbound / toggle off / interval 0). Logs once per cause change
    // instead of spamming every loop() tick, so "why isn't periodic
    // firing?" is answerable from the serial console without guessing.
    void _logPeriodicSkipOnce(const char* reason);
    bool _periodicSkipLogged = false;

    // Periodic-send bookkeeping. _lastPeriodicSendMs is updated *before*
    // sendNow() so a refused send (e.g. stale fix) does not retry on
    // every loop() iteration — the next attempt is scheduled at the
    // configured interval from this tick.
    unsigned long _lastPeriodicSendMs = 0;

    // ----- diagnostic counters (RAM-only; see Counters above) -----
    uint32_t _ticksFired       = 0;
    uint32_t _refusedNoFix     = 0;
    uint32_t _refusedStale     = 0;
    uint32_t _discoveryAborted = 0;
    uint32_t _txAccepted       = 0;

    // Cheap sanity: snapshot read of GPS -> doubles
    struct LocSample {
        bool   valid = false;
        double lat = NAN, lon = NAN, alt = NAN, speed = NAN, bearing = NAN;
        double accuracy_m = NAN;
        uint32_t age_ms = 0;
    };
    LocSample readLocation() const;
};
