from __future__ import annotations

import pytest


class _NativeCompareFn:
    restype = None
    argtypes = None

    def __init__(self, rc: int, report: bytes = b"") -> None:
        self.rc = rc
        self.report = report
        self.calls: list[tuple[bytes, bytes, bytes]] = []

    def __call__(
        self,
        expected_path: bytes,
        actual_path: bytes,
        chunk_ids: bytes,
        report_buffer,
        report_size: int,
    ) -> int:
        self.calls.append((expected_path, actual_path, chunk_ids))
        if self.report and report_size:
            report_buffer.value = self.report[: report_size - 1]
        return self.rc


class _FakeLib:
    def __init__(self, fn: _NativeCompareFn) -> None:
        self.threedi_3di3_compare_file_chunks = fn


def test_compare_3di3_chunks_requires_explicit_chunk_ids() -> None:
    from blender.opennova.threedi_compare_ffi import compare_3di3_chunks

    with pytest.raises(ValueError, match="chunk_ids"):
        compare_3di3_chunks("expected.3di", "actual.3di", "")


def test_compare_3di3_chunks_raises_native_mismatch_report(monkeypatch: pytest.MonkeyPatch) -> None:
    from blender.opennova import threedi_compare_ffi

    fn = _NativeCompareFn(1, b"GHDR byte mismatch at payload offset 0")
    monkeypatch.setattr(threedi_compare_ffi, "_lib", None)
    monkeypatch.setattr(threedi_compare_ffi, "load_lib", lambda: _FakeLib(fn))

    with pytest.raises(RuntimeError, match="GHDR byte mismatch"):
        threedi_compare_ffi.compare_3di3_chunks("expected.3di", "actual.3di", "GHDR")

    assert fn.calls == [(b"expected.3di", b"actual.3di", b"GHDR")]
