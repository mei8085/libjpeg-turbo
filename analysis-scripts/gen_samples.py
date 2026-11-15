#!/usr/bin/env python3
"""Generate JPEG resource-stress samples for libjpeg-turbo analysis.

Samples (written to the directory given as argv[1]):
  prog_base.jpg      - ordinary progressive JPEG (reference)
  scans_N.jpg        - progressive JPEG with the last scan repeated N times
  dht_N.jpg          - baseline JPEG with the DHT segment repeated N times
  bigdim_WxH.jpg     - progressive JPEG whose SOF declares WxH pixels
"""
import os
import struct
import subprocess
import sys

OUT = sys.argv[1]
BUILD = sys.argv[2]
os.makedirs(OUT, exist_ok=True)


def run(cmd):
    subprocess.run(cmd, check=True)


def make_pgm(path, w, h):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes([(x * 7 + 13) & 0xFF for x in range(w * h)]))


def segments(data):
    """Split a JPEG into (marker, payload-or-None, raw) list."""
    segs = []
    i = 0
    assert data[:2] == b"\xff\xd8"
    segs.append((0xD8, None, data[:2]))
    i = 2
    while i < len(data):
        assert data[i] == 0xFF, hex(data[i])
        m = data[i + 1]
        if m == 0xD9:  # EOI
            segs.append((m, None, data[i:i + 2]))
            i += 2
            break
        if m == 0xDA:  # SOS: header + entropy data up to next marker
            ln = struct.unpack(">H", data[i + 2:i + 4])[0]
            hdr = data[i:i + 2 + ln]
            j = i + 2 + ln
            # entropy-coded data ends at next 0xFF not followed by 0x00 or RSTn
            while True:
                k = data.index(0xFF, j)
                nxt = data[k + 1]
                if nxt == 0x00 or 0xD0 <= nxt <= 0xD7:
                    j = k + 2
                    continue
                break
            segs.append((m, None, data[i:k]))
            i = k
            continue
        ln = struct.unpack(">H", data[i + 2:i + 4])[0]
        segs.append((m, data[i + 4:i + 2 + ln], data[i:i + 2 + ln]))
        i += 2 + ln
    return segs


def join(segs):
    return b"".join(s[2] for s in segs)


# --- base images -----------------------------------------------------------
pgm = os.path.join(OUT, "ref.pgm")
make_pgm(pgm, 64, 64)
run([os.path.join(BUILD, "cjpeg-static"), "-grayscale", "-progressive",
     "-outfile", os.path.join(OUT, "prog_base.jpg"), pgm])
run([os.path.join(BUILD, "cjpeg-static"), "-grayscale",
     "-outfile", os.path.join(OUT, "base.jpg"), pgm])

base = open(os.path.join(OUT, "base.jpg"), "rb").read()
prog = open(os.path.join(OUT, "prog_base.jpg"), "rb").read()

# --- 1. repeated scans -----------------------------------------------------
psegs = segments(prog)
n_scans = sum(1 for s in psegs if s[0] == 0xDA)
print("progressive base has", n_scans, "scans")
sos_idx = [k for k, s in enumerate(psegs) if s[0] == 0xDA]
last_sos = psegs[sos_idx[-1]]
for n in (100, 1000, 5000):
    out = psegs[:-1] + [last_sos] * n + [psegs[-1]]
    open(os.path.join(OUT, "scans_%d.jpg" % (n + n_scans)), "wb").write(join(out))

# --- 2. repeated DHT --------------------------------------------------------
bsegs = segments(base)
dhts = [s for s in bsegs if s[0] == 0xC4]
print("baseline has", len(dhts), "DHT segments")
dht_blob = b"".join(s[2] for s in dhts)
for n in (100, 1000, 10000):
    out = []
    for s in bsegs:
        if s[0] == 0xC4:
            continue
        if s[0] == 0xDA and not any(x[0] == 0xC4 for x in out):
            out.append((0xC4, None, dht_blob * n))
        out.append(s)
    open(os.path.join(OUT, "dht_%d.jpg" % (n * len(dhts))), "wb").write(join(out))

# --- 3. huge dimensions, tiny entropy payload -------------------------------
for w, h, tag in ((30000, 30000, "30k"), (65500, 65500, "65k")):
    segs2 = []
    for s in psegs:
        if s[0] in (0xC0, 0xC1, 0xC2):
            p = bytearray(s[1])
            p[1:3] = struct.pack(">H", h)
            p[3:5] = struct.pack(">H", w)
            raw = b"\xff" + bytes([s[0]]) + struct.pack(">H", len(p) + 2) + bytes(p)
            segs2.append((s[0], bytes(p), raw))
        else:
            segs2.append(s)
    open(os.path.join(OUT, "bigdim_%s.jpg" % tag), "wb").write(join(segs2))

print("done")
