/**
 * @file RNSDisplay.h
 * @brief Status dashboard on the Heltec T096's 0.96" ST7735 TFT (160x80).
 *
 * Driver: Adafruit_ST7735 (mini 160x80 "plugin" init: inverted, BGR,
 * column offset 26 / row offset 1) on a second nRF52 SPI master. This
 * is the same stack RNode firmware uses for Heltec TFTs; the page and
 * timeout structure follows the rns_gateway StatusScreen.
 *
 * Pages (short press cycles, any press wakes a blanked panel):
 *   1 STATUS  name, battery, uptime, RAM, packet + byte counters,
 *             paths, last RSSI/SNR, one-line radio summary
 *   2 RADIO   full radio parameters and hardware state
 *   3 PEERS   first seven path-table entries: hash, name, hops, RSSI
 *
 * Sleep: after `timeoutSec` (0 = never) the panel is put to sleep, the
 * backlight PMOS is switched off and the Vext rail is dropped, so a
 * sleeping panel costs nothing. Wake re-powers Vext and re-runs the
 * panel init (~150 ms, done once per wake, never in the RX path).
 *
 * Rendering: text rows are cached; only rows whose text changed are
 * redrawn (fillRect + transparent text), so the 1 Hz refresh normally
 * touches one or two rows and never blocks the radio for long. The
 * radio's RX is interrupt-driven and its packet sits in the SX1262
 * buffer until poll() reads it, so a redraw cannot lose a packet.
 *
 * Compiled only when HAS_DISPLAY is 1 (RNSConfig.h, per board).
 */
#pragma once
#include "RNSConfig.h"

#if HAS_DISPLAY && !defined(NATIVE_TEST)
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "RNSTransport.h"
#include "RNSRadio.h"
#include "RNSVext.h"

extern "C" char* sbrk(int incr);

class RNSDisplay {
public:
    static const uint8_t PAGE_COUNT = 3;
    static const uint8_t COLS = 26;      // 160 px / 6 px font
    static const uint8_t ROWS = 10;      // 80 px / 8 px font

    bool     present    = false;   // panel initialised at least once
    bool     enabled    = true;    // "display off" keeps it dark
    uint16_t timeoutSec = DISPLAY_TIMEOUT_DEFAULT_SEC;

    bool begin(RNSTransport* txp, RNSRadio* rad, const char* fwVersion) {
        transport = txp; radio = rad; version = fwVersion;
        pinMode(PIN_TFT_BL, OUTPUT);
        backlight(false);
        for (uint8_t r = 0; r < ROWS; r++) cache[r][0] = '\0';
        if (!enabled) return true;
        return powerOn(millis());
    }

    /** Call every loop() pass. */
    void loop(uint32_t now) {
        if (!present || !enabled) return;
        if (!awake) return;
        if (timeoutSec > 0 && (now - wakeAt) >= (uint32_t)timeoutSec * 1000UL) {
            powerOff();
            return;
        }
        if (now - lastRefresh >= DISPLAY_REFRESH_MS || dirty) {
            lastRefresh = now;
            dirty = false;
            render();
        }
    }

    /** Button: wake if asleep, otherwise advance the page. */
    void buttonPress(uint32_t now) {
        if (!enabled) return;
        if (!awake) { powerOn(now); return; }
        page = (uint8_t)((page + 1) % PAGE_COUNT);
        wakeAt = now;
        invalidate();
    }

    void wake(uint32_t now) {
        if (!enabled) return;
        if (!awake) powerOn(now); else wakeAt = now;
    }

    void setEnabled(bool on, uint32_t now) {
        enabled = on;
        if (!on) { if (awake) powerOff(); }
        else if (!awake) powerOn(now);
    }

    void setTimeout(uint16_t sec, uint32_t now) {
        if (sec > DISPLAY_TIMEOUT_MAX_SEC) sec = DISPLAY_TIMEOUT_MAX_SEC;
        timeoutSec = sec;
        wakeAt = now;
    }

    void setPage(uint8_t p, uint32_t now) {
        page = (uint8_t)(p % PAGE_COUNT);
        wake(now);
        invalidate();
    }

    /// Battery reading supplied by the board code (see main.cpp).
    void setBattery(int8_t percent, bool onUsb) { battPercent = percent; battUsb = onUsb; }
    /// GNSS summary supplied by main (rows 7-8 of STATUS). mode: 0 off, 1 on.
    void setGnss(uint8_t mode, bool fix, uint8_t sats, int32_t latUdeg, int32_t lonUdeg, uint32_t unixNow) {
        gMode = mode; gFix = fix; gSats = sats; gLat = latUdeg; gLon = lonUdeg; gUnix = unixNow;
    }

    bool    isAwake() const { return awake; }
    uint8_t currentPage() const { return page; }

private:
    RNSTransport* transport = nullptr;
    RNSRadio*     radio     = nullptr;
    const char*   version   = "";
    SPIClass*        spi = nullptr;
    Adafruit_ST7735* tft = nullptr;

    bool     awake       = false;
    uint8_t  page        = 0;
    uint32_t wakeAt      = 0;
    uint32_t lastRefresh = 0;
    bool     dirty       = true;
    int8_t   battPercent = -1;     // -1 = unknown
    bool     battUsb     = false;
    uint8_t  gMode = 0; bool gFix = false; uint8_t gSats = 0; int32_t gLat = 0, gLon = 0; uint32_t gUnix = 0;
    char     cache[ROWS][COLS + 1];

    static const uint16_t FG = ST77XX_WHITE;
    static const uint16_t BG = ST77XX_BLACK;
    static const uint16_t ACCENT = ST77XX_CYAN;
    static const uint16_t WARN = ST77XX_ORANGE;

    void backlight(bool on) {
#if TFT_BL_ACTIVE_LOW
        digitalWrite(PIN_TFT_BL, on ? LOW : HIGH);
#else
        digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
#endif
    }

public:
    /// Re-run the panel init on an already-powered rail (after another
    /// Vext consumer powered up and may have sagged the rail).
    void reinitIfAwake() { if (awake) initPanel(); }

private:
    void initPanel() {
        if (!spi) {
            spi = new SPIClass(NRF_SPIM2, PIN_TFT_MISO_UNUSED, PIN_TFT_SCK, PIN_TFT_MOSI);
            spi->begin();
        }
        if (!tft) tft = new Adafruit_ST7735(spi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
        tft->initR(INITR_MINI160x80_PLUGIN);        // inverted, BGR, 26/1 offsets
        tft->setRotation(TFT_ROTATION);
        tft->fillScreen(BG);
        tft->setTextWrap(false);
        tft->setTextSize(1);
        invalidate();
        render();
    }

    bool powerOn(uint32_t now) {
        // Vext is shared with the GNSS rail (RNSVext). If the GNSS already
        // holds it the rail is stable; otherwise claim() raises it and
        // waits for the LDO to settle before the panel is touched.
        RNSVext::claim(RNSVext::VEXT_DISPLAY);
        delay(20);
        present = true;
        awake = true;
        wakeAt = now;
        initPanel();
        backlight(true);
        return true;
    }

    void powerOff() {
        backlight(false);
        if (tft) { tft->enableDisplay(false); tft->enableSleep(true); }
        awake = false;
        RNSVext::release(RNSVext::VEXT_DISPLAY);     // rail stays up if the GNSS holds it
    }

    void invalidate() {
        for (uint8_t r = 0; r < ROWS; r++) cache[r][0] = '\xff';   // never matches
        dirty = true;
    }

    // ── row helpers ─────────────────────────────────────────
    void putRow(uint8_t r, const char* text, uint16_t color = FG) {
        if (r >= ROWS) return;
        char line[COLS + 1];
        strncpy(line, text, COLS); line[COLS] = '\0';
        if (strcmp(line, cache[r]) == 0) return;
        strcpy(cache[r], line);
        tft->fillRect(0, r * 8, 160, 8, BG);
        tft->setCursor(0, r * 8);
        tft->setTextColor(color);
        tft->print(line);
    }

    static void fmtUptime(char* out, size_t n, uint32_t ms) {
        uint32_t s = ms / 1000UL;
        uint32_t d = s / 86400UL; s %= 86400UL;
        uint32_t h = s / 3600UL;  s %= 3600UL;
        uint32_t m = s / 60UL;    s %= 60UL;
        if (d) snprintf(out, n, "%lud%02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m);
        else   snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }

    static void fmtBytes(char* out, size_t n, uint32_t b) {
        if (b < 10000UL)          snprintf(out, n, "%lu", (unsigned long)b);
        else if (b < 10000000UL)  snprintf(out, n, "%luk", (unsigned long)(b / 1024UL));
        else                      snprintf(out, n, "%luM", (unsigned long)(b / 1048576UL));
    }

    uint32_t freeRam() const {
        char top;
        return (uint32_t)(&top - sbrk(0));
    }

    // ── pages ───────────────────────────────────────────────
    void render() {
        switch (page) {
            case 0: renderStatus(); break;
            case 1: renderRadio();  break;
            default: renderPeers(); break;
        }
    }

    void renderStatus() {
        char line[COLS + 8], a[16], b[16];
        const TransportStats& s = transport->getStats();

        // 0: name (left) + battery (right)
        char batt[8];
        if (battUsb) strcpy(batt, "USB");
        else if (battPercent >= 0) snprintf(batt, sizeof(batt), "%d%%", battPercent);
        else strcpy(batt, "--");
        const char* name = transport->getAnnounceName();
        snprintf(line, sizeof(line), "%-*.*s%*s", COLS - 5, COLS - 5, name, 5, batt);
        putRow(0, line, ACCENT);

        // 1: uptime + RAM
        fmtUptime(a, sizeof(a), millis());
        snprintf(line, sizeof(line), "up %-10s RAM %luk", a, (unsigned long)(freeRam() / 1024UL));
        putRow(1, line);

        // 2-3: packet counters
        snprintf(line, sizeof(line), "RX %-6lu TX %-6lu FW %lu",
                 (unsigned long)s.rxPackets, (unsigned long)s.txPackets, (unsigned long)s.fwdPackets);
        putRow(2, line);
        snprintf(line, sizeof(line), "AN %-6lu DP %-6lu IV %lu",
                 (unsigned long)s.announces, (unsigned long)s.duplicates, (unsigned long)s.invalidPackets);
        putRow(3, line, s.invalidPackets ? WARN : FG);

        // 4: bytes
        fmtBytes(a, sizeof(a), radio->rxBytes); fmtBytes(b, sizeof(b), radio->txBytes);
        snprintf(line, sizeof(line), "RXB %-8s TXB %s", a, b);
        putRow(4, line);

        // 5: paths + signal
        if (radio->lastRSSI != 0.0f)
            snprintf(line, sizeof(line), "PATHS %-3u %4.0fdBm %+.1fdB",
                     (unsigned)s.pathEntries, (double)radio->lastRSSI, (double)radio->lastSNR);
        else
            snprintf(line, sizeof(line), "PATHS %-3u  no RX yet", (unsigned)s.pathEntries);
        putRow(5, line);

        // 6: radio one-liner
        snprintf(line, sizeof(line), "%.1f BW%.0f SF%u CR%u %ddBm",
                 (double)radio->curFreqMHz, (double)radio->curBwKHz,
                 (unsigned)radio->curSF, (unsigned)radio->curCR, (int)radio->curTxDbm);
        putRow(6, line, radio->hwReady ? FG : WARN);

        // 7: GNSS
        if (gMode == 0) putRow(7, "GPS off");
        else if (gFix) {
            snprintf(line, sizeof(line), "GPS %2usat %.4f %.4f", (unsigned)gSats, (double)gLat / 1e6, (double)gLon / 1e6);
            putRow(7, line);
        } else {
            snprintf(line, sizeof(line), "GPS %2usat searching", (unsigned)gSats);
            putRow(7, line, WARN);
        }
        // 8: UTC clock when synced
        if (gUnix) {
            uint32_t t = gUnix; uint32_t days = t / 86400UL; uint32_t rem = t % 86400UL;
            // civil from days (Howard Hinnant)
            int64_t z = (int64_t)days + 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            unsigned doe = (unsigned)(z - era * 146097); unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
            int y = (int)yoe + (int)era * 400; unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
            unsigned mp = (5*doy + 2)/153; unsigned d = doy - (153*mp+2)/5 + 1; unsigned m = mp < 10 ? mp+3 : mp-9;
            if (m <= 2) y++;
            snprintf(line, sizeof(line), "UTC %02lu:%02lu:%02lu %04d-%02u-%02u",
                     (unsigned long)(rem/3600), (unsigned long)((rem%3600)/60), (unsigned long)(rem%60), y, m, d);
            putRow(8, line);
        } else putRow(8, "");
        snprintf(line, sizeof(line), "%-16s     pg 1/%u", version, (unsigned)PAGE_COUNT);
        putRow(9, line, ACCENT);
    }

    void renderRadio() {
        char line[COLS + 8];
        putRow(0, "RADIO  SX1262 + FEM", ACCENT);
        snprintf(line, sizeof(line), "HW   %s", radio->hwReady ? "OK (live)" : "NOT READY");
        putRow(1, line, radio->hwReady ? FG : WARN);
        snprintf(line, sizeof(line), "Freq %.3f MHz", (double)radio->curFreqMHz);       putRow(2, line);
        snprintf(line, sizeof(line), "BW   %.1f kHz  SF %u", (double)radio->curBwKHz, (unsigned)radio->curSF); putRow(3, line);
        snprintf(line, sizeof(line), "CR   4/%u  pre %u", (unsigned)radio->curCR, (unsigned)radio->curPreamble); putRow(4, line);
        snprintf(line, sizeof(line), "TX   %d dBm (cap %d)", (int)radio->curTxDbm, (int)LORA_TX_DBM_MAX_SAFE); putRow(5, line);
        snprintf(line, sizeof(line), "Sync 0x%02X  %s", (unsigned)radio->curSyncWord,
                 radio->pollMode ? "IRQ poll" : "DIO1 int");                            putRow(6, line);
        snprintf(line, sizeof(line), "Init rc %d  try %u", radio->lastInitState, (unsigned)radio->initAttempts); putRow(7, line);
        putRow(8, BOARD_DISPLAY_NAME);
        snprintf(line, sizeof(line), "%-16s     pg 2/%u", version, (unsigned)PAGE_COUNT);
        putRow(9, line, ACCENT);
    }

    void renderPeers() {
        char line[COLS + 8];
        const PathEntry* pt = transport->getPathTable();
        uint32_t now = millis();
        // Column layout matches the rows below: hash(0-3) name(5-13) hp(15-16) dBm(18-21) age(23-25)
        putRow(0, "hash name      hp  dBm age", ACCENT);
        uint8_t row = 1;
        for (int i = 0; i < PATH_TABLE_MAX && row < ROWS - 1; i++) {
            if (!pt[i].active) continue;
            char hash[6];
            snprintf(hash, sizeof(hash), "%02X%02X", pt[i].destHash[0], pt[i].destHash[1]);
            // 9 chars of name; skip a leading "RatTunnel " so the part that
            // differs between nodes is what fits.
            const char* src = pt[i].peerName[0] ? pt[i].peerName : "-";
            if (strncmp(src, "RatTunnel ", 10) == 0 && src[10]) src += 10;
            char name[10];
            strncpy(name, src, 9); name[9] = '\0';
            // age in 3 chars: "45s", "17m", " 3h"
            uint32_t ageS = (now - pt[i].learnedAt) / 1000UL;
            char age[4];
            if (ageS < 600)         snprintf(age, sizeof(age), "%2lus", (unsigned long)ageS);
            else if (ageS < 36000)  snprintf(age, sizeof(age), "%2lum", (unsigned long)(ageS / 60));
            else                    snprintf(age, sizeof(age), "%2luh", (unsigned long)(ageS / 3600 > 99 ? 99 : ageS / 3600));
            if (pt[i].lastRSSI != 0.0f)
                snprintf(line, sizeof(line), "%s %-9s %2u %4.0f %s", hash, name,
                         (unsigned)pt[i].hops, (double)pt[i].lastRSSI, age);
            else
                snprintf(line, sizeof(line), "%s %-9s %2u  --- %s", hash, name,
                         (unsigned)pt[i].hops, age);
            putRow(row++, line);
        }
        if (row == 1) putRow(row++, "(no paths yet)", WARN);
        for (; row < ROWS - 1; row++) putRow(row, "");
        snprintf(line, sizeof(line), "%u paths          pg 3/%u",
                 (unsigned)transport->getStats().pathEntries, (unsigned)PAGE_COUNT);
        putRow(9, line, ACCENT);
    }
};
#endif  // HAS_DISPLAY
