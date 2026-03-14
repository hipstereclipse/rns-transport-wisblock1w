/**
 * @file RNSRadio.h
 * @brief SX1262 radio abstraction for WisBlock 1W (RAK13302).
 *
 * Wraps RadioLib's SX1262 driver with correct pin assignments,
 * TCXO / DCDC / DIO2 antenna-switch configuration, interrupt-driven RX,
 * and CSMA/CA using Channel Activity Detection before each TX.
 *
 * SAFETY: The ISR only sets a volatile flag — all processing is in poll().
 */
#pragma once
#include "RNSConfig.h"
#include "RNSTransport.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifndef NATIVE_TEST
#include <RadioLib.h>
#include <SPI.h>
#include <nrf_gpio.h>
#endif

class RNSRadio {
public:
#ifndef NATIVE_TEST
    /// RadioLib SX1262 with explicit pin assignments (DIO1=NC — P15 not connected)
    SX1262 lora = new Module(PIN_LORA_NSS, RADIOLIB_NC,
                              PIN_LORA_RESET, PIN_LORA_BUSY);
#endif

    RNSTransport* transport = nullptr;
    Stream*       dumpStream = nullptr;  // non-null → hex-dump every RX packet

    volatile bool rxFlag   = false;
    bool          txActive = false;
    bool          hwReady  = false;  // true only after successful begin()
    bool          pollMode = false;  // true when DIO1 ISR unavailable
    float         lastRSSI = 0.0f;
    float         lastSNR  = 0.0f;
    int           lastInitState = -1; // RadioLib error code from begin()
    uint8_t       initAttempts  = 0;

    // Runtime-adjustable parameters (shadows of hardware state)
    float curFreqMHz = LORA_FREQ_MHZ;
    float curBwKHz   = LORA_BW_KHZ;
    uint8_t curSF    = LORA_SF;
    uint8_t curCR    = LORA_CR;
    int8_t  curTxDbm = LORA_TX_DBM;
    uint8_t curSyncWord = LORA_SYNC_WORD;
    uint16_t curPreamble = LORA_PREAMBLE;
    uint8_t rnodeSeq = 0;

    // ── DIO1 ISR (minimal — just set flag) ────────────────
    static RNSRadio* instance;
#ifndef NATIVE_TEST
    static void IRAM_ATTR dio1ISR() {
        if (instance) instance->rxFlag = true;
    }
#endif

    // ── Initialize the radio hardware ─────────────────────
    /**
     * @brief Power up RAK13302 module and configure SX1262.
     * @param txp  Pointer to the transport engine for packet delivery.
     * @return true on success, false on hardware failure.
     *
     * SAFETY: If this returns false, the caller should enter a
     * visible error state (LED blink) but NOT brick — the bootloader
     * remains intact and the device can be re-flashed.
     */
    bool begin(RNSTransport* txp) {
        transport = txp;
        instance  = this;
        hwReady   = false;
        rnodeSeq  = (uint8_t)random(0, 16);

#ifndef NATIVE_TEST
        // ── SPI FIX ──────────────────────────────────────────────
        // The nrf52840_dk_adafruit board variant maps the default SPI
        // to the DK's pins (P1.13/P1.14/P1.15), but the RAK4631
        // WisBlock routes SPI to P0.03/P0.29/P0.30.
        static SPIClass rakSPI(NRF_SPIM3, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
        rakSPI.begin();
        static ArduinoHal rakHal(rakSPI);

        Serial.println(F("[DIAG] SPI remapped to RAK4631 pins (SPIM3)"));
        Serial.print(F("[DIAG]   MOSI=")); Serial.print(PIN_LORA_MOSI);
        Serial.print(F(" MISO=")); Serial.print(PIN_LORA_MISO);
        Serial.print(F(" SCK=")); Serial.println(PIN_LORA_SCK);

        pinMode(PIN_LORA_NSS, OUTPUT);
        digitalWrite(PIN_LORA_NSS, HIGH);

        // ── SPIM3 register verification ──────────────────────────
        Serial.print(F("[DIAG] SPIM3 PSEL: SCK="));
        Serial.print(NRF_SPIM3->PSEL.SCK & 0x3F);
        Serial.print(F(" MOSI="));
        Serial.print(NRF_SPIM3->PSEL.MOSI & 0x3F);
        Serial.print(F(" MISO="));
        Serial.println(NRF_SPIM3->PSEL.MISO & 0x3F);
        uint32_t sckDisc  = (NRF_SPIM3->PSEL.SCK >> 31) & 1;
        uint32_t mosiDisc = (NRF_SPIM3->PSEL.MOSI >> 31) & 1;
        uint32_t misoDisc = (NRF_SPIM3->PSEL.MISO >> 31) & 1;
        if (sckDisc || mosiDisc || misoDisc) {
            Serial.println(F("[DIAG]   WARNING: One or more SPI pins DISCONNECTED in PSEL!"));
        }

        // ══════════════════════════════════════════════════════════
        // PHASE A: P34 (WB_IO2) power-gate test
        // ══════════════════════════════════════════════════════════
        // DeveloperGuide says P34=3V3_S enable. Test: drive LOW,
        // check if SPI goes dead (0xFF). If so, it IS a power gate.
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE A: P34 power-gate test ═══"));

        // Helper: raw GetStatus (no BUSY check, fixed delay)
        auto rawGetStatus = [&]() -> uint8_t {
            delay(1);
            rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);
            rakSPI.transfer(0xC0);
            uint8_t st = rakSPI.transfer(0x00);
            digitalWrite(PIN_LORA_NSS, HIGH);
            rakSPI.endTransaction();
            return st;
        };
        auto rawReadReg = [&](uint16_t addr) -> uint8_t {
            delay(1);
            rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);
            rakSPI.transfer(0x1D);
            rakSPI.transfer((addr >> 8) & 0xFF);
            rakSPI.transfer(addr & 0xFF);
            rakSPI.transfer(0x00);
            uint8_t val = rakSPI.transfer(0x00);
            digitalWrite(PIN_LORA_NSS, HIGH);
            rakSPI.endTransaction();
            return val;
        };

        // A1: baseline with P34 as INPUT (current state)
        pinMode(PIN_LORA_ENABLE, INPUT);
        delay(10);
        uint8_t baselineStatus = rawGetStatus();
        uint8_t baselineReg = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   A1 P34=INPUT:  GetStatus=0x")); Serial.print(baselineStatus, HEX);
        Serial.print(F("  reg0x0320=0x")); Serial.print(baselineReg, HEX);
        Serial.print(F("  P34=")); Serial.println(digitalRead(PIN_LORA_ENABLE));

        // A2: drive P34 HIGH explicitly
        pinMode(PIN_LORA_ENABLE, OUTPUT);
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(50);
        uint8_t hiStatus = rawGetStatus();
        uint8_t hiReg = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   A2 P34=HIGH:   GetStatus=0x")); Serial.print(hiStatus, HEX);
        Serial.print(F("  reg0x0320=0x")); Serial.println(hiReg, HEX);

        // A3: drive P34 LOW — if this is a power gate, SPI should die
        digitalWrite(PIN_LORA_ENABLE, LOW);
        delay(200);
        uint8_t loStatus = rawGetStatus();
        uint8_t loReg = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   A3 P34=LOW:    GetStatus=0x")); Serial.print(loStatus, HEX);
        Serial.print(F("  reg0x0320=0x")); Serial.println(loReg, HEX);

        bool p34IsPowerGate = (loStatus == 0xFF || loStatus == 0x00);
        if (p34IsPowerGate) {
            Serial.println(F("[DIAG]   >>> P34 IS a power gate! SPI died when LOW."));
        } else {
            Serial.println(F("[DIAG]   >>> P34 is NOT a power gate (SPI still alive)."));
        }

        // A4: restore P34 HIGH, wait for POR
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(500);
        uint8_t postCycleStatus = rawGetStatus();
        uint8_t postCycleReg = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   A4 P34=HIGH (post-cycle): GetStatus=0x")); Serial.print(postCycleStatus, HEX);
        Serial.print(F("  reg0x0320=0x")); Serial.println(postCycleReg, HEX);
        uint8_t postCycleMode = (postCycleStatus >> 4) & 0x07;
        Serial.print(F("[DIAG]   A4 ChipMode=")); Serial.println(postCycleMode);

        // ══════════════════════════════════════════════════════════
        // PHASE B: NRST brute-force discovery
        // ══════════════════════════════════════════════════════════
        // For each WB_IO candidate, toggle LOW→HIGH and check if
        // GetStatus changes from RX (mode 5) to something else.
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE B: NRST brute-force discovery ═══"));

        // Ensure power is on and chip is in known stuck state
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(100);
        uint8_t preRstStatus = rawGetStatus();
        Serial.print(F("[DIAG]   Pre-RST baseline: GetStatus=0x"));
        Serial.println(preRstStatus, HEX);

        // Candidate pins: WB_IO1(17), WB_IO3(21), WB_IO4(4),
        //                 WB_IO5(9), WB_IO6(10), plus P38(P1.06)
        static const uint8_t rstCandidates[] = {17, 21, 4, 9, 10, 38, 11, 12, 14, 16};
        static const int nRstCand = sizeof(rstCandidates) / sizeof(rstCandidates[0]);
        int discoveredRst = -1;

        for (int i = 0; i < nRstCand; i++) {
            uint8_t p = rstCandidates[i];
            // Don't toggle SPI pins or the power gate
            if (p == 3 || p == 26 || p == 29 || p == 30) continue;

            // Save current state
            uint8_t origState = digitalRead(p);

            // Toggle: LOW for 10ms, then HIGH, wait 20ms
            pinMode(p, OUTPUT);
            digitalWrite(p, LOW);
            delay(10);
            digitalWrite(p, HIGH);
            delay(20);

            // Check GetStatus
            uint8_t st = rawGetStatus();
            uint8_t mode = (st >> 4) & 0x07;

            Serial.print(F("[DIAG]   RST P")); Serial.print(p);
            Serial.print(F(": GetStatus=0x")); Serial.print(st, HEX);
            Serial.print(F(" ChipMode=")); Serial.print(mode);

            bool changed = (st != preRstStatus);
            if (changed) {
                Serial.print(F("  ← CHANGED! (was 0x"));
                Serial.print(preRstStatus, HEX);
                Serial.print(F(")"));
                if (mode == 2) {
                    Serial.print(F(" → STDBY_RC — THIS IS NRST!"));
                    discoveredRst = p;
                }
            }
            Serial.println();

            // First pin to cause STBY_RC is the real NRST; stop here
            // to avoid cascading false positives (chip stays in STBY_RC
            // after reset, making all subsequent pins look like NRST).
            if (discoveredRst >= 0) break;

            // Restore pin as input
            pinMode(p, INPUT);
        }

        if (discoveredRst >= 0) {
            Serial.print(F("[DIAG]   >>> Discovered NRST on P"));
            Serial.println(discoveredRst);
        } else {
            Serial.println(F("[DIAG]   >>> No NRST found via brute-force."));
        }

        // ══════════════════════════════════════════════════════════
        // PHASE C: BUSY pin discovery (fast GPIO sample after reset)
        // ══════════════════════════════════════════════════════════
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE C: BUSY discovery (timing-based) ═══"));

        // First, do a proper reset (use discovered RST or P17)
        int rstPin = (discoveredRst >= 0) ? discoveredRst : 17;
        // If P34 is a power gate, cycle the power too
        if (p34IsPowerGate) {
            digitalWrite(PIN_LORA_ENABLE, LOW);
            delay(100);
            digitalWrite(PIN_LORA_ENABLE, HIGH);
            delay(2); // Tiny delay — want to catch BUSY HIGH
        }
        pinMode(rstPin, OUTPUT);
        digitalWrite(rstPin, LOW);
        delay(5);
        digitalWrite(rstPin, HIGH);

        // Rapidly sample ALL 48 GPIOs every 200µs for 50ms
        // looking for HIGH→LOW transition (BUSY pattern)
        static const uint8_t spiPins[] = {3, 26, 29, 30};
        auto isSpiPin = [&](uint8_t p) {
            for (uint8_t s : spiPins) { if (p == s) return true; }
            return false;
        };
        // Sample 250 times × 200µs = 50ms
        // Store first/last HIGH time for each pin
        uint32_t firstHigh[48] = {0};
        uint32_t lastHigh[48] = {0};
        bool     sawLow[48] = {false};
        bool     sawHigh[48] = {false};

        uint32_t t0us = micros();
        for (int s = 0; s < 250; s++) {
            uint32_t nowUs = micros() - t0us;
            for (uint8_t p = 0; p < 48; p++) {
                if (isSpiPin(p)) continue;
                if (digitalRead(p) == HIGH) {
                    if (!sawHigh[p]) firstHigh[p] = nowUs;
                    lastHigh[p] = nowUs;
                    sawHigh[p] = true;
                } else {
                    sawLow[p] = true;
                }
            }
            delayMicroseconds(200);
        }
        uint32_t totalUs = micros() - t0us;

        // BUSY signature: goes HIGH briefly (during POR), then LOW
        Serial.print(F("[DIAG]   Sampled for ")); Serial.print(totalUs / 1000);
        Serial.println(F("ms after RST toggle"));
        Serial.println(F("[DIAG]   Pins with HIGH→LOW transition:"));
        int discoveredBusy = -1;
        for (uint8_t p = 0; p < 48; p++) {
            if (isSpiPin(p)) continue;
            if (sawHigh[p] && sawLow[p]) {
                Serial.print(F("[DIAG]     P")); Serial.print(p);
                Serial.print(F(": firstHIGH=")); Serial.print(firstHigh[p]);
                Serial.print(F("us  lastHIGH=")); Serial.print(lastHigh[p]);
                Serial.println(F("us"));
                // BUSY typically goes HIGH for 1-5ms after RST, then LOW
                if (lastHigh[p] < 30000) { // went LOW within 30ms
                    Serial.print(F("[DIAG]     → BUSY candidate (went LOW after "));
                    Serial.print(lastHigh[p] / 1000);
                    Serial.println(F("ms)"));
                    if (discoveredBusy < 0) discoveredBusy = p;
                }
            }
        }
        if (discoveredBusy >= 0) {
            Serial.print(F("[DIAG]   >>> Discovered BUSY on P"));
            Serial.println(discoveredBusy);
        } else {
            Serial.println(F("[DIAG]   >>> No HIGH→LOW transition found."));
            Serial.println(F("[DIAG]   All-HIGH pins: "));
            for (uint8_t p = 0; p < 48; p++) {
                if (isSpiPin(p)) continue;
                if (sawHigh[p] && !sawLow[p]) {
                    Serial.print(F("P")); Serial.print(p); Serial.print(' ');
                }
            }
            Serial.println();
        }

        // ══════════════════════════════════════════════════════════
        // PHASE D: Bitbang SPI test
        // ══════════════════════════════════════════════════════════
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE D: Bitbang SPI test ═══"));
        {
            // Use software GPIO to read register 0x0320
            // This rules out SPIM3 peripheral misconfiguration.
            // Temporarily end hardware SPI so we can control pins manually
            rakSPI.end();
            delay(5);

            uint8_t pinMOSI = PIN_LORA_MOSI, pinMISO = PIN_LORA_MISO;
            uint8_t pinSCK = PIN_LORA_SCK, pinNSS = PIN_LORA_NSS;
            pinMode(pinMOSI, OUTPUT);
            pinMode(pinMISO, INPUT);
            pinMode(pinSCK,  OUTPUT);
            pinMode(pinNSS,  OUTPUT);
            digitalWrite(pinSCK, LOW);   // CPOL=0
            digitalWrite(pinNSS, HIGH);

            // Bitbang one SPI byte (Mode 0: data on rising edge)
            auto bbTransfer = [&](uint8_t txByte) -> uint8_t {
                uint8_t rxByte = 0;
                for (int bit = 7; bit >= 0; bit--) {
                    // Set MOSI
                    digitalWrite(pinMOSI, (txByte >> bit) & 1);
                    delayMicroseconds(2);
                    // Rising edge — slave samples MOSI, we sample MISO
                    digitalWrite(pinSCK, HIGH);
                    delayMicroseconds(2);
                    rxByte |= (digitalRead(pinMISO) << bit);
                    // Falling edge
                    digitalWrite(pinSCK, LOW);
                    delayMicroseconds(2);
                }
                return rxByte;
            };

            // Bitbang GetStatus
            digitalWrite(pinNSS, LOW);
            delayMicroseconds(200);
            uint8_t bb_gs0 = bbTransfer(0xC0);
            uint8_t bb_gs1 = bbTransfer(0x00);
            digitalWrite(pinNSS, HIGH);
            delayMicroseconds(100);

            Serial.print(F("[DIAG]   Bitbang GetStatus: 0x"));
            Serial.print(bb_gs0, HEX);
            Serial.print(F(" 0x"));
            Serial.println(bb_gs1, HEX);

            // Bitbang ReadRegister(0x0320)
            digitalWrite(pinNSS, LOW);
            delayMicroseconds(200);
            bbTransfer(0x1D);
            bbTransfer(0x03);
            bbTransfer(0x20);
            bbTransfer(0x00);
            uint8_t bb_reg = bbTransfer(0x00);
            digitalWrite(pinNSS, HIGH);

            Serial.print(F("[DIAG]   Bitbang reg 0x0320: 0x"));
            Serial.println(bb_reg, HEX);

            bool bbMatch = (bb_gs1 == baselineStatus && bb_reg == baselineReg);
            if (bbMatch)
                Serial.println(F("[DIAG]   Bitbang matches HW SPI → SPI peripheral OK"));
            else
                Serial.println(F("[DIAG]   Bitbang DIFFERS from HW → SPI peripheral issue!"));

            // Restart hardware SPI
            rakSPI.begin();
            pinMode(PIN_LORA_NSS, OUTPUT);
            digitalWrite(PIN_LORA_NSS, HIGH);
        }

        // ══════════════════════════════════════════════════════════
        // PHASE E: SPI mode sweep
        // ══════════════════════════════════════════════════════════
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE E: SPI mode sweep ═══"));
        for (int mode = 0; mode < 4; mode++) {
            uint8_t spiMode;
            switch(mode) {
                case 0: spiMode = SPI_MODE0; break;
                case 1: spiMode = SPI_MODE1; break;
                case 2: spiMode = SPI_MODE2; break;
                default: spiMode = SPI_MODE3; break;
            }
            delay(2);
            rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, spiMode));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);
            rakSPI.transfer(0xC0);
            uint8_t st = rakSPI.transfer(0x00);
            digitalWrite(PIN_LORA_NSS, HIGH);
            rakSPI.endTransaction();
            Serial.print(F("[DIAG]   Mode ")); Serial.print(mode);
            Serial.print(F(": GetStatus=0x")); Serial.println(st, HEX);
        }

        // ══════════════════════════════════════════════════════════
        // PHASE F: Apply discoveries and attempt init
        // ══════════════════════════════════════════════════════════
        Serial.println();
        Serial.println(F("[DIAG] ═══ PHASE F: Init attempt with discoveries ═══"));

        // Determine effective pins
        int effBusy  = (discoveredBusy >= 0)  ? discoveredBusy  : PIN_LORA_BUSY;
        int effRst   = (discoveredRst >= 0)   ? discoveredRst   : PIN_LORA_RESET;

        Serial.print(F("[DIAG]   Effective: BUSY=P")); Serial.print(effBusy);
        Serial.print(F("  RST=P")); Serial.print(effRst);
        Serial.println(F("  DIO1=NC"));

        // Ensure power is on
        if (p34IsPowerGate) {
            pinMode(PIN_LORA_ENABLE, OUTPUT);
            digitalWrite(PIN_LORA_ENABLE, HIGH);
            delay(100);
        }

        // Configure BUSY as input
        pinMode(effBusy, INPUT);

        // Hard reset
        pinMode(effRst, OUTPUT);
        digitalWrite(effRst, LOW);
        delay(5);
        digitalWrite(effRst, HIGH);

        // Wait for BUSY LOW on the effective pin
        Serial.print(F("[DIAG]   Waiting for BUSY(P")); Serial.print(effBusy);
        Serial.print(F(") LOW... "));
        uint32_t busyWaitStart = millis();
        while (digitalRead(effBusy) == HIGH && (millis() - busyWaitStart) < 2000) {
            delay(1);
        }
        uint32_t busyWaitMs = millis() - busyWaitStart;
        Serial.print(busyWaitMs); Serial.print(F("ms, BUSY="));
        Serial.println(digitalRead(effBusy));

        Serial.println(F("[DIAG] SX1262 begin: starting init sequence (V" FW_VERSION_STRING ")"));
        Serial.print(F("[DIAG]   freq=")); Serial.print(curFreqMHz, 1);
        Serial.print(F(" bw=")); Serial.print(curBwKHz, 1);
        Serial.print(F(" sf=")); Serial.print(curSF);
        Serial.print(F(" cr=")); Serial.print(curCR);
        Serial.print(F(" tx=")); Serial.print(curTxDbm);
        Serial.print(F(" sync=0x")); Serial.print(curSyncWord, HEX);
        Serial.print(F(" pre=")); Serial.println(curPreamble);

        // Post-discovery GetStatus
        uint8_t gs0, gs1;
        rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_LORA_NSS, LOW);
        delayMicroseconds(200);
        gs0 = rakSPI.transfer(0xC0);
        gs1 = rakSPI.transfer(0x00);
        digitalWrite(PIN_LORA_NSS, HIGH);
        rakSPI.endTransaction();
        Serial.print(F("[DIAG]   GetStatus: byte0=0x")); Serial.print(gs0, HEX);
        Serial.print(F(" byte1=0x")); Serial.println(gs1, HEX);
        uint8_t chipMode = (gs1 >> 4) & 0x07;
        uint8_t cmdStat  = (gs1 >> 1) & 0x07;
        const char* modeNames[] = {"Unused","RFU","STBY_RC","STBY_XOSC","FS","RX","TX","RFU"};
        Serial.print(F("[DIAG]   ChipMode=")); Serial.print(chipMode);
        Serial.print(F(" (")); Serial.print(modeNames[chipMode & 0x07]);
        Serial.print(F(") CmdStatus=")); Serial.println(cmdStat);

        // ── BUSY-gated raw SPI helper ────────────────────────────
        auto waitBusy = [&](uint32_t timeoutMs) -> bool {
            uint32_t t0 = millis();
            while (digitalRead(effBusy) == HIGH) {
                if (millis() - t0 >= timeoutMs) return false;
                delayMicroseconds(100);
            }
            return true;
        };
        auto rawCmd = [&](const uint8_t* cmd, uint8_t len) {
            waitBusy(100);
            rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);  // chip needs ~150µs after NSS LOW
            for (uint8_t i = 0; i < len; i++) rakSPI.transfer(cmd[i]);
            digitalWrite(PIN_LORA_NSS, HIGH);
            rakSPI.endTransaction();
        };
        // rawReadReg already defined above (Phase A); now BUSY-gated via waitBusy

        // ── Raw SPI probe — verify chip is on the bus ────────────
        uint8_t regVal = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   Raw SPI reg 0x0320 = 0x"));
        Serial.println(regVal, HEX);

        if (regVal == 0x00 || regVal == 0xFF) {
            Serial.println(F("[DIAG]   SPI bus dead (0x00/0xFF)."));
            Serial.println(F("[DIAG]   → Check RAK13302 seated in IO slot"));
            Serial.println(F("[DIAG]   → Check base board provides 3V3_S"));
            lastInitState = -2;
            initAttempts = 1;
            return false;
        }
        Serial.println(F("[DIAG]   SPI OK — chip is responding"));

        // ── Raw SPI pre-init: Standby + TCXO + Calibrate ────────
        // Do this BEFORE RadioLib so the 32 MHz clock is running and
        // the chip is in a known state.  All commands are BUSY-gated.
        Serial.println(F("[DIAG]   Raw: SetStandby(STDBY_RC)"));
        uint8_t cmdStandby[] = {0x80, 0x00};
        rawCmd(cmdStandby, 2);
        delay(5);

        // SetDIO3AsTCXOCtrl(1.8V, ~5ms timeout)
        Serial.println(F("[DIAG]   Raw: SetDIO3AsTCXOCtrl(1.8V)"));
        uint8_t cmdTCXO[] = {0x97, 0x03, 0x00, 0x01, 0x40};
        rawCmd(cmdTCXO, 5);
        delay(10);

        // Calibrate(all blocks)
        Serial.println(F("[DIAG]   Raw: Calibrate(0x7F)"));
        uint8_t cmdCal[] = {0x89, 0x7F};
        rawCmd(cmdCal, 2);
        delay(50);

        // CalibrateImage for 902-928 MHz (US ISM)
        Serial.println(F("[DIAG]   Raw: CalibrateImage(902-928MHz)"));
        uint8_t cmdCalImg[] = {0x98, 0xE1, 0xE9};
        rawCmd(cmdCalImg, 3);
        delay(20);

        // Re-probe after TCXO+cal
        regVal = rawReadReg(0x0320);
        Serial.print(F("[DIAG]   Post-cal reg 0x0320 = 0x"));
        Serial.println(regVal, HEX);

        // Post-cal GetStatus
        waitBusy(100);
        rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_LORA_NSS, LOW);
        delayMicroseconds(200);
        gs0 = rakSPI.transfer(0xC0);
        gs1 = rakSPI.transfer(0x00);
        digitalWrite(PIN_LORA_NSS, HIGH);
        rakSPI.endTransaction();
        Serial.print(F("[DIAG]   Post-cal GetStatus: 0x")); Serial.print(gs0, HEX);
        Serial.print(F(" 0x")); Serial.println(gs1, HEX);

        // ── Raw SPI version string — read 16 bytes from 0x0320 ──
        {
            uint8_t verBuf[16] = {0};
            waitBusy(100);
            rakSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LORA_NSS, LOW);
            delayMicroseconds(200);
            rakSPI.transfer(0x1D);  // ReadRegister
            rakSPI.transfer(0x03);  // addr MSB
            rakSPI.transfer(0x20);  // addr LSB
            rakSPI.transfer(0x00);  // NOP (status)
            for (uint8_t i = 0; i < 16; i++) verBuf[i] = rakSPI.transfer(0x00);
            digitalWrite(PIN_LORA_NSS, HIGH);
            rakSPI.endTransaction();
            Serial.print(F("[DIAG]   Raw version string: "));
            for (uint8_t i = 0; i < 16; i++) {
                if (verBuf[i] >= 0x20 && verBuf[i] < 0x7F)
                    Serial.print((char)verBuf[i]);
                else { Serial.print(F("[0x")); Serial.print(verBuf[i], HEX); Serial.print(F("]")); }
            }
            Serial.println();
            Serial.print(F("[DIAG]   Raw version hex:    "));
            for (uint8_t i = 0; i < 16; i++) {
                if (verBuf[i] < 0x10) Serial.print('0');
                Serial.print(verBuf[i], HEX); Serial.print(' ');
            }
            Serial.println();
        }

        // ── BUSY pin state report ────────────────────────────────
        Serial.print(F("[DIAG]   eff BUSY(P")); Serial.print(effBusy);
        Serial.print(F(") state: ")); Serial.println(digitalRead(effBusy));

        // ── Cascading pin strategy for RadioLib begin() ──────────
        // Uses discovered pins first, then falls back to NC variants.
        struct PinCombo {
            int busy;
            int reset;
            int dio1;
            const char* label;
        };
        // Build label strings dynamically — use static buffers
        char lbl0[40], lbl1[40];
        snprintf(lbl0, sizeof(lbl0), "BUSY=%d/RST=%d/DIO1=NC", effBusy, effRst);
        snprintf(lbl1, sizeof(lbl1), "NC/RST=%d/DIO1=NC", effRst);

        // DIO1 must be NC: P15 is floating HIGH (not real DIO1), causing
        // RadioLib to think TX/CAD completes instantly.  With DIO1=NC,
        // RadioLib uses timeout-based waits and we poll the IRQ register.
        PinCombo combos[] = {
            { effBusy,       effRst,        RADIOLIB_NC,   lbl0 },
            { RADIOLIB_NC,   effRst,        RADIOLIB_NC,   lbl1 },
            { RADIOLIB_NC,   RADIOLIB_NC,   RADIOLIB_NC,   "NC/NC/NC (all NC)" },
        };
        const int nCombos = sizeof(combos) / sizeof(combos[0]);

        int state = RADIOLIB_ERR_UNKNOWN;
        int comboIdx = -1;
        for (int c = 0; c < nCombos; c++) {
            Serial.print(F("[DIAG]   Strategy ")); Serial.print(c + 1);
            Serial.print(F("/")); Serial.print(nCombos);
            Serial.print(F(": ")); Serial.println(combos[c].label);

            // ── Critical: re-init SPI bus before each strategy ───
            // RadioLib's SX126x::begin() calls mod->term() on failure,
            // which calls rakSPI.end(), killing the SPI peripheral.
            // We must revive it before the next attempt.
            rakSPI.begin();
            pinMode(PIN_LORA_NSS, OUTPUT);
            digitalWrite(PIN_LORA_NSS, HIGH);
            // Re-configure BUSY as INPUT (RadioLib may reconfigure pin)
            pinMode(effBusy, INPUT);

            // Hard-reset chip before each strategy
            if (p34IsPowerGate) {
                digitalWrite(PIN_LORA_ENABLE, LOW);
                delay(50);
                digitalWrite(PIN_LORA_ENABLE, HIGH);
                delay(100);
            }
            pinMode(effRst, OUTPUT);
            digitalWrite(effRst, LOW);
            delay(5);
            digitalWrite(effRst, HIGH);
            delay(10);
            waitBusy(200);

            // Re-send raw TCXO+cal — the chip was just reset
            rawCmd(cmdStandby, 2);
            delay(5);
            rawCmd(cmdTCXO, 5);
            delay(10);
            rawCmd(cmdCal, 2);
            delay(50);

            lora = SX1262(new Module(&rakHal, PIN_LORA_NSS,
                                      combos[c].dio1, combos[c].reset,
                                      combos[c].busy));

            state = lora.begin(
                curFreqMHz, curBwKHz, curSF, curCR,
                curSyncWord, curTxDbm, curPreamble, 1.8f
            );
            lastInitState = state;
            initAttempts = c + 1;

            Serial.print(F("[DIAG]     rc=")); Serial.println(state);

            if (state == RADIOLIB_ERR_NONE) {
                comboIdx = c;
                break;
            }
        }

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print(F("[DIAG] SX1262 init FAILED after all strategies, last rc="));
            Serial.println(state);
            return false;
        }

        Serial.print(F("[DIAG] SX1262 init OK with: "));
        Serial.println(combos[comboIdx].label);

        // WisBlock 1W specifics (TCXO already set via begin())
        Serial.println(F("[DIAG] SX1262 configuring DIO2/DCDC..."));
        lora.setDio2AsRfSwitch(true);
        lora.setRegulatorDCDC();
        lora.setCRC(true);
        lora.setCurrentLimit(140.0);

        // Set up RX mode — ISR-driven if DIO1 is connected, poll if NC
        if (combos[comboIdx].dio1 != RADIOLIB_NC) {
            lora.setPacketReceivedAction(dio1ISR);
            pollMode = false;
            Serial.println(F("[DIAG] RX mode: DIO1 interrupt"));
        } else {
            pollMode = true;
            Serial.println(F("[DIAG] RX mode: IRQ polling (DIO1 not connected)"));
        }
        int rxState = lora.startReceive();
        Serial.print(F("[DIAG] startReceive rc=")); Serial.println(rxState);

        hwReady = true;
        Serial.println(F("[DIAG] SX1262 init OK — radio is live"));
#else
        hwReady = true;
#endif
        return true;
    }

    // ── Poll for received packets (call from loop()) ──────
    void poll() {
#ifndef NATIVE_TEST
        if (!hwReady) return;

        // Always check SX1262 IRQ register via SPI as safety net —
        // the DIO1 ISR may not fire if the pin isn't physically connected.
        if (!rxFlag) {
            uint32_t irq = lora.getIrqFlags();
            if (irq & (1UL << RADIOLIB_IRQ_RX_DONE)) {
                rxFlag = true;
            }
        }

        if (!rxFlag) return;
        rxFlag = false;

        uint8_t buf[RNS_MTU + 1];
        int len = lora.getPacketLength();
        if (len <= 0 || len > (RNS_MTU + 1)) {
            lora.startReceive();
            return;
        }

        int state = lora.readData(buf, len);
        if (state == RADIOLIB_ERR_NONE) {
            lastRSSI = lora.getRSSI();
            lastSNR  = lora.getSNR();

            // Verbose packet hex dump when enabled via 'pktdump' cmd
            if (dumpStream) {
                dumpStream->print(F("\r\n[PKTDUMP] len="));
                dumpStream->print(len);
                dumpStream->print(F(" RSSI="));
                dumpStream->print(lastRSSI, 1);
                dumpStream->print(F(" SNR="));
                dumpStream->print(lastSNR, 1);
                dumpStream->print(F("\r\n  HEX: "));
                for (int i = 0; i < len; i++) {
                    if (buf[i] < 0x10) dumpStream->print('0');
                    dumpStream->print(buf[i], HEX);
                    if (i < len - 1) dumpStream->print(' ');
                }
                dumpStream->println();
            }

            const uint8_t* payload = buf;
            uint16_t payloadLen = (uint16_t)len;
#if RNODE_LORA_HEADER_ENABLED
            if (payloadLen <= 1) {
                lora.startReceive();
                return;
            }
            payload = &buf[1];
            payloadLen--;
#endif
            if (transport) {
                transport->lastRxRSSI = lastRSSI;
                transport->lastRxSNR  = lastSNR;
                transport->ingestPacket(payload, payloadLen);
            }
        }
        lora.startReceive();
#endif
    }

    // ── Transmit with CSMA/CA ─────────────────────────────
    /**
     * @brief Send a packet, first checking channel activity.
     * @param data  Raw bytes to transmit.
     * @param len   Byte count.
     * @return true if transmission succeeded.
     *
     * Uses up to 8 CAD attempts with random backoff before giving up.
     *
     * SAFETY: lora.scanChannel() has no internal timeout — it spins
     * while(!digitalRead(DIO1)) forever.  If the SX1262 never fires
     * a CAD-done interrupt the watchdog will trigger a hard reset.
     * We therefore use the non-blocking startChannelScan() + a
     * millis()-bounded poll so each attempt is capped at CAD_TIMEOUT_MS,
     * keeping the total worst-case time well under WDT_TIMEOUT_SEC.
     */
    bool transmit(const uint8_t* data, uint16_t len) {
        if (!data || len == 0 || len > RNS_MTU) return false;
        if (!hwReady) {
            Serial.println(F("[DIAG] TX blocked: radio not initialized"));
            return false;
        }

#ifndef NATIVE_TEST
        uint32_t txStart = millis();

        // CAD at SF8/BW125 completes in ~17 ms; 200 ms is generous.
        static const uint32_t CAD_TIMEOUT_MS = 200;

        bool channelFree = false;
        for (int attempt = 0; attempt < 8; attempt++) {
            // Ensure radio is in STBY before CAD — avoids -705 when
            // BUSY=NC leaves the chip in an indeterminate state.
            lora.standby();
            int scanRc = lora.startChannelScan();
            if (scanRc != RADIOLIB_ERR_NONE) {
                Serial.print(F("[DIAG] CAD start failed rc=")); Serial.print(scanRc);
                Serial.print(F(" attempt=")); Serial.println(attempt);
                delay(random(10, 50));
                continue;
            }
            uint32_t cadStart = millis();
            while (millis() - cadStart < CAD_TIMEOUT_MS) {
                // Check CAD done via IRQ register (works with any pin config)
                uint32_t irq = lora.getIrqFlags();
                if (irq & ((1UL << RADIOLIB_IRQ_CAD_DONE) | (1UL << RADIOLIB_IRQ_CAD_DETECTED)))
                    break;
            }
            int cad = lora.getChannelScanResult();
            if (cad == RADIOLIB_CHANNEL_FREE) {
                channelFree = true;
                break;
            }
            Serial.print(F("[DIAG] CAD busy attempt=")); Serial.print(attempt);
            Serial.print(F(" rc=")); Serial.println(cad);
            delay(random(10, 50));
        }
        if (!channelFree) {
            Serial.print(F("[DIAG] TX aborted: channel busy after 8 CAD attempts ("));
            Serial.print(millis() - txStart); Serial.println(F("ms)"));
            // Restore RX mode so the radio isn't stuck in standby
            lora.startReceive();
            return false;
        }

        uint8_t txBuf[RNS_MTU + 1];
        const uint8_t* txData = data;
        uint16_t txLen = len;
    #if RNODE_LORA_HEADER_ENABLED
        txBuf[0] = (uint8_t)(((rnodeSeq & 0x0F) << 4) | (RNODE_LORA_HEADER_FLAGS_UNSPLIT & 0x0F));
        rnodeSeq = (uint8_t)((rnodeSeq + 1) & 0x0F);
        memcpy(txBuf + 1, data, len);
        txData = txBuf;
        txLen = len + 1;
    #endif

        Serial.print(F("[DIAG] TX start: ")); Serial.print(txLen);
        Serial.print(F(" bytes, txPower=")); Serial.print(curTxDbm);
        Serial.println(F(" dBm"));

        txActive = true;

        // Non-blocking TX: RadioLib's blocking transmit() polls the DIO1
        // GPIO for TX_DONE, but DIO1 is NC so it always times out.
        // Use startTransmit() + SPI IRQ polling instead.
        int state = lora.startTransmit(txData, txLen);
        if (state != RADIOLIB_ERR_NONE) {
            txActive = false;
            Serial.print(F("[DIAG] startTransmit failed rc=")); Serial.println(state);
            lora.startReceive();
            return false;
        }

        // Poll SPI IRQ register for TX_DONE (max ~5 s for worst-case SF12)
        static const uint32_t TX_TIMEOUT_MS = 5000;
        uint32_t txWaitStart = millis();
        bool txDone = false;
        while (millis() - txWaitStart < TX_TIMEOUT_MS) {
            uint32_t irq = lora.getIrqFlags();
            if (irq & (1UL << RADIOLIB_IRQ_TX_DONE)) {
                txDone = true;
                break;
            }
            delay(1);
        }

        // Clean up TX state inside RadioLib
        state = lora.finishTransmit();
        txActive = false;

        if (!txDone) {
            state = RADIOLIB_ERR_TX_TIMEOUT;
        }

        uint32_t txElapsed = millis() - txStart;
        Serial.print(F("[DIAG] TX done: rc=")); Serial.print(state);
        Serial.print(F(" elapsed=")); Serial.print(txElapsed);
        Serial.println(F("ms"));

        int rxRc = lora.startReceive();
        if (rxRc != RADIOLIB_ERR_NONE) {
            Serial.print(F("[DIAG] startReceive after TX failed rc=")); Serial.println(rxRc);
        }

        return (state == RADIOLIB_ERR_NONE);
#else
        return true;
#endif
    }

    // ── Runtime reconfiguration ───────────────────────────
    // After any parameter change, the radio must be put back into RX.
    // RadioLib's setters internally call standby(), leaving the chip
    // in STBY_RC — no packets will be received until startReceive().
    void restartRx() {
#ifndef NATIVE_TEST
        if (hwReady) lora.startReceive();
#endif
    }

    void setFrequency(float mhz)  {
#ifndef NATIVE_TEST
        lora.setFrequency(mhz);
#endif
        curFreqMHz = mhz;
        restartRx();
    }
    void setBandwidth(float khz)  {
#ifndef NATIVE_TEST
        lora.setBandwidth(khz);
#endif
        curBwKHz = khz;
        restartRx();
    }
    void setSF(uint8_t sf) {
#ifndef NATIVE_TEST
        lora.setSpreadingFactor(sf);
#endif
        curSF = sf;
        restartRx();
    }
    void setCR(uint8_t cr) {
#ifndef NATIVE_TEST
        lora.setCodingRate(cr);
#endif
        curCR = cr;
        restartRx();
    }
    void setTxPower(int8_t dbm) {
        if (dbm > LORA_TX_DBM_MAX_SAFE) dbm = LORA_TX_DBM_MAX_SAFE;
        if (dbm < -9) dbm = -9;
#ifndef NATIVE_TEST
        lora.setOutputPower(dbm);
#endif
        curTxDbm = dbm;
        // No restartRx needed — TX power doesn't affect RX state
    }
    void setSyncWord(uint8_t syncWord) {
#ifndef NATIVE_TEST
        lora.setSyncWord(syncWord);
#endif
        curSyncWord = syncWord;
        restartRx();
    }
    void setPreamble(uint16_t preambleSymbols) {
#ifndef NATIVE_TEST
        lora.setPreambleLength(preambleSymbols);
#endif
        curPreamble = preambleSymbols;
        restartRx();
    }
};

// Static instance pointer for ISR callback
inline RNSRadio* RNSRadio::instance = nullptr;
