#!/usr/bin/env python3
"""Convert a .pcapng/.pcap capture into the hexcap format the NovaWorld in-game
decoder test consumes.

Output format (one UDP datagram per line, exactly what
`tests/novaworld/nw_ingame_histogram_test.cpp` reads via env NW_INGAME_HEXCAP):

    <srcport> <frame> <udp_payload_hex>

The UDP *payload* is emitted (the NAPI CRC envelope is byte 0) — Ethernet/IP/UDP
headers are stripped by tshark. Direction (C2S vs S2C) is inferred downstream
from the source port / the 0x43 vs 0x83 opcode, so we only need srcport here.

Requires tshark (ships with Wireshark). On Windows it is auto-detected under
"C:/Program Files/Wireshark"; override with --tshark.

Examples:
    python tools/net/pcap_to_hexcap.py capture.pcapng -o notes/ingame.hexcap
    python tools/net/pcap_to_hexcap.py capture.pcapng --ports 32768,32769 -o out.hexcap
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

# tshark autodetect locations (Windows installs + PATH name on POSIX).
_TSHARK_CANDIDATES = (
    r"C:\Program Files\Wireshark\tshark.exe",
    r"C:\Program Files (x86)\Wireshark\tshark.exe",
    "tshark",
)

_HEX_STRIP = re.compile(r"[^0-9a-fA-F]")


def find_tshark(explicit: str | None) -> str:
    """Return a runnable tshark path or exit with a clear message."""
    if explicit:
        if os.path.isfile(explicit) or shutil.which(explicit):
            return explicit
        sys.exit(f"tshark not found at --tshark={explicit!r}")
    for cand in _TSHARK_CANDIDATES:
        if os.path.isfile(cand) or shutil.which(cand):
            return cand
    sys.exit(
        "tshark not found. Install Wireshark or pass --tshark <path>. "
        "Looked in: " + ", ".join(_TSHARK_CANDIDATES)
    )


def build_filter(ports: list[int]) -> str | None:
    """Display filter restricting to UDP (optionally to the given ports)."""
    if not ports:
        return "udp"
    return " || ".join(f"udp.port=={p}" for p in ports)


def run_tshark(tshark: str, pcap: str, display_filter: str) -> list[str]:
    # Two-pass (-2) so IP-fragmented datagrams reassemble before we read
    # udp.payload; tab separator keeps fields unambiguous.
    cmd = [
        tshark, "-r", pcap, "-2", "-Y", display_filter,
        "-T", "fields",
        "-e", "udp.srcport", "-e", "frame.number", "-e", "udp.payload",
        "-E", "separator=\t", "-E", "occurrence=f",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"tshark failed (exit {proc.returncode}):\n{proc.stderr.strip()}")
    return proc.stdout.splitlines()


def convert(lines: list[str]) -> tuple[list[str], int]:
    """Map tshark rows -> hexcap lines; returns (lines, skipped_count)."""
    out: list[str] = []
    skipped = 0
    for row in lines:
        parts = row.split("\t")
        if len(parts) < 3:
            skipped += 1
            continue
        srcport, frame, payload = parts[0], parts[1], parts[2]
        payload = _HEX_STRIP.sub("", payload).lower()
        if not srcport or not frame or not payload or len(payload) % 2:
            skipped += 1
            continue
        out.append(f"{srcport} {frame} {payload}")
    return out, skipped


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", help="input .pcapng / .pcap")
    ap.add_argument("-o", "--output", help="output hexcap (default: stdout)")
    ap.add_argument("--ports", default="",
                    help="comma-separated UDP ports to keep (default: all UDP)")
    ap.add_argument("--tshark", help="path to tshark.exe (default: autodetect)")
    args = ap.parse_args()

    if not os.path.isfile(args.pcap):
        sys.exit(f"input not found: {args.pcap}")
    ports = [int(p) for p in args.ports.split(",") if p.strip()]

    tshark = find_tshark(args.tshark)
    rows = run_tshark(tshark, args.pcap, build_filter(ports))
    hexcap, skipped = convert(rows)

    if args.output:
        with open(args.output, "w", newline="\n") as fh:
            fh.write("\n".join(hexcap) + ("\n" if hexcap else ""))
        sys.stderr.write(
            f"wrote {len(hexcap)} datagrams to {args.output} "
            f"({skipped} rows skipped)\n")
    else:
        sys.stdout.write("\n".join(hexcap) + ("\n" if hexcap else ""))
        sys.stderr.write(f"# {len(hexcap)} datagrams ({skipped} skipped)\n")


if __name__ == "__main__":
    main()
