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

// Forward declaration for persistence (optional)
class RNSPersistence;

class RNSConsole {
public:
    RNSTransport*   transport   = nullptr;
    RNSRadio*       radio       = nullptr;
    RNSIdentity*    identity    = nullptr;
    RNSPersistence* persistence = nullptr;
    Stream*         io          = nullptr;

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
        io->println(F("║  RNS Transport Node — WisBlock 1W  v" FW_VERSION_STRING "  ║"));
        io->println(F("║  Reticulum LoRa Transport @ +30 dBm       ║"));
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
        else if (strcmp(command, "identity")  == 0) cmdIdentity();
        else if (strcmp(command, "stats")     == 0) cmdStatus();
        else if (strcmp(command, "radio")     == 0) cmdRadio();
        else if (strcmp(command, "set")       == 0) cmdSet(args);
        else if (strcmp(command, "save")      == 0) cmdSave();
        else if (strcmp(command, "factory-reset") == 0) cmdFactoryReset();
        else if (strcmp(command, "dfu")       == 0) cmdDfu();
        else if (strcmp(command, "reboot")    == 0) cmdReboot();
        else if (strcmp(command, "version")   == 0) cmdVersion();
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
        io->print(F("  Firmware:   ")); io->println(F(FW_VERSION_STRING));
#ifndef NATIVE_TEST
        io->print(F("  Uptime:     ")); io->print(millis() / 1000); io->println(F(" s"));
#endif
        io->print(F("  RX packets: ")); io->println(s.rxPackets);
        io->print(F("  TX packets: ")); io->println(s.txPackets);
        io->print(F("  Forwarded:  ")); io->println(s.fwdPackets);
        io->print(F("  Announces:  ")); io->println(s.announces);
        io->print(F("  Duplicates: ")); io->println(s.duplicates);
        io->print(F("  Invalid:    ")); io->println(s.invalidPackets);
        io->print(F("  Paths:      ")); io->println(s.pathEntries);
#ifndef NATIVE_TEST
        io->print(F("  Free RAM:   ")); io->print(freeMemory()); io->println(F(" bytes"));
#endif
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
        io->print(F("  Frequency: ")); io->print(radio->curFreqMHz, 1); io->println(F(" MHz"));
        io->print(F("  Bandwidth: ")); io->print(radio->curBwKHz, 1); io->println(F(" kHz"));
        io->print(F("  SF:        ")); io->println(radio->curSF);
        io->print(F("  CR:        4/")); io->println(radio->curCR);
        io->print(F("  TX Power:  ")); io->print(radio->curTxDbm);
                                        io->println(F(" dBm (SX1262 → +8 dB PA)"));
        io->print(F("  Last RSSI: ")); io->print(radio->lastRSSI); io->println(F(" dBm"));
        io->print(F("  Last SNR:  ")); io->print(radio->lastSNR); io->println(F(" dB"));
    }

    // ── set <param> <value> ───────────────────────────────
    void cmdSet(const char* args) {
        char param[16] = {0};
        char value[16] = {0};
        if (sscanf(args, "%15s %15s", param, value) < 2) {
            io->println(F("Usage: set <freq|sf|bw|cr|txpower> <value>"));
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
            if (dbm < -9 || dbm > 22) { io->println(F("TX power must be -9 to 22 dBm")); return; }
            radio->setTxPower((int8_t)dbm);
            io->print(F("TX → ")); io->print(dbm); io->println(F(" dBm"));
        } else {
            io->println(F("Unknown param. Use: freq, sf, bw, cr, txpower"));
        }
    }

    // ── save (persist current config to flash) ────────────
    void cmdSave();   // implemented in main.cpp (needs persistence ref)

    // ── factory-reset ─────────────────────────────────────
    void cmdFactoryReset();  // implemented in main.cpp

    // ── dfu: reboot into Adafruit bootloader for safe flashing ─
    void cmdDfu() {
        io->println(F("Entering DFU bootloader mode..."));
        io->println(F("Device will appear as USB drive for UF2 flashing."));
        io->flush();
#ifndef NATIVE_TEST
        delay(500);
        // The Adafruit nRF52 bootloader enters DFU on double-reset,
        // but we can also trigger it by writing a magic value to
        // the GPREGRET register and resetting.
        NRF_POWER->GPREGRET = 0x57;  // 'W' — DFU magic for Adafruit bootloader
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

    // ── version ───────────────────────────────────────────
    void cmdVersion() {
        io->print(F("RNS Transport Node v")); io->println(F(FW_VERSION_STRING));
        io->print(F("Build: ")); io->println(F(FW_BUILD_TAG));
        io->print(F("Board: WisBlock 1W (RAK3401 + RAK13302)"));
        io->println();
    }

    // ── help ──────────────────────────────────────────────
    void cmdHelp() {
        io->println(F("── Available Commands ──"));
        io->println(F("  status         Node status and counters"));
        io->println(F("  routes         Show routing table"));
        io->println(F("  identity       Display node identity hash + keys"));
        io->println(F("  radio          Current radio configuration"));
        io->println(F("  set <p> <v>    Set radio param (freq/sf/bw/cr/txpower)"));
        io->println(F("  save           Persist config to flash"));
        io->println(F("  factory-reset  Erase all persisted data"));
        io->println(F("  dfu            Reboot into bootloader (USB flashing)"));
        io->println(F("  reboot         Restart node"));
        io->println(F("  version        Firmware version"));
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
        return 0;
    }
#endif
};
