#!/usr/bin/env python
"""Mint a retail fixture-catalog revision from one self-consistent session.

The catalog pins both ends of the retail leg: the applied player position
(``register_retail_capture.py``, 1e-5) and the camera the frame renders with
(0.05 per axis, checked by both the registrar and the comparison builder). But
``onhook_apply_render_fixture`` can only set the player pose -- retail derives
the camera -- so the two fields are only jointly satisfiable by the session
that produced them. Refreshing the retail leg therefore means minting a new
catalog revision: keep every applied field verbatim, and replace each
fixture's ``camera_bms.position`` with the camera the fresh captures actually
rendered, solved from each frame's own view matrix.

Feed it the bundles produced by ``retail_capture_driver.py --no-correct``
(one ``retail.state.json`` per fixture). The canonical ``catalog_sha256`` is
recomputed the same way the registrar verifies it.

Usage::

    uv run python scripts/render/mint_retail_catalog.py \
      --base docs/render/render-fixtures-retail-v<N>.json \
      --bundles C:/evidence/retail-<date> \
      --output docs/render/render-fixtures-retail-v<N+1>.json

See docs/render/render-parity-runbook.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from retail_capture_driver import camera_bms


class MintError(RuntimeError):
    """A fail-closed catalog-minting error."""


def canonical_sha256(catalog: dict) -> str:
    payload = dict(catalog)
    payload.pop("catalog_sha256", None)
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode()
    return hashlib.sha256(canonical).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path,
                        help="the catalog revision being superseded")
    parser.add_argument("--bundles", required=True, type=Path,
                        help="root of the --no-correct capture bundles, one "
                             "directory per fixture id")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)

    catalog = json.loads(args.base.read_text(encoding="utf-8-sig"))
    if catalog.get("schema") != "opennova.render-fixtures.v2":
        raise MintError("unsupported base catalog schema")
    if canonical_sha256(catalog) != catalog.get("catalog_sha256"):
        raise MintError("base catalog fails its own canonical hash")

    derivation = (
        "APPLY_POSE request kept verbatim from the previous revision; "
        "camera_bms recalibrated from this revision's registered inverse "
        "view matrix"
    )
    for fixture in catalog["fixtures"]:
        fixture_id = fixture["id"]
        state_path = args.bundles / fixture_id / "retail.state.json"
        if not state_path.is_file():
            raise MintError(f"missing capture bundle state: {state_path}")
        state = json.loads(state_path.read_text(encoding="utf-8-sig"))

        # The bundle must be a verbatim capture of this fixture's applied
        # pose -- a corrected pose would mint a catalog the registrar then
        # rejects against these same bundles. Retail's ground snap may settle
        # the OBSERVED player after the teleport; what the registrar pins at
        # 1e-5 is the REQUESTED position, so that is what we verify here.
        if state.get("fixture_context", {}).get("fixture_id") != fixture_id:
            raise MintError(f"{state_path} does not describe {fixture_id}")
        result_path = args.bundles / fixture_id / "fixture-result.json"
        if not result_path.is_file():
            raise MintError(f"missing fixture application result: {result_path}")
        result = json.loads(result_path.read_text(encoding="utf-8-sig"))
        requested = result.get("fixture", {}).get("position_bms", {})
        applied = fixture["retail_player_bms"]["applied"]
        drift = max(
            abs(float(requested.get(axis, float("nan"))) - float(value))
            for axis, value in zip(("x", "y", "z"), applied)
        )
        if not drift <= 1.0e-5:
            raise MintError(
                f"{fixture_id}: applied position differs from the catalog by "
                f"{drift} -- not a verbatim capture"
            )

        solved = camera_bms(state_path)
        fixture["camera_bms"]["position"] = solved
        provenance = fixture["retail_player_bms"]
        provenance["derivation"] = derivation
        provenance["provenance_status"] = "required_at_capture"
        provenance.pop("legacy_observed", None)
        print(f"{fixture_id}: camera=({solved[0]:.4f},{solved[1]:.4f},"
              f"{solved[2]:.4f})")

    catalog["retail_eye_model"]["derivation"] = (
        "No eye model is assumed: camera_bms is recovered from each "
        "registered frame's inverse view matrix, and APPLY_POSE positions "
        "remain requests."
    )
    catalog["catalog_sha256"] = canonical_sha256(catalog)
    args.output.write_text(
        json.dumps(catalog, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"wrote {args.output} catalog_sha256={catalog['catalog_sha256']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MintError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
