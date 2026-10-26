# Bench verification — Ikoka Stick (XIAO nRF52840 + E22-900M30S)

Firmware env: `ikoka_stick_transport`. Work through this top to bottom the
first time a board is flashed. Do **not** leave the node announcing
unattended until section 3 is signed off.

Hardware notes that apply to every step:

* Ikoka Stick **v0.2.0 or later** only (commit 2cd2400 "Supply E22 LoRa
  module +5V2 rail from 5V when no battery is present", tag
  `v0.2.0-52a6ed2`). Earlier boards drew excessive current with no
  battery inserted. Check the silkscreen revision.
* Always have an antenna or a 50 Ω load on the E22 before any TX step.
* The MT3608 boost feeds the E22 from the XIAO charger output. Bench on
  a USB supply that can source ≥ 1 A, or a charged cell.

## 0. Flashing rule for macOS hosts

Use **UF2 drag-and-drop only**. Do not use `pio run -t upload` /
`adafruit-nrfutil dfu serial` from a Mac: the 1200-baud touch makes the
XIAO re-enumerate while the uploader still has a write in flight, and on
macOS 15.6 that panicked the kernel in `com.apple.driver.usb.cdc`
(observed 2026-09-18). Close any serial console before pressing reset
for the same reason.

## 0b. If the node hangs with a SOLID green LED and a dead console

Symptom (seen 2026-09-21): USB enumerates, the port cannot be opened or
never answers, green LED solid (not heartbeat). Cause: the WisBlock UF2
(the portal's default GitHub release asset) was written to the XIAO. It
links at 0x26000, which on the XIAO is the last page of the S140 v7.3.0
SoftDevice; RatTunnel's InternalFS layer calls into the SoftDevice at
boot and hangs. Proof: double-tap reset, copy `CURRENT.UF2` off
`XIAO-BOOT`, and the 256-byte block at 0x26000 starts with an app vector
table (`20040000 00064605 …`) instead of SoftDevice code.

Repair without serial DFU:
1. Download the XIAO bootloader hex that bundles S140 7.3.0 and matches
   `INFO_UF2.TXT` (e.g. oltaco/Adafruit_nRF52_Bootloader_OTAFIX release
   `xiao_nrf52840_ble_bootloader-0.9.2-OTAFIX1.2-BP1.2_s140_7.3.0.hex`).
2. `python tools/make_sd_repair_uf2.py <that.hex>` → emits a UF2 that
   covers only 0x1000..0x27000.
3. Double-tap reset, drag the repair UF2 onto `XIAO-BOOT`. The app at
   0x27000 is untouched; the node reboots straight into RatTunnel.

## 1. Flash and boot

- [ ] Confirm `boards/xiao_nrf52840_s140v7.json` reports
      `sd_fwid 0x0123` and the build log shows `firmware.zip` built with
      `--sd-req 0x0123`.
- [ ] Double-tap reset → `XIAO-BOOT` drive appears (INFO_UF2.TXT on the
      bench board: UF2 Bootloader 0.9.2, SoftDevice S140 7.3.0). Copy the
      **Ikoka** `firmware.uf2` from `.pio/build/ikoka_stick_transport/`.
      Do NOT flash the portal's GitHub release asset on a XIAO: that is the
      WisBlock image linked at 0x26000 and it overlaps the v7 SoftDevice.
      The copy tool may report an I/O error as the drive ejects; that is
      normal. Board reboots on its own.
- [ ] Close every portal tab in the browser before using another serial
      client: Web Serial keeps the port open (seen: five handles held by
      Brave, which blocked the console entirely).
- [ ] Serial console (115200) shows
      `[RNS] Ikoka Stick  |  RatTunnel V. 1.0.33`.
- [ ] `version` prints `Board: Ikoka Stick (XIAO nRF52840 + E22-900M30S)`
      and `TX cap: 18 dBm (variant max 20, announce 18)`.
- [ ] Green LED heartbeat, blue LED off at idle (confirms LED_ACTIVE_HIGH 0
      is right for this XIAO batch; if the RGB is lit solid at boot the
      polarity is wrong).

## 2. Radio init (SX126x findChip)

Bench result 2026-09-18 (cold reset, port opened after re-enumeration):

```
[DIAG]   post-reset BUSY=0 after 0ms
[DIAG] SX1262 begin: starting init sequence (V1.0.33)
[DIAG]   freq=915.0 bw=125.0 sf=9 cr=5 tx=18 (cap 18) sync=0x12 pre=18
[DIAG]   begin rc=0
[DIAG]   findChip OK — SX1262 version string matched
[DIAG] SX1262 configuring DIO2-as-TXEN / RXEN / DCDC...
[DIAG] RX mode: DIO1 interrupt
[DIAG] startReceive rc=0
[DIAG] SX1262 init OK — radio is live
[RNS] Active: 915.0 MHz, SF9, BW125 kHz, 18 dBm SX1262 drive (cap 18, announce 18) + board PA
[DIAG] TX start: 179 bytes, txPower=18 dBm  →  TX done: rc=0 elapsed=974ms  (announce)
[DIAG] TX start: 52 bytes,  txPower=18 dBm  →  TX done: rc=0 elapsed=402ms  (discovery)
```
(The SPI pin-dump lines print before USB CDC is open on a cold boot; use
`reinit` on the console to see them.)


- [ ] Boot log shows `[DIAG] SPI on SPIM3 (Ikoka Stick)` with
      `MOSI=47 MISO=46 SCK=45 NSS=4 RST=28 BUSY=29 DIO1=3 RXEN=5`.
- [ ] `post-reset BUSY=0` within a few ms.
- [ ] `begin rc=0` followed by `findChip OK — SX1262 version string
      matched`. `rc=-2` means findChip failed (SPI wiring / power),
      `rc=-707` means the TCXO on DIO3 did not start.
- [ ] `RX mode: DIO1 interrupt` (not IRQ polling).
- [ ] `startReceive rc=0`.
- [ ] Have a second Reticulum node announce; `peers` / `routes` on the
      console list it with a sane RSSI/SNR. This proves RXEN is being driven HIGH in
      RX by RadioLib (LNA path) and the DIO2→TXEN wiring is not stuck.

## 3. TX power at the cap (power meter) — REQUIRED before sustained TX

Ebyte publishes no PA gain figure for the E22-900M30S. The 18 dBm cap and
20 dBm variant maximum come from MeshCore's Ikoka Stick 30 dBm env
("20dBm in -> 30dBm out"); this section is where that assumption gets
checked on **your** module.

Setup: E22 antenna port → 30 dB (≥ 5 W) attenuator → RF power meter or
spectrum analyser. Note the meter's insertion loss.

- [ ] `set txpower 0` → `announce`. Record output. Expect roughly
      +10 dBm at the antenna port (PA gain ≈ 10 dB).
- [ ] `set txpower 10` → `announce`. Record. Expect ≈ +20 dBm.
- [ ] `set txpower 18` (the cap) → `announce`. Record. Expect ≈ +28 dBm.
      **If this reads more than +30 dBm, or the step from 10→18 dBm gave
      less than ~6 dB of increase (PA compressing), stop and lower
      `LORA_TX_DBM_MAX_SAFE` in `include/RNSConfig.h`.**
- [ ] `set txpower 22` → console must reply `TX power must be -9 to 18 dBm`
      and `radio` must still show 18. (Console-layer check. Verified on the
      bench 2026-09-18: 22, 20 and 19 all rejected, 10 accepted,
      `profile ratspeak-us` applies 17.)
- [ ] Write a config.bin with txDbm=22 from a WisBlock build (or hex-edit),
      reboot: boot log must show `tx=18 (cap 18)`. (Lowest-layer check:
      the clamp in `RNSRadio::clampTxDbm` caught a persisted value.)
- [ ] `lorascan` while watching the meter: no burst exceeds the cap
      reading from the previous step (the scan re-inits pass `savedTx`
      through `clampTxDbm`).
- [ ] Measure supply current during a burst at the cap. Ebyte quotes
      650 mA at 30 dBm; at ~28 dBm expect noticeably less. Note the number
      in the README table.

## 4. Brownout behaviour under announce

- [ ] Power from a weak USB source (a laptop port through a hub is
      fine). `power announce 15` and let it run 10 minutes. Console must
      show `Reset cause:` only once, at boot. Any `Reset cause: brownout`
      or `wdt` lines mean the rail is sagging; lower
      `LORA_TX_DBM_ANNOUNCE_SAFE` first, then `LORA_TX_DBM`.
- [ ] Repeat with **no battery** on a v0.2.x board and again with a
      battery inserted.
- [ ] Repeat with `set txpower 18` and the announce cap doing the
      derating (boot log line `announce 18`).
- [ ] `announce` from the console while the meter is attached: the
      burst must be at the announce cap, and `radio` afterwards must show
      the configured setpoint restored.

## 5. Path persistence across reboot

RatTunnel V1.0.33 does **not** persist the path table (`/paths.bin` is
defined but never written; see `include/RNSPersistence.h`). This section
therefore verifies what *is* persisted and documents the expected loss.

- [ ] `set txpower 15`, `save`, `reboot`: boot log shows
      `Loaded saved radio config` and `tx=15`.
- [ ] `routes` before reboot lists N entries; after reboot it is empty
      until the next announce round. This is expected on both WisBlock
      and Ikoka builds.
- [ ] Identity survives: the node's transport hash printed at boot is the
      same before and after reboot and after a `dfu` round-trip.
- [ ] `factory-reset` then reboot: fresh identity, defaults restored
      (`tx=18`).

## 6. Button / DFU

- [ ] Hold D0 (the user button) while pressing reset. All three LEDs
      light for ~0.2 s, then the `XIAO-BOOT` drive appears. Release.
- [ ] Console `dfu` does the same.
- [ ] A short press at boot (< 2 s) does nothing; normal boot continues.

## 7. Numbers to record

| Item | Value |
|---|---|
| Board silkscreen revision | |
| Output at chip 0 / 10 / 18 dBm (dBm at antenna port) | |
| Supply current at cap (mA) | |
| Free RAM (`status` → `Free RAM:`) at idle | 115,704 B (bench, 2026-09-18, 38 s uptime, 0 peers) |
| Free RAM after 30 min with peers | |
