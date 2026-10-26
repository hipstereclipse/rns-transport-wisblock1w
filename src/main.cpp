/**
 * @file main.cpp
 * @brief Application entry point for the RNS Transport Node.
 *
 * Initialization order:
 *   1. LEDs and watchdog
 *   2. USB serial
 *   3. LittleFS (persistence)
 *   4. Identity (load or generate)
 *   5. Transport engine
 *   6. Radio (SX1262 + board PA stage, see RNSConfig.h)
 *   7. Console
 *
 * SAFETY:
 *   - Hardware watchdog resets the MCU if the main loop stalls for
 *     more than WDT_TIMEOUT_SEC seconds.
 *   - Radio failure does NOT brick the device: it enters a visible
 *     error blink mode and the USB console remains active for
 *     diagnosis and DFU triggering.
 *   - The bootloader is never modified.
 */
#include <Arduino.h>
#include "RNSConfig.h"
#include "RNSIdentity.h"
#include "RNSPacket.h"
#include "RNSRadio.h"
#include "RNSTransport.h"
#include "RNSConsole.h"
#include "RNSDisplay.h"
#include "RNSDiscovery.h"
#include "RNSGnss.h"
#include "RNSVext.h"
#include "RNSEntropy.h"
#include "RNSPersistence.h"
#include "RNSDfu.h"
#include <nrf_wdt.h>
#include <ctype.h>

// ── Global objects (static allocation) ────────────────────
static RNSIdentity    nodeIdentity;
static RNSRadio       radio;
#if HAS_DISPLAY
static RNSDisplay     display;
#endif
static RNSDiscovery   discovery;
uint8_t RNSDiscovery::workblock[DISCOVERY_WORKBLOCK_LEN];
#if defined(PIN_VEXT_CTRL)
uint8_t RNSVext::claims = 0;
bool    RNSVext::on = false;
#endif
#if HAS_GNSS
static RNSGnss        gnss;
static uint32_t gnssWallClock() { return gnss.unixNow(millis()); }
#endif
static RNSTransport   transport;
static RNSConsole     console;
static RNSPersistence persist;

static bool radioOk = false;
static uint32_t nextAnnounceAt = 0;
static uint32_t nextDiscoveryAt = 0;
static uint32_t bootTime       = 0;
static bool bootLooksLikePowerOn = true;
static uint32_t announceIntervalMs = ANNOUNCE_INTERVAL_MS;
static uint32_t discoveryIntervalMs = DISCOVERY_INTERVAL_MS;

static void printResetReason() {
#ifndef NATIVE_TEST
    uint32_t reas = NRF_POWER->RESETREAS;
    if (reas == 0) {
        bootLooksLikePowerOn = true;
        Serial.println(F("[RNS] Reset cause: unknown/power-on"));
        return;
    }

    bootLooksLikePowerOn = false;

    Serial.print(F("[RNS] Reset cause: "));
    bool first = true;

#ifdef POWER_RESETREAS_RESETPIN_Msk
    if (reas & POWER_RESETREAS_RESETPIN_Msk) { Serial.print(F("pin")); first = false; }
#endif
#ifdef POWER_RESETREAS_DOG_Msk
    if (reas & POWER_RESETREAS_DOG_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("watchdog")); first = false; }
#endif
#ifdef POWER_RESETREAS_SREQ_Msk
    if (reas & POWER_RESETREAS_SREQ_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("software")); first = false; }
#endif
#ifdef POWER_RESETREAS_LOCKUP_Msk
    if (reas & POWER_RESETREAS_LOCKUP_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("lockup")); first = false; }
#endif
#ifdef POWER_RESETREAS_OFF_Msk
    if (reas & POWER_RESETREAS_OFF_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("wake-from-off")); first = false; bootLooksLikePowerOn = true; }
#endif
#ifdef POWER_RESETREAS_LPCOMP_Msk
    if (reas & POWER_RESETREAS_LPCOMP_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("lpcomp")); first = false; }
#endif
#ifdef POWER_RESETREAS_DIF_Msk
    if (reas & POWER_RESETREAS_DIF_Msk) { if (!first) Serial.print(F(", ")); Serial.print(F("debug-interface")); first = false; }
#endif

    if (first) {
        Serial.print(F("0x"));
        Serial.print(reas, HEX);
    }
    Serial.println();

    // Clear latched reason bits by writing 1s to the set positions.
    NRF_POWER->RESETREAS = reas;
#endif
}

enum MorseBlinkMode : uint8_t {
    MORSE_MODE_OFF      = 0,
    MORSE_MODE_ERRORS   = 1,
    MORSE_MODE_INCOMING = 2,
    MORSE_MODE_BOTH     = 3,
    MORSE_MODE_DEFAULT  = 4,
};

struct MorseBlinkConfigState {
    uint8_t mode = MORSE_MODE_OFF;
    char defaultMessage[17] = "";
};

static MorseBlinkConfigState morseCfg;
static uint32_t lastHeartbeatToggle = 0;
static bool heartbeatPhaseOn = true;

enum LedChannelId : uint8_t {
    LED_CH_GREEN = 0,
    LED_CH_BLUE = 1,
    LED_CH_RED = 2,
    LED_CH_COUNT = 3,
};

enum LedIdleMode : uint8_t {
    LED_IDLE_OFF = 0,
    LED_IDLE_SOLID = 1,
    LED_IDLE_HEARTBEAT = 2,
};

static const uint8_t LED_MASK_GREEN = (1u << LED_CH_GREEN);
static const uint8_t LED_MASK_BLUE  = (1u << LED_CH_BLUE);
static const uint8_t LED_MASK_RED   = (1u << LED_CH_RED);

struct LedChannelConfigState {
    uint8_t idleMode = LED_IDLE_OFF;
    bool rxFlashEnabled = false;
    bool txFlashEnabled = false;
    uint8_t morseMode = MORSE_MODE_OFF;
};

struct LedBehaviorState {
    LedChannelConfigState channels[LED_CH_COUNT];

    LedBehaviorState() {
        channels[LED_CH_GREEN].idleMode = LED_IDLE_HEARTBEAT;
        channels[LED_CH_BLUE].idleMode = LED_IDLE_OFF;
        channels[LED_CH_BLUE].rxFlashEnabled = true;
        channels[LED_CH_BLUE].txFlashEnabled = true;
        channels[LED_CH_BLUE].morseMode = MORSE_MODE_ERRORS;
    }
};

static LedBehaviorState ledCfg;

struct LedAlertBehaviorState {
    uint8_t mode = LED_ALERT_ONCE;
    uint8_t repeatCount = 3;
    uint16_t intervalSec = 20;
    uint8_t watchCount = 0;
    uint8_t watchPrefixes[LED_ALERT_WATCH_MAX][LED_ALERT_PREFIX_BYTES] = {{0}};
    uint8_t watchTargetMasks[LED_ALERT_WATCH_MAX] = {0};
};

struct PendingLedAlert {
    uint8_t prefix[LED_ALERT_PREFIX_BYTES] = {0};
    uint8_t targetMask = 0;
    uint8_t repeatsRemaining = 0;
    uint32_t nextAt = 0;
    bool active = false;
};

static LedAlertBehaviorState ledAlertCfg;
static PendingLedAlert pendingLedAlerts[LED_ALERT_WATCH_MAX];

struct RatHoleSecurityState {
    bool enabled = false;
    bool wipeOnBoot = false;
};

static RatHoleSecurityState ratHoleCfg;

static bool morseStepOn[128] = {0};
static uint16_t morseStepMs[128] = {0};
static uint8_t morseStepCount = 0;
static uint8_t morseStepIndex = 0;
static uint32_t morseStepUntil = 0;
static bool morsePlaybackActive = false;
static uint8_t morsePlaybackMask = 0;

static char morseQueue[4][17] = {{0}};
static uint8_t morseQueueMask[4] = {0};
static uint8_t morseQueueHead = 0;
static uint8_t morseQueueTail = 0;
static uint8_t morseQueueCount = 0;

static bool ledOutputState[LED_CH_COUNT] = {false};
static bool pulseOutputState[LED_CH_COUNT] = {false};
static uint8_t pulseTogglesRemaining[LED_CH_COUNT] = {0};
static uint32_t nextPulseToggleAt[LED_CH_COUNT] = {0};

// ── Fault codes (displayed as Morse numeric digit on green LED) ──
enum FaultCode : uint8_t {
    FAULT_RADIO_INIT = 1,
};

// ── Watchdog helpers ──────────────────────────────────────
static nrf_wdt_rr_register_t wdtChannel = NRF_WDT_RR0;

static void wdtInit() {
    // Configure and start hardware watchdog
    nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_PAUSE_SLEEP_HALT);
    nrf_wdt_reload_value_set(NRF_WDT, WDT_TIMEOUT_SEC * 32768);  // 32 kHz clock
    nrf_wdt_reload_request_enable(NRF_WDT, wdtChannel);
    nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);
}

static inline void wdtFeed() {
    nrf_wdt_reload_request_set(NRF_WDT, wdtChannel);
}

// ── LED error patterns ────────────────────────────────────
// Codes:
//   Solid green              = powered, booting
//   Green blink (2 s)        = running normally heartbeat
//   Blue pulse               = packet activity (RX/TX)
//   Green Morse digit loop   = fault code
//   Alternating green/blue   = fatal error

static void serviceDelay(uint32_t ms) {
    // Keep console and watchdog alive while delaying.
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        wdtFeed();
        console.poll();
        delay(20);
    }
}

static const char* morseDigitPattern(uint8_t digit) {
    static const char* table[10] = {
        "-----", ".----", "..---", "...--", "....-",
        ".....", "-....", "--...", "---..", "----."
    };
    return (digit <= 9) ? table[digit] : table[0];
}

static const char* morseCharPattern(char ch) {
    switch (toupper((unsigned char)ch)) {
        case 'A': return ".-";
        case 'B': return "-...";
        case 'C': return "-.-.";
        case 'D': return "-..";
        case 'E': return ".";
        case 'F': return "..-.";
        case 'G': return "--.";
        case 'H': return "....";
        case 'I': return "..";
        case 'J': return ".---";
        case 'K': return "-.-";
        case 'L': return ".-..";
        case 'M': return "--";
        case 'N': return "-.";
        case 'O': return "---";
        case 'P': return ".--.";
        case 'Q': return "--.-";
        case 'R': return ".-.";
        case 'S': return "...";
        case 'T': return "-";
        case 'U': return "..-";
        case 'V': return "...-";
        case 'W': return ".--";
        case 'X': return "-..-";
        case 'Y': return "-.--";
        case 'Z': return "--..";
        case '0': return "-----";
        case '1': return ".----";
        case '2': return "..---";
        case '3': return "...--";
        case '4': return "....-";
        case '5': return ".....";
        case '6': return "-....";
        case '7': return "--...";
        case '8': return "---..";
        case '9': return "----.";
        default:  return nullptr;
    }
}

static int ledChannelPin(uint8_t channel) {
    switch (channel) {
        case LED_CH_GREEN: return PIN_LED_GREEN;
        case LED_CH_BLUE: return PIN_LED_BLUE;
        case LED_CH_RED: return PIN_LED_RED;
        default: return -1;
    }
}

static const char* ledChannelName(uint8_t channel) {
    switch (channel) {
        case LED_CH_GREEN: return "green";
        case LED_CH_BLUE: return "blue";
        case LED_CH_RED: return "red";
        default: return "unknown";
    }
}

static bool ledChannelAvailable(uint8_t channel) {
    return ledChannelPin(channel) >= 0;
}

static uint8_t ledChannelMask(uint8_t channel) {
    return (channel < LED_CH_COUNT) ? (uint8_t)(1u << channel) : 0;
}

static uint8_t availableLedMask() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel)) mask |= ledChannelMask(channel);
    }
    return mask;
}

static bool morseModeUsesIncoming(uint8_t mode) {
    return mode == MORSE_MODE_INCOMING || mode == MORSE_MODE_BOTH || mode == MORSE_MODE_DEFAULT;
}

static bool morseModeUsesErrors(uint8_t mode) {
    return mode == MORSE_MODE_ERRORS || mode == MORSE_MODE_BOTH || mode == MORSE_MODE_DEFAULT;
}

static uint8_t morseTargetMaskForIncoming() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel) && morseModeUsesIncoming(ledCfg.channels[channel].morseMode)) {
            mask |= ledChannelMask(channel);
        }
    }
    return mask;
}

static uint8_t morseTargetMaskForErrors() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel) && morseModeUsesErrors(ledCfg.channels[channel].morseMode)) {
            mask |= ledChannelMask(channel);
        }
    }
    return mask;
}

static uint8_t morseEnabledMask() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel) && ledCfg.channels[channel].morseMode != MORSE_MODE_OFF) {
            mask |= ledChannelMask(channel);
        }
    }
    return mask;
}

static const char* morseModeName(uint8_t mode) {
    switch (mode) {
        case MORSE_MODE_OFF: return "off";
        case MORSE_MODE_ERRORS: return "errors";
        case MORSE_MODE_INCOMING: return "incoming";
        case MORSE_MODE_BOTH: return "both";
        case MORSE_MODE_DEFAULT: return "default";
        default: return "off";
    }
}

static bool parseMorseMode(const char* text, uint8_t& outMode) {
    if (!text || !*text) return false;
    if (strcmp(text, "off") == 0) outMode = MORSE_MODE_OFF;
    else if (strcmp(text, "errors") == 0 || strcmp(text, "error") == 0) outMode = MORSE_MODE_ERRORS;
    else if (strcmp(text, "incoming") == 0 || strcmp(text, "rx") == 0) outMode = MORSE_MODE_INCOMING;
    else if (strcmp(text, "both") == 0 || strcmp(text, "all") == 0) outMode = MORSE_MODE_BOTH;
    else if (strcmp(text, "default") == 0) outMode = MORSE_MODE_DEFAULT;
    else return false;
    return true;
}

static const char* ledIdleModeName(uint8_t mode) {
    switch (mode) {
        case LED_IDLE_OFF: return "off";
        case LED_IDLE_SOLID: return "solid";
        case LED_IDLE_HEARTBEAT: return "heartbeat";
        default: return "off";
    }
}

static bool parseLedIdleMode(const char* text, uint8_t& outMode) {
    if (!text || !*text) return false;
    if (strcmp(text, "off") == 0) outMode = LED_IDLE_OFF;
    else if (strcmp(text, "solid") == 0 || strcmp(text, "on") == 0) outMode = LED_IDLE_SOLID;
    else if (strcmp(text, "heartbeat") == 0 || strcmp(text, "pulse") == 0 || strcmp(text, "blink") == 0) outMode = LED_IDLE_HEARTBEAT;
    else return false;
    return true;
}

static bool parseLedChannel(const char* text, uint8_t& outChannel) {
    if (!text || !*text) return false;
    if (strcmp(text, "green") == 0) outChannel = LED_CH_GREEN;
    else if (strcmp(text, "blue") == 0) outChannel = LED_CH_BLUE;
    else if (strcmp(text, "red") == 0) outChannel = LED_CH_RED;
    else return false;
    return true;
}

static const char* onOffName(bool enabled) {
    return enabled ? "on" : "off";
}

static const char* ledAlertModeName(uint8_t mode) {
    switch (mode) {
        case LED_ALERT_ONCE: return "once";
        case LED_ALERT_REPEAT_COUNT: return "count";
        case LED_ALERT_UNTIL_CLEAR: return "until-clear";
        default: return "once";
    }
}

static bool parseLedAlertMode(const char* text, uint8_t& outMode) {
    if (!text || !*text) return false;
    if (strcmp(text, "once") == 0) outMode = LED_ALERT_ONCE;
    else if (strcmp(text, "count") == 0 || strcmp(text, "repeat") == 0) outMode = LED_ALERT_REPEAT_COUNT;
    else if (strcmp(text, "until-clear") == 0 || strcmp(text, "sticky") == 0 || strcmp(text, "until-read") == 0) outMode = LED_ALERT_UNTIL_CLEAR;
    else return false;
    return true;
}

static void printLedMask(Stream& io, uint8_t mask) {
    bool first = true;
    if (mask & LED_MASK_GREEN) {
        io.print(F("green"));
        first = false;
    }
    if (mask & LED_MASK_BLUE) {
        if (!first) io.print('+');
        io.print(F("blue"));
        first = false;
    }
    if (mask & LED_MASK_RED) {
        if (!first) io.print('+');
        io.print(F("red"));
        first = false;
    }
    if (first) io.print(F("auto"));
}

static bool parseLedMask(const char* text, uint8_t& outMask) {
    if (!text) return false;
    while (*text == ' ') text++;
    if (!*text) {
        outMask = 0;
        return true;
    }

    char buf[32] = {0};
    size_t len = strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ',' || buf[i] == '|') buf[i] = '+';
        else if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] - 'A' + 'a');
    }

    if (strcmp(buf, "auto") == 0 || strcmp(buf, "all") == 0 || strcmp(buf, "default") == 0) {
        outMask = 0;
        return true;
    }

    uint8_t mask = 0;
    char* tok = strtok(buf, "+");
    while (tok) {
        if (strcmp(tok, "green") == 0 || strcmp(tok, "g") == 0) mask |= LED_MASK_GREEN;
        else if (strcmp(tok, "blue") == 0 || strcmp(tok, "b") == 0) mask |= LED_MASK_BLUE;
        else if (strcmp(tok, "red") == 0 || strcmp(tok, "r") == 0) mask |= LED_MASK_RED;
        else return false;
        tok = strtok(nullptr, "+");
    }

    outMask = mask;
    return true;
}

static void printHashPrefix(Stream& io, const uint8_t prefix[LED_ALERT_PREFIX_BYTES]) {
    for (uint8_t i = 0; i < LED_ALERT_PREFIX_BYTES; i++) {
        if (prefix[i] < 0x10) io.print('0');
        io.print(prefix[i], HEX);
    }
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseHashPrefix(const char* text, uint8_t prefix[LED_ALERT_PREFIX_BYTES]) {
    if (!text) return false;
    while (*text == ' ') text++;
    size_t len = strlen(text);
    if (len < (LED_ALERT_PREFIX_BYTES * 2)) return false;
    for (uint8_t i = 0; i < LED_ALERT_PREFIX_BYTES; i++) {
        int hi = hexNibble(text[i * 2]);
        int lo = hexNibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        prefix[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool prefixEquals(const uint8_t a[LED_ALERT_PREFIX_BYTES], const uint8_t b[LED_ALERT_PREFIX_BYTES]) {
    return memcmp(a, b, LED_ALERT_PREFIX_BYTES) == 0;
}

static bool resolveIncomingAlertTargetMask(const uint8_t* srcHash, uint8_t& outMask) {
    outMask = 0;
    if (!srcHash) return false;

    uint8_t baseMask = morseTargetMaskForIncoming();
    if (baseMask == 0) return false;

    if (ledAlertCfg.watchCount == 0) {
        outMask = baseMask;
        return true;
    }

    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (!prefixEquals(srcHash, ledAlertCfg.watchPrefixes[i])) continue;
        uint8_t mask = ledAlertCfg.watchTargetMasks[i];
        if (mask == 0) mask = baseMask;
        mask &= baseMask;
        if (mask == 0) return false;
        outMask = mask;
        return true;
    }
    return false;
}

static uint8_t countPendingLedAlerts() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < LED_ALERT_WATCH_MAX; i++) {
        if (pendingLedAlerts[i].active) count++;
    }
    return count;
}

static void clearPendingLedAlert(const uint8_t* prefix = nullptr) {
    for (uint8_t i = 0; i < LED_ALERT_WATCH_MAX; i++) {
        if (!pendingLedAlerts[i].active) continue;
        if (!prefix || prefixEquals(prefix, pendingLedAlerts[i].prefix)) {
            pendingLedAlerts[i].active = false;
            memset(pendingLedAlerts[i].prefix, 0, sizeof(pendingLedAlerts[i].prefix));
            pendingLedAlerts[i].targetMask = 0;
            pendingLedAlerts[i].repeatsRemaining = 0;
            pendingLedAlerts[i].nextAt = 0;
        }
    }
}

static PendingLedAlert* findPendingLedAlert(const uint8_t prefix[LED_ALERT_PREFIX_BYTES], bool create) {
    PendingLedAlert* freeSlot = nullptr;
    for (uint8_t i = 0; i < LED_ALERT_WATCH_MAX; i++) {
        if (pendingLedAlerts[i].active && prefixEquals(prefix, pendingLedAlerts[i].prefix)) {
            return &pendingLedAlerts[i];
        }
        if (!pendingLedAlerts[i].active && !freeSlot) freeSlot = &pendingLedAlerts[i];
    }
    if (!create || !freeSlot) return nullptr;
    freeSlot->active = true;
    memcpy(freeSlot->prefix, prefix, LED_ALERT_PREFIX_BYTES);
    freeSlot->targetMask = 0;
    freeSlot->repeatsRemaining = 0;
    freeSlot->nextAt = 0;
    return freeSlot;
}

static bool addAlertWatchPrefix(const uint8_t prefix[LED_ALERT_PREFIX_BYTES], uint8_t targetMask) {
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (prefixEquals(prefix, ledAlertCfg.watchPrefixes[i])) {
            ledAlertCfg.watchTargetMasks[i] = targetMask;
            return true;
        }
    }
    if (ledAlertCfg.watchCount >= LED_ALERT_WATCH_MAX) return false;
    memcpy(ledAlertCfg.watchPrefixes[ledAlertCfg.watchCount], prefix, LED_ALERT_PREFIX_BYTES);
    ledAlertCfg.watchTargetMasks[ledAlertCfg.watchCount] = targetMask;
    ledAlertCfg.watchCount++;
    return true;
}

static bool removeAlertWatchPrefix(const uint8_t prefix[LED_ALERT_PREFIX_BYTES]) {
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (!prefixEquals(prefix, ledAlertCfg.watchPrefixes[i])) continue;
        for (uint8_t j = i + 1; j < ledAlertCfg.watchCount; j++) {
            memcpy(ledAlertCfg.watchPrefixes[j - 1], ledAlertCfg.watchPrefixes[j], LED_ALERT_PREFIX_BYTES);
            ledAlertCfg.watchTargetMasks[j - 1] = ledAlertCfg.watchTargetMasks[j];
        }
        memset(ledAlertCfg.watchPrefixes[ledAlertCfg.watchCount - 1], 0, LED_ALERT_PREFIX_BYTES);
        ledAlertCfg.watchTargetMasks[ledAlertCfg.watchCount - 1] = 0;
        ledAlertCfg.watchCount--;
        return true;
    }
    return false;
}

static void emitLedAlertStatus(Stream& io) {
    io.print(F("[LED_ALERT] mode="));
    io.print(ledAlertModeName(ledAlertCfg.mode));
    io.print(F(" count="));
    io.print(ledAlertCfg.repeatCount);
    io.print(F(" interval="));
    io.print(ledAlertCfg.intervalSec);
    io.print(F(" watch="));
    io.print(ledAlertCfg.watchCount);
    io.print(F(" pending="));
    io.println(countPendingLedAlerts());

    io.print(F("[LED_ALERT_WATCH]"));
    if (ledAlertCfg.watchCount == 0) {
        io.println(F(" all"));
        return;
    }
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        io.print(i == 0 ? ' ' : ',');
        printHashPrefix(io, ledAlertCfg.watchPrefixes[i]);
        io.print('@');
        printLedMask(io, ledAlertCfg.watchTargetMasks[i]);
    }
    io.println();
}

// LED_ACTIVE_HIGH (RNSConfig.h): WisBlock LEDs are active HIGH, the
// XIAO's common-anode RGB is active LOW.
static inline int ledLevel(bool on) {
    return (on == (LED_ACTIVE_HIGH != 0)) ? HIGH : LOW;
}

static void writeLedChannel(uint8_t channel, bool on) {
    if (!ledChannelAvailable(channel)) return;
    if (ledOutputState[channel] == on) return;
    digitalWrite(ledChannelPin(channel), ledLevel(on));
    ledOutputState[channel] = on;
}

static void writeLedMask(uint8_t mask, bool on) {
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (mask & ledChannelMask(channel)) writeLedChannel(channel, on);
    }
}

static bool ledBaseState(uint8_t channel) {
    switch (ledCfg.channels[channel].idleMode) {
        case LED_IDLE_SOLID: return true;
        case LED_IDLE_HEARTBEAT: return heartbeatPhaseOn;
        case LED_IDLE_OFF:
        default: return false;
    }
}

static void refreshLedOutputs(uint32_t now, bool force = false) {
    if (force || (now - lastHeartbeatToggle >= 2000UL)) {
        heartbeatPhaseOn = !heartbeatPhaseOn;
        lastHeartbeatToggle = now;
    }

    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (!ledChannelAvailable(channel)) continue;
        bool on = ledBaseState(channel);
        if (pulseTogglesRemaining[channel] > 0 || pulseOutputState[channel]) on = pulseOutputState[channel];
        if (morsePlaybackActive && (morsePlaybackMask & ledChannelMask(channel)) && morseStepIndex < morseStepCount) {
            on = morseStepOn[morseStepIndex];
        }
        writeLedChannel(channel, on);
    }
}

static uint8_t rxFlashMask() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel) && ledCfg.channels[channel].rxFlashEnabled) mask |= ledChannelMask(channel);
    }
    return mask;
}

static uint8_t txFlashMask() {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel) && ledCfg.channels[channel].txFlashEnabled) mask |= ledChannelMask(channel);
    }
    return mask;
}

static void emitLedStatus(Stream& io) {
    io.print(F("[LED_HW] available="));
    bool first = true;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (!ledChannelAvailable(channel)) continue;
        if (!first) io.print(',');
        io.print(ledChannelName(channel));
        first = false;
    }
    io.print(F(" missing="));
    first = true;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (ledChannelAvailable(channel)) continue;
        if (!first) io.print(',');
        io.print(ledChannelName(channel));
        first = false;
    }
    io.println();

    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        io.print(F("[LED_CH] "));
        io.print(ledChannelName(channel));
        io.print(F(" available="));
        io.print(onOffName(ledChannelAvailable(channel)));
        io.print(F(" idle="));
        io.print(ledIdleModeName(ledCfg.channels[channel].idleMode));
        io.print(F(" rx="));
        io.print(onOffName(ledCfg.channels[channel].rxFlashEnabled));
        io.print(F(" tx="));
        io.print(onOffName(ledCfg.channels[channel].txFlashEnabled));
        io.print(F(" morse="));
        io.println(morseModeName(ledCfg.channels[channel].morseMode));
    }
    emitLedAlertStatus(io);
}

static void emitRatHoleStatus(Stream& io) {
    io.print(F("[RATHOLE] enabled="));
    io.print(ratHoleCfg.enabled ? F("on") : F("off"));
    io.print(F(" boot_reset="));
    io.println(ratHoleCfg.wipeOnBoot ? F("on") : F("off"));
}

static void enqueueMorseMessage(const char* msg, uint8_t targetMask);
static const char* morseMessageForIncoming();

static void scheduleIncomingLedAlert(const uint8_t* srcHash) {
    uint8_t targetMask = 0;
    if (!resolveIncomingAlertTargetMask(srcHash, targetMask)) return;

    if (ledAlertCfg.mode == LED_ALERT_ONCE) {
        enqueueMorseMessage(morseMessageForIncoming(), targetMask);
        return;
    }

    if (ledAlertCfg.mode == LED_ALERT_REPEAT_COUNT && ledAlertCfg.repeatCount <= 1) {
        enqueueMorseMessage(morseMessageForIncoming(), targetMask);
        return;
    }

    PendingLedAlert* slot = findPendingLedAlert(srcHash, true);
    if (!slot) return;
    enqueueMorseMessage(morseMessageForIncoming(), targetMask);
    slot->targetMask = targetMask;

    if (ledAlertCfg.mode == LED_ALERT_REPEAT_COUNT) {
        slot->repeatsRemaining = ledAlertCfg.repeatCount > 0 ? (uint8_t)(ledAlertCfg.repeatCount - 1) : 0;
    } else {
        slot->repeatsRemaining = 0xFF;
    }
    slot->nextAt = millis() + ((uint32_t)ledAlertCfg.intervalSec * 1000UL);
}

static void servicePendingLedAlerts(uint32_t now) {
    if (ledAlertCfg.mode == LED_ALERT_ONCE || ledAlertCfg.intervalSec == 0) return;
    for (uint8_t i = 0; i < LED_ALERT_WATCH_MAX; i++) {
        PendingLedAlert& slot = pendingLedAlerts[i];
        if (!slot.active || now < slot.nextAt) continue;
        uint8_t targetMask = slot.targetMask;
        if (targetMask == 0) {
            targetMask = morseTargetMaskForIncoming();
        } else {
            targetMask &= morseTargetMaskForIncoming();
        }
        if (targetMask == 0) {
            slot.active = false;
            continue;
        }
        enqueueMorseMessage(morseMessageForIncoming(), targetMask);
        if (ledAlertCfg.mode == LED_ALERT_REPEAT_COUNT) {
            if (slot.repeatsRemaining == 0) {
                slot.active = false;
                continue;
            }
            slot.repeatsRemaining--;
            slot.nextAt = now + ((uint32_t)ledAlertCfg.intervalSec * 1000UL);
            if (slot.repeatsRemaining == 0) {
                slot.active = false;
            }
        } else {
            slot.nextAt = now + ((uint32_t)ledAlertCfg.intervalSec * 1000UL);
        }
    }
}

struct DiagReplyState {
    uint8_t srcHash[RNS_ADDR_LEN] = {0};
    uint32_t lastReplyAt = 0;
    bool active = false;
};

enum DiagPromptKind : uint8_t {
    DIAG_PROMPT_NONE = 0,
    DIAG_PROMPT_PING,
    DIAG_PROMPT_UPTIME,
    DIAG_PROMPT_MESH,
};

static DiagReplyState diagReplies[8];

struct PeerMessageReceiptState {
    uint8_t srcHash[RNS_ADDR_LEN] = {0};
    char msgId[13] = {0};
    uint32_t seenAt = 0;
    bool active = false;
};

static PeerMessageReceiptState peerMessageReceipts[24];
static const uint32_t PEER_MESSAGE_RECEIPT_EXPIRY_MS = 10UL * 60UL * 1000UL;

static void normalizeDiagPrompt(const char* src, char* dst, size_t dstLen) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    if (!src) return;

    while (*src && isspace((unsigned char)*src)) src++;

    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;

    size_t outPos = 0;
    for (size_t i = 0; i < len && outPos + 1 < dstLen; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        dst[outPos++] = c;
    }
    dst[outPos] = '\0';
}

static DiagPromptKind detectDiagPrompt(const char* text) {
    if (!text || !*text) return DIAG_PROMPT_NONE;
    if (strcmp(text, "RT?PING7") == 0) return DIAG_PROMPT_PING;
    if (strcmp(text, "RT?UP7") == 0) return DIAG_PROMPT_UPTIME;
    if (strcmp(text, "RT?MESH7") == 0) return DIAG_PROMPT_MESH;
    return DIAG_PROMPT_NONE;
}

static const char* diagUptimeBucket(uint32_t now) {
    if (now < (10UL * 60UL * 1000UL)) return "fresh";
    if (now < (60UL * 60UL * 1000UL)) return "warm";
    if (now < (24UL * 60UL * 60UL * 1000UL)) return "steady";
    return "longrun";
}

static const char* diagMeshBucket(uint16_t pathCount) {
    if (pathCount == 0) return "none";
    if (pathCount <= 5) return "few";
    return "many";
}

static int findDiagReplySlot(const uint8_t* srcHash) {
    if (!srcHash) return -1;
    int emptySlot = -1;
    int oldestSlot = -1;
    uint32_t oldestAt = UINT32_MAX;
    for (int i = 0; i < (int)(sizeof(diagReplies) / sizeof(diagReplies[0])); i++) {
        if (diagReplies[i].active && memcmp(diagReplies[i].srcHash, srcHash, RNS_ADDR_LEN) == 0) return i;
        if (!diagReplies[i].active && emptySlot < 0) emptySlot = i;
        if (diagReplies[i].lastReplyAt < oldestAt) {
            oldestAt = diagReplies[i].lastReplyAt;
            oldestSlot = i;
        }
    }
    return (emptySlot >= 0) ? emptySlot : oldestSlot;
}

static bool diagReplyAllowed(const uint8_t* srcHash, uint32_t now) {
    int slot = findDiagReplySlot(srcHash);
    if (slot < 0) return false;
    if (diagReplies[slot].active && (now - diagReplies[slot].lastReplyAt) < 15000UL) return false;
    return true;
}

static void markDiagReplySent(const uint8_t* srcHash, uint32_t now) {
    int slot = findDiagReplySlot(srcHash);
    if (slot < 0) return;
    diagReplies[slot].active = true;
    diagReplies[slot].lastReplyAt = now;
    memcpy(diagReplies[slot].srcHash, srcHash, RNS_ADDR_LEN);
}

static bool sendSafeDiagReply(const uint8_t* srcHash, const char* text) {
    if (!srcHash || !text || !*text) return false;
    PathEntry* peer = transport.lookupPath(srcHash);
    if (!peer || !peer->active || !peer->hasPubKey) return false;
    return transport.sendEncryptedMessage(text, peer);
}

static void emitPeerHashPrefix(const uint8_t* srcHash) {
    if (!srcHash) {
        Serial.print(F("0000000000000000"));
        return;
    }
    for (int i = 0; i < 8; i++) {
        if (srcHash[i] < 0x10) Serial.print('0');
        Serial.print(srcHash[i], HEX);
    }
}

static bool parsePeerControlAck(const char* text, char* outId, size_t outIdLen) {
    if (!text || strncmp(text, "RTA1:", 5) != 0 || !outId || outIdLen < 13) return false;
    const char* id = text + 5;
    size_t idLen = strlen(id);
    if (idLen != 12) return false;
    for (size_t i = 0; i < idLen; i++) {
        if (!isxdigit((unsigned char)id[i])) return false;
        outId[i] = (char)toupper((unsigned char)id[i]);
    }
    outId[idLen] = '\0';
    return true;
}

static bool parsePeerControlMessage(const char* text,
                                    char* outId,
                                    size_t outIdLen,
                                    char* outBody,
                                    size_t outBodyLen) {
    if (!text || strncmp(text, "RTM1:", 5) != 0 || !outId || outIdLen < 13 || !outBody || outBodyLen == 0) return false;
    const char* id = text + 5;
    const char* sep = strchr(id, ':');
    if (!sep) return false;
    size_t idLen = (size_t)(sep - id);
    if (idLen != 12) return false;
    for (size_t i = 0; i < idLen; i++) {
        if (!isxdigit((unsigned char)id[i])) return false;
        outId[i] = (char)toupper((unsigned char)id[i]);
    }
    outId[idLen] = '\0';

    const char* body = sep + 1;
    if (!*body) return false;
    size_t bodyLen = strlen(body);
    if (bodyLen >= outBodyLen) bodyLen = outBodyLen - 1;
    memcpy(outBody, body, bodyLen);
    outBody[bodyLen] = '\0';
    return true;
}

static void expirePeerMessageReceipts(uint32_t now) {
    for (int i = 0; i < (int)(sizeof(peerMessageReceipts) / sizeof(peerMessageReceipts[0])); i++) {
        if (!peerMessageReceipts[i].active) continue;
        if ((now - peerMessageReceipts[i].seenAt) > PEER_MESSAGE_RECEIPT_EXPIRY_MS) {
            peerMessageReceipts[i].active = false;
            peerMessageReceipts[i].seenAt = 0;
            peerMessageReceipts[i].msgId[0] = '\0';
            memset(peerMessageReceipts[i].srcHash, 0, sizeof(peerMessageReceipts[i].srcHash));
        }
    }
}

static bool wasPeerMessageSeen(const uint8_t* srcHash, const char* msgId) {
    if (!srcHash || !msgId || !*msgId) return false;
    uint32_t now = millis();
    expirePeerMessageReceipts(now);
    for (int i = 0; i < (int)(sizeof(peerMessageReceipts) / sizeof(peerMessageReceipts[0])); i++) {
        if (!peerMessageReceipts[i].active) continue;
        if (memcmp(peerMessageReceipts[i].srcHash, srcHash, RNS_ADDR_LEN) != 0) continue;
        if (strcmp(peerMessageReceipts[i].msgId, msgId) == 0) return true;
    }
    return false;
}

static void rememberPeerMessageSeen(const uint8_t* srcHash, const char* msgId) {
    if (!srcHash || !msgId || !*msgId) return;
    uint32_t now = millis();
    expirePeerMessageReceipts(now);

    PeerMessageReceiptState* slot = nullptr;
    PeerMessageReceiptState* oldest = nullptr;
    for (int i = 0; i < (int)(sizeof(peerMessageReceipts) / sizeof(peerMessageReceipts[0])); i++) {
        PeerMessageReceiptState& entry = peerMessageReceipts[i];
        if (entry.active && memcmp(entry.srcHash, srcHash, RNS_ADDR_LEN) == 0 && strcmp(entry.msgId, msgId) == 0) {
            entry.seenAt = now;
            return;
        }
        if (!entry.active && !slot) slot = &entry;
        if (!oldest || entry.seenAt < oldest->seenAt) oldest = &entry;
    }
    if (!slot) slot = oldest;
    if (!slot) return;
    slot->active = true;
    slot->seenAt = now;
    memcpy(slot->srcHash, srcHash, RNS_ADDR_LEN);
    strncpy(slot->msgId, msgId, sizeof(slot->msgId) - 1);
    slot->msgId[sizeof(slot->msgId) - 1] = '\0';
}

bool rnsHandleControlPeerMessage(const uint8_t* srcHash, const char* text) {
    if (!srcHash || !text || !*text) return false;

    char msgId[13] = {0};
    if (parsePeerControlAck(text, msgId, sizeof(msgId))) {
        Serial.print(F("[MSGACK] from "));
        emitPeerHashPrefix(srcHash);
        Serial.print(F("..: "));
        Serial.println(msgId);
        return true;
    }

    char body[200] = {0};
    if (!parsePeerControlMessage(text, msgId, sizeof(msgId), body, sizeof(body))) {
        return false;
    }

    char ackText[18] = {0};
    snprintf(ackText, sizeof(ackText), "RTA1:%s", msgId);
    if (sendSafeDiagReply(srcHash, ackText)) {
        Serial.print(F("[DIAG] Message ACK sent to "));
        emitPeerHashPrefix(srcHash);
        Serial.print(F("..: "));
        Serial.println(msgId);
    }

    if (wasPeerMessageSeen(srcHash, msgId)) {
        Serial.print(F("[DIAG] Duplicate peer message suppressed from "));
        emitPeerHashPrefix(srcHash);
        Serial.print(F("..: "));
        Serial.println(msgId);
        return true;
    }

    rememberPeerMessageSeen(srcHash, msgId);

    Serial.print(F("[PEERMSG] from "));
    emitPeerHashPrefix(srcHash);
    Serial.print(F("..: "));
    Serial.println(body);
    rnsHandleIncomingPeerMessage(srcHash, true, body, true);
    return true;
}

void rnsHandleIncomingPeerMessage(const uint8_t* srcHash, bool haveSrcHash, const char* text, bool parsed) {
    if (!haveSrcHash || !srcHash) return;

    if (parsed && text && *text) {
        char trimmed[24] = {0};
        normalizeDiagPrompt(text, trimmed, sizeof(trimmed));
        if (strcmp(trimmed, "R34DN0W") == 0) {
            clearPendingLedAlert();
            Serial.println(F("[RNS] Backdoor read-now received: cleared all pending LED alerts"));
            return;
        }
    }

    scheduleIncomingLedAlert(srcHash);

    if (!parsed || !text || !*text) return;

    char normalized[24] = {0};
    normalizeDiagPrompt(text, normalized, sizeof(normalized));
    DiagPromptKind prompt = detectDiagPrompt(normalized);
    if (prompt == DIAG_PROMPT_NONE) return;

    uint32_t now = millis();
    if (!diagReplyAllowed(srcHash, now)) return;

    char reply[40] = {0};
    switch (prompt) {
        case DIAG_PROMPT_PING:
            strncpy(reply, "RT!PONG7 ok", sizeof(reply) - 1);
            break;
        case DIAG_PROMPT_UPTIME:
            snprintf(reply, sizeof(reply), "RT!UP7 %s", diagUptimeBucket(now));
            break;
        case DIAG_PROMPT_MESH:
            snprintf(reply, sizeof(reply), "RT!MESH7 %s", diagMeshBucket(transport.countActivePaths()));
            break;
        default:
            return;
    }

    if (sendSafeDiagReply(srcHash, reply)) {
        markDiagReplySent(srcHash, now);
        Serial.print(F("[DIAG] Auto reply sent to "));
        for (int i = 0; i < 8; i++) {
            if (srcHash[i] < 0x10) Serial.print('0');
            Serial.print(srcHash[i], HEX);
        }
        Serial.print(F("..: "));
        Serial.println(reply);
    }
}

static void sanitizeMorseMessage(const char* src, char* dst, size_t dstLen) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t j = 0;
    bool lastSpace = false;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dstLen; i++) {
        char c = src[i];
        if (isspace((unsigned char)c)) {
            if (!lastSpace && j > 0) {
                dst[j++] = ' ';
                lastSpace = true;
            }
            continue;
        }

        c = (char)toupper((unsigned char)c);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            dst[j++] = c;
            lastSpace = false;
        }
    }

    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
    dst[dstLen - 1] = '\0';
}

static bool pushMorseStep(bool on, uint16_t durationMs) {
    if (durationMs == 0) return true;
    if (morseStepCount > 0 && morseStepOn[morseStepCount - 1] == on) {
        uint32_t merged = (uint32_t)morseStepMs[morseStepCount - 1] + durationMs;
        morseStepMs[morseStepCount - 1] = (merged > 60000) ? 60000 : (uint16_t)merged;
        return true;
    }
    if (morseStepCount >= 128) return false;
    morseStepOn[morseStepCount] = on;
    morseStepMs[morseStepCount] = durationMs;
    morseStepCount++;
    return true;
}

static bool buildMorseSteps(const char* message) {
    morseStepCount = 0;
    morseStepIndex = 0;
    const uint16_t unitMs = 90;

    if (!message || !*message) return false;

    size_t len = strlen(message);
    for (size_t i = 0; i < len; i++) {
        char ch = message[i];
        if (ch == ' ') {
            if (!pushMorseStep(false, unitMs * 7)) return false;
            continue;
        }

        const char* pattern = morseCharPattern(ch);
        if (!pattern) continue;

        for (size_t s = 0; pattern[s] != '\0'; s++) {
            if (!pushMorseStep(true, pattern[s] == '.' ? unitMs : (uint16_t)(unitMs * 3))) return false;
            if (pattern[s + 1] != '\0') {
                if (!pushMorseStep(false, unitMs)) return false;
            }
        }

        size_t next = i + 1;
        if (next < len && message[next] != ' ') {
            if (!pushMorseStep(false, unitMs * 3)) return false;
        }
    }

    return morseStepCount > 0;
}

static void startMorsePlayback(const char* message, uint8_t targetMask) {
    if (!buildMorseSteps(message) || targetMask == 0) return;
    morsePlaybackActive = true;
    morseStepIndex = 0;
    morsePlaybackMask = targetMask & availableLedMask();
    morseStepUntil = millis() + morseStepMs[0];
    refreshLedOutputs(millis(), true);
}

static void enqueueMorseMessage(const char* msg, uint8_t targetMask) {
    if (!msg || !*msg || targetMask == 0) return;
    if (morseQueueCount >= 4) return;

    strncpy(morseQueue[morseQueueTail], msg, sizeof(morseQueue[morseQueueTail]) - 1);
    morseQueue[morseQueueTail][sizeof(morseQueue[morseQueueTail]) - 1] = '\0';
    morseQueueMask[morseQueueTail] = targetMask & availableLedMask();
    morseQueueTail = (uint8_t)((morseQueueTail + 1) % 4);
    morseQueueCount++;
}

static const char* morseMessageForIncoming() {
    return (morseCfg.mode == MORSE_MODE_DEFAULT) ? morseCfg.defaultMessage : "MSG";
}

static const char* morseMessageForError() {
    return (morseCfg.mode == MORSE_MODE_DEFAULT) ? morseCfg.defaultMessage : "ERR";
}

static void clearMorsePlaybackQueue() {
    morsePlaybackActive = false;
    morsePlaybackMask = 0;
    morseStepCount = 0;
    morseStepIndex = 0;
    morseStepUntil = 0;
    morseQueueHead = 0;
    morseQueueTail = 0;
    morseQueueCount = 0;
    memset(morseQueueMask, 0, sizeof(morseQueueMask));
    refreshLedOutputs(millis(), true);
}

static void updateMorseBlinkPlayback(uint32_t now) {
    if (morseEnabledMask() == 0) {
        if (morsePlaybackActive || morseQueueCount > 0) {
            clearMorsePlaybackQueue();
        }
        return;
    }

    if (!morsePlaybackActive) {
        if (morseQueueCount == 0) return;
        char msg[17] = {0};
        strncpy(msg, morseQueue[morseQueueHead], sizeof(msg) - 1);
        uint8_t targetMask = morseQueueMask[morseQueueHead];
        morseQueueHead = (uint8_t)((morseQueueHead + 1) % 4);
        morseQueueCount--;
        startMorsePlayback(msg, targetMask);
        return;
    }

    if (now < morseStepUntil) return;
    morseStepIndex++;
    if (morseStepIndex >= morseStepCount) {
        morsePlaybackActive = false;
        morsePlaybackMask = 0;
        refreshLedOutputs(now, true);
        return;
    }

    morseStepUntil = now + morseStepMs[morseStepIndex];
    refreshLedOutputs(now, true);
}

static void blinkMorseTextOnMask(uint8_t mask, const char* text) {
    const uint32_t unitMs = 180;
    if (!text || !*text) {
        serviceDelay(unitMs * 8);
        return;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == ' ') {
            serviceDelay(unitMs * 7);
            continue;
        }

        const char* pattern = morseCharPattern(text[i]);
        if (!pattern) continue;

        for (size_t s = 0; pattern[s] != '\0'; s++) {
            writeLedMask(mask, true);
            serviceDelay(pattern[s] == '.' ? unitMs : (unitMs * 3));
            writeLedMask(mask, false);
            if (pattern[s + 1] != '\0') serviceDelay(unitMs);
        }

        if (text[i + 1] != '\0' && text[i + 1] != ' ') {
            serviceDelay(unitMs * 3);
        }
    }

    serviceDelay(unitMs * 8);
}

static void blinkMorseFaultDigit(uint8_t digit, uint8_t mask) {
    // Unit timing chosen for readability on status LEDs.
    const uint32_t unitMs = 180;
    const char* pattern = morseDigitPattern(digit);

    for (int i = 0; pattern[i] != '\0'; i++) {
        writeLedMask(mask, true);
        if (pattern[i] == '.') serviceDelay(unitMs);
        else                   serviceDelay(unitMs * 3);
        writeLedMask(mask, false);

        // Intra-symbol gap
        if (pattern[i + 1] != '\0') serviceDelay(unitMs);
    }

    // Inter-digit spacing (a little more pause for readability)
    serviceDelay(unitMs * 8);
}

static void errorBlinkFault(uint8_t faultCode) {
    // Fatal error loop. Console remains active for DFU/diagnosis.
    writeLedMask(availableLedMask(), false);
    while (true) {
        uint8_t mask = morseTargetMaskForErrors();
        if (mask == 0) mask = ledChannelAvailable(LED_CH_GREEN) ? LED_MASK_GREEN : availableLedMask();
        if (morseTargetMaskForErrors() != 0) {
            blinkMorseTextOnMask(mask, morseMessageForError());
        } else {
            blinkMorseFaultDigit(faultCode % 10, mask);
        }
    }
}

// ── Activity LED state machine ───────────────────────────
static uint32_t lastRxCount = 0;
static uint32_t lastTxCount = 0;
static uint32_t lastFwdCount = 0;
static uint32_t lastAnnounceCount = 0;

static void queuePulseMask(uint8_t mask, uint8_t toggles) {
    uint32_t now = millis();
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if (!(mask & ledChannelMask(channel)) || !ledChannelAvailable(channel) || toggles == 0) continue;
        uint16_t total = (uint16_t)pulseTogglesRemaining[channel] + toggles;
        if ((pulseTogglesRemaining[channel] == 0) && !pulseOutputState[channel]) {
            pulseOutputState[channel] = true;
            nextPulseToggleAt[channel] = now + 90UL;
            total = total > 0 ? (uint16_t)(total - 1) : 0;
        }
        if (total > 16) total = 16;
        pulseTogglesRemaining[channel] = (uint8_t)total;
    }
    refreshLedOutputs(now, true);
}

static void updateActivityLed(uint32_t now) {
    bool changed = false;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        if ((pulseTogglesRemaining[channel] == 0) || now < nextPulseToggleAt[channel]) continue;
        pulseOutputState[channel] = !pulseOutputState[channel];
        pulseTogglesRemaining[channel]--;
        nextPulseToggleAt[channel] = now + 90UL;
        if (pulseTogglesRemaining[channel] == 0 && pulseOutputState[channel]) {
            pulseOutputState[channel] = false;
        }
        changed = true;
    }
    if (changed) refreshLedOutputs(now, true);
}

static void emitPowerStatus(Stream& io) {
    io.print(F("[POWER] announce=")); io.print(announceIntervalMs / 1000UL);
    io.print(F(" discover=")); io.println(discoveryIntervalMs / 1000UL);
    io.print(F("  Announce interval:  ")); io.print(announceIntervalMs / 1000UL); io.println(F(" s"));
    io.print(F("  Discovery interval: ")); io.print(discoveryIntervalMs / 1000UL); io.println(F(" s"));
}

// ── Console command implementations needing global state ──
void RNSConsole::cmdSave() {
    if (!persistence) {
        io->println(F("Persistence not available."));
        return;
    }
    bool ok1 = persistence->saveIdentity(*identity);
    bool ok2 = persistence->saveConfig(*radio,
                                       (uint16_t)(announceIntervalMs / 1000UL),
                                       (uint16_t)(discoveryIntervalMs / 1000UL));
    bool ok3 = persistence->saveAnnounceName(transport->getAnnounceName());
    morseCfg.mode = ledCfg.channels[LED_CH_BLUE].morseMode;
    bool ok4 = persistence->saveMorseBlinkConfig(morseCfg.mode, morseCfg.defaultMessage);
    LedConfigBlob ledBlob = {};
    ledBlob.version = LED_CONFIG_VERSION;
    for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
        ledBlob.idleModes[channel] = ledCfg.channels[channel].idleMode;
        ledBlob.rxFlashEnabled[channel] = ledCfg.channels[channel].rxFlashEnabled ? 1 : 0;
        ledBlob.txFlashEnabled[channel] = ledCfg.channels[channel].txFlashEnabled ? 1 : 0;
        ledBlob.morseModes[channel] = ledCfg.channels[channel].morseMode;
    }
    ledBlob.alertMode = ledAlertCfg.mode;
    ledBlob.alertRepeatCount = ledAlertCfg.repeatCount;
    ledBlob.alertIntervalSec = ledAlertCfg.intervalSec;
    ledBlob.alertWatchCount = ledAlertCfg.watchCount;
    memcpy(ledBlob.alertWatchPrefixes, ledAlertCfg.watchPrefixes, sizeof(ledBlob.alertWatchPrefixes));
    memcpy(ledBlob.alertWatchMasks, ledAlertCfg.watchTargetMasks, sizeof(ledBlob.alertWatchMasks));
    bool ok5 = persistence->saveLedConfig(ledBlob);
    bool ok6 = persistence->saveSecurityConfig(ratHoleCfg.enabled, ratHoleCfg.wipeOnBoot);
    if (ok1 && ok2 && ok3 && ok4 && ok5 && ok6) {
        io->println(F("Configuration saved to flash."));
    } else {
        io->println(F("Save failed (partial or full)."));
    }
}

void RNSConsole::cmdPower(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || *args == '\0') {
        io->println(F("── Device Cadence ──"));
        io->println(F("Usage:"));
        io->println(F("  power announce <seconds>"));
        io->println(F("  power discover <seconds>"));
        io->println(F("Limits:"));
        io->print(F("  announce: ")); io->print(ANNOUNCE_INTERVAL_MIN_SEC); io->print(F("-")); io->print(ANNOUNCE_INTERVAL_MAX_SEC); io->println(F(" s"));
        io->print(F("  discover: ")); io->print(DISCOVERY_INTERVAL_MIN_SEC); io->print(F("-")); io->print(DISCOVERY_INTERVAL_MAX_SEC); io->println(F(" s"));
        io->println(F("Tip: run 'save' to persist."));
        emitPowerStatus(*io);
        return;
    }

    char field[16] = {0};
    size_t fieldLen = 0;
    while (args[fieldLen] && args[fieldLen] != ' ' && fieldLen < sizeof(field) - 1) {
        field[fieldLen] = args[fieldLen];
        fieldLen++;
    }
    field[fieldLen] = '\0';
    args += fieldLen;
    while (*args == ' ') args++;

    if (!*args) {
        emitPowerStatus(*io);
        return;
    }

    long value = strtol(args, nullptr, 10);
    if (strcmp(field, "announce") == 0) {
        if (value < ANNOUNCE_INTERVAL_MIN_SEC || value > ANNOUNCE_INTERVAL_MAX_SEC) {
            io->print(F("Announce interval must be between "));
            io->print(ANNOUNCE_INTERVAL_MIN_SEC);
            io->print(F(" and "));
            io->print(ANNOUNCE_INTERVAL_MAX_SEC);
            io->println(F(" seconds."));
            return;
        }
        announceIntervalMs = (uint32_t)value * 1000UL;
        nextAnnounceAt = millis() + announceIntervalMs;
        io->print(F("Announce interval -> "));
        io->print(value);
        io->println(F(" s"));
        emitPowerStatus(*io);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strcmp(field, "discover") == 0 || strcmp(field, "discovery") == 0) {
        if (value < DISCOVERY_INTERVAL_MIN_SEC || value > DISCOVERY_INTERVAL_MAX_SEC) {
            io->print(F("Discovery interval must be between "));
            io->print(DISCOVERY_INTERVAL_MIN_SEC);
            io->print(F(" and "));
            io->print(DISCOVERY_INTERVAL_MAX_SEC);
            io->println(F(" seconds."));
            return;
        }
        discoveryIntervalMs = (uint32_t)value * 1000UL;
        nextDiscoveryAt = millis() + discoveryIntervalMs;
        io->print(F("Discovery interval -> "));
        io->print(value);
        io->println(F(" s"));
        emitPowerStatus(*io);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    io->println(F("Unknown power subcommand. Use: announce, discover"));
}

void RNSConsole::cmdMorse(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || *args == '\0') {
        io->println(F("── Morse Blinker ──"));
        io->print(F("  mode:    ")); io->println(F("per-channel via led <color> morse <mode>"));
        io->print(F("  default: "));
        if (morseCfg.defaultMessage[0]) io->println(morseCfg.defaultMessage);
        else io->println(F("(none)"));
        io->println(F("Usage:"));
        io->println(F("  morse mode <off|errors|incoming|both|default>   (sets all available LEDs)"));
        io->println(F("  morse default <message|clear|none>"));
        io->println(F("  morse test [message]"));
        return;
    }

    if (strncmp(args, "mode", 4) == 0) {
        args += 4;
        while (*args == ' ') args++;
        uint8_t mode = morseCfg.mode;
        if (!parseMorseMode(args, mode)) {
            io->println(F("Invalid mode. Use: off, errors, incoming, both, default"));
            return;
        }
        morseCfg.mode = mode;
        for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
            if (!ledChannelAvailable(channel)) continue;
            ledCfg.channels[channel].morseMode = mode;
        }
        if (mode == MORSE_MODE_OFF) {
            clearMorsePlaybackQueue();
        }
        io->print(F("Morse mode for all available LEDs -> ")); io->println(morseModeName(mode));
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strncmp(args, "default", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;
        if (!*args) {
            io->print(F("Default message: "));
            if (morseCfg.defaultMessage[0]) io->println(morseCfg.defaultMessage);
            else io->println(F("(none)"));
            return;
        }

        if (strcmp(args, "clear") == 0 || strcmp(args, "none") == 0 || strcmp(args, "off") == 0) {
            morseCfg.defaultMessage[0] = '\0';
            io->println(F("Default Morse message cleared (none)."));
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        char cleaned[17] = {0};
        sanitizeMorseMessage(args, cleaned, sizeof(cleaned));
        strncpy(morseCfg.defaultMessage, cleaned, sizeof(morseCfg.defaultMessage) - 1);
        morseCfg.defaultMessage[sizeof(morseCfg.defaultMessage) - 1] = '\0';
        io->print(F("Default Morse message -> "));
        if (morseCfg.defaultMessage[0]) io->println(morseCfg.defaultMessage);
        else io->println(F("(none)"));
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strncmp(args, "test", 4) == 0) {
        args += 4;
        while (*args == ' ') args++;
        char cleaned[17] = {0};
        if (*args) sanitizeMorseMessage(args, cleaned, sizeof(cleaned));
        else strncpy(cleaned, morseCfg.defaultMessage, sizeof(cleaned) - 1);
        if (!cleaned[0]) {
            io->println(F("No Morse message set. Nothing queued."));
            return;
        }
        uint8_t targetMask = morseEnabledMask();
        if (targetMask == 0) targetMask = availableLedMask();
        enqueueMorseMessage(cleaned, targetMask);
        io->print(F("Queued Morse test: "));
        io->println(cleaned);
        return;
    }

    io->println(F("Unknown morse subcommand. Use: mode, default, test"));
}

void RNSConsole::cmdLed(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || *args == '\0') {
        io->println(F("── LED Settings ──"));
        io->print(F("  alert mode:    ")); io->println(ledAlertModeName(ledAlertCfg.mode));
        io->print(F("  alert count:   ")); io->println(ledAlertCfg.repeatCount);
        io->print(F("  alert interval:")); io->print(' '); io->print(ledAlertCfg.intervalSec); io->println(F(" s"));
        io->print(F("  alert watch:   "));
        if (ledAlertCfg.watchCount == 0) io->println(F("all senders"));
        else {
            for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
                if (i) io->print(F(", "));
                printHashPrefix(*io, ledAlertCfg.watchPrefixes[i]);
            }
            io->println();
        }
        io->println(F("Usage:"));
        io->println(F("  led <green|blue|red> idle <off|solid|heartbeat>"));
        io->println(F("  led <green|blue|red> rx <on|off>"));
        io->println(F("  led <green|blue|red> tx <on|off>"));
        io->println(F("  led <green|blue|red> morse <off|errors|incoming|both|default>"));
        io->println(F("  led green <heartbeat|solid|off>            (legacy alias)"));
        io->println(F("  led blue idle <off|solid|heartbeat>         (legacy alias)"));
        io->println(F("  led blue activity <on|off>                  (legacy alias for rx+tx)"));
        io->println(F("  led alert mode <once|count|until-clear>"));
        io->println(F("  led alert count <1-9>"));
        io->println(F("  led alert interval <seconds>"));
        io->println(F("  led alert watch <add|del|clear> [hash16] [green+blue+red|auto]"));
        io->println(F("  led alert pending clear [hash16|all]"));
        io->println(F("Tip: run 'save' to persist."));
        emitLedStatus(*io);
        return;
    }

    char colorBuf[12] = {0};
    size_t colorLen = 0;
    while (args[colorLen] && args[colorLen] != ' ' && colorLen < sizeof(colorBuf) - 1) {
        colorBuf[colorLen] = args[colorLen];
        colorLen++;
    }
    colorBuf[colorLen] = '\0';

    uint8_t channel = 0;
    if (parseLedChannel(colorBuf, channel)) {
        args += colorLen;
        while (*args == ' ') args++;

        if (!ledChannelAvailable(channel)) {
            io->print(F("LED channel unavailable on this board: "));
            io->println(ledChannelName(channel));
            return;
        }

        if (!*args) {
            emitLedStatus(*io);
            return;
        }

        char propBuf[16] = {0};
        size_t propLen = 0;
        while (args[propLen] && args[propLen] != ' ' && propLen < sizeof(propBuf) - 1) {
            propBuf[propLen] = args[propLen];
            propLen++;
        }
        propBuf[propLen] = '\0';
        args += propLen;
        while (*args == ' ') args++;

        if (channel == LED_CH_GREEN && !*args) {
            uint8_t mode = ledCfg.channels[channel].idleMode;
            if (!parseLedIdleMode(propBuf, mode)) {
                io->println(F("Invalid green LED mode. Use: heartbeat, solid, off"));
                return;
            }
            ledCfg.channels[channel].idleMode = mode;
            refreshLedOutputs(millis(), true);
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        if (channel == LED_CH_BLUE && strcmp(propBuf, "activity") == 0) {
            bool enabled = false;
            if (strcmp(args, "on") == 0 || strcmp(args, "enable") == 0 || strcmp(args, "enabled") == 0) enabled = true;
            else if (strcmp(args, "off") == 0 || strcmp(args, "disable") == 0 || strcmp(args, "disabled") == 0) enabled = false;
            else {
                io->println(F("Invalid blue activity mode. Use: on, off"));
                return;
            }
            ledCfg.channels[channel].rxFlashEnabled = enabled;
            ledCfg.channels[channel].txFlashEnabled = enabled;
            pulseTogglesRemaining[channel] = 0;
            pulseOutputState[channel] = false;
            refreshLedOutputs(millis(), true);
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        if ((strcmp(propBuf, "idle") == 0) || !*args) {
            const char* modeText = *args ? args : propBuf;
            uint8_t mode = ledCfg.channels[channel].idleMode;
            if (!parseLedIdleMode(modeText, mode)) {
                io->println(F("Invalid idle mode. Use: off, solid, heartbeat"));
                return;
            }
            ledCfg.channels[channel].idleMode = mode;
            refreshLedOutputs(millis(), true);
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        if (strcmp(propBuf, "rx") == 0 || strcmp(propBuf, "tx") == 0) {
            bool enabled = false;
            if (strcmp(args, "on") == 0 || strcmp(args, "enable") == 0 || strcmp(args, "enabled") == 0) enabled = true;
            else if (strcmp(args, "off") == 0 || strcmp(args, "disable") == 0 || strcmp(args, "disabled") == 0) enabled = false;
            else {
                io->println(F("Invalid LED flash mode. Use: on, off"));
                return;
            }
            if (strcmp(propBuf, "rx") == 0) ledCfg.channels[channel].rxFlashEnabled = enabled;
            else ledCfg.channels[channel].txFlashEnabled = enabled;
            pulseTogglesRemaining[channel] = 0;
            pulseOutputState[channel] = false;
            refreshLedOutputs(millis(), true);
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        if (strcmp(propBuf, "morse") == 0) {
            uint8_t mode = ledCfg.channels[channel].morseMode;
            if (!parseMorseMode(args, mode)) {
                io->println(F("Invalid morse mode. Use: off, errors, incoming, both, default"));
                return;
            }
            ledCfg.channels[channel].morseMode = mode;
            if (morseEnabledMask() == 0) clearMorsePlaybackQueue();
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }

        io->println(F("Unknown LED property. Use: idle, rx, tx, morse"));
        return;
    }

    if (strncmp(args, "alert", 5) == 0) {
        args += 5;
        while (*args == ' ') args++;
        if (strncmp(args, "mode", 4) == 0) {
            args += 4;
            while (*args == ' ') args++;
            uint8_t mode = ledAlertCfg.mode;
            if (!parseLedAlertMode(args, mode)) {
                io->println(F("Invalid alert mode. Use: once, count, until-clear"));
                return;
            }
            ledAlertCfg.mode = mode;
            if (mode == LED_ALERT_ONCE) clearPendingLedAlert();
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }
        if (strncmp(args, "count", 5) == 0) {
            args += 5;
            while (*args == ' ') args++;
            int value = atoi(args);
            if (value < 1 || value > 9) {
                io->println(F("Alert count must be between 1 and 9."));
                return;
            }
            ledAlertCfg.repeatCount = (uint8_t)value;
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }
        if (strncmp(args, "interval", 8) == 0) {
            args += 8;
            while (*args == ' ') args++;
            int value = atoi(args);
            if (value < 1 || value > 3600) {
                io->println(F("Alert interval must be between 1 and 3600 seconds."));
                return;
            }
            ledAlertCfg.intervalSec = (uint16_t)value;
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }
        if (strncmp(args, "watch", 5) == 0) {
            args += 5;
            while (*args == ' ') args++;
            if (strncmp(args, "clear", 5) == 0) {
                ledAlertCfg.watchCount = 0;
                memset(ledAlertCfg.watchPrefixes, 0, sizeof(ledAlertCfg.watchPrefixes));
                memset(ledAlertCfg.watchTargetMasks, 0, sizeof(ledAlertCfg.watchTargetMasks));
                emitLedStatus(*io);
                io->println(F("Tip: run 'save' to persist."));
                return;
            }
            bool adding = false;
            bool removing = false;
            if (strncmp(args, "add", 3) == 0) {
                adding = true;
                args += 3;
            } else if (strncmp(args, "del", 3) == 0 || strncmp(args, "remove", 6) == 0) {
                removing = true;
                args += (args[0] == 'd') ? 3 : 6;
            }
            while (*args == ' ') args++;
            uint8_t prefix[LED_ALERT_PREFIX_BYTES] = {0};
            if ((!adding && !removing) || !parseHashPrefix(args, prefix)) {
                io->println(F("Usage: led alert watch <add|del> <16-hex hash prefix> [green+blue+red|auto]"));
                return;
            }
            while (*args && *args != ' ') args++;
            while (*args == ' ') args++;

            uint8_t targetMask = 0;
            if (adding && *args) {
                if (!parseLedMask(args, targetMask)) {
                    io->println(F("Invalid watch target mask. Use green, blue, red, combos, or auto."));
                    return;
                }
            }

            bool ok = adding ? addAlertWatchPrefix(prefix, targetMask) : removeAlertWatchPrefix(prefix);
            io->println(ok ? (adding ? F("Watch added." ) : F("Watch removed."))
                           : (adding ? F("Watch list full or unchanged." ) : F("Watch not found.")));
            emitLedStatus(*io);
            io->println(F("Tip: run 'save' to persist."));
            return;
        }
        if (strncmp(args, "pending", 7) == 0) {
            args += 7;
            while (*args == ' ') args++;
            if (strncmp(args, "clear", 5) != 0) {
                io->println(F("Usage: led alert pending clear [hash16|all]"));
                return;
            }
            args += 5;
            while (*args == ' ') args++;
            if (!*args || strcmp(args, "all") == 0) {
                clearPendingLedAlert();
            } else {
                uint8_t prefix[LED_ALERT_PREFIX_BYTES] = {0};
                if (!parseHashPrefix(args, prefix)) {
                    io->println(F("Provide a 16-hex hash prefix or 'all'."));
                    return;
                }
                clearPendingLedAlert(prefix);
            }
            emitLedStatus(*io);
            return;
        }
        io->println(F("Unknown led alert subcommand."));
        return;
    }

    io->println(F("Unknown led subcommand. Use: <color> idle/rx/tx/morse or alert"));
}

void RNSConsole::cmdRathole(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || *args == '\0') {
        io->println(F("── RatHole Security ──"));
        io->print(F("  enabled:    ")); io->println(ratHoleCfg.enabled ? F("on") : F("off"));
        io->print(F("  boot reset: ")); io->println(ratHoleCfg.wipeOnBoot ? F("on") : F("off"));
        io->println(F("Usage:"));
        io->println(F("  rathole <on|off>"));
        io->println(F("  rathole boot-reset <on|off>"));
        io->println(F("Tip: run 'save' to persist. When enabled with boot-reset on, the node wipes identity and runtime config on every boot before generating a fresh hash."));
        emitRatHoleStatus(*io);
        return;
    }

    if (strcmp(args, "on") == 0 || strcmp(args, "enable") == 0 || strcmp(args, "enabled") == 0) {
        ratHoleCfg.enabled = true;
        emitRatHoleStatus(*io);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strcmp(args, "off") == 0 || strcmp(args, "disable") == 0 || strcmp(args, "disabled") == 0) {
        ratHoleCfg.enabled = false;
        emitRatHoleStatus(*io);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strncmp(args, "boot-reset", 10) == 0) {
        args += 10;
        while (*args == ' ') args++;
        if (strcmp(args, "on") == 0 || strcmp(args, "enable") == 0 || strcmp(args, "enabled") == 0) {
            ratHoleCfg.wipeOnBoot = true;
        } else if (strcmp(args, "off") == 0 || strcmp(args, "disable") == 0 || strcmp(args, "disabled") == 0) {
            ratHoleCfg.wipeOnBoot = false;
        } else {
            io->println(F("Usage: rathole boot-reset <on|off>"));
            return;
        }
        emitRatHoleStatus(*io);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    io->println(F("Unknown rathole subcommand. Use: on, off, boot-reset"));
}

void RNSConsole::cmdFactoryReset() {
    io->println(F("WARNING: This erases identity and config from flash."));
    io->println(F("The node will generate a new identity on next boot."));
    io->println(F("Type 'yes' to confirm, or anything else to cancel."));
    io->print(F("Confirm> "));
    io->flush();

    // Simple blocking read for confirmation (acceptable for rare operation)
    char buf[8] = {0};
    uint8_t pos = 0;
    uint32_t start = millis();
    while (millis() - start < 15000) {  // 15 s timeout
        wdtFeed();
        if (io->available()) {
            char c = io->read();
            io->print(c);
            if (c == '\r' || c == '\n') break;
            if (pos < sizeof(buf) - 1) buf[pos++] = c;
        }
    }
    io->println();

    if (strcmp(buf, "yes") == 0) {
        if (persistence) persistence->factoryReset();
        io->println(F("Factory reset complete. Rebooting..."));
        io->flush();
        delay(500);
        NVIC_SystemReset();
    } else {
        io->println(F("Cancelled."));
    }
}

// ── Board battery sense (boards with PIN_BATT_ADC) ───────────
#if defined(PIN_BATT_ADC)
// Returns pack voltage in mV (0 if unreadable). The divider is gated:
// ADC_Ctrl HIGH connects it, so no drain while idle.
static uint32_t readBatteryMillivolts() {
    pinMode(PIN_BATT_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_BATT_ADC_CTRL, HIGH);
    delay(3);
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    uint32_t acc = 0;
    for (uint8_t i = 0; i < 8; i++) acc += (uint32_t)analogRead(PIN_BATT_ADC);
    digitalWrite(PIN_BATT_ADC_CTRL, LOW);
    float raw = (float)acc / 8.0f;
    float mv = raw * (3000.0f / 4096.0f) * BATT_DIVIDER_MULT;
    return (uint32_t)mv;
}

// Single-cell LiPo open-circuit curve, coarse.
static int8_t batteryPercentFromMv(uint32_t mv) {
    static const struct { uint16_t mv; uint8_t pct; } tbl[] = {
        {4200,100},{4100,90},{4000,78},{3900,62},{3800,45},{3700,25},{3600,10},{3500,4},{3400,1},{3300,0}
    };
    if (mv >= tbl[0].mv) return 100;
    for (uint8_t i = 1; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (mv >= tbl[i].mv) {
            uint32_t span = tbl[i-1].mv - tbl[i].mv;
            uint32_t frac = mv - tbl[i].mv;
            return (int8_t)(tbl[i].pct + (tbl[i-1].pct - tbl[i].pct) * frac / span);
        }
    }
    return 0;
}

static bool usbPowerPresent() {
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}
#endif

// ── Runtime user button (short press → display wake / next page) ──
#if PIN_USER_BUTTON >= 0
static void pollUserButton(uint32_t now) {
    static bool     wasPressed = false;
    static uint32_t pressedAt  = 0;
    static uint32_t lastEdge   = 0;
    const int pressedLevel = BUTTON_ACTIVE_LOW ? LOW : HIGH;
    bool pressed = (digitalRead(PIN_USER_BUTTON) == pressedLevel);
    if (pressed != wasPressed && (now - lastEdge) < 30) return;   // debounce
    if (pressed && !wasPressed) { pressedAt = now; lastEdge = now; }
    if (!pressed && wasPressed) {
        lastEdge = now;
        uint32_t held = now - pressedAt;
        if (held < 800) {
#if HAS_DISPLAY
            display.buttonPress(now);
#endif
        }
        // ≥ 800 ms: reserved (no action yet)
    }
    wasPressed = pressed;
}
#endif

#if HAS_DISPLAY
static void persistDisplayConfig() {
    DisplayConfigBlob blob = {};
    blob.enabled    = display.enabled ? 1 : 0;
    blob.timeoutSec = display.timeoutSec;
    persist.saveDisplayConfig(blob);
}
#endif

// ── Discovery (RMAP) announce ─────────────────────────────
static void persistDiscoveryConfig() {
    DiscoveryConfigBlob b = {};
    b.enabled     = discovery.cfg.enabled ? 1 : 0;
    b.haveHeight  = discovery.cfg.haveHeight ? 1 : 0;
    b.latUdeg     = discovery.cfg.latUdeg;
    b.lonUdeg     = discovery.cfg.lonUdeg;
    b.heightM     = discovery.cfg.heightM;
    b.intervalMin = discovery.cfg.intervalMin;
    b.stampValid  = discovery.stampReady ? 1 : 0;
    memcpy(b.stamp, discovery.stamp, sizeof(b.stamp));
    memcpy(b.stampInfohash, discovery.stampInfohash, sizeof(b.stampInfohash));
    persist.saveDiscoveryConfig(b);
}

static void printHexBytes(Stream* io, const uint8_t* p, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) { if (p[i] < 0x10) io->print('0'); io->print(p[i], HEX); }
}

void RNSConsole::cmdDiscovery(const char* args) {
    uint32_t now = millis();
    if (!args || !*args) {
        io->println(F("── Interface discovery (rnstransport.discovery.interface) ──"));
        io->print(F("  Enabled:   ")); io->println(discovery.cfg.enabled ? F("yes") : F("no"));
        io->print(F("  Dest hash: ")); printHexBytes(io, discovery.destinationHash(), RNS_ADDR_LEN); io->println();
        io->print(F("  Location:  "));
        if (discovery.hasLocation()) {
            io->print(discovery.cfg.latUdeg / 1e6, 6); io->print(F(", ")); io->print(discovery.cfg.lonUdeg / 1e6, 6);
            if (discovery.cfg.haveHeight) { io->print(F(", ")); io->print(discovery.cfg.heightM); io->print(F(" m")); }
            io->println();
        } else io->println(F("not set (announce carries nil)"));
        io->print(F("  Interval:  ")); io->print(discovery.cfg.intervalMin); io->println(F(" min"));
        io->print(F("  Packed:    ")); io->print(discovery.packedLength()); io->println(F(" bytes"));
        io->print(F("  Stamp:     "));
        if (discovery.stampReady) { io->print(F("ready  ")); printHexBytes(io, discovery.stamp, 8); io->println(F("...")); }
        else if (discovery.searching) { io->print(F("searching, ")); io->print(discovery.attempts); io->print(F(" attempts, "));
                                        io->print((now - discovery.searchStartedAt) / 1000); io->println(F(" s")); }
        else io->println(F("none (enable to compute)"));
        io->print(F("  Announces: ")); io->print(discovery.announcesSent);
        if (discovery.cfg.enabled && discovery.stampReady) {
            io->print(F("  next in ")); io->print(discovery.nextAnnounceAt > now ? (discovery.nextAnnounceAt - now) / 1000 : 0); io->print(F(" s"));
        }
        io->println();
        io->println(F("  Usage: discovery on|off | interval <min> | now | dump"));
        return;
    }
    if (strcmp(args, "on") == 0)  { discovery.cfg.enabled = true;  discovery.refresh(now); persistDiscoveryConfig();
                                    io->println(discovery.stampReady ? F("Discovery enabled (stamp cached)") : F("Discovery enabled; computing stamp (~65k hashes)")); return; }
    if (strcmp(args, "off") == 0) { discovery.cfg.enabled = false; discovery.searching = false; persistDiscoveryConfig(); io->println(F("Discovery disabled")); return; }
    if (strncmp(args, "interval", 8) == 0) {
        long m = atol(args + 8);
        if (m < DISCOVERY_INTERVAL_MIN_MIN || m > 1440) { io->println(F("Interval must be 5-1440 minutes")); return; }
        discovery.cfg.intervalMin = (uint16_t)m; persistDiscoveryConfig();
        io->print(F("Discovery interval -> ")); io->print(m); io->println(F(" min")); return;
    }
    if (strcmp(args, "now") == 0) {
        if (!discovery.cfg.enabled) { io->println(F("Discovery is off")); return; }
        if (!discovery.stampReady)  { io->println(F("Stamp not ready yet")); return; }
        io->println(discovery.announce() ? F("Discovery announce sent") : F("Discovery announce failed"));
        return;
    }
    if (strcmp(args, "dump") == 0) {
        // Hex for tools/validate_discovery.py: app_data then dest hash then identity pubkey hash
        static uint8_t buf[1 + DISCOVERY_PACKED_MAX + DISCOVERY_STAMP_LEN];
        uint16_t n = discovery.buildAppData(buf, sizeof(buf));
        io->print(F("PACKED ")); printHexBytes(io, discovery.packedBytes(), discovery.packedLength()); io->println();
        io->print(F("INFOHASH ")); printHexBytes(io, discovery.currentInfohash(), 32); io->println();
        if (n) { io->print(F("APPDATA ")); printHexBytes(io, buf, n); io->println(); }
        else io->println(F("APPDATA (stamp not ready)"));
        io->print(F("DEST ")); printHexBytes(io, discovery.destinationHash(), RNS_ADDR_LEN); io->println();
        return;
    }
    io->println(F("Usage: discovery | discovery on|off | interval <min> | now | dump"));
}

void RNSConsole::cmdLocation(const char* args) {
    uint32_t now = millis();
    if (!args || !*args) {
        io->print(F("Location: "));
        if (discovery.hasLocation()) {
            io->print(discovery.cfg.latUdeg / 1e6, 6); io->print(F(" ")); io->print(discovery.cfg.lonUdeg / 1e6, 6);
            if (discovery.cfg.haveHeight) { io->print(F(" ")); io->print(discovery.cfg.heightM); io->print(F(" m")); }
            io->println();
        } else io->println(F("not set"));
        io->println(F("Usage: location <lat> <lon> [height_m] | location clear"));
        return;
    }
    if (strcmp(args, "clear") == 0) {
        discovery.cfg.latUdeg = 0; discovery.cfg.lonUdeg = 0; discovery.cfg.haveHeight = false;
        discovery.refresh(now); persistDiscoveryConfig(); io->println(F("Location cleared")); return;
    }
    char buf[96]; strncpy(buf, args, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char* save = nullptr;
    char* a = strtok_r(buf, " ,", &save);
    char* b = a ? strtok_r(nullptr, " ,", &save) : nullptr;
    char* c = b ? strtok_r(nullptr, " ,", &save) : nullptr;
    if (!a || !b) { io->println(F("Usage: location <lat> <lon> [height_m]")); return; }
    double lat = atof(a), lon = atof(b);
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) { io->println(F("Latitude -90..90, longitude -180..180")); return; }
    discovery.cfg.latUdeg = (int32_t)(lat * 1e6 + (lat >= 0 ? 0.5 : -0.5));
    discovery.cfg.lonUdeg = (int32_t)(lon * 1e6 + (lon >= 0 ? 0.5 : -0.5));
    if (c) { long h = atol(c); if (h < -4000 || h > 30000) { io->println(F("Height -4000..30000 m")); return; }
             discovery.cfg.heightM = (int16_t)h; discovery.cfg.haveHeight = true; }
    else discovery.cfg.haveHeight = false;
    discovery.refresh(now); persistDiscoveryConfig();
    io->print(F("Location -> ")); io->print(lat, 6); io->print(F(", ")); io->print(lon, 6);
    if (c) { io->print(F(", ")); io->print(discovery.cfg.heightM); io->print(F(" m")); }
    io->println(discovery.cfg.enabled ? F("  (descriptor changed; stamp recomputes)") : F(""));
}

#if HAS_GNSS
static void persistGnssConfig() {
    GnssConfigBlob b = {}; b.mode = (uint8_t)gnss.mode; b.intervalMin = gnss.intervalMin; b.dwellSec = gnss.dwellSec;
    persist.saveGnssConfig(b);
}
#endif

void RNSConsole::cmdGps(const char* args) {
#if HAS_GNSS
    uint32_t now = millis();
    if (!args || !*args) {
        io->println(F("── GNSS (UC6580) ──"));
        io->print(F("  Mode:      ")); io->print(gnss.mode == RNSGnss::GNSS_ON ? F("on") : F("off"));
        if (gnss.mode == RNSGnss::GNSS_ON) {
            if (gnss.intervalMin) { io->print(F(", every ")); io->print(gnss.intervalMin); io->print(F(" min, dwell ")); io->print(gnss.dwellSec); io->print(F(" s")); }
            else io->print(F(", continuous"));
        }
        io->println();
        if (gnss.mode == RNSGnss::GNSS_ON && gnss.intervalMin) {
            io->print(F("  Cycles:    ")); io->print(gnss.cycles); io->print(F(" run, ")); io->print(gnss.cyclesFixed); io->print(F(" fixed"));
            if (!gnss.powered) { io->print(F(", next wake in ")); io->print(gnss.nextWakeAt > now ? (gnss.nextWakeAt - now) / 1000 : 0); io->print(F(" s")); }
            io->println();
        }
        io->print(F("  Powered:   ")); io->print(gnss.powered ? F("yes, ") : F("no"));
        if (gnss.powered) { io->print((now - gnss.poweredAt) / 1000); io->println(F(" s")); } else io->println();
        io->print(F("  Sentences: ")); io->print(gnss.sentences); io->print(F(" ok, ")); io->print(gnss.badChecksum); io->print(F(" bad"));
        if (gnss.lastSentenceAt) { io->print(F(", last ")); io->print((now - gnss.lastSentenceAt) / 1000); io->print(F(" s ago")); }
        io->println();
        io->print(F("  Fix:       "));
        if (gnss.hasFix) {
            io->print(F("yes  ")); io->print(gnss.latUdeg / 1e6, 6); io->print(F(", ")); io->print(gnss.lonUdeg / 1e6, 6);
            if (gnss.hasAlt) { io->print(F(", ")); io->print(gnss.altM, 0); io->print(F(" m")); }
            io->println();
        } else io->println(gnss.powered ? F("no (searching)") : F("no"));
        io->print(F("  Sats/qual: ")); io->print(gnss.sats); io->print(F(" / ")); io->print(gnss.fixQuality);
        io->print(F("  HDOP ")); io->println(gnss.hdop, 1);
        io->print(F("  RMC/GGA:   ")); io->print(gnss.rmcSeen); io->print(F(" RMC (")); io->print(gnss.rmcValid);
        io->print(F(" valid), ")); io->print(gnss.ggaSeen); io->println(F(" GGA"));
        if (gnss.lastRmc[0]) { io->print(F("  Last RMC:  ")); io->println(gnss.lastRmc); }
        io->print(F("  Clock:     "));
        uint32_t u = gnss.unixNow(now);
        if (u) { io->print(F("unix ")); io->println(u); } else io->println(F("not set"));
        io->println(F("  Usage: gps on|off | gps interval <min> (0 = continuous) | gps dwell <sec> | gps raw [sec]"));
        return;
    }
    if (strcmp(args, "on") == 0)  { gnss.setMode(RNSGnss::GNSS_ON, now);  persistGnssConfig(); io->println(F("GNSS on (rail powered, 115200 NMEA)")); return; }
    if (strcmp(args, "off") == 0) { gnss.setMode(RNSGnss::GNSS_OFF, now); persistGnssConfig(); io->println(F("GNSS off")); return; }
    if (strncmp(args, "interval", 8) == 0) {
        long m = atol(args + 8);
        if (m < 0 || m > 1440) { io->println(F("Interval 0 (continuous) to 1440 minutes")); return; }
        gnss.setInterval((uint16_t)m, now); persistGnssConfig();
        if (m == 0) io->println(F("GNSS continuous")); else { io->print(F("GNSS every ")); io->print(m); io->println(F(" min")); }
        return;
    }
    if (strncmp(args, "raw", 3) == 0) {
        long sec = atol(args + 3); if (sec <= 0) sec = 5; if (sec > 60) sec = 60;
        gnss.rawTo = io; gnss.rawUntil = now + (uint32_t)sec * 1000UL;
        io->print(F("Echoing NMEA for ")); io->print(sec); io->println(F(" s..."));
        return;
    }
    if (strncmp(args, "dwell", 5) == 0) {
        long sec = atol(args + 5);
        if (sec < 30 || sec > 3600) { io->println(F("Dwell 30-3600 seconds")); return; }
        gnss.dwellSec = (uint16_t)sec; persistGnssConfig();
        io->print(F("GNSS dwell -> ")); io->print(sec); io->println(F(" s")); return;
    }
    io->println(F("Usage: gps | gps on|off | gps interval <min> | gps dwell <sec>"));
#else
    (void)args;
    io->println(F("No GNSS on this board"));
#endif
}

// ── Extra `status` lines: discovery, GNSS, display, power ──────────
static void statusExtraLines(Stream* io) {
    uint32_t now = millis();
    io->print(F("  Discovery:  "));
    if (!discovery.cfg.enabled) io->println(F("off"));
    else {
        io->print(discovery.stampReady ? F("on, stamp ready") : (discovery.searching ? F("on, computing stamp") : F("on, no stamp")));
        io->print(F(", ")); io->print(discovery.announcesSent); io->print(F(" sent"));
        if (discovery.stampReady) { io->print(F(", next in ")); io->print(discovery.nextAnnounceAt > now ? (discovery.nextAnnounceAt - now) / 60000UL : 0); io->print(F(" min")); }
        io->println();
    }
    io->print(F("  Location:   "));
    if (discovery.hasLocation()) {
        io->print(discovery.cfg.latUdeg / 1e6, 5); io->print(F(", ")); io->print(discovery.cfg.lonUdeg / 1e6, 5);
        if (discovery.cfg.haveHeight) { io->print(F(", ")); io->print(discovery.cfg.heightM); io->print(F(" m")); }
        io->println();
    } else io->println(F("not set"));
#if HAS_GNSS
    io->print(F("  GNSS:       "));
    if (gnss.mode != RNSGnss::GNSS_ON) io->println(F("off"));
    else {
        io->print(gnss.powered ? F("powered") : F("sleeping"));
        if (gnss.intervalMin) { io->print(F(" (every ")); io->print(gnss.intervalMin); io->print(F(" min)")); }
        io->print(F(", ")); io->print(gnss.sats); io->print(F(" sats, "));
        io->println(gnss.hasFix ? F("fix") : F("no fix"));
    }
    {
        uint32_t u = gnss.unixNow(now);
        io->print(F("  Clock:      "));
        if (u) { io->print(F("unix ")); io->print(u); io->println(F(" (GNSS)")); } else io->println(F("uptime only"));
    }
#endif
#if HAS_DISPLAY
    io->print(F("  Display:    "));
    if (!display.enabled) io->println(F("off"));
    else { io->print(display.isAwake() ? F("awake") : F("blanked")); io->print(F(", page ")); io->print(display.currentPage() + 1);
           io->print(F(", timeout ")); if (display.timeoutSec) { io->print(display.timeoutSec); io->println(F(" s")); } else io->println(F("never")); }
#endif
#if defined(PIN_BATT_ADC)
    {
        uint32_t mv = readBatteryMillivolts();
        io->print(F("  Power:      "));
        if (usbPowerPresent()) io->print(F("USB"));
        if (mv > 2500) { if (usbPowerPresent()) io->print(F(", ")); io->print(F("battery ")); io->print(mv); io->print(F(" mV (")); io->print(batteryPercentFromMv(mv)); io->print(F("%)")); }
        io->println();
    }
#endif
}

void RNSConsole::cmdDisplay(const char* args) {
#if HAS_DISPLAY
    uint32_t now = millis();
    if (!args || !*args) {
        io->println(F("── Display ──"));
        io->print(F("  Panel:   ")); io->println(display.present ? F("ST7735 160x80 OK") : F("not initialised"));
        io->print(F("  Enabled: ")); io->println(display.enabled ? F("yes") : F("no"));
        io->print(F("  Awake:   ")); io->println(display.isAwake() ? F("yes") : F("no (blanked)"));
        io->print(F("  Timeout: "));
        if (display.timeoutSec == 0) io->println(F("never")); else { io->print(display.timeoutSec); io->println(F(" s")); }
        io->print(F("  Page:    ")); io->println(display.currentPage() + 1);
        io->println(F("  Usage: display timeout <0-3600> | on | off | page [1-3]"));
        return;
    }
    if (strncmp(args, "timeout", 7) == 0) {
        const char* v = args + 7; while (*v == ' ') v++;
        if (!*v) { io->println(F("Usage: display timeout <seconds> (0 = never)")); return; }
        long sec = atol(v);
        if (sec < 0 || sec > DISPLAY_TIMEOUT_MAX_SEC) { io->println(F("Timeout must be 0-3600 s")); return; }
        display.setTimeout((uint16_t)sec, now);
        persistDisplayConfig();
        io->print(F("Display timeout -> ")); if (sec == 0) io->println(F("never")); else { io->print(sec); io->println(F(" s")); }
        return;
    }
    if (strcmp(args, "on") == 0)  { display.setEnabled(true, now);  persistDisplayConfig(); io->println(F("Display enabled"));  return; }
    if (strcmp(args, "off") == 0) { display.setEnabled(false, now); persistDisplayConfig(); io->println(F("Display disabled")); return; }
    if (strncmp(args, "page", 4) == 0) {
        const char* v = args + 4; while (*v == ' ') v++;
        if (*v) display.setPage((uint8_t)(atoi(v) - 1), now); else display.buttonPress(now);
        io->print(F("Page ")); io->println(display.currentPage() + 1);
        return;
    }
    io->println(F("Usage: display | display timeout <s> | display on|off | display page [n]"));
#else
    (void)args;
    io->println(F("No display on this board"));
#endif
}

// ── identity regen (console) ──────────────────────────────
static void identityRegenerate(Stream* io) {
    io->println(F("Generating a new hardware-seeded identity..."));
    nodeIdentity.generate();
    if (!persist.saveIdentity(nodeIdentity)) { io->println(F("FAILED to save identity")); return; }
    transport.initDestHashCache();          // recomputes the lxmf.delivery hashes from the new key
    discovery.begin(&transport, &nodeIdentity, &radio);   // new dest/transport hashes, re-stamp
    io->print(F("New identity hash: "));
    for (int i = 0; i < RNS_ADDR_LEN; i++) { if (nodeIdentity.identityHash[i] < 0x10) io->print('0'); io->print(nodeIdentity.identityHash[i], HEX); }
    io->println();
    io->println(F("All of this node's destination hashes have changed. Rebooting to apply cleanly..."));
    io->flush(); delay(300);
    NVIC_SystemReset();
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
    // LEDs
    if (PIN_LED_GREEN >= 0) pinMode(PIN_LED_GREEN, OUTPUT);
    if (PIN_LED_BLUE >= 0) pinMode(PIN_LED_BLUE, OUTPUT);
    if (PIN_LED_RED >= 0) pinMode(PIN_LED_RED, OUTPUT);
    if (PIN_LED_GREEN >= 0) digitalWrite(PIN_LED_GREEN, ledLevel(true));
    if (PIN_LED_BLUE >= 0) digitalWrite(PIN_LED_BLUE, ledLevel(false));
    if (PIN_LED_RED >= 0) digitalWrite(PIN_LED_RED, ledLevel(false));

#if defined(BOARD_HELTEC_T096)
    // ── Heltec T096: park the peripheral rails until their owners run ──
    // Vext (TFT + backlight) is raised by RNSDisplay only while awake;
    // the GNSS rail (PMOS, HIGH = off) is raised by RNSGnss on `gps on`.
    // Asserted here so a warm reboot from another firmware cannot leave
    // them on. FEM pins are handled in RNSRadio.
    RNSVext::begin();                       // Vext low until the display or GNSS claims it
    pinMode(PIN_GNSS_CTRL, OUTPUT);
    digitalWrite(PIN_GNSS_CTRL, HIGH);
#endif

    // ── User button → UF2 bootloader (boards with PIN_USER_BUTTON) ──
    // Hold the button through power-on for BUTTON_DFU_HOLD_MS and the
    // node reboots into DFU via the same GPREGRET path as the console
    // `dfu` command. The bootloader is never modified. Checked before
    // USB/serial so a wedged main loop can still be recovered.
#if PIN_USER_BUTTON >= 0
    pinMode(PIN_USER_BUTTON, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
    delay(5);
    {
        const int pressed = BUTTON_ACTIVE_LOW ? LOW : HIGH;
        uint32_t t0 = millis();
        bool held = true;
        while (millis() - t0 < BUTTON_DFU_HOLD_MS) {
            if (digitalRead(PIN_USER_BUTTON) != pressed) { held = false; break; }
            delay(10);
        }
        if (held) {
            // Signal with all available LEDs, then go.
            writeLedMask(availableLedMask(), true);
            delay(200);
            rnsRebootToDfu();
        }
    }
#endif

    // USB serial
    Serial.begin(115200);
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 3000) delay(10);
    }

    Serial.println(F("[RNS] ── RatTunnel Transport Node ──"));
    Serial.println(F("[RNS] " BOARD_DISPLAY_NAME "  |  " FW_DISPLAY_VERSION));
    printResetReason();

    // ── Persistence ───────────────────────────────────────
    Serial.print(F("[RNS] LittleFS: "));
    if (persist.begin()) {
        Serial.println(F("OK"));
    } else {
        Serial.println(F("UNAVAILABLE (defaults will be used)"));
    }

    bool ratHoleLoaded = false;
    if (persist.loadSecurityConfig(ratHoleCfg.enabled, ratHoleCfg.wipeOnBoot)) {
        ratHoleLoaded = true;
        emitRatHoleStatus(Serial);
    }

    bool wipedOnBoot = false;
    if (ratHoleCfg.enabled && ratHoleCfg.wipeOnBoot && bootLooksLikePowerOn) {
        Serial.println(F("[RATHOLE] boot scrub active; clearing persisted runtime state"));
        persist.wipeRuntimeStatePreserveSecurity();
        wipedOnBoot = true;
    } else if (ratHoleCfg.enabled && ratHoleCfg.wipeOnBoot) {
        Serial.println(F("[RATHOLE] boot scrub armed, but this reset was not a power-on boot; preserving identity"));
    } else if (!ratHoleLoaded) {
        emitRatHoleStatus(Serial);
    }

    // ── Entropy, then identity ────────────────────────────
    RNSEntropy::begin();     // hardware-seeded Crypto RNG + libc rand() (see RNSEntropy.h)
    Serial.print(F("[RNS] Identity: "));
    if (!wipedOnBoot && persist.loadIdentity(nodeIdentity)) {
        Serial.println(F("loaded from flash"));
    } else {
        if (persist.lastIdentityWasLegacy) {
            Serial.println(F("REGENERATING — the stored identity predates hardware RNG seeding"));
            Serial.println(F("[RNS]   (builds before 2026-10-16 derived the same key pair on every board;"));
            Serial.println(F("[RNS]    those keys are public. This node's hashes change now.)"));
        }
        Serial.print(F("[RNS] Identity: generating with hardware entropy... "));
        nodeIdentity.generate();
        persist.saveIdentity(nodeIdentity);
        Serial.println(F("done (saved)"));
    }

    Serial.print(F("[RNS] Hash: "));
    for (int i = 0; i < RNS_ADDR_LEN; i++) {
        if (nodeIdentity.identityHash[i] < 0x10) Serial.print('0');
        Serial.print(nodeIdentity.identityHash[i], HEX);
    }
    Serial.println();

    // ── Transport engine ──────────────────────────────────
    transport.begin(&nodeIdentity, &radio);
    transport.initDestHashCache();
    Serial.println(F("[RNS] Transport engine: OK"));

    char loadedName[RNS_ANNOUNCE_NAME_MAX + 1] = {0};
    if (persist.loadAnnounceName(loadedName, sizeof(loadedName))) {
        if (transport.setAnnounceName(loadedName)) {
            Serial.print(F("[RNS] Loaded broadcast name: "));
            Serial.println(transport.getAnnounceName());
        }
    }

    uint8_t loadedMorseMode = morseCfg.mode;
    char loadedMorseMessage[17] = {0};
    if (persist.loadMorseBlinkConfig(loadedMorseMode, loadedMorseMessage, sizeof(loadedMorseMessage))) {
        morseCfg.mode = loadedMorseMode;
        sanitizeMorseMessage(loadedMorseMessage, morseCfg.defaultMessage, sizeof(morseCfg.defaultMessage));
        Serial.print(F("[RNS] Morse mode: "));
        Serial.print(morseModeName(morseCfg.mode));
        Serial.print(F(" default='"));
        Serial.print(morseCfg.defaultMessage);
        Serial.println(F("'"));
        ledCfg.channels[LED_CH_BLUE].morseMode = morseCfg.mode;
    }

    LedConfigBlob loadedLedCfg = {};
    if (persist.loadLedConfig(loadedLedCfg)) {
        for (uint8_t channel = 0; channel < LED_CH_COUNT; channel++) {
            ledCfg.channels[channel].idleMode = loadedLedCfg.idleModes[channel];
            ledCfg.channels[channel].rxFlashEnabled = loadedLedCfg.rxFlashEnabled[channel] != 0;
            ledCfg.channels[channel].txFlashEnabled = loadedLedCfg.txFlashEnabled[channel] != 0;
            ledCfg.channels[channel].morseMode = loadedLedCfg.morseModes[channel];
        }
        ledAlertCfg.mode = loadedLedCfg.alertMode;
        ledAlertCfg.repeatCount = loadedLedCfg.alertRepeatCount;
        ledAlertCfg.intervalSec = loadedLedCfg.alertIntervalSec;
        ledAlertCfg.watchCount = loadedLedCfg.alertWatchCount > LED_ALERT_WATCH_MAX ? LED_ALERT_WATCH_MAX : loadedLedCfg.alertWatchCount;
        memcpy(ledAlertCfg.watchPrefixes, loadedLedCfg.alertWatchPrefixes, sizeof(loadedLedCfg.alertWatchPrefixes));
        memcpy(ledAlertCfg.watchTargetMasks, loadedLedCfg.alertWatchMasks, sizeof(loadedLedCfg.alertWatchMasks));
        Serial.println(F("[RNS] Loaded per-channel LED config"));
        emitLedAlertStatus(Serial);
    }

    // ── Radio ─────────────────────────────────────────────
    Serial.print(F("[RNS] Radio: "));
    radioOk = radio.begin(&transport);
    if (radioOk) {
        Serial.println(F("OK"));
        refreshLedOutputs(millis(), true);

        // Apply saved config if available
        uint16_t loadedAnnounceIntervalSec = (uint16_t)(ANNOUNCE_INTERVAL_MS / 1000UL);
        uint16_t loadedDiscoveryIntervalSec = (uint16_t)(DISCOVERY_INTERVAL_MS / 1000UL);
        if (persist.loadConfig(radio, &loadedAnnounceIntervalSec, &loadedDiscoveryIntervalSec)) {
            Serial.println(F("[RNS] Loaded saved radio config"));
            announceIntervalMs = (uint32_t)loadedAnnounceIntervalSec * 1000UL;
            discoveryIntervalMs = (uint32_t)loadedDiscoveryIntervalSec * 1000UL;
            emitPowerStatus(Serial);
        }
    } else {
        Serial.println(F("FAILED — check " BOARD_MODULE_NAME " connection"));
        Serial.print(F("[DIAG] Radio init error code: "));
        Serial.println(radio.lastInitState);
        Serial.print(F("[DIAG] Init attempts: "));
        Serial.println(radio.initAttempts);
        Serial.println(F("[DIAG] Possible causes:"));
        Serial.println(F("  -2  = chip not found (SPI wiring / power)"));
        Serial.println(F("  -3  = chip version mismatch"));
        Serial.println(F("  -6  = packet too long"));
        Serial.println(F("  -707 = TCXO config failed"));
        Serial.println(F("  -16 = SPI comm error"));
        Serial.println(F("[RNS] Console is still active; type 'dfu' to flash."));
    }

    // ── Console ───────────────────────────────────────────
    console.begin(&Serial, &transport, &radio, &nodeIdentity, &persist);    console.keepAlive = wdtFeed;
    console.statusExtra = statusExtraLines;
    console.identityRegen = identityRegenerate;

#if HAS_DISPLAY
    {
        DisplayConfigBlob dcfg = {};
        if (persist.loadDisplayConfig(dcfg)) {
            display.enabled    = dcfg.enabled != 0;
            display.timeoutSec = dcfg.timeoutSec;
        }
        display.begin(&transport, &radio, FW_VERSION_STRING);
        Serial.print(F("[DISP] ST7735 160x80 ")); Serial.print(display.present ? F("initialised") : F("skipped"));
        Serial.print(F(", timeout ")); Serial.print(display.timeoutSec); Serial.println(F(" s"));
    }
#endif

    // ── Interface discovery (RMAP) ──
    {
        DiscoveryConfigBlob dc = {};
        if (persist.loadDiscoveryConfig(dc)) {
            discovery.cfg.enabled     = dc.enabled != 0;
            discovery.cfg.haveHeight  = dc.haveHeight != 0;
            discovery.cfg.latUdeg     = dc.latUdeg;
            discovery.cfg.lonUdeg     = dc.lonUdeg;
            discovery.cfg.heightM     = dc.heightM;
            discovery.cfg.intervalMin = dc.intervalMin ? dc.intervalMin : DISCOVERY_INTERVAL_DEFAULT_MIN;
            if (dc.stampValid) {
                memcpy(discovery.stamp, dc.stamp, sizeof(discovery.stamp));
                memcpy(discovery.stampInfohash, dc.stampInfohash, sizeof(discovery.stampInfohash));
                discovery.stampReady = true;
            }
        }
        discovery.begin(&transport, &nodeIdentity, &radio);
        Serial.print(F("[DISC] discovery ")); Serial.print(discovery.cfg.enabled ? F("on") : F("off"));
        Serial.print(F(", stamp ")); Serial.println(discovery.stampReady ? F("cached") : (discovery.searching ? F("computing") : F("none")));
    }

#if HAS_GNSS
    {
        GnssConfigBlob gc = {};
        RNSGnss::Mode m = RNSGnss::GNSS_OFF;
        if (persist.loadGnssConfig(gc)) {
            if (gc.mode == 1) m = RNSGnss::GNSS_ON;
            gnss.intervalMin = gc.intervalMin;
            gnss.dwellSec = (gc.dwellSec >= 30) ? gc.dwellSec : 300;
        }
        gnss.begin(m, millis());
        transport.wallClockSeconds = gnssWallClock;
        Serial.print(F("[GNSS] UC6580 ")); Serial.println(m == RNSGnss::GNSS_ON ? F("on") : F("off"));
    }
#endif
    // ── Watchdog ──────────────────────────────────────────
    wdtInit();

    // ── Summary ───────────────────────────────────────────
    if (radioOk) {
        Serial.print(F("[RNS] Active: "));
        Serial.print(radio.curFreqMHz, 3); Serial.print(F(" MHz, SF"));
        Serial.print(radio.curSF); Serial.print(F(", BW"));
        Serial.print(radio.curBwKHz, 0); Serial.print(F(" kHz, "));
        Serial.print(radio.curTxDbm); Serial.print(F(" dBm SX1262 drive (cap "));
        Serial.print(LORA_TX_DBM_MAX_SAFE); Serial.print(F(", announce "));
        Serial.print(LORA_TX_DBM_ANNOUNCE_SAFE); Serial.println(F(") + board PA"));
    }

    // If radio failed, enter error mode (console stays alive)
    if (!radioOk) {
        errorBlinkFault(FAULT_RADIO_INIT);  // does not return; keeps WDT fed
    }

    const auto& startupStats = transport.getStats();
    lastRxCount       = startupStats.rxPackets;
    lastTxCount       = startupStats.txPackets;
    lastFwdCount      = startupStats.fwdPackets;
    lastAnnounceCount = startupStats.announces;

    nextAnnounceAt = millis() + ANNOUNCE_STARTUP_DELAY_MS;
    nextDiscoveryAt = millis() + DISCOVERY_STARTUP_DELAY_MS;
    bootTime = millis();
    lastHeartbeatToggle = bootTime;
    heartbeatPhaseOn = true;
    refreshLedOutputs(bootTime, true);
}

// ── Main loop ─────────────────────────────────────────────
static uint32_t lastTransportLoop = 0;

static inline void scheduleNextAnnounce(uint32_t now) {
    uint32_t interval = announceIntervalMs;
    nextAnnounceAt = now + interval;
}

static inline void scheduleNextDiscovery(uint32_t now) {
    uint32_t interval = discoveryIntervalMs;
    nextDiscoveryAt = now + interval;
}

void loop() {
    wdtFeed();
    RNSEntropy::service(millis());   // keep the Crypto RNG stirred with hardware entropy

    // Poll radio for received packets (interrupt-driven flag check)
    radio.poll();

    // Run transport engine at ~5 ms intervals
    uint32_t now = millis();
    if (now - lastTransportLoop >= TRANSPORT_LOOP_MS) {
        transport.loop();
        lastTransportLoop = now;
    }

    // Poll console for serial commands
    console.poll();

    // Activity indication from transport counters:
    //   RX event  -> one blue pulse
    //   TX/FWD/announce event -> two blue pulses
    const auto& s = transport.getStats();
    if (s.rxPackets > lastRxCount) {
        uint32_t delta = s.rxPackets - lastRxCount;
        if (delta > 3) delta = 3;
        queuePulseMask(rxFlashMask(), (uint8_t)(delta * 2));
        lastRxCount = s.rxPackets;
    }

    if (s.txPackets > lastTxCount || s.fwdPackets > lastFwdCount || s.announces > lastAnnounceCount) {
        queuePulseMask(txFlashMask(), 4);
        lastTxCount       = s.txPackets;
        lastFwdCount      = s.fwdPackets;
        lastAnnounceCount = s.announces;
    }

    servicePendingLedAlerts(now);
    updateMorseBlinkPlayback(now);
    updateActivityLed(now);

    // Periodic self-announce so peers can discover this node
    if (radioOk && now >= nextAnnounceAt) {
        Serial.println(F("[DIAG] Periodic announce starting..."));
        if (transport.sendLocalAnnounce()) {
            Serial.println(F("[RNS] Announce sent"));
        } else {
            Serial.println(F("[RNS] Announce send failed"));
            uint8_t errorMask = morseTargetMaskForErrors();
            if (errorMask) enqueueMorseMessage(morseMessageForError(), errorMask);
        }
        scheduleNextAnnounce(now);
    }

    if (radioOk && now >= nextDiscoveryAt) {
        if (transport.sendDiscoverySweep()) {
            Serial.println(F("[RNS] Discovery sweep sent"));
        } else {
            Serial.println(F("[RNS] Discovery sweep failed"));
        }
        scheduleNextDiscovery(now);
    }

    refreshLedOutputs(now);

#if PIN_USER_BUTTON >= 0
    pollUserButton(now);
#endif
#if HAS_GNSS
    gnss.loop(now);
#if HAS_DISPLAY
    // The GNSS module's power-up inrush shares the Vext LDO with the
    // panel; re-run the panel init once the module has settled.
    {
        static uint32_t reinitAt = 0;
        if (gnss.justPoweredOn) { gnss.justPoweredOn = false; reinitAt = now + 300; }
        if (reinitAt && now >= reinitAt) { reinitAt = 0; display.reinitIfAwake(); }
    }
#endif
    {
        // Push a fresh fix into the map position: on first fix, or after
        // moving GNSS_LOCATION_SYNC_MIN_M, at most every holdoff period.
        static uint32_t lastLocSyncAt = 0;
        if (gnss.hasFix && (lastLocSyncAt == 0 || (now - lastLocSyncAt) >= GNSS_LOCATION_SYNC_HOLDOFF_MS)) {
            double dLat = (discovery.cfg.latUdeg - gnss.latUdeg) / 1e6 * 111320.0;
            double dLon = (discovery.cfg.lonUdeg - gnss.lonUdeg) / 1e6 * 111320.0 * cos(gnss.latUdeg / 1e6 * 3.14159265 / 180.0);
            double moved = sqrt(dLat * dLat + dLon * dLon);
            if (!discovery.hasLocation() || moved >= GNSS_LOCATION_SYNC_MIN_M ||
                (gnss.hasAlt && (!discovery.cfg.haveHeight || fabs((double)discovery.cfg.heightM - gnss.altM) > 30.0))) {
                discovery.cfg.latUdeg = gnss.latUdeg;
                discovery.cfg.lonUdeg = gnss.lonUdeg;
                if (gnss.hasAlt) { discovery.cfg.heightM = (int16_t)(gnss.altM + 0.5f); discovery.cfg.haveHeight = true; }
                discovery.refresh(now);
                persistDiscoveryConfig();
                Serial.print(F("[GNSS] location -> ")); Serial.print(gnss.latUdeg / 1e6, 6); Serial.print(F(", "));
                Serial.println(gnss.lonUdeg / 1e6, 6);
            }
            lastLocSyncAt = now;
        }
#if HAS_DISPLAY
        display.setGnss((uint8_t)gnss.mode, gnss.hasFix, gnss.sats, gnss.latUdeg, gnss.lonUdeg, gnss.unixNow(now));
#endif
    }
#endif
    if (discovery.loop(now)) {
        Serial.print(F("[DISC] stamp found after ")); Serial.print(discovery.attempts);
        Serial.print(F(" attempts, ")); Serial.print((now - discovery.searchStartedAt) / 1000); Serial.println(F(" s"));
        persistDiscoveryConfig();
    }
#if HAS_DISPLAY
    {
#if defined(PIN_BATT_ADC)
        static uint32_t nextBattAt = 0;
        if (now >= nextBattAt) {
            nextBattAt = now + 10000UL;
            uint32_t mv = readBatteryMillivolts();
            display.setBattery(mv > 2500 ? batteryPercentFromMv(mv) : -1, usbPowerPresent());
        }
#endif
        display.loop(now);
    }
#endif
}
