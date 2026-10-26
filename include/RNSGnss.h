/**
 * @file RNSGnss.h
 * @brief UC6580 GNSS on the Heltec T096: position for the map announce,
 *        wall clock for LXMF timestamps.
 *
 * Wiring (schematic sheet 1, U10 UC6580): module TX → P0.23 (nRF RX),
 * module RX ← P0.25 (nRF TX), 115200 baud NMEA. The module's 3V3 rail
 * is a PMOS (Q3) gated by VGNSS_CTRL P0.06: LOW = powered. GNSS_RST on
 * P1.14 is active LOW and is held HIGH. PPS on P1.11 is not used.
 *
 * Serial1 (UARTE0) is moved to those pins with setPins(); the console
 * runs on USB CDC so UARTE0 is otherwise free.
 *
 * Parsing: minimal NMEA-0183. RMC gives UTC time, date, fix status and
 * lat/lon; GGA gives fix quality, satellite count and altitude. Both
 * GP and GN talker IDs are accepted (the UC6580 emits GN sentences).
 * Checksums are verified; sentences that fail are counted and dropped.
 *
 * Modes: OFF (rail down, default) and ON (rail up, continuous, ~40 mA).
 * Position is exported as microdegrees so it plugs into the discovery
 * config unchanged. The clock is unix seconds derived from the last
 * RMC plus elapsed millis() since it was parsed.
 */
#pragma once
#include "RNSConfig.h"

#if HAS_GNSS && !defined(NATIVE_TEST)
#include <Arduino.h>
#include "RNSVext.h"

class RNSGnss {
public:
    enum Mode : uint8_t { GNSS_OFF = 0, GNSS_ON = 1 };

    Mode     mode = GNSS_OFF;
    bool     powered = false;

    // Duty cycle (mode ON only): intervalMin == 0 → continuous. Otherwise
    // the rail comes up every intervalMin, stays up until a fix has been
    // held for GNSS_FIX_SETTLE_MS or dwellSec elapses, then drops.
    uint16_t intervalMin = 0;
    uint16_t dwellSec    = 300;
    uint32_t nextWakeAt  = 0;
    uint32_t cycles = 0, cyclesFixed = 0;

    // Fix state
    bool     hasFix   = false;
    int32_t  latUdeg  = 0;
    int32_t  lonUdeg  = 0;
    float    altM     = 0.0f;
    bool     hasAlt   = false;
    uint8_t  sats     = 0;
    uint8_t  fixQuality = 0;
    float    hdop     = 0.0f;
    uint32_t lastFixAt = 0;      // millis of last valid RMC

    // Clock
    bool     timeValid = false;
    uint32_t epoch     = 0;      // unix seconds at epochAtMillis
    uint32_t epochAtMillis = 0;

    // Diagnostics
    uint32_t sentences = 0, badChecksum = 0, lastSentenceAt = 0;
    uint32_t poweredAt = 0;
    uint32_t rmcSeen = 0, rmcValid = 0, ggaSeen = 0;
    char     lastRmc[100] = {0};     // last RMC sentence as received (for `gps raw`)
    Stream*  rawTo = nullptr;        // when set, every sentence is echoed here
    uint32_t rawUntil = 0;

    void begin(Mode m, uint32_t now) {
        pinMode(PIN_GNSS_CTRL, OUTPUT);
        pinMode(PIN_GNSS_RESET, OUTPUT);
        digitalWrite(PIN_GNSS_RESET, HIGH);     // reset is active LOW: inactive
        mode = m;
        if (mode == GNSS_ON) { powerOn(now); if (intervalMin) cycles++; } else powerOff(false);
    }

    void setMode(Mode m, uint32_t now) {
        mode = m;
        if (mode == GNSS_ON) { if (!powered) { powerOn(now); if (intervalMin) cycles++; } }
        else powerOff(false);
    }

    void setInterval(uint16_t minutes, uint32_t now) {
        intervalMin = minutes;
        if (mode == GNSS_ON && !powered) { powerOn(now); cycles++; }
    }

    void loop(uint32_t now) {
        if (mode == GNSS_ON && intervalMin > 0) {
            if (!powered) {
                if ((int32_t)(now - nextWakeAt) >= 0) { powerOn(now); cycles++; }
                return;
            }
            // powered, in a duty cycle: done when a fix has settled or dwell expired
            bool settled = hasFix && (now - firstFixAt) >= GNSS_FIX_SETTLE_MS;
            bool expired = (now - poweredAt) >= (uint32_t)dwellSec * 1000UL;
            if (settled || expired) {
                if (settled) cyclesFixed++;
                lastCycleFixed = settled;
                powerOff(true);
                nextWakeAt = now + (uint32_t)intervalMin * 60000UL;
                return;
            }
        }
        if (!powered) return;   // (a kept fix from the last cycle stays valid while asleep)
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '$') { lineLen = 0; inLine = true; }
            if (!inLine) continue;
            if (c == '\n' || c == '\r') {
                if (lineLen > 6) { line[lineLen] = '\0'; handleLine(now); }
                inLine = false; lineLen = 0;
                continue;
            }
            if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
            else { inLine = false; lineLen = 0; }
        }
        if (hasFix && (now - lastFixAt) > 15000UL) { hasFix = false; firstFixAt = 0; }   // stale
    }

    bool lastCycleFixed = false;
    static const uint32_t GNSS_FIX_SETTLE_MS = 20000UL;   // hold a fix this long before sleeping

    /// Unix seconds now, or 0 when the clock has never been set.
    uint32_t unixNow(uint32_t now) const {
        if (!timeValid) return 0;
        return epoch + (now - epochAtMillis) / 1000UL;
    }

private:
    char     line[100];
    uint8_t  lineLen = 0;
    bool     inLine  = false;

    void powerOn(uint32_t now) {
        // The GNSS rail is switched from Vext (schematic: Q3 source is
        // Vext_3V3), so Vext must be up first and held while we run.
        RNSVext::claim(RNSVext::VEXT_GNSS);
        digitalWrite(PIN_GNSS_CTRL, LOW);        // PMOS: LOW = rail on
        delay(50);
        justPoweredOn = true;
        Serial1.setPins(PIN_GNSS_RX, PIN_GNSS_TX);
        Serial1.begin(GNSS_BAUD);
        powered = true; poweredAt = now;
        sentences = 0; badChecksum = 0; hasFix = false;
    }

    // keepFix: a duty cycle keeps the last position/clock as "current"
    // while the rail is down; a manual `gps off` clears it.
    void powerOff(bool keepFix) {
        if (powered) Serial1.end();
        digitalWrite(PIN_GNSS_CTRL, HIGH);
        RNSVext::release(RNSVext::VEXT_GNSS);        // rail stays up if the display holds it
        powered = false;
        if (!keepFix) { hasFix = false; sats = 0; fixQuality = 0; }
        firstFixAt = 0;
    }
    uint32_t firstFixAt = 0;
public:
    /// Set by powerOn(); main clears it after re-initialising the display.
    bool justPoweredOn = false;
private:

    static bool checksumOk(const char* s, uint8_t len, uint8_t& payloadEnd) {
        // s = "$....*hh"
        uint8_t star = 0;
        for (uint8_t i = 1; i < len; i++) if (s[i] == '*') { star = i; break; }
        if (!star || star + 2 >= len + 1) return false;
        uint8_t x = 0;
        for (uint8_t i = 1; i < star; i++) x ^= (uint8_t)s[i];
        uint8_t h = (uint8_t)((hexVal(s[star + 1]) << 4) | hexVal(s[star + 2]));
        payloadEnd = star;
        return x == h;
    }
    static uint8_t hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    // Split payload into fields in place; returns count.
    uint8_t split(char* s, uint8_t end, const char** f, uint8_t maxF) {
        uint8_t n = 0; f[n++] = s + 1;
        for (uint8_t i = 1; i < end && n < maxF; i++) {
            if (s[i] == ',') { s[i] = '\0'; f[n++] = s + i + 1; }
        }
        s[end] = '\0';
        return n;
    }

    // "ddmm.mmmm" / "dddmm.mmmm" with hemisphere → microdegrees
    static int32_t nmeaToUdeg(const char* v, char hemi) {
        if (!v || !*v) return 0;
        const char* dot = strchr(v, '.');
        uint8_t intLen = dot ? (uint8_t)(dot - v) : (uint8_t)strlen(v);
        if (intLen < 3) return 0;
        uint8_t degLen = intLen - 2;
        uint32_t deg = 0;
        for (uint8_t i = 0; i < degLen; i++) deg = deg * 10 + (v[i] - '0');
        double minutes = atof(v + degLen);
        double d = (double)deg + minutes / 60.0;
        int32_t udeg = (int32_t)(d * 1e6 + 0.5);
        if (hemi == 'S' || hemi == 'W') udeg = -udeg;
        return udeg;
    }

    static uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return (uint32_t)(era * 146097 + (int)doe - 719468);
    }

    void handleLine(uint32_t now) {
        if (rawTo) { if (now < rawUntil) rawTo->println(line); else rawTo = nullptr; }
        uint8_t end = 0;
        if (!checksumOk(line, lineLen, end)) { badChecksum++; return; }
        sentences++; lastSentenceAt = now;
        if (lineLen > 5 && strncmp(line + 3, "RMC", 3) == 0) { rmcSeen++; strncpy(lastRmc, line, sizeof(lastRmc) - 1); lastRmc[sizeof(lastRmc) - 1] = '\0'; }
        const char* f[20]; uint8_t n = split(line, end, f, 20);
        if (n < 2) return;
        const char* id = f[0];                     // e.g. "GNRMC"
        if (strlen(id) != 5) return;
        if (strcmp(id + 2, "RMC") == 0 && n >= 10) {
            // 1 time hhmmss.ss, 2 status A/V, 3 lat, 4 N/S, 5 lon, 6 E/W, 9 date ddmmyy
            bool valid = (f[2][0] == 'A');
            if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
                unsigned hh = (f[1][0]-'0')*10 + (f[1][1]-'0');
                unsigned mm = (f[1][2]-'0')*10 + (f[1][3]-'0');
                unsigned ss = (f[1][4]-'0')*10 + (f[1][5]-'0');
                unsigned dd = (f[9][0]-'0')*10 + (f[9][1]-'0');
                unsigned mo = (f[9][2]-'0')*10 + (f[9][3]-'0');
                unsigned yy = (f[9][4]-'0')*10 + (f[9][5]-'0');
                if (valid && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31) {
                    epoch = daysFromCivil(2000 + (int)yy, mo, dd) * 86400UL + hh * 3600UL + mm * 60UL + ss;
                    epochAtMillis = now; timeValid = true;
                }
            }
            if (valid) {
                rmcValid++;
                latUdeg = nmeaToUdeg(f[3], f[4][0]);
                lonUdeg = nmeaToUdeg(f[5], f[6][0]);
                bool nowFix = (latUdeg != 0 || lonUdeg != 0);
                if (nowFix && !hasFix) firstFixAt = now;
                hasFix = nowFix;
                if (hasFix) lastFixAt = now;
            }
        } else if (strcmp(id + 2, "GGA") == 0 && n >= 10) {
            ggaSeen++;
            // 6 fix quality, 7 sats, 8 hdop, 9 altitude
            fixQuality = (uint8_t)atoi(f[6]);
            sats = (uint8_t)atoi(f[7]);
            hdop = (float)atof(f[8]);
            if (f[9][0]) { altM = (float)atof(f[9]); hasAlt = fixQuality > 0; }
        }
    }
};
#endif
