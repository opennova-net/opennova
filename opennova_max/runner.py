"""External ``3dsmaxbatch.exe`` grouped runner."""
from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path
from typing import Callable, Iterable, List, Optional

from opennova_jobs import ImportRequest, ImportResult

from .batch import read_batch_results, write_batch_request
from .discovery import resolve_3dsmaxbatch


ENV_BATCH_REQUEST = "OPENNOVA_MAX_BATCH_REQUEST"
ENV_BATCH_RESULT = "OPENNOVA_MAX_BATCH_RESULT"
QT_SUBPROCESS_ENV_KEYS = (
    "QT_QPA_PLATFORM",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
    "QT_DEBUG_PLUGINS",
)


class MaxBatchRunner:
    """Run one or more import requests inside a single 3ds Max batch process."""

    def __init__(
        self,
        maxbatch_path=None,
        *,
        resolver=resolve_3dsmaxbatch,
        timeout_seconds=60 * 30,
    ):
        # type: (Optional[Path], Callable[[], Optional[Path]], int) -> None
        self._maxbatch_path = Path(maxbatch_path) if maxbatch_path is not None else None
        self._resolver = resolver
        self.timeout_seconds = int(timeout_seconds)

    @property
    def maxbatch_path(self):
        # type: () -> Optional[Path]
        if self._maxbatch_path is not None:
            return self._maxbatch_path
        return self._resolver()

    @property
    def available(self):
        # type: () -> bool
        return self.maxbatch_path is not None

    def run(self, requests):
        # type: (Iterable[ImportRequest]) -> List[ImportResult]
        request_list = list(requests)
        if not request_list:
            return []

        exe = self.maxbatch_path
        if exe is None:
            return [_missing_3dsmaxbatch_result(request) for request in request_list]

        with tempfile.TemporaryDirectory(prefix="opennova-max-") as temp_dir:
            temp_path = Path(temp_dir)
            request_path = temp_path / "request.json"
            result_path = temp_path / "result.json"
            write_batch_request(request_path, request_list)

            env = _max_subprocess_env()
            env[ENV_BATCH_REQUEST] = str(request_path)
            env[ENV_BATCH_RESULT] = str(result_path)
            command = [str(exe), str(_batch_entry_path())]
            try:
                process = subprocess.run(
                    command,
                    cwd=str(_repo_root()),
                    env=env,
                    capture_output=True,
                    text=True,
                    errors="replace",
                    timeout=self.timeout_seconds,
                )
            except subprocess.TimeoutExpired as exc:
                return [
                    ImportResult.failure(
                        request,
                        error="3dsmaxbatch.exe timed out after %s seconds." % exc.timeout,
                        output_path=request.likely_output_dir,
                    )
                    for request in request_list
                ]

            if result_path.is_file():
                return read_batch_results(result_path)

            error = _process_error(process)
            return [
                ImportResult.failure(
                    request,
                    error=error,
                    output_path=request.likely_output_dir,
                )
                for request in request_list
            ]


def _batch_entry_path():
    # type: () -> Path
    return Path(__file__).with_name("batch_entry.py")


def _repo_root():
    # type: () -> Path
    return Path(__file__).resolve().parents[1]


def _max_subprocess_env():
    # type: () -> dict
    env = os.environ.copy()
    for key in QT_SUBPROCESS_ENV_KEYS:
        env.pop(key, None)
    return env


def _missing_3dsmaxbatch_result(request):
    # type: (ImportRequest) -> ImportResult
    return ImportResult.failure(
        request,
        error=(
            "3dsmaxbatch.exe was not found. Set OPENNOVA_3DSMAXBATCH or install "
            "Autodesk 3ds Max to write .max output."
        ),
        output_path=request.likely_output_dir,
    )


def _process_error(process):
    # type: (subprocess.CompletedProcess) -> str
    details = []
    stdout = (getattr(process, "stdout", "") or "").strip()
    stderr = (getattr(process, "stderr", "") or "").strip()
    if stdout:
        details.append("stdout: " + _tail(stdout))
    if stderr:
        details.append("stderr: " + _tail(stderr))
    suffix = " " + " ".join(details) if details else ""
    return "3dsmaxbatch.exe did not produce a result file (exit code %s).%s" % (
        getattr(process, "returncode", "unknown"),
        suffix,
    )


def _tail(text, limit=1200):
    # type: (str, int) -> str
    if len(text) <= limit:
        return text
    return text[-limit:]
