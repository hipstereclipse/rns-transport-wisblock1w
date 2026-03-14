/**
 * @file RNSIdentity.h
 * @brief Reticulum cryptographic identity: Ed25519 + X25519 keypair,
 *        destination hashing, and announce signature validation.
 *
 * Uses the rweather/Crypto library for all primitives.
 * On nRF52840 at 64 MHz: Ed25519 verify ≈ 340 ms (software).
 */
#pragma once
#include "RNSConfig.h"
#ifndef NATIVE_TEST
#include <Ed25519.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <AES.h>
#endif
#include <string.h>

class RNSIdentity {
public:
    enum AnnounceLayout : uint8_t {
        ANNOUNCE_LAYOUT_STANDARD = 0,
        ANNOUNCE_LAYOUT_LEGACY_FIXED_SIG = 1,
    };

    struct AnnounceInfo {
        const uint8_t* signature = nullptr;
        const uint8_t* signingKey = nullptr;
        const uint8_t* appData = nullptr;
        uint16_t sigOffset = 0;
        uint16_t appDataLen = 0;
        AnnounceLayout layout = ANNOUNCE_LAYOUT_STANDARD;
    };

    uint8_t pubEncKey[32];    ///< X25519 public
    uint8_t privEncKey[32];   ///< X25519 private
    uint8_t pubSigKey[32];    ///< Ed25519 public
    uint8_t privSigKey[64];   ///< Ed25519 private (rweather uses 64 B)

    uint8_t identityHash[RNS_ADDR_LEN];  ///< SHA-256(pubkey)[:16]
    bool    initialized = false;

    // ── Generate a fresh random identity ──────────────────
    void generate() {
        Curve25519::dh1(pubEncKey, privEncKey);
        Ed25519::generatePrivateKey(privSigKey);
        Ed25519::derivePublicKey(pubSigKey, privSigKey);
        computeIdentityHash();
        initialized = true;
    }

    // ── Load from persisted bytes ─────────────────────────
    void load(const uint8_t* privEnc32, const uint8_t* privSig64,
              const uint8_t* pubEnc32,  const uint8_t* pubSig32) {
        memcpy(privEncKey, privEnc32, 32);
        memcpy(privSigKey, privSig64, 64);
        memcpy(pubEncKey,  pubEnc32,  32);
        memcpy(pubSigKey,  pubSig32,  32);
        computeIdentityHash();
        initialized = true;
    }

    // ── Export combined 64-byte public key ─────────────────
    void getPublicKey(uint8_t out[RNS_KEYSIZE]) const {
        memcpy(out,      pubEncKey, 32);
        memcpy(out + 32, pubSigKey, 32);
    }

    // ── Export raw key material for persistence ───────────
    void exportKeys(uint8_t* outPrivEnc32, uint8_t* outPrivSig64,
                    uint8_t* outPubEnc32,  uint8_t* outPubSig32) const {
        memcpy(outPrivEnc32, privEncKey, 32);
        memcpy(outPrivSig64, privSigKey, 64);
        memcpy(outPubEnc32,  pubEncKey,  32);
        memcpy(outPubSig32,  pubSigKey,  32);
    }

    // ── Identity hash = SHA-256(X25519_pub ∥ Ed25519_pub)[:16]
    void computeIdentityHash() {
        uint8_t combined[RNS_KEYSIZE];
        getPublicKey(combined);

        SHA256 sha;
        sha.reset();
        sha.update(combined, RNS_KEYSIZE);
        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(identityHash, full, RNS_ADDR_LEN);
    }

    // ── Compute SINGLE destination hash ───────────────────
    /**
     * Reticulum destination hash for SINGLE destinations:
     *   name_hash     = SHA-256(fullName)[:10]
     *   identity_hash = SHA-256(pubkey)[:16]
     *   dest_hash     = SHA-256(name_hash || identity_hash)[:16]
     */
    static void computeDestHash(const char* fullName,
                                const uint8_t pubkey[RNS_KEYSIZE],
                                uint8_t outHash[RNS_ADDR_LEN]) {
        uint8_t nameHash[RNS_NAME_HASH_LEN];
        {
            SHA256 sha;
            sha.reset();
            sha.update(fullName, strlen(fullName));
            uint8_t full[32];
            sha.finalize(full, 32);
            memcpy(nameHash, full, RNS_NAME_HASH_LEN);
        }

        uint8_t idHash[RNS_ADDR_LEN];
        {
            SHA256 sha;
            sha.reset();
            sha.update(pubkey, RNS_KEYSIZE);
            uint8_t full[32];
            sha.finalize(full, 32);
            memcpy(idHash, full, RNS_ADDR_LEN);
        }

        {
            SHA256 sha;
            sha.reset();
            sha.update(nameHash, RNS_NAME_HASH_LEN);
            sha.update(idHash, RNS_ADDR_LEN);
            uint8_t full[32];
            sha.finalize(full, 32);
            memcpy(outHash, full, RNS_ADDR_LEN);
        }
    }

    // ── Compute PLAIN destination hash (no identity) ──────
    /**
     * PLAIN destination: dest_hash = SHA-256(name_hash)[:16]
     * where name_hash = SHA-256(full_name)[:10]
     */
    static void computePlainDestHash(const char* fullName,
                                     uint8_t outHash[RNS_ADDR_LEN]) {
        uint8_t nameHash[RNS_NAME_HASH_LEN];
        {
            SHA256 sha;
            sha.reset();
            sha.update(fullName, strlen(fullName));
            uint8_t full[32];
            sha.finalize(full, 32);
            memcpy(nameHash, full, RNS_NAME_HASH_LEN);
        }
        {
            SHA256 sha;
            sha.reset();
            sha.update(nameHash, RNS_NAME_HASH_LEN);
            uint8_t full[32];
            sha.finalize(full, 32);
            memcpy(outHash, full, RNS_ADDR_LEN);
        }
    }

    // ── Validate announce packet's Ed25519 signature ──────
    /**
     * Announce data layout:
     *   [pubkey 64B][nameHash 10B][randomBlob 10B]
     *   [optional ratchet 32B][optional appData]
     *   [signature 64B]  ← last 64 bytes
     *
     * @return true if signature is valid.
     */
    static bool inspectAnnounce(const uint8_t* destHash, const uint8_t* data, uint16_t len,
                                AnnounceInfo* outInfo = nullptr) {
        const uint16_t minLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN
                              + RNS_RANDOM_BLOB_LEN + RNS_SIGLENGTH;
        if (!destHash || !data || len < minLen) return false;

        AnnounceInfo info;
        if (inspectAnnounceLayout(destHash, data, len, false, info)) {
            if (outInfo) *outInfo = info;
            return true;
        }
        if (inspectAnnounceLayout(destHash, data, len, true, info)) {
            if (outInfo) *outInfo = info;
            return true;
        }
        return false;
    }

    static bool validateAnnounce(const uint8_t* destHash, const uint8_t* data, uint16_t len) {
        return inspectAnnounce(destHash, data, len, nullptr);
    }

    // ── Extract 64-byte public key from announce data ─────
    static void extractAnnouncePublicKey(const uint8_t* data,
                                         uint8_t outPubKey[RNS_KEYSIZE]) {
        memcpy(outPubKey, data, RNS_KEYSIZE);
    }

private:
    static bool inspectAnnounceLayout(const uint8_t* destHash, const uint8_t* data, uint16_t len,
                                      bool legacyFixedSignature, AnnounceInfo& outInfo) {
        const uint16_t baseLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        const uint16_t minLen = baseLen + RNS_SIGLENGTH;
        if (!destHash || !data || len < minLen) return false;

        uint16_t sigOffset = legacyFixedSignature ? baseLen : (uint16_t)(len - RNS_SIGLENGTH);
        if (sigOffset < baseLen || (sigOffset + RNS_SIGLENGTH) > len) return false;

        uint16_t appDataOffset = legacyFixedSignature ? (uint16_t)(baseLen + RNS_SIGLENGTH) : baseLen;
        uint16_t appDataLen = legacyFixedSignature
            ? (uint16_t)(len - appDataOffset)
            : (uint16_t)(sigOffset - baseLen);
        const uint16_t signedLen = RNS_ADDR_LEN + baseLen + appDataLen;
        if (signedLen > RNS_MTU) return false;

        static uint8_t signedBuf[RNS_MTU];
        memcpy(signedBuf, destHash, RNS_ADDR_LEN);
        memcpy(signedBuf + RNS_ADDR_LEN, data, baseLen);
        if (appDataLen > 0) {
            memcpy(signedBuf + RNS_ADDR_LEN + baseLen, data + appDataOffset, appDataLen);
        }

        const uint8_t* signature = data + sigOffset;
        const uint8_t* signingKey = data + 32;
        if (!Ed25519::verify(signature, signingKey, signedBuf, signedLen)) return false;

        outInfo.signature = signature;
        outInfo.signingKey = signingKey;
        outInfo.appData = (appDataLen > 0) ? (data + appDataOffset) : nullptr;
        outInfo.sigOffset = sigOffset;
        outInfo.appDataLen = appDataLen;
        outInfo.layout = legacyFixedSignature ? ANNOUNCE_LAYOUT_LEGACY_FIXED_SIG
                                              : ANNOUNCE_LAYOUT_STANDARD;
        return true;
    }

public:

    // ── Sign arbitrary message ────────────────────────────
    void sign(const uint8_t* msg, uint16_t len, uint8_t sig[64]) const {
        Ed25519::sign(sig, privSigKey, pubSigKey, msg, len);
    }

    // ── Verify arbitrary signature ────────────────────────
    static bool verify(const uint8_t* sig, const uint8_t* pubSigKey,
                       const uint8_t* msg, uint16_t len) {
        return Ed25519::verify(sig, pubSigKey, msg, len);
    }

    // ── HMAC-SHA256 ───────────────────────────────────────
    static void hmacSha256(const uint8_t* key, uint16_t keyLen,
                           const uint8_t* data, uint16_t dataLen,
                           uint8_t out[32]) {
        static uint8_t kPad[64];
        memset(kPad, 0, 64);
        if (keyLen > 64) {
            SHA256 sha; sha.reset();
            sha.update(key, keyLen);
            sha.finalize(kPad, 32);
        } else {
            memcpy(kPad, key, keyLen);
        }
        static uint8_t iPad[64];
        for (int i = 0; i < 64; i++) iPad[i] = kPad[i] ^ 0x36;
        SHA256 inner; inner.reset();
        inner.update(iPad, 64);
        inner.update(data, dataLen);
        static uint8_t innerHash[32];
        inner.finalize(innerHash, 32);
        static uint8_t oPad[64];
        for (int i = 0; i < 64; i++) oPad[i] = kPad[i] ^ 0x5C;
        SHA256 outer; outer.reset();
        outer.update(oPad, 64);
        outer.update(innerHash, 32);
        outer.finalize(out, 32);
    }

    // ── HKDF-SHA256 (output up to 64 bytes, 2 iterations) ──
    static void hkdfSha256(const uint8_t* ikm, uint16_t ikmLen,
                           const uint8_t* salt, uint16_t saltLen,
                           const uint8_t* info, uint16_t infoLen,
                           uint8_t* out, uint16_t outLen) {
        static uint8_t prk[32];
        hmacSha256(salt, saltLen, ikm, ikmLen, prk);
        if (outLen > 64) outLen = 64;
        if (infoLen > 32) infoLen = 32;
        static uint8_t prev[32];
        uint16_t prevLen = 0;
        uint16_t generated = 0;
        uint8_t counter = 1;
        while (generated < outLen) {
            static uint8_t expandBuf[32 + 32 + 1];
            uint16_t expandLen = 0;
            if (prevLen > 0) { memcpy(expandBuf, prev, prevLen); expandLen += prevLen; }
            if (info && infoLen > 0) { memcpy(expandBuf + expandLen, info, infoLen); expandLen += infoLen; }
            expandBuf[expandLen++] = counter;
            hmacSha256(prk, 32, expandBuf, expandLen, prev);
            prevLen = 32;
            uint16_t toCopy = (outLen - generated < 32) ? (outLen - generated) : 32;
            memcpy(out + generated, prev, toCopy);
            generated += toCopy;
            counter++;
        }
    }

    // ── Decrypt SINGLE-destination encrypted token ─────────
    /**
     * Reticulum SINGLE-destination encryption:
     *   token = ephemeral_pub(32) + IV(16) + ciphertext + HMAC(32)
     * @return decrypted length, or 0 on failure.
     */
    uint16_t decrypt(const uint8_t* token, uint16_t tokenLen,
                     uint8_t* plaintext, uint16_t maxLen) const {
        return decryptWithSalt(token, tokenLen, plaintext, maxLen, identityHash);
    }

    uint16_t decryptWithSalt(const uint8_t* token, uint16_t tokenLen,
                             uint8_t* plaintext, uint16_t maxLen,
                             const uint8_t salt[RNS_ADDR_LEN],
                             const uint8_t* info = nullptr,
                             uint16_t infoLen = 0) const {
        if (!token || tokenLen < 96 || !plaintext) return 0;
        const uint8_t* ephemeralPub = token;
        const uint8_t* fernetToken = token + 32;
        uint16_t fernetLen = tokenLen - 32;
        // ECDH shared secret (static to keep stack shallow)
        static uint8_t shared[32];
        memcpy(shared, ephemeralPub, 32);
        static uint8_t privCopy[32];
        memcpy(privCopy, privEncKey, 32);
        if (!Curve25519::dh2(shared, privCopy)) return 0;
        // HKDF: derive 64 bytes (AES-256 Token format)
        static uint8_t derivedKey[64];
        hkdfSha256(shared, 32, salt, RNS_ADDR_LEN, info, infoLen, derivedKey, 64);
        const uint8_t* signingKey = derivedKey;       // 32 bytes
        const uint8_t* encKey = derivedKey + 32;      // 32 bytes
        // Token layout: IV(16) + ciphertext + HMAC(32)
        if (fernetLen < 64) return 0;
        const uint8_t* iv = fernetToken;
        const uint8_t* hmacRecv = fernetToken + fernetLen - 32;
        const uint8_t* cipher = fernetToken + 16;
        uint16_t cipherLen = fernetLen - 16 - 32;
        if (cipherLen == 0 || (cipherLen % 16) != 0) return 0;
        if (cipherLen > maxLen) return 0;
        // Verify HMAC (32-byte signing key)
        static uint8_t hmacCalc[32];
        hmacSha256(signingKey, 32, fernetToken, fernetLen - 32, hmacCalc);
        if (memcmp(hmacCalc, hmacRecv, 32) != 0) return 0;
        // AES-256-CBC decrypt
        AES256 aes;
        aes.setKey(encKey, 32);
        static uint8_t prev[16];
        memcpy(prev, iv, 16);
        for (uint16_t i = 0; i < cipherLen; i += 16) {
            static uint8_t dec[16];
            aes.decryptBlock(dec, cipher + i);
            for (int j = 0; j < 16; j++) plaintext[i + j] = dec[j] ^ prev[j];
            memcpy(prev, cipher + i, 16);
        }
        // PKCS7 unpad
        uint8_t pad = plaintext[cipherLen - 1];
        if (pad < 1 || pad > 16) return 0;
        for (uint16_t i = cipherLen - pad; i < cipherLen; i++)
            if (plaintext[i] != pad) return 0;
        return cipherLen - pad;
    }

    // ── Diagnostic decrypt: prints [DIAG] showing where decrypt fails ──
    void decryptDiag(const uint8_t* token, uint16_t tokenLen,
                     const uint8_t* altSalt = nullptr) const {
        Serial.print(F("[DIAG] decrypt diag: "));
        Serial.print(tokenLen);
        Serial.println(F(" bytes"));
        if (!token || tokenLen < 96) {
            Serial.println(F("[DIAG]   FAIL: tokenLen < 96"));
            return;
        }
        const uint8_t* ephemeralPub = token;
        const uint8_t* fernetToken = token + 32;
        uint16_t fernetLen = tokenLen - 32;

        if (fernetLen < 64) { Serial.println(F("[DIAG]   FAIL: fernetLen < 64")); return; }
        uint16_t cipherLen = fernetLen - 16 - 32;
        if (cipherLen == 0 || (cipherLen % 16) != 0) {
            Serial.print(F("[DIAG]   FAIL: bad cipherLen=")); Serial.println(cipherLen); return;
        }
        const uint8_t* hmacRecv = fernetToken + fernetLen - 32;

        // Compute ECDH once and save
        static uint8_t ecdhShared[32];
        {
            static uint8_t tmpShared[32], tmpPriv[32];
            memcpy(tmpShared, ephemeralPub, 32);
            memcpy(tmpPriv, privEncKey, 32);
            if (!Curve25519::dh2(tmpShared, tmpPriv)) {
                Serial.println(F("[DIAG]   FAIL: ECDH")); return;
            }
            memcpy(ecdhShared, tmpShared, 32);
        }
        Serial.print(F("[DIAG]   ECDH shared[0..3]: "));
        for (int i = 0; i < 4; i++) { if (ecdhShared[i]<0x10) Serial.print('0'); Serial.print(ecdhShared[i],HEX); Serial.print(' '); }
        Serial.println();

        // Test combos: {identityHash, destHash} with 64-byte key (AES-256 Token)
        struct { const uint8_t* salt; const char* label; } salts[] = {
            { identityHash, "idHash" },
            { altSalt,      "destHash" },
        };
        static uint8_t derivedKey[64];
        static uint8_t hmacCalc[32];
        bool anyOk = false;

        for (int s = 0; s < 2; s++) {
            if (!salts[s].salt) continue;
            hkdfSha256(ecdhShared, 32, salts[s].salt, RNS_ADDR_LEN, nullptr, 0, derivedKey, 64);
            hmacSha256(derivedKey, 32, fernetToken, fernetLen - 32, hmacCalc);
            bool ok = (memcmp(hmacCalc, hmacRecv, 32) == 0);
            Serial.print(F("[DIAG]   "));
            Serial.print(salts[s].label);
            Serial.print(F("+AES256: "));
            Serial.println(ok ? F("OK") : F("MISMATCH"));
            if (ok) anyOk = true;
        }
        if (!anyOk) {
            Serial.print(F("[DIAG]   recv HMAC: ")); for(int i=0;i<8;i++){if(hmacRecv[i]<0x10)Serial.print('0');Serial.print(hmacRecv[i],HEX);} Serial.println(F("..."));
        }
        Serial.print(F("[DIAG]   pubEncKey: "));
        for (int i = 0; i < 32; i++) { if (pubEncKey[i]<0x10) Serial.print('0'); Serial.print(pubEncKey[i],HEX); }
        Serial.println();
    }

    // ── Encrypt for a SINGLE destination ──────────────────
    static uint16_t encrypt(const uint8_t targetPubEncKey[32],
                            const uint8_t targetIdHash[RNS_ADDR_LEN],
                            const uint8_t* plain, uint16_t plainLen,
                            uint8_t* tokenOut, uint16_t maxTokenLen) {
        if (!plain || plainLen == 0 || !tokenOut) return 0;
        uint16_t paddedLen = ((plainLen / 16) + 1) * 16;
        uint16_t tokenLen = 32 + 16 + paddedLen + 32;
        if (tokenLen > maxTokenLen) return 0;
        // All buffers static to keep stack shallow (Curve25519 is deep)
        static uint8_t ephPub[32], ephPriv[32];
        Curve25519::dh1(ephPub, ephPriv);
        static uint8_t shared[32];
        memcpy(shared, targetPubEncKey, 32);
        Curve25519::dh2(shared, ephPriv);
        // HKDF: derive 64 bytes (AES-256 Token format), empty context
        static uint8_t derivedKey[64];
        hkdfSha256(shared, 32, targetIdHash, RNS_ADDR_LEN, nullptr, 0, derivedKey, 64);
        const uint8_t* signingKey = derivedKey;       // 32 bytes
        const uint8_t* encKey = derivedKey + 32;      // 32 bytes
        // Random IV
        static uint8_t iv[16];
        for (int i = 0; i < 16; i++) iv[i] = (uint8_t)random(0, 256);
        // PKCS7 pad
        static uint8_t padded[RNS_MTU];
        memcpy(padded, plain, plainLen);
        uint8_t padByte = (uint8_t)(paddedLen - plainLen);
        for (uint16_t i = plainLen; i < paddedLen; i++) padded[i] = padByte;
        // AES-256-CBC encrypt
        AES256 aes;
        aes.setKey(encKey, 32);
        uint8_t* cipherOut = tokenOut + 32 + 16;
        static uint8_t prev[16];
        memcpy(prev, iv, 16);
        for (uint16_t i = 0; i < paddedLen; i += 16) {
            static uint8_t xored[16];
            for (int j = 0; j < 16; j++) xored[j] = padded[i + j] ^ prev[j];
            aes.encryptBlock(cipherOut + i, xored);
            memcpy(prev, cipherOut + i, 16);
        }
        // Assemble: ephPub + IV + ciphertext + HMAC
        memcpy(tokenOut, ephPub, 32);
        memcpy(tokenOut + 32, iv, 16);
        static uint8_t hmac[32];
        hmacSha256(signingKey, 32, tokenOut + 32, 16 + paddedLen, hmac);
        memcpy(tokenOut + 32 + 16 + paddedLen, hmac, 32);
        return tokenLen;
    }
};
