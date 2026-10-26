/**
 * @file RNSConfig.h
 * @brief Hardware pins, LoRa defaults, Reticulum protocol constants,
 *        and transport-engine tuning.
 *
 * Board selection (exactly one, set from platformio.ini build_flags):
 *   (default)            WisBlock 1W  — RAK3401 + RAK13302 (SKY66122 PA)
 *   BOARD_IKOKA_STICK    Ikoka Stick  — Seeed XIAO nRF52840 + EBYTE E22
 *
 * RF-module selection for the TX-power invariant (see "TX power" below):
 *   (default)                 RAK13302
 *   RADIO_MODULE_E22_900M30S  EBYTE E22-900M30S (30 dBm PA)
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
#define FW_VERSION_PATCH    33
#define FW_VERSION_STRING   "1.0.33"
#define FW_PRODUCT_NAME     "RatTunnel"
#define FW_DISPLAY_VERSION  "RatTunnel V. 1.0.33"
#if defined(BOARD_IKOKA_STICK)
#define FW_BUILD_TAG        "rattunnel-ikoka-stick"
#else
#define FW_BUILD_TAG        "rattunnel-wisblock1w"
#endif

#if defined(BOARD_IKOKA_STICK)
// ── Ikoka Stick (Seeed XIAO nRF52840 + EBYTE E22-900M30S) pin mapping ─
// Board: https://github.com/ndoo/ikoka-stick-meshtastic-device
// Pin numbers are raw nRF52840 GPIO (pca10056 variant: P0.x = x,
// P1.x = 32 + x), the same convention as the WisBlock map below.
//
//   XIAO  nRF52840  Function      Source
//   D0    P0.02     user button   Ikoka README GPIO table (pull-up on
//                                 board since commit d419bfd, active LOW)
//   D1    P0.03     E22 DIO1      Ikoka README GPIO table
//   D2    P0.28     E22 RST       Ikoka README GPIO table
//   D3    P0.29     E22 BUSY      Ikoka README GPIO table
//   D4    P0.04     E22 NSS       Ikoka README GPIO table
//   D5    P0.05     E22 RXEN      Ikoka README GPIO table
//   D8    P1.13     E22 SCK       Ikoka README GPIO table
//   D9    P1.14     E22 MISO      Ikoka README GPIO table
//   D10   P1.15     E22 MOSI      Ikoka README GPIO table
//   —     —         E22 TXEN      NOT a GPIO. Wired to SX1262 DIO2 in
//                                 ikoka-stick-meshtastic-device.kicad_sch
//                                 (net TXEN(187.96,57.15) → DIO2
//                                 (223.52,52.07)). Driven by
//                                 setDio2AsRfSwitch(true); RXEN via
//                                 setRfSwitchPins(RXEN, NC). Matches
//                                 MeshCore variants/ikoka_stick_nrf/
//                                 platformio.ini (SX126X_TXEN=RADIOLIB_NC,
//                                 SX126X_DIO2_AS_RF_SWITCH=1).
//   D6/D7 P1.11/12  SSD1306 I2C   optional OLED, not used by this firmware
#define BOARD_DISPLAY_NAME  "Ikoka Stick"
#define BOARD_MODULE_NAME   "XIAO nRF52840 + E22-900M30S"
#define PIN_LORA_NSS         4   // P0.04  D4
#define PIN_LORA_SCK        45   // P1.13  D8
#define PIN_LORA_MISO       46   // P1.14  D9
#define PIN_LORA_MOSI       47   // P1.15  D10
#define PIN_LORA_DIO1_ACTIVE true   // real DIO1 line → interrupt-driven RX
#define PIN_LORA_DIO1_PIN    3   // P0.03  D1
#define PIN_LORA_BUSY       29   // P0.29  D3
#define PIN_LORA_RESET      28   // P0.28  D2
#define PIN_LORA_RXEN        5   // P0.05  D5 — E22 LNA/RF-switch RX enable
#define PIN_LORA_ENABLE     -1   // no gated rail (MT3608 boost is always on)
#define RADIO_HAS_RAK_PIN_DISCOVERY 0  // skip WisBlock P34/NRST/BUSY probing

// LEDs — XIAO on-board RGB, common-anode, ACTIVE LOW.
// Pins from Seeed Adafruit_nRF52_Arduino variants/Seeed_XIAO_nRF52840/
// variant.cpp (D11=P0.26 red, D12=P0.06 blue, D13=P0.30 green);
// polarity from MeshCore variants/ikoka_stick_nrf/variant.h LED_STATE_ON (0).
#define PIN_LED_GREEN       30   // P0.30
#define PIN_LED_BLUE         6   // P0.06
#define PIN_LED_RED         26   // P0.26
#define LED_ACTIVE_HIGH      0

// User button — D0 / P0.02, active LOW (board pull-up). Hold during
// boot to reboot into the UF2 bootloader (same GPREGRET path as the
// console `dfu` command; the bootloader itself is never touched).
#define PIN_USER_BUTTON      2   // P0.02
#define BUTTON_ACTIVE_LOW    1
#define BUTTON_DFU_HOLD_MS   2000

#else
// ── WisBlock 1W (RAK3401 + RAK13302) pin mapping ─────────
#define BOARD_DISPLAY_NAME  "WisBlock 1W"
#define BOARD_MODULE_NAME   "RAK3401 + RAK13302"
#define PIN_LORA_NSS        26   // WB_SPI_CS
#define PIN_LORA_SCK         3   // WB_SPI_CLK
#define PIN_LORA_MISO       29   // WB_SPI_MISO
#define PIN_LORA_MOSI       30   // WB_SPI_MOSI
#define PIN_LORA_DIO1_ACTIVE false  // P15 is floating HIGH (not real DIO1); use IRQ polling
#define PIN_LORA_DIO1_PIN   15     // physical pin (only for diagnostics/pintest)
#define PIN_LORA_BUSY        9   // discovered P9 via Phase C timing probe
#define PIN_LORA_RESET       4   // discovered P4 via Phase B brute-force
#define PIN_LORA_ENABLE     34   // WB_IO2 → 3V3_S gate (P34, tested V1.0.12)
#define PIN_LORA_RXEN       -1   // RAK13302 RF switch is DIO2-only
#define RADIO_HAS_RAK_PIN_DISCOVERY 1

// LEDs (active HIGH)
#define PIN_LED_GREEN       35
#define PIN_LED_BLUE        36
#define PIN_LED_RED         -1
#define LED_ACTIVE_HIGH      1

// No user button on the WisBlock base (see SAFE_BOOT note below)
#define PIN_USER_BUTTON     -1
#define BUTTON_ACTIVE_LOW    1
#define BUTTON_DFU_HOLD_MS   2000
#endif

// ── LoRa default parameters ──────────────────────────────
#define LORA_FREQ_MHZ       915.0f   // US ISM band
#define LORA_BW_KHZ         125.0f
#define LORA_SF             9
#define LORA_CR             5        // coding rate 4/5

// ── TX power: HARD FIRMWARE INVARIANT, NOT A USER SETTING ─────────────
// The SX1262 setpoint written to the chip is clamped to
// LORA_TX_DBM_MAX_SAFE in RNSRadio::clampTxDbm(), which every write of
// the setpoint (begin(), setTxPower(), console scan re-inits) goes
// through. No console command, config file, profile or region path can
// exceed it. LORA_TX_DBM_VARIANT_MAX is the per-RF-module ceiling; the
// static_asserts below refuse to build a cap above it.
#if defined(RADIO_MODULE_E22_900M30S)
// EBYTE E22-900M30S = SX1262 + PA + LNA, 30 dBm at the antenna.
//
// Ebyte's own user manual (E22-900M30S_UserManual_EN v1.20 §2.2 p.3;
// v1.5 rev 1.4 §2.2 p.5) states only "Max Tx power 29.5/30.0/31 dBm"
// and "TX current 650 mA"; it publishes NO PA gain figure and NO
// chip-setting-to-output table. So the drive level cannot be derived
// from the datasheet alone.
//
// Ceiling taken from MeshCore variants/ikoka_stick_nrf/platformio.ini
// (commit 0679dbe), env [ikoka_stick_nrf_e22_30dbm]:
//   "limit txpower to 20dBm on E22-900M30S. Anything higher will
//    cause distortion in the PA output. 20dBm in -> 30dBm out"
// i.e. ~10 dB PA gain; 20 dBm chip drive is the field-proven maximum.
// (The often-quoted 9 dBm limit is MeshCore's rule for the E22-900M33S,
// a different module: "9dBm in -> 33dBm out".)
//
// Cap is set 2 dB under that ceiling. Expected output at 18 dBm drive
// is ~28 dBm; MUST be confirmed with a power meter before sustained TX
// (see docs/BENCH_IKOKA_STICK.md).
#define LORA_TX_DBM_VARIANT_MAX   20   // MeshCore-proven ceiling for E22-900M30S
#define LORA_TX_DBM_MAX_SAFE      18   // configured cap for this build
#define LORA_TX_DBM               18   // default setpoint (SX1262 dBm, before PA)
#define LORA_TX_DBM_ANNOUNCE_SAFE 18   // announce-time cap (same PA basis; the
                                       // MT3608 rail is rated 2.5 A since Ikoka
                                       // 84d24a0 so no extra derating is applied)
#else
// RAK13302 (SX1262 + SKY66122 PA, ~+8 dB). Values unchanged from V1.0.33.
#define LORA_TX_DBM_VARIANT_MAX   17
#define LORA_TX_DBM               17       // safer default for power stability (PA adds ~8 dB)
#define LORA_TX_DBM_MAX_SAFE      17
#define LORA_TX_DBM_ANNOUNCE_SAFE LORA_TX_DBM  // Use full TX power so peers discover us
#endif
#define LORA_TX_DBM_MIN           -9       // SX1262 minimum

static_assert(LORA_TX_DBM_MAX_SAFE <= LORA_TX_DBM_VARIANT_MAX,
              "LORA_TX_DBM_MAX_SAFE exceeds the RF module's variant maximum");
static_assert(LORA_TX_DBM <= LORA_TX_DBM_MAX_SAFE,
              "LORA_TX_DBM default exceeds LORA_TX_DBM_MAX_SAFE");
static_assert(LORA_TX_DBM_ANNOUNCE_SAFE <= LORA_TX_DBM_MAX_SAFE,
              "LORA_TX_DBM_ANNOUNCE_SAFE exceeds LORA_TX_DBM_MAX_SAFE");
static_assert(LORA_TX_DBM >= LORA_TX_DBM_MIN && LORA_TX_DBM_ANNOUNCE_SAFE >= LORA_TX_DBM_MIN,
              "TX power below SX1262 minimum");
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
#define PATH_TABLE_MAX      200
#define HASH_CACHE_MAX      256
#define ANNOUNCE_QUEUE_MAX   32
#define ANNOUNCE_CACHE_MAX  128
#define ANNOUNCE_CACHE_RAW_MAX 256
#define PATH_EXPIRY_MS       (24UL * 3600UL * 1000UL)  // 24 h
#define DEDUP_EXPIRY_MS      (5UL  * 60UL   * 1000UL)  // 5 min
#define ANNOUNCE_JITTER_MS   2000
#define TRANSPORT_LOOP_MS    5
#define ANNOUNCE_STARTUP_DELAY_MS  2000UL
#define ANNOUNCE_INTERVAL_MS       (60UL * 1000UL)
#define ANNOUNCE_FAST_INTERVAL_MS  (15UL * 1000UL)   // faster announces for first 5 min after boot
#define ANNOUNCE_FAST_PERIOD_MS    (5UL * 60UL * 1000UL)
#define ANNOUNCE_INTERVAL_MIN_SEC  15U
#define ANNOUNCE_INTERVAL_MAX_SEC  3600U
#define DISCOVERY_STARTUP_DELAY_MS (ANNOUNCE_STARTUP_DELAY_MS + 3000UL)
#define DISCOVERY_INTERVAL_MS      (2UL * 60UL * 1000UL)
#define DISCOVERY_FAST_INTERVAL_MS (30UL * 1000UL)
#define DISCOVERY_INTERVAL_MIN_SEC 30U
#define DISCOVERY_INTERVAL_MAX_SEC 7200U
#define DISCOVERY_RESPONSE_COOLDOWN_MS 3000UL

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

enum LedAlertMode : uint8_t {
    LED_ALERT_ONCE = 0,
    LED_ALERT_REPEAT_COUNT = 1,
    LED_ALERT_UNTIL_CLEAR = 2,
};

static const uint8_t LED_ALERT_PREFIX_BYTES = 8;
static const uint8_t LED_ALERT_WATCH_MAX = 6;
static const uint8_t LED_CONFIG_VERSION = 3;

// ── Watchdog timeout (seconds) ───────────────────────────
#define WDT_TIMEOUT_SEC      8

// ── LittleFS persistence ─────────────────────────────────
#define IDENTITY_FILE        "/identity.bin"
#define CONFIG_FILE          "/config.bin"
#define ANNOUNCE_NAME_FILE   "/announce_name.txt"
#define MORSE_CONFIG_FILE    "/morse.bin"
#define LED_CONFIG_FILE      "/leds.bin"
#define SECURITY_CONFIG_FILE "/security.bin"
#define PATH_TABLE_FILE      "/paths.bin"
#define AUTH_FILE            "/auth.bin"

// ── Safe-boot: hold this pin LOW during reset to skip main app
//    and enter a minimal serial console for recovery ──────
// On WisBlock the user button varies; we use a console "safeboot"
// flag stored in flash instead.
#define SAFE_BOOT_MAGIC      0xDEADBEEF
