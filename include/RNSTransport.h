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
    float    lastRSSI;                ///< RSSI at which this peer was last heard
    float    lastSNR;                 ///< SNR at which this peer was last heard
    char     peerName[PEER_NAME_MAX]; ///< Display name from announce appData
    uint8_t  peerPubKey[RNS_KEYSIZE]; ///< 64-byte public key (X25519+Ed25519) from announce
    bool     hasPubKey;               ///< true if peerPubKey is populated
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
    float          lastRxRSSI = 0.0f;
    float          lastRxSNR  = 0.0f;

private:
    PathEntry          pathTable[PATH_TABLE_MAX];
    HashCacheEntry     hashCache[HASH_CACHE_MAX];
    AnnounceQueueEntry announceQueue[ANNOUNCE_QUEUE_MAX];
    char               announceName[RNS_ANNOUNCE_NAME_MAX + 1] = "RatTunnel";
    uint8_t            cachedTransportDestHash[RNS_ADDR_LEN] = {0};
    uint8_t            cachedTransportNameHash[RNS_NAME_HASH_LEN] = {0};
    uint8_t            cachedPathReqDestHash[RNS_ADDR_LEN] = {0};
    bool               pathReqHashReady = false;
    uint32_t           lastPathReqAnnounceAt = 0;
    bool               announceHashCacheReady = false;

    // ── Msgpack skip helper ───────────────────────────────
    static bool skipMsgpackElem(const uint8_t* d, uint16_t len, uint16_t& p) {
        if (p >= len) return false;
        uint8_t b = d[p++];
        if (b <= 0x7F || b >= 0xE0) return true;
        if ((b & 0xF0) == 0x80) { uint8_t n = b & 0x0F; for (uint8_t i = 0; i < n*2; i++) { if (!skipMsgpackElem(d,len,p)) return false; } return true; }
        if ((b & 0xF0) == 0x90) { uint8_t n = b & 0x0F; for (uint8_t i = 0; i < n; i++) { if (!skipMsgpackElem(d,len,p)) return false; } return true; }
        if ((b & 0xE0) == 0xA0) { p += (b & 0x1F); return p <= len; }
        if (b == 0xC0 || b == 0xC2 || b == 0xC3) return true;
        if (b == 0xC4) { if (p>=len) return false; p += d[p]+1; return p<=len; }
        if (b == 0xC5) { if (p+2>len) return false; uint16_t n=(d[p]<<8)|d[p+1]; p+=2+n; return p<=len; }
        if (b == 0xCA) { p += 4; return p <= len; }
        if (b == 0xCB) { p += 8; return p <= len; }
        if (b >= 0xCC && b <= 0xCF) { static const uint8_t sz[]={1,2,4,8}; p+=sz[b-0xCC]; return p<=len; }
        if (b >= 0xD0 && b <= 0xD3) { static const uint8_t sz[]={1,2,4,8}; p+=sz[b-0xD0]; return p<=len; }
        if (b == 0xD9) { if (p>=len) return false; p+=d[p]+1; return p<=len; }
        if (b == 0xDA) { if (p+2>len) return false; uint16_t n=(d[p]<<8)|d[p+1]; p+=2+n; return p<=len; }
        if (b == 0xDE) { if (p+2>len) return false; uint16_t n=(d[p]<<8)|d[p+1]; p+=2; for(uint16_t i=0;i<n*2;i++){if(!skipMsgpackElem(d,len,p))return false;} return true; }
        if (b == 0xDC) { if (p+2>len) return false; uint16_t n=(d[p]<<8)|d[p+1]; p+=2; for(uint16_t i=0;i<n;i++){if(!skipMsgpackElem(d,len,p))return false;} return true; }
        return false;
    }

    // ── Read msgpack string or bin into char buffer ───────
    static bool readMsgpackStr(const uint8_t* d, uint16_t len,
                               uint16_t& p, char* out, uint16_t maxOut) {
        if (p >= len || maxOut == 0) return false;
        uint8_t b = d[p++];
        uint16_t sLen = 0;
        if ((b & 0xE0) == 0xA0) sLen = b & 0x1F;
        else if (b == 0xD9) { if (p>=len) return false; sLen=d[p++]; }
        else if (b == 0xDA) { if (p+2>len) return false; sLen=(d[p]<<8)|d[p+1]; p+=2; }
        else if (b == 0xC4) { if (p>=len) return false; sLen=d[p++]; }
        else if (b == 0xC5) { if (p+2>len) return false; sLen=(d[p]<<8)|d[p+1]; p+=2; }
        else return false;
        if (p + sLen > len) return false;
        uint16_t cp = (sLen < maxOut-1) ? sLen : maxOut-1;
        uint16_t oi = 0;
        for (uint16_t i = 0; i < cp; i++) {
            uint8_t c = d[p+i];
            if (c >= 0x20 && c <= 0x7E) out[oi++] = (char)c;
        }
        out[oi] = '\0';
        p += sLen;
        return oi > 0;
    }

    // ── Parse decrypted LXMF payload for message text ─────
    static bool parseLxmfMessage(const uint8_t* data, uint16_t len,
                                 uint8_t srcHash[RNS_ADDR_LEN],
                                 char* outText, uint16_t maxText) {
        const uint16_t lxmfOverhead = (2 * RNS_ADDR_LEN) + RNS_SIGLENGTH;
        if (!data || len < lxmfOverhead + 8 || !outText) return false;

        if (srcHash) memcpy(srcHash, data + RNS_ADDR_LEN, RNS_ADDR_LEN);

        uint16_t pos = lxmfOverhead;
        // Parse msgpack array: [timestamp, title, content, fields, ...optional stamp]
        uint8_t arr = data[pos++];
        uint16_t arrLen = 0;
        if ((arr & 0xF0) == 0x90) arrLen = arr & 0x0F;
        else if (arr == 0xDC && pos+2 <= len) { arrLen=(data[pos]<<8)|data[pos+1]; pos+=2; }
        else return false;
        if (arrLen < 4) return false;
        // Skip timestamp
        if (!skipMsgpackElem(data, len, pos)) return false;
        // Skip title
        if (!skipMsgpackElem(data, len, pos)) return false;
        // Read content (message text)
        return readMsgpackStr(data, len, pos, outText, maxText);
    }

    // ── Pack LXMF content as msgpack ──────────────────────
    static uint16_t packLxmfContent(const char* text, uint32_t uptimeMs,
                                    uint8_t* out, uint16_t maxLen) {
        if (!text || !out) return 0;
        uint16_t tLen = strlen(text);
        if (tLen > 200) tLen = 200;
        uint16_t needed = 1 + 9 + 2 + (tLen >= 32 ? 2 : 1) + tLen + 1;
        if (needed > maxLen) return 0;
        uint16_t pos = 0;
        out[pos++] = 0x94; // array of 4
        out[pos++] = 0xCB; // float64 timestamp
        double ts = (double)(uptimeMs / 1000);
        uint8_t* tb = (uint8_t*)&ts;
        for (int i = 7; i >= 0; i--) out[pos++] = tb[i]; // big-endian
        out[pos++] = 0xC4; out[pos++] = 0x00; // empty bin8 title
        if (tLen < 32) { out[pos++] = 0xA0 | (uint8_t)tLen; }
        else { out[pos++] = 0xD9; out[pos++] = (uint8_t)tLen; }
        memcpy(out + pos, text, tLen); pos += tLen;
        out[pos++] = 0x80; // empty fixmap fields
        return pos;
    }

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

    /** @brief Pre-compute dest hash so we can recognise packets for us
     *         before the first announce is sent. Call after begin(). */
    void initDestHashCache();

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
        static RNSPacket pkt;
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
        } else if (pkt.isData() && pkt.destType == PLAIN) {
            handlePlainData(pkt);
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

        if (!RNSIdentity::validateAnnounce(pkt.destHash, pkt.data, pkt.dataLen)) {
            stats.invalidPackets++;
            return;
        }
        stats.announces++;

        // Extract peer name and messages from announce appData
        char extractedName[PEER_NAME_MAX] = {0};
        const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        const uint16_t sigLen = RNS_SIGLENGTH;
#ifndef NATIVE_TEST
        if (pkt.dataLen > (baseLen + sigLen)) {
            const uint16_t appLen = pkt.dataLen - baseLen - sigLen;
            const uint8_t* app = pkt.data + baseLen + sigLen;

            // Check for MSG: prefix (peer message)
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
            // Check for MsgPack-encoded name: fixarray(1+) + fixstr
            // Sideband sends [name] or [name, stamp_cost, ...] as 0x91-0x9F arrays
            else if (app && appLen >= 3 && (app[0] & 0xF0) == 0x90 && (app[0] & 0x0F) >= 1
                     && (app[1] & 0xE0) == 0xA0) {
                uint8_t nameLen = app[1] & 0x1F;
                if (nameLen > 0 && (2 + nameLen) <= appLen && nameLen < PEER_NAME_MAX) {
                    uint8_t outIdx = 0;
                    for (uint8_t ni = 0; ni < nameLen && outIdx < (PEER_NAME_MAX - 1); ni++) {
                        uint8_t c = app[2 + ni];
                        if (c >= 0x20 && c <= 0x7E)
                            extractedName[outIdx++] = (char)c;
                    }
                    extractedName[outIdx] = '\0';
                }
            }
        }
#endif

        // RSSI/SNR from last RX (set by radio before ingestPacket)
        float rxRSSI = lastRxRSSI;
        float rxSNR  = lastRxSNR;

        // Record path: destHash → arrived here at this hop count
        updatePath(pkt.destHash, nullptr, pkt.hops, rxRSSI, rxSNR,
                   extractedName[0] ? extractedName : nullptr,
                   pkt.data);

        // Queue retransmission if within hop limit
        if (pkt.hops < RNS_MAX_HOPS) {
            queueAnnounceRetransmit(pkt);
        }
    }

    // ── Handle PLAIN DATA packets (path requests etc.) ────
    void handlePlainData(RNSPacket& pkt) {
#ifndef NATIVE_TEST
        if (!pathReqHashReady) return;
        if (memcmp(pkt.destHash, cachedPathReqDestHash, RNS_ADDR_LEN) != 0) return;
        // Path request: data = requested_dest_hash(16) + random(16)
        if (pkt.dataLen < RNS_ADDR_LEN) return;
        if (!announceHashCacheReady) return;
        if (memcmp(pkt.data, cachedTransportDestHash, RNS_ADDR_LEN) == 0) {
            // Someone is requesting a path to US — respond with announce
            // Rate-limit: at most one response per 2 seconds
            uint32_t now = millis();
            if (now - lastPathReqAnnounceAt < 2000) return;
            lastPathReqAnnounceAt = now;
            Serial.println(F("[RNS] Path request for us — sending announce"));
            sendLocalAnnounce();
        }
#endif
    }

    // ── Routable packet handling ──────────────────────────
    void handleRoutable(RNSPacket& pkt) {
        // Check if packet is addressed to our own destination
        if (announceHashCacheReady &&
            memcmp(pkt.destHash, cachedTransportDestHash, RNS_ADDR_LEN) == 0) {
            handleLocalDelivery(pkt);
            return;
        }
        PathEntry* path = lookupPath(pkt.destHash);
        if (path) {
            forwardPacket(pkt, path);
        }
        // Unknown destination → silently drop (standard Reticulum behaviour)
    }

    // ── Handle DATA/LINK/PROOF packets addressed to us ────
    void handleLocalDelivery(RNSPacket& pkt) {
#ifndef NATIVE_TEST
        if (pkt.isData() && identity && identity->initialized) {
            static uint8_t plainBuf[RNS_MTU];
            uint16_t plainLen = identity->decrypt(
                pkt.data, pkt.dataLen, plainBuf, sizeof(plainBuf));
            if (plainLen > 0) {
                // Parse LXMF: dest_hash(16) + source_hash(16) + sig(64) + msgpack payload
                static uint8_t srcHash[RNS_ADDR_LEN];
                static char msgText[200];
                memset(srcHash, 0, RNS_ADDR_LEN);
                memset(msgText, 0, sizeof(msgText));
                if (parseLxmfMessage(plainBuf, plainLen, srcHash, msgText, sizeof(msgText))) {
                    Serial.print(F("[PEERMSG] from "));
                    for (int i = 0; i < 8; i++) {
                        if (srcHash[i] < 0x10) Serial.print('0');
                        Serial.print(srcHash[i], HEX);
                    }
                    Serial.print(F("..: "));
                    Serial.println(msgText);
                } else {
                    // Decrypted but couldn't parse LXMF - show raw
                    if (plainLen >= (2 * RNS_ADDR_LEN)) {
                        memcpy(srcHash, plainBuf + RNS_ADDR_LEN, RNS_ADDR_LEN);
                    }
                    Serial.print(F("[PEERMSG] from "));
                    bool haveSrcHash = false;
                    for (int i = 0; i < RNS_ADDR_LEN; i++) {
                        if (srcHash[i] != 0) {
                            haveSrcHash = true;
                            break;
                        }
                    }
                    if (haveSrcHash) {
                        for (int i = 0; i < 8; i++) {
                            if (srcHash[i] < 0x10) Serial.print('0');
                            Serial.print(srcHash[i], HEX);
                        }
                    } else if (pkt.headerType == HEADER_2) {
                        for (int i = 0; i < 8; i++) {
                            if (pkt.transportId[i] < 0x10) Serial.print('0');
                            Serial.print(pkt.transportId[i], HEX);
                        }
                    } else {
                        Serial.print(F("0000000000000000"));
                    }
                    Serial.print(F("..: [decrypted, "));
                    Serial.print(plainLen);
                    Serial.println(F(" bytes]"));
                }
            } else {
                // Decryption failed - show encrypted notification with diagnostics
                Serial.print(F("[PEERMSG] from "));
                if (pkt.headerType == HEADER_2) {
                    for (int i = 0; i < 8; i++) {
                        if (pkt.transportId[i] < 0x10) Serial.print('0');
                        Serial.print(pkt.transportId[i], HEX);
                    }
                } else {
                    Serial.print(F("0000000000000000"));
                }
                Serial.print(F("..: [encrypted, "));
                Serial.print(pkt.dataLen);
                Serial.println(F(" bytes]"));
                identity->decryptDiag(pkt.data, pkt.dataLen,
                    announceHashCacheReady ? cachedTransportDestHash : nullptr);
            }
        } else if (pkt.isData()) {
            Serial.print(F("[PEERMSG] from 0000000000000000..: [encrypted, "));
            Serial.print(pkt.dataLen);
            Serial.println(F(" bytes]"));
        } else {
            const char* typeStr = "UNKNOWN";
            if (pkt.isLinkRequest()) typeStr = "LINK_REQUEST";
            else if (pkt.isProof())  typeStr = "PROOF";
            Serial.print(F("[RNS] Local "));
            Serial.print(typeStr);
            Serial.print(F(" packet received ("));
            Serial.print(pkt.dataLen);
            Serial.println(F(" bytes)"));
        }
#endif
    }

    // ── Forward packet (implemented in RNSTransport.cpp) ──
    void forwardPacket(RNSPacket& pkt, PathEntry* path);

    // ── Path table ────────────────────────────────────────
    void updatePath(const uint8_t destHash[RNS_ADDR_LEN],
                    const uint8_t* nextHop, uint8_t hopCount,
                    float rssi = 0.0f, float snr = 0.0f,
                    const char* name = nullptr,
                    const uint8_t* pubKey = nullptr) {
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
                // Always update signal quality and name
                if (rssi != 0.0f) pathTable[i].lastRSSI = rssi;
                if (snr != 0.0f)  pathTable[i].lastSNR  = snr;
                if (name && name[0]) {
                    strncpy(pathTable[i].peerName, name, PEER_NAME_MAX - 1);
                    pathTable[i].peerName[PEER_NAME_MAX - 1] = '\0';
                }
                if (pubKey) {
                    memcpy(pathTable[i].peerPubKey, pubKey, RNS_KEYSIZE);
                    pathTable[i].hasPubKey = true;
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
        pathTable[slot].lastRSSI  = rssi;
        pathTable[slot].lastSNR   = snr;
        memcpy(pathTable[slot].destHash, destHash, RNS_ADDR_LEN);
        if (nextHop) memcpy(pathTable[slot].nextHop, nextHop, RNS_ADDR_LEN);
        else         memset(pathTable[slot].nextHop, 0, RNS_ADDR_LEN);
        if (name && name[0]) {
            strncpy(pathTable[slot].peerName, name, PEER_NAME_MAX - 1);
            pathTable[slot].peerName[PEER_NAME_MAX - 1] = '\0';
        } else {
            pathTable[slot].peerName[0] = '\0';
        }
        if (pubKey) {
            memcpy(pathTable[slot].peerPubKey, pubKey, RNS_KEYSIZE);
            pathTable[slot].hasPubKey = true;
        } else {
            pathTable[slot].hasPubKey = false;
        }

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
                static uint8_t newRaw[RNS_MTU];
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

    // ── Send a message-bearing announce ──────────────────
    bool sendMessageAnnounce(const char* text);
    // ── Send encrypted DATA message to a known peer ───
    bool sendEncryptedMessage(const char* text, PathEntry* peer);
    // ── Peer lookup by hex prefix ────────────────────────
    PathEntry* lookupPeerByPrefix(const char* hexPrefix) {
        if (!hexPrefix || !*hexPrefix) return nullptr;
        size_t prefixLen = strlen(hexPrefix);
        if (prefixLen > RNS_ADDR_LEN * 2) return nullptr;
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (!pathTable[i].active) continue;
            bool match = true;
            for (size_t j = 0; j < prefixLen && j / 2 < RNS_ADDR_LEN; j++) {
                uint8_t nibble;
                char c = hexPrefix[j];
                if      (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
                else { match = false; break; }
                uint8_t byte = pathTable[i].destHash[j / 2];
                uint8_t actual = (j % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
                if (nibble != actual) { match = false; break; }
            }
            if (match) return &pathTable[i];
        }
        return nullptr;
    }

    // ── Peer lookup by name (case-insensitive prefix) ────
    PathEntry* lookupPeerByName(const char* name) {
        if (!name || !*name) return nullptr;
        size_t nLen = strlen(name);
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (!pathTable[i].active || !pathTable[i].peerName[0]) continue;
            bool match = true;
            for (size_t j = 0; j < nLen; j++) {
                char a = name[j], b = pathTable[i].peerName[j];
                if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
                if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
                if (a != b) { match = false; break; }
            }
            if (match) return &pathTable[i];
        }
        return nullptr;
    }

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
