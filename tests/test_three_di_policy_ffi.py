from __future__ import annotations

import json
from pathlib import Path


class _NativePolicyFn:
    restype = None
    argtypes = None

    def __init__(self, rc: int, payload: dict) -> None:
        self.rc = rc
        self.payload = payload
        self.calls: list[tuple[bytes, bytes, int, bytes]] = []

    def __call__(self, expected_path: bytes, actual_path: bytes, flags: int, report_path: bytes) -> int:
        self.calls.append((expected_path, actual_path, int(getattr(flags, "value", flags)), report_path))
        Path(report_path.decode("utf-8")).write_text(json.dumps(self.payload), encoding="utf-8")
        return self.rc


class _FakeLib:
    def __init__(self, compare_fn: _NativePolicyFn, validate_fn: _NativePolicyFn) -> None:
        self.object_3di_compare_files = compare_fn
        self.object_3di_validate_geometry_chunks = validate_fn


def test_compare_files_returns_native_report_with_status(monkeypatch, tmp_path: Path) -> None:
    from blender.opennova import three_di_policy_ffi

    payload = {
        "failed": False,
        "events": [{"path": "USRP", "status": "numeric-tolerated", "reason": "tick"}],
    }
    compare_fn = _NativePolicyFn(0, payload)
    validate_fn = _NativePolicyFn(0, {"failed": False, "events": []})
    monkeypatch.setattr(three_di_policy_ffi, "_bound", False)
    monkeypatch.setattr(three_di_policy_ffi, "load_lib", lambda: _FakeLib(compare_fn, validate_fn))

    report_path = tmp_path / "compare.json"
    report = three_di_policy_ffi.compare_files(
        "expected.3di",
        "actual.3di",
        policy=three_di_policy_ffi.OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        report_path=report_path,
    )

    assert report["status"] == 0
    assert report["events"][0]["status"] == "numeric-tolerated"
    assert compare_fn.calls == [
        (
            b"expected.3di",
            b"actual.3di",
            three_di_policy_ffi.OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
            str(report_path).encode("utf-8"),
        )
    ]


def test_compare_files_raises_on_native_error(monkeypatch, tmp_path: Path) -> None:
    from blender.opennova import three_di_policy_ffi
    import pytest

    compare_fn = _NativePolicyFn(-2, {"failed": True, "events": []})
    validate_fn = _NativePolicyFn(0, {"failed": False, "events": []})
    monkeypatch.setattr(three_di_policy_ffi, "_bound", False)
    monkeypatch.setattr(three_di_policy_ffi, "load_lib", lambda: _FakeLib(compare_fn, validate_fn))

    with pytest.raises(RuntimeError, match="status -2"):
        three_di_policy_ffi.compare_files(
            "expected.3di",
            "actual.3di",
            report_path=tmp_path / "compare.json",
        )
