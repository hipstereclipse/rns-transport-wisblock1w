/**
 * @file RNSDfu.h
 * @brief Reboot into the resident Adafruit/Seeed nRF52 UF2 bootloader.
 *
 * Shared by the console `dfu` command and the boot-time user-button
 * path. The sequence only writes the GPREGRET magic the stock
 * bootloader already looks for and resets — the bootloader itself is
 * never touched, so this can never brick the board.
 */
#pragma once

#ifndef NATIVE_TEST
#include <Arduino.h>
#if __has_include("nrf_soc.h")
#include <nrf_soc.h>
#endif

#define DFU_MAGIC_UF2_BOOTLOADER 0x57   // Adafruit nRF52 bootloader: enter UF2/DFU

inline void rnsRebootToDfu() {
    // Use SoftDevice API when SD is active, else direct register write.
    // Both paths end with a DSB to guarantee the write commits before reset.
    uint32_t rc = 0xFFFFFFFF;
#if __has_include("nrf_soc.h")
    rc  = sd_power_gpregret_clr(0, 0xFF);
    rc |= sd_power_gpregret_set(0, DFU_MAGIC_UF2_BOOTLOADER);
#endif
    if (rc != 0) {
        NRF_POWER->GPREGRET = DFU_MAGIC_UF2_BOOTLOADER;
    }
    __DSB();
    __ISB();
    NVIC_SystemReset();
}
#endif
