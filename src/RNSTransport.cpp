/**
 * @file RNSTransport.cpp
 * @brief Implementations of RNSTransport methods that need the radio
 *        object (forward declarations break header-only builds).
 */
#include "RNSTransport.h"
#include "RNSRadio.h"
#ifndef NATIVE_TEST
#include <SHA256.h>
#endif

static void computeNameHash10(const char* fullName, uint8_t outHash[RNS_NAME_HASH_LEN]) {
#ifndef NATIVE_TEST
    SHA256 sha;
    sha.reset();
    sha.update(fullName, strlen(fullName));
    uint8_t digest[32];
    sha.finalize(digest, sizeof(digest));
    memcpy(outHash, digest, RNS_NAME_HASH_LEN);
#else
    memset(outHash, 0, RNS_NAME_HASH_LEN);
    (void)fullName;
#endif
}

static uint16_t encodeAnnounceNameMsgPack(const char* name, uint8_t* out, uint16_t outMax) {
    if (!name || !out || outMax < 2) return 0;
    size_t len = strlen(name);
    if (len == 0) return 0;
    if (len > 31) len = 31;
    if ((uint16_t)(2 + len) > outMax) return 0;

    out[0] = 0x91;
    out[1] = (uint8_t)(0xA0 | (uint8_t)len);
    memcpy(out + 2, name, len);
    return (uint16_t)(2 + len);
}

bool RNSTransport::sendLocalAnnounce(const uint8_t* nameHash,
                                     const uint8_t* appData,
                                     uint16_t appDataLen) {
    if (!radio || !identity || !identity->initialized) return false;
    if (!radio->hwReady) {
        Serial.println(F("[DIAG] sendLocalAnnounce: radio HW not ready, skipping"));
        return false;
    }

    if (!announceHashCacheReady) {
        uint8_t publicKeyForHash[RNS_KEYSIZE];
        identity->getPublicKey(publicKeyForHash);
        RNSIdentity::computeDestHash(RNS_TRANSPORT_DEST_NAME, publicKeyForHash, cachedTransportDestHash);
        computeNameHash10(RNS_TRANSPORT_DEST_NAME, cachedTransportNameHash);
        announceHashCacheReady = true;
    }

    const uint8_t* announceAppData = appData;
    uint16_t announceAppDataLen = appDataLen;
    uint8_t msgpackName[34];
    if (!announceAppData) {
        announceAppDataLen = encodeAnnounceNameMsgPack(announceName, msgpackName, sizeof(msgpackName));
        announceAppData = (announceAppDataLen > 0) ? msgpackName : nullptr;
    }

    const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
    const uint16_t totalDataLen = baseLen + RNS_SIGLENGTH + announceAppDataLen;
    if (totalDataLen > RNS_MTU) return false;

    static uint8_t announceData[RNS_MTU];
    memset(announceData, 0, totalDataLen);

    // [pubkey 64B]
    identity->getPublicKey(announceData);

    // [nameHash 10B]
    if (nameHash) {
        memcpy(announceData + RNS_KEYSIZE, nameHash, RNS_NAME_HASH_LEN);
    } else {
        memcpy(announceData + RNS_KEYSIZE, cachedTransportNameHash, RNS_NAME_HASH_LEN);
    }

    // [randomBlob 10B]
    for (uint16_t i = 0; i < RNS_RANDOM_BLOB_LEN; i++) {
        announceData[RNS_KEYSIZE + RNS_NAME_HASH_LEN + i] = (uint8_t)random(0, 256);
    }

    // Reticulum wire format: pubkey|nameHash|randomHash|SIGNATURE|appData
    // Signed data = destHash + pubkey + nameHash + randomHash + appData
    // The destHash is prepended per Reticulum spec but NOT in wire payload.
    static uint8_t signedBuf[RNS_MTU];
    memcpy(signedBuf, cachedTransportDestHash, RNS_ADDR_LEN);
    memcpy(signedBuf + RNS_ADDR_LEN, announceData, baseLen);
    if (announceAppData && announceAppDataLen > 0) {
        memcpy(signedBuf + RNS_ADDR_LEN + baseLen, announceAppData, announceAppDataLen);
    }
    identity->sign(signedBuf, RNS_ADDR_LEN + baseLen + announceAppDataLen, announceData + baseLen);

    // [appData after signature]
    if (announceAppData && announceAppDataLen > 0) {
        memcpy(announceData + baseLen + RNS_SIGLENGTH, announceAppData, announceAppDataLen);
    }

    RNSPacket pkt;
    pkt.ifacFlag    = false;
    pkt.headerType  = HEADER_1;
    pkt.contextFlag = false;
    pkt.propType    = BROADCAST;
    pkt.destType    = SINGLE;
    pkt.packetType  = ANNOUNCE;
    pkt.hops        = 0;
    memcpy(pkt.destHash, cachedTransportDestHash, RNS_ADDR_LEN);
    pkt.context = 0x00;
    pkt.data    = announceData;
    pkt.dataLen = totalDataLen;

    static uint8_t outBuf[RNS_MTU];
    uint16_t outLen = pkt.serialize(outBuf, RNS_MTU);
    if (outLen == 0) return false;

    // Transmit at configured TX power (no announce power reduction;
    // full power ensures peers reliably discover this node).
    bool ok = radio->transmit(outBuf, outLen);

    if (ok) {
        stats.txPackets++;
        return true;
    }
    return false;
}

/**
 * @brief Send an announce carrying a MSG: text payload.
 */
bool RNSTransport::sendMessageAnnounce(const char* text) {
    if (!text || !*text) return false;

    // Build "MSG:<text>" appData
    char payload[80] = "MSG:";
    size_t prefixLen = 4;
    size_t textLen = strlen(text);
    if (textLen > sizeof(payload) - prefixLen - 1) textLen = sizeof(payload) - prefixLen - 1;
    memcpy(payload + prefixLen, text, textLen);
    payload[prefixLen + textLen] = '\0';

    return sendLocalAnnounce(nullptr, (const uint8_t*)payload, (uint16_t)(prefixLen + textLen));
}

bool RNSTransport::sendDiscoverySweep() {
    if (!radio || !identity || !identity->initialized) return false;
    if (!radio->hwReady) return false;
    if (!pathReqHashReady) initDestHashCache();
    if (!pathReqHashReady) return false;

    uint8_t requestData[RNS_ADDR_LEN * 2] = {0};
    for (uint8_t i = RNS_ADDR_LEN; i < (RNS_ADDR_LEN * 2); i++) {
        requestData[i] = (uint8_t)random(0, 256);
    }

    RNSPacket pkt;
    pkt.ifacFlag = false;
    pkt.headerType = HEADER_1;
    pkt.contextFlag = false;
    pkt.propType = BROADCAST;
    pkt.destType = PLAIN;
    pkt.packetType = DATA;
    pkt.hops = 0;
    memcpy(pkt.destHash, cachedPathReqDestHash, RNS_ADDR_LEN);
    pkt.context = 0x00;
    pkt.data = requestData;
    pkt.dataLen = sizeof(requestData);

    uint8_t outBuf[RNS_MTU];
    uint16_t outLen = pkt.serialize(outBuf, RNS_MTU);
    if (outLen == 0) return false;

    bool ok = radio->transmit(outBuf, outLen);
    if (ok) stats.txPackets++;
    return ok;
}

/**
 * @brief Send an encrypted LXMF DATA packet to a specific peer.
 *
 * Builds an LXMF message, encrypts with the peer's X25519 public key,
 * and transmits as a Reticulum SINGLE/DATA packet.
 */
bool RNSTransport::sendEncryptedMessage(const char* text, PathEntry* peer) {
    if (!text || !*text || !peer || !peer->hasPubKey) return false;
    if (!radio || !identity || !identity->initialized) return false;
    if (!radio->hwReady) return false;
    if (!announceHashCacheReady) {
        initDestHashCache();
    }
    if (!announceHashCacheReady) return false;

    // 1. Pack LXMF payload: [timestamp, title, content, fields]
    // All large buffers are static to avoid stack overflow (crypto calls are deep)
    static uint8_t packedContent[256];
    uint16_t contentLen = packLxmfContent(text, millis(), packedContent, sizeof(packedContent));
    if (contentLen == 0) return false;

    // 2. Build standard LXMF bytes:
    //    dest_hash + source_hash + signature + msgpack_payload
    static uint8_t hashedPart[RNS_MTU];
    uint16_t hashedLen = 0;
    memcpy(hashedPart + hashedLen, peer->destHash, RNS_ADDR_LEN);
    hashedLen += RNS_ADDR_LEN;
    memcpy(hashedPart + hashedLen, cachedTransportDestHash, RNS_ADDR_LEN);
    hashedLen += RNS_ADDR_LEN;
    memcpy(hashedPart + hashedLen, packedContent, contentLen);
    hashedLen += contentLen;

    static uint8_t messageHash[32];
    {
        SHA256 sha;
        sha.reset();
        sha.update(hashedPart, hashedLen);
        sha.finalize(messageHash, sizeof(messageHash));
    }

    static uint8_t signedBuf[RNS_MTU];
    memcpy(signedBuf, hashedPart, hashedLen);
    memcpy(signedBuf + hashedLen, messageHash, sizeof(messageHash));

    static uint8_t signature[64];
    identity->sign(signedBuf, hashedLen + sizeof(messageHash), signature);

    static uint8_t lxmfPayload[RNS_MTU];
    uint16_t lxmfLen = 0;
    memcpy(lxmfPayload + lxmfLen, peer->destHash, RNS_ADDR_LEN);
    lxmfLen += RNS_ADDR_LEN;
    memcpy(lxmfPayload + lxmfLen, cachedTransportDestHash, RNS_ADDR_LEN);
    lxmfLen += RNS_ADDR_LEN;
    memcpy(lxmfPayload + lxmfLen, signature, sizeof(signature));
    lxmfLen += sizeof(signature);
    memcpy(lxmfPayload + lxmfLen, packedContent, contentLen);
    lxmfLen += contentLen;

    // 3. Compute target identity hash from their public key
    uint8_t targetIdHash[RNS_ADDR_LEN];
    {
        SHA256 sha; sha.reset();
        sha.update(peer->peerPubKey, RNS_KEYSIZE);
        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(targetIdHash, full, RNS_ADDR_LEN);
    }

    // 4. Encrypt full LXMF bytes (X25519 pub is first 32 bytes of peerPubKey)
    static uint8_t encrypted[RNS_MTU];
    uint16_t encLen = RNSIdentity::encrypt(
        peer->peerPubKey, targetIdHash,
        lxmfPayload, lxmfLen,
        encrypted, sizeof(encrypted));
    if (encLen == 0) return false;

    // 5. Build DATA packet
    RNSPacket pkt;
    pkt.ifacFlag    = false;
    pkt.headerType  = HEADER_1;
    pkt.contextFlag = false;
    pkt.propType    = BROADCAST;
    pkt.destType    = SINGLE;
    pkt.packetType  = DATA;
    pkt.hops        = 0;
    memcpy(pkt.destHash, peer->destHash, RNS_ADDR_LEN);
    pkt.context = 0x00;
    pkt.data    = encrypted;
    pkt.dataLen = encLen;

    static uint8_t outBuf[RNS_MTU];
    uint16_t outLen = pkt.serialize(outBuf, RNS_MTU);
    if (outLen == 0) return false;

    bool ok = radio->transmit(outBuf, outLen);
    if (ok) stats.txPackets++;
    return ok;
}

/**
 * @brief Pre-compute the dest hash so we can recognise DATA packets
 *        addressed to us before the first announce is sent.
 */
void RNSTransport::initDestHashCache() {
    if (announceHashCacheReady) return;
    if (!identity || !identity->initialized) return;

    uint8_t publicKeyForHash[RNS_KEYSIZE];
    identity->getPublicKey(publicKeyForHash);
    RNSIdentity::computeDestHash(RNS_TRANSPORT_DEST_NAME, publicKeyForHash, cachedTransportDestHash);
    computeNameHash10(RNS_TRANSPORT_DEST_NAME, cachedTransportNameHash);
    announceHashCacheReady = true;

    // Compute path request PLAIN destination hash
    RNSIdentity::computePlainDestHash("rnstransport.path.request", cachedPathReqDestHash);
    pathReqHashReady = true;
}

/**
 * @brief Forward a routable packet toward its destination.
 *
 * Rewrites HEADER_1/BROADCAST → HEADER_2/TRANSPORT, inserting this
 * node's identity hash as the transport-ID field so the next hop
 * knows who relayed the packet.  Increments the hop counter.
 */
void RNSTransport::forwardPacket(RNSPacket& pkt, PathEntry* path) {
    if (!radio || !identity) return;
    if (pkt.hops >= RNS_MAX_HOPS) return;

    static uint8_t outBuf[RNS_MTU];

    static RNSPacket fwd;
    fwd.ifacFlag    = pkt.ifacFlag;
    fwd.headerType  = HEADER_2;
    fwd.contextFlag = pkt.contextFlag;
    fwd.propType    = TRANSPORT;
    fwd.destType    = pkt.destType;
    fwd.packetType  = pkt.packetType;
    fwd.hops        = pkt.hops + 1;

    // Our identity hash goes in the transport-ID field
    memcpy(fwd.transportId, identity->identityHash, RNS_ADDR_LEN);
    memcpy(fwd.destHash,    pkt.destHash,           RNS_ADDR_LEN);
    fwd.context = pkt.context;
    fwd.data    = pkt.data;
    fwd.dataLen = pkt.dataLen;

    uint16_t outLen = fwd.serialize(outBuf, RNS_MTU);
    if (outLen > 0) {
        if (radio->transmit(outBuf, outLen)) {
            stats.fwdPackets++;
            stats.txPackets++;
        }
    }
}

/**
 * @brief Drain the announce retransmission queue.
 *
 * Queued announces are sent at their scheduled time (millis-based
 * random jitter) to avoid synchronized rebroadcasts.
 */
void RNSTransport::processAnnounceQueue(uint32_t now) {
    if (!radio) return;

    for (int i = 0; i < ANNOUNCE_QUEUE_MAX; i++) {
        if (announceQueue[i].pending && now >= announceQueue[i].scheduledAt) {
            if (radio->transmit(announceQueue[i].raw, announceQueue[i].rawLen)) {
                stats.txPackets++;
            }
            announceQueue[i].pending = false;
        }
    }
}
