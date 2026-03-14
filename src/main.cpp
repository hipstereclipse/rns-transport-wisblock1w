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
 *   6. Radio (SX1262 + RAK13302)
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
#include "RNSPersistence.h"
#include <nrf_wdt.h>
#include <ctype.h>

// ── Global objects (static allocation) ────────────────────
static RNSIdentity    nodeIdentity;
static RNSRadio       radio;
static RNSTransport   transport;
static RNSConsole     console;
static RNSPersistence persist;

static bool radioOk = false;
static uint32_t nextAnnounceAt = 0;
static uint32_t nextDiscoveryAt = 0;
static uint32_t bootTime       = 0;
static bool bootLooksLikePowerOn = true;

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
};

struct PendingLedAlert {
    uint8_t prefix[LED_ALERT_PREFIX_BYTES] = {0};
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

static bool senderMatchesAlertWatch(const uint8_t* srcHash) {
    if (!srcHash) return false;
    if (ledAlertCfg.watchCount == 0) return true;
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (prefixEquals(srcHash, ledAlertCfg.watchPrefixes[i])) return true;
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
    freeSlot->repeatsRemaining = 0;
    freeSlot->nextAt = 0;
    return freeSlot;
}

static bool addAlertWatchPrefix(const uint8_t prefix[LED_ALERT_PREFIX_BYTES]) {
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (prefixEquals(prefix, ledAlertCfg.watchPrefixes[i])) return true;
    }
    if (ledAlertCfg.watchCount >= LED_ALERT_WATCH_MAX) return false;
    memcpy(ledAlertCfg.watchPrefixes[ledAlertCfg.watchCount], prefix, LED_ALERT_PREFIX_BYTES);
    ledAlertCfg.watchCount++;
    return true;
}

static bool removeAlertWatchPrefix(const uint8_t prefix[LED_ALERT_PREFIX_BYTES]) {
    for (uint8_t i = 0; i < ledAlertCfg.watchCount; i++) {
        if (!prefixEquals(prefix, ledAlertCfg.watchPrefixes[i])) continue;
        for (uint8_t j = i + 1; j < ledAlertCfg.watchCount; j++) {
            memcpy(ledAlertCfg.watchPrefixes[j - 1], ledAlertCfg.watchPrefixes[j], LED_ALERT_PREFIX_BYTES);
        }
        memset(ledAlertCfg.watchPrefixes[ledAlertCfg.watchCount - 1], 0, LED_ALERT_PREFIX_BYTES);
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
    }
    io.println();
}

static void writeLedChannel(uint8_t channel, bool on) {
    if (!ledChannelAvailable(channel)) return;
    if (ledOutputState[channel] == on) return;
    digitalWrite(ledChannelPin(channel), on ? HIGH : LOW);
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
    uint8_t targetMask = morseTargetMaskForIncoming();
    if (!srcHash || targetMask == 0) return;
    if (!senderMatchesAlertWatch(srcHash)) return;

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

    if (ledAlertCfg.mode == LED_ALERT_REPEAT_COUNT) {
        slot->repeatsRemaining = ledAlertCfg.repeatCount > 0 ? (uint8_t)(ledAlertCfg.repeatCount - 1) : 0;
    } else {
        slot->repeatsRemaining = 0xFF;
    }
    slot->nextAt = millis() + ((uint32_t)ledAlertCfg.intervalSec * 1000UL);
}

static void servicePendingLedAlerts(uint32_t now) {
    if (ledAlertCfg.mode == LED_ALERT_ONCE || ledAlertCfg.intervalSec == 0) return;
    uint8_t targetMask = morseTargetMaskForIncoming();
    if (targetMask == 0) return;
    for (uint8_t i = 0; i < LED_ALERT_WATCH_MAX; i++) {
        PendingLedAlert& slot = pendingLedAlerts[i];
        if (!slot.active || now < slot.nextAt) continue;
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

void rnsHandleIncomingPeerMessage(const uint8_t* srcHash, bool haveSrcHash, const char* text, bool parsed) {
    if (!haveSrcHash || !srcHash) return;
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

// ── Console command implementations needing global state ──
void RNSConsole::cmdSave() {
    if (!persistence) {
        io->println(F("Persistence not available."));
        return;
    }
    bool ok1 = persistence->saveIdentity(*identity);
    bool ok2 = persistence->saveConfig(*radio);
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
    bool ok5 = persistence->saveLedConfig(ledBlob);
    bool ok6 = persistence->saveSecurityConfig(ratHoleCfg.enabled, ratHoleCfg.wipeOnBoot);
    if (ok1 && ok2 && ok3 && ok4 && ok5 && ok6) {
        io->println(F("Configuration saved to flash."));
    } else {
        io->println(F("Save failed (partial or full)."));
    }
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
        io->println(F("  led alert watch <add|del|clear> [hash16]"));
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
                io->println(F("Usage: led alert watch <add|del> <16-hex hash prefix>"));
                return;
            }
            bool ok = adding ? addAlertWatchPrefix(prefix) : removeAlertWatchPrefix(prefix);
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

// ── Setup ─────────────────────────────────────────────────
void setup() {
    // LEDs
    if (PIN_LED_GREEN >= 0) pinMode(PIN_LED_GREEN, OUTPUT);
    if (PIN_LED_BLUE >= 0) pinMode(PIN_LED_BLUE, OUTPUT);
    if (PIN_LED_RED >= 0) pinMode(PIN_LED_RED, OUTPUT);
    if (PIN_LED_GREEN >= 0) digitalWrite(PIN_LED_GREEN, HIGH);
    if (PIN_LED_BLUE >= 0) digitalWrite(PIN_LED_BLUE, LOW);
    if (PIN_LED_RED >= 0) digitalWrite(PIN_LED_RED, LOW);

    // USB serial
    Serial.begin(115200);
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 3000) delay(10);
    }

    Serial.println(F("[RNS] ── RatTunnel Transport Node ──"));
    Serial.println(F("[RNS] WisBlock 1W  |  " FW_DISPLAY_VERSION));
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

    // ── Identity ──────────────────────────────────────────
    Serial.print(F("[RNS] Identity: "));
    if (!wipedOnBoot && persist.loadIdentity(nodeIdentity)) {
        Serial.println(F("loaded from flash"));
    } else {
        Serial.print(F("generating... "));
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
        if (persist.loadConfig(radio)) {
            Serial.println(F("[RNS] Loaded saved radio config"));
        }
    } else {
        Serial.println(F("FAILED — check RAK13302 connection"));
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
    // ── Watchdog ──────────────────────────────────────────
    wdtInit();

    // ── Summary ───────────────────────────────────────────
    if (radioOk) {
        Serial.print(F("[RNS] Active: "));
        Serial.print(radio.curFreqMHz, 1); Serial.print(F(" MHz, SF"));
        Serial.print(radio.curSF); Serial.print(F(", BW"));
        Serial.print(radio.curBwKHz, 0); Serial.print(F(" kHz, "));
        Serial.print(radio.curTxDbm); Serial.println(F(" dBm (+8 dB PA)"));
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
    uint32_t interval = ((now - bootTime) < ANNOUNCE_FAST_PERIOD_MS)
                        ? ANNOUNCE_FAST_INTERVAL_MS
                        : ANNOUNCE_INTERVAL_MS;
    nextAnnounceAt = now + interval;
}

static inline void scheduleNextDiscovery(uint32_t now) {
    uint32_t interval = ((now - bootTime) < ANNOUNCE_FAST_PERIOD_MS)
                        ? DISCOVERY_FAST_INTERVAL_MS
                        : DISCOVERY_INTERVAL_MS;
    nextDiscoveryAt = now + interval;
}

void loop() {
    wdtFeed();

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
}
