# RatTunnel V. 1.0.33 — WisBlock 1W

`RatTunnel` is a standalone Reticulum transport/repeater/node firmware for the RAKwireless WisBlock 1W stack (`RAK3401 + RAK13302`).
The name **RatTunnel** is a nod to [Ratspeak](https://github.com/ratspeak/), a messaging app/platform built on [Reticulum](https://github.com/markqvist/Reticulum/).

https://github.com/markqvist/Reticulum

It is designed for safe field updates with:
- browser-based flashing (Chrome/Edge)
- local firmware hosting via `tools/serve_flasher.py`
- UF2 drag-and-drop fallback
- bootloader-safe update flow (non-bricking)

## Current Status

- Firmware brand/version: `RatTunnel V. 1.0.33`
- Hardware target: WisBlock 1W (`nRF52840 + SX1262`)
- Primary flashing path: Web flasher (`flasher/index.html`) served over HTTP
- Secondary flashing path: UF2 drag-and-drop bootloader drive
- OTA helper script: `tools/ota_update.py`

## What RatTunnel Currently Supports

- Reticulum-compatible announce handling, route learning, and path table persistence (up to 200 entries, 24 h expiry)
- Packet forwarding/relay for mesh/backbone transport with duplicate suppression
- Announce relay queue with jitter and hop-count-aware flooding (32-slot queue)
- Active peer discovery via periodic discovery sweeps; nearby nodes replay cached signed announces
- Configurable periodic announce and discovery cadence, adjustable at runtime and persisted to flash
- **Encrypted peer-to-peer messaging** using X25519 + Ed25519 (per-peer public keys extracted from announces)
- Broadcast announce-sideband messaging for peers without known keys
- Per-peer addressing by hash prefix or name (`msg @<hash>`, `msg @<name>`)
- Direct encrypted chat retries transient send failures, and the browser flasher waits for peer ACK before marking delivery complete
- Auto-reply to safe diagnostic prompts (`RT?PING7`, `RT?UP7`, `RT?MESH7`) — encrypted, rate-limited
- RSSI and SNR tracked per peer in the routing table
- USB serial command console with rich command set (see table below)
- Runtime radio parameter changes and persistence to flash (LittleFS)
- Per-channel LED behavior: idle mode (off / solid / heartbeat), RX/TX flash, Morse alert mode
- Morse code blink notifications for incoming messages and fault codes on-air
- LED incoming-message alert system with watch-list, repeat-count, and sticky modes
- **RatHole security mode**: optional identity wipe-on-boot for operational security
- Hardware watchdog (8 s timeout) — device auto-recovers from hangs
- Safe reboot into Adafruit nRF52 DFU bootloader (`dfu` command or button)

## Network Scope

This firmware target is for the RAK3401 + RAK13302 LoRa stack. It does not include an on-device Wi-Fi interface or TCP/IP stack for direct connection to a LAN or Reticulum TCP server.

To reach the Ratspeak Hub, use the host computer's network connection together with the built-in browser bridge:
- Run `python tools/serve_flasher.py --hub-bridge`
- Open the flasher UI and use the `Hub` tab
- The local WebSocket bridge forwards packets between the USB-connected radio and `rns.ratspeak.org:4242`

This gives the practical result most users want: the WisBlock radio participates in LoRa locally, and your network-connected computer bridges it into the wider Ratspeak/Reticulum hub.

## LED Behavior

The WisBlock 1W exposes two physical LEDs (green = P35, blue = P36). The firmware controls them independently per-channel:

| State | Meaning |
|---|---|
| Green heartbeat (2 s) | Running normally |
| Blue pulse | Packet activity (RX or TX) |
| Green Morse digit loop | Fault code (e.g. `1` = radio init failed) |
| Alternating green/blue | Fatal error |

LED behavior is configurable at runtime with the `led` command and persisted to flash with `save`. Each channel supports:
- **Idle mode**: off, solid, or 2-second heartbeat blink
- **RX/TX flash**: brief pulse on packet activity
- **Morse alert mode**: blink Morse code for incoming messages (`incoming`), fault codes (`errors`), or both

### Incoming message alerts

The `led alert` system can:
- Blink a Morse alert immediately on every incoming message (`once`)
- Repeat the alert a configurable number of times at a configurable interval (`count`)
- Continue blinking until the conversation is opened (`until-clear`)
- Filter alerts to specific sender hash prefixes, optionally per LED channel (`led alert watch <hash16> [green+blue+red|auto]`)

## Peer Messaging

RatTunnel supports text messaging over the Reticulum radio link:

- `msg <text>` — broadcast to all peers; automatically uses encrypted path for peers with known public keys
- `msg @<hash> <text>` or `msg @<name> <text>` — address a specific peer; sends encrypted if key is available, falls back to broadcast sideband
- Encrypted messages use X25519 (key exchange) + Ed25519 (identity) keys extracted from announce packets
- Peer public keys are stored in the path table and survive path refreshes
- The browser flasher wraps direct encrypted chat with delivery ACKs and bounded retries; both peers must run current firmware for end-to-end delivery confirmation

### Auto-reply diagnostic prompts

Any peer can send the following exact strings to trigger a safe automated encrypted reply:

| Prompt | Reply | Meaning |
|---|---|---|
| `RT?PING7` | `RT!PONG7 ok` | Connectivity check |
| `RT?UP7` | `RT!UP7 <fresh\|warm\|steady\|longrun>` | Uptime bucket |
| `RT?MESH7` | `RT!MESH7 <none\|few\|many>` | Path table size |

Replies are encrypted-only to authenticated peers, coarse-grained, and rate-limited per sender (15 s cooldown).

## RatHole Security Mode

RatHole provides a lightweight operational security feature for nodes that need to avoid persistent identity:

```bash
rathole on               # enable RatHole mode
rathole boot-reset on    # wipe identity and config on every reboot
save                     # persist the setting
```

When `boot-reset` is on, each power cycle generates a fresh identity key pair — the node appears as a different Reticulum destination on every boot. The RatHole flag itself is preserved across wipes so the setting survives reboot.

## Hardware Requirements

| Component | Part Number |
|---|---|
| WisMesh 1W Booster Starter Kit | RAK10724 |
| or Core | RAK3401 |
| and IO module | RAK13302 |
| and Base board | RAK19007 |
| Power | USB-C (recommended 5V/1.5A+) or LiPo |

> Important: the 1W radio path can draw high peak current. Use a stable power source.

## Build Firmware Locally

```bash
pip install platformio
pio run -e wisblock_1w_transport
```

Build outputs are generated under `.pio/build/wisblock_1w_transport/`, including:
- `firmware.hex`
- `firmware.uf2` (generated by `tools/generate_uf2.py` post-build)

## Use `serve_flasher.py` (Recommended)

`tools/serve_flasher.py` launches a local HTTP server rooted at the workspace so modern browser APIs are available.

### Why this script exists

Opening `flasher/index.html` directly with `file://` is unreliable for Web Serial + File System Access workflows. Serving over HTTP is the most consistent setup.

### Start server

```bash
python tools/serve_flasher.py
```

Default behavior:
- binds to `127.0.0.1:8000`
- serves workspace root
- opens browser automatically to `http://127.0.0.1:8000/flasher/`

### Common options

```bash
python tools/serve_flasher.py --host 0.0.0.0 --port 8080
python tools/serve_flasher.py --no-open
python tools/serve_flasher.py --hub-bridge
python tools/serve_flasher.py --hub-bridge --hub-ws-port 8765 --hub-tcp-host rns.ratspeak.org --hub-tcp-port 4242
```

| Flag | Default | Purpose |
|---|---|---|
| `--host` | `127.0.0.1` | Bind interface (`0.0.0.0` = LAN-visible) |
| `--port` | `8000` | HTTP server port |
| `--no-open` | off | Do not auto-launch browser |
| `--hub-bridge` | off | Also start local WebSocket→TCP bridge for Ratspeak Hub |
| `--hub-ws-host` | `127.0.0.1` | WebSocket bind host for bridge |
| `--hub-ws-port` | `8765` | WebSocket bind port for bridge |
| `--hub-tcp-host` | `rns.ratspeak.org` | Upstream hub hostname |
| `--hub-tcp-port` | `4242` | Upstream hub TCP port |

## Web Flasher Workflow

1. Start server with `python tools/serve_flasher.py`
2. Open Chrome or Edge desktop
3. Choose a release version (or local file)
4. Optional: click `Connect device` for auto DFU trigger
5. Click `Flash firmware`
6. If prompted, click `Write to device drive` and choose `RAK4631` bootloader drive
7. Wait for success and automatic reboot

### Repeater settings menu (connectivity)

After serial connect and repeater detection, switch to **Repeater mode** to use the settings menu:
- Choose a connectivity profile (`RNode EU` / `RNode US` / `Ratspeak US balanced`) or keep `Custom`
- Edit frequency/SF/BW/CR/TX/sync word and optional broadcast name
- Use `Apply + Save` to write settings and persist in one step
- Use `Announce now` to immediately test visibility from peers
- Use `Verify Link` to run `radio`, `status`, and `announce` in one click with pass/fail hints
- Repeater mode now shows a **Detected nodes** area populated from live routing table entries

### Browser/API requirements

- Supported: Chrome / Edge (desktop, Web Serial enabled)
- Not supported: Safari, Firefox for this flow

## How the Flasher Selects Firmware

The web UI:
1. queries GitHub Releases API for this repo
2. filters releases that include upload-complete assets ending in `.uf2`, `.bin`, or `.hex`
3. falls back to `flasher/firmware/latest.uf2` from `main` if no formal release is found
4. allows local file upload as final fallback

## Publishing a Public Build (Release Path)

To make the new build available publicly from the version picker:

1. Build firmware (`pio run -e wisblock_1w_transport`)
2. Copy/update `flasher/firmware/latest.uf2`
3. Commit + push to GitHub
4. Create a GitHub Release and attach firmware assets (`.uf2` preferred)

Once a release exists with valid assets, the web flasher will list it automatically.

## Console Commands (Runtime)

### Core status and identity

| Command | Purpose |
|---|---|
| `status` / `stats` | Core node counters, radio health, uptime, free RAM |
| `radio` | Current LoRa parameters and last RSSI/SNR |
| `routes` | Routing table (dest hash, hops, age) |
| `peers` | Known peers with name, hops, RSSI, SNR, age |
| `identity` | Node identity hash and public keys |
| `version` | Firmware brand, version, build tag |
| `test` / `ping` | One-line health response (machine-readable) |

### Radio configuration

| Command | Purpose |
|---|---|
| `set freq\|sf\|bw\|cr\|txpower\|syncword\|preamble <value>` | Live radio tuning |
| `profile rnode-eu\|rnode-us\|ratspeak-us` | One-shot Reticulum LoRa preset |
| `power [announce\|discover <seconds>]` | Show or set periodic announce/discovery cadence |

### Naming and messaging

| Command | Purpose |
|---|---|
| `name [text]` | Show or set the node broadcast name |
| `msg <text>` | Broadcast message to all peers |
| `msg @<hash\|name> <text>` | Send message to specific peer (encrypted if key known) |
| `notify sound\|morse\|both\|silent` | Configure incoming message notification mode |
| `announce [payload]` | Transmit local announce (optional raw payload) |
| `discover` | Send manual discovery sweep to elicit peer announce replies |

### LED and Morse

| Command | Purpose |
|---|---|
| `led` | Show per-channel LED configuration and alert status |
| `led <green\|blue> idle <off\|solid\|heartbeat>` | Set channel idle mode |
| `led <green\|blue> rx <on\|off>` | Enable/disable RX-activity flash |
| `led <green\|blue> tx <on\|off>` | Enable/disable TX-activity flash |
| `led <green\|blue> morse <off\|errors\|incoming\|both>` | Set channel Morse blink mode |
| `led alert mode <once\|count\|until-clear>` | Alert repeat strategy |
| `led alert count <n>` | Repeat count for `count` mode |
| `led alert interval <s>` | Seconds between alert repeats |
| `led alert watch <add\|del\|clear> [hash16] [green+blue+red\|auto]` | Manage sender watch list and optional LED target mask |
| `led alert clear` | Clear pending alerts |
| `morse mode <off\|errors\|incoming\|both\|default>` | Set Morse mode for all available LEDs |
| `morse default <message\|clear>` | Set or clear the default Morse message |
| `morse test [message]` | Queue a Morse blink test immediately |

### Security

| Command | Purpose |
|---|---|
| `rathole` | Show RatHole security state |
| `rathole <on\|off>` | Enable/disable RatHole mode |
| `rathole boot-reset <on\|off>` | Wipe identity and config on every boot |

### Persistence and device management

| Command | Purpose |
|---|---|
| `save` | Persist all current config to flash |
| `factory-reset` | Erase stored identity and config |
| `dfu` | Reboot into Adafruit nRF52 bootloader |
| `reboot` | Restart firmware |
| `reinit` | Retry radio hardware init without rebooting |

### Diagnostics (advanced)

| Command | Purpose |
|---|---|
| `rxdiag` | Dump last raw received packet details |
| `irqmon [on\|off]` | Toggle IRQ monitoring output |
| `rxraw` | Print next raw received frame |
| `txraw <hex>` | Transmit raw hex-encoded frame |
| `pktdump [on\|off]` | Toggle packet dump on all received frames |
| `nfloor` | Measure noise floor (RSSI with TX off) |
| `regdump` | Dump SX1262 register state |
| `lorascan` | Scan common LoRa frequencies for activity |
| `pintest` | Full 48-pin GPIO scan to identify SX1262 wiring |

## Troubleshooting

### Flasher page loads but connect fails
- Ensure Chrome or Edge desktop is used
- Close other serial terminal apps that may own the port
- Replug USB cable and retry `Connect device`

### `Read error: The device has been lost`
- This usually means the USB serial link briefly dropped (cable movement, power dip, or device reset)
- The flasher now attempts to auto-reconnect to previously authorized serial ports
- If disconnects repeat, use a shorter/high-quality USB cable and a stable 5V supply (1.5A+ recommended)
- Avoid running multiple serial tools at the same time (browser + terminal app)

### Node does not appear on a known-good Reticulum LoRa device
- Ensure on-air parameters match exactly on both sides: frequency, bandwidth, spreading factor, coding rate, and sync word
- In console, run `radio` to inspect current settings
- Fast path: apply one-shot preset with `profile rnode-eu`, `profile rnode-us`, or `profile ratspeak-us`
- Set values as needed, for example: `set freq 915`, `set sf 9`, `set bw 125`, `set cr 5`, `set txpower 17`, `set syncword 0x12`, `set preamble 18`
- If `announce` causes reconnect/reboot, lower TX power first (17 dBm or less) and ensure stable USB power/cable
- `announce` now transmits with a temporary safe TX cap to reduce brownout-triggered resets, then restores configured TX power
- Run `save` to persist settings, then `announce` to broadcast immediately

### Flash button disabled
- Select a release version or enable local file mode
- Wait for release metadata fetch to complete
- Check network access to GitHub API/releases

### Device does not enter DFU automatically
- Use `Enter DFU mode` button
- If needed, double-tap hardware reset button
- Confirm bootloader USB drive appears (`RAK4631` or similar)

### `Write to device drive` cannot find drive
- Re-enter DFU mode and wait a few seconds
- Reconnect USB cable
- Manually download UF2 and drag/drop to bootloader drive

### Firmware update interrupted
- Re-enter DFU mode and repeat flash
- Bootloader remains intact; previous app typically remains recoverable

### No releases shown in picker
- Verify release assets are uploaded and not drafts
- Confirm filenames end with `.uf2`, `.bin`, or `.hex`
- Use local file fallback immediately if needed

## Safety Model

- Bootloader is not overwritten by this project
- Watchdog protects against stalled main loop
- Radio init failure still leaves serial console available
- DFU mode is reachable from command (`dfu`) or double-reset

## Repo Layout

```text
platformio.ini
include/
src/
flasher/
tools/
docs/
test/
```

## License

Apache 2.0. See dependency licenses for bundled/linked libraries.
