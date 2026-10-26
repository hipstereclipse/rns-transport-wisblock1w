# Developer Guide — RNS Transport Node for WisBlock 1W

## Architecture Overview

The firmware is organized into six layers, each in its own header (and optional source) file. All data structures use static allocation — there are zero calls to `malloc` or `new` after `setup()` completes.

```
┌─────────────────────────────────────────────────┐
│  main.cpp — init, super-loop, watchdog          │
├────────────┬────────────┬───────────────────────┤
│ RNSConsole │ RNSPersist │ LED / Status          │
├────────────┴────────────┴───────────────────────┤
│         RNSTransport — routing engine           │
│  path table · dedup cache · announce queue      │
├─────────────────────┬───────────────────────────┤
│   RNSIdentity       │      RNSPacket            │
│  Ed25519 + X25519   │  wire format parse/ser    │
├─────────────────────┴───────────────────────────┤
│         RNSRadio — SX1262 via RadioLib          │
│  CSMA/CA · interrupt RX · CAD · airtime track   │
├─────────────────────────────────────────────────┤
│     Hardware: RAK3401 (nRF52840) + RAK13302     │
│     SX1262 + SKY66122 PA → +30 dBm / 1 W       │
└─────────────────────────────────────────────────┘
```

### Layer Responsibilities

| Layer | File(s) | Role |
|-------|---------|------|
| Radio | `RNSRadio.h` | SX1262 init, TX with CSMA, interrupt-driven RX |
| Packet | `RNSPacket.h` | Wire-format parsing, serialization, SHA-256 hashing |
| Identity | `RNSIdentity.h` | Keypair management, announce validation, dest hashing |
| Transport | `RNSTransport.h/.cpp` | Path table, dedup, announce queue, forwarding |
| Console | `RNSConsole.h` | USB serial CLI, config commands, DFU trigger |
| Persistence | `RNSPersistence.h` | LittleFS identity/config storage |
| Main | `main.cpp` | Init sequence, super-loop, watchdog, error handling |

## Build Environment

### Prerequisites

- **PlatformIO Core** (CLI) or **PlatformIO IDE** (VS Code extension)
- Python 3.8+ (for tools scripts)
- `adafruit-nrfutil` and `pyserial` (for OTA updates)

### Build Commands

```bash
# Clone the repository
git clone <repo-url> && cd RNS-Transport-WisBlock

# Build firmware
pio run -e wisblock_1w_transport

# Build and upload via USB bootloader
pio run -e wisblock_1w_transport --target upload

# Run host-side unit tests
cd test && g++ -std=c++17 -DNATIVE_TEST -I../include test_packet.cpp -o test_packet && ./test_packet
cd test && g++ -std=c++17 -DNATIVE_TEST -I../include test_transport.cpp -o test_transport && ./test_transport
cd test && g++ -std=c++17 -DNATIVE_TEST -I../include test_identity.cpp -o test_identity && ./test_identity
```

### Board Configuration

The firmware targets `wiscore_rak4631` in PlatformIO, which is compatible with both the RAK4631 and RAK3401 cores. Key differences for the 1W kit:

- The RAK3401 has **no built-in LoRa radio** — the RAK13302 IO module provides SX1262
- The RAK13302's `WB_IO2` (GPIO 34) must be driven HIGH to enable 3V3_S power
- DIO2 controls the antenna RF switch internally
- DIO3 drives the TCXO at 1.8 V

### Pin Mapping Reference

| Signal | GPIO | Notes |
|--------|------|-------|
| SPI CS | 26 | WB_SPI_CS |
| SPI CLK | 3 | WB_SPI_CLK |
| SPI MISO | 29 | |
| SPI MOSI | 30 | |
| DIO1 (IRQ) | 15 | IO-slot interrupt |
| BUSY | 16 | |
| RESET | 17 | WB_IO1 |
| Module Enable | 34 | WB_IO2 → 3V3_S |
| LED Green | 35 | Active HIGH |
| LED Blue | 36 | Active HIGH |

## Safety Architecture

### Cannot Brick the Device

The firmware is designed with multiple layers of protection:

1. **Bootloader is never modified.** The Adafruit nRF52 bootloader lives in a separate protected flash region. No firmware operation can overwrite it.

2. **Watchdog timer (8 s).** If the main loop stalls for any reason, the hardware watchdog resets the MCU. The device always comes back to a working state.

3. **Radio failure is non-fatal.** If the SX1262 fails to initialize, the firmware enters an error-blink mode but keeps the USB serial console alive. The user can type `dfu` to enter the bootloader.

4. **LittleFS corruption recovery.** If persisted data fails checksum validation, the firmware generates fresh defaults. A corrupted filesystem triggers a format-and-retry.

5. **DFU escape hatch.** The `dfu` console command writes a magic value to GPREGRET and resets, entering the Adafruit bootloader. Alternatively, a physical double-tap on the reset button always works.

6. **UF2 drag-and-drop.** The build produces a `.uf2` file that can be dragged onto the bootloader's USB mass-storage device — the safest possible flashing method.

### Memory Safety

- All buffers are statically allocated with known sizes
- Packet parsing validates lengths before every memcpy
- The transport engine uses fixed-size tables with oldest-entry eviction
- No dynamic allocation after setup() — stack usage is deterministic

## Reticulum Protocol Notes

### Packet Format

```
Byte 0: Flags
  [7]    IFAC flag
  [6]    Header type (0=HEADER_1, 1=HEADER_2)
  [5]    Context flag
  [4]    Propagation (0=BROADCAST, 1=TRANSPORT)
  [3:2]  Dest type (SINGLE/GROUP/PLAIN/LINK)
  [1:0]  Packet type (DATA/ANNOUNCE/LINK_REQUEST/PROOF)

Byte 1: Hop count (uint8, incremented by each transport node)

Bytes 2+: Addresses
  HEADER_1: [destHash 16B]
  HEADER_2: [transportId 16B][destHash 16B]

Next: Context byte (1B)
Rest: Data payload
```

### Transport Forwarding

When forwarding a packet, the transport node:
1. Rewrites HEADER_1 → HEADER_2
2. Sets propagation type to TRANSPORT
3. Inserts its own identity hash as the transport-ID
4. Copies the original destination hash
5. Increments the hop counter
6. Transmits with CSMA/CA

### Announce Validation

Announce packets carry an Ed25519 signature over all data preceding it. The transport node:
1. Checks minimum length (64B pubkey + 10B name hash + 10B random + 64B sig = 148B)
2. Extracts the Ed25519 public key from bytes 32–63
3. Verifies the signature over bytes 0 through (len - 64)
4. Records the path entry (destHash → hop count, timestamp)
5. Queues retransmission with random jitter

## Ratspeak Compatibility

The transport node is protocol-transparent — it forwards any valid Reticulum packet regardless of the application layer. Ratspeak voice and text packets are standard Reticulum DATA packets and are forwarded identically to any other traffic.

For Ratspeak to discover this transport node:
- The node participates in announce propagation (validates and rebroadcasts)
- The node emits local announces on the standard LXMF delivery destination name (`lxmf.delivery`) so peers can learn a valid Reticulum path
- Ratspeak clients on either side of the transport node will learn paths through it
- Voice streams (which are DATA packets to a LINK destination) are forwarded via the path table

No Ratspeak-specific code is needed in the transport node.

## Extending the Firmware

### Adding BLE UART Console

The Adafruit Bluefruit API provides `BLEUart` which implements the Nordic UART Service. To add BLE console:

1. Include `<bluefruit.h>` in main.cpp
2. Initialize Bluefruit and BLEUart in setup()
3. Create a second `RNSConsole` instance bound to the BLEUart stream
4. Poll both consoles in loop()

### Adding New Interfaces

To add a second radio (e.g., a second LoRa module on a different frequency):
1. Create a second `RNSRadio` instance with different pin assignments
2. Create a second `RNSTransport` (or extend the existing one for multi-interface)
3. Route announces and data between interfaces

### microReticulum Integration

For production, consider replacing the from-scratch transport engine with the [microReticulum](https://github.com/attermann/microReticulum) library (v0.2.9+). It provides a more complete Reticulum stack including link establishment, Fernet encryption, and ratchets. The radio layer (`RNSRadio.h`) and console (`RNSConsole.h`) from this project can be reused as-is.

## QA Checklist

- [ ] Packet parsing: all types (DATA, ANNOUNCE, LINK_REQUEST, PROOF)
- [ ] Packet parsing: HEADER_1 and HEADER_2
- [ ] Packet parsing: reject malformed (too short, too long, null)
- [ ] Serialize/parse roundtrip
- [ ] Path table: insert, update shorter, reject longer
- [ ] Path table: eviction when full
- [ ] Dedup cache: record and detect duplicates
- [ ] Announce validation: valid signature accepted
- [ ] Announce validation: invalid signature rejected
- [ ] Announce validation: too-short data rejected
- [ ] Identity: generate, export, load roundtrip
- [ ] Radio: init on real hardware
- [ ] Radio: TX/RX between two nodes
- [ ] Forwarding: packet forwarded with correct header rewrite
- [ ] Console: all commands produce expected output
- [ ] Persistence: identity survives reboot
- [ ] Persistence: config survives reboot
- [ ] Persistence: corrupted data → fresh generation
- [ ] Watchdog: device recovers from infinite loop injection
- [ ] DFU: `dfu` command enters bootloader
- [ ] OTA: full update cycle via ota_update.py
- [ ] Ratspeak: two clients communicate through transport node


---

## Ikoka Stick target notes

See `README.md` → "Ikoka Stick target" for the user-facing summary and
`docs/BENCH_IKOKA_STICK.md` for the sign-off checklist. Developer facts:

* Board selection is `-DBOARD_IKOKA_STICK=1 -DRADIO_MODULE_E22_900M30S=1`
  in `platformio.ini`; `include/RNSConfig.h` is the only file that
  `#if`s on those names. Everything else keys off capability macros
  (`PIN_LORA_DIO1_ACTIVE`, `RADIO_HAS_RAK_PIN_DISCOVERY`, `LED_ACTIVE_HIGH`,
  `PIN_USER_BUTTON`, `PIN_LORA_RXEN`).
* `RNSRadio::begin()` has two bodies: the WisBlock one (P34 gate probe,
  NRST/BUSY brute-force, cascading pin strategies — unchanged) under
  `#if RADIO_HAS_RAK_PIN_DISCOVERY`, and a fixed-pin-map one for boards
  whose schematic is known. Both write the SX1262 setpoint through
  `RNSRadio::clampTxDbm()`.
* DIO1 is real on the XIAO, so RX is ISR-driven. Because TX_DONE and
  CAD_DONE also raise DIO1, `transmit()` clears `rxFlag` before
  re-entering RX (a no-op on the WisBlock, where DIO1 is NC).
* TXEN is not a GPIO on the Ikoka Stick; it is hard-wired to SX1262
  DIO2 (`ikoka-stick-meshtastic-device.kicad_sch`, TXEN pin ↔ DIO2 pin
  via the 186.69/226.06 wire pair). RXEN is P0.05 and RadioLib drives
  it via `setRfSwitchPins(PIN_LORA_RXEN, RADIOLIB_NC)`.
* SoftDevice S140 v7.3.0: custom board JSON + linker + API headers, see
  `boards/README.md`. `sd_fwid 0x0123` ends up in `firmware.zip`
  (`--sd-req`). The UF2 path is unaffected (family 0xADA52840, base
  address read from the hex).
* `tools/generate_uf2.py` syncs `flasher/firmware/latest.uf2` from
  whichever env was built last. With two envs this file is ambiguous;
  CI publishes per-env assets instead. Do not commit a `latest.uf2`
  built from the Ikoka env unless that is intended.

### Path persistence: current state and QSPI costing

RatTunnel does not persist paths on either target. `PATH_TABLE_FILE`
(`/paths.bin`) is reserved and removed on factory reset, nothing writes
it. Internal LittleFS (Adafruit `InternalFileSystem`) is 7 × 4096 B =
28 KB at 0xED000 with 128 B blocks, shared by identity, config, LED,
morse, security, auth and announce-name blobs (a few hundred bytes
total today). `PathEntry` is ~137 B packed (16+16+1+4+4+1+4+4+16+64+1 +
padding) so the in-RAM table of 200 is ~27 KB — it will not fit in
InternalFS beside the other blobs, and 128 B LittleFS blocks on
NRF52 internal flash wear quickly under a table that rewrites on every
announce.

Cost of moving path persistence to the XIAO's on-board 2 MB P25Q16H over
QSPI (not implemented):

| Item | Cost |
|---|---|
| Libraries | `adafruit/Adafruit SPIFlash` (+ its `SdFat - Adafruit Fork` dependency) — two new `lib_deps`, ~20–30 KB flash. `Adafruit_SPIFlash` lists `P25Q16H` in `flash_devices.h` (2 MiB, JEDEC 85 60 15, QSPI writes supported). |
| Filesystem | Reuse `Adafruit_LittleFS` with an `Adafruit_FlashTransport_QSPI` backend (pins from Seeed variant: SCK P0.21, CS P0.25, IO0–3 P0.20/P0.24/P0.22/P0.23). 4 KB erase blocks → LittleFS cache/lookahead buffers ~1–2 KB RAM per mounted FS. |
| RAM | ~1.5 KB (LittleFS buffers) + 4 KB if a page-sized write staging buffer is kept static, per the "no heap after setup()" rule. |
| Capacity | 2 MB ≫ 27 KB; could persist the full 200 entries plus the announce cache (`ANNOUNCE_CACHE_RAW_MAX` 256 × ~500 B = 128 KB) with room to spare. |
| Power | P25Q16H active ~5–8 mA during writes, ~1 µA in deep power-down; must be put to sleep explicitly or it costs idle current. |
| Code | New `RNSPersistence::savePathTable()/loadPathTable()` pair following the existing checksum-blob pattern, a write-back trigger in `RNSTransport::learnPath()` rate-limited to avoid rewriting on every announce, and a `PIN_QSPI_*` block in `RNSConfig.h` under `BOARD_IKOKA_STICK`. Not applicable to the WisBlock (RAK4631 has a 2 MB IS25 QSPI part too, but that is a separate pin map). |
| Boot | +5 ms flash start-up (`start_up_time_us` 5000) before mount. |
