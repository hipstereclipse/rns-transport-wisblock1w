# CONVENTIONS.md — RatTunnel host conventions

Inventory for anyone adding a feature or a board. Short rules with a
pointer each; not documentation.

## Build / boards
- One PlatformIO env per hardware target, named `<board>_transport`
  (`wisblock_1w_transport`, `ikoka_stick_transport`). `native_test` for
  host tests. `platformio.ini` is flat: no `[env]` inheritance; copy the
  block and edit.
- Board and RF-module selection is a `-D` in the env's `build_flags`
  (`BOARD_IKOKA_STICK`, `RADIO_MODULE_E22_900M30S`) consumed only by
  `include/RNSConfig.h`. Source files never `#if` on a board name; they
  `#if` on capability macros the config header defines
  (`PIN_LORA_DIO1_ACTIVE`, `RADIO_HAS_RAK_PIN_DISCOVERY`, `PIN_* >= 0`,
  `LED_ACTIVE_HIGH`).
- Pin numbers are raw nRF52840 GPIO (pca10056 variant: P0.x = x,
  P1.x = 32 + x). Custom boards go in `boards/*.json` with provenance in
  `boards/README.md`; non-BSP SoftDevice headers in `lib/nrf52/`.
- Libraries: `jgromes/RadioLib` and `rweather/Crypto` only. New
  dependencies need an explicit decision.
- `tools/generate_uf2.py` runs as a post-script and overwrites
  `flasher/firmware/latest.uf2` with whichever env built last.

## Config constants
- All tunables are `#define`s in `include/RNSConfig.h`, grouped under
  `// ── Section ──` banners. Names: `LORA_*`, `RNS_*`, `PIN_*`,
  `ANNOUNCE_*`, `*_FILE`. Small enums live in the same header.
- TX power: `LORA_TX_DBM_VARIANT_MAX` (per RF module) ≥
  `LORA_TX_DBM_MAX_SAFE` (build cap) ≥ `LORA_TX_DBM` (default) and
  `LORA_TX_DBM_ANNOUNCE_SAFE`, enforced by `static_assert` in
  `RNSConfig.h` and at runtime by `RNSRadio::clampTxDbm()`, the only
  path to `lora.setOutputPower()` / `lora.begin()`. Never write the
  setpoint elsewhere.

## Modules
- Header-only classes in `include/RNS<Thing>.h` (`RNSRadio`,
  `RNSConsole`, `RNSPersistence`, …); one `.cpp` (`src/RNSTransport.cpp`)
  for the heavy transport bodies; `src/main.cpp` owns LEDs, morse, WDT,
  button, boot sequence. Static allocation, no heap after `setup()`.
- Board-specific hardware bring-up lives inside the module that owns
  the peripheral (`RNSRadio::begin()`), under a capability `#if`, not in
  a separate board file.

## Logging
- `Serial.print(F("[TAG] ..."))`. Tags: `[RNS]` lifecycle, `[DIAG]`
  radio/hardware detail, `[POWER]`, `[LED_CH]`, `[RATHOLE]`. Console
  replies use `io->println(F(...))` with no tag.

## Settings / persistence
- Adafruit InternalFS (LittleFS, 28 KB at 0xED000), one file per blob,
  names in `RNSConfig.h` (`*_FILE`). Blobs are packed structs with a
  trailing XOR `checksum`; bump `RADIO_CONFIG_VERSION` / `LED_CONFIG_VERSION`
  when a struct changes. Load = read, checksum, version-check, apply via
  the owning module's setters (`RNSPersistence::loadConfig`).
- Everything persisted is also settable from the console (`set`, `save`)
  and the web console (`flasher/index.html`).

## Console
- Commands are `else if (strcmp(command, "x") == 0) cmdX();` in
  `RNSConsole::handleLine`, one `cmdX()` method each, and a line in
  `cmdHelp()`. Radio params always go through `radio->setX()`.

## LEDs / button / DFU
- Three logical channels (green/blue/red) in `src/main.cpp`; a board
  sets `PIN_LED_*` (-1 = absent) and `LED_ACTIVE_HIGH`. Only
  `writeLedChannel()` and `setup()` touch the pins, via `ledLevel()`.
- DFU entry is `rnsRebootToDfu()` in `include/RNSDfu.h` (GPREGRET 0x57 +
  reset). Used by console `dfu` and, on boards with `PIN_USER_BUTTON`,
  by the boot-time hold check in `setup()`.

## Tests
- `test/*.cpp` build under `native_test` with `NATIVE_TEST` stubs in
  `RNSConfig.h`; hardware code is fenced with `#ifndef NATIVE_TEST`.
