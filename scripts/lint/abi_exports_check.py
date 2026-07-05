#!/usr/bin/env python3
"""C-ABI export-identity guard (docs/maturity-program.md NET-4, ADR 0019 §5).

Two checks over the built `opennova_shared` library (`opennova.dll` /
`libopennova.so` / `libopennova.dylib`):

  1. FORBIDDEN FAMILIES (never bypassable): no symbol from the net libs —
     novaworld / npwire / napi / novacrypto / netsim / nwu — may appear in
     the shared library, exported or dynamic, mangled or not. The net libs
     are C++-linked only (ADR 0019 §5); the C ABI never grows a wire surface.
  2. EXPORT IDENTITY: the flat C-ABI export list must equal the committed
     baseline (scripts/lint/abi_exports_baseline.txt). Any drift fails; the
     maintainer escape hatch is `--write-baseline` in the SAME commit that
     deliberately changes the ABI, logged per the enforcement table.

Wired as the `abi_export_identity` ctest (tests/CMakeLists.txt), so it runs
wherever BUILD_SHARED_LIB=ON builds run ctest — scripts/build.sh and the CI
build-and-test job. Stdlib only; PE parsing is built in (no dumpbin needed),
ELF/Mach-O go through `nm`.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

BASELINE_DEFAULT = Path(__file__).resolve().parent / "abi_exports_baseline.txt"

# Substring families are specific enough to catch C++-mangled names too
# (e.g. _ZN8opennova9novaworld...); the short prefixes require a _-boundary.
FORBIDDEN_SUBSTRINGS = ("novaworld", "npwire", "novacrypto", "netsim")
FORBIDDEN_PREFIXED = re.compile(r"(^|_)(napi|nwu)_", re.IGNORECASE)


def pe_export_names(path: Path) -> list[str]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE image")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_off = pe + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    ddir_off = opt_off + (112 if magic == 0x20B else 96)
    exp_rva = struct.unpack_from("<I", data, ddir_off)[0]
    if exp_rva == 0:
        return []
    sec_off = opt_off + struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(nsec):
        off = sec_off + i * 40
        vsz, va, _rsz, ra = struct.unpack_from("<IIII", data, off + 8)
        sections.append((va, vsz, ra))

    def rva2off(rva: int) -> int:
        for va, vsz, ra in sections:
            if va <= rva < va + vsz:
                return ra + (rva - va)
        raise ValueError(f"rva {rva:#x} outside sections")

    eo = rva2off(exp_rva)
    nnames = struct.unpack_from("<I", data, eo + 24)[0]
    names_off = rva2off(struct.unpack_from("<I", data, eo + 32)[0])
    names = []
    for i in range(nnames):
        off = rva2off(struct.unpack_from("<I", data, names_off + i * 4)[0])
        names.append(data[off:data.index(b"\0", off)].decode("ascii"))
    return names


def nm_symbol_names(path: Path) -> list[str]:
    """Defined external symbols via nm (ELF: -D dynamic; Mach-O: -gU)."""
    for args in (["nm", "-D", "--defined-only", str(path)],
                 ["nm", "-gU", str(path)]):
        try:
            out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        names = []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                name = parts[-1]
                names.append(name[1:] if name.startswith("_") and sys.platform == "darwin" else name)
        if names:
            return names
    raise RuntimeError(f"no usable nm output for {path}")


def extract_symbols(path: Path) -> list[str]:
    if path.suffix.lower() == ".dll":
        return pe_export_names(path)
    return nm_symbol_names(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lib", required=True, type=Path,
                        help="path to the built opennova_shared library")
    parser.add_argument("--baseline", type=Path, default=BASELINE_DEFAULT)
    parser.add_argument("--write-baseline", action="store_true",
                        help="maintainer escape hatch: rewrite the baseline "
                             "to the current C-ABI export list (same-commit, "
                             "logged in docs/maturity-program.md)")
    args = parser.parse_args()

    symbols = extract_symbols(args.lib)

    # Check 1 — forbidden families, over EVERY symbol, never bypassable.
    bad = [s for s in symbols
           if any(f in s.lower() for f in FORBIDDEN_SUBSTRINGS) or FORBIDDEN_PREFIXED.search(s)]
    if bad:
        print(f"[abi] FORBIDDEN net-lib symbols in {args.lib.name} (ADR 0019 §5 — "
              "the net libs are C++-linked only, never in the C ABI):")
        for s in sorted(bad):
            print(f"[abi]   {s}")
        return 2

    baseline = []
    if args.baseline.exists():
        baseline = [ln.strip() for ln in args.baseline.read_text(encoding="utf-8").splitlines()
                    if ln.strip() and not ln.startswith("#")]

    # The C-ABI family set comes from the baseline itself; platform noise
    # (_init/_fini, mangled locals that leak into dynsym) is filtered out.
    prefixes = {name.split("_", 1)[0] for name in baseline}
    if prefixes:
        current = sorted(s for s in symbols if s.split("_", 1)[0] in prefixes)
    else:
        current = sorted(symbols)

    if args.write_baseline:
        args.baseline.write_text(
            "# opennova_shared C-ABI export baseline (scripts/lint/abi_exports_check.py).\n"
            "# Regenerate ONLY in the same commit as a deliberate ABI change:\n"
            "#   python scripts/lint/abi_exports_check.py --lib <built lib> --write-baseline\n"
            + "\n".join(current) + "\n",
            encoding="utf-8")
        print(f"[abi] baseline written: {len(current)} exports -> {args.baseline}")
        return 0

    if not baseline:
        print(f"[abi] no baseline at {args.baseline} — run with --write-baseline first")
        return 1

    added = sorted(set(current) - set(baseline))
    removed = sorted(set(baseline) - set(current))
    if added or removed:
        print(f"[abi] C-ABI export drift vs {args.baseline.name} "
              f"({len(added)} added, {len(removed)} removed):")
        for s in added:
            print(f"[abi]   + {s}")
        for s in removed:
            print(f"[abi]   - {s}")
        print("[abi] If this ABI change is deliberate, regenerate the baseline in the SAME")
        print("[abi] commit (--write-baseline) and note it in docs/maturity-program.md;")
        print("[abi] shipped exports keep their historical semantics (libs/CLAUDE.md).")
        return 1

    print(f"[abi] OK: {len(current)} C-ABI exports match the baseline; no net-lib symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
