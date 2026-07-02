#!/usr/bin/env python3
# Byte-level SHAPE diff of the S2C 0x0A per-frame-update stream: ours vs a golden
# retail-host capture. The two captures are different sessions, so raw bytes
# (positions, ticks, seq, anim phase) legitimately differ every frame. This tool
# instead compares what SHOULD match between any two hosts on the same map:
#
#   * sub-block usage      -- which flags2 low-2 sub-block (timer/env/aim/gametype)
#                             each frame carries, as a distribution. The golden
#                             CYCLES 0/1/2/3; if we only ever emit one, that's a gap.
#   * record-class mix     -- how many Player / Vehicle / Infantry tag-1 records
#                             flow. Golden replicates vehicles; if we emit zero,
#                             that's the biggest 0x0A gap.
#   * header field values  -- the value SET each side uses for the fixed header
#                             (health/mount/state) -- these are semantic, not
#                             per-frame noise, so a set mismatch is a real diff.
#   * per-class field pop.  -- for each record class, which fields are ever
#                             populated (non-zero / non-sentinel). A field golden
#                             always fills but we always leave 0 is an under-send.
#
# It parses the repo's own `nw_pp --stream` labelled output (the stable decode
# contract), so it tracks the decoders automatically. The golden's parsed profile
# is cached to <golden>.0a.json so re-runs during iteration are instant; delete
# the cache (or pass --refresh) to re-decode.
#
# Usage:
#   python scripts/net/diff_0a.py \
#       --ours   .scratch/ov-<stamp>.pcapng \
#       --golden .scratch/retail-ashi5a-<stamp>.pcapng \
#       --items  ~/Desktop/JOX/ITEMS.DEF
#
# Exit 0 always (this is a report, not a gate); the SUMMARY line names the gaps.

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

NW_PP = os.path.join("build", "apps", "nw_pp", "Release", "nw_pp.exe")

# --- line patterns (match nw_pp --stream 0x0A output) -----------------------
RE_FRAME = re.compile(r"tag=0x0a\[per-frame-update\]")
RE_HDR = re.compile(r"\[0x0A\].*flags1=(0x[0-9a-f]+)\s+flags2=(0x[0-9a-f]+)\s+sub=(\d+)")
RE_SUBLINE = re.compile(r"^\s+(timer|env|aim|gametype):")
RE_HEADER = re.compile(r"^\s+header:\s+(.*)$")
RE_REC = re.compile(r"^\s+rec \d+ hdl=(0x[0-9a-f]+) p(\d)\([^)]*\)/s\d+ type=(0x[0-9a-f]+).*class=(\w+)")
RE_BODY = re.compile(r"^\s+(player|vehicle|infantry|guided):\s+(.*)$")
# key=value tokens: keys are word chars; values are hex (either case), dec,
# (tuples), or bare words. Hex must be case-insensitive: nw_pp prints 0xFFFF
# uppercase, and a lowercase-only class would spill the "=none" suffix into a
# bogus token.
RE_KV = re.compile(r"(\w+)=(\([^)]*\)|0x[0-9a-fA-F]+|-?\d+|[A-Za-z]+)")

# Fields that are per-frame session noise -- present/absent matters, value does not.
NOISE_FIELDS = {"pos", "yaw", "pitch", "eulerZ", "hdgBAM", "aimY", "aimZ", "seconds", "refs"}
# Values treated as "field carries no data" for the population heuristic ONLY. This
# is a DISPLAY threshold for the shape diff (does golden ever put something here that
# we never do?), NOT a claim that these are wire sentinels -- only call a value a
# sentinel where the original handler is witnessed treating it as one. 0xFFFF is the
# witnessed no-handle marker across the spawn/mount handlers (e.g. @0x42e79d,
# @0x4337c5); the rest are just zero/empty.
EMPTY_VALUES = {"0", "0x0", "0x00", "0x0000", "0xffff", "0xFFFF", "none", "-1", "unmounted"}


def decode_stream(cap, items):
    """Run nw_pp --stream and yield lines (text)."""
    cmd = [NW_PP, cap, "--stream"]
    if items:
        cmd += ["--items", items]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, bufsize=1)
    for line in p.stdout:
        yield line.rstrip("\n")
    p.wait()


def profile_capture(cap, items):
    """Parse the 0x0A stream of one capture into an aggregate profile."""
    subs = Counter()               # sub-block id -> count
    classes = Counter()            # record class -> count
    header_vals = defaultdict(set)  # header field -> set of values
    # per class: field -> "populated in >=1 record" bool (tracked as set of pop-states)
    field_pop = defaultdict(lambda: defaultdict(lambda: {"pop": 0, "seen": 0}))
    frames = 0

    cur_frame = False
    for line in decode_stream(cap, items):
        if RE_FRAME.search(line):
            cur_frame = True
            frames += 1
            continue
        if not cur_frame:
            continue
        m = RE_HDR.search(line)
        if m:
            subs[int(m.group(3))] += 1
            continue
        m = RE_HEADER.match(line)
        if m:
            for k, v in RE_KV.findall(m.group(1)):
                if k not in NOISE_FIELDS:
                    header_vals[k].add(v)
            continue
        m = RE_REC.match(line)
        if m:
            cur_class = m.group(4)
            classes[cur_class] += 1
            continue
        m = RE_BODY.match(line)
        if m:
            cls = m.group(1)
            for k, v in RE_KV.findall(m.group(2)):
                fp = field_pop[cls][k]
                fp["seen"] += 1
                if k not in NOISE_FIELDS and v not in EMPTY_VALUES:
                    fp["pop"] += 1
            continue
        # a non-0x0A line ends the frame context
        if line.startswith("[S ") or line.startswith("[C "):
            cur_frame = False

    return {
        "frames": frames,
        "subs": dict(subs),
        "classes": dict(classes),
        "header_vals": {k: sorted(v) for k, v in header_vals.items()},
        "field_pop": {cls: {k: fp for k, fp in fields.items()}
                      for cls, fields in field_pop.items()},
    }


def load_or_profile(cap, items, refresh):
    cache = cap + ".0a.json"
    if not refresh and os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(cap):
        with open(cache) as f:
            return json.load(f)
    prof = profile_capture(cap, items)
    try:
        with open(cache, "w") as f:
            json.dump(prof, f)
    except OSError:
        pass
    return prof


SUB_NAME = {0: "aim", 1: "timer", 2: "env", 3: "gametype"}


def report(ours, gold):
    gaps = []
    print("=== 0x0A SHAPE DIFF (ours vs golden retail host) ===")
    print(f"frames: ours={ours['frames']}  golden={gold['frames']}\n")

    print("SUB-BLOCK usage (flags2 low2 -> which per-frame sub-block):")
    # A fresh profile keys subs by int; a .0a.json cache round-trips them to str.
    osubs = {int(k): v for k, v in ours["subs"].items()}
    gsubs = {int(k): v for k, v in gold["subs"].items()}
    for s in sorted(set(osubs) | set(gsubs)):
        o = osubs.get(s, 0)
        g = gsubs.get(s, 0)
        flag = ""
        if g > 0 and o == 0:
            flag = "  <- GAP: golden emits this sub-block, we never do"
            gaps.append(f"sub-block {s}({SUB_NAME.get(s,'?')})")
        print(f"  sub={s:<2}({SUB_NAME.get(s,'?'):<8}) ours={o:<7} golden={g}{flag}")

    print("\nRECORD-CLASS mix (tag-1 records replicated per session):")
    for c in sorted(set(ours["classes"]) | set(gold["classes"])):
        o = ours["classes"].get(c, 0)
        g = gold["classes"].get(c, 0)
        flag = ""
        if g > 0 and o == 0:
            flag = "  <- GAP: golden replicates this class, we send none"
            gaps.append(f"record class {c}")
        print(f"  {c:<10} ours={o:<7} golden={g}{flag}")

    print("\nHEADER field value-sets (semantic, should match):")
    for k in sorted(set(ours["header_vals"]) | set(gold["header_vals"])):
        o = ours["header_vals"].get(k, [])
        g = gold["header_vals"].get(k, [])
        flag = "  <- DIFF" if set(o) != set(g) else ""
        print(f"  {k:<12} ours={o} golden={g}{flag}")

    print("\nPER-CLASS field population (field ever non-sentinel):")
    for cls in sorted(set(ours["field_pop"]) | set(gold["field_pop"])):
        of = ours["field_pop"].get(cls, {})
        gf = gold["field_pop"].get(cls, {})
        print(f"  [{cls}]")
        for k in sorted(set(of) | set(gf)):
            op = of.get(k, {"pop": 0, "seen": 0})
            gp = gf.get(k, {"pop": 0, "seen": 0})
            o_state = "POP" if op["pop"] else ("zero" if op["seen"] else "-")
            g_state = "POP" if gp["pop"] else ("zero" if gp["seen"] else "-")
            flag = ""
            if g_state == "POP" and o_state in ("zero", "-"):
                flag = "  <- golden fills, we leave empty"
                gaps.append(f"{cls}.{k}")
            print(f"    {k:<12} ours={o_state:<4} golden={g_state}{flag}")

    print(f"\nSUMMARY gaps={len(gaps)}: {', '.join(gaps) if gaps else 'none'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True)
    ap.add_argument("--golden", required=True)
    ap.add_argument("--items", default=os.environ.get("NW_PP_ITEMS", ""))
    ap.add_argument("--refresh", action="store_true", help="ignore cached golden profile")
    args = ap.parse_args()

    if not os.path.exists(NW_PP):
        sys.exit(f"nw_pp not built at {NW_PP} (run scripts/build.sh)")

    print(f"profiling OURS   : {args.ours}", file=sys.stderr)
    ours = load_or_profile(args.ours, args.items, args.refresh)
    print(f"profiling GOLDEN : {args.golden}", file=sys.stderr)
    gold = load_or_profile(args.golden, args.items, args.refresh)
    report(ours, gold)


if __name__ == "__main__":
    main()
