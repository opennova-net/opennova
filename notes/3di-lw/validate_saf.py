#!/usr/bin/env python3
"""Validate Land Warrior SAF1 skeletal animation files.

From Dflw.exe `sub_44A0B0 @ 0x44a0b0` (anim loader):
  16-byte file header: magic "SAF1" (== dword_57DBFC) | u32 a | u32 frame_count | u32 root_block_size(=0x34)
  per frame: root block of `root_block_size` bytes, whose first u32 = n_parts (<=15),
             then n_parts * 4-byte part keyframes (byte part-id + i16 angle).
On-disk per-frame size = root_block_size + 4*n_parts. A complete file -> cursor lands on EOF.

The loader scales floats: translation *85.333, rotation *1365.333 -> i16; pitch clamped >= -30.
This script only checks structural integrity + that the files are NOT encrypted (plain "SAF1").
"""
import struct, sys, glob, os
from collections import Counter

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def parse(path):
    with open(path, "rb") as f:
        b = f.read()
    name, size = os.path.basename(path), len(b)
    if b[0:4] != b"SAF1":
        return name, -1, f"{name:16} NOT-SAF1 (magic={b[0:4]!r}) — possibly encrypted/other"
    a, frames, root = u32(b, 4), u32(b, 8), u32(b, 12)
    pos = 16
    for _ in range(frames):
        if pos + root > size:
            return name, frames, f"{name:16} OVERRUN at frame, pos={pos}/{size}"
        nparts = u32(b, pos)
        pos += root + 4 * nparts
    ok = pos == size
    return name, frames, (f"{name:16} frames={frames:<4} a={a:<4} root={root} "
                          f"{'OK' if ok else f'MISMATCH cur={pos}/{size}'}")

def main():
    args = sys.argv[1:] or [r"C:\Users\taylor\Desktop\DFLW\*.SAF", r"C:\Users\taylor\Desktop\DFLW\*.saf"]
    files = sorted({p for a in args for p in glob.glob(a)})
    bad = 0
    for p in files:
        _, _, line = parse(p)
        if not line.endswith("OK"):
            print(line); bad += 1
    print(f"\n{len(files)-bad}/{len(files)} SAF files structurally valid (plain SAF1, not encrypted)")

if __name__ == "__main__":
    main()
