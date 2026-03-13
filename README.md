# RNS Transport Node — WisBlock 1W

A standalone **Reticulum transport node** (mesh repeater) for the RAKwireless WisBlock 1W platform (RAK3401 + RAK13302), operating at **+30 dBm (1 W)** on LoRa with a 170 dB link budget.

## What It Does

This firmware turns a WisBlock 1W Starter Kit into a Reticulum backbone relay:

- **Receives** LoRa packets from any Reticulum node
- **Validates** announce signatures (Ed25519)
- **Learns** paths via announce propagation
- **Forwards** data packets toward their destinations
- **Deduplicates** to prevent packet storms
- Works transparently with **Ratspeak** and all other Reticulum apps

## Hardware Required

| Component | Part Number |
|-----------|-------------|
| WisMesh 1W Booster Starter Kit | RAK10724 |
| *or* WisBlock Core | RAK3401 |
| *and* WisBlock IO (1W LoRa) | RAK13302 |
| *and* WisBlock Base Board | RAK19007 |
| Power | USB-C or LiPo battery |

> **Important:** The RAK13302 draws up to 1 A at peak TX. Use a capable USB power source (5V/1.5A+) or a LiPo battery.

## Quick Start

### 1. Install PlatformIO

```bash
pip install platformio
```

### 2. Build and Flash

```bash
git clone <repo-url> && cd RNS-Transport-WisBlock

# Build
pio run

# Flash via USB (device must be connected)
pio run --target upload
```

Or use the **drag-and-drop method**: double-tap the reset button, then copy the generated `.uf2` file to the USB drive that appears.

### 3. Connect to Console

```bash
# Using the included tool
python tools/console_client.py

# Or any serial terminal at 115200 baud
screen /dev/ttyACM0 115200
```

### 4. Verify Operation

```
rns> status
── Transport Node Status ──
  Firmware:   0.2.0
  Uptime:     42 s
  RX packets: 0
  ...

rns> radio
── Radio Configuration ──
  Frequency: 915.0 MHz
  Bandwidth: 125.0 kHz
  SF:        8
  ...
```

## Web Flasher

Open `flasher/index.html` in Chrome/Edge (Web Serial API required) for a browser-based flashing experience. No drivers or toolchain needed.

## Console Commands

| Command | Description |
|---------|-------------|
| `status` | Node stats: packets, paths, memory |
| `routes` | Show routing table |
| `identity` | Display node identity hash and keys |
| `radio` | Current radio parameters |
| `set freq 915.0` | Change frequency (MHz) |
| `set sf 10` | Change spreading factor (5–12) |
| `set bw 125.0` | Change bandwidth (kHz) |
| `set txpower 22` | Change TX power (-9 to 22 dBm) |
| `save` | Persist config to flash |
| `factory-reset` | Erase identity and config |
| `dfu` | Enter bootloader for USB flashing |
| `reboot` | Restart node |
| `help` | List commands |

## Safety Guarantees

This firmware **cannot brick your device**:

- The bootloader is never modified
- Hardware watchdog recovers from any firmware hang
- Radio failure keeps the console alive for DFU
- Double-tap reset always enters the bootloader
- Corrupted flash data triggers fresh generation, not a crash

## Project Structure

```
├── platformio.ini          Build config
├── include/
│   ├── RNSConfig.h         Pins, constants, tuning
│   ├── RNSPacket.h         Wire-format parser
│   ├── RNSIdentity.h       Crypto identity
│   ├── RNSRadio.h          SX1262 driver
│   ├── RNSTransport.h      Routing engine
│   ├── RNSConsole.h        Serial CLI
│   └── RNSPersistence.h    Flash storage
├── src/
│   ├── main.cpp            Entry point
│   └── RNSTransport.cpp    Transport impl
├── test/                   Unit tests
├── tools/
│   ├── ota_update.py       OTA flashing script
│   ├── console_client.py   Serial terminal
│   └── generate_uf2.py     UF2 converter
├── flasher/
│   └── index.html          Web Serial flasher
├── docs/
│   ├── DeveloperGuide.md   Architecture docs
│   └── UserManual.pdf      Operation guide
└── README.md
```

## OTA Updates

```bash
# Via the included script (triggers DFU automatically)
python tools/ota_update.py --port /dev/ttyACM0 --hex .pio/build/wisblock_1w_transport/firmware.hex

# Or via console command + UF2
rns> dfu
# Then drag firmware.uf2 to the USB drive
```

## License

Apache 2.0. See individual library licenses for RadioLib and rweather/Crypto.

## Acknowledgments

- [Reticulum](https://reticulum.network/) by Mark Qvist
- [RadioLib](https://github.com/jgromes/RadioLib) by Jan Gromeš
- [microReticulum](https://github.com/attermann/microReticulum) by Scott Attermann
- [RAKwireless](https://www.rakwireless.com/) for the WisBlock platform
