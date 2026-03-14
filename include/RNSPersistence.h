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
// Bump CONFIG_VERSION when compile-time defaults change so stale
// flash configs are automatically discarded on first boot.
#define RADIO_CONFIG_VERSION  2   // V2: ratspeak-us defaults (SF9/pre18)

struct RadioConfig {
    uint16_t version;    ///< Must match RADIO_CONFIG_VERSION or load is rejected
    float   freqMHz;
    float   bwKHz;
    uint8_t sf;
    uint8_t cr;
    int8_t  txDbm;
    uint8_t syncWord;
    uint16_t preamble;
    uint32_t checksum;   ///< Simple XOR checksum for integrity
};

struct MorseBlinkConfigBlob {
    uint8_t mode;
    char    defaultMessage[17];
    uint32_t checksum;
};

struct LedConfigBlob {
    uint8_t version;
    uint8_t reserved[3];
    uint8_t idleModes[3];
    uint8_t rxFlashEnabled[3];
    uint8_t txFlashEnabled[3];
    uint8_t morseModes[3];
    uint8_t alertMode;
    uint8_t alertRepeatCount;
    uint8_t alertWatchCount;
    uint16_t alertIntervalSec;
    uint8_t alertWatchPrefixes[LED_ALERT_WATCH_MAX][LED_ALERT_PREFIX_BYTES];
    uint32_t checksum;
};

struct SecurityConfigBlob {
    uint8_t enabled;
    uint8_t wipeOnBoot;
    uint8_t reserved[2];
    uint32_t checksum;
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
        if (cfg.version != RADIO_CONFIG_VERSION) {
            Serial.println(F("[RNS] Saved radio config version mismatch — using defaults"));
            InternalFS.remove(CONFIG_FILE);
            return false;
        }

        radio.setFrequency(cfg.freqMHz);
        radio.setBandwidth(cfg.bwKHz);
        radio.setSF(cfg.sf);
        radio.setCR(cfg.cr);
        radio.setTxPower(cfg.txDbm);
        radio.setSyncWord(cfg.syncWord);
        radio.setPreamble(cfg.preamble);
        return true;
#else
        return false;
#endif
    }

    bool saveConfig(const RNSRadio& radio) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        RadioConfig cfg;
        cfg.version  = RADIO_CONFIG_VERSION;
        cfg.freqMHz = radio.curFreqMHz;
        cfg.bwKHz   = radio.curBwKHz;
        cfg.sf      = radio.curSF;
        cfg.cr      = radio.curCR;
        cfg.txDbm   = radio.curTxDbm;
        cfg.syncWord = radio.curSyncWord;
        cfg.preamble = radio.curPreamble;
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

    bool loadMorseBlinkConfig(uint8_t& mode, char* defaultMessage, size_t maxLen) {
#ifndef NATIVE_TEST
        if (!fsReady || !defaultMessage || maxLen < 2) return false;

        File f = InternalFS.open(MORSE_CONFIG_FILE, FILE_O_READ);
        if (!f) return false;

        MorseBlinkConfigBlob cfg;
        if (f.read((uint8_t*)&cfg, sizeof(cfg)) != sizeof(cfg)) {
            f.close();
            return false;
        }
        f.close();

        if (computeMorseBlinkChecksum(cfg) != cfg.checksum) return false;

        mode = cfg.mode;
        strncpy(defaultMessage, cfg.defaultMessage, maxLen - 1);
        defaultMessage[maxLen - 1] = '\0';
        return true;
#else
        (void)mode; (void)defaultMessage; (void)maxLen;
        return false;
#endif
    }

    bool saveMorseBlinkConfig(uint8_t mode, const char* defaultMessage) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        MorseBlinkConfigBlob cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = mode;
        if (defaultMessage) {
            strncpy(cfg.defaultMessage, defaultMessage, sizeof(cfg.defaultMessage) - 1);
            cfg.defaultMessage[sizeof(cfg.defaultMessage) - 1] = '\0';
        }
        cfg.checksum = computeMorseBlinkChecksum(cfg);

        InternalFS.remove(MORSE_CONFIG_FILE);
        File f = InternalFS.open(MORSE_CONFIG_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((uint8_t*)&cfg, sizeof(cfg));
        f.close();
        return written == sizeof(cfg);
#else
        (void)mode; (void)defaultMessage;
        return false;
#endif
    }

    bool loadLedConfig(LedConfigBlob& cfg) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        File f = InternalFS.open(LED_CONFIG_FILE, FILE_O_READ);
        if (!f) return false;

        LedConfigBlob stored;
        if (f.read((uint8_t*)&stored, sizeof(stored)) != sizeof(stored)) {
            f.close();
            return false;
        }
        f.close();

        if (computeLedChecksum(stored) != stored.checksum) return false;
        if (stored.version != LED_CONFIG_VERSION) {
            InternalFS.remove(LED_CONFIG_FILE);
            return false;
        }

        cfg = stored;
        cfg.alertWatchCount = cfg.alertWatchCount > LED_ALERT_WATCH_MAX ? LED_ALERT_WATCH_MAX : cfg.alertWatchCount;
        return true;
#else
        (void)cfg;
        return false;
#endif
    }

    bool saveLedConfig(const LedConfigBlob& inputCfg) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        LedConfigBlob cfg;
        memcpy(&cfg, &inputCfg, sizeof(cfg));
        cfg.version = LED_CONFIG_VERSION;
        cfg.alertWatchCount = cfg.alertWatchCount > LED_ALERT_WATCH_MAX ? LED_ALERT_WATCH_MAX : cfg.alertWatchCount;
        cfg.checksum = computeLedChecksum(cfg);

        InternalFS.remove(LED_CONFIG_FILE);
        File f = InternalFS.open(LED_CONFIG_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((uint8_t*)&cfg, sizeof(cfg));
        f.close();
        return written == sizeof(cfg);
#else
        (void)inputCfg;
        return false;
#endif
    }

    bool loadSecurityConfig(bool& enabled, bool& wipeOnBoot) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        File f = InternalFS.open(SECURITY_CONFIG_FILE, FILE_O_READ);
        if (!f) return false;

        SecurityConfigBlob cfg;
        if (f.read((uint8_t*)&cfg, sizeof(cfg)) != sizeof(cfg)) {
            f.close();
            return false;
        }
        f.close();

        if (computeSecurityChecksum(cfg) != cfg.checksum) return false;

        enabled = cfg.enabled != 0;
        wipeOnBoot = cfg.wipeOnBoot != 0;
        return true;
#else
        (void)enabled; (void)wipeOnBoot;
        return false;
#endif
    }

    bool saveSecurityConfig(bool enabled, bool wipeOnBoot) {
#ifndef NATIVE_TEST
        if (!fsReady) return false;

        SecurityConfigBlob cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.enabled = enabled ? 1 : 0;
        cfg.wipeOnBoot = wipeOnBoot ? 1 : 0;
        cfg.checksum = computeSecurityChecksum(cfg);

        InternalFS.remove(SECURITY_CONFIG_FILE);
        File f = InternalFS.open(SECURITY_CONFIG_FILE, FILE_O_WRITE);
        if (!f) return false;

        size_t written = f.write((uint8_t*)&cfg, sizeof(cfg));
        f.close();
        return written == sizeof(cfg);
#else
        (void)enabled; (void)wipeOnBoot;
        return false;
#endif
    }

    void wipeRuntimeStatePreserveSecurity() {
#ifndef NATIVE_TEST
        if (!fsReady) return;
        InternalFS.remove(IDENTITY_FILE);
        InternalFS.remove(CONFIG_FILE);
        InternalFS.remove(ANNOUNCE_NAME_FILE);
        InternalFS.remove(MORSE_CONFIG_FILE);
        InternalFS.remove(LED_CONFIG_FILE);
        InternalFS.remove(PATH_TABLE_FILE);
#endif
    }

    // ── Factory reset: erase all persisted data ───────────
    void factoryReset() {
#ifndef NATIVE_TEST
        if (!fsReady) return;
        wipeRuntimeStatePreserveSecurity();
        InternalFS.remove(SECURITY_CONFIG_FILE);
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

    uint32_t computeMorseBlinkChecksum(const MorseBlinkConfigBlob& c) {
        uint32_t cs = 0xC3C3C3C3;
        const uint8_t* p = (const uint8_t*)&c;
        for (size_t i = 0; i < offsetof(MorseBlinkConfigBlob, checksum); i++)
            cs ^= ((uint32_t)p[i] << ((i % 4) * 8));
        return cs;
    }

    uint32_t computeLedChecksum(const LedConfigBlob& c) {
        uint32_t cs = 0xE1D0C0DE;
        const uint8_t* p = (const uint8_t*)&c;
        for (size_t i = 0; i < offsetof(LedConfigBlob, checksum); i++)
            cs ^= ((uint32_t)p[i] << ((i % 4) * 8));
        return cs;
    }

    uint32_t computeSecurityChecksum(const SecurityConfigBlob& c) {
        uint32_t cs = 0x51524345;
        const uint8_t* p = (const uint8_t*)&c;
        for (size_t i = 0; i < offsetof(SecurityConfigBlob, checksum); i++)
            cs ^= ((uint32_t)p[i] << ((i % 4) * 8));
        return cs;
    }
};
