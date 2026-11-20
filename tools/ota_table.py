#!/usr/bin/env python3
"""The OTA slot table of an OTA build (port/xtensa/ota_layout.h): one sector with the
slots' lengths and SHA-256 digests, a sequence number and a CRC-32.

    tools/ota_table.py OUT.bin --slot N RAM.bin FLASH.bin [--slot M RAM.bin FLASH.bin]
                       [--active N] [--trial N] [--sequence S] [--key KEY.pem]

Every slot's digest is signed with the ed25519 key (default ota_key.pem, tools/ota_keygen.py).

A slot given here is CONFIRMED unless --trial names it (then with OTA_TRIALS boots). The
layout constants are read from port/xtensa/ota_layout.h so that there is one place for
them. The second copy of the table is not written: the selector takes a copy whose magic
or CRC does not hold as absent, and a freshly erased sector is one."""
import sys, os, re, struct, hashlib, zlib
from cryptography.hazmat.primitives import serialization

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT = os.path.join(HERE, "..", "port", "xtensa", "ota_layout.h")

def constants():
    c = {}
    for m in re.finditer(r"#define\s+(OTA_\w+)\s+(0x[0-9A-Fa-f]+|\d+)", open(LAYOUT).read()):
        c[m.group(1)] = int(m.group(2), 0)
    return c

def main():
    c = constants()
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(2)
    out = args.pop(0)
    slots = [None] * c["OTA_SLOTS"]
    active, trial, sequence, key_path = None, None, 1, "ota_key.pem"
    while args:
        a = args.pop(0)
        if a == "--slot":
            n = int(args.pop(0)); ram = open(args.pop(0), "rb").read(); flash = open(args.pop(0), "rb").read()
            slots[n] = (ram, flash)
        elif a == "--active": active = int(args.pop(0))
        elif a == "--trial": trial = int(args.pop(0))
        elif a == "--sequence": sequence = int(args.pop(0))
        elif a == "--key": key_path = args.pop(0)
        else:
            sys.exit("unknown argument " + a)
    key = serialization.load_pem_private_key(open(key_path, "rb").read(), None)
    if active is None:
        active = next(i for i, s in enumerate(slots) if s is not None)
    body = struct.pack("<IIIII", c["OTA_TABLE_MAGIC"], c["OTA_TABLE_VERSION"], sequence, active,
                       c["OTA_TRIALS"] if trial is not None else 0)
    for i, s in enumerate(slots):
        if s is None:
            body += struct.pack("<III", c["OTA_SLOT_EMPTY"], 0, 0) + bytes(32) + bytes(64)
        else:
            ram, flash = s
            if len(ram) > c["OTA_SLOT_FLASH_OFF"]:
                sys.exit("RAM image of slot %d is %d bytes, more than the %d the slot has" % (i, len(ram), c["OTA_SLOT_FLASH_OFF"]))
            state = c["OTA_SLOT_TRIAL"] if trial == i else c["OTA_SLOT_CONFIRMED"]
            digest = hashlib.sha256(ram + flash).digest()
            body += struct.pack("<III", state, len(ram), len(flash)) + digest + key.sign(digest)
    body += struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    with open(out, "wb") as f:
        f.write(body)
    print("ota table: %d bytes, active slot %d%s, slots %s" % (len(body), active,
          " (trial %d)" % trial if trial is not None else "",
          ", ".join("%d: %d+%d B" % (i, len(s[0]), len(s[1])) for i, s in enumerate(slots) if s)))

if __name__ == "__main__":
    main()
