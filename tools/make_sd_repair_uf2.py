#!/usr/bin/env python3
"""
make_sd_repair_uf2.py — build a SoftDevice-only repair UF2 for the XIAO
nRF52840 (Ikoka Stick), S140 v7.3.0.

    python tools/make_sd_repair_uf2.py <bootloader+sd .hex> [out.uf2]

Input is one of Seeed's / oltaco's XIAO bootloader release hex files that
bundles S140 7.3.0, e.g.
  xiao_nrf52840_ble_bootloader-0.9.2-OTAFIX1.2-BP1.2_s140_7.3.0.hex
(pick the same bootloader version the board reports in INFO_UF2.TXT).

Only flash 0x1000..0x27000 (the S140 body) is emitted. The MBR (<0x1000),
the application (>=0x27000), the bootloader and UICR are never touched, so
the RatTunnel app already on the board survives.

WHY THIS EXISTS
The WisBlock 1W image links at 0x26000 (S140 v6.1.1 app start). On a
XIAO, 0x26000..0x27000 is the LAST PAGE of S140 v7.3.0. Dragging the
WisBlock UF2 onto a XIAO therefore overwrites that page with an app
vector table while the SoftDevice's size record still says the SD ends
at 0x27000. RatTunnel's InternalFS layer calls into the SoftDevice at
boot, so the node hangs in setup() with a solid green LED and an
enumerated-but-dead USB CDC. Observed 2026-09-21; diagnosed from the
bootloader's CURRENT.UF2 dump. Drag the UF2 this script produces onto
XIAO-BOOT to restore the page.
"""
from __future__ import annotations

import hashlib
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import generate_uf2 as g  # noqa: E402

SD_BASE = 0x1000
SD_END = 0x27000          # S140 v7.3.0: SD_SIZE_GET() == 0x27000 (measured from 0x0)
SD_MAGIC = 0x51B1E5DB
SD_FWID_7_3_0 = 0x0123


def parse_hex(path: pathlib.Path) -> dict[int, int]:
    mem: dict[int, int] = {}
    ext = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        rec = bytes.fromhex(line[1:])
        n, addr, rtype = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if rtype == 4:
            ext = ((rec[4] << 8) | rec[5]) << 16
        elif rtype == 2:
            ext = ((rec[4] << 8) | rec[5]) << 4
        elif rtype == 0:
            for i in range(n):
                mem[ext + addr + i] = rec[4 + i]
    return mem


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    hex_path = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else hex_path.with_name("s140_7.3.0_softdevice_repair.uf2")

    mem = parse_hex(hex_path)
    img = bytes(mem.get(SD_BASE + i, 0xFF) for i in range(SD_END - SD_BASE))

    # SoftDevice info struct lives at SD_BASE + 0x2000 (nrf_sdm.h SOFTDEVICE_INFO_STRUCT_OFFSET)
    magic = struct.unpack("<I", img[0x2004:0x2008])[0]
    size = struct.unpack("<I", img[0x2008:0x200C])[0]
    fwid = struct.unpack("<H", img[0x200C:0x200E])[0]
    if magic != SD_MAGIC:
        print("ERROR: no SoftDevice info block at 0x3000 in %s" % hex_path)
        return 2
    if size != SD_END or fwid != SD_FWID_7_3_0:
        print("ERROR: expected S140 v7.3.0 (size 0x27000, fwid 0x0123); got size 0x%X fwid 0x%04X" % (size, fwid))
        return 2

    uf2 = g.bin_to_uf2(img, SD_BASE)
    addrs = [struct.unpack_from("<I", uf2, i + 12)[0] for i in range(0, len(uf2), 512)]
    assert min(addrs) == SD_BASE and max(addrs) + 256 == SD_END
    out.write_bytes(uf2)
    print("OK  %s" % out)
    print("    %d blocks, flash 0x%X..0x%X, fwid 0x%04X, sha256 %s"
          % (len(addrs), min(addrs), max(addrs) + 256, fwid, hashlib.sha256(uf2).hexdigest()[:16]))
    print("Drag onto the XIAO-BOOT drive. The app at 0x27000 is not modified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
