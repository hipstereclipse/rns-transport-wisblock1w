/**
 * @file RNSConfig.h
 * @brief Hardware pins, LoRa defaults, Reticulum protocol constants,
 *        and transport-engine tuning.
 *
 * Board selection (exactly one, set from platformio.ini build_flags):
 *   (default)            WisBlock 1W  — RAK3401 + RAK13302 (SKY66122 PA)
 *   BOARD_IKOKA_STICK    Ikoka Stick  — Seeed XIAO nRF52840 + EBYTE E22
 *   BOARD_HELTEC_T096    Heltec Mesh Node T096 — nRF52840 + SX1262 + KCT8103L PA
 *
 * RF-module selection for the TX-power invariant (see "TX power" below):
 *   (default)                 RAK13302
 *   RADIO_MODULE_E22_900M30S  EBYTE E22-900M30S (30 dBm PA)
 *   RADIO_MODULE_KCT8103L     Heltec T096 on-board KCT8103L FEM (~28 dBm)
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
#define FW_VERSION_PATCH    34
#define FW_VERSION_STRING   "1.0.34"
#define FW_PRODUCT_NAME     "RatTunnel"
#define FW_DISPLAY_VERSION  "RatTunnel V. 1.0.34"
#if defined(BOARD_IKOKA_STICK)
#define FW_BUILD_TAG        "rattunnel-ikoka-stick"
#elif defined(BOARD_HELTEC_T096)
#define FW_BUILD_TAG        "rattunnel-heltec-t096"
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

#elif defined(BOARD_HELTEC_T096)
// ── Heltec Mesh Node T096 (nRF52840 + SX1262 + KCT8103L FEM) pin map ──
// Sources: Heltec schematic Mesh_Node_T096_V0.2.pdf (net table, sheet 1),
// cross-checked against Meshtastic variants/nrf52840/heltec_mesh_node_t096
// and MeshCore variants/heltec_t096. Raw nRF52840 GPIO numbers (pca10056:
// P0.x = x, P1.x = 32 + x).
//
//   Net         GPIO    Function
//   LoRa_NSS    P0.05   SX1262 NSS (10K pull-up on board)
//   LoRa_SCK    P1.08   SX1262 SCK
//   LoRa_MOSI   P0.11   SX1262 MOSI
//   LoRa_MISO   P0.14   SX1262 MISO
//   LoRa_RST    P0.16   SX1262 NRESET
//   LoRa_BUSY   P0.19   SX1262 BUSY
//   DIO1        P0.21   SX1262 DIO1 (real line → interrupt-driven RX)
//   —           —       SX1262 DIO2 → KCT8103L CPS (TX/RX path select),
//                       10K pulldown. setDio2AsRfSwitch(true).
//   —           —       SX1262 DIO3 → 32 MHz TCXO supply (1.8 V).
//   VFEM_Ctrl   P0.30   TLV75733 LDO enable for the FEM rail (Vfem).
//                       5.1M pull-up to 3V3 → weakly ON by default; we
//                       drive it HIGH explicitly.
//   PA_CSD      P0.12   KCT8103L chip enable (HIGH = on). 10K pulldown →
//                       the PA is OFF until firmware asserts it.
//   PA_CTX      P1.09   KCT8103L RX path: LOW = 21 dB LNA, HIGH = bypass.
//                       10K pulldown → LNA mode by default.
//   Vext_Ctrl   P0.26   CE6260 LDO enable for Vext_3V3 (TFT + backlight +
//                       header). 100K pulldown → OFF by default. The
//                       SX1262 VDD_IN/VBAT are on the ALWAYS-ON VDD_3V3
//                       rail (U1), so the radio does not need Vext.
//   VGNSS_CTRL  P0.06   PMOS gate for the UC6580 GNSS rail: LOW = on,
//                       HIGH = off. Driven HIGH (GNSS unused).
//   LED         P0.28   White LED, 330R to GND → active HIGH.
//   Button      P1.10   USER_SW to GND, 10K pull-up (R21) → active LOW.
//   ADC_Ctrl    P1.15   Battery divider gate (unused here).
//   ADC_IN      P0.03   Battery divider (390K/100K) (unused here).
#define BOARD_DISPLAY_NAME  "Heltec T096"
#define BOARD_MODULE_NAME   "SX1262 + KCT8103L"
#define PIN_LORA_NSS         5   // P0.05
#define PIN_LORA_SCK        40   // P1.08
#define PIN_LORA_MISO       14   // P0.14
#define PIN_LORA_MOSI       11   // P0.11
#define PIN_LORA_DIO1_ACTIVE true   // real DIO1 line → interrupt-driven RX
#define PIN_LORA_DIO1_PIN   21   // P0.21
#define PIN_LORA_BUSY       19   // P0.19
#define PIN_LORA_RESET      16   // P0.16
#define PIN_LORA_RXEN       -1   // no separate RX-enable line
#define PIN_LORA_TXEN       41   // P1.09 PA_CTX: KCT8103L needs CTX HIGH during TX,
                                 // LOW for the RX LNA path. Driven per packet by
                                 // RadioLib (setRfSwitchPins), CPS by DIO2.
#define PIN_LORA_ENABLE     -1   // radio rail is always on (VDD_3V3)
#define RADIO_HAS_RAK_PIN_DISCOVERY 0  // fixed, schematic-verified pin map

// KCT8103L front-end control (MeshCore variants/heltec_t096/LoRaFEMControl):
//   TX:      CSD=1, CTX=1, CPS=1      RX (LNA): CSD=1, CTX=0, CPS=0
// VFEM and CSD are enabled once in RNSRadio::begin(). CTX MUST toggle
// per packet: it is PIN_LORA_TXEN above, so RadioLib raises it for
// every transmit and drops it for receive; DIO2 drives CPS. Holding CTX
// low (builds up to 2026-10-01) sent TX through the receive path: the
// node was heard at -60..-76 dBm from 10 cm, ~50 dB low, and never at
// the deployment site, while it received everyone fine.
#define PIN_PA_VFEM         30   // P0.30  VFEM_Ctrl  → HIGH
#define PIN_PA_CSD          12   // P0.12  PA_CSD     → HIGH
#define PIN_PA_CTX          PIN_LORA_TXEN   // P1.09, per-packet via RadioLib
#define PA_SETTLE_MS         5

// Peripherals: Vext (TFT rail) is owned by RNSDisplay and is only high
// while the panel is awake; the GNSS rail is owned by RNSGnss (`gps on`).
#define PIN_VEXT_CTRL       26   // P0.26 → HIGH powers TFT + backlight rail
#define PIN_GNSS_CTRL        6   // P0.06 PMOS gate: LOW = GNSS rail on, HIGH = off

// ── UC6580 GNSS (schematic sheet 1, U10) ──────────────────────────────
// Parked 2026-10-01 (Gene): compiled out until revisited. The GNSS rail
// is switched from Vext, which the display also owns (see RNSVext.h);
// the shared-rail handling is in place but the feature is off.
#define HAS_GNSS             0
#define PIN_GNSS_RX         23   // P0.23  GNSS_TX net → nRF UART RX
#define PIN_GNSS_TX         25   // P0.25  GNSS_RX net ← nRF UART TX
#define PIN_GNSS_RESET      46   // P1.14  active LOW, held HIGH
#define PIN_GNSS_PPS        43   // P1.11  (unused)
#define GNSS_BAUD           115200

// ── 0.96" ST7735 TFT (80x160, "mini 160x80 plugin" init) ─────────────
// Schematic sheet 1, FFC U2: SCL P0.20, SDA P0.17, RST P0.13, CS P0.22,
// RS(DC) P0.15, LEDA backlight via Q1 PMOS on P1.12 (LOW = on).
// Panel is on the Vext rail. Second SPI master (SPIM2); the SX1262 keeps
// SPIM3. MISO is not wired; SPIClass needs a pin so an unused GPIO is
// named (P0.27 has no net on the schematic).
#define HAS_DISPLAY          1
#define PIN_TFT_SCK         20   // P0.20
#define PIN_TFT_MOSI        17   // P0.17
#define PIN_TFT_MISO_UNUSED 27   // P0.27 — no net; placeholder for SPIClass
#define PIN_TFT_CS          22   // P0.22
#define PIN_TFT_DC          15   // P0.15
#define PIN_TFT_RST         13   // P0.13
#define PIN_TFT_BL          44   // P1.12 LEDA_K via PMOS: LOW = backlight on
#define TFT_BL_ACTIVE_LOW    1
#define TFT_ROTATION         1   // landscape, 160x80, header pins on the left

// ── Battery sense: 390K/100K divider on P0.03 (AIN1), gated by P1.15 ──
// Q7 (NPN) → Q6 (PMOS): ADC_Ctrl HIGH connects the divider. Multiplier
// (390+100)/100 = 4.9; Meshtastic uses 4.916 for this board.
#define PIN_BATT_ADC         3   // P0.03 / AIN1
#define PIN_BATT_ADC_CTRL   47   // P1.15
#define BATT_DIVIDER_MULT    4.916f

// LED — single white LED on P0.28, active HIGH. Mapped to the "green"
// channel so the heartbeat / alert code paths work unchanged.
#define PIN_LED_GREEN       28   // P0.28
#define PIN_LED_BLUE        -1
#define PIN_LED_RED         -1
#define LED_ACTIVE_HIGH      1

// User button — P1.10, active LOW (10K pull-up on board). Hold during
// boot to reboot into the UF2 bootloader (GPREGRET 0x57, same as the
// console `dfu` command; the Heltec bootloader is Adafruit-derived).
#define PIN_USER_BUTTON     42   // P1.10
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

#ifndef HAS_GNSS
#define HAS_GNSS             0
#endif
#define GNSS_CONFIG_VERSION  2
#define GNSS_LOCATION_SYNC_MIN_M   100    // move this far before the map position updates
#define GNSS_LOCATION_SYNC_HOLDOFF_MS (10UL * 60UL * 1000UL)

// ── Display (boards with HAS_DISPLAY) ─────────────────────
#ifndef HAS_DISPLAY
#define HAS_DISPLAY          0
#endif
#define DISPLAY_TIMEOUT_DEFAULT_SEC  60     // 0 = never blank
#define DISPLAY_TIMEOUT_MAX_SEC      3600
#define DISPLAY_REFRESH_MS           1000
#define DISPLAY_CONFIG_VERSION       1

#ifndef PIN_LORA_TXEN
#define PIN_LORA_TXEN       -1
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
#elif defined(RADIO_MODULE_KCT8103L)
// Heltec T096: SX1262 RFO → match → 21 dB pi pad → KCT8103L FEM (PA +
// LNA) → antenna. Heltec quotes 28 ± 1 dBm at the antenna.
//
// Verified 2026-09-25 from primary sources (see README "TX power"):
//  * Schematic Mesh_Node_T096_V0.2 sheet 1: pad R37 = 280 R series,
//    R38 = R39 = 59 R shunt → 21.1 dB insertion loss in 50 Ω. So the
//    FEM TX input sees chip − 21.1 dB: 22 dBm chip → +0.9 dBm at TX.
//  * Kangxi product portfolio, KCT8103L row: 0.86–0.93 GHz, TX gain
//    33.0 dB and Psat 28.0 dBm at 3.3 V (33.5 dB / 32.0 dBm at 5 V).
//    Vfem on the T096 is a 3.3 V LDO (U5 TLV75733), so Psat = 28 dBm.
//  * KCT8101L datasheet (same family, Oct 2019 rev C): absolute max TX
//    RF input +8 dBm. No KCT8103L datasheet was obtainable; +8 dBm is
//    used as the family figure, with the margin below noted.
//
// Consequences:
//  * Psat is reached at TX input −5 dBm = 16 dBm chip drive. Above 16
//    the FEM is in saturation: output stays ~28 dBm, current rises.
//    Matches the Heltec V4 bench table (MeshCore #1708, same FEM
//    family): flat 27–28 dBm from setting 18 upward.
//  * Even the full 22 dBm chip drive puts only +0.9 dBm into the FEM,
//    7 dB below the family's absolute-maximum input. The board's pad
//    makes the whole SX1262 range electrically safe for the FEM, which
//    is why Meshtastic and MeshCore both allow 22 here.
//
// Cap: 18 dBm drive = 2 dB past the saturation knee, ~28 dBm out,
// −3.1 dBm at the FEM input, 11 dB below the family abs-max. Nothing
// above 18 buys output. A power-meter check is still the sign-off for
// the bench sheet (docs/BENCH_HELTEC_T096.md), not a safety gate.
#define LORA_TX_DBM_VARIANT_MAX   22   // full SX1262 range per Meshtastic/MeshCore
#define LORA_TX_DBM_MAX_SAFE      18   // configured cap for this build
#define LORA_TX_DBM               18   // default setpoint (SX1262 dBm, before FEM)
#define LORA_TX_DBM_ANNOUNCE_SAFE 18   // announce-time cap (same FEM basis)
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
#define RNODE_LORA_FLAG_SPLIT     0x01   // RNode: packet spans two LoRa frames

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
#define DISPLAY_CONFIG_FILE  "/display.bin"
#define DISCOVERY_CONFIG_FILE "/discovery.bin"
#define GNSS_CONFIG_FILE     "/gps.bin"
#define SECURITY_CONFIG_FILE "/security.bin"
#define PATH_TABLE_FILE      "/paths.bin"
#define AUTH_FILE            "/auth.bin"

// ── Safe-boot: hold this pin LOW during reset to skip main app
//    and enter a minimal serial console for recovery ──────
// On WisBlock the user button varies; we use a console "safeboot"
// flag stored in flash instead.
#define SAFE_BOOT_MAGIC      0xDEADBEEF
