from __future__ import annotations

from pathlib import Path


RDTA_DEFERRED_LEAVES = {"VERT", "INDX", "STRP"}
TOLERATED_STATUSES = {"numeric-tolerated", "fp-tolerated"}
STOCK_DCC_COMPARE_TOLERANCES = {
    "akcrate": {("USRP", "numeric-tolerated")},
    "Armry01": {
        ("USRP", "numeric-tolerated"),
        ("OCCL/OOBJ", "numeric-tolerated"),
    },
    "Beret": {
        ("USRP", "numeric-tolerated"),
        ("MTRX", "fp-tolerated"),
    },
    "mp5_1st": {("MTRX", "fp-tolerated")},
    "US01": {
        ("USRP", "numeric-tolerated"),
        ("MTRX", "fp-tolerated"),
    },
}


def _is_deferred_geometry(path: str) -> bool:
    if path.startswith("CDTA/"):
        return True
    if path.startswith("RDTA/RLOD["):
        return path.rsplit("/", 1)[-1] in RDTA_DEFERRED_LEAVES
    return False


def _is_allowed_tolerance(
    path: str,
    status: str,
    allowed_tolerances: set[tuple[str, str]],
) -> bool:
    if (path, status) in allowed_tolerances:
        return True
    if status in TOLERATED_STATUSES and path.startswith("RDTA/RLOD[") and path.endswith("/ROBJ"):
        return True
    if status in TOLERATED_STATUSES and path.startswith("RDTA/RLOD[") and path.endswith("/PANM"):
        return True
    return False


def assert_tight_non_geometry_compare(
    report: dict,
    report_path: Path,
    *,
    allowed_tolerances: set[tuple[str, str]],
) -> None:
    """Fail if a 3DI compare report hides unexpected non-geometry tolerance."""
    unexpected: list[dict] = []
    for event in report.get("events", []):
        path = str(event.get("path", ""))
        status = str(event.get("status", ""))
        if status == "byte-exact":
            continue
        if status == "deferred" and _is_deferred_geometry(path):
            continue
        if _is_allowed_tolerance(path, status, allowed_tolerances):
            continue
        unexpected.append(event)

    assert not unexpected, (
        f"Unexpected 3DI compare tolerance/failure in {report_path}: {unexpected}\n"
        f"{report_path.read_text(encoding='utf-8', errors='replace')}"
    )
