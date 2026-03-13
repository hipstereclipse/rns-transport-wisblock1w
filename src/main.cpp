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

enum MorseBlinkMode : uint8_t {
    MORSE_MODE_OFF      = 0,
    MORSE_MODE_ERRORS   = 1,
    MORSE_MODE_INCOMING = 2,
    MORSE_MODE_BOTH     = 3,
    MORSE_MODE_DEFAULT  = 4,
};

struct MorseBlinkConfigState {
    uint8_t mode = MORSE_MODE_ERRORS;
    char defaultMessage[17] = "SOS";
};

static MorseBlinkConfigState morseCfg;

static bool morseStepOn[128] = {0};
static uint16_t morseStepMs[128] = {0};
static uint8_t morseStepCount = 0;
static uint8_t morseStepIndex = 0;
static uint32_t morseStepUntil = 0;
static bool morsePlaybackActive = false;

static char morseQueue[4][17] = {{0}};
static uint8_t morseQueueHead = 0;
static uint8_t morseQueueTail = 0;
static uint8_t morseQueueCount = 0;

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

static bool morseModeUsesIncoming() {
    return morseCfg.mode == MORSE_MODE_INCOMING || morseCfg.mode == MORSE_MODE_BOTH || morseCfg.mode == MORSE_MODE_DEFAULT;
}

static bool morseModeUsesErrors() {
    return morseCfg.mode == MORSE_MODE_ERRORS || morseCfg.mode == MORSE_MODE_BOTH || morseCfg.mode == MORSE_MODE_DEFAULT;
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
    if (dst[0] == '\0') strncpy(dst, "SOS", dstLen - 1);
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

static void startMorsePlayback(const char* message) {
    if (!buildMorseSteps(message)) return;
    morsePlaybackActive = true;
    morseStepIndex = 0;
    digitalWrite(PIN_LED_BLUE, morseStepOn[0] ? HIGH : LOW);
    morseStepUntil = millis() + morseStepMs[0];
}

static void enqueueMorseMessage(const char* msg) {
    if (!msg || !*msg) return;
    if (morseQueueCount >= 4) return;

    strncpy(morseQueue[morseQueueTail], msg, sizeof(morseQueue[morseQueueTail]) - 1);
    morseQueue[morseQueueTail][sizeof(morseQueue[morseQueueTail]) - 1] = '\0';
    morseQueueTail = (uint8_t)((morseQueueTail + 1) % 4);
    morseQueueCount++;
}

static const char* morseMessageForIncoming() {
    return (morseCfg.mode == MORSE_MODE_DEFAULT) ? morseCfg.defaultMessage : "MSG";
}

static const char* morseMessageForError() {
    return (morseCfg.mode == MORSE_MODE_DEFAULT) ? morseCfg.defaultMessage : "ERR";
}

static void updateMorseBlinkPlayback(uint32_t now) {
    if (!morsePlaybackActive) {
        if (morseQueueCount == 0) return;
        char msg[17] = {0};
        strncpy(msg, morseQueue[morseQueueHead], sizeof(msg) - 1);
        morseQueueHead = (uint8_t)((morseQueueHead + 1) % 4);
        morseQueueCount--;
        startMorsePlayback(msg);
        return;
    }

    if (now < morseStepUntil) return;
    morseStepIndex++;
    if (morseStepIndex >= morseStepCount) {
        morsePlaybackActive = false;
        digitalWrite(PIN_LED_BLUE, LOW);
        return;
    }

    digitalWrite(PIN_LED_BLUE, morseStepOn[morseStepIndex] ? HIGH : LOW);
    morseStepUntil = now + morseStepMs[morseStepIndex];
}

static void blinkMorseTextOnGreen(const char* text) {
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
            digitalWrite(PIN_LED_GREEN, HIGH);
            serviceDelay(pattern[s] == '.' ? unitMs : (unitMs * 3));
            digitalWrite(PIN_LED_GREEN, LOW);
            if (pattern[s + 1] != '\0') serviceDelay(unitMs);
        }

        if (text[i + 1] != '\0' && text[i + 1] != ' ') {
            serviceDelay(unitMs * 3);
        }
    }

    serviceDelay(unitMs * 8);
}

static void blinkMorseFaultDigit(uint8_t digit) {
    // Unit timing chosen for readability on status LEDs.
    const uint32_t unitMs = 180;
    const char* pattern = morseDigitPattern(digit);

    for (int i = 0; pattern[i] != '\0'; i++) {
        digitalWrite(PIN_LED_GREEN, HIGH);
        if (pattern[i] == '.') serviceDelay(unitMs);
        else                   serviceDelay(unitMs * 3);
        digitalWrite(PIN_LED_GREEN, LOW);

        // Intra-symbol gap
        if (pattern[i + 1] != '\0') serviceDelay(unitMs);
    }

    // Inter-digit spacing (a little more pause for readability)
    serviceDelay(unitMs * 8);
}

static void errorBlinkFault(uint8_t faultCode) {
    // Fatal error loop. Console remains active for DFU/diagnosis.
    digitalWrite(PIN_LED_BLUE, LOW);
    while (true) {
        if (morseModeUsesErrors()) {
            blinkMorseTextOnGreen(morseMessageForError());
        } else {
            blinkMorseFaultDigit(faultCode % 10);
        }
    }
}

// ── Activity LED state machine (blue LED) ───────────────
static uint32_t lastRxCount = 0;
static uint32_t lastTxCount = 0;
static uint32_t lastFwdCount = 0;
static uint32_t lastAnnounceCount = 0;
static uint8_t  activityTogglesRemaining = 0;
static uint32_t nextActivityToggleAt = 0;

static void queueBluePulseToggles(uint8_t toggles) {
    uint16_t total = (uint16_t)activityTogglesRemaining + toggles;
    if (total > 16) total = 16;  // prevent long LED backlog under burst traffic
    activityTogglesRemaining = (uint8_t)total;
}

static void updateActivityLed(uint32_t now) {
    if (morsePlaybackActive) {
        return;
    }

    if (activityTogglesRemaining == 0) {
        digitalWrite(PIN_LED_BLUE, LOW);
        return;
    }

    if (now >= nextActivityToggleAt) {
        digitalWrite(PIN_LED_BLUE, !digitalRead(PIN_LED_BLUE));
        activityTogglesRemaining--;
        nextActivityToggleAt = now + 90;

        if (activityTogglesRemaining == 0) {
            digitalWrite(PIN_LED_BLUE, LOW);
        }
    }
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
    bool ok4 = persistence->saveMorseBlinkConfig(morseCfg.mode, morseCfg.defaultMessage);
    if (ok1 && ok2 && ok3 && ok4) {
        io->println(F("Configuration saved to flash."));
    } else {
        io->println(F("Save failed (partial or full)."));
    }
}

void RNSConsole::cmdMorse(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || *args == '\0') {
        io->println(F("── Morse Blinker ──"));
        io->print(F("  mode:    ")); io->println(morseModeName(morseCfg.mode));
        io->print(F("  default: ")); io->println(morseCfg.defaultMessage);
        io->println(F("Usage:"));
        io->println(F("  morse mode <off|errors|incoming|both|default>"));
        io->println(F("  morse default <message>"));
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
        io->print(F("Morse mode -> ")); io->println(morseModeName(morseCfg.mode));
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strncmp(args, "default", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;
        if (!*args) {
            io->print(F("Default message: "));
            io->println(morseCfg.defaultMessage);
            return;
        }

        char cleaned[17] = {0};
        sanitizeMorseMessage(args, cleaned, sizeof(cleaned));
        strncpy(morseCfg.defaultMessage, cleaned, sizeof(morseCfg.defaultMessage) - 1);
        morseCfg.defaultMessage[sizeof(morseCfg.defaultMessage) - 1] = '\0';
        io->print(F("Default Morse message -> "));
        io->println(morseCfg.defaultMessage);
        io->println(F("Tip: run 'save' to persist."));
        return;
    }

    if (strncmp(args, "test", 4) == 0) {
        args += 4;
        while (*args == ' ') args++;
        char cleaned[17] = {0};
        if (*args) sanitizeMorseMessage(args, cleaned, sizeof(cleaned));
        else strncpy(cleaned, morseCfg.defaultMessage, sizeof(cleaned) - 1);
        enqueueMorseMessage(cleaned);
        io->print(F("Queued Morse test: "));
        io->println(cleaned);
        return;
    }

    io->println(F("Unknown morse subcommand. Use: mode, default, test"));
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
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE,  OUTPUT);
    digitalWrite(PIN_LED_GREEN, HIGH);   // power indicator
    digitalWrite(PIN_LED_BLUE,  LOW);

    // USB serial
    Serial.begin(115200);
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 3000) delay(10);
    }

    Serial.println(F("[RNS] ── RatTunnel Transport Node ──"));
    Serial.println(F("[RNS] WisBlock 1W  |  " FW_DISPLAY_VERSION));

    // ── Persistence ───────────────────────────────────────
    Serial.print(F("[RNS] LittleFS: "));
    if (persist.begin()) {
        Serial.println(F("OK"));
    } else {
        Serial.println(F("UNAVAILABLE (defaults will be used)"));
    }

    // ── Identity ──────────────────────────────────────────
    Serial.print(F("[RNS] Identity: "));
    if (persist.loadIdentity(nodeIdentity)) {
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
    }

    // ── Radio ─────────────────────────────────────────────
    Serial.print(F("[RNS] Radio: "));
    radioOk = radio.begin(&transport);
    if (radioOk) {
        Serial.println(F("OK"));
        digitalWrite(PIN_LED_BLUE, LOW);

        // Apply saved config if available
        if (persist.loadConfig(radio)) {
            Serial.println(F("[RNS] Loaded saved radio config"));
        }
    } else {
        Serial.println(F("FAILED — check RAK13302 connection"));
        Serial.println(F("[RNS] Console is still active; type 'dfu' to flash."));
    }

    // ── Console ───────────────────────────────────────────
    console.begin(&Serial, &transport, &radio, &nodeIdentity, &persist);

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
}

// ── Main loop ─────────────────────────────────────────────
static uint32_t lastTransportLoop = 0;
static uint32_t lastLedToggle     = 0;

static inline void scheduleNextAnnounce(uint32_t now) {
    nextAnnounceAt = now + ANNOUNCE_INTERVAL_MS;
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
        if (morseModeUsesIncoming()) {
            (void)delta;
            enqueueMorseMessage(morseMessageForIncoming());
        } else {
            if (delta > 3) delta = 3;
            queueBluePulseToggles((uint8_t)(delta * 2));
        }
        lastRxCount = s.rxPackets;
    }

    if (s.txPackets > lastTxCount || s.fwdPackets > lastFwdCount || s.announces > lastAnnounceCount) {
        queueBluePulseToggles(4);
        lastTxCount       = s.txPackets;
        lastFwdCount      = s.fwdPackets;
        lastAnnounceCount = s.announces;
    }

    updateMorseBlinkPlayback(now);
    updateActivityLed(now);

    // Periodic self-announce so peers can discover this node
    if (radioOk && now >= nextAnnounceAt) {
        if (transport.sendLocalAnnounce()) {
            Serial.println(F("[RNS] Announce sent"));
        } else {
            Serial.println(F("[RNS] Announce send failed"));
            if (morseModeUsesErrors()) enqueueMorseMessage(morseMessageForError());
        }
        scheduleNextAnnounce(now);
    }

    // Heartbeat: toggle green LED every 2 seconds
    if (now - lastLedToggle >= 2000) {
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
        lastLedToggle = now;
    }
}
