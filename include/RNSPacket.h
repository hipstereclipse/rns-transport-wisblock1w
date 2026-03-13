/**
 * @file RNSPacket.h
 * @brief Reticulum wire-format packet parsing, serialization, and hashing.
 *
 * Supports HEADER_1 (single address) and HEADER_2 (transport, two addresses).
 * All packet types: DATA, ANNOUNCE, LINK_REQUEST, PROOF.
 * Hash uses truncated SHA-256 (16 bytes) for deduplication and path lookups.
 */
#pragma once
#include "RNSConfig.h"
#include <string.h>
#ifndef NATIVE_TEST
#include <SHA256.h>
#endif

// ── Flag-field enumerations ──────────────────────────────
enum RNSHeaderType : uint8_t { HEADER_1 = 0, HEADER_2 = 1 };
enum RNSPropType   : uint8_t { BROADCAST = 0, TRANSPORT = 1 };
enum RNSDestType   : uint8_t { SINGLE = 0, GROUP = 1, PLAIN = 2, LINK = 3 };
enum RNSPacketType : uint8_t { DATA = 0, ANNOUNCE = 1, LINK_REQUEST = 2, PROOF = 3 };

/**
 * @struct RNSPacket
 * @brief In-memory representation of a single Reticulum packet.
 *
 * `raw[]` holds the original wire bytes; header fields are parsed out
 * by `parse()` and can be reconstituted with `serialize()`.
 */
struct RNSPacket {
    // ── Raw wire data ─────────────────────────────────────
    uint8_t  raw[RNS_MTU];
    uint16_t rawLen = 0;

    // ── Parsed header fields ──────────────────────────────
    bool          ifacFlag   = false;
    RNSHeaderType headerType = HEADER_1;
    bool          contextFlag= false;
    RNSPropType   propType   = BROADCAST;
    RNSDestType   destType   = SINGLE;
    RNSPacketType packetType = DATA;
    uint8_t       hops       = 0;

    uint8_t       transportId[RNS_ADDR_LEN];   ///< Valid only when HEADER_2
    uint8_t       destHash[RNS_ADDR_LEN];
    uint8_t       context    = 0;

    const uint8_t* data      = nullptr;         ///< Points into raw[]
    uint16_t       dataLen   = 0;

    uint8_t       packetHash[RNS_ADDR_LEN];     ///< Truncated SHA-256

    // ── Parse raw bytes into fields ───────────────────────
    /**
     * @brief Decode a Reticulum packet from raw bytes.
     * @param buf   Pointer to received bytes.
     * @param len   Number of bytes.
     * @return true on success, false if packet is malformed.
     */
    bool parse(const uint8_t* buf, uint16_t len) {
        if (!buf) return false;
        if (len < RNS_HEADER_SIZE + RNS_ADDR_LEN + 1) return false;
        if (len > RNS_MTU) return false;

        memcpy(raw, buf, len);
        rawLen = len;

        uint8_t flags = raw[0];
        ifacFlag    = (flags >> 7) & 1;
        headerType  = static_cast<RNSHeaderType>((flags >> 6) & 1);
        contextFlag = (flags >> 5) & 1;
        propType    = static_cast<RNSPropType>((flags >> 4) & 1);
        destType    = static_cast<RNSDestType>((flags >> 2) & 3);
        packetType  = static_cast<RNSPacketType>(flags & 3);

        hops = raw[1];

        // Skip IFAC bytes (production would need per-interface config)
        uint16_t pos = 2;

        if (headerType == HEADER_2) {
            if (pos + 2 * RNS_ADDR_LEN > len) return false;
            memcpy(transportId, &raw[pos], RNS_ADDR_LEN); pos += RNS_ADDR_LEN;
            memcpy(destHash,    &raw[pos], RNS_ADDR_LEN); pos += RNS_ADDR_LEN;
        } else {
            if (pos + RNS_ADDR_LEN > len) return false;
            memcpy(destHash, &raw[pos], RNS_ADDR_LEN); pos += RNS_ADDR_LEN;
            memset(transportId, 0, RNS_ADDR_LEN);
        }

        if (pos >= len) return false;
        context = raw[pos]; pos++;

        data    = &raw[pos];
        dataLen = len - pos;

        computeHash();
        return true;
    }

    // ── Serialize back to wire format ─────────────────────
    /**
     * @brief Pack this packet struct back to raw bytes.
     * @param out     Output buffer.
     * @param maxLen  Size of output buffer.
     * @return Number of bytes written, or 0 on error.
     */
    uint16_t serialize(uint8_t* out, uint16_t maxLen) const {
        if (!out || maxLen < RNS_HEADER_SIZE + RNS_ADDR_LEN + 1) return 0;

        uint16_t pos = 0;
        uint8_t flags = ((ifacFlag ? 1 : 0) << 7)
                      | (headerType << 6)
                      | ((contextFlag ? 1 : 0) << 5)
                      | (propType << 4)
                      | (destType << 2)
                      | packetType;
        out[pos++] = flags;
        out[pos++] = hops;

        if (headerType == HEADER_2) {
            if (pos + 2 * RNS_ADDR_LEN + 1 > maxLen) return 0;
            memcpy(&out[pos], transportId, RNS_ADDR_LEN); pos += RNS_ADDR_LEN;
        }
        memcpy(&out[pos], destHash, RNS_ADDR_LEN); pos += RNS_ADDR_LEN;

        out[pos++] = context;

        if (data && dataLen > 0) {
            if (pos + dataLen > maxLen) return 0;
            memcpy(&out[pos], data, dataLen);
            pos += dataLen;
        }
        return pos;
    }

    // ── Compute truncated SHA-256 packet hash ─────────────
    void computeHash() {
        SHA256 sha;
        sha.reset();
        sha.update(raw, rawLen);
        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(packetHash, full, RNS_ADDR_LEN);
    }

    // ── Type helpers ──────────────────────────────────────
    bool isAnnounce()    const { return packetType == ANNOUNCE;      }
    bool isData()        const { return packetType == DATA;          }
    bool isLinkRequest() const { return packetType == LINK_REQUEST;  }
    bool isProof()       const { return packetType == PROOF;         }
    bool isTransport()   const { return propType   == TRANSPORT;     }
};
