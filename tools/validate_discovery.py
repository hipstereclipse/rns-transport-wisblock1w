#!/usr/bin/env python3
"""
validate_discovery.py — check a RatTunnel `discovery dump` against the
reference RNS implementation (needs `pip install rns lxmf`).

    python tools/validate_discovery.py < dump.txt
    python tools/validate_discovery.py --appdata <hex> [--dest <hex>] [--identity <pubkey hex>]

Paste the console output of `discovery dump` (the PACKED / INFOHASH /
APPDATA / DEST lines) on stdin. The script re-runs exactly what
RNS.Discovery.InterfaceAnnounceHandler.received_announce does: flags,
stamp split, infohash, workblock (LXStamper, 20 expand rounds), stamp
value against DEFAULT_STAMP_VALUE, msgpack unpack and the field checks.
"""
import sys, argparse, hashlib

try:
    import RNS
    from RNS.Discovery import InterfaceAnnouncer, InterfaceAnnounceHandler
    from RNS.vendor import umsgpack as msgpack
except ImportError as e:
    print("Reference RNS not importable:", e); sys.exit(2)

# Stamp primitives transcribed from LXMF/LXStamper.py (master), which is
# what RNS.Discovery calls with expand_rounds=20. Kept local so an older
# installed LXMF (whose stamp_workblock has no expand_rounds) cannot skew
# the check.
class LXStamper:
    STAMP_SIZE = RNS.Identity.HASHLENGTH // 8          # 32 bytes
    @staticmethod
    def stamp_workblock(message_id, expand_rounds):
        wb = b""
        for n in range(expand_rounds):
            wb += RNS.Cryptography.hkdf(length=256, derive_from=message_id,
                                        salt=RNS.Identity.full_hash(message_id + msgpack.packb(n)), context=None)
        return wb
    @staticmethod
    def stamp_value(workblock, stamp):
        value = 0; bits = 256
        i = int.from_bytes(RNS.Identity.full_hash(workblock + stamp), byteorder="big")
        while (i & (1 << (bits - 1))) == 0 and value < bits:
            i = (i << 1); value += 1
        return value
    @staticmethod
    def stamp_valid(stamp, target_cost, workblock):
        target = 0b1 << 256 - target_cost
        result = RNS.Identity.full_hash(workblock + stamp)
        return int.from_bytes(result, byteorder="big") <= target

KEYS = {0x00:"INTERFACE_TYPE",0x01:"TRANSPORT",0x02:"REACHABLE_ON",0x03:"LATITUDE",0x04:"LONGITUDE",
        0x05:"HEIGHT",0x06:"PORT",0x07:"IFAC_NETNAME",0x08:"IFAC_NETKEY",0x09:"FREQUENCY",0x0A:"BANDWIDTH",
        0x0B:"SPREADINGFACTOR",0x0C:"CODINGRATE",0x0D:"MODULATION",0x0E:"CHANNEL",0xF0:"OP_ADDR",
        0xFC:"TRANSPORT_VERS",0xFD:"TRANSPORT_IMPL",0xFE:"TRANSPORT_ID",0xFF:"NAME"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--appdata"); ap.add_argument("--dest"); ap.add_argument("--identity")
    a = ap.parse_args()
    fields = {}
    if not a.appdata:
        for line in sys.stdin:
            parts = line.strip().split()
            if len(parts) == 2 and parts[0] in ("PACKED","INFOHASH","APPDATA","DEST"):
                fields[parts[0]] = parts[1]
        a.appdata = fields.get("APPDATA"); a.dest = fields.get("DEST")
    if not a.appdata:
        print("no APPDATA"); sys.exit(2)
    app_data = bytes.fromhex(a.appdata)
    ok = True

    flags = app_data[0]; body = app_data[1:]
    print(f"flags 0x{flags:02X}  signed={bool(flags & InterfaceAnnounceHandler.FLAG_SIGNED)} encrypted={bool(flags & InterfaceAnnounceHandler.FLAG_ENCRYPTED)}")
    ssz = LXStamper.STAMP_SIZE
    stamp = body[-ssz:]; packed = body[:-ssz]
    infohash = RNS.Identity.full_hash(packed)
    print(f"packed {len(packed)} B  stamp {len(stamp)} B  infohash {infohash.hex()}")
    if "INFOHASH" in fields and fields["INFOHASH"].lower() != infohash.hex():
        print("  MISMATCH: firmware INFOHASH differs from SHA256(packed)"); ok = False

    workblock = LXStamper.stamp_workblock(infohash, expand_rounds=InterfaceAnnouncer.WORKBLOCK_EXPAND_ROUNDS)
    value = LXStamper.stamp_value(workblock, stamp)
    valid = LXStamper.stamp_valid(stamp, InterfaceAnnouncer.DEFAULT_STAMP_VALUE, workblock)
    print(f"workblock {len(workblock)} B  stamp value {value} bits  required {InterfaceAnnouncer.DEFAULT_STAMP_VALUE}  -> {'VALID' if valid else 'INVALID'}")
    ok &= valid

    unpacked = msgpack.unpackb(packed)
    print("fields:")
    for k, v in unpacked.items():
        name = KEYS.get(k, f"0x{k:02X}")
        print(f"  {name:16s} {v.hex() if isinstance(v, bytes) else v!r}")

    # the handler's own checks
    try:
        if type(unpacked[0x01]) != bool: raise ValueError("TRANSPORT not bool")
        for k in (0x03, 0x04, 0x05):
            if type(unpacked[k]) not in [type(None), float]: raise ValueError(f"{KEYS[k]} not float/None")
        if len(unpacked[0xFE]) != RNS.Identity.TRUNCATED_HASHLENGTH // 8: raise ValueError("TRANSPORT_ID length")
        if unpacked[0x00] not in InterfaceAnnouncer.DISCOVERABLE_INTERFACE_TYPES: raise ValueError("interface type")
        print("handler field checks: PASS")
    except Exception as e:
        print("handler field checks: FAIL", e); ok = False

    if a.dest and a.identity:
        pub = bytes.fromhex(a.identity)
        ident = RNS.Identity(create_keys=False); ident.load_public_key(pub)
        d = RNS.Destination(ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "rnstransport", "discovery", "interface")
        print(f"dest hash from pubkey: {d.hash.hex()}  firmware: {a.dest}  {'MATCH' if d.hash.hex()==a.dest.lower() else 'MISMATCH'}")
        ok &= d.hash.hex() == a.dest.lower()
    print("RESULT:", "OK" if ok else "FAIL")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
