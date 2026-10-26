# boards/

Custom PlatformIO board definitions and linker scripts for targets the
stock `nordicnrf52` platform does not describe correctly.

## xiao_nrf52840_s140v7.json

Seeed XIAO nRF52840 as fitted to the Ikoka Stick. The XIAO ships with
Nordic SoftDevice **S140 v7.3.0** (fwid 0x0123); the platform's
`nrf52840_dk_adafruit` board assumes S140 v6.1.1 (fwid 0x00B6) and links
the application at 0x26000, which overlaps the v7 SoftDevice image and
which the v7 bootloader refuses in DFU.

* SoftDevice block (`sd_version`, `sd_fwid`) taken verbatim from
  MeshCore `boards/seeed-xiao-afruitnrf52-nrf52840.json` (commit 0679dbe).
  `sd_fwid` is what `adafruit-nrfutil dfu genpkg --sd-req` embeds in
  `firmware.zip`.
* `hwids` taken from the same MeshCore JSON (Seeed VID 0x2886).
* `variant` is **pca10056**, not Seeed's `Seeed_XIAO_nRF52840` variant,
  because the Adafruit BSP that PlatformIO ships does not carry the Seeed
  variant. pca10056 numbers pins as raw GPIO (P0.x = x, P1.x = 32 + x),
  the same convention `include/RNSConfig.h` already uses for the RAK4631,
  so no pin translation layer is needed.
* `maximum_ram_size` = 0x40000 - 0x6000 (S140 v7 RAM reservation at
  0x20006000, see the linker script).

## nrf52840_s140_v7.ld

Copied unmodified from MeshCore `boards/nrf52840_s140_v7.ld`
(commit 0679dbe). FLASH origin 0x27000 (v6 scripts use 0x26000),
RAM origin 0x20006000. Referenced from `platformio.ini` via
`board_build.ldscript`, so nothing has to be copied into the BSP tree.

## lib/nrf52/s140_nrf52_7.3.0_API

Nordic SoftDevice S140 v7.3.0 API headers, copied from MeshCore
`lib/nrf52/s140_nrf52_7.3.0_API` (commit 0679dbe). The Adafruit BSP only
carries the 6.1.1 headers. The Ikoka env puts this directory first on the
include path so `nrf_soc.h` / `nrf_sdm.h` resolve to the v7 API. Nordic's
license text is embedded in each header. The RAK env does not reference
this directory.

## heltec_t096_s140v6.json

Heltec Mesh Node T096 (nRF52840 + SX1262 + KCT8103L FEM). The T096
ships Heltec's Adafruit-derived UF2 bootloader with SoftDevice
**S140 v6.1.1** (fwid 0x00B6) — the same SoftDevice generation as the
RAK4631, so the app links at 0x26000 with the BSP's stock
`nrf52840_s140_v6.ld` and the 6.1.1 API headers. No linker script or
SoftDevice headers are added for this board.

* `hwids`: Adafruit VID 0x239A; PID 0x8071 is what the bench board enumerates as in
  normal mode (`HT_n5262G`, 2026-09-25); 0x8029/0x0029/0x002A/0x802A come from
  the `softdevice` block are taken from MeshCore `boards/heltec_t096.json`.
* `variant` is **pca10056** (raw GPIO numbering), like the other two
  targets; `include/RNSConfig.h` carries the schematic-verified pin map.
* Bootloader drive name: `HT-n5262G` (double-tap reset). Confirm the
  SoftDevice in `INFO_UF2.TXT` reads S140 6.1.1 before the first flash;
  a v7 bootloader would need the Ikoka-style 0x27000 layout instead.
* `maximum_ram_size` = 0x40000 - 0x3400 (S140 v6 reservation, same as
  `nrf52840_dk_adafruit`).
