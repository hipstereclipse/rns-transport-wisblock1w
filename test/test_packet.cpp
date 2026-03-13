/**
 * @file test_packet.cpp
 * Compile: g++ -std=c++17 -DNATIVE_TEST -I../include test_packet.cpp -o test_packet
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>

#ifndef NATIVE_TEST
#define NATIVE_TEST
#endif

class SHA256 {
    uint8_t buf[4096]; size_t len = 0;
public:
    void reset() { len = 0; }
    void update(const void* d, size_t n) { if(len+n<=sizeof(buf)){memcpy(buf+len,d,n);len+=n;} }
    void finalize(uint8_t* out, size_t ol) { memset(out,0,ol); for(size_t i=0;i<len;i++) out[i%ol]^=buf[i]; }
};

#include "RNSPacket.h"

static int ok=0, bad=0;
static void chk(bool c, const char* m) { printf("  %s %s\n", c?"PASS":"FAIL", m); c?ok++:bad++; }

int main() {
    printf("=== RNSPacket Tests ===\n");

    // HEADER_1 DATA
    {
        uint8_t pkt[64]={};
        pkt[0]=0x00; pkt[1]=3;
        for(int i=0;i<16;i++) pkt[2+i]=0xAA+i;
        pkt[18]=0x42; pkt[19]=0xDE; pkt[20]=0xAD;
        RNSPacket p;
        chk(p.parse(pkt,21), "H1 parse");
        chk(p.headerType==HEADER_1, "H1 type");
        chk(p.hops==3, "H1 hops");
        chk(p.context==0x42, "H1 ctx");
        chk(p.dataLen==2, "H1 dlen");
        chk(p.data[0]==0xDE, "H1 data");
    }

    // HEADER_2 TRANSPORT
    {
        uint8_t pkt[80]={};
        pkt[0]=(1<<6)|(1<<4); pkt[1]=5;
        for(int i=0;i<16;i++) pkt[2+i]=0x10+i;
        for(int i=0;i<16;i++) pkt[18+i]=0x20+i;
        pkt[34]=0x99; pkt[35]=0xFF;
        RNSPacket p;
        chk(p.parse(pkt,36), "H2 parse");
        chk(p.headerType==HEADER_2, "H2 type");
        chk(p.propType==TRANSPORT, "H2 prop");
        chk(p.transportId[0]==0x10, "H2 tid");
        chk(p.destHash[0]==0x20, "H2 dest");
    }

    // ANNOUNCE
    {
        uint8_t pkt[256]={};
        pkt[0]=0x01; pkt[1]=0;
        for(int i=0;i<16;i++) pkt[2+i]=0xBB;
        pkt[18]=0x00;
        for(int i=0;i<148;i++) pkt[19+i]=(uint8_t)(i&0xFF);
        RNSPacket p;
        chk(p.parse(pkt,167), "ANN parse");
        chk(p.isAnnounce(), "ANN type");
        chk(p.dataLen==148, "ANN dlen");
    }

    // Roundtrip
    {
        uint8_t orig[64]={};
        orig[0]=0x00; orig[1]=7;
        for(int i=0;i<16;i++) orig[2+i]=0xCC+i;
        orig[18]=0x55; orig[19]=0xAB; orig[20]=0xCD;
        RNSPacket p;
        chk(p.parse(orig,21), "RT parse");
        uint8_t out[64];
        uint16_t n = p.serialize(out,sizeof(out));
        chk(n==21, "RT len");
        chk(memcmp(orig,out,21)==0, "RT match");
    }

    // Reject invalid
    {
        RNSPacket p;
        uint8_t s[5]={};
        chk(!p.parse(s,5), "reject short");
        uint8_t l[600]={};
        chk(!p.parse(l,600), "reject long");
        chk(!p.parse(nullptr,100), "reject null");
    }

    printf("\n%d passed, %d failed\n", ok, bad);
    return bad>0?1:0;
}
