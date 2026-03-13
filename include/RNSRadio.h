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
#endif

class RNSRadio {
public:
#ifndef NATIVE_TEST
    /// RadioLib SX1262 with explicit pin assignments
    SX1262 lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1,
                              PIN_LORA_RESET, PIN_LORA_BUSY);
#endif

    RNSTransport* transport = nullptr;

    volatile bool rxFlag   = false;
    bool          txActive = false;
    bool          hwReady  = false;  // true only after successful begin()
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
        // Power-cycle 3V3_S rail for a clean SX1262 start
        pinMode(PIN_LORA_ENABLE, OUTPUT);
        digitalWrite(PIN_LORA_ENABLE, LOW);
        delay(40);
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(180);

        Serial.println(F("[DIAG] SX1262 begin: starting init sequence"));
        Serial.print(F("[DIAG]   NSS=")); Serial.print(PIN_LORA_NSS);
        Serial.print(F(" DIO1=")); Serial.print(PIN_LORA_DIO1);
        Serial.print(F(" RST=")); Serial.print(PIN_LORA_RESET);
        Serial.print(F(" BUSY=")); Serial.print(PIN_LORA_BUSY);
        Serial.print(F(" EN=")); Serial.println(PIN_LORA_ENABLE);
        Serial.print(F("[DIAG]   freq=")); Serial.print(curFreqMHz, 1);
        Serial.print(F(" bw=")); Serial.print(curBwKHz, 1);
        Serial.print(F(" sf=")); Serial.print(curSF);
        Serial.print(F(" cr=")); Serial.print(curCR);
        Serial.print(F(" tx=")); Serial.print(curTxDbm);
        Serial.print(F(" sync=0x")); Serial.print(curSyncWord, HEX);
        Serial.print(F(" pre=")); Serial.println(curPreamble);

        int state = RADIOLIB_ERR_UNKNOWN;
        for (int attempt = 0; attempt < 3; attempt++) {
            initAttempts = attempt + 1;
            Serial.print(F("[DIAG]   attempt ")); Serial.print(attempt + 1);
            Serial.print(F("/3 ... "));

            state = lora.begin(
                curFreqMHz, curBwKHz, curSF, curCR,
                curSyncWord, curTxDbm, curPreamble, 0
            );
            lastInitState = state;

            Serial.print(F("rc=")); Serial.println(state);

            if (state == RADIOLIB_ERR_NONE) break;

            // Retry after power-cycle with increasing delays.
            Serial.println(F("[DIAG]   power-cycling SX1262..."));
            digitalWrite(PIN_LORA_ENABLE, LOW);
            delay(80 + attempt * 60);
            digitalWrite(PIN_LORA_ENABLE, HIGH);
            delay(200 + attempt * 100);
        }
        if (state != RADIOLIB_ERR_NONE) {
            Serial.print(F("[DIAG] SX1262 init FAILED after "));
            Serial.print(initAttempts);
            Serial.print(F(" attempts, last rc="));
            Serial.println(state);
            return false;
        }

        // WisBlock 1W specifics
        Serial.println(F("[DIAG] SX1262 configuring DIO2/TCXO/DCDC..."));
        lora.setDio2AsRfSwitch(true);
        lora.setTCXO(1.8);
        lora.setRegulatorDCDC();
        lora.setCRC(true);
        lora.setCurrentLimit(140.0);

        // Attach DIO1 interrupt for RX-complete
        lora.setPacketReceivedAction(dio1ISR);
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
        if (!rxFlag) return;
        rxFlag = false;

#ifndef NATIVE_TEST
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
            if (transport) transport->ingestPacket(payload, payloadLen);
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
            int scanRc = lora.startChannelScan();
            if (scanRc != RADIOLIB_ERR_NONE) {
                Serial.print(F("[DIAG] CAD start failed rc=")); Serial.print(scanRc);
                Serial.print(F(" attempt=")); Serial.println(attempt);
                delay(random(10, 50));
                continue;
            }
            uint32_t cadStart = millis();
            while (!digitalRead(PIN_LORA_DIO1)) {
                if (millis() - cadStart >= CAD_TIMEOUT_MS) break;
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
        int state = lora.transmit(txData, txLen);
        txActive = false;

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
    void setFrequency(float mhz)  {
#ifndef NATIVE_TEST
        lora.setFrequency(mhz);
#endif
        curFreqMHz = mhz;
    }
    void setBandwidth(float khz)  {
#ifndef NATIVE_TEST
        lora.setBandwidth(khz);
#endif
        curBwKHz = khz;
    }
    void setSF(uint8_t sf) {
#ifndef NATIVE_TEST
        lora.setSpreadingFactor(sf);
#endif
        curSF = sf;
    }
    void setCR(uint8_t cr) {
#ifndef NATIVE_TEST
        lora.setCodingRate(cr);
#endif
        curCR = cr;
    }
    void setTxPower(int8_t dbm) {
        if (dbm > LORA_TX_DBM_MAX_SAFE) dbm = LORA_TX_DBM_MAX_SAFE;
        if (dbm < -9) dbm = -9;
#ifndef NATIVE_TEST
        lora.setOutputPower(dbm);
#endif
        curTxDbm = dbm;
    }
    void setSyncWord(uint8_t syncWord) {
#ifndef NATIVE_TEST
        lora.setSyncWord(syncWord);
#endif
        curSyncWord = syncWord;
    }
    void setPreamble(uint16_t preambleSymbols) {
#ifndef NATIVE_TEST
        lora.setPreambleLength(preambleSymbols);
#endif
        curPreamble = preambleSymbols;
    }
};

// Static instance pointer for ISR callback
inline RNSRadio* RNSRadio::instance = nullptr;
