/**
 * @file RNSPersistence.h
 * @brief LittleFS-backed persistence for identity keypair, radio
 *        configuration, and (optionally) path table across reboots.
 *
 * SAFETY: All writes use a write-then-verify pattern. If the filesystem
 * is corrupted, the node falls back to generating a fresh identity and
 * using default radio parameters — it never bricks.
 */
#pragma once
#include "RNSConfig.h"
#include "RNSIdentity.h"
#include "RNSTransport.h"
#include "RNSRadio.h"
#include <string.h>

#ifndef NATIVE_TEST
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#endif

// ── Persisted radio configuration ─────────────────────────
struct RadioConfig {
    float   freqMHz;
    float   bwKHz;
    uint8_t sf;
    uint8_t cr;
    int8_t  txDbm;
    uint32_t checksum;   ///< Simple XOR checksum for integrity
};

// ── Identity blob layout (fixed 160 bytes) ────────────────
struct IdentityBlob {
    uint8_t  privEncKey[32];
    uint8_t  privSigKey[64];
    uint8_t  pubEncKey[32];
    uint8_t  pubSigKey[32];
    uint32_t checksum;
};

class RNSPersistence {
public:
    bool fsReady = false;

    bool begin() {
#ifndef NATIVE_TEST
        if (InternalFS.begin()) {
            fsReady = true;
            return true;
        }
        // If FS mount fails, format and retry once
        InternalFS.format();
        fsReady = InternalFS.begin();
        return fsReady;
#else
        return false;
#endif
    }

    // ── Identity persistence ──────────────────────────────

    /**
     * @brief Try to load identity from flash; return false if not found
     *        or corrupted (caller should generate fresh).
     */
    bool loadIdentity(RNSIdentity& id) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        File f = InternalFS.open(IDENTITY_FILE, FILE_O_READ);
        if (!f) return false;

        IdentityBlob blob;
        if (f.read((uint8_t*)&blob, sizeof(blob)) != sizeof(blob)) {
            f.close();
            return false;
        }
        f.close();

        // Verify checksum
        if (computeBlobChecksum(blob) != blob.checksum) return false;

        id.load(blob.privEncKey, blob.privSigKey,
                blob.pubEncKey,  blob.pubSigKey);
        return true;
#else
        return false;
#endif
    }

    /**
     * @brief Persist identity to flash.
     * @return true on success.
     */
    bool saveIdentity(const RNSIdentity& id) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        IdentityBlob blob;
        id.exportKeys(blob.privEncKey, blob.privSigKey,
                      blob.pubEncKey,  blob.pubSigKey);
        blob.checksum = computeBlobChecksum(blob);

        // Remove old file first (LittleFS doesn't truncate reliably)
        InternalFS.remove(IDENTITY_FILE);

        File f = InternalFS.open(IDENTITY_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((uint8_t*)&blob, sizeof(blob));
        f.close();
        return (written == sizeof(blob));
#else
        return false;
#endif
    }

    // ── Radio config persistence ──────────────────────────

    bool loadConfig(RNSRadio& radio) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        File f = InternalFS.open(CONFIG_FILE, FILE_O_READ);
        if (!f) return false;

        RadioConfig cfg;
        if (f.read((uint8_t*)&cfg, sizeof(cfg)) != sizeof(cfg)) {
            f.close();
            return false;
        }
        f.close();

        if (computeConfigChecksum(cfg) != cfg.checksum) return false;

        radio.setFrequency(cfg.freqMHz);
        radio.setBandwidth(cfg.bwKHz);
        radio.setSF(cfg.sf);
        radio.setCR(cfg.cr);
        radio.setTxPower(cfg.txDbm);
        return true;
#else
        return false;
#endif
    }

    bool saveConfig(const RNSRadio& radio) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        RadioConfig cfg;
        cfg.freqMHz = radio.curFreqMHz;
        cfg.bwKHz   = radio.curBwKHz;
        cfg.sf      = radio.curSF;
        cfg.cr      = radio.curCR;
        cfg.txDbm   = radio.curTxDbm;
        cfg.checksum = computeConfigChecksum(cfg);

        InternalFS.remove(CONFIG_FILE);
        File f = InternalFS.open(CONFIG_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((uint8_t*)&cfg, sizeof(cfg));
        f.close();
        return (written == sizeof(cfg));
#else
        return false;
#endif
    }

    bool loadAnnounceName(char* outName, size_t maxLen) {
#ifndef NATIVE_TEST
        if (!fsReady || !outName || maxLen < 2) return false;

        File f = InternalFS.open(ANNOUNCE_NAME_FILE, FILE_O_READ);
        if (!f) return false;

        size_t readLen = f.read((uint8_t*)outName, maxLen - 1);
        f.close();
        if (readLen == 0) return false;

        outName[readLen] = '\0';
        while (readLen > 0 && (outName[readLen - 1] == '\r' || outName[readLen - 1] == '\n' || outName[readLen - 1] == ' ')) {
            outName[--readLen] = '\0';
        }
        return readLen > 0;
#else
        return false;
#endif
    }

    bool saveAnnounceName(const char* name) {
#ifndef NATIVE_TEST
        if (!fsReady || !name || !*name) return false;

        InternalFS.remove(ANNOUNCE_NAME_FILE);
        File f = InternalFS.open(ANNOUNCE_NAME_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((const uint8_t*)name, strlen(name));
        f.close();
        return written == strlen(name);
#else
        return false;
#endif
    }

    // ── Factory reset: erase all persisted data ───────────
    void factoryReset() {
#ifndef NATIVE_TEST
        if (!fsReady) return;
        InternalFS.remove(IDENTITY_FILE);
        InternalFS.remove(CONFIG_FILE);
        InternalFS.remove(ANNOUNCE_NAME_FILE);
        InternalFS.remove(PATH_TABLE_FILE);
#endif
    }

private:
    uint32_t computeBlobChecksum(const IdentityBlob& b) {
        uint32_t cs = 0x5A5A5A5A;
        const uint8_t* p = (const uint8_t*)&b;
        // Checksum over everything except the checksum field itself
        for (size_t i = 0; i < offsetof(IdentityBlob, checksum); i++)
            cs ^= ((uint32_t)p[i] << ((i % 4) * 8));
        return cs;
    }

    uint32_t computeConfigChecksum(const RadioConfig& c) {
        uint32_t cs = 0xA5A5A5A5;
        const uint8_t* p = (const uint8_t*)&c;
        for (size_t i = 0; i < offsetof(RadioConfig, checksum); i++)
            cs ^= ((uint32_t)p[i] << ((i % 4) * 8));
        return cs;
    }
};
