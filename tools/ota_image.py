#!/usr/bin/env python3
"""The image file an application feeds to ota_write() (hal/ota.h): a 112 byte header, the
RAM image and the flash image of an OTA build.

    tools/ota_image.py [--key KEY.pem] RAM.bin FLASH.bin OUT.nota     key: ota_key.pem

Header, little endian: "NOTA", version 2, RAM length, flash length, SHA-256 over the RAM
image followed by the flash image, and the ed25519 signature of those 32 digest bytes by
the key (tools/ota_keygen.py). The same two files tools/ota_table.py hashes and signs for
the slot table, so a slot written over the air carries what the selector checks."""
import sys, struct, hashlib
from cryptography.hazmat.primitives import serialization

def main():
    args = sys.argv[1:]; key_path = "ota_key.pem"
    if len(args) >= 2 and args[0] == "--key":
        key_path = args[1]; args = args[2:]
    if len(args) != 3:
        print(__doc__); sys.exit(2)
    key = serialization.load_pem_private_key(open(key_path, "rb").read(), None)
    ram = open(args[0], "rb").read(); flash = open(args[1], "rb").read()
    digest = hashlib.sha256(ram + flash).digest()
    header = b"NOTA" + struct.pack("<III", 2, len(ram), len(flash)) + digest + key.sign(digest)
    assert len(header) == 112
    with open(args[2], "wb") as f:
        f.write(header + ram + flash)
    print("ota image: %d bytes (RAM %d, flash %d), sha256 %s, signed with %s" % (len(header) + len(ram) + len(flash), len(ram), len(flash), digest.hex()[:16], key_path))

if __name__ == "__main__":
    main()
