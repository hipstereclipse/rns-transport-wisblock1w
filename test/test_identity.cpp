/**
 * @file test_identity.cpp
 * Compile: g++ -std=c++17 -DNATIVE_TEST -I../include test_identity.cpp -o test_identity
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

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
    inline void generatePrivateKey(uint8_t s[32]){
        for(int i=0;i<32;i++)s[i]=0xB0+(i&0xF);
    }
    inline void derivePublicKey(uint8_t p[32],const uint8_t s[32]){
        for(int i=0;i<32;i++)p[i]=s[i]^0x11;
    }
    inline void sign(uint8_t sig[64],const uint8_t*,const uint8_t*,const uint8_t*,size_t){memset(sig,0x33,64);}
}
namespace Curve25519 {
    inline bool dh1(uint8_t p[32],uint8_t s[32]){
        for(int i=0;i<32;i++)p[i]=0xC0+(i&0xF);
        for(int i=0;i<32;i++)s[i]=0xD0+(i&0xF);
        return true;
    }
}

#include "RNSIdentity.h"

static int ok=0,bad=0;
static void chk(bool c,const char*m){printf("  %s %s\n",c?"PASS":"FAIL",m);c?ok++:bad++;}

int main(){
    printf("=== RNSIdentity Tests ===\n");

    // Generate
    {
        RNSIdentity id;
        chk(!id.initialized, "uninit");
        id.generate();
        chk(id.initialized, "init after gen");
        bool allZero=true;
        for(int i=0;i<RNS_ADDR_LEN;i++) if(id.identityHash[i]!=0)allZero=false;
        chk(!allZero, "hash non-zero");
    }

    // Export/load roundtrip
    {
        RNSIdentity orig; orig.generate();
        uint8_t pe[32],ps[64],ue[32],us[32];
        orig.exportKeys(pe,ps,ue,us);
        RNSIdentity loaded; loaded.load(pe,ps,ue,us);
        chk(memcmp(orig.identityHash,loaded.identityHash,RNS_ADDR_LEN)==0, "roundtrip hash");
        chk(memcmp(orig.pubEncKey,loaded.pubEncKey,32)==0, "roundtrip enckey");
        chk(memcmp(orig.pubSigKey,loaded.pubSigKey,32)==0, "roundtrip sigkey");
    }

    // Combined pubkey
    {
        RNSIdentity id; id.generate();
        uint8_t combined[RNS_KEYSIZE];
        id.getPublicKey(combined);
        chk(memcmp(combined,id.pubEncKey,32)==0, "combined enc");
        chk(memcmp(combined+32,id.pubSigKey,32)==0, "combined sig");
    }

    // Validate announce edge cases
    {
        uint8_t short_data[10]={};
        uint8_t dummy_hash[RNS_ADDR_LEN]={};
        chk(!RNSIdentity::validateAnnounce(dummy_hash, short_data,10), "reject short ann");
        chk(!RNSIdentity::validateAnnounce(dummy_hash, nullptr,200), "reject null ann");
    }

    printf("\n%d passed, %d failed\n",ok,bad);
    return bad>0?1:0;
}
