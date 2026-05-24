"""3ds Max batch entry point.

``3dsmaxbatch.exe`` treats arguments after the script path as its own options,
so the parent process passes request/result paths through environment variables.
"""
from __future__ import annotations

import os
import sys
import traceback
from pathlib import Path


ENV_BATCH_REQUEST = "OPENNOVA_MAX_BATCH_REQUEST"
ENV_BATCH_RESULT = "OPENNOVA_MAX_BATCH_RESULT"


def main():
    # type: () -> int
    _prepend_repo_root()
    request_path = os.environ.get(ENV_BATCH_REQUEST, "").strip()
    result_path = os.environ.get(ENV_BATCH_RESULT, "").strip()
    if not request_path or not result_path:
        raise RuntimeError(
            "%s and %s are required for OpenNova Max batch imports"
            % (ENV_BATCH_REQUEST, ENV_BATCH_RESULT)
        )

    from opennova_max.batch import read_batch_request, write_batch_results

    requests = read_batch_request(Path(request_path))
    try:
        from opennova_max.import_runner import run_batch

        results = run_batch(requests)
    except Exception as exc:  # noqa: BLE001 - batch boundary must report every request
        from opennova_jobs import ImportResult

        detail = "%s: %s\n%s" % (type(exc).__name__, exc, traceback.format_exc())
        results = [
            ImportResult.failure(
                request,
                error=detail,
                output_path=request.likely_output_dir,
            )
            for request in requests
        ]
    write_batch_results(Path(result_path), results)
    return 0


def _prepend_repo_root():
    # type: () -> None
    root = Path(__file__).resolve().parents[1]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))


if __name__ == "__main__":
    raise SystemExit(main())
