"""Strict 3DI3 chunk-tree comparison helpers.

This intentionally works from the raw 3DI3 chunk tree instead of
``Threedi3di3`` so missing typed-model fields cannot create false confidence.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
import math
import struct
import tempfile
from pathlib import Path
from typing import Iterable

from pyopennova.three_di_policy_ffi import (
    OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    compare_files,
)
from pyopennova.threedi_ffi import read_chunk_tree_3di3


_VERTEX_LAYER_NAMES = {
    (40, 0x01): "static-simple",
    (64, 0x15): "static-advanced",
    (56, 0x41): "skinned-simple",
    (80, 0x55): "skinned-advanced",
}

DEFAULT_DEFERRED_SUBTREE_IDS: tuple[str, ...] = (
    "CDTA/CVRT",
    "CDTA/CNRM",
    "CDTA/CFAC",
    "CDTA/BPLN",
    "CDTA/BVOL",
    "CDTA/COBJ",
    "CDTA/CXLT",
    "RDTA/RLOD/VERT",
    "RDTA/RLOD/INDX",
    "RDTA/RLOD/STRP",
    "RDTA/RLOD/ROBJ",
    "RDTA/RLOD/PANM",
)
DEFAULT_STRICT_TARGET_CHUNK_IDS: tuple[str, ...] = (
    "INFO",
    "GHDR",
    "USRP",
    "MTRL",
    "LGHT",
    "MTRX",
)

_CDTA_CHILD_ORDER: tuple[str, ...] = (
    "CMDL",
    "CVRT",
    "CNRM",
    "CFAC",
    "BPLN",
    "BVOL",
    "COBJ",
    "CXLT",
)

_RLOD_CHILD_ORDER: tuple[str, ...] = (
    "RMDL",
    "VERT",
    "INDX",
    "STRP",
    "ROBJ",
    "PANM",
)


@dataclass
class Chunk:
    id: str
    offset: int
    content_len: int
    is_parent: bool
    data: bytes = b""
    children: list["Chunk"] = field(default_factory=list)


@dataclass
class ComparisonEvent:
    path: str
    status: str
    reason: str


@dataclass
class ComparisonReport:
    events: list[ComparisonEvent] = field(default_factory=list)

    @property
    def failed(self) -> bool:
        return any(event.status == "fail" for event in self.events)

    def add(self, path: str, status: str, reason: str) -> None:
        self.events.append(ComparisonEvent(path, status, reason))

    def assert_ok(self) -> None:
        if not self.failed:
            return
        failures = [event for event in self.events if event.status == "fail"]
        preview = "\n".join(
            f"{event.path}: {event.reason}" for event in failures[:10]
        )
        raise AssertionError(preview)

    def to_json(self) -> str:
        return json.dumps(
            [event.__dict__ for event in self.events],
            indent=2,
            sort_keys=True,
        )

    def parity_table(self) -> list[dict[str, str]]:
        return [
            {
                "path": event.path,
                "status": _table_status(event),
                "reason": event.reason,
            }
            for event in self.events
        ]


def _table_status(event: ComparisonEvent) -> str:
    if event.status == "fail":
        return "fail"
    if event.status == "skip":
        return "skipped"
    if event.status == "byte-exact":
        return "byte-exact"
    if event.status == "fp-tolerated":
        return "fp-tolerated"
    if "deferred subtree" in event.reason or "deferred path" in event.reason:
        return "deferred"
    if "deferred topology counts" in event.reason:
        return "deferred"
    if event.reason.startswith("exact "):
        return "byte-exact"
    if (
        event.reason.startswith("masked ")
        or event.reason.startswith("case-folded ")
        or event.reason.startswith("policy-normalized")
    ):
        return "masked"
    if "tolerated" in event.reason or "relaxed" in event.reason:
        return "fp-tolerated"
    return event.status


def read_3di3_chunks(path: str | Path) -> Chunk:
    return _chunk_from_native(read_chunk_tree_3di3(path))


def snapshot_json(path: str | Path) -> str:
    root = read_3di3_chunks(path)
    return json.dumps(_snapshot(root), indent=2, sort_keys=True)


def compare_3di3(
    expected_path: str | Path,
    actual_path: str | Path,
    *,
    skip_ghdr: bool = True,
    deferred_subtree_ids: Iterable[str] = DEFAULT_DEFERRED_SUBTREE_IDS,
    strict_chunk_ids: Iterable[str] = (),
) -> ComparisonReport:
    """Compatibility wrapper around the native C++ 3DI comparator.

    New tests should import pyopennova.three_di_policy_ffi.compare_files
    directly. This shim keeps older tools on the native comparator instead of
    the removed Python parity implementation.
    """
    strict = tuple(strict_chunk_ids)
    if strict:
        raise ValueError("strict_chunk_ids is not supported by the native 3DI comparator")
    deferred = frozenset(deferred_subtree_ids)
    if not deferred:
        policy = 0
    elif deferred == frozenset(DEFAULT_DEFERRED_SUBTREE_IDS):
        policy = OBJECT_3DI_COMPARE_RELAX_GEOMETRY
    else:
        raise ValueError("custom deferred_subtree_ids are not supported by the native 3DI comparator")

    _ = skip_ghdr
    with tempfile.TemporaryDirectory(prefix="opennova_3di_compare_") as tmp:
        payload = compare_files(
            expected_path,
            actual_path,
            policy=policy,
            report_path=Path(tmp) / "report.json",
        )
    report = ComparisonReport()
    for event in payload.get("events", []):
        report.add(
            str(event.get("path", "")),
            str(event.get("status", "")),
            str(event.get("reason", "")),
        )
    return report


def _chunk_from_native(chunk) -> Chunk:
    return Chunk(
        id=chunk.id,
        offset=chunk.offset,
        content_len=chunk.content_len,
        is_parent=chunk.is_parent,
        data=chunk.data,
        children=[_chunk_from_native(child) for child in chunk.children],
    )


def _snapshot(chunk: Chunk) -> dict:
    out = {
        "id": chunk.id,
        "offset": chunk.offset,
        "content_len": chunk.content_len,
        "is_parent": chunk.is_parent,
    }
    if chunk.is_parent:
        out["children"] = [_snapshot(child) for child in chunk.children]
    else:
        out["sha256"] = hashlib.sha256(chunk.data).hexdigest()
    return out


def _path_ends(path: str, chunk_id: str) -> bool:
    return path == chunk_id or path.endswith("/" + chunk_id)


def _policy_path(path: str) -> str:
    parts: list[str] = []
    for part in path.split("/"):
        parts.append("RLOD" if part.startswith("RLOD[") else part)
    return "/".join(parts)


def _is_deferred_path(path: str, deferred_paths: frozenset[str]) -> bool:
    return path in deferred_paths or _policy_path(path) in deferred_paths


def _compare_children(
    expected: Chunk,
    actual: Chunk,
    path: str,
    report: ComparisonReport,
    *,
    skip_ghdr: bool,
    deferred_subtree_ids: frozenset[str],
    strict_chunk_ids: frozenset[str],
) -> None:
    expected_rlods = expected.children and all(c.id == "RLOD" for c in expected.children)
    actual_rlods = actual.children and all(c.id == "RLOD" for c in actual.children)
    if expected_rlods or actual_rlods:
        if len(expected.children) != len(actual.children):
            report.add(path or "/", "fail",
                       f"RLOD count mismatch: {len(expected.children)} != {len(actual.children)}")
        for index, (left, right) in enumerate(zip(expected.children, actual.children)):
            _compare_chunk(
                left,
                right,
                f"{path}/RLOD[{index}]",
                report,
                skip_ghdr=skip_ghdr,
                deferred_subtree_ids=deferred_subtree_ids,
                strict_chunk_ids=strict_chunk_ids,
            )
        return

    used: set[int] = set()
    for left in expected.children:
        child_path = f"{path}/{left.id}" if path else left.id
        if skip_ghdr and left.id == "GHDR":
            match = _find_unmatched(actual.children, left.id, used)
            if match is not None:
                used.add(match)
            report.add(child_path, "skip", "GHDR carries model name/path noise")
            continue
        match = _find_unmatched(actual.children, left.id, used)
        if match is None:
            report.add(child_path, "fail", "missing in actual")
            continue
        used.add(match)
        _compare_chunk(
            left,
            actual.children[match],
            child_path,
            report,
            skip_ghdr=skip_ghdr,
            deferred_subtree_ids=deferred_subtree_ids,
            strict_chunk_ids=strict_chunk_ids,
        )

    for index, right in enumerate(actual.children):
        if index not in used:
            child_path = f"{path}/{right.id}" if path else right.id
            report.add(child_path, "fail", "extra in actual")


def _find_unmatched(children: Iterable[Chunk], chunk_id: str, used: set[int]) -> int | None:
    for index, child in enumerate(children):
        if index not in used and child.id == chunk_id:
            return index
    return None


def _compare_chunk(
    expected: Chunk,
    actual: Chunk,
    path: str,
    report: ComparisonReport,
    *,
    skip_ghdr: bool,
    deferred_subtree_ids: frozenset[str],
    strict_chunk_ids: frozenset[str],
) -> None:
    if expected.id != actual.id:
        report.add(path, "fail", f"id mismatch: {expected.id} != {actual.id}")
        return
    if expected.is_parent != actual.is_parent:
        report.add(path, "fail", "parent flag mismatch")
        return
    if _is_deferred_path(path, deferred_subtree_ids):
        report.add(path, "pass", "deferred path")
        return
    if expected.is_parent:
        policy_path = _policy_path(path)
        if policy_path == "CDTA":
            _compare_ordered_children(
                expected,
                actual,
                path,
                report,
                allowed_order=_CDTA_CHILD_ORDER,
                skip_ghdr=skip_ghdr,
                deferred_subtree_ids=deferred_subtree_ids,
                strict_chunk_ids=strict_chunk_ids,
            )
        elif policy_path == "RDTA":
            _compare_rdta_children(
                expected,
                actual,
                path,
                report,
                skip_ghdr=skip_ghdr,
                deferred_subtree_ids=deferred_subtree_ids,
                strict_chunk_ids=strict_chunk_ids,
            )
        elif policy_path == "RDTA/RLOD":
            _compare_ordered_children(
                expected,
                actual,
                path,
                report,
                allowed_order=_RLOD_CHILD_ORDER,
                skip_ghdr=skip_ghdr,
                deferred_subtree_ids=deferred_subtree_ids,
                strict_chunk_ids=strict_chunk_ids,
            )
        else:
            _compare_children(
                expected,
                actual,
                path,
                report,
                skip_ghdr=skip_ghdr,
                deferred_subtree_ids=deferred_subtree_ids,
                strict_chunk_ids=strict_chunk_ids,
            )
    else:
        _compare_leaf(expected, actual, path, report, strict_chunk_ids=strict_chunk_ids)


def _compare_rdta_children(
    expected: Chunk,
    actual: Chunk,
    path: str,
    report: ComparisonReport,
    *,
    skip_ghdr: bool,
    deferred_subtree_ids: frozenset[str],
    strict_chunk_ids: frozenset[str],
) -> None:
    expected_ids = tuple(child.id for child in expected.children)
    actual_ids = tuple(child.id for child in actual.children)
    if any(chunk_id != "RLOD" for chunk_id in expected_ids + actual_ids):
        report.add(path, "fail", f"child order mismatch: expected {expected_ids!r} actual {actual_ids!r}")
        return
    if len(expected.children) != len(actual.children):
        report.add(path, "fail", f"RLOD count mismatch: {len(expected.children)} != {len(actual.children)}")
        return
    for index, (left, right) in enumerate(zip(expected.children, actual.children)):
        _compare_chunk(
            left,
            right,
            f"{path}/RLOD[{index}]",
            report,
            skip_ghdr=skip_ghdr,
            deferred_subtree_ids=deferred_subtree_ids,
            strict_chunk_ids=strict_chunk_ids,
        )


def _compare_ordered_children(
    expected: Chunk,
    actual: Chunk,
    path: str,
    report: ComparisonReport,
    *,
    allowed_order: tuple[str, ...],
    skip_ghdr: bool,
    deferred_subtree_ids: frozenset[str],
    strict_chunk_ids: frozenset[str],
) -> None:
    expected_ids = tuple(child.id for child in expected.children)
    actual_ids = tuple(child.id for child in actual.children)
    if not _child_ids_follow_order(expected_ids, allowed_order):
        report.add(path, "fail", f"unexpected expected child order: {expected_ids!r}")
        return
    if not _child_ids_follow_order(actual_ids, allowed_order) or actual_ids != expected_ids:
        report.add(path, "fail", f"child order mismatch: expected {expected_ids!r} actual {actual_ids!r}")
        return
    for left, right in zip(expected.children, actual.children):
        _compare_chunk(
            left,
            right,
            f"{path}/{left.id}",
            report,
            skip_ghdr=skip_ghdr,
            deferred_subtree_ids=deferred_subtree_ids,
            strict_chunk_ids=strict_chunk_ids,
        )


def _child_ids_follow_order(child_ids: tuple[str, ...], allowed_order: tuple[str, ...]) -> bool:
    order = {chunk_id: index for index, chunk_id in enumerate(allowed_order)}
    previous = -1
    for chunk_id in child_ids:
        index = order.get(chunk_id)
        if index is None or index <= previous:
            return False
        previous = index
    return True


def _compare_leaf(
    expected: Chunk,
    actual: Chunk,
    path: str,
    report: ComparisonReport,
    *,
    strict_chunk_ids: frozenset[str],
) -> None:
    if expected.data == actual.data:
        status = "byte-exact" if _is_strict_path(path, strict_chunk_ids) else "pass"
        report.add(path, status, f"exact {len(expected.data)} bytes")
        return
    if _is_strict_path(path, strict_chunk_ids):
        _compare_strict_leaf(expected.data, actual.data, path, report)
        return
    if _structured_leaf_compare(expected.data, actual.data, path, report):
        return
    if _leaf_equal_with_policy(expected.data, actual.data, path, report):
        return
    report.add(path, "fail", _first_diff(expected.data, actual.data))


def _is_strict_path(path: str, strict_chunk_ids: frozenset[str]) -> bool:
    return any(_path_ends(path, chunk_id) for chunk_id in strict_chunk_ids)


def _compare_strict_leaf(
    expected: bytes,
    actual: bytes,
    path: str,
    report: ComparisonReport,
) -> None:
    if len(expected) != len(actual):
        report.add(path, "fail", f"size mismatch: {len(expected)} != {len(actual)}")
        return
    if _path_ends(path, "MTRX") and _compare_strict_mtrx_fields(path, expected, actual, report):
        return
    if _path_ends(path, "LGHT") and _compare_strict_lght_fields(path, expected, actual, report):
        return
    if _path_ends(path, "USRP") and _compare_strict_usrp_fields(path, expected, actual, report):
        return
    report.add(path, "fail", _first_diff(expected, actual))


def _structured_leaf_compare(
    expected: bytes,
    actual: bytes,
    path: str,
    report: ComparisonReport,
) -> bool:
    if _path_ends(path, "CMDL"):
        return _compare_cmdl_fields(path, expected, actual, report)
    if _path_ends(path, "CNRM"):
        return _compare_record_fields(path, expected, actual, 8, _CNRM_FIELDS, report)
    if _path_ends(path, "CFAC"):
        return _compare_record_fields(path, expected, actual, 44, _CFAC_FIELDS, report)
    if _path_ends(path, "VERT"):
        return _compare_vert_fields(path, expected, actual, report)
    if _path_ends(path, "INDX"):
        return _compare_indx_fields(path, expected, actual, report)
    if _path_ends(path, "STRP"):
        return _compare_strp_fields(path, expected, actual, report)
    if _path_ends(path, "ROBJ"):
        return _compare_record_fields(path, expected, actual, 52, _ROBJ_FIELDS, report)
    if _path_ends(path, "USRP"):
        return _compare_usrp_fields(path, expected, actual, report)
    if _path_ends(path, "MTRX"):
        return _compare_mtrx_fields(path, expected, actual, report)
    return False


@dataclass(frozen=True)
class _FieldSpec:
    name: str
    fmt: str
    offset: int
    relaxed: bool = False


_CFAC_FIELDS = (
    _FieldSpec("vert_index[0]", "h", 0),
    _FieldSpec("vert_index[1]", "h", 2),
    _FieldSpec("vert_index[2]", "h", 4),
    _FieldSpec("normal_index", "h", 6, relaxed=True),
    _FieldSpec("plane_dist_fp16", "i", 8),
    _FieldSpec("min_x_fp16", "i", 12),
    _FieldSpec("min_y_fp16", "i", 16),
    _FieldSpec("min_z_fp16", "i", 20),
    _FieldSpec("max_x_fp16", "i", 24),
    _FieldSpec("max_y_fp16", "i", 28),
    _FieldSpec("max_z_fp16", "i", 32),
    _FieldSpec("material_flags", "I", 36),
    _FieldSpec("poly_type", "B", 40),
    _FieldSpec("pad[0]", "B", 41, relaxed=True),
    _FieldSpec("pad[1]", "B", 42, relaxed=True),
    _FieldSpec("pad[2]", "B", 43, relaxed=True),
)


_CMDL_FIELDS = (
    _FieldSpec("bbox_min_x_fp16", "i", 0),
    _FieldSpec("bbox_min_y_fp16", "i", 4),
    _FieldSpec("bbox_min_z_fp16", "i", 8),
    _FieldSpec("bbox_max_x_fp16", "i", 12),
    _FieldSpec("bbox_max_y_fp16", "i", 16),
    _FieldSpec("bbox_max_z_fp16", "i", 20),
    _FieldSpec("max_radius_fp16", "i", 24),
    _FieldSpec("max_radius_xy_fp16", "i", 28),
    _FieldSpec("max_radius_z_fp16", "i", 32),
    _FieldSpec("num_vertices", "i", 36),
    _FieldSpec("num_normals", "i", 40),
    _FieldSpec("num_faces", "i", 44),
    _FieldSpec("num_objects", "i", 48),
    _FieldSpec("num_transforms", "i", 52),
    _FieldSpec("num_bounding_planes", "i", 56),
    _FieldSpec("num_bounding_volumes", "i", 60),
)

_CMDL_DEFERRED_TOPOLOGY_FIELDS = frozenset({
    "num_vertices",
    "num_normals",
    "num_faces",
})


_CNRM_FIELDS = (
    _FieldSpec("normal_x_s1_14", "h", 0),
    _FieldSpec("normal_y_s1_14", "h", 2),
    _FieldSpec("normal_z_s1_14", "h", 4),
    _FieldSpec("dominant_axis", "h", 6),
)


_USRP_FIELDS = (
    _FieldSpec("x_fp16", "i", 0),
    _FieldSpec("y_fp16", "i", 4),
    _FieldSpec("z_fp16", "i", 8),
    _FieldSpec("rot_x_fp16", "i", 12),
    _FieldSpec("rot_y_fp16", "i", 16),
    _FieldSpec("rot_z_fp16", "i", 20),
    _FieldSpec("subobject_index", "i", 24),
    _FieldSpec("userpoint_type", "i", 28),
    _FieldSpec("name", "16s", 32),
)


_STRP_FIELDS_48 = (
    _FieldSpec("material_index", "i", 0),
    _FieldSpec("index_offset", "i", 4),
    _FieldSpec("num_indices", "H", 8),
    _FieldSpec("num_triangles", "H", 10),
    _FieldSpec("is_strip", "i", 12),
    _FieldSpec("start_vertex", "i", 16),
    _FieldSpec("num_vertices", "i", 20),
    _FieldSpec("min.x", "f", 24),
    _FieldSpec("min.y", "f", 28),
    _FieldSpec("min.z", "f", 32),
    _FieldSpec("max.x", "f", 36),
    _FieldSpec("max.y", "f", 40),
    _FieldSpec("max.z", "f", 44),
)


_STRP_FIELDS_68 = _STRP_FIELDS_48 + tuple(
    _FieldSpec(f"bone_table[{index}]", "B", 48 + index) for index in range(16)
) + (
    _FieldSpec("bone_table_length", "i", 64),
)


_ROBJ_FIELDS = (
    _FieldSpec("num_strips", "i", 0),
    _FieldSpec("num_alpha_strips", "i", 4),
    _FieldSpec("parent_index", "i", 8),
    _FieldSpec("rel.x", "f", 12),
    _FieldSpec("rel.y", "f", 16),
    _FieldSpec("rel.z", "f", 20),
    _FieldSpec("abs.x", "f", 24),
    _FieldSpec("abs.y", "f", 28),
    _FieldSpec("abs.z", "f", 32),
    _FieldSpec("bounding_center.x", "f", 36),
    _FieldSpec("bounding_center.y", "f", 40),
    _FieldSpec("bounding_center.z", "f", 44),
    _FieldSpec("bounding_radius", "f", 48),
)


def _compare_struct_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    expected_size: int,
    fields: tuple[_FieldSpec, ...],
    report: ComparisonReport,
) -> bool:
    if len(expected) != expected_size or len(actual) != expected_size:
        report.add(path, "fail", f"struct payload size mismatch: {len(expected)} != {len(actual)} expected {expected_size}")
        return True

    failures: list[str] = []
    relaxed: list[str] = []
    for field in fields:
        left = _field_value(expected, 0, field)
        right = _field_value(actual, 0, field)
        if _field_bytes(expected, actual, 0, field):
            continue
        field_path = f"{path}.{field.name}"
        if field.relaxed:
            relaxed.append(field_path)
            continue
        close, note = _field_close(left, right, field, path, expected, actual, 0)
        if close:
            relaxed.append(f"{field_path} ({note})")
            continue
        failures.append(f"{field_path}: expected={left!r} actual={right!r}")
        if len(failures) >= 10:
            break

    if failures:
        report.add(path, "fail", "; ".join(failures))
    else:
        note = "field comparison matched"
        if relaxed:
            note = "field comparison matched; relaxed " + ", ".join(relaxed[:8])
        report.add(path, "pass", note)
    return True


def _compare_cmdl_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    if len(expected) != 64 or len(actual) != 64:
        report.add(path, "fail", f"struct payload size mismatch: {len(expected)} != {len(actual)} expected 64")
        return True

    failures: list[str] = []
    relaxed: list[str] = []
    deferred: list[str] = []
    for field in _CMDL_FIELDS:
        left = _field_value(expected, 0, field)
        right = _field_value(actual, 0, field)
        if _field_bytes(expected, actual, 0, field):
            continue
        field_path = f"{path}.{field.name}"
        close, note = _field_close(left, right, field, path, expected, actual, 0)
        if close:
            relaxed.append(f"{field_path} ({note})")
            continue
        if field.name in _CMDL_DEFERRED_TOPOLOGY_FIELDS:
            deferred.append(f"{field_path}: expected={left!r} actual={right!r}")
            continue
        failures.append(f"{field_path}: expected={left!r} actual={right!r}")
        if len(failures) >= 10:
            break

    if failures:
        report.add(path, "fail", "; ".join(failures))
        return True

    note_parts = ["field comparison matched"]
    if relaxed:
        note_parts.append("relaxed " + ", ".join(relaxed[:8]))
    if deferred:
        preview = ", ".join(deferred[:8])
        if len(deferred) > 8:
            preview += f", +{len(deferred) - 8} more"
        note_parts.append("deferred topology counts " + preview)
    report.add(path, "pass", "; ".join(note_parts))
    return True


def _compare_record_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    expected_record_size: int,
    fields: tuple[_FieldSpec, ...],
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, expected_record_size)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True

    count, record_size = struct.unpack_from("<II", expected, 0)
    failures: list[str] = []
    relaxed: list[str] = []
    for record_index in range(count):
        base = 8 + record_index * record_size
        for field in fields:
            left = _field_value(expected, base, field)
            right = _field_value(actual, base, field)
            if _field_bytes(expected, actual, base, field):
                continue
            field_path = f"{path}[{record_index}].{field.name}"
            if field.relaxed:
                relaxed.append(field_path)
                continue
            close, note = _field_close(left, right, field, path, expected, actual, base)
            if close:
                relaxed.append(f"{field_path} ({note})")
                continue
            failures.append(f"{field_path}: expected={left!r} actual={right!r}")
            if len(failures) >= 10:
                break
        if len(failures) >= 10:
            break

    if failures:
        report.add(path, "fail", "; ".join(failures))
    else:
        note = "field comparison matched"
        if relaxed:
            preview = ", ".join(relaxed[:8])
            if len(relaxed) > 8:
                preview += f", +{len(relaxed) - 8} more"
            note = f"field comparison matched; relaxed {preview}"
        report.add(path, "pass", note)
    return True


def _compare_usrp_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 48)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True

    exact_order_report = ComparisonReport()
    _compare_record_fields(path, expected, actual, 48, _USRP_FIELDS, exact_order_report)
    if not exact_order_report.failed:
        report.events.extend(exact_order_report.events)
        return True

    if _usrp_records_match_unordered(expected, actual):
        report.add(
            path,
            "fail",
            "USRP record order mismatch; unordered records match within field tolerances",
        )
        return True

    report.events.extend(exact_order_report.events)
    return True


def _usrp_records_match_unordered(expected: bytes, actual: bytes) -> bool:
    count, record_size = struct.unpack_from("<II", expected, 0)
    used: set[int] = set()
    for left_index in range(count):
        left_base = 8 + left_index * record_size
        match_index = None
        for right_index in range(count):
            if right_index in used:
                continue
            right_base = 8 + right_index * record_size
            if _usrp_record_close(expected, left_base, actual, right_base):
                match_index = right_index
                break
        if match_index is None:
            return False
        used.add(match_index)
    return True


def _usrp_record_close(
    expected: bytes,
    left_base: int,
    actual: bytes,
    right_base: int,
) -> bool:
    for field in _USRP_FIELDS:
        left = _field_value(expected, left_base, field)
        right = _field_value(actual, right_base, field)
        if left == right:
            continue
        if field.name in {"x_fp16", "y_fp16", "z_fp16"} and abs(int(left) - int(right)) <= 4:
            continue
        return False
    return True


def _compare_vert_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    if len(expected) < 12 or len(actual) < 12:
        report.add(path, "fail", f"VERT chunk too small: {len(expected)} != {len(actual)}")
        return True

    left_count, left_stride, left_flags = struct.unpack_from("<III", expected, 0)
    right_count, right_stride, right_flags = struct.unpack_from("<III", actual, 0)
    if left_count != right_count:
        left_layer = _vert_layer_name(left_stride, left_flags)
        right_layer = _vert_layer_name(right_stride, right_flags)
        report.add(
            path,
            "fail",
            f"VERT.count mismatch: {left_count} != {right_count} "
            f"({left_layer} stride {left_stride}/{right_stride}, "
            f"flags 0x{left_flags:08X}/0x{right_flags:08X}, actual layer {right_layer})",
        )
        return True
    if left_stride != right_stride:
        report.add(
            path,
            "fail",
            f"VERT.stride mismatch: {left_stride} != {right_stride} "
            f"({_vert_layer_name(left_stride, left_flags)} != {_vert_layer_name(right_stride, right_flags)})",
        )
        return True
    if left_flags != right_flags:
        report.add(
            path,
            "fail",
            f"VERT.flags mismatch: 0x{left_flags:08X} != 0x{right_flags:08X} "
            f"({_vert_layer_name(left_stride, left_flags)} != {_vert_layer_name(right_stride, right_flags)})",
        )
        return True
    layer_name = _vert_layer_name(left_stride, left_flags)
    if layer_name.startswith("unknown"):
        report.add(path, "fail", f"unexpected VERT layer: {layer_name}")
        return True
    expected_len = 12 + left_count * left_stride
    if len(expected) != expected_len or len(actual) != expected_len:
        report.add(path, "fail", f"VERT payload size mismatch: {len(expected)} != {len(actual)} expected {expected_len}")
        return True

    fields = _vert_fields(left_stride, left_flags)
    failures: list[str] = []
    relaxed: list[str] = []
    for record_index in range(left_count):
        base = 12 + record_index * left_stride
        for field in fields:
            left = _field_value(expected, base, field)
            right = _field_value(actual, base, field)
            if _field_bytes(expected, actual, base, field):
                continue
            field_path = f"{path}[{record_index}].{field.name}"
            close, note = _field_close(left, right, field, path, expected, actual, base)
            if close:
                relaxed.append(f"{field_path} ({note})")
                continue
            failures.append(f"{field_path}: expected={left!r} actual={right!r}")
            if len(failures) >= 10:
                break
        if len(failures) >= 10:
            break

    if failures:
        report.add(path, "fail", "; ".join(failures))
    else:
        note = f"{layer_name} field comparison matched"
        if relaxed:
            preview = ", ".join(relaxed[:8])
            if len(relaxed) > 8:
                preview += f", +{len(relaxed) - 8} more"
            note = f"{layer_name} field comparison matched; relaxed {preview}"
        report.add(path, "pass", note)
    return True


def _vert_layer_name(stride: int, flags: int) -> str:
    name = _VERTEX_LAYER_NAMES.get((stride, flags))
    if name is not None:
        return name
    return f"unknown(stride={stride}, flags=0x{flags:08X})"


def _vert_fields(stride: int, flags: int) -> tuple[_FieldSpec, ...]:
    fields: list[_FieldSpec] = [
        _FieldSpec("position.x", "f", 0),
        _FieldSpec("position.y", "f", 4),
        _FieldSpec("position.z", "f", 8),
    ]
    offset = 12
    is_skinned = (flags & 0x40) != 0
    has_tangents = (flags & 0x14) != 0
    if is_skinned:
        fields.extend(
            [
                _FieldSpec("bone_weight[0]", "f", offset + 0),
                _FieldSpec("bone_weight[1]", "f", offset + 4),
                _FieldSpec("bone_weight[2]", "f", offset + 8),
                _FieldSpec("bone_index[0]", "B", offset + 12),
                _FieldSpec("bone_index[1]", "B", offset + 13),
                _FieldSpec("bone_index[2]", "B", offset + 14),
                _FieldSpec("bone_index[3]", "B", offset + 15),
            ]
        )
        offset += 16
    fields.extend(
        [
            _FieldSpec("normal.x", "f", offset + 0),
            _FieldSpec("normal.y", "f", offset + 4),
            _FieldSpec("normal.z", "f", offset + 8),
            _FieldSpec("uv0.u", "f", offset + 12),
            _FieldSpec("uv0.v", "f", offset + 16),
            _FieldSpec("uv1.u", "f", offset + 20),
            _FieldSpec("uv1.v", "f", offset + 24),
        ]
    )
    offset += 28
    if has_tangents:
        fields.extend(
            [
                _FieldSpec("tangent.x", "f", offset + 0),
                _FieldSpec("tangent.y", "f", offset + 4),
                _FieldSpec("tangent.z", "f", offset + 8),
                _FieldSpec("bitangent.x", "f", offset + 12),
                _FieldSpec("bitangent.y", "f", offset + 16),
                _FieldSpec("bitangent.z", "f", offset + 20),
            ]
        )
        offset += 24
    if offset != stride:
        fields.append(_FieldSpec("trailing_bytes_unmodeled", f"{stride - offset}s", offset))
    return tuple(fields)


def _compare_indx_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 2)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True
    count = struct.unpack_from("<I", expected, 0)[0]
    for index in range(count):
        offset = 8 + index * 2
        left = struct.unpack_from("<H", expected, offset)[0]
        right = struct.unpack_from("<H", actual, offset)[0]
        if left != right:
            report.add(path, "fail", f"{path}[{index}]: expected={left} actual={right}")
            return True
    report.add(path, "pass", "field comparison matched")
    return True


def _compare_strp_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    if len(expected) < 8 or len(actual) < 8:
        report.add(path, "fail", f"record chunk too small: {len(expected)} != {len(actual)}")
        return True
    record_size = struct.unpack_from("<I", expected, 4)[0]
    fields = _STRP_FIELDS_68 if record_size == 68 else _STRP_FIELDS_48
    return _compare_record_fields(path, expected, actual, (48, 68), fields, report)


def _compare_mtrx_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 64)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True

    count, record_size = struct.unpack_from("<II", expected, 0)
    failures: list[str] = []
    relaxed = 0
    for record_index in range(count):
        base = 8 + record_index * record_size
        for field_index in range(16):
            offset = base + field_index * 4
            if expected[offset:offset + 4] == actual[offset:offset + 4]:
                continue
            left = struct.unpack_from("<f", expected, offset)[0]
            right = struct.unpack_from("<f", actual, offset)[0]
            if _mtrx_float_close(left, right):
                relaxed += 1
                continue
            failures.append(
                f"{path}[{record_index}].m[{field_index}]: expected={left!r} actual={right!r}"
            )
            if len(failures) >= 10:
                break
        if len(failures) >= 10:
            break

    if failures:
        report.add(path, "fail", "; ".join(failures))
    else:
        note = "field comparison matched"
        if relaxed:
            note = f"field comparison matched; relaxed {relaxed} ASE-rebake float(s)"
        report.add(path, "pass", note)
    return True


def _compare_strict_mtrx_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 64)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True
    count, record_size = struct.unpack_from("<II", expected, 0)
    tolerated: list[str] = []
    failures: list[str] = []
    for record_index in range(count):
        base = 8 + record_index * record_size
        if expected[base:base + record_size] == actual[base:base + record_size]:
            continue
        if _strict_mtrx_record_fieldwise_close(expected, actual, base, record_index, tolerated):
            continue
        if _strict_mtrx_record_reinversion_close(expected, actual, base):
            tolerated.append(f"m[{record_index}] x87 matrix re-inversion")
            continue
        failures.append(_first_record_diff(expected, actual, base, record_size))
        if len(failures) >= 10:
            break
    if failures:
        report.add(path, "fail", "; ".join(failures))
    elif tolerated:
        report.add(path, "fp-tolerated", _strict_tolerance_reason(tolerated))
    else:
        report.add(path, "byte-exact", f"exact {len(expected)} bytes")
    return True


def _strict_mtrx_record_fieldwise_close(
    expected: bytes,
    actual: bytes,
    base: int,
    record_index: int,
    tolerated: list[str],
) -> bool:
    local_tolerated: list[str] = []
    for field_index in range(16):
        offset = base + field_index * 4
        if expected[offset:offset + 4] == actual[offset:offset + 4]:
            continue
        if not _strict_float_close(expected, actual, offset):
            return False
        local_tolerated.append(f"m[{record_index}]+{field_index * 4}")
    tolerated.extend(local_tolerated)
    return True


def _strict_mtrx_record_reinversion_close(expected: bytes, actual: bytes, base: int) -> bool:
    for field_index in (3, 7, 11, 12, 13, 14, 15):
        offset = base + field_index * 4
        if expected[offset:offset + 4] == actual[offset:offset + 4]:
            continue
        if not _strict_float_close(expected, actual, offset):
            return False

    left = _mtrx_record_3x3(expected, base)
    right = _mtrx_record_3x3(actual, base)
    if left is None or right is None:
        return False
    if not (_mtrx_orthonormal(left) and _mtrx_orthonormal(right)):
        return False
    max_abs = max(abs(a - b) for row_l, row_r in zip(left, right) for a, b in zip(row_l, row_r))
    return max_abs <= 8e-4


def _mtrx_record_3x3(data: bytes, base: int) -> tuple[tuple[float, float, float], ...] | None:
    values = struct.unpack_from("<16f", data, base)
    matrix = (
        (values[0], values[1], values[2]),
        (values[4], values[5], values[6]),
        (values[8], values[9], values[10]),
    )
    if any(not math.isfinite(value) for row in matrix for value in row):
        return None
    return matrix


def _mtrx_orthonormal(matrix: tuple[tuple[float, float, float], ...]) -> bool:
    for row in matrix:
        length_sq = sum(value * value for value in row)
        if abs(length_sq - 1.0) > 2e-3:
            return False
    for left_index in range(3):
        for right_index in range(left_index + 1, 3):
            dot = sum(matrix[left_index][axis] * matrix[right_index][axis] for axis in range(3))
            if abs(dot) > 2e-3:
                return False
    return True


def _compare_strict_lght_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 116)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True
    count, record_size = struct.unpack_from("<II", expected, 0)
    float_offsets = {offset: "float" for offset in (0, 4, 8, 12, 16)}
    float_offsets.update({36 + index * 4: "float" for index in range(4)})
    float_offsets.update({52 + index * 4: "float" for index in range(16)})
    tolerated: list[str] = []
    failures = _strict_record_numeric_compare(
        path,
        expected,
        actual,
        count=count,
        record_size=record_size,
        numeric_offsets=float_offsets,
        record_label="light",
        tolerated=tolerated,
    )
    if failures:
        report.add(path, "fail", failures)
    elif tolerated:
        report.add(path, "fp-tolerated", _strict_tolerance_reason(tolerated))
    else:
        report.add(path, "byte-exact", f"exact {len(expected)} bytes")
    return True


def _compare_strict_usrp_fields(
    path: str,
    expected: bytes,
    actual: bytes,
    report: ComparisonReport,
) -> bool:
    header_error = _record_header_error(expected, actual, 48)
    if header_error is not None:
        report.add(path, "fail", header_error)
        return True
    count, record_size = struct.unpack_from("<II", expected, 0)
    fixed_offsets = {offset: "fixed" for offset in (0, 4, 8)}
    tolerated: list[str] = []
    failures = _strict_record_numeric_compare(
        path,
        expected,
        actual,
        count=count,
        record_size=record_size,
        numeric_offsets=fixed_offsets,
        record_label="userpoint position",
        tolerated=tolerated,
    )
    if failures:
        report.add(path, "fail", failures)
    elif tolerated:
        report.add(path, "fp-tolerated", _strict_tolerance_reason(tolerated))
    else:
        report.add(path, "byte-exact", f"exact {len(expected)} bytes")
    return True


def _strict_record_numeric_compare(
    path: str,
    expected: bytes,
    actual: bytes,
    *,
    count: int,
    record_size: int,
    numeric_offsets: dict[int, str],
    record_label: str,
    tolerated: list[str],
) -> str | None:
    index = 8
    while index < len(expected):
        if expected[index] == actual[index]:
            index += 1
            continue
        record_index = (index - 8) // record_size
        rel = (index - 8) % record_size
        field_start_rel = rel & ~3
        field_type = numeric_offsets.get(field_start_rel)
        field_start = 8 + record_index * record_size + field_start_rel
        if record_index < count and field_type and field_start + 4 <= len(expected):
            if field_type == "float" and _strict_float_close(expected, actual, field_start):
                tolerated.append(f"{record_label}[{record_index}]+{field_start_rel}")
                index = field_start + 4
                continue
            if field_type == "fixed" and _strict_fixed_close(expected, actual, field_start):
                tolerated.append(f"{record_label}[{record_index}]+{field_start_rel}")
                index = field_start + 4
                continue
        return _diff_at(expected, actual, index)
    return None


def _strict_float_close(left: bytes, right: bytes, offset: int) -> bool:
    a = struct.unpack_from("<f", left, offset)[0]
    b = struct.unpack_from("<f", right, offset)[0]
    if math.isnan(a) and math.isnan(b):
        return True
    if not (math.isfinite(a) and math.isfinite(b)):
        return False
    if a == b:
        return True
    return _ulp_distance(a, b) <= 2


def _strict_fixed_close(left: bytes, right: bytes, offset: int) -> bool:
    a = struct.unpack_from("<i", left, offset)[0]
    b = struct.unpack_from("<i", right, offset)[0]
    return abs(a - b) <= 1


def _strict_tolerance_reason(fields: list[str]) -> str:
    preview = ", ".join(fields[:8])
    if len(fields) > 8:
        preview += f", +{len(fields) - 8} more"
    return f"fp-tolerated {len(fields)} field(s): {preview}"


def _mtrx_float_close(left: float, right: float) -> bool:
    if math.isnan(left) and math.isnan(right):
        return True
    if not (math.isfinite(left) and math.isfinite(right)):
        return False
    if left == right:
        return True
    return abs(left - right) <= 1e-3


def _record_header_error(expected: bytes, actual: bytes, record_size: int | tuple[int, ...]) -> str | None:
    if len(expected) < 8 or len(actual) < 8:
        return f"record chunk too small: {len(expected)} != {len(actual)}"
    left_count, left_record_size = struct.unpack_from("<II", expected, 0)
    right_count, right_record_size = struct.unpack_from("<II", actual, 0)
    if left_count != right_count:
        return f"record count mismatch: {left_count} != {right_count}"
    if left_record_size != right_record_size:
        return f"record size mismatch: {left_record_size} != {right_record_size}"
    valid_sizes = (record_size,) if isinstance(record_size, int) else record_size
    if left_record_size not in valid_sizes:
        expected_sizes = "/".join(str(value) for value in valid_sizes)
        return f"unexpected record size: {left_record_size} != {expected_sizes}"
    expected_len = 8 + left_count * left_record_size
    if len(expected) != expected_len or len(actual) != expected_len:
        return f"record payload size mismatch: {len(expected)} != {len(actual)} expected {expected_len}"
    return None


def _field_bytes(expected: bytes, actual: bytes, base: int, field: _FieldSpec) -> bool:
    size = struct.calcsize("<" + field.fmt)
    start = base + field.offset
    end = start + size
    return expected[start:end] == actual[start:end]


def _field_value(data: bytes, base: int, field: _FieldSpec):
    return struct.unpack_from("<" + field.fmt, data, base + field.offset)[0]


def _field_close(
    left,
    right,
    field: _FieldSpec,
    path: str,
    expected: bytes,
    actual: bytes,
    base: int,
) -> tuple[bool, str | None]:
    if _path_ends(path, "ROBJ") and field.name.startswith("rel."):
        left_parent = struct.unpack_from("<i", expected, base + 8)[0]
        right_parent = struct.unpack_from("<i", actual, base + 8)[0]
        if left_parent == right_parent == -1:
            return True, "unparented ROBJ relative offset"
    # Stock 3DI float fields are computed from artist-authored pre-fp16-
    # quantization vertex positions; our regen computes them from the
    # post-quantization values stored in the IR. The resulting drift is
    # bounded by ~1/256 = 0.004 units per vertex, which can cascade into
    # ~0.01 units (= ~656 fp16 ticks) on derived bbox/radius/plane_dist
    # fields. See notes/3di-pipeline/cdta_remaining_gaps.md class (A).
    if _path_ends(path, "CNRM") and field.name.startswith("normal_"):
        if abs(int(left) - int(right)) <= 700:
            return True, "fp16-source quantization tolerance"
    if _path_ends(path, "CFAC") and field.name in {
        "plane_dist_fp16",
        "min_x_fp16",
        "min_y_fp16",
        "min_z_fp16",
        "max_x_fp16",
        "max_y_fp16",
        "max_z_fp16",
    }:
        if abs(int(left) - int(right)) <= 700:
            return True, "fp16-source quantization tolerance"
    if _path_ends(path, "CMDL") and field.name in {
        "bbox_min_x_fp16",
        "bbox_min_y_fp16",
        "bbox_min_z_fp16",
        "bbox_max_x_fp16",
        "bbox_max_y_fp16",
        "bbox_max_z_fp16",
        "max_radius_fp16",
        "max_radius_xy_fp16",
        "max_radius_z_fp16",
    }:
        if abs(int(left) - int(right)) <= 700:
            return True, "fp16-source quantization tolerance"
    if _path_ends(path, "USRP") and field.name in {
        "x_fp16",
        "y_fp16",
        "z_fp16",
        "rot_x_fp16",
        "rot_y_fp16",
        "rot_z_fp16",
    }:
        if abs(int(left) - int(right)) <= 4:
            return True, "fixed-point tolerance"
    if field.fmt != "f":
        return False, None
    if _path_ends(path, "ROBJ") and field.name.startswith("rel."):
        if abs(float(left)) < 1e-6 and abs(float(right)) < 1e-6:
            return True, "near-zero ROBJ relative offset"
    # Bounding centers/radii computed from material-bucket vertex sets;
    # cascades from RVRT bucket-membership differences (see
    # notes/3di-pipeline/oed_writerdta_audit.md). Tolerance ~0.01 units.
    if _path_ends(path, "ROBJ") and (
        field.name.startswith("bounding_center.") or field.name == "bounding_radius"
    ):
        if abs(float(left) - float(right)) <= 0.01:
            return True, "fp16-source bbox/radius tolerance"
    packed_left = struct.pack("<f", float(left))
    packed_right = struct.pack("<f", float(right))
    if _float_close(packed_left, packed_right, 0, path):
        return True, "numeric tolerance"
    return False, None


def _leaf_equal_with_policy(
    expected: bytes,
    actual: bytes,
    path: str,
    report: ComparisonReport,
) -> bool:
    if len(expected) != len(actual):
        report.add(path, "fail", f"size mismatch: {len(expected)} != {len(actual)}")
        return True

    left = bytearray(expected)
    right = bytearray(actual)
    policy_notes: list[str] = []

    if _path_ends(path, "LGHT") and len(left) >= 8:
        count = struct.unpack_from("<I", left, 0)[0]
        for index in range(count):
            flags_offset = 8 + index * 116 + 33
            if flags_offset < len(left):
                left[flags_offset] &= 0x0F
                right[flags_offset] &= 0x0F
        policy_notes.append("masked LGHT high flag bits")

    if _path_ends(path, "OOBJ") and len(left) >= 8:
        count = struct.unpack_from("<I", left, 0)[0]
        for index in range(count):
            record = 8 + index * 36
            if record + 3 > len(left):
                break
            occ_type = left[record]
            if occ_type not in (2, 3):
                left[record + 2] = 0
                right[record + 2] = 0
        policy_notes.append("masked non-OP connecting_subobject")

    if _path_ends(path, "CFAC") and len(left) >= 8:
        count, record_size = struct.unpack_from("<II", left, 0)
        if record_size == 44:
            for index in range(count):
                record = 8 + index * record_size
                if record + record_size > len(left):
                    break
                for rel in (6, 7, 41, 42, 43):
                    left[record + rel] = 0
                    right[record + rel] = 0
            policy_notes.append("masked CFAC normal_index and pad bytes")

    if _path_ends(path, "MTRL"):
        case_diffs = 0
        for index, (a, b) in enumerate(zip(left, right)):
            if a == b:
                continue
            if _ascii_lower_byte(a) == _ascii_lower_byte(b):
                left[index] = _ascii_lower_byte(a)
                right[index] = _ascii_lower_byte(b)
                case_diffs += 1
        if case_diffs:
            policy_notes.append(f"case-folded {case_diffs} MTRL byte(s)")

    if left == right:
        report.add(path, "pass", ", ".join(policy_notes) or "policy-normalized")
        return True

    tolerated = _float_tolerated_diff_count(bytes(left), bytes(right), path)
    if tolerated >= 0:
        note = f"{tolerated} tolerated numeric field(s)"
        if policy_notes:
            note = ", ".join(policy_notes + [note])
        report.add(path, "pass", note)
        return True
    return False


def _float_tolerated_diff_count(left: bytes, right: bytes, path: str) -> int:
    tolerated = 0
    index = 0
    while index < len(left):
        if left[index] == right[index]:
            index += 1
            continue
        aligned = index & ~3
        if aligned + 4 <= len(left) and _float_close(left, right, aligned, path):
            tolerated += 1
            index = aligned + 4
            continue
        if aligned + 4 <= len(left) and _fixed_or_int_close(left, right, aligned, path):
            tolerated += 1
            index = aligned + 4
            continue
        return -1
    return tolerated


def _ascii_lower_byte(value: int) -> int:
    return value + 32 if 0x41 <= value <= 0x5A else value


def _float_close(left: bytes, right: bytes, offset: int, path: str) -> bool:
    if not _float_tolerance_allowed(path):
        return False
    a = struct.unpack_from("<f", left, offset)[0]
    b = struct.unpack_from("<f", right, offset)[0]
    # MTRX zero-axis sentinel matrices land in NaN. OED's sub_420E30
    # @ 0x420E30 cofactor inversion produces NaN sign-bit patterns
    # that depend on the runtime FPU state and the input axis's exact
    # zero encoding (+0.0 vs -0.0). Stock author's source produced one
    # bit pattern; our pipeline's hard-coded sentinel produces another.
    # Both represent the same semantic "no inverse exists" sentinel
    # and both deserialise to NaN in the engine, so we treat any
    # (NaN, NaN) pair on MTRX as equivalent.
    if _path_ends(path, "MTRX") and math.isnan(a) and math.isnan(b):
        return True
    if not (math.isfinite(a) and math.isfinite(b)):
        return False
    if a == 0.0 and b == 0.0:
        return True
    if abs(a) < 1e-8 and abs(b) < 1e-8:
        return True
    if abs(a - b) < _float_abs_epsilon(path):
        return True
    if abs(a) < 1e-8 or abs(b) < 1e-8:
        return False
    return _ulp_distance(a, b) <= _float_ulp_epsilon(path)


def _float_tolerance_allowed(path: str) -> bool:
    return any(
        _path_ends(path, chunk)
        for chunk in (
            "MTRL",
            "LGHT",
            "OOBJ",
            "CVRT",
            "CNRM",
            "STRP",
            "VERT",
            "ROBJ",
            "BPLN",
            "BVOL",
            "MTRX",
            "PANM",
        )
    )


def _fixed_or_int_close(left: bytes, right: bytes, offset: int, path: str) -> bool:
    if not any(_path_ends(path, chunk) for chunk in ("COBJ", "USRP", "BPLN", "BVOL", "CXLT", "PANM", "VERT", "ROBJ")):
        return False
    a = struct.unpack_from("<i", left, offset)[0]
    b = struct.unpack_from("<i", right, offset)[0]
    diff = abs(a - b)
    if _path_ends(path, "USRP"):
        return diff <= 4
    limit = max(abs(a), abs(b)) // 2 + 65536
    return diff <= limit


_FLOAT_ABS_EPSILON = 1e-5
_FLOAT_ULP_EPSILON = 2


def _float_abs_epsilon(path: str) -> float:
    return _FLOAT_ABS_EPSILON


def _float_ulp_epsilon(path: str) -> int:
    return _FLOAT_ULP_EPSILON


def _ulp_distance(a: float, b: float) -> int:
    ai = struct.unpack("<i", struct.pack("<f", a))[0]
    bi = struct.unpack("<i", struct.pack("<f", b))[0]
    if (ai ^ bi) < 0:
        return 2**31 - 1
    return abs(ai - bi)


def _first_diff(left: bytes, right: bytes) -> str:
    limit = min(len(left), len(right))
    for index in range(limit):
        if left[index] != right[index]:
            return _diff_at(left, right, index)
    return f"size mismatch: {len(left)} != {len(right)}"


def _first_record_diff(left: bytes, right: bytes, base: int, size: int) -> str:
    end = min(base + size, len(left), len(right))
    for index in range(base, end):
        if left[index] != right[index]:
            return _diff_at(left, right, index)
    return f"size mismatch: {len(left)} != {len(right)}"


def _diff_at(left: bytes, right: bytes, index: int) -> str:
    return (
        f"first diff at byte {index}: "
        f"expected=0x{left[index]:02X} actual=0x{right[index]:02X}"
    )
