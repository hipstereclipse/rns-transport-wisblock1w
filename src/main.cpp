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

// ── Global objects (static allocation) ────────────────────
static RNSIdentity    nodeIdentity;
static RNSRadio       radio;
static RNSTransport   transport;
static RNSConsole     console;
static RNSPersistence persist;

static bool radioOk = false;
static uint32_t nextAnnounceAt = 0;

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
//   Green blink (2 s)        = running normally
//   Blue on                  = radio OK
//   Fast green blink (200 ms)= radio init failure
//   Alternating green/blue   = fatal error

static void errorBlinkRadio() {
    // Fast green blink — indicates radio failure but device is alive.
    // Console remains available for DFU or diagnosis.
    while (true) {
        wdtFeed();
        console.poll();   // keep console alive in error state
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
        delay(200);
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
    if (ok1 && ok2) {
        io->println(F("Configuration saved to flash."));
    } else {
        io->println(F("Save failed (partial or full)."));
    }
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

    Serial.println(F("[RNS] ── Reticulum Transport Node ──"));
    Serial.println(F("[RNS] WisBlock 1W  |  v" FW_VERSION_STRING));

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

    // ── Radio ─────────────────────────────────────────────
    Serial.print(F("[RNS] Radio: "));
    radioOk = radio.begin(&transport);
    if (radioOk) {
        Serial.println(F("OK"));
        digitalWrite(PIN_LED_BLUE, HIGH);

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
        errorBlinkRadio();  // does not return; keeps WDT fed
    }

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

    // Periodic self-announce so peers can discover this node
    if (radioOk && now >= nextAnnounceAt) {
        if (transport.sendLocalAnnounce()) {
            Serial.println(F("[RNS] Announce sent"));
        } else {
            Serial.println(F("[RNS] Announce send failed"));
        }
        scheduleNextAnnounce(now);
    }

    // Heartbeat: toggle green LED every 2 seconds
    if (now - lastLedToggle >= 2000) {
        digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
        lastLedToggle = now;
    }
}
