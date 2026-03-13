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

#ifndef NATIVE_TEST
static uint32_t announceDebugCounter = 0;
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
#ifndef NATIVE_TEST
    const uint32_t dbgId = ++announceDebugCounter;
    const uint32_t t0 = millis();
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.println(F(" STEP0 enter sendLocalAnnounce"));
#endif

    if (!radio || !identity || !identity->initialized) return false;

    if (!announceHashCacheReady) {
#ifndef NATIVE_TEST
        Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
        Serial.println(F(" STEP1 build hash cache"));
#endif
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

#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" STEP2 appDataLen=")); Serial.print(announceAppDataLen);
    Serial.print(F(" name='")); Serial.print(announceName); Serial.println(F("'"));
#endif

    const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
    const uint16_t sigOffset = baseLen + announceAppDataLen;
    const uint16_t totalDataLen = sigOffset + RNS_SIGLENGTH;
    if (totalDataLen > RNS_MTU) return false;

#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" STEP3 lens base=")); Serial.print(baseLen);
    Serial.print(F(" sigOff=")); Serial.print(sigOffset);
    Serial.print(F(" total=")); Serial.println(totalDataLen);
#endif

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

    // [optional appData]
    if (announceAppData && announceAppDataLen > 0) {
        memcpy(announceData + baseLen, announceAppData, announceAppDataLen);
    }

    // [signature 64B]
#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.println(F(" STEP4 signing"));
#endif
    identity->sign(announceData, sigOffset, announceData + sigOffset);

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
#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.println(F(" STEP5 serialize"));
#endif
    uint16_t outLen = pkt.serialize(outBuf, RNS_MTU);
    if (outLen == 0) return false;

#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" STEP6 serialized outLen=")); Serial.println(outLen);
#endif

    int8_t originalTx = radio->curTxDbm;
    int8_t announceTx = (originalTx > LORA_TX_DBM_ANNOUNCE_SAFE)
        ? LORA_TX_DBM_ANNOUNCE_SAFE
        : originalTx;
#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" STEP7 tx original=")); Serial.print(originalTx);
    Serial.print(F(" capped=")); Serial.println(announceTx);
#endif
    if (announceTx != originalTx) {
        radio->setTxPower(announceTx);
#ifndef NATIVE_TEST
        Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
        Serial.println(F(" STEP8 tx power capped applied"));
#endif
    }

#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.println(F(" STEP9 radio->transmit begin"));
#endif
    bool ok = radio->transmit(outBuf, outLen);

#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" STEP10 radio->transmit end ok=")); Serial.println(ok ? F("true") : F("false"));
#endif

    if (announceTx != originalTx) {
        radio->setTxPower(originalTx);
#ifndef NATIVE_TEST
        Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
        Serial.println(F(" STEP11 tx power restored"));
#endif
    }

    if (ok) {
        stats.txPackets++;
#ifndef NATIVE_TEST
        Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
        Serial.print(F(" DONE success dtMs=")); Serial.println(millis() - t0);
#endif
        return true;
    }
#ifndef NATIVE_TEST
    Serial.print(F("[ANNDBG] #")); Serial.print(dbgId);
    Serial.print(F(" DONE fail dtMs=")); Serial.println(millis() - t0);
#endif
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
