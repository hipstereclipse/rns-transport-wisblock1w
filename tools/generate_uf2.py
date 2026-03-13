#!/usr/bin/env python3
"""
generate_uf2.py — PlatformIO post-build script.

Converts the compiled .hex firmware to a .uf2 file for safe drag-and-drop
flashing via the Adafruit nRF52 bootloader.

The UF2 format is designed so that an incomplete or interrupted transfer
cannot brick the device — the bootloader validates all blocks before
committing the firmware.

Usage:
    Automatically invoked by PlatformIO via extra_scripts in platformio.ini.
    Can also be called manually:
        python generate_uf2.py <input.hex> <output.uf2>
"""
import sys
import os
import struct

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILY  = 0x00002000
NRF52840_FAMILY  = 0xADA52840

def convert_hex_to_bin(hex_path):
    """Parse Intel HEX file into (base_address, data_bytes)."""
    data = bytearray()
    base_addr = None
    ext_addr = 0

    with open(hex_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line.startswith(':'):
                continue
            rec = bytes.fromhex(line[1:])
            length = rec[0]
            addr = (rec[1] << 8) | rec[2]
            rtype = rec[3]

            if rtype == 0x00:  # Data
                full_addr = ext_addr + addr
                if base_addr is None:
                    base_addr = full_addr
                # Pad if there are gaps
                offset = full_addr - base_addr
                if offset > len(data):
                    data.extend(b'\xFF' * (offset - len(data)))
                elif offset < len(data):
                    # Overwrite
                    pass
                data[offset:offset + length] = rec[4:4 + length]
            elif rtype == 0x02:  # Extended segment address
                ext_addr = ((rec[4] << 8) | rec[5]) << 4
            elif rtype == 0x04:  # Extended linear address
                ext_addr = ((rec[4] << 8) | rec[5]) << 16
            elif rtype == 0x01:  # EOF
                break

    return base_addr or 0, bytes(data)


def bin_to_uf2(data, base_addr, family_id=NRF52840_FAMILY):
    """Convert binary data to UF2 blocks."""
    BLOCK_SIZE = 256
    blocks = []
    num_blocks = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE

    for i in range(num_blocks):
        offset = i * BLOCK_SIZE
        chunk = data[offset:offset + BLOCK_SIZE]
        if len(chunk) < BLOCK_SIZE:
            chunk = chunk + b'\x00' * (BLOCK_SIZE - len(chunk))

        flags = UF2_FLAG_FAMILY
        block = struct.pack('<IIIIIIII',
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            flags,
            base_addr + offset,
            BLOCK_SIZE,
            i,
            num_blocks,
            family_id
        )
        block += chunk
        block += b'\x00' * (512 - 32 - 256 - 4)  # padding
        block += struct.pack('<I', UF2_MAGIC_END)
        blocks.append(block)

    return b''.join(blocks)


def main():
    if len(sys.argv) >= 3:
        # Manual invocation
        hex_path = sys.argv[1]
        uf2_path = sys.argv[2]
    else:
        # PlatformIO post-build: read from env
        try:
            Import = __builtins__.__import__  # noqa
        except:
            pass

        # When called by PlatformIO, arguments are different
        print("UF2 generator: use 'python generate_uf2.py input.hex output.uf2'")
        return

    if not os.path.exists(hex_path):
        print(f"Error: {hex_path} not found")
        sys.exit(1)

    print(f"Converting {hex_path} → {uf2_path}")
    base_addr, data = convert_hex_to_bin(hex_path)
    uf2_data = bin_to_uf2(data, base_addr)

    with open(uf2_path, 'wb') as f:
        f.write(uf2_data)

    print(f"  Base address: 0x{base_addr:08X}")
    print(f"  Firmware size: {len(data)} bytes")
    print(f"  UF2 blocks: {len(uf2_data) // 512}")
    print(f"  Output: {uf2_path} ({len(uf2_data)} bytes)")


# PlatformIO post-build hook
try:
    Import("env")

    def post_build_uf2(source, target, env):
        firmware_hex = str(target[0]).replace(".elf", ".hex")
        firmware_uf2 = str(target[0]).replace(".elf", ".uf2")
        if os.path.exists(firmware_hex):
            base_addr, data = convert_hex_to_bin(firmware_hex)
            uf2_data = bin_to_uf2(data, base_addr)
            with open(firmware_uf2, 'wb') as f:
                f.write(uf2_data)
            print(f"UF2 created: {firmware_uf2} ({len(uf2_data)} bytes)")

    env.AddPostAction("buildprog", post_build_uf2)
except:
    pass

if __name__ == '__main__':
    main()
