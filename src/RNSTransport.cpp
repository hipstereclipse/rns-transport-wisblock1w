/**
 * @file RNSTransport.cpp
 * @brief Implementations of RNSTransport methods that need the radio
 *        object (forward declarations break header-only builds).
 */
#include "RNSTransport.h"
#include "RNSRadio.h"

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
