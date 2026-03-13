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
        Ed25519::generateKey(pubSigKey, privSigKey);
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
     * hash = SHA-256("appName.aspects." + hex(pubkey))[:16]
     */
    static void computeDestHash(const char* fullName,
                                const uint8_t pubkey[RNS_KEYSIZE],
                                uint8_t outHash[RNS_ADDR_LEN]) {
        SHA256 sha;
        sha.reset();
        sha.update(fullName, strlen(fullName));
        sha.update(".", 1);

        char hexBuf[3];
        for (int i = 0; i < RNS_KEYSIZE; i++) {
            snprintf(hexBuf, sizeof(hexBuf), "%02x", pubkey[i]);
            sha.update(hexBuf, 2);
        }

        uint8_t full[32];
        sha.finalize(full, 32);
        memcpy(outHash, full, RNS_ADDR_LEN);
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
    static bool validateAnnounce(const uint8_t* data, uint16_t len) {
        const uint16_t minLen = RNS_KEYSIZE + RNS_NAME_HASH_LEN
                              + RNS_RANDOM_BLOB_LEN + RNS_SIGLENGTH;
        if (!data || len < minLen) return false;

        uint16_t sigOffset      = len - RNS_SIGLENGTH;
        const uint8_t* signature = &data[sigOffset];
        const uint8_t* signingKey = &data[32];   // Ed25519 pub at bytes 32..63
        const uint8_t* signedData = data;
        uint16_t signedLen       = sigOffset;

        return Ed25519::verify(signature, signingKey, signedData, signedLen);
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
};
