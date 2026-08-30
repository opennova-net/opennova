"""Headless twin of the ida-pro-mcp `py_eval`: run IDAPython snippets from a JSON job
against a database opened by idat, write their results, and never save the database.

    idat -A -L idat.log -S"idb_batch.py job.json out.json" database.i64

`job.json` is `{"snippets": ["<python>", ...]}`. Each snippet runs in a fresh namespace and
reports through the same two conventions the MCP path uses: `result` (a JSON string) or
`payload` (any JSON-serialisable value). `out.json` is
`{"input_sha256": "<hex>", "results": [{"ok": true, "value": ...} | {"ok": false, "error": "..."}]}`.

The database flag DBFL_KILL is set before anything runs, so idat discards the unpacked
working files on exit instead of repacking them into the .i64: a snapshot passed here comes
back byte-identical. `scripts/ida/cite_sweep.py --snapshot` is the caller (and copies the
.i64 to a private directory first anyway).
"""

import json
import traceback

import ida_auto
import ida_loader
import ida_nalt
import ida_pro
import idc


def main() -> int:
    if len(idc.ARGV) != 3:
        print("usage: idb_batch.py JOB_JSON OUT_JSON")
        return 2
    job_path, out_path = idc.ARGV[1], idc.ARGV[2]
    ida_loader.set_database_flag(ida_loader.DBFL_KILL)  # never write the database back
    ida_auto.auto_wait()
    with open(job_path, encoding="utf-8") as f:
        job = json.load(f)
    digest = ida_nalt.retrieve_input_file_sha256()
    out = {"input_sha256": digest.hex() if digest else "", "results": []}
    for code in job["snippets"]:
        scope = {"__name__": "__idb_batch__"}
        try:
            exec(compile(code, "<snippet>", "exec"), scope)
            value = scope["payload"] if "payload" in scope else scope.get("result")
            if isinstance(value, str):  # `result` snippets json.dumps their value
                try:
                    value = json.loads(value)
                except ValueError:
                    pass
            out["results"].append({"ok": True, "value": value})
        except Exception:  # noqa: BLE001 - the caller reports the traceback per snippet
            out["results"].append({"ok": False, "error": traceback.format_exc()})
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f)
    print("idb_batch: %d snippet(s) into %s" % (len(out["results"]), out_path))
    return 0


try:
    rc = main()
except BaseException:  # noqa: BLE001 - batch mode must always exit
    traceback.print_exc()
    rc = 1
ida_pro.qexit(rc)
