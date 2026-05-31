from __future__ import annotations

import importlib
import logging
import multiprocessing
import sys


def test_entrypoint_calls_freeze_support_before_logging_setup(monkeypatch, tmp_path) -> None:
    events: list[str] = []

    def fake_freeze_support() -> None:
        events.append("freeze_support")

    def fake_file_handler(*_args, **_kwargs) -> logging.Handler:
        events.append("file_handler")
        return logging.NullHandler()

    monkeypatch.setenv("APPDATA", str(tmp_path))
    monkeypatch.setattr(multiprocessing, "freeze_support", fake_freeze_support)
    monkeypatch.setattr(logging, "FileHandler", fake_file_handler)
    sys.modules.pop("apps.importer.__main__", None)

    try:
        importlib.import_module("apps.importer.__main__")
    finally:
        sys.modules.pop("apps.importer.__main__", None)

    assert events[:2] == ["freeze_support", "file_handler"]
