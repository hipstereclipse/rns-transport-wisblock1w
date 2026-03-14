/**
 * @file RNSConsole.h
 * @brief Interactive command-line console over USB CDC serial (and
 *        optionally BLE UART).  Provides status, routing, identity,
 *        radio config, factory-reset, and OTA-trigger commands.
 *
 * SAFETY: The "factory-reset" command erases persisted state but
 * preserves the bootloader. The "dfu" command reboots into the
 * Adafruit nRF52 bootloader for safe USB flashing.
 */
#pragma once
#include "RNSConfig.h"
#include "RNSTransport.h"
#include "RNSRadio.h"
#include "RNSIdentity.h"

#ifndef NATIVE_TEST
#include <nrf_gpio.h>
extern "C" char* sbrk(int incr);
#endif

// Stringify helper for pin numbers in F() strings
#define PIN_STR_INNER(x) #x
#define PIN_STR(x) PIN_STR_INNER(x)

// Forward declaration for persistence (optional)
class RNSPersistence;

class RNSConsole {
public:
    RNSTransport*   transport   = nullptr;
    RNSRadio*       radio       = nullptr;
    RNSIdentity*    identity    = nullptr;
    RNSPersistence* persistence = nullptr;
    Stream*         io          = nullptr;
    bool            pktDumpEnabled = false;

    char    cmdBuf[128];
    uint8_t cmdPos = 0;

    void begin(Stream* stream, RNSTransport* txp,
               RNSRadio* rad, RNSIdentity* id, RNSPersistence* pers = nullptr) {
        io          = stream;
        transport   = txp;
        radio       = rad;
        identity    = id;
        persistence = pers;
        printBanner();
        printPrompt();
    }

    /** @brief Call from main loop() to process serial input. */
    void poll() {
        if (!io) return;
        while (io->available()) {
            char c = io->read();
            if (c == '\r' || c == '\n') {
                if (cmdPos > 0) {
                    cmdBuf[cmdPos] = '\0';
                    processCommand(cmdBuf);
                    cmdPos = 0;
                }
                printPrompt();
            } else if (c == 0x7F || c == '\b') {
                if (cmdPos > 0) { cmdPos--; io->print("\b \b"); }
            } else if (cmdPos < sizeof(cmdBuf) - 1) {
                cmdBuf[cmdPos++] = c;
                io->print(c);
            }
        }
    }

private:
    void printBanner() {
        io->println(F("\r\n╔════════════════════════════════════════════╗"));
        io->println(F("║  RatTunnel Node — WisBlock 1W             ║"));
        io->println(F("║  " FW_DISPLAY_VERSION "                    ║"));
            io->println(F("║  Reticulum LoRa Transport @ safe TX       ║"));
        io->println(F("╚════════════════════════════════════════════╝"));
    }

    void printPrompt() { io->print(F("\r\nrns> ")); }

    // ── Command dispatcher ────────────────────────────────
    void processCommand(const char* cmd) {
        io->println();

        // Tokenize: first word is the command, rest is args
        char command[32] = {0};
        char args[96]    = {0};
        const char* space = strchr(cmd, ' ');
        if (space) {
            size_t cmdLen = space - cmd;
            if (cmdLen >= sizeof(command)) cmdLen = sizeof(command) - 1;
            strncpy(command, cmd, cmdLen);
            strncpy(args, space + 1, sizeof(args) - 1);
        } else {
            strncpy(command, cmd, sizeof(command) - 1);
        }

        if      (strcmp(command, "status")    == 0) cmdStatus();
        else if (strcmp(command, "routes")    == 0) cmdRoutes();
        else if (strcmp(command, "peers")     == 0) cmdPeers();
        else if (strcmp(command, "identity")  == 0) cmdIdentity();
        else if (strcmp(command, "stats")     == 0) cmdStatus();
        else if (strcmp(command, "radio")     == 0) cmdRadio();
        else if (strcmp(command, "set")       == 0) cmdSet(args);
        else if (strcmp(command, "profile")   == 0) cmdProfile(args);
        else if (strcmp(command, "name")      == 0) cmdName(args);
        else if (strcmp(command, "save")      == 0) cmdSave();
        else if (strcmp(command, "factory-reset") == 0) cmdFactoryReset();
        else if (strcmp(command, "dfu")       == 0) cmdDfu();
        else if (strcmp(command, "reboot")    == 0) cmdReboot();
        else if (strcmp(command, "announce")  == 0) cmdAnnounce(args);
        else if (strcmp(command, "morse")     == 0) cmdMorse(args);
        else if (strcmp(command, "test")      == 0) cmdTest();
        else if (strcmp(command, "ping")      == 0) cmdTest();
        else if (strcmp(command, "reinit")    == 0) cmdReinit();
        else if (strcmp(command, "pintest")   == 0) cmdPintest();
        else if (strcmp(command, "version")   == 0) cmdVersion();
        else if (strcmp(command, "rxdiag")    == 0) cmdRxDiag();
        else if (strcmp(command, "irqmon")    == 0) cmdIrqMon(args);
        else if (strcmp(command, "rxraw")     == 0) cmdRxRaw();
        else if (strcmp(command, "txraw")     == 0) cmdTxRaw(args);
        else if (strcmp(command, "pktdump")   == 0) cmdPktDump(args);
        else if (strcmp(command, "help")      == 0) cmdHelp();
        else {
            io->print(F("Unknown: "));
            io->println(cmd);
            io->println(F("Type 'help' for commands."));
        }
    }

    // ── status / stats ────────────────────────────────────
    void cmdStatus() {
        const auto& s = transport->getStats();
        io->println(F("── Transport Node Status ──"));
        io->print(F("  Firmware:   ")); io->println(F(FW_DISPLAY_VERSION));
#ifndef NATIVE_TEST
        io->print(F("  Uptime:     ")); io->print(millis() / 1000); io->println(F(" s"));
#endif
        io->print(F("  Radio HW:   ")); io->println(radio->hwReady ? F("OK") : F("FAILED"));
        if (!radio->hwReady) {
            io->print(F("  Init error: ")); io->println(radio->lastInitState);
            io->print(F("  Attempts:   ")); io->println(radio->initAttempts);
        }
        io->print(F("  RX packets: ")); io->println(s.rxPackets);
        io->print(F("  TX packets: ")); io->println(s.txPackets);
        io->print(F("  Forwarded:  ")); io->println(s.fwdPackets);
        io->print(F("  Announces:  ")); io->println(s.announces);
        io->print(F("  Duplicates: ")); io->println(s.duplicates);
        io->print(F("  Invalid:    ")); io->println(s.invalidPackets);
        io->print(F("  Paths:      ")); io->println(s.pathEntries);
        io->print(F("  Broadcast name: ")); io->println(transport->getAnnounceName());
#ifndef NATIVE_TEST
        io->print(F("  Free RAM:   ")); io->print(freeMemory()); io->println(F(" bytes"));
#endif
    }

    // ── name [new name...] ──────────────────────────────
    void cmdName(const char* args) {
        if (!transport) {
            io->println(F("Transport not available."));
            return;
        }

        while (args && *args == ' ') args++;

        if (!args || *args == '\0') {
            io->print(F("Broadcast name: "));
            io->println(transport->getAnnounceName());
            io->println(F("Usage: name <text>"));
            return;
        }

        if (!transport->setAnnounceName(args)) {
            io->println(F("Invalid name. Use 1-32 visible characters."));
            return;
        }

        io->print(F("Broadcast name → "));
        io->println(transport->getAnnounceName());
        io->println(F("Tip: run 'save' to persist this name, then 'announce' to broadcast now."));
    }

    // ── routes ────────────────────────────────────────────
    void cmdRoutes() {
        const PathEntry* pt = transport->getPathTable();
        io->println(F("── Routing Table ──"));
        io->println(F("  Dest Hash          Hops  Age(s)"));
        io->println(F("  ────────────────── ───── ──────"));
#ifndef NATIVE_TEST
        uint32_t now = millis();
#else
        uint32_t now = 0;
#endif
        int count = 0;
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (!pt[i].active) continue;
            io->print(F("  "));
            printHex(pt[i].destHash, 8);
            io->print(F("..  "));
            io->print(pt[i].hops);
            io->print(F("     "));
            io->println((now - pt[i].learnedAt) / 1000);
            count++;
        }
        if (count == 0) io->println(F("  (empty)"));
    }

    // ── peers (alias with peer-oriented heading) ────────
    void cmdPeers() {
        const PathEntry* pt = transport->getPathTable();
        io->println(F("── Known Peers ──"));
        io->println(F("  Peer Hash          Hops  Age(s)"));
        io->println(F("  ────────────────── ───── ──────"));
#ifndef NATIVE_TEST
        uint32_t now = millis();
#else
        uint32_t now = 0;
#endif
        int count = 0;
        for (int i = 0; i < PATH_TABLE_MAX; i++) {
            if (!pt[i].active) continue;
            io->print(F("  "));
            printHex(pt[i].destHash, 8);
            io->print(F("..  "));
            io->print(pt[i].hops);
            io->print(F("     "));
            io->println((now - pt[i].learnedAt) / 1000);
            count++;
        }
        if (count == 0) io->println(F("  (none yet — run announce and wait for peers)"));
    }

    // ── identity ──────────────────────────────────────────
    void cmdIdentity() {
        io->println(F("── Node Identity ──"));
        io->print(F("  Hash:       ")); printHex(identity->identityHash, RNS_ADDR_LEN); io->println();
        io->print(F("  X25519 pub: ")); printHex(identity->pubEncKey, 32); io->println();
        io->print(F("  Ed25519 pub: ")); printHex(identity->pubSigKey, 32); io->println();
    }

    // ── radio ─────────────────────────────────────────────
    void cmdRadio() {
        io->println(F("── Radio Configuration ──"));
        io->print(F("  HW Status: ")); io->println(radio->hwReady ? F("OK (SX1262 live)") : F("FAILED (not initialized)"));
        if (!radio->hwReady) {
            io->print(F("  Init RC:   ")); io->println(radio->lastInitState);
            io->print(F("  Attempts:  ")); io->println(radio->initAttempts);
        }
        io->print(F("  Frequency: ")); io->print(radio->curFreqMHz, 1); io->println(F(" MHz"));
        io->print(F("  Bandwidth: ")); io->print(radio->curBwKHz, 1); io->println(F(" kHz"));
        io->print(F("  SF:        ")); io->println(radio->curSF);
        io->print(F("  CR:        4/")); io->println(radio->curCR);
        io->print(F("  TX Power:  ")); io->print(radio->curTxDbm);
                                        io->println(F(" dBm (SX1262 → +8 dB PA)"));
        io->print(F("  Sync Word: 0x"));
        if (radio->curSyncWord < 0x10) io->print('0');
        io->println(radio->curSyncWord, HEX);
        io->print(F("  Preamble:  ")); io->println(radio->curPreamble);
        io->print(F("  Last RSSI: ")); io->print(radio->lastRSSI); io->println(F(" dBm"));
        io->print(F("  Last SNR:  ")); io->print(radio->lastSNR); io->println(F(" dB"));
    }

    // ── set <param> <value> ───────────────────────────────
    void cmdSet(const char* args) {
        char param[16] = {0};
        char value[16] = {0};
        if (sscanf(args, "%15s %15s", param, value) < 2) {
            io->println(F("Usage: set <freq|sf|bw|cr|txpower|syncword|preamble> <value>"));
            return;
        }

        if (strcmp(param, "freq") == 0) {
            float f = atof(value);
            if (f < 100.0f || f > 1000.0f) { io->println(F("Invalid frequency")); return; }
            radio->setFrequency(f);
            io->print(F("Frequency → ")); io->print(f, 1); io->println(F(" MHz"));
        } else if (strcmp(param, "sf") == 0) {
            int sf = atoi(value);
            if (sf < 5 || sf > 12) { io->println(F("SF must be 5–12")); return; }
            radio->setSF((uint8_t)sf);
            io->print(F("SF → ")); io->println(sf);
        } else if (strcmp(param, "bw") == 0) {
            float bw = atof(value);
            radio->setBandwidth(bw);
            io->print(F("BW → ")); io->print(bw, 1); io->println(F(" kHz"));
        } else if (strcmp(param, "cr") == 0) {
            int cr = atoi(value);
            if (cr < 5 || cr > 8) { io->println(F("CR must be 5–8")); return; }
            radio->setCR((uint8_t)cr);
            io->print(F("CR → 4/")); io->println(cr);
        } else if (strcmp(param, "txpower") == 0) {
            int dbm = atoi(value);
                if (dbm < -9 || dbm > LORA_TX_DBM_MAX_SAFE) {
                    io->print(F("TX power must be -9 to "));
                    io->print(LORA_TX_DBM_MAX_SAFE);
                    io->println(F(" dBm"));
                    return;
                }
            radio->setTxPower((int8_t)dbm);
            io->print(F("TX → ")); io->print(dbm); io->println(F(" dBm"));
        } else if (strcmp(param, "sync") == 0 || strcmp(param, "syncword") == 0) {
            char* end = nullptr;
            long sync = strtol(value, &end, 0);
            if (!end || *end != '\0' || sync < 0 || sync > 255) {
                io->println(F("Sync word must be 0-255 (hex allowed, e.g. 0x12)"));
                return;
            }
            radio->setSyncWord((uint8_t)sync);
            io->print(F("Sync word → 0x"));
            if (sync < 0x10) io->print('0');
            io->println((uint8_t)sync, HEX);
        } else if (strcmp(param, "preamble") == 0) {
            int preamble = atoi(value);
            if (preamble < 4 || preamble > 255) { io->println(F("Preamble must be 4–255 symbols")); return; }
            radio->setPreamble((uint16_t)preamble);
            io->print(F("Preamble → ")); io->println(preamble);
        } else {
            io->println(F("Unknown param. Use: freq, sf, bw, cr, txpower, syncword, preamble"));
        }
    }

    // ── profile <name> (one-shot presets) ─────────────────
    void cmdProfile(const char* args) {
        while (args && *args == ' ') args++;

        if (!args || *args == '\0') {
            io->println(F("Usage: profile <rnode-eu|rnode-us|ratspeak-us>"));
            io->println(F("  rnode-eu:    867.2 MHz, BW125, SF8, CR5, TX7, sync 0x12, preamble 8"));
            io->println(F("  rnode-us:    915.0 MHz, BW125, SF8, CR5, TX7, sync 0x12, preamble 8"));
            io->println(F("  ratspeak-us: 915.0 MHz, BW125, SF9, CR5, TX17, sync 0x12, preamble 18"));
            return;
        }

        if (strcmp(args, "rnode-eu") == 0 || strcmp(args, "eu") == 0) {
            radio->setFrequency(867.2f);
            radio->setBandwidth(125.0f);
            radio->setSF(8);
            radio->setCR(5);
            radio->setTxPower(7);
            radio->setSyncWord(0x12);
            radio->setPreamble(8);
            io->println(F("Profile applied: rnode-eu"));
            io->println(F("Tip: run 'save' to persist, then 'announce' to broadcast now."));
            cmdRadio();
            return;
        }

        if (strcmp(args, "rnode-us") == 0 || strcmp(args, "us") == 0) {
            radio->setFrequency(915.0f);
            radio->setBandwidth(125.0f);
            radio->setSF(8);
            radio->setCR(5);
            radio->setTxPower(7);
            radio->setSyncWord(0x12);
            radio->setPreamble(8);
            io->println(F("Profile applied: rnode-us"));
            io->println(F("Tip: run 'save' to persist, then 'announce' to broadcast now."));
            cmdRadio();
            return;
        }

        if (strcmp(args, "ratspeak-us") == 0 || strcmp(args, "rs-us") == 0) {
            radio->setFrequency(915.0f);
            radio->setBandwidth(125.0f);
            radio->setSF(9);
            radio->setCR(5);
            radio->setTxPower(17);
            radio->setSyncWord(0x12);
            radio->setPreamble(18);
            io->println(F("Profile applied: ratspeak-us"));
            io->println(F("Tip: run 'save' to persist, then 'announce' to broadcast now."));
            cmdRadio();
            return;
        }

        io->print(F("Unknown profile: "));
        io->println(args);
        io->println(F("Use: profile <rnode-eu|rnode-us|ratspeak-us>"));
    }

    // ── save (persist current config to flash) ────────────
    void cmdSave();   // implemented in main.cpp (needs persistence ref)

    // ── morse blink settings ──────────────────────────────
    void cmdMorse(const char* args); // implemented in main.cpp

    // ── factory-reset ─────────────────────────────────────
    void cmdFactoryReset();  // implemented in main.cpp

    // ── dfu: reboot into Adafruit bootloader for safe flashing ─
    void cmdDfu() {
        io->println(F("Entering DFU bootloader mode..."));
        io->println(F("Device will appear as USB drive for UF2 flashing."));
        io->flush();
#ifndef NATIVE_TEST
        delay(500);
        // The Adafruit nRF52 bootloader enters DFU when GPREGRET == 0x57.
        // Use SoftDevice API when SD is active, else direct register write.
        // Both paths end with a DSB to guarantee the write commits before reset.
        uint32_t rc = 0xFFFFFFFF;
#if __has_include("nrf_soc.h")
        rc  = sd_power_gpregret_clr(0, 0xFF);
        rc |= sd_power_gpregret_set(0, 0x57);
#endif
        if (rc != 0) {
            NRF_POWER->GPREGRET = 0x57;
        }
        __DSB();
        __ISB();
        NVIC_SystemReset();
#endif
    }

    // ── reboot ────────────────────────────────────────────
    void cmdReboot() {
        io->println(F("Rebooting..."));
        io->flush();
#ifndef NATIVE_TEST
        delay(200);
        NVIC_SystemReset();
#endif
    }

    // ── announce ─────────────────────────────────────────
    void cmdAnnounce(const char* args) {
        if (!transport) {
            io->println(F("Transport not available."));
            return;
        }
        if (!radio->hwReady) {
            io->println(F("Announce blocked: radio hardware not initialized."));
            io->print(F("  Init error code: ")); io->println(radio->lastInitState);
            io->println(F("  Try power-cycling the device or check RAK13302 connection."));
            return;
        }

        const uint8_t* appData = nullptr;
        uint16_t appDataLen = 0;
        uint8_t appDataBuf[80] = {0};

        while (args && *args == ' ') args++;
        if (args && *args != '\0') {
            size_t inLen = strlen(args);
            if (inLen > sizeof(appDataBuf) - 1) inLen = sizeof(appDataBuf) - 1;
            memcpy(appDataBuf, args, inLen);
            appDataBuf[inLen] = '\0';
            appData = appDataBuf;
            appDataLen = (uint16_t)inLen;
        }

        if (transport->sendLocalAnnounce(nullptr, appData, appDataLen)) {
            io->println(F("Local announce transmitted."));
            if (appData && appDataLen > 0) {
                io->print(F("Announce payload: "));
                io->println((const char*)appDataBuf);
            }
        } else {
            io->println(F("Announce transmit failed (radio busy/offline)."));
        }
    }

    // ── test / ping: one-line health response ───────────
    void cmdTest() {
        const auto& s = transport->getStats();
        io->print(F("TEST_OK "));
#ifndef NATIVE_TEST
        io->print(F("uptime_s=")); io->print(millis() / 1000); io->print(F(" "));
#endif
        io->print(F("rx=")); io->print(s.rxPackets); io->print(F(" "));
        io->print(F("tx=")); io->print(s.txPackets); io->print(F(" "));
        io->print(F("fwd=")); io->print(s.fwdPackets); io->print(F(" "));
        io->print(F("paths=")); io->print(s.pathEntries); io->print(F(" "));
        io->print(F("freq_mhz=")); io->print(radio->curFreqMHz, 1); io->print(F(" "));
        io->print(F("sf=")); io->print(radio->curSF); io->print(F(" "));
        io->print(F("bw_khz=")); io->print(radio->curBwKHz, 1); io->print(F(" "));
        io->print(F("tx_dbm=")); io->println(radio->curTxDbm);
    }

    // ── version ───────────────────────────────────────────
    void cmdVersion() {
        io->println(F(FW_DISPLAY_VERSION));
        io->print(F("Build: ")); io->println(F(FW_BUILD_TAG));
        io->print(F("Board: WisBlock 1W (RAK3401 + RAK13302)"));
        io->println();
    }

    // ── reinit (retry radio init without rebooting) ───────
    void cmdReinit() {
        io->println(F("Retrying radio initialization..."));
        io->println(F("(watch serial output for [DIAG] lines)"));
        bool ok = radio->begin(transport);
        if (ok) {
            io->println(F("Radio init SUCCESS — SX1262 is now live."));
        } else {
            io->println(F("Radio init FAILED again."));
            io->print(F("  Last error: ")); io->println(radio->lastInitState);
            io->println(F("  Try: reseat RAK13302, check base board 3V3_S rail."));
        }
    }

    // ── pintest (scan IO pins to identify SX1262 connections) ─
    void cmdPintest() {
#ifndef NATIVE_TEST
        // ── Full GPIO scan: ALL 48 pins (0-47) ──────────────────
        // Exclude only active SPI data lines that could cause bus
        // contention: SCK(3), MISO(29), MOSI(30), NSS(26).
        // We include everything else to find SX1262 BUSY/DIO1/RST
        // which may be on unexpected pins.
        static const uint8_t spiPins[] = {3, 26, 29, 30};
        auto isSpiPin = [&](uint8_t p) {
            for (uint8_t s : spiPins) { if (p == s) return true; }
            return false;
        };

        io->println(F("── PIN SCAN: full 48-GPIO power+RST cycle test ──"));
        io->println(F("  PIN_LORA_ENABLE(P34)=WB_IO2, PIN_LORA_RESET(P17)=WB_IO1"));
        io->println(F("  Skipping SPI pins: 3(SCK) 26(NSS) 29(MISO) 30(MOSI)"));
        io->println();

        // Remove any pull-down/pull-up overrides from init code
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
        }

        uint8_t ph[4][48];  // phase readings for all 48 GPIOs

        // Phase 1: Read with power ON (current state)
        for (uint8_t p = 0; p < 48; p++)
            ph[0][p] = isSpiPin(p) ? 2 : digitalRead(p);

        // Phase 2: Drive P34 LOW (power-gate test)
        pinMode(PIN_LORA_ENABLE, OUTPUT);
        digitalWrite(PIN_LORA_ENABLE, LOW);
        delay(100);
        for (uint8_t p = 0; p < 48; p++)
            ph[1][p] = isSpiPin(p) ? 2 : digitalRead(p);

        // Phase 3: P34 HIGH + RST toggle, read at +5ms
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(10);
        pinMode(PIN_LORA_RESET, OUTPUT);
        digitalWrite(PIN_LORA_RESET, LOW);
        delay(5);
        digitalWrite(PIN_LORA_RESET, HIGH);
        delay(5);
        for (uint8_t p = 0; p < 48; p++)
            ph[2][p] = isSpiPin(p) ? 2 : digitalRead(p);

        // Phase 4: Wait for boot to finish (+500ms = BUSY LOW)
        delay(500);
        for (uint8_t p = 0; p < 48; p++)
            ph[3][p] = isSpiPin(p) ? 2 : digitalRead(p);

        // ── Print only pins that CHANGED between any phase ──────
        io->println(F("  Pins that CHANGED during cycle:"));
        io->println(F("  Pin   PRE P34L +5ms  +500ms  Signature"));
        io->println(F("  ────  ─── ──── ────  ──────  ─────────"));
        uint8_t changedCount = 0;
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            bool changed = false;
            for (uint8_t j = 1; j < 4; j++) {
                if (ph[j][p] != ph[0][p]) { changed = true; break; }
            }
            if (!changed) continue;
            changedCount++;
            io->print(F("  P")); if (p < 10) io->print(' ');
            io->print(p);
            for (uint8_t j = 0; j < 4; j++) {
                io->print(F("   ")); io->print(ph[j][p]);
            }
            // Identify signature
            io->print(F("     "));
            uint8_t a=ph[0][p], b=ph[1][p], c=ph[2][p], d=ph[3][p];
            if (a==1 && b==0 && c==1 && d==1) io->print(F("← POWER-GATE (lost during P34=LOW)"));
            else if (a==1 && b==1 && c==0 && d==1) io->print(F("← RST pin (dropped at RST)"));
            else if (b==0 && c==1 && d==0) io->print(F("← BUSY candidate (0→1→0)"));
            else if (b==0 && c==1 && d==1) io->print(F("← BUSY-stuck or TCXO fail (0→1→1)"));
            else if (b==0 && c==0 && d==1) io->print(F("← late-rise (DIO1?)"));
            else io->print(F("← other"));
            io->println();
        }
        if (changedCount == 0)
            io->println(F("  (NONE — no pins changed! SX1262 may be disconnected)"));

        // ── Print all pins that are STATIC LOW ──────────────────
        io->println();
        io->print(F("  Static LOW:  "));
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            bool allSame = true;
            for (uint8_t j = 1; j < 4; j++) { if (ph[j][p] != ph[0][p]) { allSame = false; break; } }
            if (allSame && ph[0][p] == 0) { io->print('P'); io->print(p); io->print(' '); }
        }
        io->println();
        io->print(F("  Static HIGH: "));
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            bool allSame = true;
            for (uint8_t j = 1; j < 4; j++) { if (ph[j][p] != ph[0][p]) { allSame = false; break; } }
            if (allSame && ph[0][p] == 1) { io->print('P'); io->print(p); io->print(' '); }
        }
        io->println();

        // ── BUSY brute-force discovery ──────────────────────────
        // For each candidate pin, try it as BUSY: wait for it to
        // go LOW, then do an SPI read and check for non-0xD2 data.
        io->println();
        io->println(F("  ── BUSY discovery (SPI probe per pin) ──"));

        // First power-cycle + hard reset the module
        pinMode(PIN_LORA_ENABLE, OUTPUT);
        digitalWrite(PIN_LORA_ENABLE, LOW);
        delay(50);
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(10);
        pinMode(PIN_LORA_RESET, OUTPUT);
        digitalWrite(PIN_LORA_RESET, LOW);
        delay(5);
        digitalWrite(PIN_LORA_RESET, HIGH);
        delay(500);  // generous settle time

        // Re-init SPI
        static SPIClass& scanSPI = *(new SPIClass(NRF_SPIM3, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI));
        scanSPI.begin();
        pinMode(PIN_LORA_NSS, OUTPUT);
        digitalWrite(PIN_LORA_NSS, HIGH);

        // Raw SPI read of reg 0x0320 (version string byte 0)
        auto spiProbe = [&]() -> uint8_t {
            scanSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);
            scanSPI.transfer(0x1D);
            scanSPI.transfer(0x03);
            scanSPI.transfer(0x20);
            scanSPI.transfer(0x00);
            uint8_t val = scanSPI.transfer(0x00);
            digitalWrite(PIN_LORA_NSS, HIGH);
            scanSPI.endTransaction();
            return val;
        };

        // Baseline: what we get without any BUSY management
        uint8_t baseline = spiProbe();
        io->print(F("  Baseline SPI read (no BUSY): 0x"));
        io->println(baseline, HEX);

        // Now try each candidate as BUSY input
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            if (p == PIN_LORA_ENABLE) continue;  // don't mess with power

            // Configure pin as input, no pull
            nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
            uint8_t rawState = digitalRead(p);

            // Wait for this pin to go LOW (max 50ms)
            bool wentLow = (rawState == LOW);
            if (!wentLow) {
                uint32_t t0 = millis();
                while (millis() - t0 < 50) {
                    if (digitalRead(p) == LOW) { wentLow = true; break; }
                    delayMicroseconds(100);
                }
            }

            if (!wentLow) continue;  // skip pins stuck HIGH

            // This pin is LOW — do SPI probe
            uint8_t val = spiProbe();
            if (val != baseline && val != 0x00 && val != 0xFF) {
                io->print(F("  *** P")); io->print(p);
                io->print(F(" → SPI read=0x")); io->print(val, HEX);
                io->println(F(" ← LIKELY BUSY PIN! ***"));
            }
        }

        // ── Multi-CS probe ──────────────────────────────────────
        // Try alternate CS pins in case module is on a different slot
        io->println();
        io->println(F("  ── Multi-CS probe ──"));
        uint8_t csPins[] = {26, 36, 32, 42, 6, 19, 25};
        for (uint8_t cs : csPins) {
            if (cs == PIN_LORA_NSS) continue;
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);
            delay(1);

            // Read with this alternate CS
            scanSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(cs, LOW);
            delayMicroseconds(200);
            scanSPI.transfer(0x1D);
            scanSPI.transfer(0x03);
            scanSPI.transfer(0x20);
            scanSPI.transfer(0x00);
            uint8_t val = scanSPI.transfer(0x00);
            digitalWrite(cs, HIGH);
            scanSPI.endTransaction();

            // Read 2nd byte for more info
            scanSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(cs, LOW);
            delayMicroseconds(200);
            uint8_t gs0 = scanSPI.transfer(0xC0);
            uint8_t gs1 = scanSPI.transfer(0x00);
            digitalWrite(cs, HIGH);
            scanSPI.endTransaction();

            io->print(F("  CS=P")); io->print(cs);
            io->print(F(": reg=0x")); io->print(val, HEX);
            io->print(F(" status=0x")); io->print(gs0, HEX);
            io->print(F("/0x")); io->print(gs1, HEX);
            if (val != 0x00 && val != 0xFF && val != baseline)
                io->print(F(" ← DIFFERENT!"));
            io->println();

            // Restore as input
            pinMode(cs, INPUT);
        }

        io->println();
        io->println(F("  Current config: DIO1=NC"
                       " BUSY="PIN_STR(PIN_LORA_BUSY)
                       " RST="PIN_STR(PIN_LORA_RESET)));
#endif
    }

    // ── rxdiag: SX1262 RX state deep dive ─────────────────
    void cmdRxDiag() {
#ifndef NATIVE_TEST
        if (!radio || !radio->hwReady) {
            io->println(F("Radio not ready."));
            return;
        }
        io->println(F("── RX Diagnostic ──"));

        // 1. Chip operating mode via GetStatus SPI command
        SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_LORA_NSS, LOW);
        delayMicroseconds(200);
        SPI.transfer(0xC0); // GetStatus opcode
        uint8_t status = SPI.transfer(0x00);
        digitalWrite(PIN_LORA_NSS, HIGH);
        SPI.endTransaction();

        uint8_t chipMode = (status >> 4) & 0x07;
        uint8_t cmdStatus = (status >> 1) & 0x07;
        const char* modeStr;
        switch (chipMode) {
            case 2: modeStr = "STBY_RC"; break;
            case 3: modeStr = "STBY_XOSC"; break;
            case 4: modeStr = "FS"; break;
            case 5: modeStr = "RX"; break;
            case 6: modeStr = "TX"; break;
            default: modeStr = "UNKNOWN"; break;
        }
        io->print(F("  ChipMode: ")); io->print(chipMode);
        io->print(F(" (")); io->print(modeStr); io->println(F(")"));
        io->print(F("  CmdStatus: ")); io->println(cmdStatus);

        // 2. IRQ flags
        uint32_t irq = radio->lora.getIrqFlags();
        io->print(F("  IRQ flags: 0x")); io->println(irq, HEX);
        if (irq & (1UL << RADIOLIB_IRQ_TX_DONE))            io->println(F("    [TX_DONE]"));
        if (irq & (1UL << RADIOLIB_IRQ_RX_DONE))            io->println(F("    [RX_DONE]"));
        if (irq & (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED))  io->println(F("    [PREAMBLE]"));
        if (irq & (1UL << RADIOLIB_IRQ_SYNC_WORD_VALID))    io->println(F("    [SYNCWORD]"));
        if (irq & (1UL << RADIOLIB_IRQ_HEADER_VALID))       io->println(F("    [HDR_VALID]"));
        if (irq & (1UL << RADIOLIB_IRQ_HEADER_ERR))         io->println(F("    [HDR_ERR]"));
        if (irq & (1UL << RADIOLIB_IRQ_CRC_ERR))            io->println(F("    [CRC_ERR]"));
        if (irq & (1UL << RADIOLIB_IRQ_CAD_DONE))           io->println(F("    [CAD_DONE]"));
        if (irq & (1UL << RADIOLIB_IRQ_CAD_DETECTED))       io->println(F("    [CAD_DET]"));
        if (irq & (1UL << RADIOLIB_IRQ_TIMEOUT))            io->println(F("    [TIMEOUT]"));

        // 3. BUSY pin
        io->print(F("  BUSY(P")); io->print(PIN_LORA_BUSY);
        io->print(F("): ")); io->println(digitalRead(PIN_LORA_BUSY));

        // 4. rxFlag / pollMode
        io->print(F("  rxFlag: ")); io->println(radio->rxFlag ? "true" : "false");
        io->print(F("  pollMode: ")); io->println(radio->pollMode ? "true" : "false");

        // 5. Packet buffer check
        int pktLen = radio->lora.getPacketLength();
        io->print(F("  Pending pktLen: ")); io->println(pktLen);

        // 6. Current radio params
        io->print(F("  Freq: ")); io->print(radio->curFreqMHz, 1); io->println(F(" MHz"));
        io->print(F("  SF: ")); io->println(radio->curSF);
        io->print(F("  BW: ")); io->print(radio->curBwKHz, 1); io->println(F(" kHz"));
        io->print(F("  Sync: 0x")); io->println(radio->curSyncWord, HEX);
        io->print(F("  RNODE header: "));
        #if RNODE_LORA_HEADER_ENABLED
            io->println(F("ON (1-byte prefix stripped on RX, added on TX)"));
        #else
            io->println(F("OFF (raw Reticulum frames)"));
        #endif

        // 7. Re-enter RX and report
        int rc = radio->lora.startReceive();
        io->print(F("  Re-startReceive rc: ")); io->println(rc);
#endif
    }

    // ── irqmon: Monitor IRQ register for N seconds ────────
    void cmdIrqMon(const char* args) {
#ifndef NATIVE_TEST
        if (!radio || !radio->hwReady) {
            io->println(F("Radio not ready."));
            return;
        }
        int seconds = 10;
        if (args && args[0]) seconds = atoi(args);
        if (seconds < 1) seconds = 1;
        if (seconds > 60) seconds = 60;

        io->print(F("── IRQ Monitor (")); io->print(seconds);
        io->println(F("s) — press any key to stop ──"));
        io->println(F("  Watching for: PREAMBLE, SYNCWORD, RX_DONE, CRC_ERR, TIMEOUT, HDR_ERR"));

        radio->lora.startReceive();
        uint32_t prevIrq = 0;
        uint32_t end = millis() + (uint32_t)seconds * 1000;
        uint32_t ticks = 0;
        uint32_t preambles = 0, syncs = 0, rxDone = 0, crcErr = 0, timeouts = 0, hdrErr = 0;

        while (millis() < end) {
            if (io->available()) break;  // key pressed
            uint32_t irq = radio->lora.getIrqFlags();
            if (irq != prevIrq) {
                uint32_t dt = millis();
                io->print(F("  [")); io->print(dt / 1000); io->print(F(".")); 
                io->print((dt % 1000) / 100); io->print(F("s] IRQ=0x")); io->print(irq, HEX);
                if (irq & (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED))  { io->print(F(" PREAMBLE")); preambles++; }
                if (irq & (1UL << RADIOLIB_IRQ_SYNC_WORD_VALID))    { io->print(F(" SYNC")); syncs++; }
                if (irq & (1UL << RADIOLIB_IRQ_HEADER_VALID))       io->print(F(" HDR_OK"));
                if (irq & (1UL << RADIOLIB_IRQ_RX_DONE))            { io->print(F(" RX_DONE")); rxDone++; }
                if (irq & (1UL << RADIOLIB_IRQ_CRC_ERR))            { io->print(F(" CRC_ERR")); crcErr++; }
                if (irq & (1UL << RADIOLIB_IRQ_HEADER_ERR))         { io->print(F(" HDR_ERR")); hdrErr++; }
                if (irq & (1UL << RADIOLIB_IRQ_TIMEOUT))            { io->print(F(" TIMEOUT")); timeouts++; }
                io->println();
                prevIrq = irq;
                
                // If RX_DONE, dump the packet
                if (irq & (1UL << RADIOLIB_IRQ_RX_DONE)) {
                    int len = radio->lora.getPacketLength();
                    if (len > 0 && len <= RNS_MTU) {
                        uint8_t buf[RNS_MTU];
                        radio->lora.readData(buf, len);
                        io->print(F("    RSSI=")); io->print(radio->lora.getRSSI(), 1);
                        io->print(F(" SNR=")); io->print(radio->lora.getSNR(), 1);
                        io->print(F(" len=")); io->println(len);
                        io->print(F("    HEX: "));
                        for (int i = 0; i < len && i < 64; i++) {
                            if (buf[i] < 0x10) io->print('0');
                            io->print(buf[i], HEX);
                            if (i < len - 1) io->print(' ');
                        }
                        if (len > 64) io->print(F("..."));
                        io->println();
                    }
                    radio->lora.startReceive(); // re-enter RX
                    prevIrq = 0;
                }
            }
            ticks++;
            delay(1);
        }

        io->println(F("  ── Summary ──"));
        io->print(F("    Polls: ")); io->println(ticks);
        io->print(F("    Preambles: ")); io->println(preambles);
        io->print(F("    SyncWords: ")); io->println(syncs);
        io->print(F("    RX_DONE: ")); io->println(rxDone);
        io->print(F("    CRC_ERR: ")); io->println(crcErr);
        io->print(F("    HDR_ERR: ")); io->println(hdrErr);
        io->print(F("    Timeouts: ")); io->println(timeouts);

        radio->lora.startReceive(); // make sure we end in RX
#endif
    }

    // ── rxraw: Force-read SX1262 FIFO ─────────────────────
    void cmdRxRaw() {
#ifndef NATIVE_TEST
        if (!radio || !radio->hwReady) {
            io->println(F("Radio not ready."));
            return;
        }
        io->println(F("── RX Raw FIFO Dump ──"));
        uint32_t irq = radio->lora.getIrqFlags();
        io->print(F("  IRQ: 0x")); io->println(irq, HEX);

        int len = radio->lora.getPacketLength();
        io->print(F("  pktLen: ")); io->println(len);

        if (len > 0 && len <= RNS_MTU) {
            uint8_t buf[RNS_MTU];
            int rc = radio->lora.readData(buf, len);
            io->print(F("  readData rc: ")); io->println(rc);
            io->print(F("  RSSI: ")); io->print(radio->lora.getRSSI(), 1);
            io->print(F("  SNR: ")); io->println(radio->lora.getSNR(), 1);
            io->print(F("  RAW[")); io->print(len); io->print(F("]: "));
            for (int i = 0; i < len; i++) {
                if (buf[i] < 0x10) io->print('0');
                io->print(buf[i], HEX);
                if (i < len - 1) io->print(' ');
            }
            io->println();

            // Try to parse as Reticulum packet
            const uint8_t* payload = buf;
            uint16_t payloadLen = (uint16_t)len;
            #if RNODE_LORA_HEADER_ENABLED
            if (payloadLen > 1) {
                io->print(F("  RNode hdr: 0x")); io->println(buf[0], HEX);
                payload = &buf[1];
                payloadLen--;
            }
            #endif
            if (payloadLen >= RNS_HEADER_SIZE + RNS_ADDR_LEN + 1) {
                uint8_t flags = payload[0];
                io->print(F("  RNS flags: 0x")); io->println(flags, HEX);
                io->print(F("    ifac=")); io->print((flags >> 7) & 1);
                io->print(F(" hdr=")); io->print((flags >> 6) & 1);
                io->print(F(" ctx=")); io->print((flags >> 5) & 1);
                io->print(F(" prop=")); io->print((flags >> 4) & 1);
                io->print(F(" dest=")); io->print((flags >> 2) & 3);
                io->print(F(" type=")); io->println(flags & 3);
                io->print(F("    hops=")); io->println(payload[1]);
                io->print(F("    destHash: "));
                for (int i = 2; i < 18 && i < payloadLen; i++) {
                    if (payload[i] < 0x10) io->print('0');
                    io->print(payload[i], HEX);
                }
                io->println();
            }
        } else {
            io->println(F("  (no packet in FIFO)"));
        }
        radio->lora.startReceive();
#endif
    }

    // ── txraw: Transmit raw hex bytes ─────────────────────
    void cmdTxRaw(const char* args) {
#ifndef NATIVE_TEST
        if (!radio || !radio->hwReady) {
            io->println(F("Radio not ready."));
            return;
        }
        if (!args || !args[0]) {
            io->println(F("Usage: txraw <hex bytes>"));
            io->println(F("  e.g. txraw 0011FF (bypasses RNODE header & CSMA)"));
            return;
        }

        // Parse hex string to bytes
        uint8_t buf[RNS_MTU];
        uint16_t len = 0;
        const char* p = args;
        while (*p && len < RNS_MTU) {
            while (*p == ' ') p++;
            if (!*p) break;
            char hi = *p++;
            if (!*p) break;
            char lo = *p++;
            auto hexVal = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            buf[len++] = (hexVal(hi) << 4) | hexVal(lo);
        }
        if (len == 0) {
            io->println(F("No valid hex data."));
            return;
        }

        io->print(F("TX raw ")); io->print(len); io->println(F(" bytes (no CSMA, no header)"));

        radio->lora.standby();
        int rc = radio->lora.startTransmit(buf, len);
        if (rc != RADIOLIB_ERR_NONE) {
            io->print(F("  startTransmit failed rc=")); io->println(rc);
            radio->lora.startReceive();
            return;
        }
        uint32_t t0 = millis();
        bool done = false;
        while (millis() - t0 < 5000) {
            uint32_t irq = radio->lora.getIrqFlags();
            if (irq & (1UL << RADIOLIB_IRQ_TX_DONE)) { done = true; break; }
            delay(1);
        }
        radio->lora.finishTransmit();
        io->print(F("  rc=")); io->print(done ? 0 : -5);
        io->print(F(" elapsed=")); io->print(millis() - t0);
        io->println(F("ms"));
        radio->lora.startReceive();
#endif
    }

    // ── pktdump: Toggle verbose per-packet hex dump ───────
    void cmdPktDump(const char* args) {
#ifndef NATIVE_TEST
        if (!radio) {
            io->println(F("Radio not ready."));
            return;
        }
        if (args && (strcmp(args, "on") == 0 || strcmp(args, "1") == 0)) {
            pktDumpEnabled = true;
        } else if (args && (strcmp(args, "off") == 0 || strcmp(args, "0") == 0)) {
            pktDumpEnabled = false;
        } else {
            pktDumpEnabled = !pktDumpEnabled;
        }
        radio->dumpStream = pktDumpEnabled ? io : nullptr;
        io->print(F("Packet hex dump: "));
        io->println(pktDumpEnabled ? F("ON — all RX packets will be hex-dumped") : F("OFF"));
#endif
    }

    // ── help ──────────────────────────────────────────────
    void cmdHelp() {
        io->println(F("── Available Commands ──"));
        io->println(F("  status         Node status and counters"));
        io->println(F("  routes         Show routing table"));
        io->println(F("  peers          Show known peers (learned routes)"));
        io->println(F("  identity       Display node identity hash + keys"));
        io->println(F("  radio          Current radio configuration"));
        io->println(F("  set <p> <v>    Set radio param (freq/sf/bw/cr/txpower/syncword/preamble)"));
        io->println(F("  profile <p>    One-shot radio preset (rnode-eu/rnode-us/ratspeak-us)"));
        io->println(F("  name <text>    Set/display broadcast announce name"));
        io->println(F("  save           Persist config to flash"));
        io->println(F("  factory-reset  Erase all persisted data"));
        io->println(F("  dfu            Reboot into bootloader (USB flashing)"));
        io->println(F("  reboot         Restart node"));
        io->println(F("  announce [txt] Broadcast announce (optional text payload)"));
        io->println(F("  morse          Configure Morse blinker (mode/default)"));
        io->println(F("  test|ping      Emit one-line node health response"));
        io->println(F("  reinit         Retry radio initialization"));
        io->println(F("  pintest        Scan IO pins to find SX1262 BUSY/DIO1/RST"));
        io->println(F("  version        Firmware version"));
        io->println(F("── Diagnostics ──"));
        io->println(F("  rxdiag         SX1262 RX state dump (chipMode, IRQ, BUSY, params)"));
        io->println(F("  irqmon [sec]   Monitor IRQ flags for N seconds (default 10)"));
        io->println(F("  rxraw          Force-read SX1262 FIFO and hex dump"));
        io->println(F("  txraw <hex>    Transmit raw hex bytes (no header, no CSMA)"));
        io->println(F("  pktdump [on|off] Toggle verbose hex dump of every RX packet"));
        io->println(F("  help           This message"));
    }

    // ── Utilities ─────────────────────────────────────────
    void printHex(const uint8_t* data, int len) {
        for (int i = 0; i < len; i++) {
            if (data[i] < 0x10) io->print('0');
            io->print(data[i], HEX);
        }
    }

#ifndef NATIVE_TEST
    uint32_t freeMemory() {
        uintptr_t heapEnd = (uintptr_t)sbrk(0);
        uintptr_t stackPtr = (uintptr_t)__get_MSP();
        if (stackPtr <= heapEnd) return 0;
        return (uint32_t)(stackPtr - heapEnd);
    }
#endif
};
