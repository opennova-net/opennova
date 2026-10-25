#!/usr/bin/env python3
"""Compare two retail-LAN parity suites' verdicts row by row (ADR 0050 R4).

    python scripts/net/diff_verdicts.py --old <suite dir> --new <suite dir> --out <dir>
        [--attribution scripts/net/parity-attribution.json] [--lanes <json>]
        [--fixed D-NET-505,D-NET-506,...]

A suite dir holds the `<case>.verdict.json` files `run_parity_suite.ps1` writes
(`opennova.parity-verdict.v1`; `.scratch\\parity-suites\\<prefix>\\`, or a
suite-summary.json, whose directory is taken). A row is keyed by (case,
topology, direction, metric, key) and classed:

    FIXED        failed in old, passes in new
    REGRESSED    passed in old, fails in new
    PERSISTS     fails in both (with the distance delta)
    NEW          fails in new, absent from old
    GONE         failed in old, absent from new
    SKIP-CHANGE  skip / short on one side, a different status on the other

The widening report lists each A/A row widened in either suite (its floor,
the old and new A/A distances, every new subject row of the key with its
distance and whether it passes only on the widened threshold), and the keys
widened in more than one case. Rows compare only within the cases both
suites judged.

With --attribution, each failing (or FIXED) row lists the D-NET rows whose
entries match it; a failing row none matches joins the minting queue.
--fixed names the rows claimed fixed: for each, the old failing rows
attributed to it and which of them still fail. --lanes adds the owning lane
(first matching metric-family rule) to each row. Every failing row is written as
a triage line:

    case | topology | dir | metric | key | code | distance/floor/aa/threshold |
    subject | rr | class | attribution | lane | capture refs | note

Writes <out>/verdict-diff.md and <out>/verdict-diff.json, prints
VERDICT_DIFF lines and VERDICT_DIFF_REPORT=<md>. Exit 0, or 2 on an input
error. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

FAILING = {"fail"}
SKIPPED = {"skip", "short"}
CLASSES = ["REGRESSED", "NEW", "PERSISTS", "FIXED", "GONE", "SKIP-CHANGE"]


class InputError(Exception):
    pass


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise InputError(f"{path}: {error}") from error


def load_suite(spec: str) -> dict:
    """case id -> verdict document for every <case>.verdict.json of a suite."""
    path = Path(spec)
    if path.is_file():
        path = path.parent
    if not path.is_dir():
        raise InputError(f"{spec}: not a suite directory")
    verdicts = {}
    for file in sorted(path.glob("*.verdict.json")):
        doc = load_json(file)
        if doc.get("schema") != "opennova.parity-verdict.v1":
            raise InputError(f"{file}: not an opennova.parity-verdict.v1 document")
        verdicts[file.name[: -len(".verdict.json")]] = doc
    if not verdicts:
        raise InputError(f"{spec}: no *.verdict.json")
    return verdicts


def row_key(case: str, row: dict) -> tuple:
    return (case, row.get("topology", ""), row.get("direction", ""), row.get("metric", ""),
            row.get("key", ""))


def index_rows(suite: dict) -> dict:
    rows = {}
    for case, doc in suite.items():
        for row in doc.get("rows", []):
            rows[row_key(case, row)] = row
    return rows


def classify(old: dict | None, new: dict | None) -> str | None:
    old_status = old.get("status") if old else None
    new_status = new.get("status") if new else None
    if old_status == new_status:
        return "PERSISTS" if new_status in FAILING else None
    if old_status in SKIPPED or new_status in SKIPPED:
        return "SKIP-CHANGE"
    if old is None:
        return "NEW" if new_status in FAILING else None
    if new is None:
        return "GONE" if old_status in FAILING else None
    if old_status in FAILING and new_status not in FAILING:
        return "FIXED"
    if new_status in FAILING:
        return "REGRESSED"
    return None


def compile_pattern(text: str):
    return re.compile(f"(?:{text})", re.IGNORECASE)


class Matcher:
    """Entries of an attribution or lane table, matched against a verdict row."""

    def __init__(self, entries: list, label: str):
        self.entries = []
        for entry in entries:
            try:
                self.entries.append({
                    **entry,
                    "_metric": compile_pattern(entry.get("metric", ".*")),
                    "_key": compile_pattern(entry.get("key", ".*")),
                })
            except re.error as error:
                raise InputError(f"{label}: bad pattern in {entry}: {error}") from error

    @staticmethod
    def _applies(entry: dict, key: tuple) -> bool:
        case, topology, direction, metric, row_key_text = key
        topologies = entry.get("topologies", "*")
        if topologies != "*" and topology not in topologies:
            return False
        if entry.get("direction", "*") not in ("*", direction):
            return False
        cases = entry.get("cases", "*")
        if cases != "*" and case not in cases:
            return False
        return bool(entry["_metric"].fullmatch(metric) and entry["_key"].fullmatch(row_key_text))

    def all(self, key: tuple) -> list:
        return [entry for entry in self.entries if self._applies(entry, key)]

    def first(self, key: tuple):
        for entry in self.entries:
            if self._applies(entry, key):
                return entry
        return None


def number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def capture_refs(doc: dict | None, topology: str) -> str:
    if not doc:
        return ""
    runs = doc.get("runs", {})
    refs = []
    for name in (topology.lower().replace("-", "_"), "rr"):
        run = runs.get(name)
        if isinstance(run, dict) and run.get("capture"):
            refs.append(f"{name}={run['capture']}")
    return " ".join(refs)


def fmt(value) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value)


def cell(text) -> str:
    return fmt(text).replace("|", "\\|").replace("\n", " ")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--old", required=True, help="the earlier suite's verdict directory")
    parser.add_argument("--new", required=True, help="the later suite's verdict directory")
    parser.add_argument("--out", required=True, help="directory for verdict-diff.md / .json")
    parser.add_argument("--attribution", help="opennova.parity-attribution.v1 table")
    parser.add_argument("--lanes", help="opennova.parity-lanes.v1 table (metric family -> lane)")
    parser.add_argument("--fixed", default="", help="comma-separated rows claimed fixed (D-NET-n)")
    args = parser.parse_args(argv)

    try:
        old_suite = load_suite(args.old)
        new_suite = load_suite(args.new)
        attribution = None
        if args.attribution:
            table = load_json(Path(args.attribution))
            if table.get("schema") != "opennova.parity-attribution.v1":
                raise InputError(f"{args.attribution}: not an opennova.parity-attribution.v1 table")
            attribution = Matcher(table.get("rows", []), args.attribution)
        lanes = None
        if args.lanes:
            table = load_json(Path(args.lanes))
            if table.get("schema") != "opennova.parity-lanes.v1":
                raise InputError(f"{args.lanes}: not an opennova.parity-lanes.v1 table")
            lanes = Matcher(table.get("lanes", []), args.lanes)
    except InputError as error:
        print(f"diff_verdicts: {error}", file=sys.stderr)
        return 2
    fixed_claims = [item.strip() for item in args.fixed.split(",") if item.strip()]

    # Rows compare within the cases both suites judged; a case one suite lacks
    # is reported as missing, not as every row GONE or NEW.
    common = set(old_suite) & set(new_suite)
    old_rows = {key: row for key, row in index_rows(old_suite).items() if key[0] in common}
    new_rows = {key: row for key, row in index_rows(new_suite).items() if key[0] in common}
    diff = []
    for key in sorted(set(old_rows) | set(new_rows)):
        old, new = old_rows.get(key), new_rows.get(key)
        verdict_class = classify(old, new)
        if verdict_class is None:
            continue
        shown = new if new is not None else old
        case, topology, direction, metric, row_key_text = key
        ids = sorted({entry["id"] for entry in attribution.all(key)}) if attribution else []
        lane = None
        if lanes:
            entry = lanes.first(key)
            lane = entry.get("lane") if entry else None
        old_distance = number(old.get("distance")) if old else None
        new_distance = number(new.get("distance")) if new else None
        diff.append({
            "case": case, "topology": topology, "direction": direction, "metric": metric,
            "key": row_key_text, "code": shown.get("code", ""), "class": verdict_class,
            "old": {name: old.get(name) for name in ("status", "distance", "floor", "aa_distance", "threshold", "widened")} if old else None,
            "new": {name: new.get(name) for name in ("status", "distance", "floor", "aa_distance", "threshold", "widened")} if new else None,
            "distance_delta": (new_distance - old_distance)
            if old_distance is not None and new_distance is not None else None,
            "subject": shown.get("subject"), "rr": shown.get("rr"), "detail": shown.get("detail", ""),
            "attribution": ids, "lane": lane,
            "captures": capture_refs(new_suite.get(case) if new else old_suite.get(case), topology),
        })

    # Case-level status.
    cases = {}
    for case in sorted(set(old_suite) | set(new_suite)):
        o, n = old_suite.get(case, {}), new_suite.get(case, {})
        cases[case] = {
            "old_status": o.get("status"), "old_failures": o.get("failures"),
            "new_status": n.get("status"), "new_failures": n.get("failures"),
            "classes": dict(Counter(row["class"] for row in diff if row["case"] == case)),
            "compared": case in common,
        }

    # The widening report: every A/A row widened in either suite, with the new
    # subject rows of its key.
    def aa_index(suite: dict) -> dict:
        out = {}
        for case, doc in suite.items():
            for row in doc.get("aa", []):
                out[(case, row.get("direction", ""), row.get("metric", ""), row.get("key", ""))] = row
        return out

    old_aa = {key: row for key, row in aa_index(old_suite).items() if key[0] in common}
    new_aa = {key: row for key, row in aa_index(new_suite).items() if key[0] in common}
    widening = []
    for key in sorted(set(old_aa) | set(new_aa)):
        o, n = old_aa.get(key), new_aa.get(key)
        if not any(row and row.get("status") == "widened" for row in (o, n)):
            continue
        case, direction, metric, row_key_text = key
        subjects = []
        for topology in ("RO", "OR", "OO"):
            row = new_rows.get((case, topology, direction, metric, row_key_text))
            if not row:
                continue
            distance, floor = number(row.get("distance")), number(row.get("floor"))
            subjects.append({
                "topology": topology, "status": row.get("status"), "distance": distance,
                "threshold": row.get("threshold"),
                "pass_on_widening": row.get("status") == "pass" and bool(row.get("widened"))
                and distance is not None and floor is not None and distance > floor,
            })
        widening.append({
            "case": case, "direction": direction, "metric": metric, "key": row_key_text,
            "floor": (n or o).get("floor"),
            "aa_old": o.get("distance") if o else None, "aa_old_status": o.get("status") if o else None,
            "aa_new": n.get("distance") if n else None, "aa_new_status": n.get("status") if n else None,
            "subjects": subjects,
        })
    widened_cases = defaultdict(set)
    for item in widening:
        if item["aa_new_status"] == "widened":
            widened_cases[(item["direction"], item["metric"], item["key"])].add(item["case"])
    widened_keys = [
        {"direction": d, "metric": m, "key": k, "cases": sorted(c)}
        for (d, m, k), c in sorted(widened_cases.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    ]

    # The fixed claims: the old failing rows attributed to each, and which still fail.
    claims = []
    if attribution:
        for claim in fixed_claims:
            attributed = [key for key, row in old_rows.items()
                          if row.get("status") in FAILING and claim in {e["id"] for e in attribution.all(key)}]
            still = [key for key in attributed if new_rows.get(key, {}).get("status") in FAILING]
            claims.append({
                "id": claim, "old_failing": len(attributed),
                "now_passing": sum(1 for key in attributed if new_rows.get(key, {}).get("status") == "pass"),
                "still_failing": [dict(zip(("case", "topology", "direction", "metric", "key"), key)) for key in still],
                "absent": sum(1 for key in attributed if key not in new_rows),
            })

    evidence = {
        side: {case: doc.get("evidence", []) for case, doc in suite.items() if doc.get("evidence")}
        for side, suite in (("old", old_suite), ("new", new_suite))
    }
    failing_now = [row for row in diff if row["class"] in ("REGRESSED", "NEW", "PERSISTS")]
    minting = [row for row in failing_now if attribution and not row["attribution"]]

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "schema": "opennova.parity-verdict-diff.v1",
        "old": str(Path(args.old)), "new": str(Path(args.new)),
        "attribution": args.attribution or "", "lanes": args.lanes or "",
        "cases": cases,
        "counts": dict(Counter(row["class"] for row in diff)),
        "rows": diff,
        "fixed_claims": claims,
        "widening": widening, "widened_keys": widened_keys,
        "evidence": evidence,
        "minting_queue": minting,
    }
    json_path = out_dir / "verdict-diff.json"
    json_path.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")

    def triage_line(row: dict) -> str:
        values = row["new"] or row["old"] or {}
        numbers = "/".join(fmt(values.get(name)) for name in ("distance", "floor", "aa_distance", "threshold"))
        return " | ".join([
            row["case"], row["topology"], row["direction"], row["metric"], row["key"] or "-",
            row["code"] or "-", numbers, cell(row["subject"]), cell(row["rr"]), row["class"],
            ",".join(row["attribution"]) or ("MINT" if attribution else "-"), row["lane"] or "-",
            row["captures"] or "-", cell(row["detail"]) if row["detail"] else "-",
        ])

    md = [f"# Parity verdict diff", "", f"- old: `{args.old}`", f"- new: `{args.new}`"]
    if args.attribution:
        md.append(f"- attribution: `{args.attribution}`")
    md += ["", "## Cases", "", "| Case | Old | New | " + " | ".join(CLASSES) + " |",
           "|---|---|---|" + "---|" * len(CLASSES)]
    for case, info in cases.items():
        if not info["compared"]:
            md.append(f"| {case} | {fmt(info['old_status'])} | {fmt(info['new_status'])} | "
                      + " | ".join("-" for _ in CLASSES) + " |")
            continue
        md.append(f"| {case} | {info['old_status']} {fmt(info['old_failures'])} | "
                  f"{info['new_status']} {fmt(info['new_failures'])} | "
                  + " | ".join(str(info["classes"].get(name, 0)) for name in CLASSES) + " |")
    if claims:
        md += ["", "## Fixed claims", "", "| Row | Old failing rows | Now passing | Absent | Still failing |",
               "|---|---|---|---|---|"]
        for claim in claims:
            still = "; ".join(f"{s['case']} {s['topology']} {s['direction']} {s['metric']} {s['key'] or '-'}"
                              for s in claim["still_failing"]) or "-"
            md.append(f"| {claim['id']} | {claim['old_failing']} | {claim['now_passing']} | "
                      f"{claim['absent']} | {cell(still)} |")
    header = ("case | topology | dir | metric | key | code | distance/floor/aa/threshold | subject | rr | "
              "class | attribution | lane | capture refs | note")
    for name in CLASSES:
        rows = [row for row in diff if row["class"] == name]
        if not rows:
            continue
        md += ["", f"## {name} ({len(rows)})", "", "```", header]
        for row in rows:
            line = triage_line(row)
            if name == "PERSISTS" and row["distance_delta"] is not None:
                line += f" | delta {fmt(row['distance_delta'])}"
            md.append(line)
        md.append("```")
    if attribution:
        md += ["", f"## Minting queue ({len(minting)})", "",
               "Failing rows no attribution entry matches.", "", "```", header]
        md += [triage_line(row) for row in minting]
        md.append("```")
    md += ["", "## Widening", "", "| Case | Dir | Metric | Key | Floor | A/A old | A/A new | Subjects (distance, pass on widening) |",
           "|---|---|---|---|---|---|---|---|"]
    for item in widening:
        subjects = ", ".join(
            f"{s['topology']} {s['status']} {fmt(s['distance'])}{' WIDENED-PASS' if s['pass_on_widening'] else ''}"
            for s in item["subjects"]) or "-"
        md.append(f"| {item['case']} | {item['direction']} | {item['metric']} | {cell(item['key'] or '-')} | "
                  f"{fmt(item['floor'])} | {fmt(item['aa_old'])} ({item['aa_old_status'] or '-'}) | "
                  f"{fmt(item['aa_new'])} ({item['aa_new_status'] or '-'}) | {subjects} |")
    md += ["", "Keys widened in the new suite, by case count:", ""]
    md += [f"- {k['direction']} {k['metric']} {k['key'] or '-'}: {len(k['cases'])} ({', '.join(k['cases'])})"
           for k in widened_keys] or ["- none"]
    md += ["", "## Evidence findings", ""]
    for side in ("old", "new"):
        for case, items in evidence[side].items():
            for item in items:
                md.append(f"- {side} {case} {item.get('run', '-')}: {item.get('code', '')} {item.get('detail', '')}")
    if not evidence["old"] and not evidence["new"]:
        md.append("- none")
    md_path = out_dir / "verdict-diff.md"
    md_path.write_text("\n".join(md) + "\n", encoding="utf-8")

    for name in CLASSES:
        print(f"VERDICT_DIFF class={name} rows={report['counts'].get(name, 0)}")
    for claim in claims:
        print(f"VERDICT_DIFF fixed={claim['id']} old_failing={claim['old_failing']} "
              f"now_passing={claim['now_passing']} still_failing={len(claim['still_failing'])}")
    if attribution:
        print(f"VERDICT_DIFF minting_queue={len(minting)}")
    print(f"VERDICT_DIFF_REPORT={md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
