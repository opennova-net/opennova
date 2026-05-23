from __future__ import annotations

from pathlib import Path

import pytest


class _NativeBakeFn:
    restype = None
    argtypes = None

    def __init__(self, rc: int) -> None:
        self.rc = rc
        self.calls: list[tuple[bytes, bytes, bytes | None, int]] = []

    def __call__(
        self,
        project_path: bytes,
        output_path: bytes,
        model_name: bytes | None,
        update_mask: int,
    ) -> int:
        self.calls.append(
            (
                project_path,
                output_path,
                model_name,
                int(getattr(update_mask, "value", update_mask)),
            )
        )
        return self.rc


class _FakeLib:
    def __init__(self, bake_fn: _NativeBakeFn) -> None:
        self.bake_project_export = bake_fn


def test_bake_project_export_passes_paths_model_name_and_update_mask(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    from blender.opennova import bake_ffi

    bake_fn = _NativeBakeFn(0)
    monkeypatch.setattr(bake_ffi, "_bound", False)
    monkeypatch.setattr(bake_ffi, "load_lib", lambda: _FakeLib(bake_fn))

    project_path = tmp_path / "source.3dp"
    output_path = tmp_path / "out.3di"
    rc = bake_ffi.bake_project_export(
        project_path,
        output_path,
        model_name="Fixture",
        update_mask=bake_ffi.BAKE_UPDATE_MTRL,
    )

    assert rc == bake_ffi.BAKE_STATUS_OK
    assert bake_fn.calls == [
        (
            str(project_path).encode("utf-8"),
            str(output_path).encode("utf-8"),
            b"Fixture",
            bake_ffi.BAKE_UPDATE_MTRL,
        )
    ]


def test_bake_project_export_accepts_default_model_name_and_raises_on_failure(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    from blender.opennova import bake_ffi

    bake_fn = _NativeBakeFn(bake_ffi.BAKE_STATUS_EXPORT_FAILED)
    monkeypatch.setattr(bake_ffi, "_bound", False)
    monkeypatch.setattr(bake_ffi, "load_lib", lambda: _FakeLib(bake_fn))

    with pytest.raises(RuntimeError, match="status -5"):
        bake_ffi.bake_project_export(tmp_path / "source.3dp", tmp_path / "out.3di")

    assert bake_fn.calls[0][2] is None
    assert bake_fn.calls[0][3] == bake_ffi.BAKE_UPDATE_ALL
