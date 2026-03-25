#pragma once

// =============================================================================
// NMEAParser — Zero-dependency NMEA 0183 sentence parser
// Parses $GNRMC/$GPRMC (time, date, position) and $GNGGA/$GPGGA (satellites,
// altitude, HDOP). Character-by-character state machine with XOR checksum
// validation. No heap allocation.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>

struct NMEAData {
    // Time (from RMC)
    uint8_t hour = 0, minute = 0, second = 0;
    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    bool timeValid = false;

    // Position (from RMC + GGA)
    double latitude = 0.0;     // decimal degrees, negative = south
    double longitude = 0.0;    // decimal degrees, negative = west
    double altitude = 0.0;     // meters above MSL (from GGA)
    bool locationValid = false;

    // Satellites / fix (from GGA)
    uint8_t satellites = 0;
    double hdop = 99.9;
    uint8_t fixQuality = 0;    // 0=none, 1=GPS, 2=DGPS, 6=estimated

    // Updated flags — set per-parse, cleared by caller
    bool timeUpdated = false;
    bool locationUpdated = false;
};

class NMEAParser {
public:
    // Feed one character at a time. Returns true when a valid sentence was parsed.
    bool encode(char c) {
        if (c == '$') {
            // Start of new sentence
            _pos = 0;
            _checksum = 0;
            _checksumming = true;
            _complete = false;
            return false;
        }

        if (_pos >= SENTENCE_MAX - 1) {
            // Overflow — discard
            _pos = 0;
            return false;
        }

        if (c == '*') {
            // End of data, next 2 chars are checksum hex
            _sentence[_pos] = '\0';
            _checksumming = false;
            _checksumIdx = 0;
            _receivedChecksum = 0;
            return false;
        }

        if (!_checksumming && _checksumIdx < 2) {
            // Collecting checksum hex digits
            _receivedChecksum = (_receivedChecksum << 4) | hexVal(c);
            _checksumIdx++;
            if (_checksumIdx == 2) {
                // Validate checksum
                if (_receivedChecksum == _checksum) {
                    return parseSentence();
                }
            }
            return false;
        }

        if (c == '\r' || c == '\n') {
            _pos = 0;
            return false;
        }

        if (_checksumming) {
            _sentence[_pos++] = c;
            _checksum ^= (uint8_t)c;
        }

        return false;
    }

    const NMEAData& data() const { return _data; }
    NMEAData& data() { return _data; }

    // When false, position fields (lat/lon/alt/sats/hdop) are not parsed
    void setParseLocation(bool enable) { _parseLocation = enable; }
    bool parseLocation() const { return _parseLocation; }

    uint32_t sentencesParsed() const { return _sentencesParsed; }
    uint32_t charsProcessed() const { return _charsProcessed; }

    // Call encode() and also track total chars for diagnostics
    bool feed(char c) {
        _charsProcessed++;
        return encode(c);
    }

private:
    static constexpr int SENTENCE_MAX = 96;
    static constexpr int MAX_FIELDS = 20;

    char _sentence[SENTENCE_MAX] = {};
    int _pos = 0;
    uint8_t _checksum = 0;
    bool _checksumming = false;
    uint8_t _receivedChecksum = 0;
    int _checksumIdx = 0;
    bool _complete = false;

    NMEAData _data;
    bool _parseLocation = false;   // Off by default — user must opt in
    uint32_t _sentencesParsed = 0;
    uint32_t _charsProcessed = 0;

    static uint8_t hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    // Split _sentence by commas into field pointers. Returns field count.
    int splitFields(char* fields[], int maxFields) {
        int count = 0;
        char* p = _sentence;
        fields[count++] = p;
        while (*p && count < maxFields) {
            if (*p == ',') {
                *p = '\0';
                fields[count++] = p + 1;
            }
            p++;
        }
        return count;
    }

    bool parseSentence() {
        char* fields[MAX_FIELDS];
        int n = splitFields(fields, MAX_FIELDS);
        if (n < 1) return false;

        // Identify sentence type (skip talker ID prefix: GP, GN, GL, GA, GB)
        const char* type = fields[0];
        const char* suffix = type;
        if (strlen(type) >= 5) {
            suffix = type + 2;  // Skip "GN", "GP", etc.
        }

        bool parsed = false;
        if (strcmp(suffix, "RMC") == 0 && n >= 10) {
            parsed = parseRMC(fields, n);
        } else if (strcmp(suffix, "GGA") == 0 && n >= 10) {
            parsed = parseGGA(fields, n);
        }

        if (parsed) _sentencesParsed++;
        return parsed;
    }

    // $xxRMC — Recommended Minimum
    // fields: type,time,status,lat,N/S,lon,E/W,speed,course,date,magvar,E/W,mode
    bool parseRMC(char* fields[], int n) {
        // Status: A=active/valid, V=void
        if (fields[2][0] != 'A') {
            _data.timeValid = false;
            _data.locationValid = false;
            return true;  // Parsed successfully, just no fix
        }

        // Time: HHMMSS.ss
        if (strlen(fields[1]) >= 6) {
            _data.hour   = (fields[1][0] - '0') * 10 + (fields[1][1] - '0');
            _data.minute = (fields[1][2] - '0') * 10 + (fields[1][3] - '0');
            _data.second = (fields[1][4] - '0') * 10 + (fields[1][5] - '0');
        }

        // Date: DDMMYY
        if (n >= 10 && strlen(fields[9]) >= 6) {
            _data.day   = (fields[9][0] - '0') * 10 + (fields[9][1] - '0');
            _data.month = (fields[9][2] - '0') * 10 + (fields[9][3] - '0');
            int yy      = (fields[9][4] - '0') * 10 + (fields[9][5] - '0');
            _data.year  = 2000 + yy;
            _data.timeValid = true;
            _data.timeUpdated = true;
        }

        // Position — only parse if location tracking is enabled
        if (_parseLocation) {
            // Latitude: DDMM.MMMM,N/S
            if (strlen(fields[3]) > 0 && strlen(fields[4]) > 0) {
                _data.latitude = parseCoord(fields[3]);
                if (fields[4][0] == 'S') _data.latitude = -_data.latitude;
            }

            // Longitude: DDDMM.MMMM,E/W
            if (strlen(fields[5]) > 0 && strlen(fields[6]) > 0) {
                _data.longitude = parseCoord(fields[5]);
                if (fields[6][0] == 'W') _data.longitude = -_data.longitude;
                _data.locationValid = true;
                _data.locationUpdated = true;
            }
        }

        return true;
    }

    // $xxGGA — Global Positioning System Fix Data
    // fields: type,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,geoid,M,age,station
    bool parseGGA(char* fields[], int n) {
        // Always parse fix quality and satellites (needed for time validation)
        _data.fixQuality = atoi(fields[6]);
        _data.satellites = atoi(fields[7]);

        // Only parse position fields if location tracking is enabled
        if (_parseLocation) {
            if (strlen(fields[8]) > 0) {
                _data.hdop = atof(fields[8]);
            }
            if (n >= 10 && strlen(fields[9]) > 0) {
                _data.altitude = atof(fields[9]);
            }
        }

        return true;
    }

    // Parse NMEA coordinate: DDDMM.MMMM → decimal degrees
    static double parseCoord(const char* s) {
        double raw = atof(s);
        int degrees = (int)(raw / 100.0);
        double minutes = raw - (degrees * 100.0);
        return degrees + minutes / 60.0;
    }
};
