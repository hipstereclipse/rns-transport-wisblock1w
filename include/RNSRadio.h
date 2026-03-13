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

#define RNS_TX_ERR_INVALID_INPUT   (-9001)
#define RNS_TX_ERR_CAD_NOT_FREE    (-9002)

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
    float         lastRSSI = 0.0f;
    float         lastSNR  = 0.0f;
    int           lastTxState = 0;
    uint32_t      lastTxDurationMs = 0;

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
        rnodeSeq  = (uint8_t)random(0, 16);

#ifndef NATIVE_TEST
        // Power-cycle 3V3_S rail for a clean SX1262 start
        pinMode(PIN_LORA_ENABLE, OUTPUT);
        digitalWrite(PIN_LORA_ENABLE, LOW);
        delay(40);
        digitalWrite(PIN_LORA_ENABLE, HIGH);
        delay(180);

        int state = RADIOLIB_ERR_UNKNOWN;
        for (int attempt = 0; attempt < 2; attempt++) {
            state = lora.begin(
                curFreqMHz, curBwKHz, curSF, curCR,
                curSyncWord, curTxDbm, curPreamble, 0
            );
            if (state == RADIOLIB_ERR_NONE) break;

            // Retry once after another short power-cycle.
            digitalWrite(PIN_LORA_ENABLE, LOW);
            delay(80);
            digitalWrite(PIN_LORA_ENABLE, HIGH);
            delay(200);
        }
        if (state != RADIOLIB_ERR_NONE) return false;

        // WisBlock 1W specifics
        lora.setDio2AsRfSwitch(true);
        lora.setTCXO(1.8);
        lora.setRegulatorDCDC();
        lora.setCRC(true);
        lora.setCurrentLimit(140.0);

        // Attach DIO1 interrupt for RX-complete
        lora.setPacketReceivedAction(dio1ISR);
        lora.startReceive();
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
        if (!data || len == 0 || len > RNS_MTU) {
            lastTxState = RNS_TX_ERR_INVALID_INPUT;
            lastTxDurationMs = 0;
            return false;
        }

#ifndef NATIVE_TEST
        const bool dbgAnnounce = ((data[0] & 0x03) == ANNOUNCE);
        const uint32_t txStartMs = millis();
        if (dbgAnnounce) {
            Serial.print(F("[ANNDBG] RADIO TX enter len="));
            Serial.print(len);
            Serial.print(F(" sf=")); Serial.print(curSF);
            Serial.print(F(" bw=")); Serial.print(curBwKHz, 1);
            Serial.print(F(" tx=")); Serial.println(curTxDbm);
        }

        // CAD at SF8/BW125 completes in ~17 ms; 200 ms is generous.
        static const uint32_t CAD_TIMEOUT_MS = 200;

        // Announce path: skip CAD to avoid repeated startChannelScan failures
        // observed on some boots, which can trigger watchdog/power resets.
        bool channelFree = dbgAnnounce;

        if (dbgAnnounce) {
            Serial.println(F("[ANNDBG] RADIO CAD bypass for ANNOUNCE"));
        }

        if (!channelFree) {
            for (int attempt = 0; attempt < 4; attempt++) {
                if (lora.startChannelScan() != RADIOLIB_ERR_NONE) {
                    lora.startReceive();
                    delay(15);
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
                delay(random(10, 35));
            }
            if (!channelFree) {
                lastTxState = RNS_TX_ERR_CAD_NOT_FREE;
                lastTxDurationMs = millis() - txStartMs;
                return false;
            }
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

        txActive = true;
        int state = lora.transmit(txData, txLen);
        txActive = false;

        if (dbgAnnounce && state != RADIOLIB_ERR_NONE) {
            Serial.print(F("[ANNDBG] RADIO TX recovery retry, firstState="));
            Serial.println(state);
            lora.standby();
            delay(12);
            lora.startReceive();
            delay(20);

            txActive = true;
            state = lora.transmit(txData, txLen);
            txActive = false;

            Serial.print(F("[ANNDBG] RADIO TX retry state="));
            Serial.println(state);
        }

        if (dbgAnnounce) {
            Serial.print(F("[ANNDBG] RADIO TX state="));
            Serial.print(state);
            Serial.print(F(" txLen="));
            Serial.print(txLen);
            Serial.print(F(" dtMs="));
            Serial.println(millis() - txStartMs);
        }

        lora.startReceive();
        lastTxState = state;
        lastTxDurationMs = millis() - txStartMs;
        return (state == RADIOLIB_ERR_NONE);
#else
        lastTxState = 0;
        lastTxDurationMs = 0;
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
