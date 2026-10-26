/**
 * @file RNSEntropy.h
 * @brief Hardware entropy for key generation, IVs and nonces.
 *
 * WHY THIS EXISTS (2026-10-16): the rweather Crypto library's RNG has no
 * hardware source on nRF52 (RNG.cpp only knows Due/ESP). Unseeded, its
 * begin() initialises ChaCha20 from fixed constants, so Curve25519::dh1()
 * and Ed25519::generatePrivateKey() returned the SAME key pair on every
 * board: two nodes ("Aster", "Bleeding Heart") were found sharing one
 * identity, and that identity's private keys are derivable from the
 * library source by anyone. Arduino random() on this core is libc
 * rand(), also unseeded, so IVs and announce random blobs repeated too.
 *
 * Source: the nRF52840 RNG peripheral (true random, bias-corrected),
 * read directly when no SoftDevice is enabled, or through
 * sd_rand_application_vector_get() when one is. The Crypto RNG is
 * seeded from it with full entropy credit and stirred periodically, and
 * libc rand() is seeded for the non-cryptographic random() users.
 */
#pragma once
#include "RNSConfig.h"
#ifndef NATIVE_TEST
#include <Arduino.h>
#include <RNG.h>
#include <nrf.h>
#if __has_include("nrf_sdm.h")
#include <nrf_sdm.h>
#include <nrf_soc.h>
#define RNS_HAVE_SD_API 1
#endif

namespace RNSEntropy {

inline bool softDeviceEnabled() {
#ifdef RNS_HAVE_SD_API
    uint8_t en = 0;
    if (sd_softdevice_is_enabled(&en) == NRF_SUCCESS) return en != 0;
#endif
    return false;
}

/// Fill `out` with hardware random bytes. Blocks ~30 us per byte.
inline void hwRandomBytes(uint8_t* out, size_t len) {
#ifdef RNS_HAVE_SD_API
    if (softDeviceEnabled()) {
        size_t done = 0;
        while (done < len) {
            uint8_t avail = 0;
            sd_rand_application_bytes_available_get(&avail);
            if (avail == 0) { delayMicroseconds(100); continue; }
            uint8_t n = (uint8_t)((len - done) < avail ? (len - done) : avail);
            if (sd_rand_application_vector_get(out + done, n) == NRF_SUCCESS) done += n;
        }
        return;
    }
#endif
    NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk;       // bias correction on
    NRF_RNG->EVENTS_VALRDY = 0;
    NRF_RNG->TASKS_START = 1;
    for (size_t i = 0; i < len; i++) {
        while (NRF_RNG->EVENTS_VALRDY == 0) { }
        NRF_RNG->EVENTS_VALRDY = 0;
        out[i] = (uint8_t)NRF_RNG->VALUE;
    }
    NRF_RNG->TASKS_STOP = 1;
}

inline uint32_t hwRandom32() {
    uint32_t v; hwRandomBytes((uint8_t*)&v, 4); return v;
}

/// Seed the Crypto RNG and libc rand(). Idempotent; call before any key
/// generation. Mixes 64 bytes of hardware entropy (full credit) plus the
/// chip's FICR device ID so even a failed TRNG cannot make two boards alike.
inline void begin() {
    static bool done = false;
    if (done) return;
    done = true;
    RNG.begin("RatTunnel " FW_VERSION_STRING);
    uint8_t seed[64];
    hwRandomBytes(seed, sizeof(seed));
    RNG.stir(seed, sizeof(seed), sizeof(seed) * 8);
    uint32_t dev[2] = { NRF_FICR->DEVICEID[0], NRF_FICR->DEVICEID[1] };
    RNG.stir((const uint8_t*)dev, sizeof(dev), 0);
    randomSeed(hwRandom32());
    memset(seed, 0, sizeof(seed));
}

/// Call from loop(): adds fresh hardware entropy every few seconds.
inline void service(uint32_t now) {
    static uint32_t nextAt = 0;
    if ((int32_t)(now - nextAt) < 0) return;
    nextAt = now + 5000UL;
    uint8_t fresh[16];
    hwRandomBytes(fresh, sizeof(fresh));
    RNG.stir(fresh, sizeof(fresh), sizeof(fresh) * 8);
}

/// Cryptographic random bytes (seeded Crypto RNG, hardware-stirred).
inline void randomBytes(uint8_t* out, size_t len) {
    begin();
    RNG.rand(out, len);
}

} // namespace RNSEntropy
#endif
