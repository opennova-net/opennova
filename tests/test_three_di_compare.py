from __future__ import annotations

import struct
from pathlib import Path

from pyopennova.three_di_policy_ffi import (
    OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    compare_files,
)


def _leaf(chunk_id: str, data: bytes) -> bytes:
    return chunk_id.encode("ascii") + struct.pack("<I", len(data)) + data


def _parent(chunk_id: str, *children: bytes) -> bytes:
    data = b"".join(children)
    return chunk_id.encode("ascii") + struct.pack("<I", 0x80000000 | len(data)) + data


def _write_3di(path: Path, root: bytes) -> None:
    path.write_bytes(b"3DI3" + struct.pack("<I", 259) + root)


def _compare(tmp_path: Path, expected_root: bytes, actual_root: bytes, *, policy: int) -> dict:
    expected = tmp_path / "expected.3di"
    actual = tmp_path / "actual.3di"
    report_path = tmp_path / "report.json"
    _write_3di(expected, expected_root)
    _write_3di(actual, actual_root)
    return compare_files(expected, actual, policy=policy, report_path=report_path)


def _geometry_tree(cdta_payload: bytes = b"expected", rdta_payload: bytes = b"expected") -> bytes:
    return _parent(
        "ROOT",
        _leaf("INFO", b"same"),
        _parent(
            "CDTA",
            _leaf("CMDL", bytes(64)),
            _leaf("CVRT", cdta_payload),
        ),
        _parent(
            "RDTA",
            _parent(
                "RLOD",
                _leaf("RMDL", bytes(12)),
                _leaf("VERT", rdta_payload),
            ),
        ),
    )


def _robj_payload(
    *,
    num_strips: int = 1,
    num_alpha_strips: int = 0,
    parent_index: int = -1,
    rel_x: float = 0.25,
) -> bytes:
    return struct.pack(
        "<IIiiiffffffffff",
        1,
        52,
        num_strips,
        num_alpha_strips,
        parent_index,
        rel_x,
        0.0,
        -0.5,
        0.25,
        0.0,
        -0.5,
        1.0,
        2.0,
        3.0,
        4.0,
    )


def _usrp_payload(
    *,
    x: int = 100,
    y: int = -200,
    z: int = 300,
    rot_x: int = 65536,
    rot_y: int = 0,
    rot_z: int = 0,
) -> bytes:
    return struct.pack(
        "<IIiiiiiiii16s",
        1,
        48,
        x,
        y,
        z,
        rot_x,
        rot_y,
        rot_z,
        4,
        ord("S"),
        b"Look",
    )


def _rdta_policy_tree(
    *,
    rmdl: bytes = bytes(12),
    panm_marker: int = 0,
    panm_parent_subobject: int = 0,
    robj: bytes | None = None,
) -> bytes:
    panm = bytearray(struct.pack("<II", 1, 68) + bytes(68))
    panm[8] = panm_marker
    panm[12] = panm_parent_subobject
    return _parent(
        "ROOT",
        _parent(
            "RDTA",
            _parent(
                "RLOD",
                _leaf("RMDL", rmdl),
                _leaf("VERT", b"deferred vert"),
                _leaf("INDX", b"deferred indx"),
                _leaf("STRP", b"deferred strp"),
                _leaf("ROBJ", robj if robj is not None else _robj_payload()),
                _leaf("PANM", bytes(panm)),
            ),
        ),
    )


def _event_paths(report: dict, status: str) -> set[str]:
    return {
        event["path"]
        for event in report["events"]
        if event["status"] == status
    }


def test_relaxed_compare_defers_cdta_and_rdta_rlod_child_payloads(tmp_path: Path) -> None:
    report = _compare(
        tmp_path,
        _geometry_tree(cdta_payload=b"expected collision", rdta_payload=b"expected render"),
        _geometry_tree(cdta_payload=b"actual collisionxx", rdta_payload=b"actual renderxx"),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )

    assert not report["failed"]
    assert _event_paths(report, "deferred") == {
        "CDTA/CMDL",
        "CDTA/CVRT",
        "RDTA/RLOD[0]/VERT",
    }


def test_relaxed_compare_keeps_non_stripification_rdta_children_strict(tmp_path: Path) -> None:
    rmdl_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(rmdl=bytes([1]) + bytes(11)),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert rmdl_report["failed"]
    assert "RDTA/RLOD[0]/RMDL" in _event_paths(rmdl_report, "fail")

    panm_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(panm_marker=1),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert panm_report["failed"]
    assert "RDTA/RLOD[0]/PANM" in _event_paths(panm_report, "fail")

    disabled_parent_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(panm_parent_subobject=7),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert not disabled_parent_report["failed"]


def test_relaxed_compare_partially_compares_robj(tmp_path: Path) -> None:
    strip_count_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(robj=_robj_payload(num_strips=3, num_alpha_strips=2)),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert not strip_count_report["failed"]

    tiny_float_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(robj=_robj_payload(rel_x=0.25000018)),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert not tiny_float_report["failed"]

    parent_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(robj=_robj_payload(parent_index=0)),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert parent_report["failed"]
    assert "RDTA/RLOD[0]/ROBJ" in _event_paths(parent_report, "fail")

    large_float_report = _compare(
        tmp_path,
        _rdta_policy_tree(),
        _rdta_policy_tree(robj=_robj_payload(rel_x=0.25001)),
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )
    assert large_float_report["failed"]
    assert "RDTA/RLOD[0]/ROBJ" in _event_paths(large_float_report, "fail")


def test_usrp_tolerates_position_ticks_but_keeps_direction_strict(tmp_path: Path) -> None:
    position_tick_report = _compare(
        tmp_path,
        _parent("ROOT", _leaf("USRP", _usrp_payload())),
        _parent("ROOT", _leaf("USRP", _usrp_payload(x=101))),
        policy=0,
    )
    assert not position_tick_report["failed"]
    assert "USRP" in _event_paths(position_tick_report, "numeric-tolerated")

    direction_tick_report = _compare(
        tmp_path,
        _parent("ROOT", _leaf("USRP", _usrp_payload())),
        _parent("ROOT", _leaf("USRP", _usrp_payload(rot_x=65535))),
        policy=0,
    )
    assert direction_tick_report["failed"]
    assert "USRP" in _event_paths(direction_tick_report, "fail")


def test_strict_compare_opens_cdta_and_rdta_rlod_child_payloads(tmp_path: Path) -> None:
    report = _compare(
        tmp_path,
        _geometry_tree(cdta_payload=b"expected", rdta_payload=b"expected"),
        _geometry_tree(cdta_payload=b"actualxx", rdta_payload=b"actualxx"),
        policy=0,
    )

    assert report["failed"]
    failed_paths = _event_paths(report, "fail")
    assert "CDTA/CVRT" in failed_paths
    assert "RDTA/RLOD[0]/VERT" in failed_paths


def test_relaxed_compare_still_requires_deferred_geometry_shape(tmp_path: Path) -> None:
    expected = _geometry_tree()
    actual = _parent(
        "ROOT",
        _leaf("INFO", b"same"),
        _parent("CDTA", _leaf("CMDL", bytes(64))),
        _parent("RDTA", _parent("RLOD", _leaf("VERT", b"same"), _leaf("RMDL", bytes(12)))),
    )

    report = _compare(
        tmp_path,
        expected,
        actual,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )

    assert report["failed"]
    failures = {event["path"]: event["reason"] for event in report["events"] if event["status"] == "fail"}
    assert "CDTA" in failures or "RDTA/RLOD[0]" in failures


def test_relaxed_compare_does_not_mask_non_geometry_payload_drift(tmp_path: Path) -> None:
    expected = _parent("ROOT", _leaf("INFO", b"expected"))
    actual = _parent("ROOT", _leaf("INFO", b"actualxx"))

    report = _compare(
        tmp_path,
        expected,
        actual,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    )

    assert report["failed"]
    assert any(event["path"] == "INFO" for event in report["events"])
