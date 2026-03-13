#!/usr/bin/env python3
"""
generate_uf2.py — PlatformIO post-build script.

Converts the compiled .hex firmware to a .uf2 file for safe drag-and-drop
flashing via the Adafruit nRF52 bootloader.

Also syncs flasher/firmware/latest.uf2 so the web flasher always serves
the latest locally built artifact.

Usage:
    Automatically invoked by PlatformIO via extra_scripts in platformio.ini.
    Can also be called manually:
        python tools/generate_uf2.py <input.hex> <output.uf2>
"""
from __future__ import annotations

import pathlib
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
NRF52840_FAMILY = 0xADA52840


def convert_hex_to_bin(hex_path: str):
    """Parse Intel HEX file into (base_address, data_bytes)."""
    data = bytearray()
    base_addr = None
    ext_addr = 0

    with open(hex_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line.startswith(":"):
                continue
            rec = bytes.fromhex(line[1:])
            length = rec[0]
            addr = (rec[1] << 8) | rec[2]
            rtype = rec[3]

            if rtype == 0x00:  # Data
                full_addr = ext_addr + addr
                if base_addr is None:
                    base_addr = full_addr
                offset = full_addr - base_addr
                if offset > len(data):
                    data.extend(b"\xFF" * (offset - len(data)))
                data[offset : offset + length] = rec[4 : 4 + length]
            elif rtype == 0x02:  # Extended segment address
                ext_addr = ((rec[4] << 8) | rec[5]) << 4
            elif rtype == 0x04:  # Extended linear address
                ext_addr = ((rec[4] << 8) | rec[5]) << 16
            elif rtype == 0x01:  # EOF
                break

    return base_addr or 0, bytes(data)


def bin_to_uf2(data: bytes, base_addr: int, family_id: int = NRF52840_FAMILY) -> bytes:
    """Convert binary data to UF2 blocks."""
    block_size = 256
    blocks = []
    num_blocks = (len(data) + block_size - 1) // block_size

    for i in range(num_blocks):
        offset = i * block_size
        chunk = data[offset : offset + block_size]
        if len(chunk) < block_size:
            chunk = chunk + b"\x00" * (block_size - len(chunk))

        block = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY,
            base_addr + offset,
            block_size,
            i,
            num_blocks,
            family_id,
        )
        block += chunk
        block += b"\x00" * (512 - 32 - 256 - 4)
        block += struct.pack("<I", UF2_MAGIC_END)
        blocks.append(block)

    return b"".join(blocks)


def generate_uf2(hex_path: pathlib.Path, uf2_path: pathlib.Path) -> bytes:
    if not hex_path.exists():
        raise FileNotFoundError(f"HEX input not found: {hex_path}")

    base_addr, data = convert_hex_to_bin(str(hex_path))
    uf2_data = bin_to_uf2(data, base_addr)

    uf2_path.parent.mkdir(parents=True, exist_ok=True)
    uf2_path.write_bytes(uf2_data)

    print(f"UF2 created: {uf2_path} ({len(uf2_data)} bytes)")
    return uf2_data


def sync_latest_flasher_uf2(uf2_data: bytes, script_path: pathlib.Path) -> None:
    workspace_root = script_path.resolve().parent.parent
    flasher_latest = workspace_root / "flasher" / "firmware" / "latest.uf2"
    flasher_latest.parent.mkdir(parents=True, exist_ok=True)
    flasher_latest.write_bytes(uf2_data)
    print(f"Flasher latest synced: {flasher_latest} ({len(uf2_data)} bytes)")


def manual_main() -> int:
    if len(sys.argv) < 3:
        print("UF2 generator usage: python tools/generate_uf2.py <input.hex> <output.uf2>")
        return 1

    hex_path = pathlib.Path(sys.argv[1])
    uf2_path = pathlib.Path(sys.argv[2])
    print(f"Converting {hex_path} -> {uf2_path}")
    uf2_data = generate_uf2(hex_path, uf2_path)
    sync_latest_flasher_uf2(uf2_data, pathlib.Path(__file__))
    return 0


try:
    Import("env")

    def _script_path_from_env(env) -> pathlib.Path:
        """Resolve this script's path inside PlatformIO, which has no __file__."""
        # extra_scripts paths are stored in the environment
        for raw in (env.get("EXTRA_SCRIPTS", "") or "").split(","):
            raw = raw.strip()
            if raw.endswith("generate_uf2.py"):
                return pathlib.Path(raw).resolve()

        # Fallback: infer from project root
        proj_dir = pathlib.Path(env.subst("$PROJECT_DIR"))
        return proj_dir / "tools" / "generate_uf2.py"

    def post_build_uf2(source, target, env):
        build_dir = pathlib.Path(env.subst("$BUILD_DIR"))
        progname = env.subst("$PROGNAME")
        firmware_hex = build_dir / f"{progname}.hex"
        firmware_uf2 = build_dir / f"{progname}.uf2"

        try:
            uf2_data = generate_uf2(firmware_hex, firmware_uf2)
            sync_latest_flasher_uf2(uf2_data, _script_path_from_env(env))
        except Exception as exc:
            print(f"UF2 generation failed: {exc}")

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", post_build_uf2)
except Exception:
    pass


if __name__ == "__main__":
    raise SystemExit(manual_main())
