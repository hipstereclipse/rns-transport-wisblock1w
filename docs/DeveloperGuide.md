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
