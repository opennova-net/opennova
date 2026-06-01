#!/usr/bin/env python3
"""Land Warrior .3di structural validator.

Verifies the byte-exact layout reverse-engineered from Dflw.exe:
  - top-level loader      sub_47CB60 @ 0x47cb60
  - per-LOD geometry      sub_47CF80 @ 0x47cf80  (thunk sub_402D97)
  - per-material reader    sub_47E040 @ 0x47e040  (thunk sub_40258B)

Layout:
  off 0x00  char  magic[3]   = "3DI"
  off 0x03  u8    version     (>= 10; samples are 0x0A)
  off 0x04  char  name[~16]
  off 0x14  u32   lod_count   (<= 4)
  off 0x18  u32   lod_thresh[3]  Q16.16 far/mid/near
  off 0x28  char  tag[lod_count][4]   per-LOD render tags
  off 0x58+ ...   STALE runtime pointers on disk (loader overwrites)
  off 0x78  u32   pre_count   -> pre_count * 20-byte records (often 0)
  --- end 160-byte (0xA0) header ---
  u32   mat_count -> mat_count * 80-byte material records (sub_47E040 reads 0x50)
  per LOD: 232-byte (0xE8) LOD header, blob_size = u32 @ (lodhdr+0x14), then blob_size bytes

A correct field map means the cursor lands exactly on EOF with zero leftover bytes.
"""
import struct
import sys
import glob
import os

HEADER_SIZE = 0xA0       # 160
LOD_HEADER_SIZE = 0xE8   # 232
MAT_RECORD_SIZE = 0x50   # 80
PRE_RECORD_SIZE = 20

def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]

def cstr(b, off, n):
    raw = b[off:off + n]
    z = raw.find(b"\x00")
    return raw[:z if z >= 0 else n].decode("latin1", "replace")

class Result:
    def __init__(self, name, ver, status, line=""):
        self.name = name; self.ver = ver; self.status = status; self.line = line

def parse(path):
    with open(path, "rb") as f:
        b = f.read()
    size = len(b)
    name = os.path.basename(path)
    if b[0:3] != b"3DI":
        return Result(name, -1, "NOT-3DI", f"{name:16} NOT-3DI (magic={b[0:4]!r})")
    ver = b[3]
    # Dflw.exe (sub_47CB60 @ 0x47cbd6) rejects byte[3] < 10. v8/v9 are an older
    # on-disk layout NOT handled by this binary; don't apply the v10 field map to them.
    if ver < 10:
        return Result(name, ver, "OLD-VER", f"{name:16} v{ver:<3} (pre-v10 layout, rejected by Dflw.exe loader)")

    lod_count = u32(b, 0x14)
    thresh = [u32(b, 0x18), u32(b, 0x1C), u32(b, 0x20)]
    tags = [cstr(b, 0x28 + 4 * i, 4) for i in range(min(lod_count, 4))]
    pre_count = u32(b, 0x78)

    def guard(need):
        return pos + need <= size

    pos = HEADER_SIZE
    if pre_count * PRE_RECORD_SIZE > size:
        return Result(name, ver, "BAD", f"{name:16} v{ver} implausible pre_count={pre_count}")
    pos += pre_count * PRE_RECORD_SIZE
    if not guard(4):
        return Result(name, ver, "BAD", f"{name:16} v{ver} truncated before mat_count")
    mat_count = u32(b, pos)
    pos += 4
    mat0 = cstr(b, pos, 16) if mat_count else ""
    if mat_count * MAT_RECORD_SIZE > size - pos:
        return Result(name, ver, "BAD", f"{name:16} v{ver} implausible mat_count={mat_count}")
    pos += mat_count * MAT_RECORD_SIZE

    # Per-LOD blob decomposition. From sub_47CF80 @ 0x47cf80 the blob is 9 arrays,
    # concatenated in this order with these strides; counts live at LOD-header dword
    # indices (byte offset = lod_pos + 4*idx). Odd indices in between are runtime ptrs.
    #   idx 32 verts x8   | 34 normals x8 | 36 facerefs x80 | 38 x12 | 40 subobj x120
    #   42 tri-idx x12    | 46 x8         | 48 x80          | 44 surfaces x128 (last)
    BLOB = [(32, 8), (34, 8), (36, 80), (38, 12), (40, 120),
            (42, 12), (46, 8), (48, 80), (44, 128)]
    blob_mismatch = 0
    for _ in range(lod_count):
        if not guard(LOD_HEADER_SIZE):
            return Result(name, ver, "OVERRUN", f"{name:16} v{ver} overrun at LOD header pos={pos}/{size}")
        blob = u32(b, pos + 0x14)          # lodhdr[5]
        predicted = sum(u32(b, pos + 4 * idx) * stride for idx, stride in BLOB)
        if predicted != blob:
            blob_mismatch += 1
        pos += LOD_HEADER_SIZE + blob

    file_ok = pos == size
    status = "OK" if (file_ok and blob_mismatch == 0) else ("BLOB" if file_ok else "MISMATCH")
    tail = ("OK " if status == "OK"
            else f"BLOB-MISMATCH ({blob_mismatch} lods)" if status == "BLOB"
            else f"MISMATCH (cursor={pos}, leftover={size - pos})")
    th = "/".join(f"{t/65536:g}" for t in thresh)
    line = (f"{name:16} v{ver:<3} lods={lod_count} pre={pre_count} "
            f"mats={mat_count:<3} thr={th:<14} tag={tags} mat0={mat0!r:18} {tail}")
    return Result(name, ver, status, line)

def main():
    args = sys.argv[1:] or [r"C:\Users\taylor\Desktop\dflw_3di\*.3DI",
                            r"C:\Users\taylor\Desktop\dflw_3di\*.3di"]
    files = []
    for a in args:
        files.extend(glob.glob(a))
    files = sorted(set(files))
    results = [parse(p) for p in files]
    show_all = "-v" in sys.argv
    from collections import Counter
    ver_hist = Counter(r.ver for r in results)
    stat_hist = Counter(r.status for r in results)
    for r in results:
        if show_all or r.status not in ("OK", "OLD-VER"):
            print(r.line)
    v10 = [r for r in results if r.ver >= 10]
    n_ok = sum(1 for r in v10 if r.status == "OK")
    print("\nversion histogram:", dict(sorted(ver_hist.items())))
    print("status histogram :", dict(stat_hist))
    print(f"v>=10 files: {n_ok}/{len(v10)} validated with zero leftover bytes")

if __name__ == "__main__":
    main()
