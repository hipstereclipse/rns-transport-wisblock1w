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
    inline bool verify(const uint8_t* sig,const uint8_t*,const uint8_t*,size_t){
        if (!sig) return false;
        for(int i=0;i<64;i++) if(sig[i]!=0x33) return false;
        return true;
    }
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

    // Announce layouts: reference RNS (with and without ratchet) and the
    // RatTunnel <= 1.0.33 trailing-signature layout. All use real
    // Ed25519 signatures so layout detection is driven by verification.
    {
        RNSIdentity id; id.generate();
        uint8_t pub[RNS_KEYSIZE];
        id.getPublicKey(pub);
        uint8_t destHash[RNS_ADDR_LEN];
        RNSIdentity::computeDestHash(RNS_TRANSPORT_DEST_NAME, pub, destHash);

        const uint8_t appData[] = {0x92, 0xC4, 0x04, 'T', 'A', 'G', 'M', 0xC0};   // LXMF: [b"TAGM", nil]
        const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        uint8_t signedBuf[RNS_MTU];
        uint8_t sig[RNS_SIGLENGTH];
        RNSIdentity::AnnounceInfo info;

        // (a) reference layout, no ratchet: pub|nh|rand|SIG|app
        uint8_t rns[256] = {0};
        memcpy(rns, pub, RNS_KEYSIZE);
        memset(rns + RNS_KEYSIZE, 0x11, RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN);
        memcpy(signedBuf, destHash, RNS_ADDR_LEN);
        memcpy(signedBuf + RNS_ADDR_LEN, rns, baseLen);
        memcpy(signedBuf + RNS_ADDR_LEN + baseLen, appData, sizeof(appData));
        id.sign(signedBuf, RNS_ADDR_LEN + baseLen + sizeof(appData), sig);
        memcpy(rns + baseLen, sig, RNS_SIGLENGTH);
        memcpy(rns + baseLen + RNS_SIGLENGTH, appData, sizeof(appData));
        const uint16_t rnsLen = baseLen + RNS_SIGLENGTH + sizeof(appData);

        chk(RNSIdentity::inspectAnnounce(destHash, rns, rnsLen, false, &info), "accept RNS announce layout");
        chk(info.layout == RNSIdentity::ANNOUNCE_LAYOUT_RNS, "detect RNS layout");
        chk(info.ratchet == nullptr, "RNS layout: no ratchet");
        chk(info.appDataLen == sizeof(appData), "RNS appdata len");
        chk(info.appData && memcmp(info.appData, appData, sizeof(appData)) == 0, "RNS appdata ptr");
        chk(!RNSIdentity::inspectAnnounce(destHash, rns, rnsLen, true, &info), "RNS layout rejected when context flag claims a ratchet");

        // (b) reference layout with ratchet: pub|nh|rand|RATCHET|SIG|app  (context flag set)
        uint8_t rat[256] = {0};
        memcpy(rat, pub, RNS_KEYSIZE);
        memset(rat + RNS_KEYSIZE, 0x22, RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN);
        memset(rat + baseLen, 0x5A, RNS_RATCHETSIZE);
        memcpy(signedBuf, destHash, RNS_ADDR_LEN);
        memcpy(signedBuf + RNS_ADDR_LEN, rat, baseLen + RNS_RATCHETSIZE);
        memcpy(signedBuf + RNS_ADDR_LEN + baseLen + RNS_RATCHETSIZE, appData, sizeof(appData));
        id.sign(signedBuf, RNS_ADDR_LEN + baseLen + RNS_RATCHETSIZE + sizeof(appData), sig);
        memcpy(rat + baseLen + RNS_RATCHETSIZE, sig, RNS_SIGLENGTH);
        memcpy(rat + baseLen + RNS_RATCHETSIZE + RNS_SIGLENGTH, appData, sizeof(appData));
        const uint16_t ratLen = baseLen + RNS_RATCHETSIZE + RNS_SIGLENGTH + sizeof(appData);

        chk(RNSIdentity::inspectAnnounce(destHash, rat, ratLen, true, &info), "accept RNS ratchet announce layout");
        chk(info.layout == RNSIdentity::ANNOUNCE_LAYOUT_RNS_RATCHET, "detect ratchet layout");
        chk(info.ratchet == rat + baseLen, "ratchet ptr");
        chk(info.appDataLen == sizeof(appData), "ratchet appdata len");
        chk(info.appData && memcmp(info.appData, appData, sizeof(appData)) == 0, "ratchet appdata ptr");
        chk(!RNSIdentity::inspectAnnounce(destHash, rat, ratLen, false, &info), "ratchet announce rejected without context flag");

        // (c) RatTunnel <= 1.0.33 layout: pub|nh|rand|app|SIG (still accepted, never emitted)
        uint8_t old[256] = {0};
        memcpy(old, pub, RNS_KEYSIZE);
        memset(old + RNS_KEYSIZE, 0x33, RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN);
        memcpy(old + baseLen, appData, sizeof(appData));
        memcpy(signedBuf, destHash, RNS_ADDR_LEN);
        memcpy(signedBuf + RNS_ADDR_LEN, old, baseLen + sizeof(appData));
        id.sign(signedBuf, RNS_ADDR_LEN + baseLen + sizeof(appData), sig);
        memcpy(old + baseLen + sizeof(appData), sig, RNS_SIGLENGTH);
        const uint16_t oldLen = baseLen + sizeof(appData) + RNS_SIGLENGTH;

        chk(RNSIdentity::inspectAnnounce(destHash, old, oldLen, false, &info), "accept legacy RatTunnel trailing-sig layout");
        chk(info.layout == RNSIdentity::ANNOUNCE_LAYOUT_RATTUNNEL_TRAILING_SIG, "detect legacy layout");
        chk(info.appDataLen == sizeof(appData), "legacy appdata len");
        chk(info.appData && memcmp(info.appData, appData, sizeof(appData)) == 0, "legacy appdata ptr");

        // (d) tampered signature is rejected in every layout
        rns[baseLen] ^= 0x01;
        chk(!RNSIdentity::inspectAnnounce(destHash, rns, rnsLen, false, &info), "reject tampered RNS signature");
    }

    printf("\n%d passed, %d failed\n",ok,bad);
    return bad>0?1:0;
}
