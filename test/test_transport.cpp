/**
 * @file test_transport.cpp
 * Compile: g++ -std=c++17 -DNATIVE_TEST -I../include test_transport.cpp -o test_transport
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>

#ifndef NATIVE_TEST
#define NATIVE_TEST
#endif

#define private public

class SHA256 {
    uint8_t buf[4096]; size_t len=0;
public:
    void reset(){len=0;}
    void update(const void*d,size_t n){if(len+n<=sizeof(buf)){memcpy(buf+len,d,n);len+=n;}}
    void finalize(uint8_t*o,size_t ol){memset(o,0,ol);for(size_t i=0;i<len;i++)o[i%ol]^=buf[i];}
};
namespace Ed25519 {
    inline bool verify(const uint8_t* sig,const uint8_t*,const uint8_t*,size_t){
        if (!sig) return false;
        for(int i=0;i<64;i++) if(sig[i]!=0x33) return false;
        return true;
    }
    inline void generatePrivateKey(uint8_t s[32]){memset(s,0x22,32);}
    inline void derivePublicKey(uint8_t p[32],const uint8_t s[32]){for(int i=0;i<32;i++)p[i]=s[i]^0x11;}
    inline void sign(uint8_t sig[64],const uint8_t*,const uint8_t*,const uint8_t*,size_t){memset(sig,0x33,64);}
}
namespace Curve25519 {
    inline bool dh1(uint8_t p[32],uint8_t s[32]){memset(p,0x44,32);memset(s,0x55,32);return true;}
}

#include "RNSPacket.h"
#include "RNSIdentity.h"
#include "RNSTransport.h"

#undef private

class RNSRadio {
public:
    bool hwReady = true;
    uint8_t lastTx[RNS_MTU] = {0};
    uint16_t lastTxLen = 0;
    bool transmit(const uint8_t* data,uint16_t len){
        if(!data || len > sizeof(lastTx)) return false;
        memcpy(lastTx, data, len);
        lastTxLen = len;
        return true;
    }
};

void RNSTransport::forwardPacket(RNSPacket&,PathEntry*){stats.fwdPackets++;stats.txPackets++;}
void RNSTransport::processAnnounceQueue(uint32_t){}

static int ok=0,bad=0;
static void chk(bool c,const char*m){printf("  %s %s\n",c?"PASS":"FAIL",m);c?ok++:bad++;}

int main(){
    printf("=== RNSTransport Tests ===\n");
    RNSIdentity id; id.generate();
    RNSRadio rad;

    // Insert path
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        uint8_t h[16]; memset(h,0xAA,16);
        t.updatePath(h,nullptr,3);
        chk(t.lookupPath(h)!=nullptr, "insert lookup");
        chk(t.lookupPath(h)->hops==3, "insert hops");
        chk(t.countActivePaths()==1, "insert count");
    }

    // Update shorter
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        uint8_t h[16]; memset(h,0xBB,16);
        t.updatePath(h,nullptr,5);
        t.updatePath(h,nullptr,2);
        chk(t.lookupPath(h)->hops==2, "shorter wins");
        chk(t.countActivePaths()==1, "no dup");
    }

    // Reject longer
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        uint8_t h[16]; memset(h,0xCC,16);
        t.updatePath(h,nullptr,2);
        t.updatePath(h,nullptr,5);
        chk(t.lookupPath(h)->hops==2, "longer rejected");
    }

    // Dedup
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        uint8_t h[16]; memset(h,0xDD,16);
        chk(!t.isDuplicate(h), "not dup before");
        t.recordHash(h);
        chk(t.isDuplicate(h), "dup after");
    }

    // Eviction
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        for(int i=0;i<PATH_TABLE_MAX;i++){
            uint8_t h[16]; memset(h,(uint8_t)i,16);
            t.updatePath(h,nullptr,1);
        }
        chk(t.countActivePaths()==PATH_TABLE_MAX, "full table");
        uint8_t extra[16]; memset(extra,0xFF,16);
        t.updatePath(extra,nullptr,1);
        chk(t.countActivePaths()==PATH_TABLE_MAX, "still full");
        chk(t.lookupPath(extra)!=nullptr, "extra found");
    }

    // Lookup miss
    {
        RNSTransport t; t.begin(&id,(RNSRadio*)&rad);
        uint8_t h[16]; memset(h,0xEE,16);
        chk(t.lookupPath(h)==nullptr, "miss returns null");
    }

    // Parse standard LXMF message layout: dest + source + sig + payload
    {
        uint8_t payload[64] = {0};
        uint16_t payloadLen = RNSTransport::packLxmfContent("test", 1234, payload, sizeof(payload));
        chk(payloadLen > 0, "lxmf payload packed");

        uint8_t lxmf[160] = {0};
        memset(lxmf, 0xA1, 16);
        memset(lxmf+16, 0xB2, 16);
        memset(lxmf+32, 0x33, 64);
        memcpy(lxmf+96, payload, payloadLen);

        uint8_t src[16] = {0};
        char msg[32] = {0};
        bool okParse = RNSTransport::parseLxmfMessage(lxmf, (uint16_t)(96+payloadLen), src, msg, sizeof(msg));
        chk(okParse, "parse standard lxmf bytes");
        chk(memcmp(src, lxmf+16, 16) == 0, "extract source hash");
        chk(strcmp(msg, "test") == 0, "extract lxmf content");
    }

    // Loop back a standard announce and learn peer metadata
    {
        RNSIdentity localId; localId.generate();
        RNSIdentity remoteId; remoteId.generate();
        RNSRadio txRadio, rxRadio;
        RNSTransport sender; sender.begin(&remoteId, (RNSRadio*)&txRadio); sender.initDestHashCache(); sender.setAnnounceName("Remote");
        RNSTransport receiver; receiver.begin(&localId, (RNSRadio*)&rxRadio); receiver.initDestHashCache();

        chk(sender.sendLocalAnnounce(), "send standard announce");
        chk(txRadio.lastTxLen > 0, "capture standard announce");
        receiver.ingestPacket(txRadio.lastTx, txRadio.lastTxLen);
        chk(receiver.getStats().announces == 1, "count standard announce");
        chk(receiver.countActivePaths() == 1, "learn path from standard announce");
        chk(receiver.lookupPeerByName("Remote") != nullptr, "learn peer name from standard announce");
    }

    // Accept legacy fixed-signature announce layout for backwards compatibility
    {
        RNSIdentity localId; localId.generate();
        RNSIdentity remoteId; remoteId.generate();
        RNSRadio rxRadio;
        RNSTransport receiver; receiver.begin(&localId, (RNSRadio*)&rxRadio); receiver.initDestHashCache();

        uint8_t pub[RNS_KEYSIZE] = {0};
        remoteId.getPublicKey(pub);
        uint8_t destHash[RNS_ADDR_LEN] = {0};
        RNSIdentity::computeDestHash(RNS_TRANSPORT_DEST_NAME, pub, destHash);
        const uint8_t appData[] = {0x91, 0xA6, 'L', 'e', 'g', 'a', 'c', 'y'};
        const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        uint8_t raw[256] = {0};
        raw[0] = 0x01;
        raw[1] = 2;
        memcpy(raw + 2, destHash, RNS_ADDR_LEN);
        raw[18] = 0x00;
        uint8_t* data = raw + 19;
        memcpy(data, pub, RNS_KEYSIZE);
        memset(data + RNS_KEYSIZE, 0x55, RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN);
        memset(data + baseLen, 0x33, RNS_SIGLENGTH);
        memcpy(data + baseLen + RNS_SIGLENGTH, appData, sizeof(appData));
        uint16_t rawLen = (uint16_t)(19 + baseLen + RNS_SIGLENGTH + sizeof(appData));

        receiver.ingestPacket(raw, rawLen);
        chk(receiver.getStats().announces == 1, "count legacy announce");
        chk(receiver.countActivePaths() == 1, "learn path from legacy announce");
        chk(receiver.lookupPeerByName("Legacy") != nullptr, "learn peer name from legacy announce");
    }

    printf("\n%d passed, %d failed\n",ok,bad);
    return bad>0?1:0;
}
