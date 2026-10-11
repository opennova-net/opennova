"""Read a live retail process's ammo-definition CRC rows (D-NET-181).

Reproduces the ammo half of an integrity-challenge profile from one running
Jointops.exe in a mission, read-only (PROCESS_VM_READ):

  g_AmmoDefTable @0xA2ECE8 (a pointer to the 276-byte rows) and
  g_AmmoDefCapacity @0xA2ECEC, read as NetPacket_WriteEntityCRCChecksum
  @0x42B020 reads them; each row is copied, its little-endian dwords at
  0x40/0x44/0x48/0x4C/0x68/0x70 zeroed as that function zeroes them, and the
  copy run through CRC_ComputeCustomTable @0x53C820 (CRC-32/MPEG-2,
  engine/base/io/crc32_mpeg2.h).

The whole-WeaponSlotDef CRC (the S2C 0x30 source) is not read here: a retail
joiner's C2S 0x20 reply to a stock host's {FF,00,00} challenge carries it.

Usage:
    python scripts/net/read_integrity_profile.py --pid <Jointops.exe pid>
        [--expect ROW=CRC ...] [--json <out.json>]

Prints the rows as C++ IntegrityCrcEntry initializers plus the row count, the
executable's SHA-256 and the FNV-1a 64 digest host_punt_test pins. Exits 1 when
an --expect anchor disagrees. Stdlib only; Windows only.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import struct
import sys

AMMO_TABLE_PTR = 0xA2ECE8
AMMO_CAPACITY = 0xA2ECEC
ROW_SIZE = 276
VOLATILE_DWORDS = (0x40, 0x44, 0x48, 0x4C, 0x68, 0x70)
IMAGE_BASE = 0x400000
PINNED_JOX01_SHA256 = "9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57"

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.argtypes = (wt.DWORD, wt.BOOL, wt.DWORD)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.ReadProcessMemory.argtypes = (
    wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
kernel32.ReadProcessMemory.restype = wt.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = (
    wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD))
kernel32.QueryFullProcessImageNameW.restype = wt.BOOL
kernel32.CloseHandle.argtypes = (wt.HANDLE,)


def crc32_mpeg2(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc


def read(handle, address, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size,
                                      ctypes.byref(got)) or got.value != size:
        raise OSError(f"ReadProcessMemory 0x{address:08X}+{size} failed "
                      f"(error {ctypes.get_last_error()}, read {got.value})")
    return buf.raw


def image_path(handle):
    size = wt.DWORD(1024)
    buf = ctypes.create_unicode_buffer(size.value)
    if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
        raise OSError(f"QueryFullProcessImageNameW failed (error {ctypes.get_last_error()})")
    return buf.value


def fnv1a64(rows):
    # host_punt_test's digest: index byte then the CRC's four little-endian bytes.
    h = 0xCBF29CE484222325
    for index, crc in rows:
        for byte in bytes([index]) + struct.pack("<I", crc):
            h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--expect", action="append", default=[],
                        help="ROW=CRC anchor, hex (e.g. 0x16=0xB4D3A790)")
    parser.add_argument("--json", help="write the rows and identity here")
    args = parser.parse_args()

    handle = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                                  False, args.pid)
    if not handle:
        sys.exit(f"OpenProcess({args.pid}) failed (error {ctypes.get_last_error()})")
    try:
        exe = image_path(handle)
        with open(exe, "rb") as f:
            exe_sha256 = hashlib.sha256(f.read()).hexdigest()
        if read(handle, IMAGE_BASE, 2) != b"MZ":
            sys.exit(f"no image at 0x{IMAGE_BASE:X} in pid {args.pid}")
        table, capacity = struct.unpack("<Ii", read(handle, AMMO_TABLE_PTR, 8))
        if table == 0 or not 0 < capacity <= 255:
            sys.exit(f"ammo table not loaded (table=0x{table:08X} capacity={capacity}); "
                     "read during a mission")
        raw = read(handle, table, capacity * ROW_SIZE)
    finally:
        kernel32.CloseHandle(handle)

    rows = []
    for index in range(capacity):
        row = bytearray(raw[index * ROW_SIZE:(index + 1) * ROW_SIZE])
        for offset in VOLATILE_DWORDS:
            row[offset:offset + 4] = b"\0\0\0\0"
        rows.append((index, crc32_mpeg2(row)))

    print(f"// exe {exe}")
    print(f"// sha256 {exe_sha256}"
          f"{'' if exe_sha256 == PINNED_JOX01_SHA256 else '  (NOT the pinned jox01 exe)'}")
    print(f"// ammo rows {capacity}, fnv1a64 0x{fnv1a64(rows):016X}")
    for index, crc in rows:
        print(f"\t\t{{0x{index:02X}, 0x{crc:08X}u}},")

    failed = False
    by_index = dict(rows)
    for anchor in args.expect:
        row_text, crc_text = anchor.split("=")
        index, want = int(row_text, 16), int(crc_text, 16)
        got = by_index.get(index)
        ok = got == want
        failed |= not ok
        print(f"// anchor row 0x{index:02X}: want 0x{want:08X} got "
              f"{'-' if got is None else f'0x{got:08X}'} {'OK' if ok else 'MISMATCH'}")

    if args.json:
        with open(args.json, "x", encoding="utf-8") as f:
            json.dump({"schema": "opennova.integrity-ammo-rows.v1", "exe": exe,
                       "exe_sha256": exe_sha256, "capacity": capacity,
                       "fnv1a64": f"0x{fnv1a64(rows):016X}",
                       "rows": [[i, f"0x{c:08X}"] for i, c in rows]}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
