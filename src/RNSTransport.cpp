/**
 * @file RNSTransport.cpp
 * @brief Implementations of RNSTransport methods that need the radio
 *        object (forward declarations break header-only builds).
 */
#include "RNSTransport.h"
#include "RNSRadio.h"

bool RNSTransport::sendLocalAnnounce(const uint8_t* nameHash,
                                     const uint8_t* appData,
                                     uint16_t appDataLen) {
    if (!radio || !identity || !identity->initialized) return false;

    const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
    const uint16_t sigOffset = baseLen + appDataLen;
    const uint16_t totalDataLen = sigOffset + RNS_SIGLENGTH;
    if (totalDataLen > RNS_MTU) return false;

    static uint8_t announceData[RNS_MTU];
    memset(announceData, 0, totalDataLen);

    // [pubkey 64B]
    identity->getPublicKey(announceData);

    // [nameHash 10B]
    if (nameHash) {
        memcpy(announceData + RNS_KEYSIZE, nameHash, RNS_NAME_HASH_LEN);
    } else {
        memset(announceData + RNS_KEYSIZE, 0, RNS_NAME_HASH_LEN);
    }

    // [randomBlob 10B]
    for (uint16_t i = 0; i < RNS_RANDOM_BLOB_LEN; i++) {
        announceData[RNS_KEYSIZE + RNS_NAME_HASH_LEN + i] = (uint8_t)random(0, 256);
    }

    // [optional appData]
    if (appData && appDataLen > 0) {
        memcpy(announceData + baseLen, appData, appDataLen);
    }

    // [signature 64B]
    identity->sign(announceData, sigOffset, announceData + sigOffset);

    RNSPacket pkt;
    pkt.ifacFlag    = false;
    pkt.headerType  = HEADER_1;
    pkt.contextFlag = false;
    pkt.propType    = BROADCAST;
    pkt.destType    = SINGLE;
    pkt.packetType  = ANNOUNCE;
    pkt.hops        = 0;
    memcpy(pkt.destHash, identity->identityHash, RNS_ADDR_LEN);
    pkt.context = 0x00;
    pkt.data    = announceData;
    pkt.dataLen = totalDataLen;

    uint8_t outBuf[RNS_MTU];
    uint16_t outLen = pkt.serialize(outBuf, RNS_MTU);
    if (outLen == 0) return false;

    if (radio->transmit(outBuf, outLen)) {
        stats.txPackets++;
        return true;
    }
    return false;
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

    uint8_t outBuf[RNS_MTU];

    RNSPacket fwd;
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
