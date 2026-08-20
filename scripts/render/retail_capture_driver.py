#!/usr/bin/env python
"""Drive onHook to capture retail reference frames for the fixture catalog.

The registered workflow imposes two timing/geometry constraints that an
interactive operator cannot meet by issuing MCP calls one at a time:

* ``register_retail_capture.py`` rejects a capture more than **120 frames**
  after its fixture application. Calls issued separately through an agent or by
  hand run 500-1800 frames apart; issued back-to-back on one stdio connection
  they cost 3-5.

* ``build_retail_side_by_side.py`` requires the registered retail camera to
  match the catalog ``camera_bms`` within 0.05 per axis, but
  ``onhook_apply_render_fixture`` can only set the PLAYER pose -- retail derives
  the camera from it. This driver closes the loop: it solves the camera from the
  captured frame's own view matrix, corrects the applied player pose by the
  residual, and repeats.

IMPORTANT -- read before using the correction loop. ``register_retail_capture``
also pins the APPLIED player position to the catalog's
``retail_player_bms.applied`` within 1e-5, so a corrected pose is NOT
registerable against that catalog. The loop is therefore a CALIBRATION tool: use
it to discover the pose/camera pair retail actually produces, and mint a new
catalog revision from a self-consistent session. Use ``--no-correct`` to capture
at the catalog's applied positions verbatim, which is what a catalog revision
should be derived from.

Retail also pins a player who is already in water: float/settle physics
overrides the applied z and the residual repeats bit-for-bit. Only the first
teleport of a process lands, so water fixtures need ``--fresh-host``.

See docs/render/render-parity-runbook.md.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

TOLERANCE = 0.05


class DriverError(RuntimeError):
    """A fail-closed capture error."""


class Client:
    """Minimal MCP stdio client for one onhook-mcp.exe process."""

    def __init__(self, exe: Path) -> None:
        self.proc = subprocess.Popen(
            [str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
        )
        self.seq = 0
        self.call("initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "retail-capture-driver", "version": "1"},
        })
        self.notify("notifications/initialized")

    def notify(self, method: str, params: Any = None) -> None:
        msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def call(self, method: str, params: Any) -> dict[str, Any]:
        self.seq += 1
        rid = self.seq
        self.proc.stdin.write(json.dumps({
            "jsonrpc": "2.0", "id": rid, "method": method, "params": params,
        }) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise DriverError("onHook MCP closed the connection")
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("id") == rid:
                if "error" in msg:
                    raise DriverError(json.dumps(msg["error"]))
                return msg["result"]

    def tool(self, name: str, args: dict[str, Any]) -> dict[str, Any]:
        result = self.call("tools/call", {"name": name, "arguments": args})
        if result.get("isError"):
            raise DriverError(f"{name}: {json.dumps(result)[:400]}")
        if "structuredContent" in result:
            return result["structuredContent"]
        for item in result.get("content", []):
            if item.get("type") == "text":
                try:
                    return json.loads(item["text"])
                except (json.JSONDecodeError, KeyError):
                    continue
        raise DriverError(f"{name}: no structured content in result")

    def close(self) -> None:
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def camera_bms(state_path: Path) -> list[float]:
    """Solve the camera in BMS coordinates from the frame's own view matrix.

    Verified against the published 2026-08-16 registrations: this reproduces
    their recorded ``camera_bms`` exactly. Measure the camera, never assume an
    eye offset.
    """
    state = json.loads(state_path.read_text(encoding="utf-8-sig"))
    view = state["render_state"]["view_matrix_row_major"]
    m = [view[0:4], view[4:8], view[8:12], view[12:16]]
    rows = [[m[i][0], m[i][1], m[i][2]] for i in range(3)]
    t = [m[3][0], m[3][1], m[3][2]]
    a = [-(t[0] * rows[i][0] + t[1] * rows[i][1] + t[2] * rows[i][2])
         for i in range(3)]
    return [a[2], -a[0], a[1]]


def save(path: Path, obj: Any) -> None:
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8", newline="\n")


def capture_fixture(
    client: Client,
    catalog: dict[str, Any],
    fixture_id: str,
    args: argparse.Namespace,
    instance_id: str,
    instances: dict[str, Any],
    log_path: Path,
) -> bool:
    fixture = {f["id"]: f for f in catalog["fixtures"]}[fixture_id]
    target = fixture["camera_bms"]
    pos = list(fixture["retail_player_bms"]["applied"])
    out_dir = args.output / fixture_id
    out_dir.mkdir(parents=True, exist_ok=True)
    png = out_dir / "retail.png"
    state = out_dir / "retail.state.json"

    attempts = 1 if args.no_correct else args.attempts
    for attempt in range(1, attempts + 1):
        for stale in (png, state):
            if stale.exists():
                stale.unlink()
        applied = {
            "fixture_id": fixture_id,
            "instance_id": instance_id,
            "position_bms": {"x": pos[0], "y": pos[1], "z": pos[2]},
            "yaw_degrees": target["yaw_deg"],
            "pitch_degrees": target["pitch_deg"],
            "camera_mode": "first_person",
            "vertical_fov_degrees": target["vertical_fov_deg"],
            "time_of_day_seconds": int(fixture["minutes_of_day"][0]) * 60,
            "mission_file": fixture["mission"],
            "mission_sha256": catalog["missions"][fixture["mission"]],
        }
        fix = client.tool("onhook_apply_render_fixture", applied)
        cap = client.tool("onhook_capture_retail_reference", {
            "fixture_id": fixture_id,
            "instance_id": instance_id,
            "path": str(png),
            "preview_max_dim": 64,
        })
        gap = int(cap["frame_serial"]) - int(fix["frame_serial"])
        got = camera_bms(state)
        residual = [target["position"][i] - got[i] for i in range(3)]
        worst = max(abs(r) for r in residual)
        print(f"  {fixture_id} try{attempt}: gap={gap} worst={worst:.4f} "
              f"camera=({got[0]:.4f},{got[1]:.4f},{got[2]:.4f})")

        landed = (0 < gap <= 120) and (args.no_correct or worst <= TOLERANCE)
        if landed:
            save(out_dir / "instance-status.json", instances)
            save(out_dir / "fixture-result.json", fix)
            save(out_dir / "capture-result.json", cap)
            shutil.copyfile(args.stage_manifest, out_dir / "retail-stage.json")
            shutil.copyfile(log_path, out_dir / "onhook.log")
            print(f"OK  {fixture_id}: gap={gap} worst={worst:.4f}")
            return True

        # Undamped correction 2-cycles where reachable camera heights straddle
        # the target (ground snap, stance bob); halve the step.
        pos = [pos[i] + residual[i] * 0.5 for i in range(3)]
        time.sleep(0.3)

    print(f"BAD {fixture_id}: never landed within {TOLERANCE}")
    return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="bundle root; use a DURABLE path outside every "
                             "worktree (the absolute path is baked into "
                             "capture-result.json)")
    parser.add_argument("--onhook-mcp", required=True, type=Path)
    parser.add_argument("--game-dir", required=True, type=Path)
    parser.add_argument("--stage-manifest", required=True, type=Path)
    parser.add_argument("--expansion", default="revx02")
    parser.add_argument("--attempts", type=int, default=5)
    parser.add_argument(
        "--no-correct", action="store_true",
        help="capture at the catalog's applied position verbatim (what a "
             "catalog revision must be derived from); without it the driver "
             "corrects the pose to land on the catalog camera, which the "
             "registrar will not accept against that same catalog",
    )
    parser.add_argument(
        "--fresh-host", metavar="MISSION.bms",
        help="launch a dedicated retail process per fixture; required for "
             "water fixtures, where a player already in water is pinned",
    )
    parser.add_argument("--run-root", type=Path,
                        help="run artifact root for --fresh-host launches")
    parser.add_argument("fixtures", nargs="+")
    args = parser.parse_args(argv)

    catalog = json.loads(args.catalog.read_text(encoding="utf-8-sig"))
    client = Client(args.onhook_mcp)
    failures: list[str] = []
    try:
        if args.fresh_host:
            run_root = args.run_root or (args.output / "_runs")
            for fixture_id in args.fixtures:
                run_id = f"rp-{fixture_id}"[:60]
                print(f"launching fresh host for {fixture_id} ...")
                host = client.tool("onhook_host_lan", {
                    "mission": args.fresh_host,
                    "game_dir": str(args.game_dir),
                    "expansion": args.expansion,
                    "windowed": True,
                    "allow_many": True,
                    "run_id": run_id,
                    # Every run needs its own directory: the hook log is
                    # create-new, and a reused path makes the launch time out.
                    "output_dir": str(run_root / run_id),
                    "wait_timeout_ms": 240000,
                })
                try:
                    instances = client.tool("onhook_instances", {})
                    if not capture_fixture(
                        client, catalog, fixture_id, args,
                        host["instance_id"], instances,
                        Path(host["log_path"]),
                    ):
                        failures.append(fixture_id)
                finally:
                    client.tool("onhook_stop_run", {"run_id": host["run_id"]})
                    time.sleep(3)
        else:
            instances = client.tool("onhook_instances", {})
            live = [i for i in instances["instances"] if i.get("capture_ready")]
            if len(live) != 1:
                raise DriverError(
                    f"expected exactly 1 capture-ready instance, got {len(live)}"
                )
            if not args.run_root:
                raise DriverError(
                    "--run-root is required without --fresh-host, to locate "
                    "the running host's onhook.log"
                )
            log_path = args.run_root / "host" / "onhook.log"
            print(f"instance {live[0]['instance_id']} pid {live[0]['pid']}")
            for fixture_id in args.fixtures:
                if not capture_fixture(
                    client, catalog, fixture_id, args,
                    live[0]["instance_id"], instances, log_path,
                ):
                    failures.append(fixture_id)
                time.sleep(0.2)
    finally:
        client.close()

    if failures:
        print("FAILED: " + ", ".join(failures), file=sys.stderr)
        return 1
    print(f"captured {len(args.fixtures)} fixtures")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except DriverError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
