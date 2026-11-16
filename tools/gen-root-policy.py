#!/usr/bin/env python3
"""Sign the OS and SystemOS windows of the nabu userdata image as VerifiedRoot policies."""
import sys
from pathlib import Path

sys.path.insert(0, "/root/fresh/UniXNU/kits/VerifiedRoot/tools")
import manifest

IMAGE = Path("/root/fresh/UniXNU/out/nabu/ipados-17.7.11-21H461/nabu/userdata/userdata-root.img")
OUT = Path("/root/fresh/UniXNU/out/nabu/ipados-17.7.11-21H461/nabu/root-policy")
KEY = Path("/root/official/UniXNU-nabu/keys/owner.key")
PUB = Path("/root/official/UniXNU-nabu/keys/owner.pub")

CASES = [
    ("OS", 0x100000, 6077580800, "04769D26-2CA0-4825-B443-E2E5F88300F2"),
    ("SystemOS", 0x16A600000, 3303014400, "8846FE78-2910-4297-AF97-D59BB6F8FC16"),
]


class Limited:
    def __init__(self, f, n):
        self.f = f
        self.left = n

    def read(self, k):
        if self.left <= 0:
            return b""
        data = self.f.read(min(k, self.left))
        self.left -= len(data)
        return data


def main():
    with IMAGE.open("rb") as f:
        for name, offset, size, volume in CASES:
            f.seek(offset)
            record = manifest.make(Limited(f, size), size, KEY, PUB, volume, 1, OUT / name)
            print(f"{name}: blocks={record['block_count']} table={record['hash_table_sha256']}", flush=True)


if __name__ == "__main__":
    main()
