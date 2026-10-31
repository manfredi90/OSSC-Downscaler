#!/usr/bin/env python3
"""Sanity checks for a secondary-slot OSSC firmware image (.bin).

Mirrors what the stock firmware checks before/after flashing, so a bad image
is caught in CI instead of on the board.
"""
import struct
import sys
import zlib

SECONDARY_MAX = 0x70000          # 0x80000-0xEFFFF (0xF0000 = settings sector)
FINGERPRINT = 0xEFEFEF56         # read by "Launch 2nd FW" at slot + 0x20
SW_OFFSET = 0x50000


def bitswap8(b):
    return int(f"{b:08b}"[::-1], 2)


def main(path):
    img = open(path, "rb").read()
    hdr, data = img[:512], img[512:]
    key = hdr[0:4]
    major, minor = hdr[4], hdr[5]
    suffix = hdr[6:14].split(b"\0")[0].decode()
    hdr_len, data_len, data_crc = struct.unpack(">III", hdr[14:26])
    hdr_crc = struct.unpack(">I", hdr[508:512])[0]
    print(f"{path}: key={key.decode()} v{major}.{minor}-{suffix} data={data_len} bytes")

    errors = []
    if key != b"OSS2":
        errors.append(f"key is {key!r}, must be b'OSS2' (secondary slot)")
    if zlib.crc32(hdr[:hdr_len]) != hdr_crc:
        errors.append("header CRC mismatch")
    if len(data) != data_len:
        errors.append(f"payload is {len(data)} bytes, header says {data_len}")
    if zlib.crc32(data) != data_crc:
        errors.append("data CRC mismatch")
    if data_len > SECONDARY_MAX:
        errors.append(f"image larger than secondary slot ({data_len} > {SECONDARY_MAX})")
    # The updater bit-swaps every byte on its way to flash.
    fp = bytes(bitswap8(b) for b in data[0x20:0x24])
    if struct.unpack("<I", fp)[0] != FINGERPRINT:
        errors.append(f"bitstream fingerprint {fp.hex()} != {FINGERPRINT:08x}")
    if len(data) <= SW_OFFSET:
        errors.append("no SW image at offset 0x50000")
    else:
        rbf_end = len(data[:SW_OFFSET].rstrip(b"\xff"))
        print(f"  bitstream ~{rbf_end} bytes (limit {SW_OFFSET}), SW {len(data) - SW_OFFSET} bytes")

    for e in errors:
        print("ERROR:", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
