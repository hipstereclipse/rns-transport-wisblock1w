/**
 * @file RNSDiscovery.h
 * @brief Reticulum interface-discovery announce ("discoverable = yes").
 *
 * Emits the same announce a Python RNS node sends for a discoverable
 * interface, so this node appears on RMAP-style maps:
 *
 *   destination  rnstransport.discovery.interface  (SINGLE, our identity)
 *   app_data     [flags 0x00][msgpack map][stamp 32 B]
 *
 * Map keys / value types follow RNS/Discovery.py get_interface_announce_data:
 *   0x00 INTERFACE_TYPE   str   "RNodeInterface"
 *   0x01 TRANSPORT        bool  true
 *   0xFE TRANSPORT_ID     bin   16-byte identity hash
 *   0xFD TRANSPORT_IMPL   str
 *   0xFC TRANSPORT_VERS   str
 *   0xFF NAME             str
 *   0x03 LATITUDE         float64 | nil
 *   0x04 LONGITUDE        float64 | nil
 *   0x05 HEIGHT           float64 | nil
 *   0x09 FREQUENCY        uint  Hz
 *   0x0A BANDWIDTH        uint  Hz
 *   0x0B SPREADINGFACTOR  uint
 *   0x0C CODINGRATE       uint
 *
 * Stamp (LXMF/LXStamper.py, RNS/Discovery.py):
 *   infohash  = SHA256(packed)
 *   workblock = concat over n in 0..19 of
 *               HKDF(len 256, ikm=infohash, salt=SHA256(infohash+msgpack(n)), info="")
 *   stamp     = 32 random bytes with SHA256(workblock+stamp) <= 2^(256-16),
 *               i.e. 16 leading zero bits (DEFAULT_STAMP_VALUE).
 *
 * The workblock is fixed for a given packed descriptor, so the search
 * keeps a SHA-256 midstate after the 5120-byte workblock (exactly 80
 * blocks) and each attempt costs one compression. ~65k attempts on
 * average, run in slices from loop() so the radio and console stay
 * live. The found stamp is persisted with its infohash and reused
 * across reboots until the descriptor changes.
 *
 * Verified byte-exact against the installed RNS 1.5.4 handler with
 * tools/validate_discovery.py.
 */
#pragma once
#include "RNSConfig.h"
#include "RNSTransport.h"
#include "RNSIdentity.h"

#ifndef NATIVE_TEST
#include <Arduino.h>
#include <SHA256.h>
#endif

#define DISCOVERY_APP_NAME        "rnstransport.discovery.interface"
#define DISCOVERY_IMPL_NAME       "RatTunnel"
#define DISCOVERY_STAMP_VALUE     16
#define DISCOVERY_EXPAND_ROUNDS   20
#define DISCOVERY_HKDF_LEN        256
#define DISCOVERY_WORKBLOCK_LEN   (DISCOVERY_EXPAND_ROUNDS * DISCOVERY_HKDF_LEN)   // 5120
#define DISCOVERY_STAMP_LEN       32
#define DISCOVERY_PACKED_MAX      224
#define DISCOVERY_INTERVAL_DEFAULT_MIN 360      // RNS default: 6 h
#define DISCOVERY_INTERVAL_MIN_MIN     5        // RNS floor: 5 min
#define DISCOVERY_FIRST_DELAY_MS       30000UL
#define DISCOVERY_ATTEMPTS_PER_SLICE   48

struct DiscoveryConfig {
    bool     enabled     = false;
    int32_t  latUdeg     = 0;          // microdegrees; 0/0 = unset
    int32_t  lonUdeg     = 0;
    int16_t  heightM     = 0;
    bool     haveHeight  = false;
    uint16_t intervalMin = DISCOVERY_INTERVAL_DEFAULT_MIN;
};

class RNSDiscovery {
public:
    DiscoveryConfig cfg;

    // Stamp cache (persisted alongside cfg): valid when stampInfohash == current infohash.
    uint8_t  stamp[DISCOVERY_STAMP_LEN] = {0};
    uint8_t  stampInfohash[32] = {0};
    bool     stampReady = false;

    // Search progress
    bool     searching  = false;
    uint32_t attempts   = 0;
    uint32_t searchStartedAt = 0;

    uint32_t lastAnnounceAt = 0;
    uint32_t announcesSent  = 0;
    uint32_t nextAnnounceAt = 0;

    void begin(RNSTransport* txp, RNSIdentity* id, RNSRadio* rad) {
        transport = txp; identity = id; radio = rad;
        uint8_t pub[RNS_KEYSIZE];
        identity->getPublicKey(pub);
        RNSIdentity::computeDestHash(DISCOVERY_APP_NAME, pub, destHash);
        {   SHA256 sha; sha.reset(); sha.update(DISCOVERY_APP_NAME, strlen(DISCOVERY_APP_NAME));
            uint8_t full[32]; sha.finalize(full, 32); memcpy(nameHash, full, RNS_NAME_HASH_LEN); }
        {   SHA256 sha; sha.reset(); sha.update(pub, RNS_KEYSIZE);
            uint8_t full[32]; sha.finalize(full, 32); memcpy(identityHash, full, RNS_ADDR_LEN); }
        refresh(millis());
    }

    /** Rebuild the descriptor; (re)start the stamp search if it changed. */
    void refresh(uint32_t now) {
        snapshotInputs();
        packedLen = packDescriptor(packed, sizeof(packed));
        SHA256 sha; sha.reset(); sha.update(packed, packedLen); sha.finalize(infohash, 32);
        if (stampReady && memcmp(stampInfohash, infohash, 32) == 0) {
            searching = false;
        } else {
            stampReady = false;
            if (cfg.enabled) startSearch(now); else searching = false;
        }
        nextAnnounceAt = now + DISCOVERY_FIRST_DELAY_MS;
    }

    /** Call every loop() pass. Returns true when a stamp was just found (caller persists). */
    bool loop(uint32_t now) {
        bool found = false;
        // Descriptor inputs that can change at runtime (console `name`,
        // `set`, `profile`): re-pack when they do, which restarts the
        // stamp search if the packed bytes actually changed.
        if (radio && (radio->curFreqMHz != lastFreq || radio->curBwKHz != lastBw ||
                      radio->curSF != lastSF || radio->curCR != lastCR ||
                      strncmp(transport->getAnnounceName(), lastName, sizeof(lastName) - 1) != 0)) {
            snapshotInputs();
            refresh(now);
        }
        if (searching) found = searchSlice();
        if (cfg.enabled && stampReady && radio && radio->hwReady && now >= nextAnnounceAt) {
            if (announce()) { lastAnnounceAt = now; announcesSent++; }
            nextAnnounceAt = now + (uint32_t)cfg.intervalMin * 60000UL;
        }
        return found;
    }

    bool announce() {
        if (!stampReady || packedLen == 0) return false;
        static uint8_t appData[1 + DISCOVERY_PACKED_MAX + DISCOVERY_STAMP_LEN];
        uint16_t n = buildAppData(appData, sizeof(appData));
        if (n == 0) return false;
        return transport->sendAnnounceFor(destHash, nameHash, appData, n, false);
    }

    /** flags + packed + stamp, for the console `discovery dump` and validation. */
    uint16_t buildAppData(uint8_t* out, uint16_t outMax) const {
        if (!stampReady || outMax < 1 + packedLen + DISCOVERY_STAMP_LEN) return 0;
        out[0] = 0x00;
        memcpy(out + 1, packed, packedLen);
        memcpy(out + 1 + packedLen, stamp, DISCOVERY_STAMP_LEN);
        return (uint16_t)(1 + packedLen + DISCOVERY_STAMP_LEN);
    }

    const uint8_t* destinationHash() const { return destHash; }
    const uint8_t* currentInfohash() const { return infohash; }
    uint16_t packedLength() const { return packedLen; }
    const uint8_t* packedBytes() const { return packed; }
    bool hasLocation() const { return cfg.latUdeg != 0 || cfg.lonUdeg != 0; }

private:
    RNSTransport* transport = nullptr;
    RNSIdentity*  identity  = nullptr;
    RNSRadio*     radio     = nullptr;
    uint8_t destHash[RNS_ADDR_LEN] = {0};
    uint8_t nameHash[RNS_NAME_HASH_LEN] = {0};
    uint8_t identityHash[RNS_ADDR_LEN] = {0};
    uint8_t packed[DISCOVERY_PACKED_MAX] = {0};
    uint16_t packedLen = 0;
    uint8_t infohash[32] = {0};

    // Runtime inputs snapshot (change detection)
    float   lastFreq = 0, lastBw = 0; uint8_t lastSF = 0, lastCR = 0;
    char    lastName[RNS_ANNOUNCE_NAME_MAX + 1] = {0};
    void snapshotInputs() {
        if (radio) { lastFreq = radio->curFreqMHz; lastBw = radio->curBwKHz; lastSF = radio->curSF; lastCR = radio->curCR; }
        strncpy(lastName, transport->getAnnounceName(), sizeof(lastName) - 1);
        lastName[sizeof(lastName) - 1] = '\0';
    }

    // Stamp search state
    static uint8_t workblock[DISCOVERY_WORKBLOCK_LEN];
    SHA256   midstate;                 // SHA-256 state after the workblock
    uint32_t rng[4] = {0};

    // ── msgpack writer ─────────────────────────────────────
    struct Writer {
        uint8_t* buf; uint16_t max; uint16_t pos; bool ok;
        Writer(uint8_t* b, uint16_t m) : buf(b), max(m), pos(0), ok(true) {}
        void byte(uint8_t v) { if (pos < max) buf[pos++] = v; else ok = false; }
        void raw(const void* p, uint16_t n) { if (pos + n <= max) { memcpy(buf + pos, p, n); pos += n; } else ok = false; }
        void mapHeader(uint8_t n) { byte((uint8_t)(0x80 | (n & 0x0F))); }
        void uint(uint32_t v) {
            if (v < 128)         { byte((uint8_t)v); }
            else if (v < 256)    { byte(0xCC); byte((uint8_t)v); }
            else if (v < 65536)  { byte(0xCD); byte((uint8_t)(v >> 8)); byte((uint8_t)v); }
            else { byte(0xCE); byte((uint8_t)(v >> 24)); byte((uint8_t)(v >> 16)); byte((uint8_t)(v >> 8)); byte((uint8_t)v); }
        }
        void str(const char* s) {
            uint16_t n = (uint16_t)strlen(s);
            if (n < 32) byte((uint8_t)(0xA0 | n)); else { byte(0xD9); byte((uint8_t)n); }
            raw(s, n);
        }
        void bin(const uint8_t* p, uint8_t n) { byte(0xC4); byte(n); raw(p, n); }
        void boolean(bool v) { byte(v ? 0xC3 : 0xC2); }
        void nil() { byte(0xC0); }
        void f64(double d) {
            uint64_t bits; memcpy(&bits, &d, 8);
            byte(0xCB);
            for (int i = 7; i >= 0; i--) byte((uint8_t)(bits >> (8 * i)));
        }
    };

    uint16_t packDescriptor(uint8_t* out, uint16_t outMax) const {
        Writer w(out, outMax);
        w.mapHeader(13);
        w.uint(0x00); w.str("RNodeInterface");
        w.uint(0x01); w.boolean(true);
        w.uint(0xFE); w.bin(identityHash, RNS_ADDR_LEN);
        w.uint(0xFD); w.str(DISCOVERY_IMPL_NAME);
        w.uint(0xFC); w.str(FW_VERSION_STRING);
        w.uint(0xFF); w.str(sanitizedName());
        if (hasLocation()) {
            w.uint(0x03); w.f64((double)cfg.latUdeg / 1e6);
            w.uint(0x04); w.f64((double)cfg.lonUdeg / 1e6);
            w.uint(0x05); if (cfg.haveHeight) w.f64((double)cfg.heightM); else w.nil();
        } else {
            w.uint(0x03); w.nil();
            w.uint(0x04); w.nil();
            w.uint(0x05); w.nil();
        }
        // Round at kHz resolution first: 914.9f * 1e6f is not exactly
        // representable and came out as 914900032 on the bench.
        w.uint(0x09); w.uint((uint32_t)((uint32_t)(radio->curFreqMHz * 1000.0f + 0.5f)) * 1000UL);
        w.uint(0x0A); w.uint((uint32_t)(radio->curBwKHz * 1000.0f + 0.5f));
        w.uint(0x0B); w.uint(radio->curSF);
        w.uint(0x0C); w.uint(radio->curCR);
        return w.ok ? w.pos : 0;
    }

    const char* sanitizedName() const {
        // RNS sanitize(): strip CR/LF and surrounding whitespace. The
        // transport's announce name is already cleaned that way.
        const char* n = transport->getAnnounceName();
        return (n && *n) ? n : "RatTunnel";
    }

    // ── workblock + stamp search ──────────────────────────
    void startSearch(uint32_t now) {
        buildWorkblock();
        midstate.reset();
        midstate.update(workblock, DISCOVERY_WORKBLOCK_LEN);
        // xorshift128 seeded from the infohash and time; the stamp only
        // needs to be unpredictable enough to spread the search.
        memcpy(rng, infohash, 16);
        rng[0] ^= now; rng[3] ^= (uint32_t)identityHash[0] << 24;
        if (!rng[0] && !rng[1] && !rng[2] && !rng[3]) rng[0] = 1;
        attempts = 0; searchStartedAt = now; searching = true;
    }

    uint32_t xorshift() {
        uint32_t t = rng[3]; uint32_t s = rng[0];
        rng[3] = rng[2]; rng[2] = rng[1]; rng[1] = s;
        t ^= t << 11; t ^= t >> 8;
        rng[0] = t ^ s ^ (s >> 19);
        return rng[0];
    }

    bool searchSlice() {
        uint8_t cand[DISCOVERY_STAMP_LEN];
        uint8_t digest[32];
        for (uint8_t i = 0; i < DISCOVERY_ATTEMPTS_PER_SLICE; i++) {
            for (uint8_t k = 0; k < DISCOVERY_STAMP_LEN; k += 4) {
                uint32_t r = xorshift();
                cand[k] = (uint8_t)r; cand[k+1] = (uint8_t)(r >> 8); cand[k+2] = (uint8_t)(r >> 16); cand[k+3] = (uint8_t)(r >> 24);
            }
            SHA256 h = midstate;              // copy midstate: one compression per attempt
            h.update(cand, DISCOVERY_STAMP_LEN);
            h.finalize(digest, 32);
            attempts++;
            // valid iff SHA256(workblock+stamp) <= 2^(256-16): first 16 bits zero
            if (digest[0] == 0 && digest[1] == 0) {
                memcpy(stamp, cand, DISCOVERY_STAMP_LEN);
                memcpy(stampInfohash, infohash, 32);
                stampReady = true; searching = false;
                return true;
            }
        }
        return false;
    }

    // workblock = for n in 0..19: HKDF(256, ikm=infohash, salt=SHA256(infohash+msgpack(n)), info="")
    void buildWorkblock() {
        for (uint8_t n = 0; n < DISCOVERY_EXPAND_ROUNDS; n++) {
            uint8_t salt[32];
            { SHA256 sha; sha.reset(); sha.update(infohash, 32); sha.update(&n, 1);   // msgpack positive fixint == n
              sha.finalize(salt, 32); }
            hkdf256(infohash, 32, salt, workblock + (uint16_t)n * DISCOVERY_HKDF_LEN);
        }
    }

    // RFC 5869 HKDF-SHA256, 256 output bytes (8 blocks), empty info.
    static void hkdf256(const uint8_t* ikm, uint16_t ikmLen, const uint8_t salt[32], uint8_t* out) {
        uint8_t prk[32];
        RNSIdentity::hmacSha256(salt, 32, ikm, ikmLen, prk);
        uint8_t prev[32]; uint16_t prevLen = 0;
        for (uint8_t i = 0; i < DISCOVERY_HKDF_LEN / 32; i++) {
            uint8_t msg[33]; uint16_t m = 0;
            if (prevLen) { memcpy(msg, prev, 32); m = 32; }
            msg[m++] = (uint8_t)(i + 1);
            RNSIdentity::hmacSha256(prk, 32, msg, m, prev);
            prevLen = 32;
            memcpy(out + (uint16_t)i * 32, prev, 32);
        }
    }
};
