"""Focused MTRX tolerance tests through the native 3DI comparator."""

from __future__ import annotations

import struct
from pathlib import Path

from pyopennova.three_di_policy_ffi import compare_files


def _leaf(chunk_id: str, data: bytes) -> bytes:
    return chunk_id.encode("ascii") + struct.pack("<I", len(data)) + data


def _parent(chunk_id: str, *children: bytes) -> bytes:
    data = b"".join(children)
    return chunk_id.encode("ascii") + struct.pack("<I", 0x80000000 | len(data)) + data


def _write_3di(path: Path, root: bytes) -> None:
    path.write_bytes(b"3DI3" + struct.pack("<I", 259) + root)


def _mtrx_payload(first_word: int) -> bytes:
    return struct.pack("<II", 1, 64) + struct.pack("<I", first_word) + bytes(60)


def _compare_mtrx(tmp_path: Path, left_word: int, right_word: int) -> dict:
    expected = tmp_path / "expected.3di"
    actual = tmp_path / "actual.3di"
    report_path = tmp_path / "report.json"
    _write_3di(expected, _parent("ROOT", _leaf("MTRX", _mtrx_payload(left_word))))
    _write_3di(actual, _parent("ROOT", _leaf("MTRX", _mtrx_payload(right_word))))
    return compare_files(expected, actual, report_path=report_path)


def test_mtrx_nan_pair_with_different_sign_bits_is_tolerated(tmp_path: Path) -> None:
    report = _compare_mtrx(tmp_path, 0xFFC00000, 0x7FC00000)

    assert not report["failed"]
    assert any(event["path"] == "MTRX" for event in report["events"])


def test_mtrx_nan_vs_finite_value_is_not_tolerated(tmp_path: Path) -> None:
    report = _compare_mtrx(tmp_path, 0xFFC00000, 0x3F800000)

    assert report["failed"]
    assert any(event["path"] == "MTRX" for event in report["events"])


def test_mtrx_one_ulp_finite_value_is_tolerated(tmp_path: Path) -> None:
    report = _compare_mtrx(tmp_path, 0x3F800000, 0x3F800001)

    assert not report["failed"]
    assert any(event["status"] == "fp-tolerated" for event in report["events"])


def test_nan_tolerance_does_not_apply_to_other_chunks(tmp_path: Path) -> None:
    expected = tmp_path / "expected.3di"
    actual = tmp_path / "actual.3di"
    report_path = tmp_path / "report.json"
    payload = struct.pack("<II", 1, 584) + struct.pack("<I", 0xFFC00000) + bytes(580)
    changed = struct.pack("<II", 1, 584) + struct.pack("<I", 0x7FC00000) + bytes(580)
    _write_3di(expected, _parent("ROOT", _leaf("MTRL", payload)))
    _write_3di(actual, _parent("ROOT", _leaf("MTRL", changed)))

    report = compare_files(expected, actual, report_path=report_path)

    assert report["failed"]
    assert any(event["path"] == "MTRL" for event in report["events"])
