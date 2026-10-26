# Bench verification — Heltec Mesh Node T096 (nRF52840 + SX1262 + KCT8103L)

Firmware env: `heltec_t096_transport`. Work through this top to bottom the
first time a board is flashed. Do **not** leave the node announcing
unattended until section 3 is signed off.

Hardware notes that apply to every step:

* Always have an antenna or a 50 Ω load on the IPEX connector before any
  TX step. The KCT8103L is rated ~28 dBm; an open output at that level
  can damage it.
* The FEM rail (Vfem, TLV75733) is fed from the battery/VBUS node. Bench
  on a USB supply that can source ≥ 1 A, or a charged cell on JP1.
* Schematic: `Mesh_Node_T096_V0.2.pdf` (Heltec). Net table transcribed
  in `include/RNSConfig.h` under `BOARD_HELTEC_T096`.

## 0. Flashing rule for macOS hosts

Use **UF2 drag-and-drop only**. Do not use `pio run -t upload` /
`adafruit-nrfutil dfu serial` from a Mac (kernel panic in the CDC driver
observed 2026-09-18 on the Ikoka Stick; same bootloader family). Close
any serial console before pressing reset.

## 1. Bootloader check (before the first flash)

Bench board, 2026-09-25: `INFO_UF2.TXT` = "UF2 Bootloader
0.9.0-2-g836c8dc-dirty … Board-ID: HT-n5262G, Date: Mar 19 2026,
SoftDevice: S140 6.1.1". Factory `CURRENT.UF2` (1,908,736 B, 0x1000..
0xEA000, app vector table at 0x26000) backed up to
`~/Downloads/T096_factory_CURRENT_2026-09-25.uf2`. Normal-mode USB:
VID 0x239A PID 0x8071, product `HT_n5262G`.

- [x] Double-tap reset → `HT-n5262G` drive appears. Open `INFO_UF2.TXT`
      and record: bootloader version, `SoftDevice: S140 version 6.1.1`.
      **If the SoftDevice is not 6.1.1, stop.** The image links at
      0x26000 and would overwrite the last page of a v7 SoftDevice (the
      2026-09-21 Ikoka incident). A v7 board needs a new board JSON with
      the 0x27000 layout, mirroring `xiao_nrf52840_s140v7.json`.
- [ ] Record `CURRENT.UF2` size and the factory firmware name if the
      drive shows one, for rollback (Heltec ships MeshCore or Meshtastic).

## 2. Flash and boot

- [x] Copy `.pio/build/heltec_t096_transport/firmware.uf2` onto
      `HT-n5262G`. (2026-09-25: `cp` reported "fcopyfile failed:
      Input/output error" as the drive ejected; board rebooted into
      RatTunnel on its own.) The copy tool may report an I/O error as the drive
      ejects; that is normal. Board reboots on its own.
- [x] White LED (P0.28) heartbeat starts. Solid LED + dead console means
      the SoftDevice check in §1 was wrong — rollback per §1.
- [x] Open the console (115200). `status` shows
      `Board: Heltec T096 (SX1262 + KCT8103L)`,
      `Build: rattunnel-heltec-t096`, TX cap 18 dBm, variant max 22.
- [x] Boot log shows, in order (captured via `reinit`, 2026-09-25):
      `[DIAG] FEM enabled: VFEM(P30)=1 CSD(P12)=1 CTX(P41)=0 (RX LNA)`,
      `post-reset BUSY=0`, `findChip OK`, `RX mode: DIO1 interrupt`,
      `startReceive rc=0`, `SX1262 init OK — radio is live`.
      Record free RAM from `status`. → 115,808 B. Noise floor with the
      LNA path in circuit: avg −88.5 dBm, min −91 (`nfloor 3`); the
      21 dB LNA lifts it above the −100…−115 range the test expects.
      `set txpower 19` / `22` rejected ("must be -9 to 18 dBm").
      TFT is dark by design (Vext held LOW; no display driver).
- [ ] Hold the user button (P1.10) through a reset for 2 s → board
      reboots into `HT-n5262G`. Confirms the Heltec bootloader honours
      GPREGRET 0x57. Reset again to return to RatTunnel.

## 3. Receive

2026-09-25 finding (not T096-specific): with the 1.0.33 announce parser
the board logged `RX packets: 6, Announces: 0, Invalid: 2, Paths: 0`
while a phone's LXMF announce (context flag set = 32-byte ratchet,
relayed 3 hops, RSSI −6) was hex-dumped on the console. The parser had no
ratchet layout and also expected the signature after the app data, the
reverse of RNS/Destination.py. Fixed on branch heltec-t096: parser now
takes the reference layout (with/without ratchet) first and the old
RatTunnel trailing-signature layout as a fallback; outgoing announces
now use the reference layout; msgpack bin8/str8 names are accepted. The
captured frame verifies with Python Ed25519 at the new offsets.

- [ ] `profile ratspeak-us` (915.0 MHz, BW125, SF9, CR5, sync 0x12,
      preamble 18). Use `pktdump` and confirm packets from an existing
      RatTunnel / RNode node on the same profile arrive with sane RSSI
      and SNR. The KCT8103L LNA path (CTX low) is in circuit; compare
      RSSI against a WisBlock at the same distance and note the delta.
- [ ] Announces from the bench peer show up in `peers` / `paths`.

## 4. Transmit and power

**2026-10-01 finding:** CTX (P1.09) was held LOW; the KCT8103L needs CTX
HIGH during TX. Symptom: heard at -58..-76 dBm from 10 cm by an Ikoka
Stick, never heard at the deployment site, receive fine. Fixed by
driving CTX as RadioLib's TXEN. Re-run the RSSI check below after the
fix; a power-meter table is still the real sign-off.

- [x] Two-radio check 2026-10-01: Ikoka `peers` RSSI for the T096 at
      10 cm: before fix -58..-76 dBm, after fix 0..-7 dBm (node announce
      192 B at -5/-0/-7, map announce frames at -0). ~55 dB recovered.


- [ ] Power meter on the IPEX (through a known attenuator). `set txpower`
      at 5, 10, 14, 18. Expected from the Prns gain table: ~19, ~24,
      ~27, ~28 dBm. Record the actual figures here:

      | drive dBm | measured dBm |
      |---|---|
      | 5 | |
      | 10 | |
      | 14 | |
      | 18 | |

- [ ] `set txpower 19` and `22` must be rejected by the console (cap 18).
- [ ] Watch supply current at 18 dBm during an announce burst. Note the
      peak. No brown-out resets (check reset-cause on the next boot).
- [ ] A peer receives this node's announce and the path resolves.

## 4b. Display

- [x] 2026-09-25: first display build flashed. Panel initialised with
      `INITR_MINI160x80_PLUGIN`, rotation 1, backlight PMOS active-LOW,
      Vext gated by P0.26. Dash rendered correctly on the first try
      (Gene: "looks good"). Flash +16 KB, RAM +360 B.
- [ ] `display timeout 20` → panel blanks after 20 s, Vext drops; short
      press re-lights it on the same page.
- [ ] Short press cycles STATUS → RADIO → PEERS → STATUS.
- [ ] `display off` → panel dark across a reboot; `display on` restores.
- [ ] With a cell on JP1 and USB unplugged, top row shows a percentage.

## 4c. Discovery announce

- [x] 2026-09-25: `location` + `discovery on` → stamp computed on-device;
      `discovery dump` validated with tools/validate_discovery.py against
      RNS 1.5.4's handler: VALID, field checks PASS. Frequency rounding
      fixed afterwards (914.9 MHz packed as 914900032 Hz).
- [x] 2026-09-25: first `discovery now` failed with startTransmit rc=-4
      (324 bytes > 255-byte SX1262 frame). Added RNode split framing;
      re-flashed: "TX start: 255 bytes (frame 1/2)", "70 bytes (frame
      2/2)", "TX done: rc=0 elapsed=771ms", "Discovery announce sent".
- [x] 2026-09-25: rmap.world lists "RatTunnel Rhodedendron" at the
      configured position (Gene confirmed). End-to-end: on-device stamp,
      split-frame TX, relay via the network to an internet transport,
      accepted by the RMAP collector.

## 5. Radio-with-Vext-off check

Prns keeps Vext on and claims it feeds the radio; the V0.2 schematic
shows the SX1262 on the always-on VDD_3V3 rail. Sections 2–4 passing with
`PIN_VEXT_CTRL` held LOW settles it. If the radio only works with Vext
high, change the setup() default and note it here.

## 6. Sign-off

Date, board revision (silkscreen), bootloader + SoftDevice from §1,
measured power table, free RAM, and who ran it.
