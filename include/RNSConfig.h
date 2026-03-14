/**
 * @file RNSConfig.h
 * @brief Hardware pins, LoRa defaults, Reticulum protocol constants,
 *        and transport-engine tuning for WisBlock 1W.
 *
 * Safety:  All tuning values sized for nRF52840 (256 KB RAM).
 *          Static allocation only — no heap usage after setup().
 */
#pragma once

#ifndef NATIVE_TEST
#include <Arduino.h>
#else
#include <stdint.h>
#include <stdlib.h>
#define HIGH 1
#define LOW  0
inline uint32_t millis() { return 0; }
inline long random(long a, long b) { return a + (rand() % (b - a)); }
#endif

// ── Firmware version ──────────────────────────────────────
#define FW_VERSION_MAJOR    1
#define FW_VERSION_MINOR    0
#define FW_VERSION_PATCH    31
#define FW_VERSION_STRING   "1.0.31"
#define FW_PRODUCT_NAME     "RatTunnel"
#define FW_DISPLAY_VERSION  "RatTunnel V. 1.0.31"
#define FW_BUILD_TAG        "rattunnel-wisblock1w"

// ── WisBlock 1W (RAK3401 + RAK13302) pin mapping ─────────
#define PIN_LORA_NSS        26   // WB_SPI_CS
#define PIN_LORA_SCK         3   // WB_SPI_CLK
#define PIN_LORA_MISO       29   // WB_SPI_MISO
#define PIN_LORA_MOSI       30   // WB_SPI_MOSI
#define PIN_LORA_DIO1_ACTIVE false  // P15 is floating HIGH (not real DIO1); use IRQ polling
#define PIN_LORA_DIO1_PIN   15     // physical pin (only for diagnostics/pintest)
#define PIN_LORA_BUSY        9   // discovered P9 via Phase C timing probe
#define PIN_LORA_RESET       4   // discovered P4 via Phase B brute-force
#define PIN_LORA_ENABLE     34   // WB_IO2 → 3V3_S gate (P34, tested V1.0.12)

// LEDs (active HIGH)
#define PIN_LED_GREEN       35
#define PIN_LED_BLUE        36

// ── LoRa default parameters ──────────────────────────────
#define LORA_FREQ_MHZ       915.0f   // US ISM band
#define LORA_BW_KHZ         125.0f
#define LORA_SF             9
#define LORA_CR             5        // coding rate 4/5
#define LORA_TX_DBM         17       // safer default for power stability (PA adds ~8 dB)
#define LORA_TX_DBM_MAX_SAFE 17
#define LORA_TX_DBM_ANNOUNCE_SAFE LORA_TX_DBM  // Use full TX power so peers discover us
#define LORA_PREAMBLE       18       // ratspeak-us balanced default
#define LORA_SYNC_WORD      0x12     // private LoRa sync word

// ── Reticulum protocol constants ─────────────────────────
#define RNS_MTU             500
#define RNS_HEADER_SIZE       2
#define RNS_ADDR_LEN         16      // truncated SHA-256 destination hash
#define RNS_NAME_HASH_LEN   10
#define RNS_RANDOM_BLOB_LEN 10
#define RNS_ANNOUNCE_NAME_MAX 32
#define RNS_SIGLENGTH        64      // Ed25519 signature
#define RNS_KEYSIZE          64      // X25519(32) + Ed25519(32)
#define RNS_RATCHETSIZE      32
#define RNS_TOKEN_OVERHEAD   48      // IV(16) + HMAC(32)
#define RNS_MAX_HOPS        128
#define RNS_PLAIN_MDU       464
#define RNS_ENCRYPTED_MDU   383
#define RNS_ANNOUNCE_CAP_PCT  2      // percent of airtime budget
#define RNS_TRANSPORT_DEST_NAME "lxmf.delivery"

// RatDeck/RNode LoRa framing
#define RNODE_LORA_HEADER_ENABLED 1
#define RNODE_LORA_HEADER_FLAGS_UNSPLIT 0x00

// ── Transport tuning (fits comfortably in 256 KB RAM) ────
#define PATH_TABLE_MAX       64
#define HASH_CACHE_MAX      128
#define ANNOUNCE_QUEUE_MAX    8
#define PATH_EXPIRY_MS       (24UL * 3600UL * 1000UL)  // 24 h
#define DEDUP_EXPIRY_MS      (5UL  * 60UL   * 1000UL)  // 5 min
#define ANNOUNCE_JITTER_MS   2000
#define TRANSPORT_LOOP_MS    5
#define ANNOUNCE_STARTUP_DELAY_MS  2000UL
#define ANNOUNCE_INTERVAL_MS       (60UL * 1000UL)
#define ANNOUNCE_FAST_INTERVAL_MS  (15UL * 1000UL)   // faster announces for first 5 min after boot
#define ANNOUNCE_FAST_PERIOD_MS    (5UL * 60UL * 1000UL)

// ── Peer messaging ────────────────────────────────────────
#define PEER_NAME_MAX        16
#define MSG_NOTIFY_FILE      "/notify.bin"

// Notification modes for incoming peer messages
enum MsgNotifyMode : uint8_t {
    NOTIFY_SOUND  = 0,   // web UI plays audio notification
    NOTIFY_MORSE  = 1,   // blink morse on blue LED
    NOTIFY_BOTH   = 2,   // audio + morse
    NOTIFY_SILENT = 3,   // suppress all notification
};

// ── Watchdog timeout (seconds) ───────────────────────────
#define WDT_TIMEOUT_SEC      8

// ── LittleFS persistence ─────────────────────────────────
#define IDENTITY_FILE        "/identity.bin"
#define CONFIG_FILE          "/config.bin"
#define ANNOUNCE_NAME_FILE   "/announce_name.txt"
#define MORSE_CONFIG_FILE    "/morse.bin"
#define PATH_TABLE_FILE      "/paths.bin"

// ── Safe-boot: hold this pin LOW during reset to skip main app
//    and enter a minimal serial console for recovery ──────
// On WisBlock the user button varies; we use a console "safeboot"
// flag stored in flash instead.
#define SAFE_BOOT_MAGIC      0xDEADBEEF
