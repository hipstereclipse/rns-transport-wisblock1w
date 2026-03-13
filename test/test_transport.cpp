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

class SHA256 {
    uint8_t buf[4096]; size_t len=0;
public:
    void reset(){len=0;}
    void update(const void*d,size_t n){if(len+n<=sizeof(buf)){memcpy(buf+len,d,n);len+=n;}}
    void finalize(uint8_t*o,size_t ol){memset(o,0,ol);for(size_t i=0;i<len;i++)o[i%ol]^=buf[i];}
};
namespace Ed25519 {
    inline bool verify(const uint8_t*,const uint8_t*,const uint8_t*,size_t){return true;}
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

class RNSRadio {
public:
    bool transmit(const uint8_t*,uint16_t){return true;}
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

    printf("\n%d passed, %d failed\n",ok,bad);
    return bad>0?1:0;
}
