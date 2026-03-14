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

    // ── Validate announce packet's Ed25519 signature ──────
    /**
     * Announce data layout:
     *   [pubkey 64B][nameHash 10B][randomBlob 10B]
     *   [optional ratchet 32B][optional appData]
     *   [signature 64B]  ← last 64 bytes
     *
     * @return true if signature is valid.
     */
    static bool validateAnnounce(const uint8_t* destHash, const uint8_t* data, uint16_t len) {
        const uint16_t minLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN
                              + RNS_RANDOM_BLOB_LEN + RNS_SIGLENGTH;
        if (!destHash || !data || len < minLen) return false;

        // Reticulum wire format: pubkey|nameHash|randomHash|SIGNATURE|appData
        // Signature is at fixed offset, NOT at end of data.
        const uint16_t sigOffset = RNS_KEYSIZE + RNS_NAME_HASH_LEN + RNS_RANDOM_BLOB_LEN;
        const uint8_t* signature  = &data[sigOffset];
        const uint8_t* signingKey = &data[32];   // Ed25519 pub at bytes 32..63

        // Signed data = destHash + pubkey + nameHash + randomHash + appData (skip signature)
        // The destHash from the packet header is prepended per Reticulum spec.
        const uint16_t appDataOffset = sigOffset + RNS_SIGLENGTH;
        const uint16_t appDataLen = (len > appDataOffset) ? (len - appDataOffset) : 0;
        const uint16_t signedLen  = RNS_ADDR_LEN + sigOffset + appDataLen;
        if (signedLen > RNS_MTU) return false;

        uint8_t signedBuf[RNS_MTU];
        memcpy(signedBuf, destHash, RNS_ADDR_LEN);
        memcpy(signedBuf + RNS_ADDR_LEN, data, sigOffset);
        if (appDataLen > 0) {
            memcpy(signedBuf + RNS_ADDR_LEN + sigOffset, data + appDataOffset, appDataLen);
        }

        return Ed25519::verify(signature, signingKey, signedBuf, signedLen);
    }

    // ── Extract 64-byte public key from announce data ─────
    static void extractAnnouncePublicKey(const uint8_t* data,
                                         uint8_t outPubKey[RNS_KEYSIZE]) {
        memcpy(outPubKey, data, RNS_KEYSIZE);
    }

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
        uint8_t kPad[64];
        memset(kPad, 0, 64);
        if (keyLen > 64) {
            SHA256 sha; sha.reset();
            sha.update(key, keyLen);
            sha.finalize(kPad, 32);
        } else {
            memcpy(kPad, key, keyLen);
        }
        uint8_t iPad[64];
        for (int i = 0; i < 64; i++) iPad[i] = kPad[i] ^ 0x36;
        SHA256 inner; inner.reset();
        inner.update(iPad, 64);
        inner.update(data, dataLen);
        uint8_t innerHash[32];
        inner.finalize(innerHash, 32);
        uint8_t oPad[64];
        for (int i = 0; i < 64; i++) oPad[i] = kPad[i] ^ 0x5C;
        SHA256 outer; outer.reset();
        outer.update(oPad, 64);
        outer.update(innerHash, 32);
        outer.finalize(out, 32);
    }

    // ── HKDF-SHA256 (output up to 32 bytes) ───────────────
    static void hkdfSha256(const uint8_t* ikm, uint16_t ikmLen,
                           const uint8_t* salt, uint16_t saltLen,
                           const uint8_t* info, uint16_t infoLen,
                           uint8_t* out, uint16_t outLen) {
        uint8_t prk[32];
        hmacSha256(salt, saltLen, ikm, ikmLen, prk);
        uint8_t expandBuf[64];
        if (infoLen > 62) infoLen = 62;
        memcpy(expandBuf, info, infoLen);
        expandBuf[infoLen] = 0x01;
        uint8_t okm[32];
        hmacSha256(prk, 32, expandBuf, infoLen + 1, okm);
        if (outLen > 32) outLen = 32;
        memcpy(out, okm, outLen);
    }

    // ── Decrypt SINGLE-destination encrypted token ─────────
    /**
     * Reticulum SINGLE-destination encryption:
     *   token = ephemeral_pub(32) + IV(16) + ciphertext + HMAC(32)
     * @return decrypted length, or 0 on failure.
     */
    uint16_t decrypt(const uint8_t* token, uint16_t tokenLen,
                     uint8_t* plaintext, uint16_t maxLen) const {
        if (!token || tokenLen < 96 || !plaintext) return 0;
        const uint8_t* ephemeralPub = token;
        const uint8_t* fernetToken = token + 32;
        uint16_t fernetLen = tokenLen - 32;
        // ECDH shared secret
        uint8_t shared[32];
        memcpy(shared, ephemeralPub, 32);
        uint8_t privCopy[32];
        memcpy(privCopy, privEncKey, 32);
        if (!Curve25519::dh2(shared, privCopy)) return 0;
        // HKDF key derivation
        static const uint8_t ctx[] = "rns_token";
        uint8_t derivedKey[32];
        hkdfSha256(shared, 32, identityHash, RNS_ADDR_LEN, ctx, 9, derivedKey, 32);
        const uint8_t* signingKey = derivedKey;
        const uint8_t* encKey = derivedKey + 16;
        // Fernet layout: IV(16) + ciphertext + HMAC(32)
        if (fernetLen < 64) return 0;
        const uint8_t* iv = fernetToken;
        const uint8_t* hmacRecv = fernetToken + fernetLen - 32;
        const uint8_t* cipher = fernetToken + 16;
        uint16_t cipherLen = fernetLen - 16 - 32;
        if (cipherLen == 0 || (cipherLen % 16) != 0) return 0;
        if (cipherLen > maxLen) return 0;
        // Verify HMAC
        uint8_t hmacCalc[32];
        hmacSha256(signingKey, 16, fernetToken, fernetLen - 32, hmacCalc);
        if (memcmp(hmacCalc, hmacRecv, 32) != 0) return 0;
        // AES-128-CBC decrypt
        AES128 aes;
        aes.setKey(encKey, 16);
        uint8_t prev[16];
        memcpy(prev, iv, 16);
        for (uint16_t i = 0; i < cipherLen; i += 16) {
            uint8_t dec[16];
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

    // ── Encrypt for a SINGLE destination ──────────────────
    static uint16_t encrypt(const uint8_t targetPubEncKey[32],
                            const uint8_t targetIdHash[RNS_ADDR_LEN],
                            const uint8_t* plain, uint16_t plainLen,
                            uint8_t* tokenOut, uint16_t maxTokenLen) {
        if (!plain || plainLen == 0 || !tokenOut) return 0;
        uint16_t paddedLen = ((plainLen / 16) + 1) * 16;
        uint16_t tokenLen = 32 + 16 + paddedLen + 32;
        if (tokenLen > maxTokenLen) return 0;
        // Ephemeral X25519 keypair
        uint8_t ephPub[32], ephPriv[32];
        Curve25519::dh1(ephPub, ephPriv);
        uint8_t shared[32];
        memcpy(shared, targetPubEncKey, 32);
        Curve25519::dh2(shared, ephPriv);
        // HKDF
        static const uint8_t ctx[] = "rns_token";
        uint8_t derivedKey[32];
        hkdfSha256(shared, 32, targetIdHash, RNS_ADDR_LEN, ctx, 9, derivedKey, 32);
        const uint8_t* signingKey = derivedKey;
        const uint8_t* encKey = derivedKey + 16;
        // Random IV
        uint8_t iv[16];
        for (int i = 0; i < 16; i++) iv[i] = (uint8_t)random(0, 256);
        // PKCS7 pad
        uint8_t padded[RNS_MTU];
        memcpy(padded, plain, plainLen);
        uint8_t padByte = (uint8_t)(paddedLen - plainLen);
        for (uint16_t i = plainLen; i < paddedLen; i++) padded[i] = padByte;
        // AES-128-CBC encrypt
        AES128 aes;
        aes.setKey(encKey, 16);
        uint8_t* cipherOut = tokenOut + 32 + 16;
        uint8_t prev[16];
        memcpy(prev, iv, 16);
        for (uint16_t i = 0; i < paddedLen; i += 16) {
            uint8_t xored[16];
            for (int j = 0; j < 16; j++) xored[j] = padded[i + j] ^ prev[j];
            aes.encryptBlock(cipherOut + i, xored);
            memcpy(prev, cipherOut + i, 16);
        }
        // Assemble: ephPub + IV + ciphertext + HMAC
        memcpy(tokenOut, ephPub, 32);
        memcpy(tokenOut + 32, iv, 16);
        uint8_t hmac[32];
        hmacSha256(signingKey, 16, tokenOut + 32, 16 + paddedLen, hmac);
        memcpy(tokenOut + 32 + 16 + paddedLen, hmac, 32);
        return tokenLen;
    }
};
