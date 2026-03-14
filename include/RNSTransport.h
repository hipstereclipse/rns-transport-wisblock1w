/**
 * @file RNSTransport.h
 * @brief Reticulum transport engine: path table, dedup cache, announce
 *        queue, packet ingestion/forwarding, and entry expiry.
 *
 * All data structures are statically allocated. No heap after begin().
 */
#pragma once
#include "RNSConfig.h"
#include "RNSPacket.h"
#include "RNSIdentity.h"
#include <string.h>

class RNSRadio;  // forward declaration

// ── Path table entry ──────────────────────────────────────
struct PathEntry {
    uint8_t  destHash[RNS_ADDR_LEN];
    uint8_t  nextHop[RNS_ADDR_LEN];   ///< Transport ID of next hop (zero = direct)
    uint8_t  hops;
    uint32_t learnedAt;
    uint32_t expiresAt;
    bool     active;
};

// ── Dedup hash-cache entry ────────────────────────────────
struct HashCacheEntry {
    uint8_t  packetHash[RNS_ADDR_LEN];
    uint32_t seenAt;
    bool     active;
};

// ── Announce retransmission queue entry ───────────────────
struct AnnounceQueueEntry {
    uint8_t  raw[RNS_MTU];
    uint16_t rawLen;
    uint32_t scheduledAt;
    bool     pending;
};

// ── Aggregate counters ────────────────────────────────────
struct TransportStats {
    uint32_t rxPackets      = 0;
    uint32_t txPackets      = 0;
    uint32_t fwdPackets     = 0;
    uint32_t announces      = 0;
    uint32_t duplicates     = 0;
    uint32_t invalidPackets = 0;
    uint32_t pathEntries    = 0;
};

class RNSTransport {
public:
    RNSIdentity*   identity = nullptr;
    RNSRadio*      radio    = nullptr;
    TransportStats stats;

private:
    PathEntry          pathTable[PATH_TABLE_MAX];
    HashCacheEntry     hashCache[HASH_CACHE_MAX];
    AnnounceQueueEntry announceQueue[ANNOUNCE_QUEUE_MAX];
    char               announceName[RNS_ANNOUNCE_NAME_MAX + 1] = "RatTunnel";
    uint8_t            cachedTransportDestHash[RNS_ADDR_LEN] = {0};
    uint8_t            cachedTransportNameHash[RNS_NAME_HASH_LEN] = {0};
    bool               announceHashCacheReady = false;

public:
    /**
     * @brief Initialize the transport engine.
     * Must be called once during setup() after identity and radio are ready.
     */
    void begin(RNSIdentity* id, RNSRadio* rad) {
        identity = id;
        radio    = rad;
        memset(pathTable,     0, sizeof(pathTable));
        memset(hashCache,     0, sizeof(hashCache));
        memset(announceQueue, 0, sizeof(announceQueue));
        strncpy(announceName, "RatTunnel", sizeof(announceName) - 1);
        announceName[sizeof(announceName) - 1] = '\0';
        announceHashCacheReady = false;
    }

    bool setAnnounceName(const char* name) {
        if (!name) return false;

        while (*name == ' ') name++;
        if (*name == '\0') return false;

        char cleaned[RNS_ANNOUNCE_NAME_MAX + 1];
        size_t outPos = 0;
        for (size_t i = 0; name[i] != '\0' && outPos < RNS_ANNOUNCE_NAME_MAX; i++) {
            char c = name[i];
            if (c == '\r' || c == '\n' || c == '\t') continue;
            cleaned[outPos++] = c;
        }

        while (outPos > 0 && cleaned[outPos - 1] == ' ') outPos--;
        if (outPos == 0) return false;

        cleaned[outPos] = '\0';
        strncpy(announceName, cleaned, sizeof(announceName) - 1);
        announceName[sizeof(announceName) - 1] = '\0';
        return true;
    }

    const char* getAnnounceName() const { return announceName; }

    /** @brief Run from main loop at TRANSPORT_LOOP_MS intervals. */
    void loop() {
        uint32_t now = millis();
        processAnnounceQueue(now);
        expireEntries(now);
    }

    // ── Primary ingestion entry-point ─────────────────────
    /**
     * @brief Accept raw bytes from the radio and route them.
     * Called by RNSRadio::poll() when a valid packet arrives.
     */
    void ingestPacket(const uint8_t* buf, uint16_t len) {
        RNSPacket pkt;
        if (!pkt.parse(buf, len)) {
            stats.invalidPackets++;
            return;
        }
        stats.rxPackets++;

        if (isDuplicate(pkt.packetHash)) {
            stats.duplicates++;
            return;
        }
        recordHash(pkt.packetHash);

        if (pkt.isAnnounce()) {
            handleAnnounce(pkt);
        } else if (pkt.isData() || pkt.isLinkRequest() || pkt.isProof()) {
            handleRoutable(pkt);
        }
    }

    // ── Announce handling ─────────────────────────────────
    void handleAnnounce(RNSPacket& pkt) {
        const uint16_t minAnnounce = RNS_KEYSIZE + RNS_NAME_HASH_LEN
                                   + RNS_RANDOM_BLOB_LEN + RNS_SIGLENGTH;
        if (pkt.dataLen < minAnnounce) {
            stats.invalidPackets++;
            return;
        }

        if (!RNSIdentity::validateAnnounce(pkt.data, pkt.dataLen)) {
            stats.invalidPackets++;
            return;
        }
        stats.announces++;

#ifndef NATIVE_TEST
        const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        const uint16_t sigLen = RNS_SIGLENGTH;
        if (pkt.dataLen > (baseLen + sigLen)) {
            const uint16_t appLen = pkt.dataLen - baseLen - sigLen;
            const uint8_t* app = pkt.data + baseLen + sigLen;
            if (app && appLen > 4 && appLen < 96 &&
                app[0] == 'M' && app[1] == 'S' && app[2] == 'G' && app[3] == ':') {
                char msg[97] = {0};
                uint16_t out = 0;
                for (uint16_t i = 4; i < appLen && out < (sizeof(msg) - 1); i++) {
                    uint8_t c = app[i];
                    if (c >= 0x20 && c <= 0x7E) {
                        msg[out++] = (char)c;
                    }
                }
                msg[out] = '\0';
                if (out > 0) {
                    Serial.print(F("[PEERMSG] from "));
                    for (int i = 0; i < 8; i++) {
                        if (pkt.destHash[i] < 0x10) Serial.print('0');
                        Serial.print(pkt.destHash[i], HEX);
                    }
                    Serial.print(F("..: "));
                    Serial.println(msg);
                }
            }
        }
#endif

        // Record path: destHash → arrived here at this hop count
        updatePath(pkt.destHash, nullptr, pkt.hops);

        // Queue retransmission if within hop limit
        if (pkt.hops < RNS_MAX_HOPS) {
            queueAnnounceRetransmit(pkt);
        }
    }

    // ── Routable packet handling ──────────────────────────
    void handleRoutable(RNSPacket& pkt) {
        PathEntry* path = lookupPath(pkt.destHash);
        if (path) {
            forwardPacket(pkt, path);
        }
        // Unknown destination → silently drop (standard Reticulum behaviour)
    }

    // ── Forward packet (implemented in RNSTransport.cpp) ──
    void forwardPacket(RNSPacket& pkt, PathEntry* path);

    // ── Path table ────────────────────────────────────────
    void updatePath(const uint8_t destHash[RNS_ADDR_LEN],
                    const uint8_t* nextHop, uint8_t hopCount) {
        uint32_t now = millis();

        // Update existing entry if new path is equal or shorter
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (pathTable[i].active &&
                memcmp(pathTable[i].destHash, destHash, RNS_ADDR_LEN) == 0) {
                if (hopCount <= pathTable[i].hops) {
                    pathTable[i].hops      = hopCount;
                    pathTable[i].learnedAt = now;
                    pathTable[i].expiresAt = now + PATH_EXPIRY_MS;
                    if (nextHop) memcpy(pathTable[i].nextHop, nextHop, RNS_ADDR_LEN);
                }
                return;
            }
        }

        // Find an empty slot, or evict oldest
        int slot = -1;
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (!pathTable[i].active) { slot = i; break; }
            if (pathTable[i].learnedAt < oldest) {
                oldest = pathTable[i].learnedAt;
                slot = i;
            }
        }
        if (slot < 0) slot = 0;

        pathTable[slot].active    = true;
        pathTable[slot].hops      = hopCount;
        pathTable[slot].learnedAt = now;
        pathTable[slot].expiresAt = now + PATH_EXPIRY_MS;
        memcpy(pathTable[slot].destHash, destHash, RNS_ADDR_LEN);
        if (nextHop) memcpy(pathTable[slot].nextHop, nextHop, RNS_ADDR_LEN);
        else         memset(pathTable[slot].nextHop, 0, RNS_ADDR_LEN);

        stats.pathEntries = countActivePaths();
    }

    PathEntry* lookupPath(const uint8_t destHash[RNS_ADDR_LEN]) {
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (pathTable[i].active &&
                memcmp(pathTable[i].destHash, destHash, RNS_ADDR_LEN) == 0)
                return &pathTable[i];
        }
        return nullptr;
    }

    uint16_t countActivePaths() const {
        uint16_t n = 0;
        for (int i = 0; i < PATH_TABLE_MAX; i++)
            if (pathTable[i].active) n++;
        return n;
    }

    // ── Dedup cache ───────────────────────────────────────
    bool isDuplicate(const uint8_t hash[RNS_ADDR_LEN]) {
        for (int i = 0; i < HASH_CACHE_MAX; i++) {
            if (hashCache[i].active &&
                memcmp(hashCache[i].packetHash, hash, RNS_ADDR_LEN) == 0)
                return true;
        }
        return false;
    }

    void recordHash(const uint8_t hash[RNS_ADDR_LEN]) {
        int slot = -1;
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < HASH_CACHE_MAX; i++) {
            if (!hashCache[i].active) { slot = i; break; }
            if (hashCache[i].seenAt < oldest) {
                oldest = hashCache[i].seenAt;
                slot = i;
            }
        }
        if (slot < 0) slot = 0;

        hashCache[slot].active = true;
        hashCache[slot].seenAt = millis();
        memcpy(hashCache[slot].packetHash, hash, RNS_ADDR_LEN);
    }

    // ── Announce retransmit queue ─────────────────────────
    void queueAnnounceRetransmit(RNSPacket& pkt) {
        for (int i = 0; i < ANNOUNCE_QUEUE_MAX; i++) {
            if (!announceQueue[i].pending) {
                uint8_t newRaw[RNS_MTU];
                memcpy(newRaw, pkt.raw, pkt.rawLen);
                newRaw[1] = pkt.hops + 1;   // increment hop counter

                memcpy(announceQueue[i].raw, newRaw, pkt.rawLen);
                announceQueue[i].rawLen      = pkt.rawLen;
                announceQueue[i].scheduledAt = millis() + random(100, ANNOUNCE_JITTER_MS);
                announceQueue[i].pending     = true;
                return;
            }
        }
        // Queue full — drop (announce flood protection)
    }

    void processAnnounceQueue(uint32_t now);  // in RNSTransport.cpp

    // ── Local announce emission ──────────────────────────
    bool sendLocalAnnounce(const uint8_t* nameHash = nullptr,
                           const uint8_t* appData = nullptr,
                           uint16_t appDataLen = 0);

    // ── Expiry sweep ──────────────────────────────────────
    void expireEntries(uint32_t now) {
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (pathTable[i].active && now > pathTable[i].expiresAt)
                pathTable[i].active = false;
        }
        for (int i = 0; i < HASH_CACHE_MAX; i++) {
            if (hashCache[i].active &&
                (now - hashCache[i].seenAt) > DEDUP_EXPIRY_MS)
                hashCache[i].active = false;
        }
        stats.pathEntries = countActivePaths();
    }

    // ── Accessors for console / persistence ───────────────
    const PathEntry*       getPathTable() const { return pathTable; }
    const TransportStats&  getStats()     const { return stats; }
    PathEntry*             getPathTableMut()    { return pathTable; }
};
