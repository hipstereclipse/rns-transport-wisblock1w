/**
 * @file RNSVext.h
 * @brief Shared ownership of the Heltec T096 Vext rail.
 *
 * Schematic Mesh_Node_T096_V0.2: U3 (CE6260B33M) makes Vext_3V3, enabled
 * by Vext_Ctrl P0.26. Vext feeds the TFT and its backlight AND is the
 * source of the GNSS rail (Q3 PMOS, VGNSS_CTRL). Two consumers therefore
 * share one switch: the display (while awake) and the GNSS (while on).
 * The rail is high while either claims it and dropped only when both
 * release. Claimants call claim()/release(); apply() drives the pin.
 */
#pragma once
#include "RNSConfig.h"
#if defined(PIN_VEXT_CTRL) && !defined(NATIVE_TEST)
#include <Arduino.h>

class RNSVext {
public:
    enum Claimant : uint8_t { VEXT_DISPLAY = 0x01, VEXT_GNSS = 0x02 };

    static void begin() {
        pinMode(PIN_VEXT_CTRL, OUTPUT);
        digitalWrite(PIN_VEXT_CTRL, LOW);
        claims = 0; on = false;
    }
    /// Returns true if the rail was just switched on (caller should let it settle).
    static bool claim(Claimant c) {
        claims |= c;
        if (!on) { digitalWrite(PIN_VEXT_CTRL, HIGH); on = true; delay(VEXT_SETTLE_MS); return true; }
        return false;
    }
    static void release(Claimant c) {
        claims &= (uint8_t)~c;
        if (claims == 0 && on) { digitalWrite(PIN_VEXT_CTRL, LOW); on = false; }
    }
    static bool isOn() { return on; }
    static bool heldBy(Claimant c) { return (claims & c) != 0; }

    static const uint16_t VEXT_SETTLE_MS = 30;
private:
    static uint8_t claims;
    static bool on;
};
#endif
